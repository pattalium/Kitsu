#include "../src/kitsu_gateway_lan_runtime.h"
#include "../src/kitsu_mobile_relay.h"

#include <assert.h>
#include <string.h>
#include <windows.h>
#include <bcrypt.h>

#include <deque>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

using namespace kitsu868;
using namespace kitsu868::connectivity;

bool good(NTSTATUS status) { return status >= 0; }

class WindowsHashes final : public companion::CompanionCrypto {
 public:
  bool randomBytes(uint8_t* output, size_t outputBytes) override {
    if (!output && outputBytes != 0U) return false;
    for (size_t i = 0U; i < outputBytes; ++i) {
      state_ = state_ * 1664525UL + 1013904223UL;
      output[i] = static_cast<uint8_t>(state_ >> 24U);
    }
    return true;
  }

  bool sha256(const companion::CryptoPart* parts, size_t count,
              uint8_t output[32]) override {
    return hash(nullptr, 0U, parts, count, output);
  }

  bool hmacSha256(const uint8_t key[32],
                  const companion::CryptoPart* parts, size_t count,
                  uint8_t output[32]) override {
    return hash(key, 32U, parts, count, output);
  }

  bool hkdfSha256(const uint8_t inputKey[32], const uint8_t* salt,
                  size_t saltBytes, const uint8_t* info, size_t infoBytes,
                  uint8_t output[32]) override {
    uint8_t prk[32]{};
    const companion::CryptoPart extract[] = {
        companion::CryptoPart(inputKey, 32U)};
    if (!hash(salt, saltBytes, extract, 1U, prk)) return false;
    const uint8_t counter = 1U;
    const companion::CryptoPart expand[] = {
        companion::CryptoPart(info, infoBytes),
        companion::CryptoPart(&counter, 1U)};
    const bool ok = hash(prk, sizeof(prk), expand, 2U, output);
    SecureZeroMemory(prk, sizeof(prk));
    return ok;
  }

 private:
  static bool hash(const uint8_t* key, size_t keyBytes,
                   const companion::CryptoPart* parts, size_t count,
                   uint8_t output[32]) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE handle = nullptr;
    if (!output || !good(BCryptOpenAlgorithmProvider(
                       &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                       key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0U))) {
      return false;
    }
    DWORD objectBytes = 0U;
    DWORD copied = 0U;
    bool ok = good(BCryptGetProperty(
        algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &copied,
        0U));
    std::vector<uint8_t> object(objectBytes);
    if (ok) {
      ok = good(BCryptCreateHash(
          algorithm, &handle, object.data(), objectBytes,
          const_cast<PUCHAR>(key), static_cast<ULONG>(keyBytes), 0U));
    }
    for (size_t i = 0U; ok && i < count; ++i) {
      if (parts[i].bytes > ULONG_MAX) {
        ok = false;
      } else if (parts[i].bytes != 0U) {
        ok = good(BCryptHashData(
            handle, const_cast<PUCHAR>(parts[i].data),
            static_cast<ULONG>(parts[i].bytes), 0U));
      }
    }
    if (ok) ok = good(BCryptFinishHash(handle, output, 32U, 0U));
    if (handle) BCryptDestroyHash(handle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0U);
    if (!object.empty()) SecureZeroMemory(object.data(), object.size());
    return ok;
  }

  uint32_t state_ = 7U;
};

bool fromHex(const char* input, uint8_t* output, size_t outputBytes) {
  if (!input || !output || strlen(input) != outputBytes * 2U) return false;
  for (size_t i = 0U; i < outputBytes; ++i) {
    unsigned value = 0U;
    if (sscanf_s(input + i * 2U, "%2x", &value) != 1) return false;
    output[i] = static_cast<uint8_t>(value);
  }
  return true;
}

class Provider final : public GatewayLanCredentialProvider {
 public:
  Provider() {
    assert(fromHex("ffeeddccbbaa99887766554433221100",
                   companion, sizeof(companion)));
    assert(fromHex("f0e0d0c0b0a090807060504030201000",
                   gateway, sizeof(gateway)));
    memset(ca, 0x30, sizeof(ca));
    memset(leaf, 0x31, sizeof(leaf));
    memset(chain, 0x32, sizeof(chain));
    memset(privateKey, 0x33, sizeof(privateKey));
    for (size_t i = 0U; i < sizeof(secret); ++i) {
      secret[i] = static_cast<uint8_t>(i);
      spki[i] = static_cast<uint8_t>(0x80U + i);
    }
  }

  bool remoteConnectivityAllowed() const override { return allowed; }

  bool acquire(GatewayLanCredentialView& output) override {
    if (!available) return false;
    output = GatewayLanCredentialView{};
    output.host = host;
    output.serverName = serverName;
    output.port = 7443U;
    output.caCertificateDer = ca;
    output.caCertificateBytes = sizeof(ca);
    memcpy(output.spkiSha256, spki, sizeof(spki));
    memcpy(output.companionUuid, companion, sizeof(companion));
    memcpy(output.gatewayUuid, gateway, sizeof(gateway));
    output.keyVersion = keyVersion;
    output.privateKey = privateKey;
    output.privateKeyBytes = sizeof(privateKey);
    output.leafCertificateDer = leaf;
    output.leafCertificateBytes = sizeof(leaf);
    output.certificateChainDer[0] = chain;
    output.certificateChainBytes[0] = sizeof(chain);
    output.certificateChainCount = 1U;
    output.backendHmacSecret = secret;
    output.backendHmacSecretBytes = sizeof(secret);
    ++acquires;
    return true;
  }

  void release(GatewayLanCredentialView& view) override {
    view = GatewayLanCredentialView{};
    ++releases;
  }

  bool allowed = true;
  bool available = true;
  unsigned acquires = 0U;
  unsigned releases = 0U;
  uint32_t keyVersion = 0x01020304UL;
  const char* host = "gateway.example";
  const char* serverName = "gateway.k32.run";
  uint8_t ca[8]{};
  uint8_t leaf[8]{};
  uint8_t chain[8]{};
  uint8_t privateKey[32]{};
  uint8_t secret[32]{};
  uint8_t spki[32]{};
  uint8_t companion[16]{};
  uint8_t gateway[16]{};
};

class Sequences final : public GatewayLanSequenceStore {
 public:
  bool remoteConnectivityAllowed() const override { return allowed; }
  bool reserveTx(uint16_t blockSize, uint64_t& first,
                 uint64_t& last) override {
    ++reservations;
    if (fail) return false;
    first = reserved + 1U;
    reserved += blockSize;
    last = reserved;
    return true;
  }
  bool acceptNextRx(uint64_t sequence) override {
    if (fail || sequence != rx + 1U) return false;
    rx = sequence;
    return true;
  }
  uint64_t rxHighWater() const override { return rx; }

  bool allowed = true;
  bool fail = false;
  unsigned reservations = 0U;
  uint64_t reserved = 0U;
  uint64_t rx = 0U;
};

class Replay final : public LanActionReplayStore {
 public:
  LanReplayDecision acceptAction(const uint8_t actionId[16], int64_t,
                                 int64_t) override {
    if (fail) return LanReplayDecision::Failed;
    if (seen && memcmp(id, actionId, sizeof(id)) == 0) {
      return LanReplayDecision::Duplicate;
    }
    memcpy(id, actionId, sizeof(id));
    seen = true;
    return LanReplayDecision::Fresh;
  }
  bool fail = false;
  bool seen = false;
  uint8_t id[16]{};
};

class Sink final : public GatewayLanActionSink {
 public:
  bool acceptAuthenticatedAction(
      const uint8_t* framedJson, size_t framedJsonBytes,
      const LanGatewayFrame&, const uint8_t* paramsJson,
      size_t paramsJsonBytes, int64_t) override {
    ++calls;
    frame.assign(framedJson, framedJson + framedJsonBytes);
    params.assign(paramsJson, paramsJson + paramsJsonBytes);
    return accept;
  }
  bool repeatAuthenticatedAction(
      const uint8_t* framedJson, size_t framedJsonBytes,
      const LanGatewayFrame&, const uint8_t* paramsJson,
      size_t paramsJsonBytes, int64_t) override {
    ++repeats;
    repeatedFrame.assign(framedJson, framedJson + framedJsonBytes);
    repeatedParams.assign(paramsJson, paramsJson + paramsJsonBytes);
    return accept;
  }
  bool accept = true;
  unsigned calls = 0U;
  unsigned repeats = 0U;
  std::vector<uint8_t> frame;
  std::vector<uint8_t> params;
  std::vector<uint8_t> repeatedFrame;
  std::vector<uint8_t> repeatedParams;
};

class RelayConfigSink final : public MobileRelayGatewayConfigSink {
 public:
  MobileRelayGatewayConfigResult commitMobileRelayGateway(
      const uint8_t gatewayUuid[16], const uint8_t* caCertificateDer,
      size_t caCertificateBytes) override {
    if (!accept || !gatewayUuid || !caCertificateDer ||
        caCertificateBytes == 0U) {
      return MobileRelayGatewayConfigResult::Failed;
    }
    if (gateway.size() == 16U && ca.size() == caCertificateBytes &&
        memcmp(gateway.data(), gatewayUuid, 16U) == 0 &&
        memcmp(ca.data(), caCertificateDer, caCertificateBytes) == 0) {
      return MobileRelayGatewayConfigResult::Unchanged;
    }
    gateway.assign(gatewayUuid, gatewayUuid + 16U);
    ca.assign(caCertificateDer, caCertificateDer + caCertificateBytes);
    return MobileRelayGatewayConfigResult::Changed;
  }

  bool accept = true;
  std::vector<uint8_t> gateway;
  std::vector<uint8_t> ca;
};

class RelayEnrollment final : public MobileRelayEnrollmentDelegate {
 public:
  bool buildMobileRelayEnrollmentRequest(
      uint8_t* output, size_t outputCapacity,
      size_t& outputBytes) override {
    outputBytes = 0U;
    if (!output || request.size() > outputCapacity) return false;
    memcpy(output, request.data(), request.size());
    outputBytes = request.size();
    return true;
  }

  MobileRelayResult installMobileRelayEnrollmentResponse(
      const uint8_t* response, size_t responseBytes) override {
    if (!response) return MobileRelayResult::EnrollmentUnavailable;
    if (installResult != MobileRelayResult::Ok) return installResult;
    installed.assign(response, response + responseBytes);
    return MobileRelayResult::Ok;
  }

  std::string request = "{\"claim_token\":\"exact-inner-request\"}";
  MobileRelayResult installResult = MobileRelayResult::Ok;
  std::vector<uint8_t> installed;
};

class Transport final : public GatewayLanTlsTransport {
 public:
  GatewayLanIoResult beginConnect(const GatewayLanCredentialView&,
                                  const char* alpn, uint32_t,
                                  GatewayLanTlsEvidence& output) override {
    ++connects;
    assert(!connectPendingValue);
    assert(strcmp(alpn, kGatewayLanAlpn) == 0);
    if (connectWouldBlock != 0U) {
      --connectWouldBlock;
      connectPendingValue = true;
      return GatewayLanIoResult::WouldBlock;
    }
    return finishConnect(output);
  }
  GatewayLanIoResult pollConnect(
      GatewayLanTlsEvidence& output) override {
    ++connectPolls;
    if (!connectPendingValue) return GatewayLanIoResult::InvalidArgument;
    if (connectWouldBlock != 0U) {
      --connectWouldBlock;
      return GatewayLanIoResult::WouldBlock;
    }
    connectPendingValue = false;
    return finishConnect(output);
  }
  GatewayLanIoResult finishConnect(GatewayLanTlsEvidence& output) {
    if (connectResult != GatewayLanIoResult::Ok) return connectResult;
    output.serverChainVerified = evidenceGood;
    output.serverNameVerified = evidenceGood;
    output.spkiMatched = evidenceGood;
    output.alpnMatched = evidenceGood;
    output.tlsVersionAtLeast12 = evidenceGood;
    output.systemTimeValid = evidenceGood;
    output.clientCredentialPresented = evidenceGood;
    output.clientCertificateBindsCompanion = evidenceGood;
    connectedValue = true;
    return GatewayLanIoResult::Ok;
  }
  GatewayLanIoResult writeOneFrame(const uint8_t* frame, size_t frameBytes,
                                   uint32_t) override {
    if (!connectedValue) return GatewayLanIoResult::Closed;
    if (writeWouldBlock != 0U) {
      --writeWouldBlock;
      return GatewayLanIoResult::WouldBlock;
    }
    writes.emplace_back(frame, frame + frameBytes);
    return writeResult;
  }
  GatewayLanIoResult receiveOneFrame(const uint8_t*& frame,
                                     size_t& frameBytes, uint32_t,
                                     uint32_t) override {
    frame = nullptr;
    frameBytes = 0U;
    if (!connectedValue) return GatewayLanIoResult::Closed;
    if (incoming.empty()) return GatewayLanIoResult::WouldBlock;
    frame = incoming.front().data();
    frameBytes = incoming.front().size();
    return GatewayLanIoResult::Ok;
  }
  void consumeReceivedFrame() override {
    if (!incoming.empty()) incoming.pop_front();
  }
  bool connected() const override { return connectedValue; }
  void close() override {
    connectedValue = false;
    connectPendingValue = false;
    ++closes;
  }

  void push(const std::string& value) {
    incoming.emplace_back(value.begin(), value.end());
  }

  bool evidenceGood = true;
  bool connectedValue = false;
  bool connectPendingValue = false;
  unsigned connects = 0U;
  unsigned connectPolls = 0U;
  unsigned closes = 0U;
  unsigned connectWouldBlock = 0U;
  unsigned writeWouldBlock = 0U;
  GatewayLanIoResult connectResult = GatewayLanIoResult::Ok;
  GatewayLanIoResult writeResult = GatewayLanIoResult::Ok;
  std::vector<std::vector<uint8_t>> writes;
  std::deque<std::vector<uint8_t>> incoming;
};

std::string ack(uint64_t sequence, uint64_t record = 7U) {
  return std::string("{\"v\":1,\"type\":\"gateway_ack\"," 
                     "\"spool_record_id\":\"") +
         std::to_string(record) + "\",\"device_sequence\":\"" +
         std::to_string(sequence) + "\"}";
}

std::string actionJson() {
  return "{\"schema\":\"kitsu.remote-action.v1\"," 
         "\"action_id\":\"00112233-4455-6677-8899-aabbccddeeff\"," 
         "\"companion_id\":\"ffeeddcc-bbaa-9988-7766-554433221100\"," 
         "\"key_version\":16909060," 
         "\"nonce_b64\":\"AAECAwQFBgcICQoLDA0ODw\"," 
         "\"action_type\":\"companion.pet\"," 
         "\"created_epoch\":\"1800000000\"," 
         "\"expires_epoch\":\"1800000060\"," 
         "\"params_b64\":\"eyJnZXN0dXJlIjoiZWFyLXNjcmF0Y2gifQ\"," 
         "\"signature_b64\":\"Ba24Hq65ANHWNZ3ZDYkfhVQ1KorRFzfLgmxPEBTNdQ4\"}";
}

std::string base64Url(const std::string& value) {
  std::string output(
      companion::base64UrlEncodedBytes(value.size()), '\0');
  size_t outputBytes = 0U;
  assert(companion::encodeBase64Url(
      reinterpret_cast<const uint8_t*>(value.data()), value.size(),
      output.data(), output.size(), outputBytes));
  output.resize(outputBytes);
  return output;
}

std::string relayPull(const char* kind, size_t offset = 0U) {
  return std::string("{\"schema\":\"kitsu.mobile-relay.exchange.v1\","
                     "\"kind\":\"") +
      kind + "\",\"offset\":" + std::to_string(offset) + "}";
}

std::string relayPush(const char* kind, const std::string& value,
                      size_t offset = 0U, size_t total = 0U,
                      bool final = true) {
  if (total == 0U) total = offset + value.size();
  return std::string("{\"schema\":\"kitsu.mobile-relay.exchange.v1\","
                     "\"kind\":\"") +
      kind + "\",\"offset\":" + std::to_string(offset) +
      ",\"total\":" + std::to_string(total) +
      ",\"data_b64\":\"" + base64Url(value) +
      "\",\"final\":" + (final ? "true}" : "false}");
}

std::string decodedRelayChunk(const std::string& response) {
  static const std::string prefix = "\"data_b64\":\"";
  const size_t start = response.find(prefix);
  assert(start != std::string::npos);
  const size_t dataStart = start + prefix.size();
  const size_t end = response.find('"', dataStart);
  assert(end != std::string::npos);
  const std::string encoded = response.substr(dataStart, end - dataStart);
  std::vector<uint8_t> decoded(encoded.size());
  size_t decodedBytes = 0U;
  assert(companion::decodeBase64Url(
      encoded.data(), encoded.size(), decoded.data(), decoded.size(),
      decodedBytes));
  return std::string(reinterpret_cast<const char*>(decoded.data()),
                     decodedBytes);
}

struct Fixture {
  Provider provider;
  Sequences sequences;
  WindowsHashes hashes;
  Replay replay;
  Sink sink;
  Transport transport;
  KitsuGatewayLanRuntime runtime;

  void begin() {
    assert(runtime.begin(provider, sequences, hashes, replay, sink,
                         transport) == GatewayLanRuntimeResult::Ok);
  }
};

void testConnectivityGateAndBoundedQueue() {
  Fixture fixture;
  fixture.provider.allowed = false;
  assert(fixture.runtime.begin(fixture.provider, fixture.sequences,
                               fixture.hashes, fixture.replay, fixture.sink,
                               fixture.transport) ==
         GatewayLanRuntimeResult::RemoteConnectivityUnavailable);
  static const uint8_t payload[] = "{}";
  assert(fixture.runtime.enqueueDevicePayload(
             "heartbeat", payload, sizeof(payload) - 1U, 0) ==
         GatewayLanRuntimeResult::RemoteConnectivityUnavailable);
  assert(fixture.transport.connects == 0U);

  fixture.provider.allowed = true;
  fixture.runtime.stop();
  fixture.begin();
  for (size_t i = 0U; i < kGatewayLanQueueDepth; ++i) {
    assert(fixture.runtime.enqueueDevicePayload(
               "heartbeat", payload, sizeof(payload) - 1U, 0) ==
           GatewayLanRuntimeResult::Ok);
  }
  assert(fixture.runtime.status().queuedFrames == kGatewayLanQueueDepth);
  assert(fixture.runtime.status().queuedBytes <=
         kGatewayLanMaximumQueuedBytes);
  assert(fixture.runtime.enqueueDevicePayload(
             "heartbeat", payload, sizeof(payload) - 1U, 0) ==
         GatewayLanRuntimeResult::QueueFull);
}

void testEndpointHostAcceptsDnsIpv4AndIpv6ButSniRequiresDns() {
  static const uint8_t payload[] = "{}";
  for (const char* endpoint : {"gateway.example", "192.0.2.10",
                               "2001:db8::1", "::1"}) {
    Fixture fixture;
    fixture.provider.host = endpoint;
    fixture.begin();
    assert(fixture.runtime.enqueueDevicePayload(
               "heartbeat", payload, sizeof(payload) - 1U, 0) ==
           GatewayLanRuntimeResult::Ok);
  }
  for (const char* endpoint : {"2001:::1", "not_a_host"}) {
    Fixture fixture;
    fixture.provider.host = endpoint;
    fixture.begin();
    assert(fixture.runtime.enqueueDevicePayload(
               "heartbeat", payload, sizeof(payload) - 1U, 0) ==
           GatewayLanRuntimeResult::CredentialsInvalid);
  }
  Fixture numericSni;
  numericSni.provider.serverName = "192.0.2.10";
  numericSni.begin();
  assert(numericSni.runtime.enqueueDevicePayload(
             "heartbeat", payload, sizeof(payload) - 1U, 0) ==
         GatewayLanRuntimeResult::CredentialsInvalid);
}

void testAckAndExactRetryWithBackoff() {
  Fixture fixture;
  fixture.begin();
  static const uint8_t payload[] = "{\"battery_pct\":87}";
  uint64_t sequence = 0U;
  assert(fixture.runtime.enqueueDevicePayload(
             "companion.snapshot", payload, sizeof(payload) - 1U,
             1800000000LL, &sequence) == GatewayLanRuntimeResult::Ok);
  assert(sequence == 1U);
  assert(fixture.sequences.reservations == 1U);
  assert(fixture.runtime.poll(100U, 1800000030LL, true) ==
         GatewayLanRuntimeResult::Ok);
  assert(fixture.transport.writes.size() == 1U);
  const std::vector<uint8_t> original = fixture.transport.writes[0];

  fixture.transport.push(ack(sequence + 1U));
  assert(fixture.runtime.poll(101U, 1800000030LL, true) ==
         GatewayLanRuntimeResult::UnexpectedAck);
  assert(!fixture.runtime.status().connected);
  assert(fixture.runtime.status().queuedFrames == 1U);
  const unsigned connects = fixture.transport.connects;
  assert(fixture.runtime.poll(500U, 1800000030LL, true) ==
         GatewayLanRuntimeResult::Ok);
  assert(fixture.transport.connects == connects);

  assert(fixture.runtime.poll(1101U, 1800000030LL, true) ==
         GatewayLanRuntimeResult::Ok);
  assert(fixture.transport.connects == connects + 1U);
  assert(fixture.transport.writes.size() == 2U);
  assert(fixture.transport.writes[1] == original);
  fixture.transport.push(ack(sequence, 99U));
  assert(fixture.runtime.poll(1102U, 1800000030LL, true) ==
         GatewayLanRuntimeResult::Ok);
  assert(fixture.runtime.status().queuedFrames == 0U);
  assert(fixture.runtime.status().lastAckedSequence == sequence);
  assert(fixture.runtime.status().lastSpoolRecordId == 99U);
}

void testByteIdenticalActionAndPersistentRxCount() {
  Fixture fixture;
  fixture.begin();
  assert(fixture.runtime.poll(1U, 1800000030LL, true) ==
         GatewayLanRuntimeResult::Ok);
  const std::string action = actionJson();
  fixture.transport.push(action);
  assert(fixture.runtime.poll(2U, 1800000030LL, true) ==
         GatewayLanRuntimeResult::Ok);
  assert(fixture.sink.calls == 1U);
  assert(fixture.sequences.rx == 1U);
  assert(std::string(fixture.sink.frame.begin(), fixture.sink.frame.end()) ==
         action);
  static const std::string params = "{\"gesture\":\"ear-scratch\"}";
  assert(std::string(fixture.sink.params.begin(), fixture.sink.params.end()) ==
         params);

  fixture.transport.push(action);
  assert(fixture.runtime.poll(3U, 1800000030LL, true) ==
         GatewayLanRuntimeResult::Ok);
  assert(fixture.sink.calls == 1U);
  assert(fixture.sink.repeats == 1U);
  assert(std::string(fixture.sink.repeatedFrame.begin(),
                     fixture.sink.repeatedFrame.end()) == action);
  assert(std::string(fixture.sink.repeatedParams.begin(),
                     fixture.sink.repeatedParams.end()) == params);
  assert(fixture.sequences.rx == 1U);
}

void testTrustEvidenceIsMandatory() {
  Fixture fixture;
  fixture.begin();
  fixture.transport.evidenceGood = false;
  assert(fixture.runtime.poll(10U, 1800000030LL, true) ==
         GatewayLanRuntimeResult::TrustRejected);
  assert(!fixture.runtime.status().connected);
  assert(fixture.runtime.status().consecutiveFailures == 1U);
}

void testConnectAndWriteWouldBlockAreProgressNotFailures() {
  Fixture fixture;
  fixture.begin();
  fixture.transport.connectWouldBlock = 2U;
  fixture.transport.writeWouldBlock = 2U;
  static const uint8_t payload[] = "{}";
  assert(fixture.runtime.enqueueDevicePayload(
             "heartbeat", payload, sizeof(payload) - 1U, 1800000000LL) ==
         GatewayLanRuntimeResult::Ok);
  assert(fixture.runtime.poll(10U, 1800000000LL, true) ==
         GatewayLanRuntimeResult::Ok);
  assert(fixture.provider.acquires == 2U);
  assert(fixture.provider.releases == 2U);
  assert(fixture.runtime.poll(11U, 1800000000LL, true) ==
         GatewayLanRuntimeResult::Ok);
  assert(fixture.provider.acquires == 2U);
  assert(fixture.provider.releases == 2U);
  assert(!fixture.runtime.status().connected);
  assert(fixture.runtime.status().consecutiveFailures == 0U);
  assert(fixture.transport.closes == 0U);
  assert(fixture.runtime.poll(12U, 1800000000LL, true) ==
         GatewayLanRuntimeResult::Ok);
  assert(fixture.provider.acquires == 2U);
  assert(fixture.provider.releases == 2U);
  assert(fixture.transport.connects == 1U);
  assert(fixture.transport.connectPolls == 2U);
  assert(fixture.runtime.status().connected);
  assert(fixture.transport.writes.empty());
  assert(fixture.runtime.poll(13U, 1800000000LL, true) ==
         GatewayLanRuntimeResult::Ok);
  assert(fixture.transport.writes.empty());
  assert(fixture.runtime.poll(14U, 1800000000LL, true) ==
         GatewayLanRuntimeResult::Ok);
  assert(fixture.transport.writes.size() == 1U);
  assert(fixture.runtime.status().waitingForAck);
  assert(fixture.runtime.status().consecutiveFailures == 0U);
}

void testAuthenticatedMobileRelayHappyAndGuards() {
  Provider provider;
  Sequences sequences;
  WindowsHashes hashes;
  Replay replay;
  Sink sink;
  RelayConfigSink config;
  RelayEnrollment enrollment;
  KitsuMobileRelay relay;
  assert(relay.begin(provider, sequences, hashes, replay, sink, config,
                     enrollment));

  uint8_t response[companion::kMaximumEnvelopePayloadBytes]{};
  size_t responseBytes = 0U;
  MobileRelayExchangeOutcome outcome{};
  MobileRelayGuards guards{};
  const std::string enrollmentPull = relayPull("enrollment_pull");
  assert(relay.handleExchange(
      reinterpret_cast<const uint8_t*>(enrollmentPull.data()),
      enrollmentPull.size(), guards, 0, false, response, sizeof(response),
      responseBytes, &outcome));
  assert(outcome.result == MobileRelayResult::AuthorizationRequired);
  assert(std::string(reinterpret_cast<char*>(response), responseBytes).find(
             "authorization_required") != std::string::npos);

  guards.authenticatedController = true;
  assert(relay.handleExchange(
      reinterpret_cast<const uint8_t*>(enrollmentPull.data()),
      enrollmentPull.size(), guards, 0, false, response, sizeof(response),
      responseBytes, &outcome));
  assert(outcome.result ==
         MobileRelayResult::PhysicalConfirmationRequired);

  const std::string configure =
      "{\"schema\":\"kitsu.mobile-relay.exchange.v1\","
      "\"kind\":\"relay_configure\","
      "\"gateway_id\":\"f0e0d0c0-b0a0-9080-7060-504030201000\","
      "\"ca_cert_der_b64\":\"MA\"}";
  assert(relay.handleExchange(
      reinterpret_cast<const uint8_t*>(configure.data()), configure.size(),
      guards, 0, false, response, sizeof(response), responseBytes,
      &outcome));
  assert(outcome.gatewayConfigured && outcome.gatewayConfigurationChanged &&
         config.gateway.size() == 16U &&
         config.ca == std::vector<uint8_t>{0x30U});

  guards.enrollmentPrgConfirmed = true;
  guards.enrollmentActive = true;
  assert(relay.handleExchange(
      reinterpret_cast<const uint8_t*>(enrollmentPull.data()),
      enrollmentPull.size(), guards, 0, false, response, sizeof(response),
      responseBytes, &outcome));
  const std::string pullResponse(
      reinterpret_cast<char*>(response), responseBytes);
  assert(pullResponse.find("kitsu.mobile-relay.chunk.v1") !=
         std::string::npos);
  assert(decodedRelayChunk(pullResponse) == enrollment.request);

  const std::string issuerResponse = "{\"issued\":true}";
  const std::string enrollmentPush =
      relayPush("enrollment_push", issuerResponse);
  assert(relay.handleExchange(
      reinterpret_cast<const uint8_t*>(enrollmentPush.data()),
      enrollmentPush.size(), guards, 0, false, response, sizeof(response),
      responseBytes, &outcome));
  assert(outcome.enrollmentCompleted);
  assert(std::string(enrollment.installed.begin(),
                     enrollment.installed.end()) == issuerResponse);

  enrollment.installResult = MobileRelayResult::StorageAllocationFailed;
  assert(relay.handleExchange(
      reinterpret_cast<const uint8_t*>(enrollmentPush.data()),
      enrollmentPush.size(), guards, 0, false, response, sizeof(response),
      responseBytes, &outcome));
  assert(outcome.result == MobileRelayResult::StorageAllocationFailed);
  assert(std::string(reinterpret_cast<char*>(response), responseBytes).find(
             "\"error_code\":\"storage_allocation_failed\"") !=
         std::string::npos);
  enrollment.installResult = MobileRelayResult::Ok;

  const std::string malformed =
      "{\"schema\":\"kitsu.mobile-relay.exchange.v1\","
      "\"kind\":\"uplink_pull\",\"offset\":0,\"extra\":0}";
  assert(!relay.handleExchange(
      reinterpret_cast<const uint8_t*>(malformed.data()), malformed.size(),
      guards, 0, false, response, sizeof(response), responseBytes));
  const std::string oversize =
      "{\"schema\":\"kitsu.mobile-relay.exchange.v1\","
      "\"kind\":\"enrollment_push\",\"offset\":0,\"total\":32769,"
      "\"data_b64\":\"e30\",\"final\":false}";
  assert(!relay.handleExchange(
      reinterpret_cast<const uint8_t*>(oversize.data()), oversize.size(),
      guards, 0, false, response, sizeof(response), responseBytes));

  const std::string emptyUplink = relayPull("uplink_pull");
  assert(relay.handleExchange(
      reinterpret_cast<const uint8_t*>(emptyUplink.data()),
      emptyUplink.size(), guards, 0, false, response, sizeof(response),
      responseBytes));
  const std::string emptyUplinkResponse(
      reinterpret_cast<char*>(response), responseBytes);
  assert(emptyUplinkResponse.find("\"available\":false") !=
             std::string::npos &&
         emptyUplinkResponse.find("\"final\":false") !=
             std::string::npos);

  static const uint8_t snapshot[] = "{\"battery_pct\":87}";
  uint64_t sequence = 0U;
  assert(relay.enqueueDevicePayload(
             "companion.snapshot", snapshot, sizeof(snapshot) - 1U,
             1800000000LL, &sequence) == MobileRelayResult::Ok);
  assert(sequence == 1U && relay.status().uplinkPending);
  assert(relay.handleExchange(
      reinterpret_cast<const uint8_t*>(configure.data()), configure.size(),
      guards, 0, false, response, sizeof(response), responseBytes,
      &outcome));
  assert(outcome.gatewayConfigured && !outcome.gatewayConfigurationChanged &&
         relay.status().uplinkPending);
  const std::string uplinkPull = relayPull("uplink_pull");
  assert(relay.handleExchange(
      reinterpret_cast<const uint8_t*>(uplinkPull.data()), uplinkPull.size(),
      guards, 1800000030LL, true, response, sizeof(response), responseBytes));
  const std::string exactUplink = decodedRelayChunk(std::string(
      reinterpret_cast<char*>(response), responseBytes));
  assert(exactUplink.find("kitsu.device-envelope.v1") != std::string::npos);

  const std::string ackPush = relayPush("downlink_push", ack(sequence, 99U));
  assert(relay.handleExchange(
      reinterpret_cast<const uint8_t*>(ackPush.data()), ackPush.size(),
      guards, 1800000030LL, true, response, sizeof(response), responseBytes,
      &outcome));
  assert(outcome.downlinkCompleted && !relay.status().uplinkPending);

  const std::string exactAction = actionJson();
  const std::string actionPush =
      relayPush("downlink_push", exactAction);
  assert(relay.handleExchange(
      reinterpret_cast<const uint8_t*>(actionPush.data()), actionPush.size(),
      guards, 1800000030LL, true, response, sizeof(response), responseBytes,
      &outcome));
  assert(outcome.downlinkCompleted && sink.calls == 1U &&
         std::string(sink.frame.begin(), sink.frame.end()) == exactAction);

  static const uint8_t queued[] = "{\"queued\":true}";
  assert(relay.enqueueDevicePayload(
             "companion.snapshot", queued, sizeof(queued) - 1U,
             1800000031LL) == MobileRelayResult::Ok);
  assert(relay.enqueueDevicePayload(
             "action_result", queued, sizeof(queued) - 1U,
             1800000031LL) == MobileRelayResult::Ok);
  assert(relay.status().uplinkPending && relay.status().pendingPayloads == 1U);
  relay.clearGatewayState();
  const MobileRelayStatus cleared = relay.status();
  assert(cleared.begun && !cleared.uploadActive && !cleared.uplinkPending &&
         cleared.pendingPayloads == 0U);
}

}  // namespace

int main() {
  static_assert(kGatewayLanQueueDepth == 4U,
                "queue contract changed without test review");
  testConnectivityGateAndBoundedQueue();
  testEndpointHostAcceptsDnsIpv4AndIpv6ButSniRequiresDns();
  testAckAndExactRetryWithBackoff();
  testByteIdenticalActionAndPersistentRxCount();
  testTrustEvidenceIsMandatory();
  testConnectAndWriteWouldBlockAreProgressNotFailures();
  testAuthenticatedMobileRelayHappyAndGuards();
  std::cout << "Kitsu gateway LAN runtime tests passed.\n";
  return 0;
}
