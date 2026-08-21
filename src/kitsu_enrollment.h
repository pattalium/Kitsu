#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_companion_protocol.h"

namespace kitsu868 {
namespace connectivity {

constexpr size_t kEnrollmentUuidBytes = 16U;
constexpr size_t kEnrollmentPrivateKeyBytes = 32U;
constexpr size_t kEnrollmentPublicKeyBytes = 65U;
constexpr size_t kEnrollmentSignatureBytes = 64U;
constexpr size_t kEnrollmentNonceBytes = 16U;
constexpr size_t kEnrollmentSecretBytes = 32U;
constexpr size_t kEnrollmentInfoBytes = 74U;
constexpr size_t kEnrollmentCiphertextBytes = 48U;
constexpr size_t kEnrollmentMaximumCsrBytes = 4096U;
constexpr size_t kEnrollmentMaximumCertificateBytes = 4096U;
constexpr size_t kEnrollmentMaximumChainCertificates = 4U;
constexpr size_t kEnrollmentMaximumClaimTokenBytes = 2048U;
constexpr size_t kEnrollmentMaximumHardwareUidBytes = 64U;
constexpr size_t kEnrollmentMaximumRequestBytes = 12000U;

enum class EnrollmentResult : uint8_t {
  Ok = 0,
  NotActive,
  InvalidArgument,
  AuthorizationRequired,
  RemoteConnectivityUnavailable,
  InvalidHardwareUid,
  ClaimTokenRejected,
  CryptoFailed,
  OutputTooSmall,
  ResponseMismatch,
  InvalidCertificate,
  DecryptionFailed,
  CommitFailed,
};

const char* enrollmentResultName(EnrollmentResult result);

// P-256 and AES-GCM operations stay in the platform crypto provider.  The
// protocol class below owns all RFC 9180 labels and byte serialization, which
// makes the wire-critical key schedule host-testable without exporting keys.
class EnrollmentPlatformCrypto {
 public:
  virtual ~EnrollmentPlatformCrypto() = default;

  virtual bool generateP256KeyPair(
      uint8_t privateKey[kEnrollmentPrivateKeyBytes],
      uint8_t publicKey[kEnrollmentPublicKeyBytes]) = 0;
  virtual bool createP256CsrDer(
      const uint8_t privateKey[kEnrollmentPrivateKeyBytes],
      const char* hardwareUid, size_t hardwareUidBytes, uint8_t* output,
      size_t outputCapacity, size_t& outputBytes) = 0;
  virtual bool signP256DigestP1363(
      const uint8_t privateKey[kEnrollmentPrivateKeyBytes],
      const uint8_t digest[32],
      uint8_t signature[kEnrollmentSignatureBytes]) = 0;
  virtual bool p256Ecdh(
      const uint8_t privateKey[kEnrollmentPrivateKeyBytes],
      const uint8_t peerPublicKey[kEnrollmentPublicKeyBytes],
      uint8_t sharedSecret[32]) = 0;
  virtual bool aes256GcmOpen(
      const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad,
      size_t aadBytes, const uint8_t* ciphertext, size_t ciphertextBytes,
      const uint8_t tag[16], uint8_t* plaintext) = 0;

  // This must check both the exact URI SAN and that the certificate public key
  // is the enrollment key.  Trust-chain validation remains mandatory when the
  // credential is installed into the TLS client.
  virtual bool certificateBindsKeyAndCompanion(
      const uint8_t* certificateDer, size_t certificateBytes,
      const uint8_t expectedPublicKey[kEnrollmentPublicKeyBytes],
      const uint8_t companionUuid[kEnrollmentUuidBytes]) = 0;
};

struct EnrollmentResponse {
  uint8_t enrollmentUuid[kEnrollmentUuidBytes]{};
  uint8_t companionUuid[kEnrollmentUuidBytes]{};
  uint8_t gatewayUuid[kEnrollmentUuidBytes]{};
  uint32_t keyVersion = 0U;
  const uint8_t* certificateDer = nullptr;
  size_t certificateBytes = 0U;
  const uint8_t* certificateChainDer[kEnrollmentMaximumChainCertificates]{};
  size_t certificateChainBytes[kEnrollmentMaximumChainCertificates]{};
  size_t certificateChainCount = 0U;
  uint8_t encapsulatedKey[kEnrollmentPublicKeyBytes]{};
  uint8_t ciphertext[kEnrollmentCiphertextBytes]{};
};

class EnrollmentCredentialSink {
 public:
  virtual ~EnrollmentCredentialSink() = default;

  // Implementations must durably commit all fields as one encrypted,
  // power-loss-safe generation before returning true.  Before committing,
  // they must validate the leaf plus supplied issuer chain against the
  // BLE-provisioned enrollment trust anchor; leaf key/SAN binding has already
  // been checked by KitsuEnrollmentRecipient.
  virtual bool commitEnrollmentCredential(
      const uint8_t companionUuid[kEnrollmentUuidBytes],
      const uint8_t gatewayUuid[kEnrollmentUuidBytes], uint32_t keyVersion,
      const uint8_t mtlsPrivateKey[kEnrollmentPrivateKeyBytes],
      const uint8_t* certificateDer, size_t certificateBytes,
      const uint8_t* const* certificateChainDer,
      const size_t* certificateChainBytes, size_t certificateChainCount,
      const uint8_t backendHmacSecret[kEnrollmentSecretBytes]) = 0;
};

// RFC 9180 Base mode recipient for DHKEM(P-256, HKDF-SHA256),
// HKDF-SHA256, AES-256-GCM (suite 0x0010/0x0001/0x0002).  info and AAD are
// the same exact 74-byte enrollment context.
EnrollmentResult openEnrollmentSecretHpke(
    const uint8_t recipientPrivateKey[kEnrollmentPrivateKeyBytes],
    const uint8_t recipientPublicKey[kEnrollmentPublicKeyBytes],
    const uint8_t encapsulatedKey[kEnrollmentPublicKeyBytes],
    const uint8_t context[kEnrollmentInfoBytes],
    const uint8_t ciphertext[kEnrollmentCiphertextBytes],
    companion::CompanionCrypto& hashes, EnrollmentPlatformCrypto& platform,
    uint8_t outputSecret[kEnrollmentSecretBytes]);

bool buildEnrollmentContext(
    const uint8_t enrollmentUuid[kEnrollmentUuidBytes],
    const uint8_t companionUuid[kEnrollmentUuidBytes],
    const uint8_t gatewayUuid[kEnrollmentUuidBytes], uint32_t keyVersion,
    uint8_t output[kEnrollmentInfoBytes]);

bool buildEnrollmentDeviceProofDigest(
    const uint8_t enrollmentUuid[kEnrollmentUuidBytes],
    const char* hardwareUid, size_t hardwareUidBytes,
    const uint8_t csrSha256[32],
    const uint8_t hpkeRecipient[kEnrollmentPublicKeyBytes],
    const uint8_t deviceNonce[kEnrollmentNonceBytes],
    companion::CompanionCrypto& hashes, uint8_t outputDigest[32]);

// Owns one enrollment attempt.  It creates a distinct long-lived mTLS P-256
// key and a one-use HPKE receiver key.  Neither private key is returned over a
// transport.  abort(), every failed finish(), and the destructor zero all
// pending secrets and the one-use claim token.
class KitsuEnrollmentRecipient {
 public:
  KitsuEnrollmentRecipient();
  ~KitsuEnrollmentRecipient();

  KitsuEnrollmentRecipient(const KitsuEnrollmentRecipient&) = delete;
  KitsuEnrollmentRecipient& operator=(const KitsuEnrollmentRecipient&) =
      delete;

  EnrollmentResult begin(
      const uint8_t enrollmentUuid[kEnrollmentUuidBytes],
      const uint8_t expectedGatewayUuid[kEnrollmentUuidBytes],
      const char* hardwareUid, size_t hardwareUidBytes,
      const char* claimToken, size_t claimTokenBytes,
      bool authenticatedBleController,
      bool physicalPrgConfirmed, bool remoteConnectivityAllowed,
      companion::CompanionCrypto& hashes,
      EnrollmentPlatformCrypto& platform);

  bool active() const;
  EnrollmentResult buildRequestJson(uint8_t* output, size_t outputCapacity,
                                    size_t& outputBytes) const;
  EnrollmentResult finish(const EnrollmentResponse& response,
                          EnrollmentCredentialSink& sink);
  void abort();

 private:
  EnrollmentResult failAndAbort(EnrollmentResult result);

  companion::CompanionCrypto* hashes_ = nullptr;
  EnrollmentPlatformCrypto* platform_ = nullptr;
  bool active_ = false;
  char hardwareUid_[kEnrollmentMaximumHardwareUidBytes + 1U]{};
  size_t hardwareUidBytes_ = 0U;
  char claimToken_[kEnrollmentMaximumClaimTokenBytes + 1U]{};
  size_t claimTokenBytes_ = 0U;
  uint8_t enrollmentUuid_[kEnrollmentUuidBytes]{};
  uint8_t expectedGatewayUuid_[kEnrollmentUuidBytes]{};
  uint8_t mtlsPrivateKey_[kEnrollmentPrivateKeyBytes]{};
  uint8_t mtlsPublicKey_[kEnrollmentPublicKeyBytes]{};
  uint8_t hpkePrivateKey_[kEnrollmentPrivateKeyBytes]{};
  uint8_t hpkePublicKey_[kEnrollmentPublicKeyBytes]{};
  uint8_t nonce_[kEnrollmentNonceBytes]{};
  uint8_t proof_[kEnrollmentSignatureBytes]{};
  uint8_t csr_[kEnrollmentMaximumCsrBytes]{};
  size_t csrBytes_ = 0U;
};

}  // namespace connectivity
}  // namespace kitsu868
