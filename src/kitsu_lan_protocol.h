#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_companion_protocol.h"

namespace kitsu868 {
namespace connectivity {

constexpr size_t kLanUuidBytes = 16U;
constexpr size_t kLanNonceBytes = 16U;
constexpr size_t kLanSignatureBytes = 32U;
constexpr size_t kLanMaximumFrameBytes = 16U * 1024U;
constexpr size_t kLanMaximumActionTypeBytes = 64U;
constexpr size_t kLanMaximumPayloadTypeBytes = 64U;
constexpr size_t kLanMaximumActionParamsBytes = 12000U;
constexpr size_t kLanMaximumDevicePayloadBytes = 12000U;
constexpr int64_t kLanMinimumKnownEpoch = 1577836800LL;
constexpr int64_t kLanMaximumKnownEpoch = 4102444800LL;

enum class LanResult : uint8_t {
  Ok = 0,
  GatewayAck,
  ActionFresh,
  ActionDuplicate,
  InvalidArgument,
  MalformedJson,
  DuplicateField,
  UnknownField,
  UnsupportedSchema,
  InvalidIdentity,
  InvalidEncoding,
  InvalidSequence,
  WrongCompanion,
  WrongKeyVersion,
  ClockRequired,
  Expired,
  AuthenticationFailed,
  UnsupportedAction,
  ReplayStoreFailed,
  OutputTooSmall,
  CryptoFailed,
};

const char* lanResultName(LanResult result);

enum class LanFrameKind : uint8_t { GatewayAck = 0, RemoteAction };
enum class LanReplayDecision : uint8_t { Fresh = 0, Duplicate, Failed };

class LanActionReplayStore {
 public:
  virtual ~LanActionReplayStore() = default;

  // Called only after identity, key version, expiry, and HMAC verification.
  // Fresh must be durably recorded before it is returned, so a caller may
  // execute a non-idempotent side effect only for ActionFresh. The expiry and
  // trusted receive time let a bounded durable implementation recycle only
  // records that can no longer pass the signed action deadline; an unexpired
  // record must never be evicted merely to make space.
  virtual LanReplayDecision acceptAction(
      const uint8_t actionId[kLanUuidBytes], int64_t expiresEpoch,
      int64_t acceptedEpoch) = 0;
};

struct LanGatewayFrame {
  LanFrameKind kind = LanFrameKind::GatewayAck;

  uint64_t spoolRecordId = 0U;
  uint64_t deviceSequence = 0U;

  uint8_t actionId[kLanUuidBytes]{};
  uint8_t companionId[kLanUuidBytes]{};
  uint32_t keyVersion = 0U;
  uint8_t nonce[kLanNonceBytes]{};
  char actionType[kLanMaximumActionTypeBytes + 1U]{};
  int64_t createdEpoch = 0;
  int64_t expiresEpoch = 0;
  size_t parameterBytes = 0U;
};

// Stateless demux supports gateway ACKs interleaved with actions while an ACK
// is outstanding. Remote action exact payload bytes are authenticated before
// the replay store is consulted. paramsOutput is populated only after HMAC.
LanResult decodeGatewayFrame(
    const uint8_t* json, size_t jsonBytes,
    const uint8_t expectedCompanionId[kLanUuidBytes],
    uint32_t expectedKeyVersion, int64_t nowEpoch, bool clockValid,
    const uint8_t backendHmacSecret[32], companion::CompanionCrypto& crypto,
    LanActionReplayStore& replayStore, LanGatewayFrame& output,
    uint8_t* paramsOutput, size_t paramsCapacity);

// Emits exactly the frozen flat device envelope. The signature transcript is
// over payload bytes, not parsed JSON. No proof/algorithm sub-object exists.
LanResult encodeDeviceEnvelope(
    const uint8_t companionId[kLanUuidBytes],
    const uint8_t gatewayId[kLanUuidBytes], uint64_t sequence,
    int64_t issuedEpoch, const uint8_t nonce[kLanNonceBytes],
    const uint8_t requestId[kLanUuidBytes], uint32_t keyVersion,
    const char* payloadType, const uint8_t* payload, size_t payloadBytes,
    const uint8_t backendHmacSecret[32], companion::CompanionCrypto& crypto,
    uint8_t* output, size_t outputCapacity, size_t& outputBytes);

}  // namespace connectivity
}  // namespace kitsu868
