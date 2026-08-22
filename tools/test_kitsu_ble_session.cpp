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
using kitsu868::connectivity::DeviceSecurityPlatform;
using kitsu868::connectivity::DeviceSecurityStorage;
using kitsu868::connectivity::KitsuBleSession;
using kitsu868::connectivity::KitsuDeviceSecurity;
using kitsu868::connectivity::SecurityMode;
using kitsu868::connectivity::SecurityResult;
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
    memcpy(data[slot], input, inputBytes);
    sizes[slot] = inputBytes;
    return true;
  }

  uint8_t data[kSecuritySlots][kSecurityBlobCapacity]{};
  size_t sizes[kSecuritySlots]{};
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

  void disconnectBle() override { disconnected = true; }

  std::vector<std::string> frames;
  bool applicationAuthenticated = false;
  bool disconnected = false;
  bool failSend = false;
  bool failAuthSwitch = false;
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

  uint32_t calls = 0U;
  std::string lastOperation;
  std::string lastPayload;
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
  assert(fixture.security.status().controllerCount == 0U);
  assert(fixture.transport.frames.back().find("\"pair_pending\"") !=
         std::string::npos);

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
  uint8_t expectedDeviceProof[32]{};
  assert(kitsu868::companion::makePairingProof(
             controllerRoot, "device", controllerId, "KT1234", clientNonce,
             deviceNonce, fixture.crypto, expectedDeviceProof) ==
         ProtocolResult::Ok);
  assert(memcmp(suppliedDeviceProof, expectedDeviceProof,
                sizeof(expectedDeviceProof)) == 0);

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
  assert(fixture.security.status().controllerCount == 1U);
  uint8_t persisted[32]{};
  assert(fixture.security.findControllerRoot(controllerId, persisted));
  assert(memcmp(persisted, controllerRoot, sizeof(persisted)) == 0);
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
  assert(fixture.transport.frames.back().find("\"device_ok\"") !=
         std::string::npos);
  assert(kitsu868::companion::deriveBleSessionKeys(
             controllerRoot, clientNonce, deviceNonce, fixture.crypto,
             c2d, d2c) == ProtocolResult::Ok);
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
  const uint8_t payload[] =
      "{\"gateway_id\":\"12345678-1234-4abc-8def-1234567890ab\"}";
  uint8_t requestJson[1024]{};
  size_t requestBytes = 0U;
  assert(kitsu868::companion::encodeEnvelope(
             EnvelopeChannel::Request, 1U, nonce, requestId, "gateway.forget",
             payload, sizeof(payload) - 1U, c2d, fixture.crypto, requestJson,
             sizeof(requestJson), requestBytes) == ProtocolResult::Ok);
  fixture.session.onFrame(requestJson, requestBytes, 2010U);
  assert(fixture.operations.calls == 1U);
  assert(fixture.operations.lastOperation == "gateway.forget");
  assert(fixture.operations.lastPayload ==
         "{\"gateway_id\":\"12345678-1234-4abc-8def-1234567890ab\"}");

  const std::string response = fixture.transport.frames.back();
  uint8_t decodedPayload[128]{};
  DecodedEnvelope decoded{};
  assert(kitsu868::companion::decodeAndVerifyEnvelope(
             reinterpret_cast<const uint8_t*>(response.data()),
             response.size(), d2c, EnvelopeChannel::Response, 1U,
             fixture.crypto, decoded, decodedPayload,
             sizeof(decodedPayload)) == ProtocolResult::Ok);
  assert(strcmp(decoded.operation, "gateway.forget") == 0);
  assert(std::string(reinterpret_cast<char*>(decodedPayload),
                     decoded.payloadBytes) == "{\"ok\":true}");

  // Exact sequence enforcement rejects the replay before delegate execution.
  fixture.session.onFrame(requestJson, requestBytes, 2011U);
  assert(fixture.operations.calls == 1U);
  assert(fixture.session.status(2011U).state == BleSessionState::Closing);
  fixture.session.loop(2300U);
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

}  // namespace

int main() {
  testPairingHandshakeEnvelopeAndReplay();
  testStrictControlsAndTimeout();
  testPairingNeverPersistsBeforeCommit();
  return 0;
}
