#include "../src/kitsu_ble_session.h"

#include <assert.h>
#include <string.h>

#include <string>
#include <vector>

using kitsu868::companion::CompanionCrypto;
using kitsu868::companion::CryptoPart;
using kitsu868::companion::DecodedEnvelope;
using kitsu868::companion::EnvelopeChannel;
using kitsu868::companion::ProtocolResult;
using kitsu868::connectivity::BleOperationDelegate;
using kitsu868::connectivity::BleSessionState;
using kitsu868::connectivity::BleSessionTransport;
using kitsu868::connectivity::ControllerRole;
using kitsu868::connectivity::DeviceSecurityPlatform;
using kitsu868::connectivity::DeviceSecurityStorage;
using kitsu868::connectivity::KitsuBleSession;
using kitsu868::connectivity::KitsuDeviceSecurity;
using kitsu868::connectivity::SecurityMode;
using kitsu868::connectivity::SecurityResult;
using kitsu868::connectivity::kBleControllerBackoffMs;
using kitsu868::connectivity::kKitsuControllerCapacity;
using kitsu868::connectivity::kKitsuSecretBytes;
using kitsu868::connectivity::kSecurityBlobCapacity;
using kitsu868::connectivity::kSecurityNonceBytes;
using kitsu868::connectivity::kSecuritySlots;
using kitsu868::connectivity::kSecurityTagBytes;

namespace {

uint32_t mix(uint32_t state, uint8_t value) {
  state ^= value;
  state *= 16777619UL;
  state ^= state >> 11U;
  return state;
}

class TestCrypto final : public CompanionCrypto {
 public:
  bool randomBytes(uint8_t* output, size_t outputBytes) override {
    if (!output && outputBytes != 0U) return false;
    for (size_t i = 0U; i < outputBytes; ++i) {
      randomState = randomState * 1664525UL + 1013904223UL;
      output[i] = static_cast<uint8_t>(randomState >> 24U);
    }
    return true;
  }

  bool sha256(const CryptoPart* parts, size_t partCount,
              uint8_t output[32]) override {
    return hash(nullptr, 0U, parts, partCount, output);
  }

  bool hmacSha256(const uint8_t key[32], const CryptoPart* parts,
                  size_t partCount, uint8_t output[32]) override {
    return hash(key, 32U, parts, partCount, output);
  }

  bool hkdfSha256(const uint8_t inputKey[32], const uint8_t* salt,
                  size_t saltBytes, const uint8_t* info, size_t infoBytes,
                  uint8_t output[32]) override {
    const CryptoPart parts[] = {{salt, saltBytes}, {info, infoBytes}};
    return hash(inputKey, 32U, parts, 2U, output);
  }

 private:
  static bool hash(const uint8_t* key, size_t keyBytes,
                   const CryptoPart* parts, size_t partCount,
                   uint8_t output[32]) {
    if ((!key && keyBytes != 0U) || (!parts && partCount != 0U) || !output) {
      return false;
    }
    uint32_t state = 2166136261UL;
    for (size_t i = 0U; i < keyBytes; ++i) state = mix(state, key[i]);
    for (size_t part = 0U; part < partCount; ++part) {
      if (!parts[part].data && parts[part].bytes != 0U) return false;
      state = mix(state, static_cast<uint8_t>(part));
      for (size_t i = 0U; i < parts[part].bytes; ++i) {
        state = mix(state, parts[part].data[i]);
      }
    }
    for (size_t i = 0U; i < 32U; ++i) {
      state = mix(state, static_cast<uint8_t>(i + keyBytes));
      output[i] = static_cast<uint8_t>(state >> ((i & 3U) * 8U));
    }
    return true;
  }

  uint32_t randomState = 0x12345678UL;
};

class TestPlatform final : public DeviceSecurityPlatform {
 public:
  SecurityMode securityMode() const override {
    return SecurityMode::Reflashable;
  }

  bool deriveWrappingKey(const uint8_t hardwareId[8],
                         uint8_t output[kKitsuSecretBytes]) override {
    for (size_t i = 0U; i < kKitsuSecretBytes; ++i) {
      output[i] = static_cast<uint8_t>(hardwareId[i & 7U] ^ i ^ 0xa5U);
    }
    return true;
  }

  bool randomBytes(uint8_t* output, size_t outputBytes) override {
    if (!output && outputBytes != 0U) return false;
    for (size_t i = 0U; i < outputBytes; ++i) {
      state = state * 1103515245UL + 12345UL;
      output[i] = static_cast<uint8_t>(state >> 16U);
    }
    return true;
  }

  bool seal(const uint8_t key[kKitsuSecretBytes], uint32_t generation,
            const uint8_t nonce[kSecurityNonceBytes],
            const uint8_t* plaintext, size_t plaintextBytes,
            uint8_t* ciphertext,
            uint8_t tag[kSecurityTagBytes]) override {
    for (size_t i = 0U; i < plaintextBytes; ++i) {
      ciphertext[i] = static_cast<uint8_t>(
          plaintext[i] ^ key[i % kKitsuSecretBytes] ^
          nonce[i % kSecurityNonceBytes] ^ static_cast<uint8_t>(generation));
    }
    makeTag(key, generation, nonce, ciphertext, plaintextBytes, tag);
    return true;
  }

  bool open(const uint8_t key[kKitsuSecretBytes], uint32_t generation,
            const uint8_t nonce[kSecurityNonceBytes],
            const uint8_t* ciphertext, size_t ciphertextBytes,
            const uint8_t tag[kSecurityTagBytes],
            uint8_t* plaintext) override {
    uint8_t expected[kSecurityTagBytes]{};
    makeTag(key, generation, nonce, ciphertext, ciphertextBytes, expected);
    if (memcmp(expected, tag, sizeof(expected)) != 0) return false;
    for (size_t i = 0U; i < ciphertextBytes; ++i) {
      plaintext[i] = static_cast<uint8_t>(
          ciphertext[i] ^ key[i % kKitsuSecretBytes] ^
          nonce[i % kSecurityNonceBytes] ^ static_cast<uint8_t>(generation));
    }
    return true;
  }

  bool hkdfSha256(const uint8_t* inputKey, size_t inputKeyBytes,
                  const uint8_t* salt, size_t saltBytes,
                  const uint8_t* info, size_t infoBytes, uint8_t* output,
                  size_t outputBytes) override {
    uint32_t hash = 2166136261UL;
    for (size_t i = 0U; i < inputKeyBytes; ++i) hash = mix(hash, inputKey[i]);
    for (size_t i = 0U; i < saltBytes; ++i) hash = mix(hash, salt[i]);
    for (size_t i = 0U; i < infoBytes; ++i) hash = mix(hash, info[i]);
    for (size_t i = 0U; i < outputBytes; ++i) {
      hash = mix(hash, static_cast<uint8_t>(i));
      output[i] = static_cast<uint8_t>(hash >> ((i & 3U) * 8U));
    }
    return true;
  }

 private:
  static void makeTag(const uint8_t key[kKitsuSecretBytes],
                      uint32_t generation,
                      const uint8_t nonce[kSecurityNonceBytes],
                      const uint8_t* data, size_t bytes,
                      uint8_t tag[kSecurityTagBytes]) {
    uint32_t hash = generation ^ 2166136261UL;
    for (size_t i = 0U; i < kKitsuSecretBytes; ++i) hash = mix(hash, key[i]);
    for (size_t i = 0U; i < kSecurityNonceBytes; ++i) hash = mix(hash, nonce[i]);
    for (size_t i = 0U; i < bytes; ++i) hash = mix(hash, data[i]);
    for (size_t i = 0U; i < kSecurityTagBytes; ++i) {
      hash = mix(hash, static_cast<uint8_t>(i));
      tag[i] = static_cast<uint8_t>(hash >> ((i & 3U) * 8U));
    }
  }

  uint32_t state = 9U;
};

class MemoryStorage final : public DeviceSecurityStorage {
 public:
  bool readSlot(uint8_t slot, uint8_t* output, size_t capacity,
                size_t& outputBytes) override {
    if (slot >= kSecuritySlots || sizes[slot] > capacity) return false;
    memcpy(output, data[slot], sizes[slot]);
    outputBytes = sizes[slot];
    return true;
  }

  bool writeSlot(uint8_t slot, const uint8_t* input,
                 size_t inputBytes) override {
    if (slot >= kSecuritySlots || inputBytes > kSecurityBlobCapacity) {
      return false;
    }
    if (failWrite) return false;
    memcpy(data[slot], input, inputBytes);
    sizes[slot] = inputBytes;
    return true;
  }

  bool clearSlot(uint8_t slot) override {
    if (slot >= kSecuritySlots || failClear) return false;
    memset(data[slot], 0, sizeof(data[slot]));
    sizes[slot] = 0U;
    return true;
  }

  uint8_t data[kSecuritySlots][kSecurityBlobCapacity]{};
  size_t sizes[kSecuritySlots]{};
  bool failWrite = false;
  bool failClear = false;
};

class TestTransport final : public BleSessionTransport {
 public:
  bool sendBleJson(const uint8_t* json, size_t jsonBytes) override {
    if (failSend || !json || jsonBytes == 0U) return false;
    frames.emplace_back(reinterpret_cast<const char*>(json), jsonBytes);
    return true;
  }

  bool setBleApplicationAuthenticated(bool authenticated) override {
    applicationAuthenticated = authenticated;
    return !failAuthSwitch;
  }

  bool bleTransmitIdle() const override { return transmitIdle; }

  void disconnectBle() override { disconnected = true; }

  std::vector<std::string> frames;
  bool applicationAuthenticated = false;
  bool disconnected = false;
  bool failSend = false;
  bool failAuthSwitch = false;
  bool transmitIdle = false;
};

class TestOperations final : public BleOperationDelegate {
 public:
  bool handleBleRequest(const DecodedEnvelope& request,
                        const uint8_t* payload, size_t payloadBytes,
                        uint8_t* response, size_t responseCapacity,
                        size_t& responseBytes) override {
    ++calls;
    lastOperation = request.operation;
    lastPayload.assign(reinterpret_cast<const char*>(payload), payloadBytes);
    static const char result[] = "{\"ok\":true}";
    if (sizeof(result) - 1U > responseCapacity) return false;
    memcpy(response, result, sizeof(result) - 1U);
    responseBytes = sizeof(result) - 1U;
    return true;
  }

  bool handleAuthorizedBleRequest(
      ControllerRole role, const DecodedEnvelope& request,
      const uint8_t* payload, size_t payloadBytes, uint8_t* response,
      size_t responseCapacity, size_t& responseBytes) override {
    lastRole = role;
    return handleBleRequest(request, payload, payloadBytes, response,
                            responseCapacity, responseBytes);
  }

  uint32_t calls = 0U;
  std::string lastOperation;
  std::string lastPayload;
  ControllerRole lastRole = static_cast<ControllerRole>(0xFFU);
};

std::string b64(const uint8_t* input, size_t bytes) {
  char output[96]{};
  size_t outputBytes = 0U;
  assert(kitsu868::companion::encodeBase64Url(
      input, bytes, output, sizeof(output), outputBytes));
  return std::string(output, outputBytes);
}

std::string jsonStringField(const std::string& json, const char* name) {
  const std::string prefix = std::string("\"") + name + "\":\"";
  const size_t start = json.find(prefix);
  assert(start != std::string::npos);
  const size_t valueStart = start + prefix.size();
  const size_t end = json.find('"', valueStart);
  assert(end != std::string::npos);
  return json.substr(valueStart, end - valueStart);
}

void decodeField(const std::string& json, const char* name, uint8_t* output,
                 size_t expectedBytes) {
  const std::string encoded = jsonStringField(json, name);
  size_t decoded = 0U;
  assert(kitsu868::companion::decodeBase64Url(
      encoded.c_str(), encoded.size(), output, expectedBytes, decoded));
  assert(decoded == expectedBytes);
}

struct Fixture {
  Fixture() {
    const uint8_t hardwareId[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    assert(security.begin(storage, platform, hardwareId) ==
           SecurityResult::OkReflashable);
    assert(session.begin(security, crypto, transport, operations,
                         "KT1234"));
  }

  MemoryStorage storage;
  TestPlatform platform;
  KitsuDeviceSecurity security;
  TestCrypto crypto;
  TestTransport transport;
  TestOperations operations;
  KitsuBleSession session;
};

void pairController(Fixture& fixture, uint8_t controllerId[16],
                    uint8_t controllerRoot[32]) {
  const uint32_t now = 1000U;
  const uint8_t controllerCountBefore =
      fixture.security.status().controllerCount;
  assert(controllerCountBefore < kKitsuControllerCapacity);
  fixture.session.onSecureLinkEstablished(true, true, true, true, now);
  fixture.session.setPairingWindow(true, 60000U, now);
  uint8_t clientNonce[16]{};
  for (size_t i = 0U; i < sizeof(clientNonce); ++i) {
    clientNonce[i] = static_cast<uint8_t>(0x40U + i);
  }
  const std::string request =
      "{\"v\":1,\"type\":\"pair_request\",\"client_nonce_b64\":\"" +
      b64(clientNonce, sizeof(clientNonce)) +
      "\",\"label\":\"Alice phone\",\"platform\":\"android\"}";
  fixture.session.onFrame(reinterpret_cast<const uint8_t*>(request.data()),
                          request.size(), now + 1U);
  assert(fixture.session.status(now + 1U).physicalConfirmationPending);
  assert(fixture.security.status().controllerCount == controllerCountBefore);
  assert(fixture.transport.frames.back().find("\"pair_pending\"") !=
         std::string::npos);
  assert(fixture.transport.frames.back().find("\"v\":1") !=
         std::string::npos);
  assert(fixture.transport.frames.back().find("\"role\"") ==
         std::string::npos);
  const std::string pending = fixture.transport.frames.back();

  assert(fixture.session.confirmPendingPairing(now + 2U));
  const std::string grant = fixture.transport.frames.back();
  assert(grant.find("\"device_uid\":\"KT1234\"") != std::string::npos);
  decodeField(grant, "controller_id_b64", controllerId, 16U);
  decodeField(grant, "root_b64", controllerRoot, 32U);
  uint8_t echoedClientNonce[16]{};
  uint8_t deviceNonce[16]{};
  uint8_t suppliedDeviceProof[32]{};
  decodeField(grant, "client_nonce_b64", echoedClientNonce, 16U);
  decodeField(grant, "device_nonce_b64", deviceNonce, 16U);
  decodeField(grant, "proof_b64", suppliedDeviceProof, 32U);
  assert(memcmp(echoedClientNonce, clientNonce, sizeof(clientNonce)) == 0);
  assert(pending ==
         "{\"v\":1,\"type\":\"pair_pending\",\"device_nonce_b64\":\"" +
             b64(deviceNonce, sizeof(deviceNonce)) +
             "\",\"expires_in_ms\":59999}");
  uint8_t expectedDeviceProof[32]{};
  assert(kitsu868::companion::makePairingProof(
             controllerRoot, "device", controllerId, "KT1234", clientNonce,
             deviceNonce, fixture.crypto, expectedDeviceProof) ==
         ProtocolResult::Ok);
  assert(memcmp(suppliedDeviceProof, expectedDeviceProof,
                sizeof(expectedDeviceProof)) == 0);
  const std::string expectedGrant =
      "{\"v\":1,\"type\":\"pair_grant\",\"controller_id_b64\":\"" +
      b64(controllerId, 16U) + "\",\"root_b64\":\"" +
      b64(controllerRoot, 32U) +
      "\",\"device_uid\":\"KT1234\",\"client_nonce_b64\":\"" +
      b64(clientNonce, sizeof(clientNonce)) +
      "\",\"device_nonce_b64\":\"" +
      b64(deviceNonce, sizeof(deviceNonce)) + "\",\"proof_b64\":\"" +
      b64(expectedDeviceProof, sizeof(expectedDeviceProof)) + "\"}";
  assert(grant == expectedGrant);

  uint8_t clientProof[32]{};
  assert(kitsu868::companion::makePairingProof(
             controllerRoot, "client", controllerId, "KT1234", clientNonce,
             deviceNonce, fixture.crypto, clientProof) == ProtocolResult::Ok);
  const std::string commit =
      "{\"v\":1,\"type\":\"pair_commit\",\"proof_b64\":\"" +
      b64(clientProof, sizeof(clientProof)) + "\"}";
  fixture.session.onFrame(reinterpret_cast<const uint8_t*>(commit.data()),
                          commit.size(), now + 3U);
  assert(fixture.transport.frames.back().find("\"pair_ok\"") !=
         std::string::npos);
  uint8_t ownerOkProof[32]{};
  assert(kitsu868::companion::makePairingProof(
             controllerRoot, "ok", controllerId, "KT1234", clientNonce,
             deviceNonce, fixture.crypto, ownerOkProof) == ProtocolResult::Ok);
  assert(fixture.transport.frames.back() ==
         "{\"v\":1,\"type\":\"pair_ok\",\"proof_b64\":\"" +
             b64(ownerOkProof, sizeof(ownerOkProof)) + "\"}");
  assert(fixture.session.status(now + 3U).pairingCompleted);
  assert(fixture.session.status(now + 3U).pairingRole ==
         ControllerRole::Owner);
  assert(fixture.security.status().controllerCount ==
         controllerCountBefore + 1U);
  uint8_t persisted[32]{};
  ControllerRole role = static_cast<ControllerRole>(0xFFU);
  assert(fixture.security.findControllerRoot(controllerId, persisted, role));
  assert(role == ControllerRole::Owner);
  assert(memcmp(persisted, controllerRoot, sizeof(persisted)) == 0);
}

struct CaretakerPairingMaterial {
  uint8_t controllerId[16]{};
  uint8_t controllerRoot[32]{};
  uint8_t clientNonce[16]{};
  uint8_t deviceNonce[16]{};
};

void beginCaretakerGrant(Fixture& fixture, uint32_t now, uint8_t nonceSeed,
                         CaretakerPairingMaterial& material,
                         bool establishLink = true) {
  if (establishLink) {
    fixture.session.onSecureLinkEstablished(true, true, true, true, now);
  }
  fixture.session.setPairingWindow(true, 60000U, now,
                                   ControllerRole::Caretaker);
  assert(fixture.session.status(now).pairingWindowOpen);
  assert(fixture.session.status(now).pairingRole ==
         ControllerRole::Caretaker);
  for (size_t i = 0U; i < sizeof(material.clientNonce); ++i) {
    material.clientNonce[i] = static_cast<uint8_t>(nonceSeed + i);
  }
  const std::string request =
      "{\"v\":2,\"type\":\"pair_request\",\"client_nonce_b64\":\"" +
      b64(material.clientNonce, sizeof(material.clientNonce)) +
      "\",\"label\":\"Caretaker phone\",\"platform\":\"android\"}";
  assert(request.find("\"role\"") == std::string::npos);
  fixture.session.onFrame(reinterpret_cast<const uint8_t*>(request.data()),
                          request.size(), now + 1U);
  assert(fixture.session.status(now + 1U).physicalConfirmationPending);
  assert(fixture.session.status(now + 1U).pairingRole ==
         ControllerRole::Caretaker);
  const std::string pending = fixture.transport.frames.back();
  assert(pending.find("{\"v\":2,\"type\":\"pair_pending\",") == 0U);
  assert(pending.find("\"role\":\"caretaker\"") != std::string::npos);

  assert(fixture.session.confirmPendingPairing(now + 2U));
  const std::string grant = fixture.transport.frames.back();
  decodeField(grant, "controller_id_b64", material.controllerId,
              sizeof(material.controllerId));
  decodeField(grant, "root_b64", material.controllerRoot,
              sizeof(material.controllerRoot));
  decodeField(grant, "device_nonce_b64", material.deviceNonce,
              sizeof(material.deviceNonce));
  uint8_t echoedClientNonce[16]{};
  uint8_t suppliedDeviceProof[32]{};
  decodeField(grant, "client_nonce_b64", echoedClientNonce,
              sizeof(echoedClientNonce));
  decodeField(grant, "proof_b64", suppliedDeviceProof,
              sizeof(suppliedDeviceProof));
  assert(memcmp(echoedClientNonce, material.clientNonce,
                sizeof(echoedClientNonce)) == 0);
  assert(pending ==
         "{\"v\":2,\"type\":\"pair_pending\",\"role\":\"caretaker\","
         "\"device_nonce_b64\":\"" +
             b64(material.deviceNonce, sizeof(material.deviceNonce)) +
             "\",\"expires_in_ms\":59999}");
  uint8_t expectedDeviceProof[32]{};
  assert(kitsu868::companion::makeRoleBoundPairingProof(
             material.controllerRoot, "device", "caretaker",
             material.controllerId, "KT1234", material.clientNonce,
             material.deviceNonce, fixture.crypto, expectedDeviceProof) ==
         ProtocolResult::Ok);
  assert(memcmp(suppliedDeviceProof, expectedDeviceProof,
                sizeof(expectedDeviceProof)) == 0);
  const std::string expectedGrant =
      "{\"v\":2,\"type\":\"pair_grant\",\"role\":\"caretaker\","
      "\"controller_id_b64\":\"" +
      b64(material.controllerId, sizeof(material.controllerId)) +
      "\",\"root_b64\":\"" +
      b64(material.controllerRoot, sizeof(material.controllerRoot)) +
      "\",\"device_uid\":\"KT1234\",\"client_nonce_b64\":\"" +
      b64(material.clientNonce, sizeof(material.clientNonce)) +
      "\",\"device_nonce_b64\":\"" +
      b64(material.deviceNonce, sizeof(material.deviceNonce)) +
      "\",\"proof_b64\":\"" +
      b64(expectedDeviceProof, sizeof(expectedDeviceProof)) + "\"}";
  assert(grant == expectedGrant);
}

void commitCaretakerGrant(Fixture& fixture,
                          const CaretakerPairingMaterial& material,
                          uint32_t now) {
  uint8_t clientProof[32]{};
  assert(kitsu868::companion::makeRoleBoundPairingProof(
             material.controllerRoot, "client", "caretaker",
             material.controllerId, "KT1234", material.clientNonce,
             material.deviceNonce, fixture.crypto, clientProof) ==
         ProtocolResult::Ok);
  const std::string commit =
      "{\"v\":2,\"type\":\"pair_commit\",\"role\":\"caretaker\","
      "\"proof_b64\":\"" +
      b64(clientProof, sizeof(clientProof)) + "\"}";
  fixture.session.onFrame(reinterpret_cast<const uint8_t*>(commit.data()),
                          commit.size(), now);
  uint8_t okProof[32]{};
  assert(kitsu868::companion::makeRoleBoundPairingProof(
             material.controllerRoot, "ok", "caretaker",
             material.controllerId, "KT1234", material.clientNonce,
             material.deviceNonce, fixture.crypto, okProof) ==
         ProtocolResult::Ok);
  assert(fixture.transport.frames.back() ==
         "{\"v\":2,\"type\":\"pair_ok\",\"role\":\"caretaker\","
         "\"proof_b64\":\"" +
             b64(okProof, sizeof(okProof)) + "\"}");
  const auto status = fixture.session.status(now);
  assert(status.pairingCompleted);
  assert(status.pairingRole == ControllerRole::Caretaker);
}

std::string caretakerCommitJson(const uint8_t proof[32], const char* role) {
  return "{\"v\":2,\"type\":\"pair_commit\",\"role\":\"" +
      std::string(role) + "\",\"proof_b64\":\"" + b64(proof, 32U) + "\"}";
}

void authenticateController(Fixture& fixture, const uint8_t controllerId[16],
                            const uint8_t controllerRoot[32],
                            uint8_t c2d[32], uint8_t d2c[32]) {
  fixture.session.onLinkClosed(2000U);
  fixture.session.onSecureLinkEstablished(true, true, true, true, 2001U);
  uint8_t clientNonce[16]{};
  memset(clientNonce, 0x65, sizeof(clientNonce));
  const std::string hello =
      "{\"v\":1,\"type\":\"client_hello\",\"controller_id_b64\":\"" +
      b64(controllerId, 16U) + "\",\"client_nonce_b64\":\"" +
      b64(clientNonce, 16U) + "\"}";
  fixture.session.onFrame(reinterpret_cast<const uint8_t*>(hello.data()),
                          hello.size(), 2002U);
  const std::string deviceHello = fixture.transport.frames.back();
  assert(deviceHello.find("\"device_hello\"") != std::string::npos);
  uint8_t deviceNonce[16]{};
  uint8_t deviceProof[32]{};
  decodeField(deviceHello, "device_nonce_b64", deviceNonce, 16U);
  decodeField(deviceHello, "proof_b64", deviceProof, 32U);
  uint8_t expectedDevice[32]{};
  assert(kitsu868::companion::makeHandshakeProof(
             controllerRoot, "device", controllerId, clientNonce,
             deviceNonce, fixture.crypto, expectedDevice) ==
         ProtocolResult::Ok);
  assert(memcmp(deviceProof, expectedDevice, sizeof(deviceProof)) == 0);

  uint8_t clientProof[32]{};
  assert(kitsu868::companion::makeHandshakeProof(
             controllerRoot, "client", controllerId, clientNonce,
             deviceNonce, fixture.crypto, clientProof) ==
         ProtocolResult::Ok);
  const std::string auth =
      "{\"v\":1,\"type\":\"client_auth\",\"proof_b64\":\"" +
      b64(clientProof, sizeof(clientProof)) + "\"}";
  fixture.session.onFrame(reinterpret_cast<const uint8_t*>(auth.data()),
                          auth.size(), 2003U);
  assert(fixture.transport.applicationAuthenticated);
  assert(fixture.session.status(2003U).state ==
         BleSessionState::Authenticated);
  ControllerRole storedRole = static_cast<ControllerRole>(0xFFU);
  uint8_t storedRoot[32]{};
  assert(fixture.security.findControllerRoot(controllerId, storedRoot,
                                             storedRole));
  assert(fixture.session.status(2003U).controllerRole == storedRole);
  assert(fixture.transport.frames.back().find("\"device_ok\"") !=
         std::string::npos);
  assert(kitsu868::companion::deriveBleSessionKeys(
             controllerRoot, clientNonce, deviceNonce, fixture.crypto,
             c2d, d2c) == ProtocolResult::Ok);
}

void provisionController(Fixture& fixture, ControllerRole role,
                         uint8_t controllerId[16],
                         uint8_t controllerRoot[32], uint8_t seed) {
  memset(controllerId, seed, 16U);
  memset(controllerRoot, static_cast<int>(seed + 0x40U), 32U);
  assert(fixture.security.commitControllerAfterPairing(
             controllerId, controllerRoot, true, true, true, true, true,
             role) == SecurityResult::Ok);
}

void sendAuthenticatedRequest(Fixture& fixture, const uint8_t c2d[32],
                              uint64_t sequence, const char* operation,
                              const char* payload, uint32_t nowMillis) {
  uint8_t nonce[16]{};
  uint8_t requestId[16]{};
  memset(nonce, static_cast<int>(0x20U + sequence), sizeof(nonce));
  memset(requestId, static_cast<int>(0x60U + sequence), sizeof(requestId));
  uint8_t requestJson[4096]{};
  size_t requestBytes = 0U;
  assert(kitsu868::companion::encodeEnvelope(
             EnvelopeChannel::Request, sequence, nonce, requestId, operation,
             reinterpret_cast<const uint8_t*>(payload), strlen(payload), c2d,
             fixture.crypto, requestJson, sizeof(requestJson), requestBytes) ==
         ProtocolResult::Ok);
  fixture.session.onFrame(requestJson, requestBytes, nowMillis);
}

void testPairingHandshakeEnvelopeAndReplay() {
  Fixture fixture;
  uint8_t controllerId[16]{};
  uint8_t controllerRoot[32]{};
  pairController(fixture, controllerId, controllerRoot);

  uint8_t c2d[32]{};
  uint8_t d2c[32]{};
  authenticateController(fixture, controllerId, controllerRoot, c2d, d2c);

  uint8_t nonce[16]{};
  uint8_t requestId[16]{};
  memset(nonce, 0x11, sizeof(nonce));
  memset(requestId, 0x22, sizeof(requestId));
  static const uint8_t payload[] =
      "{\"action_id\":\"00112233-4455-6677-8899-aabbccddeeff\","
      "\"kind\":\"pet\",\"expires_at_epoch\":1800000030,\"params\":{}}";
  uint8_t requestJson[1024]{};
  size_t requestBytes = 0U;
  assert(kitsu868::companion::encodeEnvelope(
             EnvelopeChannel::Request, 1U, nonce, requestId, "action.apply",
             payload, sizeof(payload) - 1U, c2d, fixture.crypto, requestJson,
             sizeof(requestJson), requestBytes) == ProtocolResult::Ok);
  fixture.session.onFrame(requestJson, requestBytes, 2010U);
  assert(fixture.operations.calls == 1U);
  assert(fixture.operations.lastOperation == "action.apply");
  assert(fixture.operations.lastPayload ==
         reinterpret_cast<const char*>(payload));
  assert(fixture.operations.lastRole == ControllerRole::Owner);

  const std::string response = fixture.transport.frames.back();
  uint8_t decodedPayload[128]{};
  DecodedEnvelope decoded{};
  assert(kitsu868::companion::decodeAndVerifyEnvelope(
             reinterpret_cast<const uint8_t*>(response.data()),
             response.size(), d2c, EnvelopeChannel::Response, 1U,
             fixture.crypto, decoded, decodedPayload,
             sizeof(decodedPayload)) == ProtocolResult::Ok);
  assert(strcmp(decoded.operation, "action.apply") == 0);
  assert(std::string(reinterpret_cast<char*>(decodedPayload),
                     decoded.payloadBytes) == "{\"ok\":true}");

  // Exact sequence enforcement rejects the replay before delegate execution.
  fixture.session.onFrame(requestJson, requestBytes, 2011U);
  assert(fixture.operations.calls == 1U);
  assert(fixture.session.status(2011U).state == BleSessionState::Closing);
  fixture.session.loop(2300U);
  assert(fixture.transport.disconnected);
}

void testCaretakerRoleIsEnforcedBeforeDelegation() {
  static const char kPetAction[] =
      "{\"action_id\":\"00112233-4455-6677-8899-aabbccddeeff\","
      "\"kind\":\"pet\",\"expires_at_epoch\":1800000030,\"params\":{}}";
  static const char kSendMessageAction[] =
      "{\"action_id\":\"10112233-4455-6677-8899-aabbccddeeff\","
      "\"kind\":\"send_message\",\"expires_at_epoch\":1800000030,"
      "\"params\":{\"route\":\"channel\",\"target_id\":\"0\","
      "\"text\":\"hello\"}}";

  {
    Fixture fixture;
    uint8_t controllerId[16]{};
    uint8_t controllerRoot[32]{};
    provisionController(fixture, ControllerRole::Caretaker, controllerId,
                        controllerRoot, 0x21U);
    uint8_t c2d[32]{};
    uint8_t d2c[32]{};
    authenticateController(fixture, controllerId, controllerRoot, c2d, d2c);
    assert(fixture.session.status(2003U).controllerRole ==
           ControllerRole::Caretaker);

    sendAuthenticatedRequest(fixture, c2d, 1U, "state.get", "{}", 2010U);
    assert(fixture.operations.calls == 1U);
    assert(fixture.operations.lastRole == ControllerRole::Caretaker);
    sendAuthenticatedRequest(fixture, c2d, 2U, "action.apply", kPetAction,
                             2011U);
    assert(fixture.operations.calls == 2U);
    assert(fixture.operations.lastPayload == kPetAction);

    sendAuthenticatedRequest(fixture, c2d, 3U, "mesh.configure", "{}",
                             2012U);
    assert(fixture.operations.calls == 2U);
    assert(fixture.session.status(2012U).state == BleSessionState::Closing);
    uint8_t retainedRoot[32]{};
    ControllerRole retainedRole = static_cast<ControllerRole>(0xFFU);
    assert(fixture.security.findControllerRoot(
        controllerId, retainedRoot, retainedRole));
    assert(retainedRole == ControllerRole::Caretaker);
  }

  {
    Fixture fixture;
    uint8_t controllerId[16]{};
    uint8_t controllerRoot[32]{};
    provisionController(fixture, ControllerRole::Caretaker, controllerId,
                        controllerRoot, 0x22U);
    uint8_t c2d[32]{};
    uint8_t d2c[32]{};
    authenticateController(fixture, controllerId, controllerRoot, c2d, d2c);
    sendAuthenticatedRequest(fixture, c2d, 1U, "action.apply",
                             kSendMessageAction, 2010U);
    assert(fixture.operations.calls == 0U);
    assert(fixture.session.status(2010U).state == BleSessionState::Closing);
  }

  {
    Fixture fixture;
    uint8_t controllerId[16]{};
    uint8_t controllerRoot[32]{};
    provisionController(fixture, ControllerRole::Caretaker, controllerId,
                        controllerRoot, 0x23U);
    uint8_t c2d[32]{};
    uint8_t d2c[32]{};
    authenticateController(fixture, controllerId, controllerRoot, c2d, d2c);
    sendAuthenticatedRequest(fixture, c2d, 1U, "controller.forget", "{}",
                             2010U);
    assert(fixture.operations.calls == 0U);
    uint8_t retainedRoot[32]{};
    ControllerRole retainedRole = static_cast<ControllerRole>(0xFFU);
    assert(fixture.security.findControllerRoot(
        controllerId, retainedRoot, retainedRole));
    assert(retainedRole == ControllerRole::Caretaker);
  }

  {
    Fixture fixture;
    uint8_t controllerId[16]{};
    uint8_t controllerRoot[32]{};
    provisionController(fixture, ControllerRole::Owner, controllerId,
                        controllerRoot, 0x24U);
    uint8_t c2d[32]{};
    uint8_t d2c[32]{};
    authenticateController(fixture, controllerId, controllerRoot, c2d, d2c);
    sendAuthenticatedRequest(fixture, c2d, 1U, "action.apply",
                             kSendMessageAction, 2010U);
    assert(fixture.operations.calls == 1U);
    assert(fixture.operations.lastRole == ControllerRole::Owner);
  }
}

void testEncounterOperationsReachDelegate() {
  Fixture fixture;
  uint8_t controllerId[16]{};
  uint8_t controllerRoot[32]{};
  pairController(fixture, controllerId, controllerRoot);

  uint8_t c2d[32]{};
  uint8_t d2c[32]{};
  authenticateController(fixture, controllerId, controllerRoot, c2d, d2c);

  static const char* const operations[] = {
      "encounter.codes.get.v1",
      "encounter.neighbors.get.v1",
      "encounter.neighbor.action.v1",
      "encounter.catalog.get.v1",
      "encounter.discovery.get.v1",
  };
  static const uint8_t payload[] = "{}";
  for (size_t index = 0U;
       index < sizeof(operations) / sizeof(operations[0]); ++index) {
    uint8_t nonce[16]{};
    uint8_t requestId[16]{};
    memset(nonce, static_cast<int>(0x50U + index), sizeof(nonce));
    memset(requestId, static_cast<int>(0x60U + index), sizeof(requestId));
    uint8_t requestJson[1024]{};
    size_t requestBytes = 0U;
    const uint64_t sequence = static_cast<uint64_t>(index + 1U);
    assert(kitsu868::companion::encodeEnvelope(
               EnvelopeChannel::Request, sequence, nonce, requestId,
               operations[index], payload, sizeof(payload) - 1U, c2d,
               fixture.crypto, requestJson, sizeof(requestJson),
               requestBytes) == ProtocolResult::Ok);

    fixture.session.onFrame(requestJson, requestBytes,
                            static_cast<uint32_t>(2010U + index));
    assert(fixture.operations.calls == index + 1U);
    assert(fixture.operations.lastOperation == operations[index]);
    assert(fixture.operations.lastPayload == "{}");

    const std::string response = fixture.transport.frames.back();
    uint8_t decodedPayload[128]{};
    DecodedEnvelope decoded{};
    assert(kitsu868::companion::decodeAndVerifyEnvelope(
               reinterpret_cast<const uint8_t*>(response.data()),
               response.size(), d2c, EnvelopeChannel::Response, sequence,
               fixture.crypto, decoded, decodedPayload,
               sizeof(decodedPayload)) == ProtocolResult::Ok);
    assert(strcmp(decoded.operation, operations[index]) == 0);
    assert(std::string(reinterpret_cast<char*>(decodedPayload),
                       decoded.payloadBytes) == "{\"ok\":true}");
  }

  uint8_t nonce[16]{};
  uint8_t requestId[16]{};
  memset(nonce, 0x70, sizeof(nonce));
  memset(requestId, 0x71, sizeof(requestId));
  uint8_t requestJson[1024]{};
  size_t requestBytes = 0U;
  assert(kitsu868::companion::encodeEnvelope(
             EnvelopeChannel::Request, 6U, nonce, requestId,
             "encounter.unsupported.v1", payload, sizeof(payload) - 1U, c2d,
             fixture.crypto, requestJson, sizeof(requestJson), requestBytes) ==
         ProtocolResult::Ok);
  fixture.session.onFrame(requestJson, requestBytes, 2020U);
  assert(fixture.operations.calls == 5U);
  assert(fixture.session.status(2020U).state == BleSessionState::Closing);
}

void testUnsolicitedEventBarrierRequiresQueuedAuthenticatedResponse() {
  {
    Fixture fixture;
    uint8_t controllerId[16]{};
    uint8_t controllerRoot[32]{};
    pairController(fixture, controllerId, controllerRoot);
    uint8_t c2d[32]{};
    uint8_t d2c[32]{};
    authenticateController(fixture, controllerId, controllerRoot, c2d, d2c);
    static const uint8_t payload[] = "{}";
    const size_t framesBefore = fixture.transport.frames.size();
    assert(!fixture.session.status(2004U).authenticatedRequestBarrier);
    assert(!fixture.session.sendEvent("companion.refresh", payload,
                                      sizeof(payload) - 1U));
    assert(fixture.transport.frames.size() == framesBefore);

    uint8_t nonce[16]{};
    uint8_t requestId[16]{};
    memset(nonce, 0x31, sizeof(nonce));
    memset(requestId, 0x32, sizeof(requestId));
    uint8_t requestJson[1024]{};
    size_t requestBytes = 0U;
    assert(kitsu868::companion::encodeEnvelope(
               EnvelopeChannel::Request, 1U, nonce, requestId, "clock.sync",
               payload, sizeof(payload) - 1U, c2d, fixture.crypto,
               requestJson, sizeof(requestJson), requestBytes) ==
           ProtocolResult::Ok);
    fixture.session.onFrame(requestJson, requestBytes, 2010U);
    assert(fixture.session.status(2010U).authenticatedRequestBarrier);
    assert(fixture.session.sendEvent("companion.refresh", payload,
                                     sizeof(payload) - 1U));

    fixture.session.onLinkClosed(2011U);
    assert(!fixture.session.status(2011U).authenticatedRequestBarrier);
    assert(!fixture.session.sendEvent("companion.refresh", payload,
                                      sizeof(payload) - 1U));
  }

  {
    Fixture fixture;
    uint8_t controllerId[16]{};
    uint8_t controllerRoot[32]{};
    pairController(fixture, controllerId, controllerRoot);
    uint8_t c2d[32]{};
    uint8_t d2c[32]{};
    authenticateController(fixture, controllerId, controllerRoot, c2d, d2c);
    fixture.transport.failSend = true;
    static const uint8_t payload[] = "{}";
    uint8_t nonce[16]{};
    uint8_t requestId[16]{};
    memset(nonce, 0x41, sizeof(nonce));
    memset(requestId, 0x42, sizeof(requestId));
    uint8_t requestJson[1024]{};
    size_t requestBytes = 0U;
    assert(kitsu868::companion::encodeEnvelope(
               EnvelopeChannel::Request, 1U, nonce, requestId, "clock.sync",
               payload, sizeof(payload) - 1U, c2d, fixture.crypto,
               requestJson, sizeof(requestJson), requestBytes) ==
           ProtocolResult::Ok);
    fixture.session.onFrame(requestJson, requestBytes, 2010U);
    assert(!fixture.session.status(2010U).authenticatedRequestBarrier);
    assert(fixture.session.status(2010U).state == BleSessionState::Closing);
  }
}

void testAuthenticatedControllerForgetDrainsReceipt() {
  Fixture fixture;
  uint8_t controllerId[16]{};
  uint8_t controllerRoot[32]{};
  pairController(fixture, controllerId, controllerRoot);

  uint8_t c2d[32]{};
  uint8_t d2c[32]{};
  authenticateController(fixture, controllerId, controllerRoot, c2d, d2c);
  uint8_t nonce[16]{};
  uint8_t requestId[16]{};
  memset(nonce, 0x31, sizeof(nonce));
  memset(requestId, 0x32, sizeof(requestId));
  const uint8_t payload[] = "{}";
  uint8_t requestJson[1024]{};
  size_t requestBytes = 0U;
  assert(kitsu868::companion::encodeEnvelope(
             EnvelopeChannel::Request, 1U, nonce, requestId,
             "controller.forget", payload, sizeof(payload) - 1U, c2d,
             fixture.crypto, requestJson, sizeof(requestJson), requestBytes) ==
         ProtocolResult::Ok);
  fixture.session.onFrame(requestJson, requestBytes, 2010U);
  assert(fixture.operations.calls == 0U);
  assert(fixture.security.status().controllerCount == 0U);
  uint8_t missingRoot[32]{};
  assert(!fixture.security.findControllerRoot(controllerId, missingRoot));

  const std::string response = fixture.transport.frames.back();
  uint8_t decodedPayload[128]{};
  DecodedEnvelope decoded{};
  assert(kitsu868::companion::decodeAndVerifyEnvelope(
             reinterpret_cast<const uint8_t*>(response.data()),
             response.size(), d2c, EnvelopeChannel::Response, 1U,
             fixture.crypto, decoded, decodedPayload,
             sizeof(decodedPayload)) == ProtocolResult::Ok);
  assert(strcmp(decoded.operation, "controller.forget") == 0);
  assert(std::string(reinterpret_cast<char*>(decodedPayload),
                     decoded.payloadBytes) ==
         "{\"schema\":\"kitsu.controller-forget.v1\",\"accepted\":true}");
  assert(fixture.session.status(2010U).state == BleSessionState::Closing);
  assert(!fixture.transport.applicationAuthenticated);
  fixture.session.loop(2011U);
  assert(!fixture.transport.disconnected);
  fixture.transport.transmitIdle = true;
  fixture.session.loop(2012U);
  assert(fixture.transport.disconnected);
}

void testPartialControllerForgetCannotKeepUsingSession() {
  Fixture fixture;
  uint8_t controllerId[16]{};
  uint8_t controllerRoot[32]{};
  pairController(fixture, controllerId, controllerRoot);
  uint8_t c2d[32]{};
  uint8_t d2c[32]{};
  authenticateController(fixture, controllerId, controllerRoot, c2d, d2c);
  fixture.storage.failClear = true;

  uint8_t nonce[16]{};
  uint8_t requestId[16]{};
  memset(nonce, 0x41, sizeof(nonce));
  memset(requestId, 0x42, sizeof(requestId));
  const uint8_t payload[] = "{}";
  uint8_t requestJson[1024]{};
  size_t requestBytes = 0U;
  assert(kitsu868::companion::encodeEnvelope(
             EnvelopeChannel::Request, 1U, nonce, requestId,
             "controller.forget", payload, sizeof(payload) - 1U, c2d,
             fixture.crypto, requestJson, sizeof(requestJson), requestBytes) ==
         ProtocolResult::Ok);
  fixture.session.onFrame(requestJson, requestBytes, 2010U);

  uint8_t missingRoot[32]{};
  assert(!fixture.security.findControllerRoot(controllerId, missingRoot));
  const std::string response = fixture.transport.frames.back();
  uint8_t decodedPayload[160]{};
  DecodedEnvelope decoded{};
  assert(kitsu868::companion::decodeAndVerifyEnvelope(
             reinterpret_cast<const uint8_t*>(response.data()),
             response.size(), d2c, EnvelopeChannel::Response, 1U,
             fixture.crypto, decoded, decodedPayload,
             sizeof(decodedPayload)) == ProtocolResult::Ok);
  assert(std::string(reinterpret_cast<char*>(decodedPayload),
                     decoded.payloadBytes) ==
         "{\"schema\":\"kitsu.controller-forget.v1\",\"accepted\":false,"
         "\"error\":\"storage_failed\"}");
  assert(!fixture.transport.applicationAuthenticated);
  assert(fixture.session.status(2010U).state == BleSessionState::Closing);
  fixture.transport.transmitIdle = true;
  fixture.session.loop(2011U);
  assert(fixture.transport.disconnected);
}

void testStrictControlsAndTimeout() {
  Fixture fixture;
  fixture.session.onSecureLinkEstablished(true, true, true, true, 500U);
  const std::string duplicate =
      "{\"v\":1,\"v\":1,\"type\":\"client_hello\","
      "\"controller_id_b64\":\"AAAAAAAAAAAAAAAAAAAAAA\","
      "\"client_nonce_b64\":\"AAAAAAAAAAAAAAAAAAAAAA\"}";
  fixture.session.onFrame(
      reinterpret_cast<const uint8_t*>(duplicate.data()), duplicate.size(),
      501U);
  assert(fixture.session.status(501U).proofFailures == 1U);
  assert(fixture.transport.frames.back() ==
         "{\"v\":1,\"type\":\"error\",\"code\":\"auth_failed\"}");

  fixture.session.loop(10501U);
  assert(fixture.session.status(10501U).state == BleSessionState::Closing);
  assert(fixture.transport.frames.back().find("\"timeout\"") !=
         std::string::npos);
  fixture.session.loop(10800U);
  assert(fixture.transport.disconnected);
}

void testPairingRoleDowngradeEscalationAndRetry() {
  {
    Fixture fixture;
    const uint32_t now = 11500U;
    fixture.session.onSecureLinkEstablished(true, true, true, true, now);
    fixture.session.setPairingWindow(true, 60000U, now,
                                     ControllerRole::Caretaker);
    uint8_t nonce[16]{};
    memset(nonce, 0x11, sizeof(nonce));
    const std::string remoteRoleInjection =
        "{\"v\":2,\"type\":\"pair_request\",\"role\":\"owner\","
        "\"client_nonce_b64\":\"" +
        b64(nonce, sizeof(nonce)) +
        "\",\"label\":\"Role chooser\",\"platform\":\"android\"}";
    fixture.session.onFrame(
        reinterpret_cast<const uint8_t*>(remoteRoleInjection.data()),
        remoteRoleInjection.size(), now + 1U);
    assert(fixture.transport.frames.back() ==
           "{\"v\":1,\"type\":\"error\",\"code\":\"auth_failed\"}");
    assert(!fixture.session.status(now + 1U).physicalConfirmationPending);
    assert(fixture.security.status().controllerCount == 0U);
  }

  {
    Fixture fixture;
    const uint32_t now = 12000U;
    fixture.session.onSecureLinkEstablished(true, true, true, true, now);
    fixture.session.setPairingWindow(true, 60000U, now,
                                     ControllerRole::Caretaker);
    uint8_t nonce[16]{};
    memset(nonce, 0x21, sizeof(nonce));
    const std::string v1Downgrade =
        "{\"v\":1,\"type\":\"pair_request\",\"client_nonce_b64\":\"" +
        b64(nonce, sizeof(nonce)) +
        "\",\"label\":\"Old client\",\"platform\":\"android\"}";
    fixture.session.onFrame(
        reinterpret_cast<const uint8_t*>(v1Downgrade.data()),
        v1Downgrade.size(), now + 1U);
    assert(fixture.transport.frames.back() ==
           "{\"v\":1,\"type\":\"error\",\"code\":\"auth_failed\"}");
    assert(fixture.session.status(now + 1U).proofFailures == 1U);
    assert(fixture.session.status(now + 1U).pairingWindowOpen);
    assert(fixture.session.status(now + 1U).pairingRole ==
           ControllerRole::Caretaker);
    assert(!fixture.session.status(now + 1U).physicalConfirmationPending);
    assert(fixture.security.status().controllerCount == 0U);

    CaretakerPairingMaterial retry{};
    beginCaretakerGrant(fixture, now + 10U, 0x31U, retry, false);
    commitCaretakerGrant(fixture, retry, now + 13U);
    ControllerRole storedRole = static_cast<ControllerRole>(0xFFU);
    uint8_t storedRoot[32]{};
    assert(fixture.security.findControllerRoot(retry.controllerId, storedRoot,
                                               storedRole));
    assert(storedRole == ControllerRole::Caretaker);
  }

  {
    Fixture fixture;
    const uint32_t now = 13000U;
    fixture.session.onSecureLinkEstablished(true, true, true, true, now);
    fixture.session.setPairingWindow(true, 60000U, now,
                                     ControllerRole::Owner);
    uint8_t nonce[16]{};
    memset(nonce, 0x41, sizeof(nonce));
    const std::string v2Escalation =
        "{\"v\":2,\"type\":\"pair_request\",\"client_nonce_b64\":\"" +
        b64(nonce, sizeof(nonce)) +
        "\",\"label\":\"V2 client\",\"platform\":\"android\"}";
    fixture.session.onFrame(
        reinterpret_cast<const uint8_t*>(v2Escalation.data()),
        v2Escalation.size(), now + 1U);
    assert(fixture.transport.frames.back() ==
           "{\"v\":1,\"type\":\"error\",\"code\":\"auth_failed\"}");
    assert(fixture.session.status(now + 1U).proofFailures == 1U);
    assert(fixture.session.status(now + 1U).pairingRole ==
           ControllerRole::Owner);
    assert(!fixture.session.status(now + 1U).physicalConfirmationPending);
    assert(fixture.security.status().controllerCount == 0U);
  }
}

void testCaretakerCommitRoleAndTranscriptTamperThenRetry() {
  Fixture fixture;
  const uint32_t now = 14000U;
  CaretakerPairingMaterial roleTamper{};
  beginCaretakerGrant(fixture, now, 0x51U, roleTamper);
  uint8_t validProof[32]{};
  assert(kitsu868::companion::makeRoleBoundPairingProof(
             roleTamper.controllerRoot, "client", "caretaker",
             roleTamper.controllerId, "KT1234", roleTamper.clientNonce,
             roleTamper.deviceNonce, fixture.crypto, validProof) ==
         ProtocolResult::Ok);
  const std::string changedRole = caretakerCommitJson(validProof, "owner");
  fixture.session.onFrame(
      reinterpret_cast<const uint8_t*>(changedRole.data()), changedRole.size(),
      now + 3U);
  assert(fixture.transport.frames.back().find("\"auth_failed\"") !=
         std::string::npos);
  assert(fixture.security.status().controllerCount == 0U);
  assert(!fixture.session.status(now + 3U).pairingCompleted);

  CaretakerPairingMaterial transcriptTamper{};
  beginCaretakerGrant(fixture, now + 10U, 0x61U, transcriptTamper, false);
  uint8_t legacyProof[32]{};
  assert(kitsu868::companion::makePairingProof(
             transcriptTamper.controllerRoot, "client",
             transcriptTamper.controllerId, "KT1234",
             transcriptTamper.clientNonce, transcriptTamper.deviceNonce,
             fixture.crypto, legacyProof) == ProtocolResult::Ok);
  const std::string changedTranscript =
      caretakerCommitJson(legacyProof, "caretaker");
  fixture.session.onFrame(
      reinterpret_cast<const uint8_t*>(changedTranscript.data()),
      changedTranscript.size(), now + 13U);
  assert(fixture.transport.frames.back().find("\"auth_failed\"") !=
         std::string::npos);
  assert(fixture.security.status().controllerCount == 0U);

  CaretakerPairingMaterial retry{};
  beginCaretakerGrant(fixture, now + 20U, 0x71U, retry, false);
  commitCaretakerGrant(fixture, retry, now + 23U);
  uint8_t storedRoot[32]{};
  ControllerRole storedRole = static_cast<ControllerRole>(0xFFU);
  assert(fixture.security.findControllerRoot(retry.controllerId, storedRoot,
                                             storedRole));
  assert(storedRole == ControllerRole::Caretaker);
  assert(memcmp(storedRoot, retry.controllerRoot, sizeof(storedRoot)) == 0);
}

void testCaretakerStorageFailureDoesNotIssueAndCanRetry() {
  Fixture fixture;
  const uint32_t now = 15000U;
  CaretakerPairingMaterial failed{};
  beginCaretakerGrant(fixture, now, 0x81U, failed);
  uint8_t clientProof[32]{};
  assert(kitsu868::companion::makeRoleBoundPairingProof(
             failed.controllerRoot, "client", "caretaker",
             failed.controllerId, "KT1234", failed.clientNonce,
             failed.deviceNonce, fixture.crypto, clientProof) ==
         ProtocolResult::Ok);
  const std::string commit = caretakerCommitJson(clientProof, "caretaker");
  const size_t framesBeforeCommit = fixture.transport.frames.size();
  fixture.storage.failWrite = true;
  fixture.session.onFrame(reinterpret_cast<const uint8_t*>(commit.data()),
                          commit.size(), now + 3U);
  fixture.storage.failWrite = false;
  assert(fixture.security.status().controllerCount == 0U);
  uint8_t missingRoot[32]{};
  assert(!fixture.security.findControllerRoot(failed.controllerId,
                                              missingRoot));
  assert(!fixture.session.status(now + 3U).pairingCompleted);
  for (size_t i = framesBeforeCommit; i < fixture.transport.frames.size(); ++i) {
    assert(fixture.transport.frames[i].find("\"pair_ok\"") ==
           std::string::npos);
  }
  assert(fixture.transport.frames.back().find("\"auth_failed\"") !=
         std::string::npos);

  CaretakerPairingMaterial retry{};
  beginCaretakerGrant(fixture, now + 10U, 0x91U, retry, false);
  commitCaretakerGrant(fixture, retry, now + 13U);
  ControllerRole storedRole = static_cast<ControllerRole>(0xFFU);
  assert(fixture.security.findControllerRoot(retry.controllerId, missingRoot,
                                             storedRole));
  assert(storedRole == ControllerRole::Caretaker);
}

void testPairingNeverPersistsBeforeCommit() {
  Fixture fixture;
  fixture.session.onSecureLinkEstablished(true, true, true, true, 1000U);
  fixture.session.setPairingWindow(true, 1000U, 1000U);
  uint8_t nonce[16]{};
  memset(nonce, 7, sizeof(nonce));
  const std::string request =
      "{\"v\":1,\"type\":\"pair_request\",\"client_nonce_b64\":\"" +
      b64(nonce, sizeof(nonce)) +
      "\",\"label\":\"Phone\",\"platform\":\"ios\"}";
  fixture.session.onFrame(reinterpret_cast<const uint8_t*>(request.data()),
                          request.size(), 1001U);
  assert(fixture.session.confirmPendingPairing(1002U));
  assert(fixture.security.status().controllerCount == 0U);
  fixture.session.onLinkClosed(1003U);
  assert(fixture.security.status().controllerCount == 0U);
}

void testPairingWindowSurvivesSecureLinkReconnect() {
  Fixture fixture;
  const uint32_t openedAt = 5000U;
  fixture.session.setPairingWindow(true, 1000U, openedAt);
  fixture.session.onSecureLinkEstablished(true, true, true, true,
                                          openedAt + 10U);
  fixture.session.onLinkClosed(openedAt + 20U);
  assert(fixture.session.status(openedAt + 20U).pairingWindowOpen);
  assert(fixture.session.status(openedAt + 20U).pairingRole ==
         ControllerRole::Owner);
  assert(fixture.session.status(openedAt + 20U).state ==
         BleSessionState::Disconnected);

  fixture.session.onSecureLinkEstablished(true, true, true, true,
                                          openedAt + 200U);
  fixture.session.onLinkClosed(openedAt + 210U);
  assert(fixture.session.status(openedAt + 210U).pairingWindowOpen);
  fixture.session.onSecureLinkEstablished(true, true, true, true,
                                          openedAt + 900U);
  uint8_t nonce[16]{};
  memset(nonce, 0x31, sizeof(nonce));
  const std::string request =
      "{\"v\":1,\"type\":\"pair_request\",\"client_nonce_b64\":\"" +
      b64(nonce, sizeof(nonce)) +
      "\",\"label\":\"Reconnect phone\",\"platform\":\"android\"}";
  fixture.session.onFrame(reinterpret_cast<const uint8_t*>(request.data()),
                          request.size(), openedAt + 950U);
  assert(fixture.transport.frames.back().find("\"pair_pending\"") !=
         std::string::npos);
  assert(fixture.transport.frames.back().find("\"expires_in_ms\":50") !=
         std::string::npos);
  assert(fixture.session.status(openedAt + 950U).physicalConfirmationPending);
}

void testCaretakerWindowRoleSurvivesSecureLinkReconnect() {
  Fixture fixture;
  const uint32_t openedAt = 5500U;
  fixture.session.setPairingWindow(true, 1000U, openedAt,
                                   ControllerRole::Caretaker);
  fixture.session.onSecureLinkEstablished(true, true, true, true,
                                          openedAt + 10U);
  fixture.session.onLinkClosed(openedAt + 20U);
  assert(fixture.session.status(openedAt + 20U).pairingWindowOpen);
  assert(fixture.session.status(openedAt + 20U).pairingRole ==
         ControllerRole::Caretaker);

  fixture.session.onSecureLinkEstablished(true, true, true, true,
                                          openedAt + 100U);
  uint8_t nonce[16]{};
  memset(nonce, 0x39, sizeof(nonce));
  const std::string request =
      "{\"v\":2,\"type\":\"pair_request\",\"client_nonce_b64\":\"" +
      b64(nonce, sizeof(nonce)) +
      "\",\"label\":\"Reconnect caretaker\",\"platform\":\"android\"}";
  fixture.session.onFrame(reinterpret_cast<const uint8_t*>(request.data()),
                          request.size(), openedAt + 101U);
  assert(fixture.transport.frames.back().find("\"v\":2") !=
         std::string::npos);
  assert(fixture.transport.frames.back().find("\"role\":\"caretaker\"") !=
         std::string::npos);
  assert(fixture.session.status(openedAt + 101U)
             .physicalConfirmationPending);
  assert(fixture.session.status(openedAt + 101U).pairingRole ==
         ControllerRole::Caretaker);
}

void testClosedAndExpiredPairingWindowsRejectReconnect() {
  {
    Fixture fixture;
    const uint32_t openedAt = 6000U;
    fixture.session.setPairingWindow(true, 1000U, openedAt);
    fixture.session.onSecureLinkEstablished(true, true, true, true,
                                            openedAt + 10U);
    fixture.session.onLinkClosed(openedAt + 20U);
    fixture.session.setPairingWindow(false, 0U, openedAt + 21U);
    fixture.session.onSecureLinkEstablished(true, true, true, true,
                                            openedAt + 30U);
    uint8_t nonce[16]{};
    memset(nonce, 0x41, sizeof(nonce));
    const std::string request =
        "{\"v\":1,\"type\":\"pair_request\",\"client_nonce_b64\":\"" +
        b64(nonce, sizeof(nonce)) +
        "\",\"label\":\"Closed window\",\"platform\":\"android\"}";
    fixture.session.onFrame(reinterpret_cast<const uint8_t*>(request.data()),
                            request.size(), openedAt + 40U);
    assert(fixture.transport.frames.back().find("\"pairing_closed\"") !=
           std::string::npos);
    assert(!fixture.session.status(openedAt + 40U).pairingWindowOpen);
    assert(!fixture.session.status(openedAt + 40U)
                .physicalConfirmationPending);
  }

  {
    Fixture fixture;
    const uint32_t openedAt = 7000U;
    fixture.session.setPairingWindow(true, 100U, openedAt);
    fixture.session.onSecureLinkEstablished(true, true, true, true,
                                            openedAt + 10U);
    fixture.session.onLinkClosed(openedAt + 20U);
    assert(fixture.session.status(openedAt + 99U).pairingWindowOpen);
    fixture.session.loop(openedAt + 100U);
    assert(!fixture.session.status(openedAt + 100U).pairingWindowOpen);
    fixture.session.onSecureLinkEstablished(true, true, true, true,
                                            openedAt + 101U);
    uint8_t nonce[16]{};
    memset(nonce, 0x51, sizeof(nonce));
    const std::string request =
        "{\"v\":1,\"type\":\"pair_request\",\"client_nonce_b64\":\"" +
        b64(nonce, sizeof(nonce)) +
        "\",\"label\":\"Expired window\",\"platform\":\"ios\"}";
    fixture.session.onFrame(reinterpret_cast<const uint8_t*>(request.data()),
                            request.size(), openedAt + 102U);
    assert(fixture.transport.frames.back().find("\"pairing_closed\"") !=
           std::string::npos);
    assert(!fixture.session.status(openedAt + 102U)
                .physicalConfirmationPending);
  }
}

void testPendingPairingGrantCannotCrossReconnect() {
  Fixture fixture;
  const uint32_t openedAt = 8000U;
  fixture.session.setPairingWindow(true, 1000U, openedAt);
  fixture.session.onSecureLinkEstablished(true, true, true, true,
                                          openedAt + 10U);
  uint8_t clientNonce[16]{};
  memset(clientNonce, 0x61, sizeof(clientNonce));
  const std::string request =
      "{\"v\":1,\"type\":\"pair_request\",\"client_nonce_b64\":\"" +
      b64(clientNonce, sizeof(clientNonce)) +
      "\",\"label\":\"Interrupted phone\",\"platform\":\"android\"}";
  fixture.session.onFrame(reinterpret_cast<const uint8_t*>(request.data()),
                          request.size(), openedAt + 20U);
  assert(fixture.session.confirmPendingPairing(openedAt + 30U));
  const std::string grant = fixture.transport.frames.back();

  uint8_t controllerId[16]{};
  uint8_t controllerRoot[32]{};
  uint8_t deviceNonce[16]{};
  decodeField(grant, "controller_id_b64", controllerId,
              sizeof(controllerId));
  decodeField(grant, "root_b64", controllerRoot, sizeof(controllerRoot));
  decodeField(grant, "device_nonce_b64", deviceNonce, sizeof(deviceNonce));
  uint8_t clientProof[32]{};
  assert(kitsu868::companion::makePairingProof(
             controllerRoot, "client", controllerId, "KT1234", clientNonce,
             deviceNonce, fixture.crypto, clientProof) == ProtocolResult::Ok);
  const std::string staleCommit =
      "{\"v\":1,\"type\":\"pair_commit\",\"proof_b64\":\"" +
      b64(clientProof, sizeof(clientProof)) + "\"}";

  fixture.session.onLinkClosed(openedAt + 40U);
  assert(fixture.session.status(openedAt + 40U).pairingWindowOpen);
  assert(!fixture.session.confirmPendingPairing(openedAt + 41U));
  fixture.session.onSecureLinkEstablished(true, true, true, true,
                                          openedAt + 50U);
  fixture.session.onFrame(
      reinterpret_cast<const uint8_t*>(staleCommit.data()),
      staleCommit.size(), openedAt + 60U);
  assert(fixture.transport.frames.back().find("\"auth_failed\"") !=
         std::string::npos);
  assert(fixture.security.status().controllerCount == 0U);
  assert(!fixture.session.status(openedAt + 60U)
              .physicalConfirmationPending);
}

void testAuthenticatedSessionCannotCrossReconnect() {
  Fixture fixture;
  uint8_t controllerId[16]{};
  uint8_t controllerRoot[32]{};
  pairController(fixture, controllerId, controllerRoot);

  uint8_t c2d[32]{};
  uint8_t d2c[32]{};
  authenticateController(fixture, controllerId, controllerRoot, c2d, d2c);
  uint8_t nonce[16]{};
  uint8_t requestId[16]{};
  memset(nonce, 0x71, sizeof(nonce));
  memset(requestId, 0x72, sizeof(requestId));
  static const uint8_t payload[] = "{}";
  uint8_t staleRequest[1024]{};
  size_t staleRequestBytes = 0U;
  assert(kitsu868::companion::encodeEnvelope(
             EnvelopeChannel::Request, 1U, nonce, requestId, "action.apply",
             payload, sizeof(payload) - 1U, c2d, fixture.crypto,
             staleRequest, sizeof(staleRequest), staleRequestBytes) ==
         ProtocolResult::Ok);

  fixture.session.onLinkClosed(2100U);
  assert(!fixture.session.status(2100U).applicationAuthenticated);
  fixture.session.onSecureLinkEstablished(true, true, true, true, 2101U);
  fixture.session.onFrame(staleRequest, staleRequestBytes, 2102U);
  assert(fixture.operations.calls == 0U);
  assert(!fixture.session.status(2102U).applicationAuthenticated);
  assert(fixture.transport.frames.back().find("\"auth_failed\"") !=
         std::string::npos);
}

void testPairingCapacityAndAuthenticationBackoffRemainEnforced() {
  {
    Fixture fixture;
    uint8_t controllerId[16]{};
    uint8_t controllerRoot[32]{};
    for (size_t controller = 0U; controller < kKitsuControllerCapacity;
         ++controller) {
      pairController(fixture, controllerId, controllerRoot);
    }
    assert(fixture.security.status().controllerCount ==
           kKitsuControllerCapacity);

    fixture.session.onSecureLinkEstablished(true, true, true, true, 3000U);
    fixture.session.setPairingWindow(true, 1000U, 3000U);
    uint8_t nonce[16]{};
    memset(nonce, 0x81, sizeof(nonce));
    const std::string request =
        "{\"v\":1,\"type\":\"pair_request\",\"client_nonce_b64\":\"" +
        b64(nonce, sizeof(nonce)) +
        "\",\"label\":\"Fifth phone\",\"platform\":\"android\"}";
    fixture.session.onFrame(reinterpret_cast<const uint8_t*>(request.data()),
                            request.size(), 3001U);
    assert(fixture.transport.frames.back().find("\"controller_full\"") !=
           std::string::npos);
    assert(!fixture.session.status(3001U).physicalConfirmationPending);
  }

  {
    Fixture fixture;
    const uint32_t openedAt = 10000U;
    fixture.session.setPairingWindow(true, 60000U, openedAt);
    fixture.session.onSecureLinkEstablished(true, true, true, true,
                                            openedAt + 1U);
    static const uint8_t invalidFrame[] = "{}";
    fixture.session.onFrame(invalidFrame, sizeof(invalidFrame) - 1U,
                            openedAt + 2U);
    fixture.session.onFrame(invalidFrame, sizeof(invalidFrame) - 1U,
                            openedAt + 3U);
    fixture.session.onFrame(invalidFrame, sizeof(invalidFrame) - 1U,
                            openedAt + 4U);
    assert(fixture.session.status(openedAt + 4U).proofFailures == 3U);
    assert(fixture.session.status(openedAt + 4U).state ==
           BleSessionState::Closing);

    fixture.session.onLinkClosed(openedAt + 5U);
    assert(fixture.session.status(openedAt + 5U).state ==
           BleSessionState::Backoff);
    assert(fixture.session.status(openedAt + 5U).pairingWindowOpen);
    fixture.transport.disconnected = false;
    fixture.session.onSecureLinkEstablished(true, true, true, true,
                                            openedAt + 6U);
    assert(fixture.session.status(openedAt + 6U).state ==
           BleSessionState::Backoff);
    assert(fixture.transport.disconnected);

    const uint32_t backoffEnds = openedAt + 4U + kBleControllerBackoffMs;
    fixture.session.loop(backoffEnds);
    assert(fixture.session.status(backoffEnds).state ==
           BleSessionState::Disconnected);
    fixture.transport.disconnected = false;
    fixture.session.onSecureLinkEstablished(true, true, true, true,
                                            backoffEnds + 1U);
    uint8_t nonce[16]{};
    memset(nonce, 0x91, sizeof(nonce));
    const std::string request =
        "{\"v\":1,\"type\":\"pair_request\",\"client_nonce_b64\":\"" +
        b64(nonce, sizeof(nonce)) +
        "\",\"label\":\"After backoff\",\"platform\":\"android\"}";
    fixture.session.onFrame(reinterpret_cast<const uint8_t*>(request.data()),
                            request.size(), backoffEnds + 2U);
    assert(fixture.transport.frames.back().find("\"pair_pending\"") !=
           std::string::npos);
  }
}

}  // namespace

int main() {
  testPairingHandshakeEnvelopeAndReplay();
  testCaretakerRoleIsEnforcedBeforeDelegation();
  testEncounterOperationsReachDelegate();
  testUnsolicitedEventBarrierRequiresQueuedAuthenticatedResponse();
  testAuthenticatedControllerForgetDrainsReceipt();
  testPartialControllerForgetCannotKeepUsingSession();
  testStrictControlsAndTimeout();
  testPairingRoleDowngradeEscalationAndRetry();
  testCaretakerCommitRoleAndTranscriptTamperThenRetry();
  testCaretakerStorageFailureDoesNotIssueAndCanRetry();
  testPairingNeverPersistsBeforeCommit();
  testPairingWindowSurvivesSecureLinkReconnect();
  testCaretakerWindowRoleSurvivesSecureLinkReconnect();
  testClosedAndExpiredPairingWindowsRejectReconnect();
  testPendingPairingGrantCannotCrossReconnect();
  testAuthenticatedSessionCannotCrossReconnect();
  testPairingCapacityAndAuthenticationBackoffRemainEnforced();
  return 0;
}
