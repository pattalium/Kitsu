#pragma once

#if defined(ARDUINO_ARCH_ESP32)

#include <Preferences.h>

#include "kitsu_companion_protocol.h"
#include "kitsu_device_security.h"
#include "kitsu_enrollment.h"
#include "mesh_discovery_journal.h"

namespace kitsu868 {
namespace connectivity {

// These adapters never emit key or credential bytes to Serial.  The small NVS
// blobs are application-encrypted before Preferences sees them. In the
// owner-selected reflashable mode, the wrapping root is deliberately
// recoverable from a complete flash image and device identity.
class Esp32DeviceSecurityStorage final : public DeviceSecurityStorage {
 public:
  ~Esp32DeviceSecurityStorage() override;
  bool begin(const char* partitionLabel = nullptr);
  void end();
  bool readSlot(uint8_t slot, uint8_t* output, size_t capacity,
                size_t& outputBytes) override;
  bool writeSlot(uint8_t slot, const uint8_t* input,
                 size_t inputBytes) override;

 private:
  Preferences preferences_{};
  bool begun_ = false;
};

class Esp32DeviceSecurityPlatform final : public DeviceSecurityPlatform {
 public:
  SecurityMode securityMode() const override;
  bool deriveWrappingKey(const uint8_t hardwareId[8],
                         uint8_t output[kKitsuSecretBytes]) override;
  bool randomBytes(uint8_t* output, size_t outputBytes) override;
  bool seal(const uint8_t key[kKitsuSecretBytes], uint32_t generation,
            const uint8_t nonce[kSecurityNonceBytes],
            const uint8_t* plaintext, size_t plaintextBytes,
            uint8_t* ciphertext,
            uint8_t tag[kSecurityTagBytes]) override;
  bool open(const uint8_t key[kKitsuSecretBytes], uint32_t generation,
            const uint8_t nonce[kSecurityNonceBytes],
            const uint8_t* ciphertext, size_t ciphertextBytes,
            const uint8_t tag[kSecurityTagBytes],
            uint8_t* plaintext) override;
  bool hkdfSha256(const uint8_t* inputKey, size_t inputKeyBytes,
                  const uint8_t* salt, size_t saltBytes,
                  const uint8_t* info, size_t infoBytes,
                  uint8_t* output, size_t outputBytes) override;

};

class Esp32JournalStorage final : public discovery::JournalStorage {
 public:
  ~Esp32JournalStorage() override;
  bool begin(const char* partitionLabel = nullptr);
  void end();
  bool readSlot(uint8_t slot, uint8_t* output, size_t capacity,
                size_t& outputBytes) override;
  bool writeSlot(uint8_t slot, const uint8_t* input,
                 size_t inputBytes) override;

 private:
  Preferences preferences_{};
  bool begun_ = false;
};

class Esp32JournalCrypto final : public discovery::JournalCrypto {
 public:
  Esp32JournalCrypto();
  ~Esp32JournalCrypto() override;
  bool setKey(const uint8_t key[kKitsuSecretBytes]);
  bool randomNonce(uint8_t output[discovery::kDiscoveryNonceBytes]) override;
  bool seal(uint32_t generation,
            const uint8_t nonce[discovery::kDiscoveryNonceBytes],
            const uint8_t* plaintext, size_t plaintextBytes,
            uint8_t* ciphertext,
            uint8_t tag[discovery::kDiscoveryTagBytes]) override;
  bool open(uint32_t generation,
            const uint8_t nonce[discovery::kDiscoveryNonceBytes],
            const uint8_t* ciphertext, size_t ciphertextBytes,
            const uint8_t tag[discovery::kDiscoveryTagBytes],
            uint8_t* plaintext) override;

 private:
  uint8_t key_[kKitsuSecretBytes]{};
  bool keyed_ = false;
};

class Esp32CompanionCrypto final : public companion::CompanionCrypto {
 public:
  bool randomBytes(uint8_t* output, size_t outputBytes) override;
  bool sha256(const companion::CryptoPart* parts, size_t partCount,
              uint8_t output[32]) override;
  bool hmacSha256(const uint8_t key[companion::kEnvelopeKeyBytes],
                  const companion::CryptoPart* parts, size_t partCount,
                  uint8_t output[companion::kEnvelopeMacBytes]) override;
  bool hkdfSha256(
      const uint8_t inputKey[companion::kEnvelopeKeyBytes],
      const uint8_t* salt, size_t saltBytes, const uint8_t* info,
      size_t infoBytes,
      uint8_t output[companion::kEnvelopeKeyBytes]) override;
};

class Esp32EnrollmentPlatformCrypto final : public EnrollmentPlatformCrypto {
 public:
  bool generateP256KeyPair(
      uint8_t privateKey[kEnrollmentPrivateKeyBytes],
      uint8_t publicKey[kEnrollmentPublicKeyBytes]) override;
  bool createP256CsrDer(
      const uint8_t privateKey[kEnrollmentPrivateKeyBytes],
      const char* hardwareUid, size_t hardwareUidBytes, uint8_t* output,
      size_t outputCapacity, size_t& outputBytes) override;
  bool signP256DigestP1363(
      const uint8_t privateKey[kEnrollmentPrivateKeyBytes],
      const uint8_t digest[32],
      uint8_t signature[kEnrollmentSignatureBytes]) override;
  bool p256Ecdh(
      const uint8_t privateKey[kEnrollmentPrivateKeyBytes],
      const uint8_t peerPublicKey[kEnrollmentPublicKeyBytes],
      uint8_t sharedSecret[32]) override;
  bool aes256GcmOpen(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t* aad, size_t aadBytes,
                     const uint8_t* ciphertext, size_t ciphertextBytes,
                     const uint8_t tag[16], uint8_t* plaintext) override;
  bool certificateBindsKeyAndCompanion(
      const uint8_t* certificateDer, size_t certificateBytes,
      const uint8_t expectedPublicKey[kEnrollmentPublicKeyBytes],
      const uint8_t companionUuid[kEnrollmentUuidBytes]) override;
};

}  // namespace connectivity
}  // namespace kitsu868

#endif  // ARDUINO_ARCH_ESP32
