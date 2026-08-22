#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kitsu868 {
namespace companion {

constexpr uint8_t kProtocolVersion = 1U;
constexpr size_t kFrameHeaderBytes = 4U;
constexpr size_t kMaximumFrameBytes = 16U * 1024U;
constexpr size_t kMaximumHandshakeFrameBytes = 1024U;
constexpr uint32_t kFrameAssemblyTimeoutMs = 10000UL;
constexpr size_t kEnvelopeNonceBytes = 16U;
constexpr size_t kRequestIdBytes = 16U;
constexpr size_t kEnvelopeMacBytes = 32U;
constexpr size_t kEnvelopeKeyBytes = 32U;
constexpr size_t kMaximumOperationBytes = 48U;
constexpr size_t kMaximumEnvelopePayloadBytes = 12000U;
constexpr size_t kMaximumCryptoParts = 12U;

enum class FrameResult : uint8_t {
  NeedMore = 0,
  Ready,
  InvalidArgument,
  EmptyFrame,
  Oversize,
  TimedOut,
  PipelinedFrame,
  OutputTooSmall,
};

const char* frameResultName(FrameResult result);

// Caller-owned storage keeps the 16 KiB maximum frame off task stacks and lets
// BLE use a smaller 1 KiB buffer until authentication completes.
class LengthFrameParser {
 public:
  LengthFrameParser();
  bool begin(uint8_t* storage, size_t storageBytes, size_t maximumFrameBytes,
             uint32_t timeoutMs = kFrameAssemblyTimeoutMs);
  FrameResult feed(const uint8_t* input, size_t inputBytes,
                   uint32_t nowMillis);
  FrameResult poll(uint32_t nowMillis);
  bool frame(const uint8_t*& output, size_t& outputBytes) const;
  void consume();
  void reset();

 private:
  uint8_t* storage_ = nullptr;
  size_t storageBytes_ = 0U;
  size_t maximumFrameBytes_ = 0U;
  uint32_t timeoutMs_ = 0U;
  uint32_t startedAt_ = 0U;
  uint8_t header_[kFrameHeaderBytes]{};
  uint8_t headerBytes_ = 0U;
  size_t expectedBytes_ = 0U;
  size_t receivedBytes_ = 0U;
  bool started_ = false;
  bool ready_ = false;
};

FrameResult encodeLengthFrame(const uint8_t* payload, size_t payloadBytes,
                              uint8_t* output, size_t outputCapacity,
                              size_t& outputBytes);

struct CryptoPart {
  constexpr CryptoPart(const uint8_t* input = nullptr,
                       size_t inputBytes = 0U)
      : data(input), bytes(inputBytes) {}

  const uint8_t* data;
  size_t bytes;
};

class CompanionCrypto {
 public:
  virtual ~CompanionCrypto() = default;
  virtual bool randomBytes(uint8_t* output, size_t outputBytes) = 0;
  virtual bool sha256(const CryptoPart* parts, size_t partCount,
                      uint8_t output[32]) = 0;
  virtual bool hmacSha256(const uint8_t key[kEnvelopeKeyBytes],
                          const CryptoPart* parts, size_t partCount,
                          uint8_t output[kEnvelopeMacBytes]) = 0;
  virtual bool hkdfSha256(const uint8_t inputKey[kEnvelopeKeyBytes],
                          const uint8_t* salt, size_t saltBytes,
                          const uint8_t* info, size_t infoBytes,
                          uint8_t output[kEnvelopeKeyBytes]) = 0;
};

enum class EnvelopeChannel : uint8_t {
  Request = 0,
  Response = 1,
  Event = 2,
};

enum class ProtocolResult : uint8_t {
  Ok = 0,
  InvalidArgument,
  OutputTooSmall,
  MalformedJson,
  DuplicateField,
  UnknownField,
  MissingField,
  UnsupportedVersion,
  InvalidChannel,
  InvalidSequence,
  UnexpectedSequence,
  InvalidBase64,
  InvalidOperation,
  PayloadTooLarge,
  AuthenticationFailed,
  InvalidPayloadUtf8,
  InvalidPayloadJson,
  CryptoFailed,
};

const char* protocolResultName(ProtocolResult result);

struct DecodedEnvelope {
  uint8_t version = 0U;
  EnvelopeChannel channel = EnvelopeChannel::Request;
  uint64_t sequence = 0U;
  uint8_t nonce[kEnvelopeNonceBytes]{};
  uint8_t requestId[kRequestIdBytes]{};
  char operation[kMaximumOperationBytes + 1U]{};
  size_t payloadBytes = 0U;
};

// JSON field order is fixed on encode but not required on decode.  Decode
// rejects unknown/duplicate fields and authenticates the exact decoded
// payload bytes before validating or exposing inner UTF-8 JSON.
ProtocolResult encodeEnvelope(
    EnvelopeChannel channel, uint64_t sequence,
    const uint8_t nonce[kEnvelopeNonceBytes],
    const uint8_t requestId[kRequestIdBytes], const char* operation,
    const uint8_t* payload, size_t payloadBytes,
    const uint8_t key[kEnvelopeKeyBytes], CompanionCrypto& crypto,
    uint8_t* outputJson, size_t outputCapacity, size_t& outputBytes);

// The caller supplies the exact next per-direction sequence, starting at one.
ProtocolResult decodeAndVerifyEnvelope(
    const uint8_t* json, size_t jsonBytes,
    const uint8_t key[kEnvelopeKeyBytes], EnvelopeChannel expectedChannel,
    uint64_t expectedSequence, CompanionCrypto& crypto,
    DecodedEnvelope& output, uint8_t* payloadOutput,
    size_t payloadCapacity);

size_t base64UrlEncodedBytes(size_t inputBytes);
bool encodeBase64Url(const uint8_t* input, size_t inputBytes, char* output,
                     size_t outputCapacity, size_t& outputBytes);
bool decodeBase64Url(const char* input, size_t inputBytes, uint8_t* output,
                     size_t outputCapacity, size_t& outputBytes);

bool validUtf8(const uint8_t* input, size_t inputBytes);

// Shared transcript helpers for the frozen BLE controller handshake.  The
// controller root proves roles, then derives independent per-connection keys;
// it is never used directly as an envelope MAC key.
ProtocolResult makeHandshakeProof(
    const uint8_t root[kEnvelopeKeyBytes], const char* role,
    const uint8_t controllerId[16], const uint8_t clientNonce[16],
    const uint8_t deviceNonce[16], CompanionCrypto& crypto,
    uint8_t output[kEnvelopeMacBytes]);

ProtocolResult deriveBleSessionKeys(
    const uint8_t root[kEnvelopeKeyBytes], const uint8_t clientNonce[16],
    const uint8_t deviceNonce[16], CompanionCrypto& crypto,
    uint8_t clientToDevice[kEnvelopeKeyBytes],
    uint8_t deviceToClient[kEnvelopeKeyBytes]);

ProtocolResult makePairingProof(
    const uint8_t root[kEnvelopeKeyBytes], const char* role,
    const uint8_t controllerId[16], const char deviceUid[7],
    const uint8_t clientNonce[16], const uint8_t deviceNonce[16],
    CompanionCrypto& crypto, uint8_t output[kEnvelopeMacBytes]);

}  // namespace companion
}  // namespace kitsu868
