#pragma once

#if defined(ARDUINO_ARCH_ESP32)

#include <esp_partition.h>

#include "kitsu_connectivity_config.h"

namespace kitsu868 {
namespace connectivity {

class Esp32ConnectionSlotStorage final : public ConnectionSlotStorage {
 public:
  bool begin();
  bool available() const override;
  bool readSlot(uint8_t slot, uint8_t* output, size_t capacity,
                size_t& outputBytes) override;
  bool writeSlot(uint8_t slot, const uint8_t* input,
                 size_t inputBytes) override;

 private:
  const esp_partition_t* partition_ = nullptr;
};

class Esp32ConnectionStoreCrypto final : public ConnectionStoreCrypto {
 public:
  Esp32ConnectionStoreCrypto();
  ~Esp32ConnectionStoreCrypto() override;
  bool setKey(const uint8_t key[32]);
  bool ready() const override;
  bool randomBytes(uint8_t* output, size_t outputBytes) override;
  bool seal(const uint8_t nonce[12], const uint8_t* aad,
            size_t aadBytes, const uint8_t* plaintext,
            size_t plaintextBytes, uint8_t* ciphertext,
            uint8_t tag[16]) override;
  bool open(const uint8_t nonce[12], const uint8_t* aad,
            size_t aadBytes, const uint8_t* ciphertext,
            size_t ciphertextBytes, const uint8_t tag[16],
            uint8_t* plaintext) override;

 private:
  uint8_t key_[32]{};
  bool keyed_ = false;
};

class Esp32GatewayTrustValidator final : public GatewayTrustValidator {
 public:
  bool validCertificateAuthority(const uint8_t* der,
                                 size_t derBytes) override;
  bool validateEnrollmentChain(
      const uint8_t* caDer, size_t caDerBytes, const uint8_t* leafDer,
      size_t leafDerBytes, const uint8_t* const* chainDer,
      const size_t* chainBytes, size_t chainCount) override;
};

}  // namespace connectivity
}  // namespace kitsu868

#endif
