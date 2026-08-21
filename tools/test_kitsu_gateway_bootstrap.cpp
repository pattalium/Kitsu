#include "../src/kitsu_gateway_bootstrap.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

using kitsu868::connectivity::EnrollmentResponse;
using kitsu868::connectivity::EnrollmentCredentialSink;
using kitsu868::connectivity::EnrollmentPlatformCrypto;
using kitsu868::connectivity::EnrollmentResult;
using kitsu868::connectivity::GatewayBootstrapIoResult;
using kitsu868::connectivity::GatewayBootstrapResult;
using kitsu868::connectivity::GatewayBootstrapTlsEvidence;
using kitsu868::connectivity::GatewayBootstrapTransport;
using kitsu868::connectivity::GatewayBootstrapTrust;
using kitsu868::connectivity::GatewayBootstrapWorkspace;
using kitsu868::connectivity::KitsuEnrollmentRecipient;
using kitsu868::connectivity::KitsuGatewayBootstrap;

bool fromHex(const char* input, uint8_t* output, size_t bytes) {
  if (!input || strlen(input) != bytes * 2U) return false;
  for (size_t i = 0U; i < bytes; ++i) {
    unsigned value = 0U;
    if (sscanf_s(input + i * 2U, "%2x", &value) != 1) return false;
    output[i] = static_cast<uint8_t>(value);
  }
  return true;
}

std::string b64(const uint8_t* input, size_t bytes) {
  std::vector<char> output(
      kitsu868::companion::base64UrlEncodedBytes(bytes) + 1U);
  size_t outputBytes = 0U;
  assert(kitsu868::companion::encodeBase64Url(
      input, bytes, output.data(), output.size(), outputBytes));
  return std::string(output.data(), outputBytes);
}

void fixtureIds(uint8_t enrollment[16], uint8_t companion[16],
                uint8_t gateway[16]) {
  assert(fromHex("00112233445566778899aabbccddeeff", enrollment, 16U));
  assert(fromHex("102132435465768798a9bacbdcedfe0f", companion, 16U));
  assert(fromHex("f0e0d0c0b0a090807060504030201000", gateway, 16U));
}

class FixtureHashes final : public kitsu868::companion::CompanionCrypto {
 public:
  bool randomBytes(uint8_t* output, size_t outputBytes) override {
    if (!output && outputBytes != 0U) return false;
    for (size_t i = 0U; i < outputBytes; ++i) {
      state_ = state_ * 1664525UL + 1013904223UL;
      output[i] = static_cast<uint8_t>(state_ >> 24U);
    }
    return true;
  }

  bool sha256(const kitsu868::companion::CryptoPart* parts,
              size_t partCount, uint8_t output[32]) override {
    return digest(nullptr, parts, partCount, output);
  }

  bool hmacSha256(
      const uint8_t key[32],
      const kitsu868::companion::CryptoPart* parts,
      size_t partCount, uint8_t output[32]) override {
    return digest(key, parts, partCount, output);
  }

  bool hkdfSha256(const uint8_t inputKey[32], const uint8_t* salt,
                  size_t saltBytes, const uint8_t* info, size_t infoBytes,
                  uint8_t output[32]) override {
    if (!inputKey || (!salt && saltBytes != 0U) ||
        (!info && infoBytes != 0U) || !output) {
      return false;
    }
    uint32_t value = mix(2166136261UL, inputKey, 32U);
    value = mix(value, salt, saltBytes);
    value = mix(value, info, infoBytes);
    for (size_t i = 0U; i < 32U; ++i) {
      value = value * 1664525UL + 1013904223UL;
      output[i] = static_cast<uint8_t>(value >> 24U);
    }
    return true;
  }

 private:
  static uint32_t mix(uint32_t value, const uint8_t* input, size_t bytes) {
    for (size_t i = 0U; i < bytes; ++i) {
      value ^= input[i];
      value *= 16777619UL;
    }
    return value;
  }

  static bool digest(const uint8_t* key,
                     const kitsu868::companion::CryptoPart* parts,
                     size_t partCount, uint8_t output[32]) {
    if ((!parts && partCount != 0U) || !output) return false;
    uint32_t value = 2166136261UL;
    if (key) value = mix(value, key, 32U);
    for (size_t i = 0U; i < partCount; ++i) {
      if (!parts[i].data && parts[i].bytes != 0U) return false;
      value = mix(value, parts[i].data, parts[i].bytes);
    }
    for (size_t i = 0U; i < 32U; ++i) {
      value = value * 1103515245UL + 12345UL;
      output[i] = static_cast<uint8_t>(value >> 24U);
    }
    return true;
  }

  uint32_t state_ = 0x12345678UL;
};

class FixtureEnrollmentPlatform final : public EnrollmentPlatformCrypto {
 public:
  bool generateP256KeyPair(uint8_t privateKey[32],
                           uint8_t publicKey[65]) override {
    ++generated_;
    memset(privateKey, static_cast<int>(0x10U + generated_), 32U);
    memset(publicKey, static_cast<int>(0x30U + generated_), 65U);
    publicKey[0] = 0x04U;
    return true;
  }

  bool createP256CsrDer(const uint8_t[32], const char*, size_t,
                        uint8_t* output, size_t outputCapacity,
                        size_t& outputBytes) override {
    static const uint8_t csr[] = {0x30U, 0x01U, 0x00U};
    if (!output || outputCapacity < sizeof(csr)) return false;
    memcpy(output, csr, sizeof(csr));
    outputBytes = sizeof(csr);
    return true;
  }

  bool signP256DigestP1363(const uint8_t[32], const uint8_t digest[32],
                           uint8_t signature[64]) override {
    memcpy(signature, digest, 32U);
    memcpy(signature + 32U, digest, 32U);
    return true;
  }

  bool p256Ecdh(const uint8_t[32], const uint8_t peerPublicKey[65],
                 uint8_t sharedSecret[32]) override {
    if (!peerPublicKey || peerPublicKey[0] != 0x04U) return false;
    memset(sharedSecret, 0x5a, 32U);
    return true;
  }

  bool aes256GcmOpen(const uint8_t[32], const uint8_t[12],
                     const uint8_t* aad, size_t aadBytes,
                     const uint8_t* ciphertext, size_t ciphertextBytes,
                     const uint8_t[16], uint8_t* plaintext) override {
    if (!aad || aadBytes == 0U || !ciphertext || ciphertextBytes != 32U ||
        !plaintext) {
      return false;
    }
    for (size_t i = 0U; i < ciphertextBytes; ++i) {
      plaintext[i] = static_cast<uint8_t>(0x80U + i);
    }
    return true;
  }

  bool certificateBindsKeyAndCompanion(const uint8_t* certificateDer,
                                        size_t certificateBytes,
                                        const uint8_t publicKey[65],
                                        const uint8_t companionUuid[16])
      override {
    return certificateDer && certificateBytes != 0U &&
           certificateDer[0] == 0x30U && publicKey &&
           publicKey[0] == 0x04U && companionUuid;
  }

 private:
  uint8_t generated_ = 0U;
};

class CapturingEnrollmentSink final : public EnrollmentCredentialSink {
 public:
  bool commitEnrollmentCredential(
      const uint8_t companionUuid[16], const uint8_t gatewayUuid[16],
      uint32_t keyVersion, const uint8_t privateKey[32],
      const uint8_t* certificateDer, size_t certificateBytes,
      const uint8_t* const* chainDer, const size_t* chainBytes,
      size_t chainCount, const uint8_t backendSecret[32]) override {
    if (!companionUuid || !gatewayUuid || keyVersion == 0U || !privateKey ||
        !certificateDer || certificateBytes == 0U || !chainDer ||
        !chainBytes || chainCount != 1U || !chainDer[0] ||
        chainBytes[0] == 0U || !backendSecret) {
      return false;
    }
    ++calls;
    version = keyVersion;
    memcpy(companion, companionUuid, sizeof(companion));
    memcpy(gateway, gatewayUuid, sizeof(gateway));
    memcpy(mtlsPrivate, privateKey, sizeof(mtlsPrivate));
    memcpy(secret, backendSecret, sizeof(secret));
    return accept;
  }

  bool accept = true;
  unsigned calls = 0U;
  uint32_t version = 0U;
  uint8_t companion[16]{};
  uint8_t gateway[16]{};
  uint8_t mtlsPrivate[32]{};
  uint8_t secret[32]{};
};

std::string innerResponse(const std::string& leaf,
                          const std::string& chain) {
  return std::string(
      "{\"companion_id\":\"10213243-5465-7687-98a9-bacbdcedfe0f\"," 
      "\"gateway_id\":\"f0e0d0c0-b0a0-9080-7060-504030201000\"," 
      "\"key_version\":1,\"device_certificate_der_b64\":\"") + leaf +
      "\",\"device_certificate_chain_der_b64\":[\"" + chain +
      "\"],\"sealed_secret\":{" 
      "\"suite\":\"DHKEM(P-256,HKDF-SHA256)/HKDF-SHA256/AES-256-GCM\"," 
      "\"enc_b64\":\"BMZVnUFt-1avcU8UbZF8JKv4GLL7EhYEEpZJhIIwotJYsqbYLcbGc0zwkv-qn8AS8Q9wCNOVKgjVeX6F_qul2Xc\"," 
      "\"ciphertext_b64\":\"Tg3SqAwA9XovN-ZG2uw_EuMMh4wlq3ZH-hSygkNjM-yWlSDm8FWsVBG77lmlIBdo\"}}";
}

class PollingBootstrapTransport final : public GatewayBootstrapTransport {
 public:
  explicit PollingBootstrapTransport(bool failForTime = false)
      : failForTime_(failForTime) {
    const uint8_t leaf[] = {0x30U, 0x01U, 0x00U};
    const uint8_t issuer[] = {0x30U, 0x01U, 0x01U};
    const std::string inner =
        innerResponse(b64(leaf, sizeof(leaf)), b64(issuer, sizeof(issuer)));
    response_ =
        "{\"v\":1,\"type\":\"device_enrollment_result\",\"ok\":true,"
        "\"enrollment_id\":\"00112233-4455-6677-8899-aabbccddeeff\","
        "\"response_b64\":\"" +
        b64(reinterpret_cast<const uint8_t*>(inner.data()), inner.size()) +
        "\"}";
  }

  GatewayBootstrapIoResult exchangeOneFramedRequest(
      const GatewayBootstrapTrust& trust, const char* alpn,
      const uint8_t* requestJson, size_t requestBytes,
      uint8_t* responseJson, size_t responseCapacity, size_t& responseBytes,
      GatewayBootstrapTlsEvidence& evidence) override {
    responseBytes = 0U;
    evidence = GatewayBootstrapTlsEvidence{};
    assert(trust.host && strcmp(trust.host, "192.0.2.10") == 0);
    assert(trust.serverName && strcmp(trust.serverName, "gateway.example") == 0);
    assert(trust.port == 7442U);
    assert(trust.caCertificateDer && trust.caCertificateBytes == 3U);
    assert(alpn && strcmp(alpn, "kitsu-bootstrap/1") == 0);
    assert(requestJson && requestBytes != 0U);
    assert(responseJson && responseCapacity >= response_.size());
    if (calls == 0U) {
      firstRequest_ = requestJson;
      firstRequestBytes_ = requestBytes;
      ++calls;
      return GatewayBootstrapIoResult::WouldBlock;
    }
    assert(requestJson == firstRequest_ && requestBytes == firstRequestBytes_);
    ++calls;
    evidence.systemTimeChecked = true;
    if (failForTime_) {
      evidence.systemTimeValid = false;
      return GatewayBootstrapIoResult::Failed;
    }
    evidence.serverChainVerified = true;
    evidence.serverNameVerified = true;
    evidence.spkiMatched = true;
    evidence.alpnMatched = true;
    evidence.tlsVersionAtLeast12 = true;
    evidence.systemTimeValid = true;
    evidence.plaintextFallbackUsed = false;
    evidence.redirectFollowed = false;
    evidence.clientCredentialPresented = false;
    memcpy(responseJson, response_.data(), response_.size());
    responseBytes = response_.size();
    return GatewayBootstrapIoResult::Ok;
  }

  void close() override { ++closeCalls; }

  unsigned calls = 0U;
  unsigned closeCalls = 0U;

 private:
  bool failForTime_ = false;
  const uint8_t* firstRequest_ = nullptr;
  size_t firstRequestBytes_ = 0U;
  std::string response_{};
};

GatewayBootstrapTrust fixtureTrust(const uint8_t gateway[16]) {
  static const uint8_t ca[] = {0x30U, 0x01U, 0x00U};
  GatewayBootstrapTrust trust{};
  trust.host = "192.0.2.10";
  trust.serverName = "gateway.example";
  trust.port = 7442U;
  trust.caCertificateDer = ca;
  trust.caCertificateBytes = sizeof(ca);
  memset(trust.spkiSha256, 0xa5, sizeof(trust.spkiSha256));
  memcpy(trust.gatewayUuid, gateway, sizeof(trust.gatewayUuid));
  return trust;
}

void beginRecipient(KitsuEnrollmentRecipient& recipient,
                    FixtureHashes& hashes,
                    FixtureEnrollmentPlatform& platform,
                    const uint8_t enrollment[16],
                    const uint8_t gateway[16]) {
  static const char hardwareUid[] = "KITSU868-TEST-0001";
  static const char claim[] = "one.use_claim-token";
  assert(recipient.begin(
             enrollment, gateway, hardwareUid, sizeof(hardwareUid) - 1U,
             claim, sizeof(claim) - 1U, true, true, true, hashes,
             platform) == EnrollmentResult::Ok);
  assert(recipient.active());
}

void testExactProxyWrapper() {
  uint8_t enrollment[16]{};
  uint8_t companion[16]{};
  uint8_t gateway[16]{};
  fixtureIds(enrollment, companion, gateway);
  static const uint8_t backend[] = "{\"claim_token\":\"opaque\"}";
  uint8_t wrapper[1024]{};
  size_t wrapperBytes = 0U;
  assert(kitsu868::connectivity::encodeGatewayBootstrapProxyRequest(
      enrollment, backend, sizeof(backend) - 1U, wrapper, sizeof(wrapper),
      wrapperBytes));
  const std::string expected =
      "{\"v\":1,\"type\":\"device_enrollment\"," 
      "\"enrollment_id\":\"00112233-4455-6677-8899-aabbccddeeff\"," 
      "\"request_b64\":\"" + b64(backend, sizeof(backend) - 1U) + "\"}";
  assert(std::string(reinterpret_cast<char*>(wrapper), wrapperBytes) ==
         expected);

  uint8_t decoded[256]{};
  size_t decodedBytes = 0U;
  const std::string success =
      "{\"v\":1,\"type\":\"device_enrollment_result\",\"ok\":true," 
      "\"enrollment_id\":\"00112233-4455-6677-8899-aabbccddeeff\"," 
      "\"response_b64\":\"" + b64(backend, sizeof(backend) - 1U) + "\"}";
  assert(kitsu868::connectivity::decodeGatewayBootstrapProxyResponse(
             reinterpret_cast<const uint8_t*>(success.data()),
             success.size(), enrollment, decoded, sizeof(decoded),
             decodedBytes) == GatewayBootstrapResult::ReconnectSteady);
  assert(decodedBytes == sizeof(backend) - 1U);
  assert(memcmp(decoded, backend, decodedBytes) == 0);

  // Production bootstrap reuses the 64 KiB proxy response allocation for the
  // smaller decoded backend JSON. Prove the supported forward-overlap shape.
  std::vector<uint8_t> inPlace(
      kitsu868::connectivity::kGatewayBootstrapMaximumProxyResponseBytes);
  memcpy(inPlace.data(), success.data(), success.size());
  decodedBytes = 0U;
  assert(kitsu868::connectivity::decodeGatewayBootstrapProxyResponse(
             inPlace.data(), success.size(), enrollment, inPlace.data(),
             inPlace.size(), decodedBytes) ==
         GatewayBootstrapResult::ReconnectSteady);
  assert(decodedBytes == sizeof(backend) - 1U);
  assert(memcmp(inPlace.data(), backend, decodedBytes) == 0);

  const std::string failure =
      "{\"v\":1,\"type\":\"device_enrollment_result\",\"ok\":false," 
      "\"error\":\"issuer_unavailable\"}";
  assert(kitsu868::connectivity::decodeGatewayBootstrapProxyResponse(
             reinterpret_cast<const uint8_t*>(failure.data()),
             failure.size(), enrollment, decoded, sizeof(decoded),
             decodedBytes) == GatewayBootstrapResult::ProxyRejected);

  const std::string duplicate =
      "{\"v\":1,\"v\":1,\"type\":\"device_enrollment_result\"," 
      "\"ok\":false,\"error\":\"generic\"}";
  assert(kitsu868::connectivity::decodeGatewayBootstrapProxyResponse(
             reinterpret_cast<const uint8_t*>(duplicate.data()),
             duplicate.size(), enrollment, decoded, sizeof(decoded),
             decodedBytes) == GatewayBootstrapResult::ProxyMalformed);
}

void testStrictInnerResponse() {
  uint8_t enrollment[16]{};
  uint8_t companion[16]{};
  uint8_t gateway[16]{};
  fixtureIds(enrollment, companion, gateway);
  const uint8_t leaf[] = {0x30U, 0x03U, 0x02U, 0x01U, 0x01U};
  const uint8_t chain[] = {0x30U, 0x03U, 0x02U, 0x01U, 0x02U};
  const std::string inner = innerResponse(b64(leaf, sizeof(leaf)),
                                          b64(chain, sizeof(chain)));
  static GatewayBootstrapWorkspace workspace{};
  EnrollmentResponse response{};
  assert(kitsu868::connectivity::decodeBackendEnrollmentResponse(
             reinterpret_cast<const uint8_t*>(inner.data()), inner.size(),
             enrollment, gateway, workspace, response) ==
         GatewayBootstrapResult::ReconnectSteady);
  assert(memcmp(response.enrollmentUuid, enrollment, 16U) == 0);
  assert(memcmp(response.companionUuid, companion, 16U) == 0);
  assert(memcmp(response.gatewayUuid, gateway, 16U) == 0);
  assert(response.keyVersion == 1U);
  assert(response.certificateBytes == sizeof(leaf));
  assert(memcmp(response.certificateDer, leaf, sizeof(leaf)) == 0);
  assert(response.certificateChainCount == 1U);
  assert(response.certificateChainBytes[0] == sizeof(chain));
  assert(memcmp(response.certificateChainDer[0], chain, sizeof(chain)) == 0);
  assert(response.encapsulatedKey[0] == 0x04U);

  uint8_t wrongGateway[16]{};
  memcpy(wrongGateway, gateway, sizeof(wrongGateway));
  wrongGateway[15] ^= 1U;
  assert(kitsu868::connectivity::decodeBackendEnrollmentResponse(
             reinterpret_cast<const uint8_t*>(inner.data()), inner.size(),
             enrollment, wrongGateway, workspace, response) ==
         GatewayBootstrapResult::BackendMalformed);

  std::string unknown = inner;
  unknown.insert(unknown.size() - 1U, ",\"algorithm\":\"forbidden\"");
  assert(kitsu868::connectivity::decodeBackendEnrollmentResponse(
             reinterpret_cast<const uint8_t*>(unknown.data()),
             unknown.size(), enrollment, gateway, workspace, response) ==
         GatewayBootstrapResult::BackendMalformed);

  std::string wrongSuite = inner;
  const std::string suite =
      "DHKEM(P-256,HKDF-SHA256)/HKDF-SHA256/AES-256-GCM";
  wrongSuite.replace(wrongSuite.find(suite), suite.size(), "insecure");
  assert(kitsu868::connectivity::decodeBackendEnrollmentResponse(
             reinterpret_cast<const uint8_t*>(wrongSuite.data()),
             wrongSuite.size(), enrollment, gateway, workspace, response) ==
         GatewayBootstrapResult::BackendMalformed);
}

void testFramingBound() {
  static uint8_t storage[64U * 1024U]{};
  kitsu868::connectivity::GatewayBootstrapFrameParser parser;
  assert(parser.begin(storage, sizeof(storage), 10000U));
  static const uint8_t json[] = "{\"v\":1}";
  uint8_t frame[sizeof(json) + 3U]{};
  const size_t payloadBytes = sizeof(json) - 1U;
  frame[0] = 0U;
  frame[1] = 0U;
  frame[2] = 0U;
  frame[3] = static_cast<uint8_t>(payloadBytes);
  memcpy(frame + 4U, json, payloadBytes);
  assert(parser.feed(frame, 2U, 1U) ==
         kitsu868::companion::FrameResult::NeedMore);
  assert(parser.feed(frame + 2U, sizeof(frame) - 2U, 2U) ==
         kitsu868::companion::FrameResult::Ready);
  const uint8_t* decoded = nullptr;
  size_t decodedBytes = 0U;
  assert(parser.frame(decoded, decodedBytes));
  assert(decodedBytes == payloadBytes);
  assert(memcmp(decoded, json, payloadBytes) == 0);

  parser.consume();
  const uint8_t oversize[] = {0x00U, 0x01U, 0x00U, 0x01U};
  assert(parser.feed(oversize, sizeof(oversize), 3U) ==
         kitsu868::companion::FrameResult::Oversize);

  parser.consume();
  assert(parser.feed(frame, 1U, 100U) ==
         kitsu868::companion::FrameResult::NeedMore);
  assert(parser.poll(10100U) ==
         kitsu868::companion::FrameResult::TimedOut);
}

void testNonblockingBootstrapOrchestration() {
  uint8_t enrollment[16]{};
  uint8_t companion[16]{};
  uint8_t gateway[16]{};
  fixtureIds(enrollment, companion, gateway);
  const GatewayBootstrapTrust trust = fixtureTrust(gateway);

  FixtureHashes hashes;
  FixtureEnrollmentPlatform platform;
  KitsuEnrollmentRecipient recipient;
  beginRecipient(recipient, hashes, platform, enrollment, gateway);
  CapturingEnrollmentSink sink;
  PollingBootstrapTransport transport;
  static GatewayBootstrapWorkspace workspace{};
  KitsuGatewayBootstrap bootstrap;
  assert(bootstrap.beginExchangeAndInstall(
             enrollment, true, trust, recipient, transport, sink,
             workspace) == GatewayBootstrapResult::InProgress);
  assert(bootstrap.active() && recipient.active() && sink.calls == 0U);
  assert(bootstrap.pollExchangeAndInstall() ==
         GatewayBootstrapResult::InProgress);
  assert(transport.calls == 1U && bootstrap.active() && recipient.active());
  assert(bootstrap.pollExchangeAndInstall() ==
         GatewayBootstrapResult::ReconnectSteady);
  assert(!bootstrap.active() && !recipient.active());
  assert(transport.calls == 2U && transport.closeCalls == 2U);
  assert(sink.calls == 1U && sink.version == 1U);
  assert(memcmp(sink.companion, companion, sizeof(companion)) == 0);
  assert(memcmp(sink.gateway, gateway, sizeof(gateway)) == 0);
  assert(sink.mtlsPrivate[0] == 0x11U && sink.secret[0] == 0x80U);

  // A provenance failure is terminal, maps specifically to time_unavailable,
  // aborts the staged recipient, and never commits a credential.
  FixtureHashes failedHashes;
  FixtureEnrollmentPlatform failedPlatform;
  KitsuEnrollmentRecipient failedRecipient;
  beginRecipient(failedRecipient, failedHashes, failedPlatform, enrollment,
                 gateway);
  CapturingEnrollmentSink failedSink;
  PollingBootstrapTransport timeFailure(true);
  assert(bootstrap.beginExchangeAndInstall(
             enrollment, true, trust, failedRecipient, timeFailure,
             failedSink, workspace) == GatewayBootstrapResult::InProgress);
  assert(bootstrap.pollExchangeAndInstall() ==
         GatewayBootstrapResult::InProgress);
  assert(bootstrap.pollExchangeAndInstall() ==
         GatewayBootstrapResult::TimeUnavailable);
  assert(!bootstrap.active() && !failedRecipient.active() &&
         failedSink.calls == 0U);

  // Cancellation is a bounded handoff too: it closes the transport and wipes
  // the enrollment attempt without waiting for a network deadline.
  FixtureHashes cancelledHashes;
  FixtureEnrollmentPlatform cancelledPlatform;
  KitsuEnrollmentRecipient cancelledRecipient;
  beginRecipient(cancelledRecipient, cancelledHashes, cancelledPlatform,
                 enrollment, gateway);
  CapturingEnrollmentSink cancelledSink;
  PollingBootstrapTransport cancelledTransport;
  assert(bootstrap.beginExchangeAndInstall(
             enrollment, true, trust, cancelledRecipient,
             cancelledTransport, cancelledSink, workspace) ==
         GatewayBootstrapResult::InProgress);
  bootstrap.cancel();
  assert(!bootstrap.active() && !cancelledRecipient.active() &&
         cancelledSink.calls == 0U && cancelledTransport.closeCalls == 2U);

  // The owner/runtime policy gate is checked before any request is emitted.
  FixtureHashes blockedHashes;
  FixtureEnrollmentPlatform blockedPlatform;
  KitsuEnrollmentRecipient blockedRecipient;
  beginRecipient(blockedRecipient, blockedHashes, blockedPlatform,
                 enrollment, gateway);
  CapturingEnrollmentSink blockedSink;
  PollingBootstrapTransport blockedTransport;
  assert(bootstrap.beginExchangeAndInstall(
             enrollment, false, trust, blockedRecipient, blockedTransport,
             blockedSink, workspace) ==
         GatewayBootstrapResult::RemoteConnectivityUnavailable);
  assert(!bootstrap.active() && !blockedRecipient.active() &&
         blockedTransport.calls == 0U && blockedSink.calls == 0U);
}

}  // namespace

int main() {
  static_assert(
      sizeof(GatewayBootstrapWorkspace) == 20U * 1024U,
      "bootstrap permanent workspace must remain exactly 20 KiB");
  static_assert(
      kitsu868::connectivity::kGatewayBootstrapMaximumTransientBytes ==
          80U * 1024U,
      "bootstrap request/response peak changed without memory review");
  testExactProxyWrapper();
  testStrictInnerResponse();
  testFramingBound();
  testNonblockingBootstrapOrchestration();
  std::cout << "Kitsu gateway bootstrap tests passed.\n";
  return 0;
}
