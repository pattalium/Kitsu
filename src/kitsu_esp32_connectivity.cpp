#include "kitsu_esp32_connectivity.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <esp_partition.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/oid.h>
#include <mbedtls/x509_crt.h>

#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr uint32_t kExpectedPartitionAddress = 0x7B0000UL;
constexpr uint8_t kConnectionPartitionSubtype = 0x40U;

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

bool erasedHeader(const uint8_t* input, size_t bytes) {
  if (!input) return false;
  for (size_t i = 0U; i < bytes; ++i) {
    if (input[i] != 0xffU) return false;
  }
  return true;
}

bool caCertificate(const mbedtls_x509_crt& certificate) {
  if (certificate.ca_istrue == 0) return false;
  if ((certificate.ext_types & MBEDTLS_X509_EXT_KEY_USAGE) != 0U &&
      (certificate.key_usage & MBEDTLS_X509_KU_KEY_CERT_SIGN) == 0U) {
    return false;
  }
  return true;
}

}  // namespace

bool Esp32ConnectionSlotStorage::begin() {
  partition_ = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      static_cast<esp_partition_subtype_t>(kConnectionPartitionSubtype),
      "kitsu_conn");
  if (!partition_ || partition_->address != kExpectedPartitionAddress ||
      partition_->size != kConnectionPartitionBytes) {
    partition_ = nullptr;
  }
  return partition_ != nullptr;
}

bool Esp32ConnectionSlotStorage::available() const {
  return partition_ != nullptr;
}

bool Esp32ConnectionSlotStorage::readSlot(uint8_t slot, uint8_t* output,
                                          size_t capacity,
                                          size_t& outputBytes) {
  outputBytes = 0U;
  if (!partition_ || slot >= kConnectionSlotCount || !output ||
      capacity < kConnectionSnapshotBytes) {
    return false;
  }
  const size_t offset = static_cast<size_t>(slot) * kConnectionSlotBytes;
  uint8_t header[4]{};
  if (esp_partition_read(partition_, offset, header, sizeof(header)) !=
      ESP_OK) {
    return false;
  }
  if (erasedHeader(header, sizeof(header))) return true;
  if (esp_partition_read(partition_, offset, output,
                         kConnectionSnapshotBytes) != ESP_OK) {
    return false;
  }
  outputBytes = kConnectionSnapshotBytes;
  return true;
}

bool Esp32ConnectionSlotStorage::writeSlot(uint8_t slot,
                                           const uint8_t* input,
                                           size_t inputBytes) {
  if (!partition_ || slot >= kConnectionSlotCount || !input ||
      inputBytes != kConnectionSnapshotBytes) {
    return false;
  }
  const size_t offset = static_cast<size_t>(slot) * kConnectionSlotBytes;
  return esp_partition_erase_range(partition_, offset,
                                   kConnectionSlotBytes) == ESP_OK &&
         esp_partition_write(partition_, offset, input, inputBytes) == ESP_OK;
}

Esp32ConnectionStoreCrypto::Esp32ConnectionStoreCrypto() = default;

Esp32ConnectionStoreCrypto::~Esp32ConnectionStoreCrypto() {
  secureZero(key_, sizeof(key_));
  keyed_ = false;
}

bool Esp32ConnectionStoreCrypto::setKey(const uint8_t key[32]) {
  if (!key) return false;
  memcpy(key_, key, sizeof(key_));
  keyed_ = true;
  return true;
}

bool Esp32ConnectionStoreCrypto::ready() const { return keyed_; }

bool Esp32ConnectionStoreCrypto::randomBytes(uint8_t* output,
                                              size_t outputBytes) {
  if (!keyed_ || (!output && outputBytes != 0U)) return false;
  if (outputBytes != 0U) esp_fill_random(output, outputBytes);
  return true;
}

bool Esp32ConnectionStoreCrypto::seal(
    const uint8_t nonce[12], const uint8_t* aad, size_t aadBytes,
    const uint8_t* plaintext, size_t plaintextBytes, uint8_t* ciphertext,
    uint8_t tag[16]) {
  if (!keyed_ || !nonce || (!aad && aadBytes != 0U) ||
      (!plaintext && plaintextBytes != 0U) ||
      (!ciphertext && plaintextBytes != 0U) || !tag) {
    return false;
  }
  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  const int keyed =
      mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key_, 256U);
  const int sealed = keyed == 0
      ? mbedtls_gcm_crypt_and_tag(
            &context, MBEDTLS_GCM_ENCRYPT, plaintextBytes, nonce, 12U, aad,
            aadBytes, plaintext, ciphertext, 16U, tag)
      : keyed;
  mbedtls_gcm_free(&context);
  return sealed == 0;
}

bool Esp32ConnectionStoreCrypto::open(
    const uint8_t nonce[12], const uint8_t* aad, size_t aadBytes,
    const uint8_t* ciphertext, size_t ciphertextBytes, const uint8_t tag[16],
    uint8_t* plaintext) {
  if (!keyed_ || !nonce || (!aad && aadBytes != 0U) ||
      (!ciphertext && ciphertextBytes != 0U) || !tag ||
      (!plaintext && ciphertextBytes != 0U)) {
    return false;
  }
  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  const int keyed =
      mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key_, 256U);
  const int opened = keyed == 0
      ? mbedtls_gcm_auth_decrypt(&context, ciphertextBytes, nonce, 12U, aad,
                                 aadBytes, tag, 16U, ciphertext, plaintext)
      : keyed;
  mbedtls_gcm_free(&context);
  if (opened != 0 && plaintext && ciphertextBytes != 0U) {
    secureZero(plaintext, ciphertextBytes);
  }
  return opened == 0;
}

bool Esp32GatewayTrustValidator::validCertificateAuthority(
    const uint8_t* der, size_t derBytes) {
  if (!der || derBytes == 0U || derBytes > kGatewayCaMaximumBytes) {
    return false;
  }
  mbedtls_x509_crt certificate;
  mbedtls_x509_crt_init(&certificate);
  const bool valid =
      mbedtls_x509_crt_parse_der(&certificate, der, derBytes) == 0 &&
      certificate.next == nullptr && caCertificate(certificate);
  mbedtls_x509_crt_free(&certificate);
  return valid;
}

bool Esp32GatewayTrustValidator::validateEnrollmentChain(
    const uint8_t* caDer, size_t caDerBytes, const uint8_t* leafDer,
    size_t leafDerBytes, const uint8_t* const* chainDer,
    const size_t* chainBytes, size_t chainCount) {
  if (!caDer || caDerBytes == 0U || !leafDer || leafDerBytes == 0U ||
      chainCount > kEnrollmentMaximumChainCertificates ||
      (chainCount != 0U && (!chainDer || !chainBytes))) {
    return false;
  }
  mbedtls_x509_crt trust;
  mbedtls_x509_crt leafAndChain;
  mbedtls_x509_crt_init(&trust);
  mbedtls_x509_crt_init(&leafAndChain);
  bool ok = mbedtls_x509_crt_parse_der(&trust, caDer, caDerBytes) == 0 &&
            trust.next == nullptr && caCertificate(trust) &&
            mbedtls_x509_crt_parse_der(&leafAndChain, leafDer,
                                       leafDerBytes) == 0;
  for (size_t i = 0U; ok && i < chainCount; ++i) {
    ok = chainDer[i] && chainBytes[i] != 0U &&
         mbedtls_x509_crt_parse_der(&leafAndChain, chainDer[i],
                                    chainBytes[i]) == 0;
  }
  uint32_t flags = 0U;
  if (ok) {
    ok = mbedtls_x509_crt_verify(&leafAndChain, &trust, nullptr, nullptr,
                                 &flags, nullptr, nullptr) == 0 &&
         flags == 0U;
  }
  mbedtls_x509_crt_free(&leafAndChain);
  mbedtls_x509_crt_free(&trust);
  return ok;
}

}  // namespace connectivity
}  // namespace kitsu868

#endif
