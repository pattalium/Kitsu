#include "../src/kitsu_lan_protocol.h"

#include <assert.h>
#include <windows.h>
#include <bcrypt.h>

#include <string.h>

#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

using kitsu868::companion::CompanionCrypto;
using kitsu868::companion::CryptoPart;
using kitsu868::connectivity::LanActionReplayStore;
using kitsu868::connectivity::LanFrameKind;
using kitsu868::connectivity::LanGatewayFrame;
using kitsu868::connectivity::LanReplayDecision;
using kitsu868::connectivity::LanResult;

namespace {

bool good(NTSTATUS status) { return status >= 0; }

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

  bool sha256(const CryptoPart* parts, size_t count,
              uint8_t output[32]) override {
    return hash(nullptr, 0U, parts, count, output);
  }

  bool hmacSha256(const uint8_t key[32], const CryptoPart* parts,
                  size_t count, uint8_t output[32]) override {
    return hash(key, 32U, parts, count, output);
  }

  bool hkdfSha256(const uint8_t inputKey[32], const uint8_t* salt,
                  size_t saltBytes, const uint8_t* info, size_t infoBytes,
                  uint8_t output[32]) override {
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
                   const CryptoPart* parts, size_t count,
                   uint8_t output[32]) {
    if ((!key && keyBytes != 0U) || (!parts && count != 0U) || !output ||
        keyBytes > ULONG_MAX) {
      return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE handle = nullptr;
    if (!good(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
            key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0U))) {
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
          algorithm, &handle, object.data(), objectBytes,
          const_cast<PUCHAR>(key), static_cast<ULONG>(keyBytes), 0U));
    }
    for (size_t i = 0U; ok && i < count; ++i) {
      if ((!parts[i].data && parts[i].bytes != 0U) ||
          parts[i].bytes > ULONG_MAX) {
        ok = false;
      } else if (parts[i].bytes != 0U) {
        ok = good(BCryptHashData(handle,
                                 const_cast<PUCHAR>(parts[i].data),
                                 static_cast<ULONG>(parts[i].bytes), 0U));
      }
    }
    if (ok) ok = good(BCryptFinishHash(handle, output, 32U, 0U));
    if (handle) BCryptDestroyHash(handle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0U);
    if (!object.empty()) SecureZeroMemory(object.data(), object.size());
    return ok;
  }

  uint32_t state_ = 7U;
};

bool fromHex(const char* input, uint8_t* output, size_t outputBytes) {
  if (!input || !output || strlen(input) != outputBytes * 2U) return false;
  const auto nibble = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
  };
  for (size_t i = 0U; i < outputBytes; ++i) {
    const int high = nibble(input[i * 2U]);
    const int low = nibble(input[i * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    output[i] = static_cast<uint8_t>((high << 4U) | low);
  }
  return true;
}

class MemoryReplay final : public LanActionReplayStore {
 public:
  LanReplayDecision acceptAction(const uint8_t actionId[16], int64_t,
                                 int64_t) override {
    ++calls;
    if (fail) return LanReplayDecision::Failed;
    if (seen && memcmp(id, actionId, sizeof(id)) == 0) {
      return LanReplayDecision::Duplicate;
    }
    memcpy(id, actionId, sizeof(id));
    seen = true;
    return LanReplayDecision::Fresh;
  }

  unsigned calls = 0U;
  bool seen = false;
  bool fail = false;
  uint8_t id[16]{};
};

std::string actionJson() {
  return "{\"schema\":\"kitsu.remote-action.v1\"," 
         "\"action_id\":\"00112233-4455-6677-8899-aabbccddeeff\"," 
         "\"companion_id\":\"ffeeddcc-bbaa-9988-7766-554433221100\"," 
         "\"key_version\":16909060," 
         "\"nonce_b64\":\"AAECAwQFBgcICQoLDA0ODw\"," 
         "\"action_type\":\"companion.pet\"," 
         "\"created_epoch\":\"1800000000\"," 
         "\"expires_epoch\":\"1800000060\"," 
         "\"params_b64\":\"eyJnZXN0dXJlIjoiZWFyLXNjcmF0Y2gifQ\"," 
         "\"signature_b64\":\"Ba24Hq65ANHWNZ3ZDYkfhVQ1KorRFzfLgmxPEBTNdQ4\"}";
}

LanResult decode(const std::string& json, MemoryReplay& replay,
                 LanGatewayFrame& output, uint8_t params[256],
                 bool clockValid = true, int64_t now = 1800000030LL) {
  WindowsHashes hashes;
  uint8_t companion[16]{};
  uint8_t secret[32]{};
  assert(fromHex("ffeeddccbbaa99887766554433221100", companion,
                 sizeof(companion)));
  for (size_t i = 0U; i < sizeof(secret); ++i) {
    secret[i] = static_cast<uint8_t>(i);
  }
  return kitsu868::connectivity::decodeGatewayFrame(
      reinterpret_cast<const uint8_t*>(json.data()), json.size(), companion,
      0x01020304UL, now, clockValid, secret, hashes, replay, output, params,
      256U);
}

void testFrozenRemoteActionVectorAndReplay() {
  MemoryReplay replay;
  LanGatewayFrame output{};
  uint8_t params[256]{};
  assert(decode(actionJson(), replay, output, params) ==
         LanResult::ActionFresh);
  assert(output.kind == LanFrameKind::RemoteAction);
  assert(strcmp(output.actionType, "companion.pet") == 0);
  assert(output.keyVersion == 0x01020304UL);
  static const char expected[] = "{\"gesture\":\"ear-scratch\"}";
  assert(output.parameterBytes == sizeof(expected) - 1U);
  assert(memcmp(params, expected, sizeof(expected) - 1U) == 0);
  assert(replay.calls == 1U);

  memset(params, 0, sizeof(params));
  assert(decode(actionJson(), replay, output, params) ==
         LanResult::ActionDuplicate);
  assert(replay.calls == 2U);
  assert(output.parameterBytes == sizeof(expected) - 1U);
}

void testRemoteActionRejectsBeforeReplay() {
  MemoryReplay replay;
  LanGatewayFrame output{};
  uint8_t params[256]{};
  std::string tampered = actionJson();
  const size_t signature = tampered.find("Ba24");
  assert(signature != std::string::npos);
  tampered[signature] = 'C';
  assert(decode(tampered, replay, output, params) ==
         LanResult::AuthenticationFailed);
  assert(replay.calls == 0U);

  const std::string duplicate =
      actionJson().replace(actionJson().find("\"action_id\":"), 0U,
                           "\"schema\":\"duplicate\",");
  assert(decode(duplicate, replay, output, params) ==
         LanResult::DuplicateField);
  assert(replay.calls == 0U);

  assert(decode(actionJson(), replay, output, params, false) ==
         LanResult::ClockRequired);
  assert(decode(actionJson(), replay, output, params, true, 1800000061LL) ==
         LanResult::Expired);
  assert(replay.calls == 0U);
}

void testInterleavedGatewayAck() {
  MemoryReplay replay;
  LanGatewayFrame output{};
  uint8_t params[256]{};
  const std::string ack =
      "{\"device_sequence\":\"42\",\"type\":\"gateway_ack\"," 
      "\"spool_record_id\":\"9001\",\"v\":1}";
  assert(decode(ack, replay, output, params) == LanResult::GatewayAck);
  assert(output.kind == LanFrameKind::GatewayAck);
  assert(output.spoolRecordId == 9001U && output.deviceSequence == 42U);
  assert(replay.calls == 0U);
  assert(decode(actionJson(), replay, output, params) ==
         LanResult::ActionFresh);
}

void testFlatDeviceEnvelopeShape() {
  WindowsHashes hashes;
  uint8_t companion[16]{};
  uint8_t gateway[16]{};
  uint8_t request[16]{};
  uint8_t nonce[16]{};
  uint8_t secret[32]{};
  assert(fromHex("00112233445566778899aabbccddeeff", companion, 16U));
  assert(fromHex("ffeeddccbbaa99887766554433221100", gateway, 16U));
  request[15] = 1U;
  memset(nonce, 0x11, sizeof(nonce));
  for (size_t i = 0U; i < sizeof(secret); ++i) {
    secret[i] = static_cast<uint8_t>(i);
  }
  static const uint8_t payload[] =
      "{\"type\":\"heartbeat\",\"uptime_ms\":7}";
  uint8_t json[2048]{};
  size_t jsonBytes = 0U;
  assert(kitsu868::connectivity::encodeDeviceEnvelope(
             companion, gateway, 42U, 0, nonce, request, 1U, "heartbeat",
             payload, sizeof(payload) - 1U, secret, hashes, json,
             sizeof(json), jsonBytes) == LanResult::Ok);
  const std::string encoded(reinterpret_cast<const char*>(json), jsonBytes);
  assert(encoded.find("\"schema\":\"kitsu.device-envelope.v1\"") !=
         std::string::npos);
  assert(encoded.find("\"sequence\":\"42\"") != std::string::npos);
  assert(encoded.find("\"issued_epoch\":\"0\"") != std::string::npos);
  assert(encoded.find("\"key_version\":1") != std::string::npos);
  assert(encoded.find("\"payload_type\":\"heartbeat\"") !=
         std::string::npos);
  assert(encoded.find("\"proof\"") == std::string::npos);
  assert(encoded.find("\"algorithm\"") == std::string::npos);
  assert(encoded.find("\"signature_b64\":\"") != std::string::npos);
}

}  // namespace

int main() {
  testFrozenRemoteActionVectorAndReplay();
  testRemoteActionRejectsBeforeReplay();
  testInterleavedGatewayAck();
  testFlatDeviceEnvelopeShape();
  return 0;
}
