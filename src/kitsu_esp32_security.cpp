#include "kitsu_esp32_security.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <esp_random.h>

#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr const char* kSecurityNamespace = "kitsu_sec";
constexpr const char* kJournalNamespace = "kitsu_jrn";

const char* slotKey(uint8_t slot) {
  return slot == 0U ? "slot_a" : slot == 1U ? "slot_b" : nullptr;
}

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}


bool hkdfSha256Rfc5869(const uint8_t* inputKey, size_t inputKeyBytes,
                       const uint8_t* salt, size_t saltBytes,
                       const uint8_t* info, size_t infoBytes,
                       uint8_t* output, size_t outputBytes) {
  if ((!inputKey && inputKeyBytes != 0U) ||
      (!salt && saltBytes != 0U) || (!info && infoBytes != 0U) ||
      (!output && outputBytes != 0U) || outputBytes > 255U * 32U) {
    return false;
  }
  const mbedtls_md_info_t* sha256 =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!sha256) return false;
  uint8_t zeroSalt[32]{};
  uint8_t prk[32]{};
  uint8_t previous[32]{};
  const uint8_t* extractSalt = saltBytes == 0U ? zeroSalt : salt;
  const size_t extractSaltBytes = saltBytes == 0U ? sizeof(zeroSalt)
                                                  : saltBytes;
  bool ok = mbedtls_md_hmac(sha256, extractSalt, extractSaltBytes,
                            inputKey, inputKeyBytes, prk) == 0;
  size_t written = 0U;
  size_t previousBytes = 0U;
  uint8_t counter = 1U;
  while (ok && written < outputBytes) {
    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    ok = mbedtls_md_setup(&context, sha256, 1) == 0 &&
         mbedtls_md_hmac_starts(&context, prk, sizeof(prk)) == 0;
    if (ok && previousBytes != 0U) {
      ok = mbedtls_md_hmac_update(&context, previous, previousBytes) == 0;
    }
    if (ok && infoBytes != 0U) {
      ok = mbedtls_md_hmac_update(&context, info, infoBytes) == 0;
    }
    if (ok) ok = mbedtls_md_hmac_update(&context, &counter, 1U) == 0;
    if (ok) ok = mbedtls_md_hmac_finish(&context, previous) == 0;
    mbedtls_md_free(&context);
    if (!ok) break;
    previousBytes = sizeof(previous);
    const size_t remaining = outputBytes - written;
    const size_t copying = remaining < sizeof(previous)
                               ? remaining
                               : sizeof(previous);
    if (copying != 0U) memcpy(output + written, previous, copying);
    written += copying;
    ++counter;
  }
  secureZero(zeroSalt, sizeof(zeroSalt));
  secureZero(prk, sizeof(prk));
  secureZero(previous, sizeof(previous));
  if (!ok || written != outputBytes) {
    if (output && outputBytes != 0U) secureZero(output, outputBytes);
    return false;
  }
  return true;
}

void generationAad(uint32_t generation, uint8_t output[4]) {
  output[0] = static_cast<uint8_t>(generation >> 24U);
  output[1] = static_cast<uint8_t>(generation >> 16U);
  output[2] = static_cast<uint8_t>(generation >> 8U);
  output[3] = static_cast<uint8_t>(generation);
}

bool aesGcmSeal(const uint8_t key[kKitsuSecretBytes], uint32_t generation,
                const uint8_t nonce[kSecurityNonceBytes],
                const uint8_t* plaintext, size_t plaintextBytes,
                uint8_t* ciphertext, uint8_t tag[kSecurityTagBytes]) {
  if (!key || !nonce || (!plaintext && plaintextBytes != 0U) ||
      (!ciphertext && plaintextBytes != 0U) || !tag) {
    return false;
  }
  uint8_t aad[4]{};
  generationAad(generation, aad);
  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  const int keyed = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES,
                                       key, kKitsuSecretBytes * 8U);
  const int encrypted = keyed == 0
      ? mbedtls_gcm_crypt_and_tag(&context, MBEDTLS_GCM_ENCRYPT,
                                  plaintextBytes, nonce,
                                  kSecurityNonceBytes, aad, sizeof(aad),
                                  plaintext, ciphertext,
                                  kSecurityTagBytes, tag)
      : keyed;
  mbedtls_gcm_free(&context);
  secureZero(aad, sizeof(aad));
  return encrypted == 0;
}

bool aesGcmOpen(const uint8_t key[kKitsuSecretBytes], uint32_t generation,
                const uint8_t nonce[kSecurityNonceBytes],
                const uint8_t* ciphertext, size_t ciphertextBytes,
                const uint8_t tag[kSecurityTagBytes], uint8_t* plaintext) {
  if (!key || !nonce || (!ciphertext && ciphertextBytes != 0U) || !tag ||
      (!plaintext && ciphertextBytes != 0U)) {
    return false;
  }
  uint8_t aad[4]{};
  generationAad(generation, aad);
  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  const int keyed = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES,
                                       key, kKitsuSecretBytes * 8U);
  const int decrypted = keyed == 0
      ? mbedtls_gcm_auth_decrypt(&context, ciphertextBytes, nonce,
                                 kSecurityNonceBytes, aad, sizeof(aad),
                                 tag, kSecurityTagBytes, ciphertext,
                                 plaintext)
      : keyed;
  mbedtls_gcm_free(&context);
  secureZero(aad, sizeof(aad));
  if (decrypted != 0 && plaintext && ciphertextBytes != 0U) {
    secureZero(plaintext, ciphertextBytes);
  }
  return decrypted == 0;
}

bool readPreferenceSlot(Preferences& preferences, bool begun, uint8_t slot,
                        uint8_t* output, size_t capacity,
                        size_t& outputBytes) {
  outputBytes = 0U;
  const char* key = slotKey(slot);
  if (!begun || !key || !output) return false;
  const size_t bytes = preferences.getBytesLength(key);
  if (bytes == 0U) return true;
  if (bytes > capacity) return false;
  if (preferences.getBytes(key, output, capacity) != bytes) return false;
  outputBytes = bytes;
  return true;
}

bool writePreferenceSlot(Preferences& preferences, bool begun, uint8_t slot,
                         const uint8_t* input, size_t inputBytes,
                         size_t maximumBytes) {
  const char* key = slotKey(slot);
  if (!begun || !key || !input || inputBytes == 0U ||
      inputBytes > maximumBytes) {
    return false;
  }
  return preferences.putBytes(key, input, inputBytes) == inputBytes;
}

}  // namespace

Esp32DeviceSecurityStorage::~Esp32DeviceSecurityStorage() { end(); }

bool Esp32DeviceSecurityStorage::begin(const char* partitionLabel) {
  end();
  begun_ = preferences_.begin(kSecurityNamespace, false, partitionLabel);
  return begun_;
}

void Esp32DeviceSecurityStorage::end() {
  if (begun_) preferences_.end();
  begun_ = false;
}

bool Esp32DeviceSecurityStorage::readSlot(uint8_t slot, uint8_t* output,
                                          size_t capacity,
                                          size_t& outputBytes) {
  return readPreferenceSlot(preferences_, begun_, slot, output, capacity,
                            outputBytes);
}

bool Esp32DeviceSecurityStorage::writeSlot(uint8_t slot,
                                           const uint8_t* input,
                                           size_t inputBytes) {
  return writePreferenceSlot(preferences_, begun_, slot, input, inputBytes,
                             kSecurityBlobCapacity);
}

bool Esp32DeviceSecurityStorage::clearSlot(uint8_t slot) {
  const char* key = slotKey(slot);
  if (!begun_ || !key) return false;
  if (preferences_.getBytesLength(key) == 0U) return true;
  return preferences_.remove(key);
}

SecurityMode Esp32DeviceSecurityPlatform::securityMode() const {
  return kSelectedSecurityMode;
}

bool Esp32DeviceSecurityPlatform::deriveWrappingKey(
    const uint8_t hardwareId[8], uint8_t output[kKitsuSecretBytes]) {
  if (!hardwareId || !output) return false;
  uint8_t challenge[32]{};
  static const uint8_t domain[] = "Kitsu868 wrap root v1";
  memcpy(challenge, domain, sizeof(domain) - 1U);
  memcpy(challenge + sizeof(challenge) - 8U, hardwareId, 8U);
  // This wrapping root is intentionally reproducible. It keeps credentials
  // non-plaintext and tamper-evident during normal operation, while preserving
  // the owner's ability to recover data or completely repurpose the board.
  const bool ok = mbedtls_sha256_ret(challenge, sizeof(challenge), output, 0) ==
                  0;
  secureZero(challenge, sizeof(challenge));
  return ok;
}

bool Esp32DeviceSecurityPlatform::randomBytes(uint8_t* output,
                                               size_t outputBytes) {
  if (!output && outputBytes != 0U) return false;
  if (outputBytes != 0U) esp_fill_random(output, outputBytes);
  return true;
}

bool Esp32DeviceSecurityPlatform::seal(
    const uint8_t key[kKitsuSecretBytes], uint32_t generation,
    const uint8_t nonce[kSecurityNonceBytes], const uint8_t* plaintext,
    size_t plaintextBytes, uint8_t* ciphertext,
    uint8_t tag[kSecurityTagBytes]) {
  return aesGcmSeal(key, generation, nonce, plaintext, plaintextBytes,
                    ciphertext, tag);
}

bool Esp32DeviceSecurityPlatform::open(
    const uint8_t key[kKitsuSecretBytes], uint32_t generation,
    const uint8_t nonce[kSecurityNonceBytes], const uint8_t* ciphertext,
    size_t ciphertextBytes, const uint8_t tag[kSecurityTagBytes],
    uint8_t* plaintext) {
  return aesGcmOpen(key, generation, nonce, ciphertext, ciphertextBytes, tag,
                    plaintext);
}

bool Esp32DeviceSecurityPlatform::hkdfSha256(
    const uint8_t* inputKey, size_t inputKeyBytes, const uint8_t* salt,
    size_t saltBytes, const uint8_t* info, size_t infoBytes, uint8_t* output,
    size_t outputBytes) {
  if ((!inputKey && inputKeyBytes != 0U) || (!salt && saltBytes != 0U) ||
      (!info && infoBytes != 0U) || (!output && outputBytes != 0U)) {
    return false;
  }
  return hkdfSha256Rfc5869(inputKey, inputKeyBytes, salt, saltBytes,
                           info, infoBytes, output, outputBytes);
}

Esp32JournalStorage::~Esp32JournalStorage() { end(); }

bool Esp32JournalStorage::begin(const char* partitionLabel) {
  end();
  begun_ = preferences_.begin(kJournalNamespace, false, partitionLabel);
  return begun_;
}

void Esp32JournalStorage::end() {
  if (begun_) preferences_.end();
  begun_ = false;
}

bool Esp32JournalStorage::readSlot(uint8_t slot, uint8_t* output,
                                   size_t capacity, size_t& outputBytes) {
  return readPreferenceSlot(preferences_, begun_, slot, output, capacity,
                            outputBytes);
}

bool Esp32JournalStorage::writeSlot(uint8_t slot, const uint8_t* input,
                                    size_t inputBytes) {
  return writePreferenceSlot(preferences_, begun_, slot, input, inputBytes,
                             discovery::kDiscoverySnapshotCapacity);
}

Esp32JournalCrypto::Esp32JournalCrypto() = default;

Esp32JournalCrypto::~Esp32JournalCrypto() {
  secureZero(key_, sizeof(key_));
  keyed_ = false;
}

bool Esp32JournalCrypto::setKey(const uint8_t key[kKitsuSecretBytes]) {
  if (!key) return false;
  memcpy(key_, key, sizeof(key_));
  keyed_ = true;
  return true;
}

bool Esp32JournalCrypto::randomNonce(
    uint8_t output[discovery::kDiscoveryNonceBytes]) {
  if (!keyed_ || !output) return false;
  esp_fill_random(output, discovery::kDiscoveryNonceBytes);
  return true;
}

bool Esp32JournalCrypto::seal(
    uint32_t generation,
    const uint8_t nonce[discovery::kDiscoveryNonceBytes],
    const uint8_t* plaintext, size_t plaintextBytes, uint8_t* ciphertext,
    uint8_t tag[discovery::kDiscoveryTagBytes]) {
  return keyed_ && aesGcmSeal(key_, generation, nonce, plaintext,
                              plaintextBytes, ciphertext, tag);
}

bool Esp32JournalCrypto::open(
    uint32_t generation,
    const uint8_t nonce[discovery::kDiscoveryNonceBytes],
    const uint8_t* ciphertext, size_t ciphertextBytes,
    const uint8_t tag[discovery::kDiscoveryTagBytes], uint8_t* plaintext) {
  return keyed_ && aesGcmOpen(key_, generation, nonce, ciphertext,
                              ciphertextBytes, tag, plaintext);
}

bool Esp32CompanionCrypto::randomBytes(uint8_t* output,
                                       size_t outputBytes) {
  if (!output && outputBytes != 0U) return false;
  if (outputBytes != 0U) esp_fill_random(output, outputBytes);
  return true;
}

bool Esp32CompanionCrypto::sha256(const companion::CryptoPart* parts,
                                  size_t partCount, uint8_t output[32]) {
  if ((!parts && partCount != 0U) || !output ||
      partCount > companion::kMaximumCryptoParts) {
    return false;
  }
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool ok = mbedtls_sha256_starts_ret(&context, 0) == 0;
  for (size_t i = 0U; ok && i < partCount; ++i) {
    if (!parts[i].data && parts[i].bytes != 0U) {
      ok = false;
    } else if (parts[i].bytes != 0U) {
      ok = mbedtls_sha256_update_ret(&context, parts[i].data,
                                     parts[i].bytes) == 0;
    }
  }
  if (ok) ok = mbedtls_sha256_finish_ret(&context, output) == 0;
  mbedtls_sha256_free(&context);
  if (!ok) secureZero(output, 32U);
  return ok;
}

bool Esp32CompanionCrypto::hmacSha256(
    const uint8_t key[companion::kEnvelopeKeyBytes],
    const companion::CryptoPart* parts, size_t partCount,
    uint8_t output[companion::kEnvelopeMacBytes]) {
  if (!key || (!parts && partCount != 0U) || !output ||
      partCount > companion::kMaximumCryptoParts) {
    return false;
  }
  const mbedtls_md_info_t* sha256 =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!sha256) return false;
  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  bool ok = mbedtls_md_setup(&context, sha256, 1) == 0 &&
            mbedtls_md_hmac_starts(&context, key,
                                   companion::kEnvelopeKeyBytes) == 0;
  for (size_t i = 0U; ok && i < partCount; ++i) {
    if (!parts[i].data && parts[i].bytes != 0U) {
      ok = false;
    } else if (parts[i].bytes != 0U) {
      ok = mbedtls_md_hmac_update(&context, parts[i].data,
                                  parts[i].bytes) == 0;
    }
  }
  if (ok) ok = mbedtls_md_hmac_finish(&context, output) == 0;
  mbedtls_md_free(&context);
  if (!ok) secureZero(output, companion::kEnvelopeMacBytes);
  return ok;
}

bool Esp32CompanionCrypto::hkdfSha256(
    const uint8_t inputKey[companion::kEnvelopeKeyBytes],
    const uint8_t* salt, size_t saltBytes, const uint8_t* info,
    size_t infoBytes, uint8_t output[companion::kEnvelopeKeyBytes]) {
  if (!inputKey || (!salt && saltBytes != 0U) ||
      (!info && infoBytes != 0U) || !output) {
    return false;
  }
  return hkdfSha256Rfc5869(inputKey, companion::kEnvelopeKeyBytes,
                           salt, saltBytes, info, infoBytes, output,
                           companion::kEnvelopeKeyBytes);
}

}  // namespace connectivity
}  // namespace kitsu868

#endif  // ARDUINO_ARCH_ESP32
