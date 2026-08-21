#include "../src/kitsu_companion_protocol.h"

#include <assert.h>
#include <windows.h>
#include <bcrypt.h>
#include <string.h>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

using kitsu868::companion::CompanionCrypto;
using kitsu868::companion::CryptoPart;
using kitsu868::companion::DecodedEnvelope;
using kitsu868::companion::EnvelopeChannel;
using kitsu868::companion::FrameResult;
using kitsu868::companion::LengthFrameParser;
using kitsu868::companion::ProtocolResult;

namespace {

class WindowsCrypto final : public CompanionCrypto {
 public:
  bool randomBytes(uint8_t* output, size_t outputBytes) override {
    if (!output && outputBytes != 0U) return false;
    for (size_t i = 0U; i < outputBytes; ++i) {
      randomState = randomState * 1664525UL + 1013904223UL;
      output[i] = static_cast<uint8_t>(randomState >> 24U);
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
    if (!inputKey || !salt || !info || !output) return false;
    uint8_t prk[32]{};
    const CryptoPart extract[] = {{inputKey, 32U}};
    if (!hash(salt, saltBytes, extract, 1U, prk)) return false;
    const uint8_t counter = 1U;
    const CryptoPart expand[] = {{info, infoBytes}, {&counter, 1U}};
    const bool ok = hash(prk, sizeof(prk), expand, 2U, output);
    SecureZeroMemory(prk, sizeof(prk));
    return ok;
  }

  uint32_t randomState = 1U;

 private:
  static bool good(NTSTATUS status) { return status >= 0; }

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
        break;
      }
      if (parts[i].bytes != 0U) {
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
};

bool hexEquals(const uint8_t* input, size_t inputBytes, const char* hex) {
  if (!input || !hex || strlen(hex) != inputBytes * 2U) return false;
  for (size_t i = 0U; i < inputBytes; ++i) {
    const auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int high = nibble(hex[i * 2U]);
    const int low = nibble(hex[i * 2U + 1U]);
    if (high < 0 || low < 0 ||
        input[i] != static_cast<uint8_t>((high << 4U) | low)) {
      return false;
    }
  }
  return true;
}

void insertBytes(const uint8_t* input, size_t inputBytes, size_t at,
                 const char* inserted, uint8_t* output,
                 size_t& outputBytes) {
  assert(at <= inputBytes);
  const size_t insertedBytes = strlen(inserted);
  memcpy(output, input, at);
  memcpy(output + at, inserted, insertedBytes);
  memcpy(output + at + insertedBytes, input + at, inputBytes - at);
  outputBytes = inputBytes + insertedBytes;
  output[outputBytes] = 0U;
}

void testBase64Canonical() {
  uint8_t source[32]{};
  for (size_t i = 0U; i < sizeof(source); ++i) {
    source[i] = static_cast<uint8_t>(i);
  }
  char encoded[64]{};
  size_t encodedBytes = 0U;
  assert(kitsu868::companion::encodeBase64Url(
      source, sizeof(source), encoded, sizeof(encoded), encodedBytes));
  assert(encodedBytes == 43U);
  assert(strncmp(encoded, "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8",
                 encodedBytes) == 0);
  uint8_t decoded[32]{};
  size_t decodedBytes = 0U;
  assert(kitsu868::companion::decodeBase64Url(
      encoded, encodedBytes, decoded, sizeof(decoded), decodedBytes));
  assert(decodedBytes == sizeof(decoded));
  assert(memcmp(source, decoded, sizeof(source)) == 0);
  assert(!kitsu868::companion::decodeBase64Url(
      "_x", 2U, decoded, sizeof(decoded), decodedBytes));
  assert(!kitsu868::companion::decodeBase64Url(
      "AA=", 3U, decoded, sizeof(decoded), decodedBytes));
}

void testLengthFraming() {
  const uint8_t message[] = {'h', 'e', 'l', 'l', 'o'};
  uint8_t framed[32]{};
  size_t framedBytes = 0U;
  assert(kitsu868::companion::encodeLengthFrame(
             message, sizeof(message), framed, sizeof(framed), framedBytes) ==
         FrameResult::Ready);
  assert(framedBytes == 9U && framed[3] == 5U);

  uint8_t storage[8]{};
  LengthFrameParser parser;
  assert(parser.begin(storage, sizeof(storage), sizeof(storage), 100U));
  assert(parser.feed(framed, 1U, 1000U) == FrameResult::NeedMore);
  assert(parser.feed(framed + 1U, 4U, 1001U) == FrameResult::NeedMore);
  assert(parser.feed(framed + 5U, framedBytes - 5U, 1002U) ==
         FrameResult::Ready);
  const uint8_t* output = nullptr;
  size_t outputBytes = 0U;
  assert(parser.frame(output, outputBytes));
  assert(outputBytes == sizeof(message));
  assert(memcmp(output, message, sizeof(message)) == 0);
  assert(parser.feed(message, 1U, 1003U) == FrameResult::PipelinedFrame);

  parser.consume();
  uint8_t combined[16]{};
  memcpy(combined, framed, framedBytes);
  combined[framedBytes] = 0U;
  assert(parser.feed(combined, framedBytes + 1U, 2000U) ==
         FrameResult::PipelinedFrame);

  const uint8_t oversize[] = {0U, 0U, 0U, 9U};
  assert(parser.feed(oversize, sizeof(oversize), 3000U) ==
         FrameResult::Oversize);
  const uint8_t empty[] = {0U, 0U, 0U, 0U};
  assert(parser.feed(empty, sizeof(empty), 3001U) == FrameResult::EmptyFrame);
  assert(parser.feed(framed, 2U, 0xfffffff0UL) == FrameResult::NeedMore);
  assert(parser.poll(0x00000060UL) == FrameResult::TimedOut);
}

void testEnvelopeRoundTripAndStrictOuterParser() {
  WindowsCrypto crypto;
  uint8_t key[32]{};
  uint8_t nonce[16]{};
  uint8_t requestId[16]{};
  for (size_t i = 0U; i < sizeof(key); ++i) key[i] = static_cast<uint8_t>(i);
  for (size_t i = 0U; i < sizeof(nonce); ++i) {
    nonce[i] = static_cast<uint8_t>(0x10U + i);
    requestId[i] = static_cast<uint8_t>(0x80U + i);
  }
  const uint8_t payload[] = "{\"action\":\"pet\",\"amount\":1}";
  uint8_t json[2048]{};
  size_t jsonBytes = 0U;
  assert(kitsu868::companion::encodeEnvelope(
             EnvelopeChannel::Request, 1U, nonce, requestId, "action.apply",
             payload, sizeof(payload) - 1U, key, crypto, json, sizeof(json),
             jsonBytes) == ProtocolResult::Ok);
  assert(jsonBytes < sizeof(json));
  json[jsonBytes] = 0U;
  static const char expectedPrefix[] =
      "{\"v\":1,\"channel\":0,\"seq\":\"1\"";
  assert(strncmp(reinterpret_cast<const char*>(json), expectedPrefix,
                 sizeof(expectedPrefix) - 1U) == 0);

  uint8_t decodedPayload[sizeof(payload) - 1U]{};
  DecodedEnvelope decoded{};
  assert(kitsu868::companion::decodeAndVerifyEnvelope(
             json, jsonBytes, key, EnvelopeChannel::Request, 1U, crypto,
             decoded, decodedPayload, sizeof(decodedPayload)) ==
         ProtocolResult::Ok);
  assert(decoded.sequence == 1U);
  assert(strcmp(decoded.operation, "action.apply") == 0);
  assert(decoded.payloadBytes == sizeof(decodedPayload));
  assert(memcmp(decodedPayload, payload, sizeof(decodedPayload)) == 0);
  assert(kitsu868::companion::decodeAndVerifyEnvelope(
             json, jsonBytes, key, EnvelopeChannel::Request, 2U, crypto,
             decoded, decodedPayload, sizeof(decodedPayload)) ==
         ProtocolResult::UnexpectedSequence);

  uint8_t mutated[2300]{};
  memcpy(mutated, json, jsonBytes + 1U);
  char* payloadText = strstr(reinterpret_cast<char*>(mutated),
                             "\"payload_b64\":\"");
  assert(payloadText);
  payloadText += strlen("\"payload_b64\":\"");
  payloadText[0] = payloadText[0] == 'e' ? 'f' : 'e';
  assert(kitsu868::companion::decodeAndVerifyEnvelope(
             mutated, jsonBytes, key, EnvelopeChannel::Request, 1U, crypto,
             decoded, decodedPayload, sizeof(decodedPayload)) ==
         ProtocolResult::AuthenticationFailed);

  size_t mutationBytes = 0U;
  insertBytes(json, jsonBytes, 1U, "\"v\":1,", mutated, mutationBytes);
  assert(kitsu868::companion::decodeAndVerifyEnvelope(
             mutated, mutationBytes, key, EnvelopeChannel::Request, 1U,
             crypto, decoded, decodedPayload, sizeof(decodedPayload)) ==
         ProtocolResult::DuplicateField);
  insertBytes(json, jsonBytes, 1U, "\"unknown\":1,", mutated,
              mutationBytes);
  assert(kitsu868::companion::decodeAndVerifyEnvelope(
             mutated, mutationBytes, key, EnvelopeChannel::Request, 1U,
             crypto, decoded, decodedPayload, sizeof(decodedPayload)) ==
         ProtocolResult::UnknownField);

  const char* nonceField = strstr(reinterpret_cast<const char*>(json),
                                  "\"nonce_b64\":\"");
  assert(nonceField);
  const size_t nonceEnd = static_cast<size_t>(
      nonceField - reinterpret_cast<const char*>(json)) +
      strlen("\"nonce_b64\":\"") + 22U;
  insertBytes(json, jsonBytes, nonceEnd, "=", mutated, mutationBytes);
  assert(kitsu868::companion::decodeAndVerifyEnvelope(
             mutated, mutationBytes, key, EnvelopeChannel::Request, 1U,
             crypto, decoded, decodedPayload, sizeof(decodedPayload)) ==
         ProtocolResult::InvalidBase64);

  const char* seqField = strstr(reinterpret_cast<const char*>(json),
                                "\"seq\":\"");
  assert(seqField);
  const size_t seqStart = static_cast<size_t>(
      seqField - reinterpret_cast<const char*>(json)) + strlen("\"seq\":\"");
  insertBytes(json, jsonBytes, seqStart, "0", mutated, mutationBytes);
  assert(kitsu868::companion::decodeAndVerifyEnvelope(
             mutated, mutationBytes, key, EnvelopeChannel::Request, 0U,
             crypto, decoded, decodedPayload, sizeof(decodedPayload)) ==
         ProtocolResult::InvalidSequence);
}

void testPayloadValidationAndOutputBounds() {
  WindowsCrypto crypto;
  uint8_t key[32]{};
  uint8_t nonce[16]{};
  uint8_t requestId[16]{};
  const uint8_t malformed[] = "{\"x\":1,}";
  uint8_t output[512]{};
  size_t outputBytes = 0U;
  assert(kitsu868::companion::encodeEnvelope(
             EnvelopeChannel::Event, 1U, nonce, requestId, "state.changed",
             malformed, sizeof(malformed) - 1U, key, crypto, output,
             sizeof(output), outputBytes) == ProtocolResult::InvalidPayloadJson);
  const uint8_t valid[] = "{\"text\":\"fox \\uD83E\\uDD8A\",\"v\":[true,null,-1.2e3]}";
  assert(kitsu868::companion::encodeEnvelope(
             EnvelopeChannel::Event, 1U, nonce, requestId, "state.changed",
             valid, sizeof(valid) - 1U, key, crypto, output, 10U,
             outputBytes) == ProtocolResult::OutputTooSmall);
}

void testHandshakeAndPairingVectors() {
  WindowsCrypto crypto;
  uint8_t root[32]{};
  uint8_t controllerId[16]{};
  uint8_t clientNonce[16]{};
  uint8_t deviceNonce[16]{};
  const char deviceUid[7] = "KT1234";
  for (size_t i = 0U; i < sizeof(root); ++i) root[i] = static_cast<uint8_t>(i);
  for (size_t i = 0U; i < 16U; ++i) {
    controllerId[i] = static_cast<uint8_t>(i);
    clientNonce[i] = static_cast<uint8_t>(0x10U + i);
    deviceNonce[i] = static_cast<uint8_t>(0x20U + i);
  }
  uint8_t proof[32]{};
  assert(kitsu868::companion::makeHandshakeProof(
             root, "device", controllerId, clientNonce, deviceNonce, crypto,
             proof) == ProtocolResult::Ok);
  assert(hexEquals(proof, sizeof(proof),
                   "34dcf7054c7935d925cdd3f8c22299ef98f3cd244e43ce4588152269d1eb880b"));
  uint8_t clientProof[32]{};
  assert(kitsu868::companion::makeHandshakeProof(
             root, "client", controllerId, clientNonce, deviceNonce, crypto,
             clientProof) == ProtocolResult::Ok);
  assert(memcmp(proof, clientProof, sizeof(proof)) != 0);

  uint8_t c2d[32]{};
  uint8_t d2c[32]{};
  assert(kitsu868::companion::deriveBleSessionKeys(
             root, clientNonce, deviceNonce, crypto, c2d, d2c) ==
         ProtocolResult::Ok);
  assert(hexEquals(c2d, sizeof(c2d),
                   "971531dd7d624b181d0e1ba51ef0efde287819b5dff959c7b705d6c98552e6f8"));
  assert(hexEquals(d2c, sizeof(d2c),
                   "fb50c7d6e67d285b0b542542c395a6e9da93d31c5ebdf6549fd3bd2fe6ceada6"));
  assert(memcmp(c2d, d2c, sizeof(c2d)) != 0);

  uint8_t pairDevice[32]{};
  uint8_t pairClient[32]{};
  assert(kitsu868::companion::makePairingProof(
             root, "device", controllerId, deviceUid, clientNonce, deviceNonce,
             crypto, pairDevice) == ProtocolResult::Ok);
  assert(hexEquals(pairDevice, sizeof(pairDevice),
                   "fe29b40817a936ad8c66145656e7c1bd92e1e6588002058141b00339d0cc5c0e"));
  assert(kitsu868::companion::makePairingProof(
             root, "client", controllerId, deviceUid, clientNonce, deviceNonce,
             crypto, pairClient) == ProtocolResult::Ok);
  assert(hexEquals(pairClient, sizeof(pairClient),
                   "70e68e6965dde64a1f444e0c92990fac568e6c8e64e8e8e94b18ea160e2e8df7"));
  assert(memcmp(pairDevice, pairClient, sizeof(pairDevice)) != 0);
  assert(kitsu868::companion::makePairingProof(
             root, "device", controllerId, "kt1234", clientNonce,
             deviceNonce, crypto, pairDevice) ==
         ProtocolResult::InvalidArgument);
}

}  // namespace

int main() {
  testBase64Canonical();
  testLengthFraming();
  testEnvelopeRoundTripAndStrictOuterParser();
  testPayloadValidationAndOutputBounds();
  testHandshakeAndPairingVectors();
  return 0;
}
