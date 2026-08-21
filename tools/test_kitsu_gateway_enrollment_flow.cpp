#include "../src/kitsu_gateway_enrollment_flow.h"

#include <assert.h>
#include <string.h>

#include <string>

using kitsu868::companion::CompanionCrypto;
using kitsu868::companion::CryptoPart;
using kitsu868::connectivity::EnrollmentPlatformCrypto;
using kitsu868::connectivity::GatewayEnrollmentError;
using kitsu868::connectivity::GatewayEnrollmentFlowState;
using kitsu868::connectivity::GatewayEnrollmentGuards;
using kitsu868::connectivity::GatewayEnrollmentReceipt;
using kitsu868::connectivity::KitsuEnrollmentRecipient;
using kitsu868::connectivity::KitsuGatewayEnrollmentFlow;

namespace {

constexpr char kEnrollmentId[] = "00112233-4455-6677-8899-aabbccddeeff";
constexpr char kOtherEnrollmentId[] =
    "10112233-4455-6677-8899-aabbccddeeff";
constexpr char kClaim[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

std::string beginJson(const char* id = kEnrollmentId,
                      const char* claim = kClaim) {
  return std::string("{\"schema\":\"kitsu.gateway-enrollment.begin.v1\"," 
                     "\"enrollment_id\":\"") + id +
      "\",\"claim_token\":\"" + claim + "\"}";
}

std::string finishJson(const char* id = kEnrollmentId) {
  return std::string("{\"schema\":\"kitsu.gateway-enrollment.finish.v1\"," 
                     "\"enrollment_id\":\"") + id + "\"}";
}

class TestHashes final : public CompanionCrypto {
 public:
  bool randomBytes(uint8_t* output, size_t outputBytes) override {
    if (!output && outputBytes != 0U) return false;
    for (size_t i = 0U; i < outputBytes; ++i) {
      output[i] = static_cast<uint8_t>(0x80U + i);
    }
    return true;
  }

  bool sha256(const CryptoPart* parts, size_t partCount,
              uint8_t output[32]) override {
    return digest(nullptr, parts, partCount, output);
  }

  bool hmacSha256(const uint8_t key[32], const CryptoPart* parts,
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
    CryptoPart parts[] = {CryptoPart(salt, saltBytes),
                          CryptoPart(info, infoBytes)};
    return digest(inputKey, parts, 2U, output);
  }

 private:
  static bool digest(const uint8_t* key, const CryptoPart* parts,
                     size_t partCount, uint8_t output[32]) {
    if ((!parts && partCount != 0U) || !output) return false;
    uint8_t accumulator = key ? key[0] : 0x5aU;
    for (size_t part = 0U; part < partCount; ++part) {
      if (!parts[part].data && parts[part].bytes != 0U) return false;
      for (size_t i = 0U; i < parts[part].bytes; ++i) {
        accumulator = static_cast<uint8_t>(
            (accumulator << 1U) ^ parts[part].data[i] ^ 0x3dU);
      }
    }
    for (size_t i = 0U; i < 32U; ++i) {
      output[i] = static_cast<uint8_t>(accumulator + i);
    }
    return true;
  }
};

class TestEnrollmentCrypto final : public EnrollmentPlatformCrypto {
 public:
  bool generateP256KeyPair(uint8_t privateKey[32],
                           uint8_t publicKey[65]) override {
    ++generated;
    memset(privateKey, generated, 32U);
    memset(publicKey, static_cast<int>(0x20U + generated), 65U);
    publicKey[0] = 0x04U;
    return allowCrypto;
  }

  bool createP256CsrDer(const uint8_t[32], const char*, size_t,
                        uint8_t* output, size_t capacity,
                        size_t& outputBytes) override {
    static const uint8_t csr[] = {0x30U, 0x03U, 0x01U, 0x01U, 0x00U};
    if (!allowCrypto || !output || capacity < sizeof(csr)) return false;
    memcpy(output, csr, sizeof(csr));
    outputBytes = sizeof(csr);
    return true;
  }

  bool signP256DigestP1363(const uint8_t[32], const uint8_t digest[32],
                           uint8_t signature[64]) override {
    if (!allowCrypto) return false;
    memcpy(signature, digest, 32U);
    memcpy(signature + 32U, digest, 32U);
    return true;
  }

  bool p256Ecdh(const uint8_t[32], const uint8_t[65],
                 uint8_t sharedSecret[32]) override {
    memset(sharedSecret, 0x44, 32U);
    return allowCrypto;
  }

  bool aes256GcmOpen(const uint8_t[32], const uint8_t[12],
                     const uint8_t*, size_t, const uint8_t*, size_t,
                     const uint8_t[16], uint8_t*) override {
    return false;
  }

  bool certificateBindsKeyAndCompanion(const uint8_t*, size_t,
                                        const uint8_t[65],
                                        const uint8_t[16]) override {
    return false;
  }

  bool allowCrypto = true;
  uint8_t generated = 0U;
};

GatewayEnrollmentGuards goodGuards() {
  GatewayEnrollmentGuards guards{};
  guards.authenticatedController = true;
  guards.storageReady = true;
  guards.gatewayConfigured = true;
  guards.trustedClock = true;
  guards.remoteConnectivityAllowed = true;
  return guards;
}

std::string encodeReceipt(const GatewayEnrollmentReceipt& receipt,
                          bool event = false) {
  uint8_t output[512]{};
  size_t outputBytes = 0U;
  const bool ok = event
      ? kitsu868::connectivity::encodeGatewayEnrollmentEvent(
            receipt, output, sizeof(output), outputBytes)
      : kitsu868::connectivity::encodeGatewayEnrollmentReceipt(
            receipt, output, sizeof(output), outputBytes);
  assert(ok);
  return std::string(reinterpret_cast<const char*>(output), outputBytes);
}

void testStrictCodecs() {
  uint8_t id[16]{};
  char token[44]{};
  const std::string valid = beginJson();
  assert(kitsu868::connectivity::decodeGatewayEnrollmentBegin(
      reinterpret_cast<const uint8_t*>(valid.data()), valid.size(), id,
      token));
  assert(strcmp(token, kClaim) == 0);

  const std::string reordered =
      std::string("{ \"claim_token\" : \"") + kClaim +
      "\", \"enrollment_id\" : \"" + kEnrollmentId +
      "\", \"schema\" : \"kitsu.gateway-enrollment.begin.v1\" }";
  assert(kitsu868::connectivity::decodeGatewayEnrollmentBegin(
      reinterpret_cast<const uint8_t*>(reordered.data()), reordered.size(),
      id, token));

  const std::string unknown = valid.substr(0U, valid.size() - 1U) +
      ",\"extra\":\"x\"}";
  assert(!kitsu868::connectivity::decodeGatewayEnrollmentBegin(
      reinterpret_cast<const uint8_t*>(unknown.data()), unknown.size(), id,
      token));
  const std::string duplicate =
      std::string("{\"schema\":\"kitsu.gateway-enrollment.begin.v1\"," 
                  "\"schema\":\"kitsu.gateway-enrollment.begin.v1\"," 
                  "\"enrollment_id\":\"") + kEnrollmentId +
      "\",\"claim_token\":\"" + kClaim + "\"}";
  assert(!kitsu868::connectivity::decodeGatewayEnrollmentBegin(
      reinterpret_cast<const uint8_t*>(duplicate.data()), duplicate.size(), id,
      token));
  const std::string uppercase = beginJson(
      "00112233-4455-6677-8899-AABBCCDDEEFF");
  assert(!kitsu868::connectivity::decodeGatewayEnrollmentBegin(
      reinterpret_cast<const uint8_t*>(uppercase.data()), uppercase.size(), id,
      token));
  const std::string padded = beginJson(
      kEnrollmentId, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
  assert(!kitsu868::connectivity::decodeGatewayEnrollmentBegin(
      reinterpret_cast<const uint8_t*>(padded.data()), padded.size(), id,
      token));
  const std::string escaped =
      "{\"schema\":\"kitsu.gateway-enrollment.finish.v1\"," 
      "\"enrollment_id\":\"00112233-4455-6677-8899-aabbccddee\\u0066\"}";
  assert(!kitsu868::connectivity::decodeGatewayEnrollmentFinish(
      reinterpret_cast<const uint8_t*>(escaped.data()), escaped.size(), id));
}

void testAuthorizationAndFinishFlow() {
  KitsuGatewayEnrollmentFlow flow;
  GatewayEnrollmentReceipt receipt{};
  GatewayEnrollmentGuards guards = goodGuards();
  const std::string begin = beginJson();
  flow.beginOperation(reinterpret_cast<const uint8_t*>(begin.data()),
                      begin.size(), guards, 1000U, receipt);
  assert(receipt.accepted);
  assert(receipt.state ==
         GatewayEnrollmentFlowState::PhysicalConfirmationRequired);
  assert(receipt.expiresInMs == 60000U);
  assert(receipt.error == GatewayEnrollmentError::None);
  const std::string beginReceipt = encodeReceipt(receipt);
  assert(beginReceipt ==
         std::string("{\"schema\":\"kitsu.gateway-enrollment.receipt.v1\"," 
                     "\"accepted\":true,\"state\":" 
                     "\"physical_confirmation_required\"," 
                     "\"enrollment_id\":\"") + kEnrollmentId +
             "\",\"expires_in_ms\":60000,\"error_code\":null}");
  assert(beginReceipt.find(kClaim) == std::string::npos);

  TestHashes hashes;
  TestEnrollmentCrypto crypto;
  KitsuEnrollmentRecipient recipient;
  uint8_t gateway[16]{};
  memset(gateway, 0x42, sizeof(gateway));
  static const char uid[] = "KTDEAD";
  const std::string finish = finishJson();
  flow.finishOperation(reinterpret_cast<const uint8_t*>(finish.data()),
                       finish.size(), guards, gateway, uid,
                       sizeof(uid) - 1U, 2000U, hashes, crypto, recipient,
                       receipt);
  assert(!receipt.accepted);
  assert(receipt.state ==
         GatewayEnrollmentFlowState::PhysicalConfirmationRequired);
  assert(receipt.error ==
         GatewayEnrollmentError::PhysicalConfirmationRequired);
  assert(!recipient.active());

  GatewayEnrollmentReceipt event{};
  assert(flow.confirmPhysical(2500U, event));
  assert(event.accepted &&
         event.state == GatewayEnrollmentFlowState::PhysicalConfirmed);
  const std::string eventJson = encodeReceipt(event, true);
  assert(eventJson.find("kitsu.gateway-enrollment.event.v1") !=
         std::string::npos);
  assert(eventJson.find(kClaim) == std::string::npos);

  flow.finishOperation(reinterpret_cast<const uint8_t*>(finish.data()),
                       finish.size(), guards, gateway, uid,
                       sizeof(uid) - 1U, 3000U, hashes, crypto, recipient,
                       receipt);
  assert(receipt.accepted);
  assert(receipt.state == GatewayEnrollmentFlowState::ReadyForWifi);
  assert(receipt.expiresInMs == 300000U);
  assert(recipient.active());
  assert(crypto.generated == 2U);
  assert(encodeReceipt(receipt).find(kClaim) == std::string::npos);

  // Finish retries are idempotent and never regenerate the CSR or HPKE key.
  flow.finishOperation(reinterpret_cast<const uint8_t*>(finish.data()),
                       finish.size(), guards, gateway, uid,
                       sizeof(uid) - 1U, 3500U, hashes, crypto, recipient,
                       receipt);
  assert(receipt.accepted &&
         receipt.state == GatewayEnrollmentFlowState::ReadyForWifi);
  assert(crypto.generated == 2U);

  // The intentional post-finish disconnect retains the recipient attempt.
  flow.onBleDisconnected();
  assert(recipient.active());
  assert(flow.markBootstrapping(4000U));
  flow.completeBootstrap(true);
  assert(!recipient.active());
  assert(flow.status(4001U).state == GatewayEnrollmentFlowState::Enrolled);
}

void testDisconnectTimeoutAndErrors() {
  GatewayEnrollmentGuards guards = goodGuards();
  const std::string begin = beginJson();
  const std::string finish = finishJson();
  GatewayEnrollmentReceipt receipt{};
  KitsuGatewayEnrollmentFlow flow;
  flow.beginOperation(reinterpret_cast<const uint8_t*>(begin.data()),
                      begin.size(), guards, 100U, receipt);
  assert(receipt.accepted);
  flow.onBleDisconnected();
  assert(flow.status(101U).state == GatewayEnrollmentFlowState::Idle);

  TestHashes hashes;
  TestEnrollmentCrypto crypto;
  KitsuEnrollmentRecipient recipient;
  uint8_t gateway[16]{};
  gateway[0] = 1U;
  flow.finishOperation(reinterpret_cast<const uint8_t*>(finish.data()),
                       finish.size(), guards, gateway, "KT0001", 6U, 102U,
                       hashes, crypto, recipient, receipt);
  assert(!receipt.accepted &&
         receipt.error == GatewayEnrollmentError::InvalidRequest);

  flow.beginOperation(reinterpret_cast<const uint8_t*>(begin.data()),
                      begin.size(), guards, 1000U, receipt);
  assert(flow.poll(61000U, &receipt));
  assert(receipt.state == GatewayEnrollmentFlowState::Expired);
  assert(receipt.error == GatewayEnrollmentError::Expired);
  assert(!receipt.hasEnrollmentId);
  assert(encodeReceipt(receipt, true) ==
         "{\"schema\":\"kitsu.gateway-enrollment.event.v1\"," 
         "\"accepted\":false,\"state\":\"expired\"," 
         "\"enrollment_id\":null,\"expires_in_ms\":0," 
         "\"error_code\":\"expired\"}");
  assert(!flow.status(61001U).hasEnrollmentId);

  struct GuardCase {
    GatewayEnrollmentGuards guards;
    GatewayEnrollmentError expected;
  } cases[6]{};
  cases[0] = {goodGuards(), GatewayEnrollmentError::InvalidRequest};
  cases[0].guards.authenticatedController = false;
  cases[1] = {goodGuards(), GatewayEnrollmentError::StorageFailed};
  cases[1].guards.storageReady = false;
  cases[2] = {goodGuards(), GatewayEnrollmentError::NotConfigured};
  cases[2].guards.gatewayConfigured = false;
  cases[3] = {goodGuards(), GatewayEnrollmentError::AlreadyEnrolled};
  cases[3].guards.alreadyEnrolled = true;
  cases[4] = {goodGuards(), GatewayEnrollmentError::TimeUnset};
  cases[4].guards.trustedClock = false;
  cases[5] = {goodGuards(), GatewayEnrollmentError::ConnectivityUnavailable};
  cases[5].guards.remoteConnectivityAllowed = false;
  for (const GuardCase& entry : cases) {
    KitsuGatewayEnrollmentFlow guarded;
    guarded.beginOperation(reinterpret_cast<const uint8_t*>(begin.data()),
                           begin.size(), entry.guards, 2000U, receipt);
    assert(!receipt.accepted && receipt.error == entry.expected);
  }

  KitsuGatewayEnrollmentFlow busy;
  busy.beginOperation(reinterpret_cast<const uint8_t*>(begin.data()),
                      begin.size(), guards, 3000U, receipt);
  const std::string other = beginJson(kOtherEnrollmentId);
  busy.beginOperation(reinterpret_cast<const uint8_t*>(other.data()),
                      other.size(), guards, 3001U, receipt);
  assert(!receipt.accepted && receipt.error == GatewayEnrollmentError::Busy);
}

}  // namespace

int main() {
  testStrictCodecs();
  testAuthorizationAndFinishFlow();
  testDisconnectTimeoutAndErrors();
  return 0;
}
