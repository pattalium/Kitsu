#include "kitsu_enrollment.h"

#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr uint8_t kKemSuiteId[] = {'K', 'E', 'M', 0x00U, 0x10U};
constexpr uint8_t kHpkeSuiteId[] = {
    'H', 'P', 'K', 'E', 0x00U, 0x10U, 0x00U, 0x01U, 0x00U, 0x02U};
constexpr uint8_t kHpkeVersionLabel[] = {'H', 'P', 'K', 'E', '-', 'v', '1'};
constexpr char kEnrollmentContextDomain[] =
    "KITSU-ENROLL-SECRET-1\0";
constexpr char kEnrollmentProofDomain[] =
    "KITSU-ENROLL-DEVICE-1\0";

static_assert(sizeof(kEnrollmentContextDomain) - 1U == 22U,
              "enrollment context domain must include one trailing NUL");
static_assert(sizeof(kEnrollmentProofDomain) - 1U == 22U,
              "enrollment proof domain must include one trailing NUL");

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

bool allZero(const uint8_t* input, size_t bytes) {
  if (!input) return true;
  uint8_t aggregate = 0U;
  for (size_t i = 0U; i < bytes; ++i) aggregate |= input[i];
  return aggregate == 0U;
}

void putU16Be(uint8_t output[2], uint16_t value) {
  output[0] = static_cast<uint8_t>(value >> 8U);
  output[1] = static_cast<uint8_t>(value);
}

void putU32Be(uint8_t output[4], uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24U);
  output[1] = static_cast<uint8_t>(value >> 16U);
  output[2] = static_cast<uint8_t>(value >> 8U);
  output[3] = static_cast<uint8_t>(value);
}

bool hmac(companion::CompanionCrypto& hashes, const uint8_t key[32],
          const companion::CryptoPart* parts, size_t partCount,
          uint8_t output[32]) {
  if (partCount > companion::kMaximumCryptoParts) return false;
  return hashes.hmacSha256(key, parts, partCount, output);
}

bool labeledExtract(companion::CompanionCrypto& hashes,
                    const uint8_t salt[32], const uint8_t* suite,
                    size_t suiteBytes, const char* label,
                    const uint8_t* input, size_t inputBytes,
                    uint8_t output[32]) {
  if (!salt || !suite || !label || (!input && inputBytes != 0U) || !output) {
    return false;
  }
  const companion::CryptoPart parts[] = {
      companion::CryptoPart(kHpkeVersionLabel, sizeof(kHpkeVersionLabel)),
      companion::CryptoPart(suite, suiteBytes),
      companion::CryptoPart(reinterpret_cast<const uint8_t*>(label),
                            strlen(label)),
      companion::CryptoPart(input, inputBytes),
  };
  return hmac(hashes, salt, parts, sizeof(parts) / sizeof(parts[0]), output);
}

bool labeledExpand(companion::CompanionCrypto& hashes,
                   const uint8_t prk[32], const uint8_t* suite,
                   size_t suiteBytes, const char* label,
                   const uint8_t* info, size_t infoBytes, uint8_t* output,
                   size_t outputBytes) {
  if (!prk || !suite || !label || (!info && infoBytes != 0U) || !output ||
      outputBytes == 0U || outputBytes > 32U) {
    return false;
  }
  uint8_t length[2]{};
  putU16Be(length, static_cast<uint16_t>(outputBytes));
  const uint8_t counter = 1U;
  const companion::CryptoPart parts[] = {
      companion::CryptoPart(length, sizeof(length)),
      companion::CryptoPart(kHpkeVersionLabel, sizeof(kHpkeVersionLabel)),
      companion::CryptoPart(suite, suiteBytes),
      companion::CryptoPart(reinterpret_cast<const uint8_t*>(label),
                            strlen(label)),
      companion::CryptoPart(info, infoBytes),
      companion::CryptoPart(&counter, 1U),
  };
  uint8_t block[32]{};
  const bool ok = hmac(hashes, prk, parts,
                       sizeof(parts) / sizeof(parts[0]), block);
  if (ok) memcpy(output, block, outputBytes);
  secureZero(block, sizeof(block));
  return ok;
}

bool validHardwareUid(const char* uid, size_t uidBytes) {
  if (!uid || uidBytes == 0U ||
      uidBytes > kEnrollmentMaximumHardwareUidBytes) {
    return false;
  }
  for (size_t i = 0U; i < uidBytes; ++i) {
    const char value = uid[i];
    const bool alphanumeric =
        (value >= '0' && value <= '9') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z');
    if (!alphanumeric && value != '-' && value != '_' && value != '.' &&
        value != ':') {
      return false;
    }
  }
  return true;
}

class JsonWriter {
 public:
  JsonWriter(uint8_t* output, size_t capacity)
      : output_(output), capacity_(capacity) {}

  bool literal(const char* value) {
    return value && bytes(reinterpret_cast<const uint8_t*>(value),
                          strlen(value));
  }

  bool bytes(const uint8_t* value, size_t valueBytes) {
    if ((!value && valueBytes != 0U) || valueBytes > capacity_ - used_) {
      ok_ = false;
      return false;
    }
    if (valueBytes != 0U) memcpy(output_ + used_, value, valueBytes);
    used_ += valueBytes;
    return true;
  }

  bool string(const char* value, size_t valueBytes) {
    if (!value || !literal("\"")) return false;
    for (size_t i = 0U; i < valueBytes; ++i) {
      const uint8_t byte = static_cast<uint8_t>(value[i]);
      if (byte == '"' || byte == '\\') {
        const uint8_t escaped[] = {'\\', byte};
        if (!bytes(escaped, sizeof(escaped))) return false;
      } else if (byte < 0x20U) {
        static const char hex[] = "0123456789abcdef";
        const uint8_t escaped[] = {
            '\\', 'u', '0', '0',
            static_cast<uint8_t>(hex[byte >> 4U]),
            static_cast<uint8_t>(hex[byte & 0x0fU])};
        if (!bytes(escaped, sizeof(escaped))) return false;
      } else if (!bytes(&byte, 1U)) {
        return false;
      }
    }
    return literal("\"");
  }

  bool base64(const uint8_t* value, size_t valueBytes) {
    if (!value || !literal("\"")) return false;
    size_t encodedBytes = 0U;
    if (!companion::encodeBase64Url(
            value, valueBytes, reinterpret_cast<char*>(output_ + used_),
            capacity_ - used_, encodedBytes)) {
      ok_ = false;
      return false;
    }
    used_ += encodedBytes;
    return literal("\"");
  }

  bool ok() const { return ok_; }
  size_t used() const { return used_; }

 private:
  uint8_t* output_ = nullptr;
  size_t capacity_ = 0U;
  size_t used_ = 0U;
  bool ok_ = true;
};

}  // namespace

const char* enrollmentResultName(EnrollmentResult result) {
  switch (result) {
    case EnrollmentResult::Ok: return "ok";
    case EnrollmentResult::NotActive: return "not_active";
    case EnrollmentResult::InvalidArgument: return "invalid_argument";
    case EnrollmentResult::AuthorizationRequired:
      return "authorization_required";
    case EnrollmentResult::RemoteConnectivityUnavailable:
      return "remote_connectivity_unavailable";
    case EnrollmentResult::InvalidHardwareUid:
      return "invalid_hardware_uid";
    case EnrollmentResult::ClaimTokenRejected:
      return "claim_token_rejected";
    case EnrollmentResult::CryptoFailed: return "crypto_failed";
    case EnrollmentResult::OutputTooSmall: return "output_too_small";
    case EnrollmentResult::ResponseMismatch: return "response_mismatch";
    case EnrollmentResult::InvalidCertificate: return "invalid_certificate";
    case EnrollmentResult::DecryptionFailed: return "decryption_failed";
    case EnrollmentResult::CommitFailed: return "commit_failed";
    default: return "unknown";
  }
}

bool buildEnrollmentContext(
    const uint8_t enrollmentUuid[kEnrollmentUuidBytes],
    const uint8_t companionUuid[kEnrollmentUuidBytes],
    const uint8_t gatewayUuid[kEnrollmentUuidBytes], uint32_t keyVersion,
    uint8_t output[kEnrollmentInfoBytes]) {
  if (!enrollmentUuid || !companionUuid || !gatewayUuid || !output ||
      keyVersion == 0U || allZero(enrollmentUuid, kEnrollmentUuidBytes) ||
      allZero(companionUuid, kEnrollmentUuidBytes) ||
      allZero(gatewayUuid, kEnrollmentUuidBytes)) {
    return false;
  }
  size_t cursor = 0U;
  memcpy(output + cursor, kEnrollmentContextDomain,
         sizeof(kEnrollmentContextDomain) - 1U);
  cursor += sizeof(kEnrollmentContextDomain) - 1U;
  memcpy(output + cursor, enrollmentUuid, kEnrollmentUuidBytes);
  cursor += kEnrollmentUuidBytes;
  memcpy(output + cursor, companionUuid, kEnrollmentUuidBytes);
  cursor += kEnrollmentUuidBytes;
  memcpy(output + cursor, gatewayUuid, kEnrollmentUuidBytes);
  cursor += kEnrollmentUuidBytes;
  putU32Be(output + cursor, keyVersion);
  cursor += 4U;
  return cursor == kEnrollmentInfoBytes;
}

bool buildEnrollmentDeviceProofDigest(
    const uint8_t enrollmentUuid[kEnrollmentUuidBytes],
    const char* hardwareUid, size_t hardwareUidBytes,
    const uint8_t csrSha256[32],
    const uint8_t hpkeRecipient[kEnrollmentPublicKeyBytes],
    const uint8_t deviceNonce[kEnrollmentNonceBytes],
    companion::CompanionCrypto& hashes, uint8_t outputDigest[32]) {
  if (!enrollmentUuid || !validHardwareUid(hardwareUid, hardwareUidBytes) ||
      !csrSha256 || !hpkeRecipient || !deviceNonce || !outputDigest ||
      hardwareUidBytes > 0xffffU || hpkeRecipient[0] != 0x04U) {
    return false;
  }
  uint8_t uidLength[2]{};
  putU16Be(uidLength, static_cast<uint16_t>(hardwareUidBytes));
  const companion::CryptoPart proofParts[] = {
      companion::CryptoPart(
          reinterpret_cast<const uint8_t*>(kEnrollmentProofDomain),
          sizeof(kEnrollmentProofDomain) - 1U),
      companion::CryptoPart(enrollmentUuid, kEnrollmentUuidBytes),
      companion::CryptoPart(uidLength, sizeof(uidLength)),
      companion::CryptoPart(reinterpret_cast<const uint8_t*>(hardwareUid),
                            hardwareUidBytes),
      companion::CryptoPart(csrSha256, 32U),
      companion::CryptoPart(hpkeRecipient, kEnrollmentPublicKeyBytes),
      companion::CryptoPart(deviceNonce, kEnrollmentNonceBytes),
  };
  return hashes.sha256(proofParts,
                       sizeof(proofParts) / sizeof(proofParts[0]),
                       outputDigest);
}

EnrollmentResult openEnrollmentSecretHpke(
    const uint8_t recipientPrivateKey[kEnrollmentPrivateKeyBytes],
    const uint8_t recipientPublicKey[kEnrollmentPublicKeyBytes],
    const uint8_t encapsulatedKey[kEnrollmentPublicKeyBytes],
    const uint8_t context[kEnrollmentInfoBytes],
    const uint8_t ciphertext[kEnrollmentCiphertextBytes],
    companion::CompanionCrypto& hashes, EnrollmentPlatformCrypto& platform,
    uint8_t outputSecret[kEnrollmentSecretBytes]) {
  if (!recipientPrivateKey || !recipientPublicKey || !encapsulatedKey ||
      !context || !ciphertext || !outputSecret ||
      recipientPublicKey[0] != 0x04U || encapsulatedKey[0] != 0x04U ||
      allZero(recipientPrivateKey, kEnrollmentPrivateKeyBytes)) {
    return EnrollmentResult::InvalidArgument;
  }
  memset(outputSecret, 0, kEnrollmentSecretBytes);
  uint8_t zeros[32]{};
  uint8_t dh[32]{};
  uint8_t eaePrk[32]{};
  uint8_t sharedSecret[32]{};
  uint8_t pskIdHash[32]{};
  uint8_t infoHash[32]{};
  uint8_t secret[32]{};
  uint8_t key[32]{};
  uint8_t baseNonce[12]{};
  uint8_t kemContext[kEnrollmentPublicKeyBytes * 2U]{};
  uint8_t keyScheduleContext[65]{};

  bool ok = platform.p256Ecdh(recipientPrivateKey, encapsulatedKey, dh) &&
            !allZero(dh, sizeof(dh));
  memcpy(kemContext, encapsulatedKey, kEnrollmentPublicKeyBytes);
  memcpy(kemContext + kEnrollmentPublicKeyBytes, recipientPublicKey,
         kEnrollmentPublicKeyBytes);
  if (ok) {
    ok = labeledExtract(hashes, zeros, kKemSuiteId, sizeof(kKemSuiteId),
                        "eae_prk", dh, sizeof(dh), eaePrk) &&
         labeledExpand(hashes, eaePrk, kKemSuiteId, sizeof(kKemSuiteId),
                       "shared_secret", kemContext, sizeof(kemContext),
                       sharedSecret, sizeof(sharedSecret));
  }
  if (ok) {
    ok = labeledExtract(hashes, zeros, kHpkeSuiteId, sizeof(kHpkeSuiteId),
                        "psk_id_hash", nullptr, 0U, pskIdHash) &&
         labeledExtract(hashes, zeros, kHpkeSuiteId, sizeof(kHpkeSuiteId),
                        "info_hash", context, kEnrollmentInfoBytes,
                        infoHash);
  }
  if (ok) {
    keyScheduleContext[0] = 0x00U;  // Base mode.
    memcpy(keyScheduleContext + 1U, pskIdHash, sizeof(pskIdHash));
    memcpy(keyScheduleContext + 1U + sizeof(pskIdHash), infoHash,
           sizeof(infoHash));
    ok = labeledExtract(hashes, sharedSecret, kHpkeSuiteId,
                        sizeof(kHpkeSuiteId), "secret", nullptr, 0U,
                        secret) &&
         labeledExpand(hashes, secret, kHpkeSuiteId, sizeof(kHpkeSuiteId),
                       "key", keyScheduleContext,
                       sizeof(keyScheduleContext), key, sizeof(key)) &&
         labeledExpand(hashes, secret, kHpkeSuiteId, sizeof(kHpkeSuiteId),
                       "base_nonce", keyScheduleContext,
                       sizeof(keyScheduleContext), baseNonce,
                       sizeof(baseNonce));
  }
  if (ok) {
    ok = platform.aes256GcmOpen(
        key, baseNonce, context, kEnrollmentInfoBytes, ciphertext,
        kEnrollmentSecretBytes, ciphertext + kEnrollmentSecretBytes,
        outputSecret);
  }

  secureZero(dh, sizeof(dh));
  secureZero(eaePrk, sizeof(eaePrk));
  secureZero(sharedSecret, sizeof(sharedSecret));
  secureZero(pskIdHash, sizeof(pskIdHash));
  secureZero(infoHash, sizeof(infoHash));
  secureZero(secret, sizeof(secret));
  secureZero(key, sizeof(key));
  secureZero(baseNonce, sizeof(baseNonce));
  secureZero(kemContext, sizeof(kemContext));
  secureZero(keyScheduleContext, sizeof(keyScheduleContext));
  if (!ok) {
    secureZero(outputSecret, kEnrollmentSecretBytes);
    return EnrollmentResult::DecryptionFailed;
  }
  return EnrollmentResult::Ok;
}

KitsuEnrollmentRecipient::KitsuEnrollmentRecipient() = default;

KitsuEnrollmentRecipient::~KitsuEnrollmentRecipient() { abort(); }

EnrollmentResult KitsuEnrollmentRecipient::begin(
    const uint8_t enrollmentUuid[kEnrollmentUuidBytes],
    const uint8_t expectedGatewayUuid[kEnrollmentUuidBytes],
    const char* hardwareUid, size_t hardwareUidBytes,
    const char* claimToken, size_t claimTokenBytes,
    bool authenticatedBleController,
    bool physicalPrgConfirmed, bool remoteConnectivityAllowed,
    companion::CompanionCrypto& hashes, EnrollmentPlatformCrypto& platform) {
  abort();
  if (!enrollmentUuid || !expectedGatewayUuid || !hardwareUid ||
      !claimToken) {
    return EnrollmentResult::InvalidArgument;
  }
  if (!authenticatedBleController || !physicalPrgConfirmed) {
    return EnrollmentResult::AuthorizationRequired;
  }
  if (!remoteConnectivityAllowed) {
    return EnrollmentResult::RemoteConnectivityUnavailable;
  }
  if (!validHardwareUid(hardwareUid, hardwareUidBytes)) {
    return EnrollmentResult::InvalidHardwareUid;
  }
  if (claimTokenBytes == 0U ||
      claimTokenBytes > kEnrollmentMaximumClaimTokenBytes ||
      !companion::validUtf8(reinterpret_cast<const uint8_t*>(claimToken),
                            claimTokenBytes)) {
    return EnrollmentResult::ClaimTokenRejected;
  }
  if (allZero(enrollmentUuid, kEnrollmentUuidBytes) ||
      allZero(expectedGatewayUuid, kEnrollmentUuidBytes)) {
    return EnrollmentResult::InvalidArgument;
  }

  hashes_ = &hashes;
  platform_ = &platform;
  memcpy(enrollmentUuid_, enrollmentUuid, sizeof(enrollmentUuid_));
  memcpy(expectedGatewayUuid_, expectedGatewayUuid,
         sizeof(expectedGatewayUuid_));
  memcpy(hardwareUid_, hardwareUid, hardwareUidBytes);
  hardwareUid_[hardwareUidBytes] = '\0';
  hardwareUidBytes_ = hardwareUidBytes;
  memcpy(claimToken_, claimToken, claimTokenBytes);
  claimToken_[claimTokenBytes] = '\0';
  claimTokenBytes_ = claimTokenBytes;

  if (!platform_->generateP256KeyPair(mtlsPrivateKey_, mtlsPublicKey_) ||
      !platform_->generateP256KeyPair(hpkePrivateKey_, hpkePublicKey_) ||
      !hashes_->randomBytes(nonce_, sizeof(nonce_)) ||
      !platform_->createP256CsrDer(mtlsPrivateKey_, hardwareUid_,
                                   hardwareUidBytes_, csr_, sizeof(csr_),
                                   csrBytes_) ||
      csrBytes_ == 0U || csrBytes_ > sizeof(csr_)) {
    return failAndAbort(EnrollmentResult::CryptoFailed);
  }

  uint8_t csrDigest[32]{};
  const companion::CryptoPart digestParts[] = {
      companion::CryptoPart(csr_, csrBytes_)};
  uint8_t proofDigest[32]{};
  const bool proofOk =
      hashes_->sha256(digestParts, 1U, csrDigest) &&
      buildEnrollmentDeviceProofDigest(
          enrollmentUuid_, hardwareUid_, hardwareUidBytes_, csrDigest,
          hpkePublicKey_, nonce_, *hashes_, proofDigest) &&
      platform_->signP256DigestP1363(mtlsPrivateKey_, proofDigest, proof_);
  secureZero(csrDigest, sizeof(csrDigest));
  secureZero(proofDigest, sizeof(proofDigest));
  if (!proofOk) return failAndAbort(EnrollmentResult::CryptoFailed);

  active_ = true;
  return EnrollmentResult::Ok;
}

bool KitsuEnrollmentRecipient::active() const { return active_; }

EnrollmentResult KitsuEnrollmentRecipient::buildRequestJson(
    uint8_t* output, size_t outputCapacity, size_t& outputBytes) const {
  outputBytes = 0U;
  if (!active_) return EnrollmentResult::NotActive;
  if (!output || outputCapacity == 0U) {
    return EnrollmentResult::InvalidArgument;
  }
  const size_t boundedCapacity = outputCapacity < kEnrollmentMaximumRequestBytes
      ? outputCapacity
      : kEnrollmentMaximumRequestBytes;
  JsonWriter writer(output, boundedCapacity);
  writer.literal("{\"claim_token\":");
  writer.string(claimToken_, claimTokenBytes_);
  writer.literal(",\"hardware_uid\":");
  writer.string(hardwareUid_, hardwareUidBytes_);
  writer.literal(",\"device_csr_der_b64\":");
  writer.base64(csr_, csrBytes_);
  writer.literal(",\"hpke_recipient_b64\":");
  writer.base64(hpkePublicKey_, sizeof(hpkePublicKey_));
  writer.literal(",\"device_nonce_b64\":");
  writer.base64(nonce_, sizeof(nonce_));
  writer.literal(",\"device_proof_b64\":");
  writer.base64(proof_, sizeof(proof_));
  writer.literal("}");
  if (!writer.ok()) return EnrollmentResult::OutputTooSmall;
  outputBytes = writer.used();
  return EnrollmentResult::Ok;
}

EnrollmentResult KitsuEnrollmentRecipient::finish(
    const EnrollmentResponse& response, EnrollmentCredentialSink& sink) {
  if (!active_) return EnrollmentResult::NotActive;
  if (!response.certificateDer || response.certificateBytes == 0U ||
      response.certificateBytes > kEnrollmentMaximumCertificateBytes ||
      response.keyVersion == 0U ||
      memcmp(response.enrollmentUuid, enrollmentUuid_,
             kEnrollmentUuidBytes) != 0 ||
      memcmp(response.gatewayUuid, expectedGatewayUuid_,
             kEnrollmentUuidBytes) != 0 ||
      allZero(response.companionUuid, kEnrollmentUuidBytes)) {
    return failAndAbort(EnrollmentResult::ResponseMismatch);
  }
  if (!platform_->certificateBindsKeyAndCompanion(
          response.certificateDer, response.certificateBytes,
          mtlsPublicKey_, response.companionUuid)) {
    return failAndAbort(EnrollmentResult::InvalidCertificate);
  }

  uint8_t context[kEnrollmentInfoBytes]{};
  uint8_t backendSecret[kEnrollmentSecretBytes]{};
  if (!buildEnrollmentContext(response.enrollmentUuid,
                              response.companionUuid,
                              response.gatewayUuid, response.keyVersion,
                              context)) {
    return failAndAbort(EnrollmentResult::ResponseMismatch);
  }
  const EnrollmentResult opened = openEnrollmentSecretHpke(
      hpkePrivateKey_, hpkePublicKey_, response.encapsulatedKey, context,
      response.ciphertext, *hashes_, *platform_, backendSecret);
  secureZero(context, sizeof(context));
  if (opened != EnrollmentResult::Ok) {
    secureZero(backendSecret, sizeof(backendSecret));
    return failAndAbort(opened);
  }

  const bool committed = sink.commitEnrollmentCredential(
      response.companionUuid, response.gatewayUuid, response.keyVersion,
      mtlsPrivateKey_, response.certificateDer, response.certificateBytes,
      response.certificateChainDer, response.certificateChainBytes,
      response.certificateChainCount,
      backendSecret);
  secureZero(backendSecret, sizeof(backendSecret));
  if (!committed) return failAndAbort(EnrollmentResult::CommitFailed);
  abort();
  return EnrollmentResult::Ok;
}

EnrollmentResult KitsuEnrollmentRecipient::failAndAbort(
    EnrollmentResult result) {
  abort();
  return result;
}

void KitsuEnrollmentRecipient::abort() {
  hashes_ = nullptr;
  platform_ = nullptr;
  active_ = false;
  secureZero(hardwareUid_, sizeof(hardwareUid_));
  hardwareUidBytes_ = 0U;
  secureZero(claimToken_, sizeof(claimToken_));
  claimTokenBytes_ = 0U;
  secureZero(enrollmentUuid_, sizeof(enrollmentUuid_));
  secureZero(expectedGatewayUuid_, sizeof(expectedGatewayUuid_));
  secureZero(mtlsPrivateKey_, sizeof(mtlsPrivateKey_));
  secureZero(mtlsPublicKey_, sizeof(mtlsPublicKey_));
  secureZero(hpkePrivateKey_, sizeof(hpkePrivateKey_));
  secureZero(hpkePublicKey_, sizeof(hpkePublicKey_));
  secureZero(nonce_, sizeof(nonce_));
  secureZero(proof_, sizeof(proof_));
  secureZero(csr_, sizeof(csr_));
  csrBytes_ = 0U;
}

}  // namespace connectivity
}  // namespace kitsu868
