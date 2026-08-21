#include "../src/kitsu_enrollment.h"

#include <assert.h>
#include <windows.h>
#include <bcrypt.h>

#include <string.h>

#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

using kitsu868::companion::CompanionCrypto;
using kitsu868::companion::CryptoPart;
using kitsu868::connectivity::EnrollmentCredentialSink;
using kitsu868::connectivity::EnrollmentPlatformCrypto;
using kitsu868::connectivity::EnrollmentResponse;
using kitsu868::connectivity::EnrollmentResult;
using kitsu868::connectivity::KitsuEnrollmentRecipient;
using kitsu868::connectivity::kEnrollmentCiphertextBytes;
using kitsu868::connectivity::kEnrollmentInfoBytes;
using kitsu868::connectivity::kEnrollmentNonceBytes;
using kitsu868::connectivity::kEnrollmentPrivateKeyBytes;
using kitsu868::connectivity::kEnrollmentPublicKeyBytes;
using kitsu868::connectivity::kEnrollmentSecretBytes;
using kitsu868::connectivity::kEnrollmentSignatureBytes;
using kitsu868::connectivity::kEnrollmentUuidBytes;

namespace {

bool good(NTSTATUS status) { return status >= 0; }

bool fromHex(const char* hex, uint8_t* output, size_t outputBytes) {
  if (!hex || !output || strlen(hex) != outputBytes * 2U) return false;
  const auto nibble = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  };
  for (size_t i = 0U; i < outputBytes; ++i) {
    const int high = nibble(hex[i * 2U]);
    const int low = nibble(hex[i * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    output[i] = static_cast<uint8_t>((high << 4U) | low);
  }
  return true;
}

bool fromBase64(const char* encoded, uint8_t* output, size_t capacity,
                size_t& outputBytes) {
  return kitsu868::companion::decodeBase64Url(
      encoded, strlen(encoded), output, capacity, outputBytes);
}

class WindowsHashes final : public CompanionCrypto {
 public:
  bool randomBytes(uint8_t* output, size_t outputBytes) override {
    if (!output && outputBytes != 0U) return false;
    for (size_t i = 0U; i < outputBytes; ++i) {
      state_ = state_ * 1664525UL + 1013904223UL;
      output[i] = static_cast<uint8_t>(state_ >> 24U);
    }
    return true;
  }

  bool sha256(const CryptoPart* parts, size_t partCount,
              uint8_t output[32]) override {
    return hash(nullptr, 0U, parts, partCount, output);
  }

  bool hmacSha256(const uint8_t key[32], const CryptoPart* parts,
                  size_t partCount, uint8_t output[32]) override {
    return hash(key, 32U, parts, partCount, output);
  }

  bool hkdfSha256(const uint8_t inputKey[32], const uint8_t* salt,
                  size_t saltBytes, const uint8_t* info, size_t infoBytes,
                  uint8_t output[32]) override {
    if (!inputKey || (!salt && saltBytes != 0U) ||
        (!info && infoBytes != 0U) || !output) {
      return false;
    }
    uint8_t prk[32]{};
    const CryptoPart extract[] = {CryptoPart(inputKey, 32U)};
    if (!hash(salt, saltBytes, extract, 1U, prk)) return false;
    const uint8_t counter = 1U;
    const CryptoPart expand[] = {CryptoPart(info, infoBytes),
                                 CryptoPart(&counter, 1U)};
    const bool ok = hash(prk, sizeof(prk), expand, 2U, output);
    SecureZeroMemory(prk, sizeof(prk));
    return ok;
  }

 private:
  static bool hash(const uint8_t* key, size_t keyBytes,
                   const CryptoPart* parts, size_t partCount,
                   uint8_t output[32]) {
    if ((!key && keyBytes != 0U) || (!parts && partCount != 0U) || !output ||
        keyBytes > ULONG_MAX || partCount > 32U) {
      return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    const ULONG flags = key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0U;
    if (!good(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                          nullptr, flags))) {
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
          algorithm, &hashHandle, object.data(), objectBytes,
          const_cast<PUCHAR>(key), static_cast<ULONG>(keyBytes), 0U));
    }
    for (size_t i = 0U; ok && i < partCount; ++i) {
      if ((!parts[i].data && parts[i].bytes != 0U) ||
          parts[i].bytes > ULONG_MAX) {
        ok = false;
      } else if (parts[i].bytes != 0U) {
        ok = good(BCryptHashData(
            hashHandle, const_cast<PUCHAR>(parts[i].data),
            static_cast<ULONG>(parts[i].bytes), 0U));
      }
    }
    if (ok) ok = good(BCryptFinishHash(hashHandle, output, 32U, 0U));
    if (hashHandle) BCryptDestroyHash(hashHandle);
    BCryptCloseAlgorithmProvider(algorithm, 0U);
    if (!object.empty()) SecureZeroMemory(object.data(), object.size());
    return ok;
  }

  uint32_t state_ = 0x12345678UL;
};

bool aesGcmOpenWindows(const uint8_t key[32], const uint8_t nonce[12],
                       const uint8_t* aad, size_t aadBytes,
                       const uint8_t* ciphertext, size_t ciphertextBytes,
                       const uint8_t tag[16], uint8_t* plaintext) {
  if (!key || !nonce || !aad || !ciphertext || !tag || !plaintext ||
      aadBytes > ULONG_MAX || ciphertextBytes > ULONG_MAX) {
    return false;
  }
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_KEY_HANDLE keyHandle = nullptr;
  bool ok = good(BCryptOpenAlgorithmProvider(
      &algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0U));
  if (ok) {
    ok = good(BCryptSetProperty(
        algorithm, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
        static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0U));
  }
  DWORD objectBytes = 0U;
  DWORD copied = 0U;
  if (ok) {
    ok = good(BCryptGetProperty(
        algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &copied,
        0U));
  }
  std::vector<uint8_t> object(objectBytes);
  if (ok) {
    ok = good(BCryptGenerateSymmetricKey(
        algorithm, &keyHandle, object.data(), objectBytes,
        const_cast<PUCHAR>(key), 32U, 0U));
  }
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth{};
  BCRYPT_INIT_AUTH_MODE_INFO(auth);
  auth.pbNonce = const_cast<PUCHAR>(nonce);
  auth.cbNonce = 12U;
  auth.pbAuthData = const_cast<PUCHAR>(aad);
  auth.cbAuthData = static_cast<ULONG>(aadBytes);
  auth.pbTag = const_cast<PUCHAR>(tag);
  auth.cbTag = 16U;
  ULONG written = 0U;
  if (ok) {
    ok = good(BCryptDecrypt(
        keyHandle, const_cast<PUCHAR>(ciphertext),
        static_cast<ULONG>(ciphertextBytes), &auth, nullptr, 0U, plaintext,
        static_cast<ULONG>(ciphertextBytes), &written, 0U)) &&
         written == ciphertextBytes;
  }
  if (keyHandle) BCryptDestroyKey(keyHandle);
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0U);
  if (!object.empty()) SecureZeroMemory(object.data(), object.size());
  return ok;
}

class FixturePlatform final : public EnrollmentPlatformCrypto {
 public:
  bool generateP256KeyPair(uint8_t privateKey[32],
                           uint8_t publicKey[65]) override {
    ++generated_;
    memset(privateKey, generated_ == 1U ? 0x11 : 0x22, 32U);
    memset(publicKey, generated_ == 1U ? 0x31 : 0x42, 65U);
    publicKey[0] = 0x04U;
    return true;
  }

  bool createP256CsrDer(const uint8_t[32], const char*, size_t,
                        uint8_t* output, size_t outputCapacity,
                        size_t& outputBytes) override {
    static const uint8_t csr[] = {0x30U, 0x03U, 0x01U, 0x01U, 0x00U};
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

  bool p256Ecdh(const uint8_t privateKey[32],
                 const uint8_t peerPublicKey[65],
                 uint8_t sharedSecret[32]) override {
    if (fixtureMode_) {
      uint8_t expectedPrivate[32]{};
      uint8_t expectedEnc[65]{};
      size_t decoded = 0U;
      if (!fromHex(
              "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
              expectedPrivate, sizeof(expectedPrivate)) ||
          !fromBase64(
              "BMZVnUFt-1avcU8UbZF8JKv4GLL7EhYEEpZJhIIwotJYsqbYLcbGc0zwkv-qn8AS8Q9wCNOVKgjVeX6F_qul2Xc",
              expectedEnc, sizeof(expectedEnc), decoded) ||
          decoded != sizeof(expectedEnc) ||
          memcmp(privateKey, expectedPrivate, sizeof(expectedPrivate)) != 0 ||
          memcmp(peerPublicKey, expectedEnc, sizeof(expectedEnc)) != 0) {
        return false;
      }
      return fromHex(
          "3bf93df2bbc3bbfc82a8afc49bf6025f85a9619bb95617a7f63f72c67306a77c",
          sharedSecret, 32U);
    }
    memset(sharedSecret, 0x5a, 32U);
    return true;
  }

  bool aes256GcmOpen(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t* aad, size_t aadBytes,
                     const uint8_t* ciphertext, size_t ciphertextBytes,
                     const uint8_t tag[16], uint8_t* plaintext) override {
    if (fixtureMode_) {
      return aesGcmOpenWindows(key, nonce, aad, aadBytes, ciphertext,
                               ciphertextBytes, tag, plaintext);
    }
    if (ciphertextBytes != 32U) return false;
    for (size_t i = 0U; i < ciphertextBytes; ++i) {
      plaintext[i] = static_cast<uint8_t>(0x80U + i);
    }
    return true;
  }

  bool certificateBindsKeyAndCompanion(const uint8_t* certificateDer,
                                        size_t certificateBytes,
                                        const uint8_t[65],
                                        const uint8_t[16]) override {
    return certificateValid_ && certificateDer && certificateBytes == 3U;
  }

  bool fixtureMode_ = true;
  bool certificateValid_ = true;

 private:
  uint8_t generated_ = 0U;
};

class CapturingSink final : public EnrollmentCredentialSink {
 public:
  bool commitEnrollmentCredential(const uint8_t companionUuid[16],
                                  const uint8_t gatewayUuid[16],
                                  uint32_t keyVersion,
                                  const uint8_t privateKey[32],
                                  const uint8_t*, size_t,
                                  const uint8_t* const*, const size_t*,
                                  size_t,
                                  const uint8_t secret[32]) override {
    ++calls;
    memcpy(companion, companionUuid, sizeof(companion));
    memcpy(gateway, gatewayUuid, sizeof(gateway));
    memcpy(mtlsPrivate, privateKey, sizeof(mtlsPrivate));
    memcpy(backendSecret, secret, sizeof(backendSecret));
    version = keyVersion;
    return accept;
  }

  bool accept = true;
  unsigned calls = 0U;
  uint32_t version = 0U;
  uint8_t companion[16]{};
  uint8_t gateway[16]{};
  uint8_t mtlsPrivate[32]{};
  uint8_t backendSecret[32]{};
};

void decodeFixture(uint8_t enrollment[16], uint8_t companion[16],
                   uint8_t gateway[16], uint8_t publicKey[65],
                   uint8_t encapsulated[65], uint8_t context[74],
                   uint8_t ciphertext[48]) {
  assert(fromHex("00112233445566778899aabbccddeeff", enrollment, 16U));
  assert(fromHex("102132435465768798a9bacbdcedfe0f", companion, 16U));
  assert(fromHex("f0e0d0c0b0a090807060504030201000", gateway, 16U));
  size_t bytes = 0U;
  assert(fromBase64(
      "BHpZMYCGDEA3yDwSdJhFyO4UJN0pf63LiV41glXSx9KyqMolWA8mJv5XkGL_G5n_kcJKDaBvsytb4gFIySSfVlA",
      publicKey, 65U, bytes));
  assert(bytes == 65U);
  assert(fromBase64(
      "BMZVnUFt-1avcU8UbZF8JKv4GLL7EhYEEpZJhIIwotJYsqbYLcbGc0zwkv-qn8AS8Q9wCNOVKgjVeX6F_qul2Xc",
      encapsulated, 65U, bytes));
  assert(bytes == 65U);
  assert(fromBase64(
      "S0lUU1UtRU5ST0xMLVNFQ1JFVC0xAAARIjNEVWZ3iJmqu8zd7v8QITJDVGV2h5ipusvc7f4P8ODQwLCgkIBwYFBAMCAQAAAAAAE",
      context, 74U, bytes));
  assert(bytes == 74U);
  assert(fromBase64(
      "Tg3SqAwA9XovN-ZG2uw_EuMMh4wlq3ZH-hSygkNjM-yWlSDm8FWsVBG77lmlIBdo",
      ciphertext, 48U, bytes));
  assert(bytes == 48U);
}

void testFrozenContextAndProofTranscript() {
  WindowsHashes hashes;
  uint8_t enrollment[16]{};
  uint8_t companion[16]{};
  uint8_t gateway[16]{};
  uint8_t publicKey[65]{};
  uint8_t encapsulated[65]{};
  uint8_t expectedContext[74]{};
  uint8_t ciphertext[48]{};
  decodeFixture(enrollment, companion, gateway, publicKey, encapsulated,
                expectedContext, ciphertext);
  uint8_t context[kEnrollmentInfoBytes]{};
  assert(kitsu868::connectivity::buildEnrollmentContext(
      enrollment, companion, gateway, 1U, context));
  assert(memcmp(context, expectedContext, sizeof(context)) == 0);

  uint8_t csrHash[32]{};
  uint8_t nonce[kEnrollmentNonceBytes]{};
  uint8_t expectedDigest[32]{};
  assert(fromHex(
      "d1584bafd97b7bcea1aaa537abefaa4051796b9c10fe6d54183e77b16caec69d",
      csrHash, sizeof(csrHash)));
  size_t nonceBytes = 0U;
  assert(fromBase64("gIGCg4SFhoeIiYqLjI2Ojw", nonce, sizeof(nonce),
                    nonceBytes));
  assert(nonceBytes == sizeof(nonce));
  assert(fromHex(
      "473a8019d38ff7f3b392ba3a08caf5fcee76b2110ce9ad628c979893506f155c",
      expectedDigest, sizeof(expectedDigest)));
  uint8_t digest[32]{};
  static const char hardwareUid[] = "KITSU868-TEST-0001";
  assert(kitsu868::connectivity::buildEnrollmentDeviceProofDigest(
      enrollment, hardwareUid, sizeof(hardwareUid) - 1U, csrHash, publicKey,
      nonce, hashes, digest));
  assert(memcmp(digest, expectedDigest, sizeof(digest)) == 0);
}

void testFrozenHpkeRecipientVector() {
  WindowsHashes hashes;
  FixturePlatform platform;
  uint8_t enrollment[16]{};
  uint8_t companion[16]{};
  uint8_t gateway[16]{};
  uint8_t publicKey[65]{};
  uint8_t encapsulated[65]{};
  uint8_t context[74]{};
  uint8_t ciphertext[48]{};
  decodeFixture(enrollment, companion, gateway, publicKey, encapsulated,
                context, ciphertext);
  uint8_t privateKey[32]{};
  assert(fromHex(
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
      privateKey, sizeof(privateKey)));
  uint8_t secret[kEnrollmentSecretBytes]{};
  assert(kitsu868::connectivity::openEnrollmentSecretHpke(
             privateKey, publicKey, encapsulated, context, ciphertext,
             hashes, platform, secret) == EnrollmentResult::Ok);
  uint8_t expected[32]{};
  assert(fromHex(
      "404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f",
      expected, sizeof(expected)));
  assert(memcmp(secret, expected, sizeof(secret)) == 0);

  ciphertext[0] ^= 0x01U;
  memset(secret, 0xa5, sizeof(secret));
  assert(kitsu868::connectivity::openEnrollmentSecretHpke(
             privateKey, publicKey, encapsulated, context, ciphertext,
             hashes, platform, secret) == EnrollmentResult::DecryptionFailed);
  uint8_t zeros[32]{};
  assert(memcmp(secret, zeros, sizeof(secret)) == 0);
}

void fillResponse(EnrollmentResponse& response, const uint8_t enrollment[16],
                  const uint8_t companion[16], const uint8_t gateway[16]) {
  memcpy(response.enrollmentUuid, enrollment, 16U);
  memcpy(response.companionUuid, companion, 16U);
  memcpy(response.gatewayUuid, gateway, 16U);
  response.keyVersion = 7U;
  static const uint8_t certificate[] = {0x30U, 0x01U, 0x00U};
  response.certificateDer = certificate;
  response.certificateBytes = sizeof(certificate);
  memset(response.encapsulatedKey, 0x44, sizeof(response.encapsulatedKey));
  response.encapsulatedKey[0] = 0x04U;
  memset(response.ciphertext, 0x55, sizeof(response.ciphertext));
}

void testEnrollmentAuthorizationAndAtomicCommit() {
  WindowsHashes hashes;
  FixturePlatform platform;
  platform.fixtureMode_ = false;
  KitsuEnrollmentRecipient recipient;
  uint8_t enrollment[16]{};
  uint8_t companion[16]{};
  uint8_t gateway[16]{};
  for (size_t i = 0U; i < 16U; ++i) {
    enrollment[i] = static_cast<uint8_t>(i + 1U);
    companion[i] = static_cast<uint8_t>(i + 0x21U);
    gateway[i] = static_cast<uint8_t>(i + 0x41U);
  }
  static const char uid[] = "KITSU868-TEST-0001";
  static const char claim[] = "one.use_claim-token";
  assert(recipient.begin(enrollment, gateway, uid, sizeof(uid) - 1U, claim,
                         sizeof(claim) - 1U, true, true, false, hashes,
                         platform) ==
         EnrollmentResult::RemoteConnectivityUnavailable);
  assert(!recipient.active());
  assert(recipient.begin(enrollment, gateway, uid, sizeof(uid) - 1U, claim,
                         sizeof(claim) - 1U, true, false, true, hashes,
                         platform) == EnrollmentResult::AuthorizationRequired);
  assert(!recipient.active());
  assert(recipient.begin(enrollment, gateway, uid, sizeof(uid) - 1U, claim,
                         sizeof(claim) - 1U, true, true, true, hashes,
                         platform) == EnrollmentResult::Ok);
  assert(recipient.active());

  uint8_t request[8192]{};
  size_t requestBytes = 0U;
  assert(recipient.buildRequestJson(request, sizeof(request), requestBytes) ==
         EnrollmentResult::Ok);
  const std::string json(reinterpret_cast<const char*>(request), requestBytes);
  assert(json.find("\"claim_token\":\"one.use_claim-token\"") !=
         std::string::npos);
  assert(json.find("\"hardware_uid\":\"KITSU868-TEST-0001\"") !=
         std::string::npos);
  assert(json.find("\"device_csr_der_b64\"") != std::string::npos);
  assert(json.find("\"hpke_recipient_b64\"") != std::string::npos);
  assert(json.find("\"device_nonce_b64\"") != std::string::npos);
  assert(json.find("\"device_proof_b64\"") != std::string::npos);

  EnrollmentResponse response{};
  fillResponse(response, enrollment, companion, gateway);
  CapturingSink sink;
  assert(sink.calls == 0U);
  assert(recipient.finish(response, sink) == EnrollmentResult::Ok);
  assert(!recipient.active());
  assert(sink.calls == 1U && sink.version == 7U);
  for (size_t i = 0U; i < 32U; ++i) {
    assert(sink.mtlsPrivate[i] == 0x11U);
    assert(sink.backendSecret[i] == static_cast<uint8_t>(0x80U + i));
  }

  assert(recipient.begin(enrollment, gateway, uid, sizeof(uid) - 1U, claim,
                         sizeof(claim) - 1U, true, true, true, hashes,
                         platform) == EnrollmentResult::Ok);
  response.gatewayUuid[0] ^= 0x01U;
  assert(recipient.finish(response, sink) ==
         EnrollmentResult::ResponseMismatch);
  assert(!recipient.active());
  assert(sink.calls == 1U);
}

}  // namespace

int main() {
  testFrozenContextAndProofTranscript();
  testFrozenHpkeRecipientVector();
  testEnrollmentAuthorizationAndAtomicCommit();
  return 0;
}
