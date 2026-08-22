#include "kitsu_esp32_security.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <esp_random.h>

#include <mbedtls/gcm.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include <stdio.h>
#include <stdlib.h>
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

int espRandomCallback(void*, unsigned char* output, size_t outputBytes) {
  if (!output && outputBytes != 0U) return -1;
  if (outputBytes != 0U) esp_fill_random(output, outputBytes);
  return 0;
}

bool loadP256Private(const uint8_t privateKey[kEnrollmentPrivateKeyBytes],
                     mbedtls_ecp_keypair& keypair) {
  if (!privateKey ||
      mbedtls_ecp_group_load(&keypair.grp,
                             MBEDTLS_ECP_DP_SECP256R1) != 0 ||
      mbedtls_mpi_read_binary(&keypair.d, privateKey,
                              kEnrollmentPrivateKeyBytes) != 0 ||
      mbedtls_ecp_check_privkey(&keypair.grp, &keypair.d) != 0 ||
      mbedtls_ecp_mul(&keypair.grp, &keypair.Q, &keypair.d,
                      &keypair.grp.G, espRandomCallback, nullptr) != 0) {
    return false;
  }
  return true;
}

struct DerView {
  constexpr DerView(const uint8_t* input = nullptr, size_t inputBytes = 0U)
      : data(input), bytes(inputBytes) {}

  const uint8_t* data = nullptr;
  size_t bytes = 0U;
};

bool takeDer(DerView& input, uint8_t& tag, DerView& value) {
  value = DerView{};
  if (!input.data || input.bytes < 2U) return false;
  tag = input.data[0];
  size_t cursor = 1U;
  size_t length = input.data[cursor++];
  if ((length & 0x80U) != 0U) {
    const size_t lengthBytes = length & 0x7fU;
    if (lengthBytes == 0U || lengthBytes > sizeof(size_t) ||
        cursor + lengthBytes > input.bytes ||
        input.data[cursor] == 0U) {
      return false;
    }
    length = 0U;
    for (size_t i = 0U; i < lengthBytes; ++i) {
      if (length > (static_cast<size_t>(-1) >> 8U)) return false;
      length = (length << 8U) | input.data[cursor++];
    }
    if (length < 128U) return false;
  }
  if (length > input.bytes - cursor) return false;
  value.data = input.data + cursor;
  value.bytes = length;
  cursor += length;
  input.data += cursor;
  input.bytes -= cursor;
  return true;
}

bool takeDerTag(DerView& input, uint8_t expectedTag, DerView& value) {
  uint8_t tag = 0U;
  return takeDer(input, tag, value) && tag == expectedTag;
}

bool certificateHasUriSan(const uint8_t* certificateDer,
                          size_t certificateBytes, const char* expected,
                          size_t expectedBytes) {
  DerView certificate{certificateDer, certificateBytes};
  DerView outer{};
  if (!takeDerTag(certificate, 0x30U, outer) || certificate.bytes != 0U) {
    return false;
  }
  DerView tbs{};
  if (!takeDerTag(outer, 0x30U, tbs)) return false;

  uint8_t tag = 0U;
  DerView field{};
  if (tbs.bytes != 0U && tbs.data[0] == 0xa0U &&
      !takeDer(tbs, tag, field)) {
    return false;
  }
  // serialNumber, signature, issuer, validity, subject, subjectPublicKeyInfo.
  for (size_t i = 0U; i < 6U; ++i) {
    if (!takeDer(tbs, tag, field)) return false;
  }
  DerView explicitExtensions{};
  bool foundExtensions = false;
  while (tbs.bytes != 0U) {
    if (!takeDer(tbs, tag, field)) return false;
    if (tag == 0xa3U) {
      explicitExtensions = field;
      foundExtensions = true;
      break;
    }
  }
  if (!foundExtensions) return false;

  DerView extensions{};
  if (!takeDerTag(explicitExtensions, 0x30U, extensions) ||
      explicitExtensions.bytes != 0U) {
    return false;
  }
  static const uint8_t subjectAltNameOid[] = {0x55U, 0x1dU, 0x11U};
  while (extensions.bytes != 0U) {
    DerView extension{};
    if (!takeDerTag(extensions, 0x30U, extension)) return false;
    DerView oid{};
    if (!takeDerTag(extension, 0x06U, oid)) return false;
    if (extension.bytes != 0U && extension.data[0] == 0x01U &&
        !takeDer(extension, tag, field)) {
      return false;
    }
    DerView encodedValue{};
    if (!takeDerTag(extension, 0x04U, encodedValue) ||
        extension.bytes != 0U) {
      return false;
    }
    if (oid.bytes != sizeof(subjectAltNameOid) ||
        memcmp(oid.data, subjectAltNameOid, sizeof(subjectAltNameOid)) != 0) {
      continue;
    }
    DerView names{};
    if (!takeDerTag(encodedValue, 0x30U, names) ||
        encodedValue.bytes != 0U) {
      return false;
    }
    while (names.bytes != 0U) {
      DerView name{};
      if (!takeDer(names, tag, name)) return false;
      if (tag == 0x86U && name.bytes == expectedBytes &&
          memcmp(name.data, expected, expectedBytes) == 0) {
        return true;
      }
    }
    return false;
  }
  return false;
}

void formatCompanionSan(const uint8_t uuid[kEnrollmentUuidBytes],
                        char output[57]) {
  static const char hex[] = "0123456789abcdef";
  static const char prefix[] = "urn:kitsu:companion:";
  memcpy(output, prefix, sizeof(prefix) - 1U);
  size_t cursor = sizeof(prefix) - 1U;
  for (size_t i = 0U; i < kEnrollmentUuidBytes; ++i) {
    if (i == 4U || i == 6U || i == 8U || i == 10U) output[cursor++] = '-';
    output[cursor++] = hex[uuid[i] >> 4U];
    output[cursor++] = hex[uuid[i] & 0x0fU];
  }
  output[cursor] = '\0';
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

bool Esp32EnrollmentPlatformCrypto::generateP256KeyPair(
    uint8_t privateKey[kEnrollmentPrivateKeyBytes],
    uint8_t publicKey[kEnrollmentPublicKeyBytes]) {
  if (!privateKey || !publicKey) return false;
  memset(privateKey, 0, kEnrollmentPrivateKeyBytes);
  memset(publicKey, 0, kEnrollmentPublicKeyBytes);
  mbedtls_ecp_keypair keypair;
  mbedtls_ecp_keypair_init(&keypair);
  size_t publicBytes = 0U;
  const bool ok =
      mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &keypair,
                          espRandomCallback, nullptr) == 0 &&
      mbedtls_mpi_write_binary(&keypair.d, privateKey,
                               kEnrollmentPrivateKeyBytes) == 0 &&
      mbedtls_ecp_point_write_binary(
          &keypair.grp, &keypair.Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
          &publicBytes, publicKey, kEnrollmentPublicKeyBytes) == 0 &&
      publicBytes == kEnrollmentPublicKeyBytes;
  mbedtls_ecp_keypair_free(&keypair);
  if (!ok) {
    secureZero(privateKey, kEnrollmentPrivateKeyBytes);
    secureZero(publicKey, kEnrollmentPublicKeyBytes);
  }
  return ok;
}

bool Esp32EnrollmentPlatformCrypto::createP256CsrDer(
    const uint8_t privateKey[kEnrollmentPrivateKeyBytes],
    const char* hardwareUid, size_t hardwareUidBytes, uint8_t* output,
    size_t outputCapacity, size_t& outputBytes) {
  outputBytes = 0U;
  if (!privateKey || !hardwareUid || hardwareUidBytes == 0U ||
      hardwareUidBytes > kEnrollmentMaximumHardwareUidBytes || !output ||
      outputCapacity == 0U) {
    return false;
  }
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  mbedtls_x509write_csr csr;
  mbedtls_x509write_csr_init(&csr);
  char subject[kEnrollmentMaximumHardwareUidBytes + 4U]{};
  const int subjectBytes = snprintf(subject, sizeof(subject), "CN=%.*s",
                                    static_cast<int>(hardwareUidBytes),
                                    hardwareUid);
  bool ok = subjectBytes > 0 &&
            static_cast<size_t>(subjectBytes) < sizeof(subject) &&
            mbedtls_pk_setup(
                &key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0;
  mbedtls_ecp_keypair* ec = ok ? mbedtls_pk_ec(key) : nullptr;
  if (ok) ok = ec && loadP256Private(privateKey, *ec);
  if (ok) {
    mbedtls_x509write_csr_set_key(&csr, &key);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    // The service applies the certificate profile and rejects CSR extensions.
    ok = mbedtls_x509write_csr_set_subject_name(&csr, subject) == 0;
  }
  int encoded = -1;
  if (ok) {
    encoded = mbedtls_x509write_csr_der(&csr, output, outputCapacity,
                                        espRandomCallback, nullptr);
    ok = encoded > 0 && static_cast<size_t>(encoded) <= outputCapacity;
  }
  if (ok) {
    outputBytes = static_cast<size_t>(encoded);
    memmove(output, output + outputCapacity - outputBytes, outputBytes);
  } else {
    secureZero(output, outputCapacity);
  }
  secureZero(subject, sizeof(subject));
  mbedtls_x509write_csr_free(&csr);
  mbedtls_pk_free(&key);
  return ok;
}

bool Esp32EnrollmentPlatformCrypto::signP256DigestP1363(
    const uint8_t privateKey[kEnrollmentPrivateKeyBytes],
    const uint8_t digest[32],
    uint8_t signature[kEnrollmentSignatureBytes]) {
  if (!privateKey || !digest || !signature) return false;
  memset(signature, 0, kEnrollmentSignatureBytes);
  mbedtls_ecp_keypair keypair;
  mbedtls_ecp_keypair_init(&keypair);
  mbedtls_mpi r;
  mbedtls_mpi s;
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  const bool ok =
      loadP256Private(privateKey, keypair) &&
      mbedtls_ecdsa_sign(&keypair.grp, &r, &s, &keypair.d, digest, 32U,
                         espRandomCallback, nullptr) == 0 &&
      mbedtls_mpi_write_binary(&r, signature, 32U) == 0 &&
      mbedtls_mpi_write_binary(&s, signature + 32U, 32U) == 0;
  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_ecp_keypair_free(&keypair);
  if (!ok) secureZero(signature, kEnrollmentSignatureBytes);
  return ok;
}

bool Esp32EnrollmentPlatformCrypto::p256Ecdh(
    const uint8_t privateKey[kEnrollmentPrivateKeyBytes],
    const uint8_t peerPublicKey[kEnrollmentPublicKeyBytes],
    uint8_t sharedSecret[32]) {
  if (!privateKey || !peerPublicKey || !sharedSecret ||
      peerPublicKey[0] != 0x04U) {
    return false;
  }
  memset(sharedSecret, 0, 32U);
  mbedtls_ecp_group group;
  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point peer;
  mbedtls_ecp_point_init(&peer);
  mbedtls_mpi privateValue;
  mbedtls_mpi sharedValue;
  mbedtls_mpi_init(&privateValue);
  mbedtls_mpi_init(&sharedValue);
  const bool ok =
      mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
      mbedtls_mpi_read_binary(&privateValue, privateKey,
                              kEnrollmentPrivateKeyBytes) == 0 &&
      mbedtls_ecp_check_privkey(&group, &privateValue) == 0 &&
      mbedtls_ecp_point_read_binary(&group, &peer, peerPublicKey,
                                    kEnrollmentPublicKeyBytes) == 0 &&
      mbedtls_ecp_check_pubkey(&group, &peer) == 0 &&
      mbedtls_ecdh_compute_shared(&group, &sharedValue, &peer, &privateValue,
                                  espRandomCallback, nullptr) == 0 &&
      mbedtls_mpi_write_binary(&sharedValue, sharedSecret, 32U) == 0;
  mbedtls_mpi_free(&sharedValue);
  mbedtls_mpi_free(&privateValue);
  mbedtls_ecp_point_free(&peer);
  mbedtls_ecp_group_free(&group);
  if (!ok) secureZero(sharedSecret, 32U);
  return ok;
}

bool Esp32EnrollmentPlatformCrypto::aes256GcmOpen(
    const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad,
    size_t aadBytes, const uint8_t* ciphertext, size_t ciphertextBytes,
    const uint8_t tag[16], uint8_t* plaintext) {
  if (!key || !nonce || (!aad && aadBytes != 0U) ||
      (!ciphertext && ciphertextBytes != 0U) || !tag ||
      (!plaintext && ciphertextBytes != 0U)) {
    return false;
  }
  mbedtls_gcm_context context;
  mbedtls_gcm_init(&context);
  const int keyed = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key,
                                       256U);
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

bool Esp32EnrollmentPlatformCrypto::certificateBindsKeyAndCompanion(
    const uint8_t* certificateDer, size_t certificateBytes,
    const uint8_t expectedPublicKey[kEnrollmentPublicKeyBytes],
    const uint8_t companionUuid[kEnrollmentUuidBytes]) {
  if (!certificateDer || certificateBytes == 0U || !expectedPublicKey ||
      !companionUuid || expectedPublicKey[0] != 0x04U) {
    return false;
  }
  mbedtls_x509_crt certificate;
  mbedtls_x509_crt_init(&certificate);
  bool ok = mbedtls_x509_crt_parse_der(&certificate, certificateDer,
                                       certificateBytes) == 0 &&
            mbedtls_pk_get_type(&certificate.pk) == MBEDTLS_PK_ECKEY;
  uint8_t publicKey[kEnrollmentPublicKeyBytes]{};
  size_t publicKeyBytes = 0U;
  mbedtls_ecp_keypair* ec = ok ? mbedtls_pk_ec(certificate.pk) : nullptr;
  if (ok) {
    ok = ec && ec->grp.id == MBEDTLS_ECP_DP_SECP256R1 &&
         mbedtls_ecp_point_write_binary(
             &ec->grp, &ec->Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
             &publicKeyBytes, publicKey, sizeof(publicKey)) == 0 &&
         publicKeyBytes == sizeof(publicKey) &&
         memcmp(publicKey, expectedPublicKey, sizeof(publicKey)) == 0;
  }
  char expectedSan[57]{};
  formatCompanionSan(companionUuid, expectedSan);
  if (ok) {
    ok = certificateHasUriSan(certificateDer, certificateBytes, expectedSan,
                              strlen(expectedSan));
  }
  secureZero(publicKey, sizeof(publicKey));
  secureZero(expectedSan, sizeof(expectedSan));
  mbedtls_x509_crt_free(&certificate);
  return ok;
}

}  // namespace connectivity
}  // namespace kitsu868

#endif  // ARDUINO_ARCH_ESP32
