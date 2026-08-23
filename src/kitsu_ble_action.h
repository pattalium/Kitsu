#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kitsu868 {
namespace connectivity {

constexpr size_t kBleActionUuidBytes = 16U;
constexpr size_t kBleActionUuidTextBytes = 36U;
constexpr uint32_t kBleActionMinimumTrustedEpoch = 1704067200UL;
constexpr uint32_t kBleActionMaximumTrustedEpoch = 4102444800UL;
constexpr uint32_t kBleActionMaximumExpirySeconds = 120UL;
constexpr uint32_t kBleActionMinimumListenMs = 1000UL;
constexpr uint32_t kBleActionMaximumListenMs = 60000UL;
constexpr size_t kBleActionReplayCapacity = 8U;
constexpr size_t kBleActionCommandDigestBytes = 32U;
constexpr size_t kBleActionMessageTargetBytes = 43U;
constexpr size_t kBleActionMessageTextBytes = 128U;

enum class BleMessageRoute : uint8_t {
  None = 0,
  Direct = 1,
  Channel = 2,
};

enum class BleAdvertScope : uint8_t {
  None = 0,
  Nearby = 1,
  Mesh = 2,
};

enum class BleActionKind : uint8_t {
  Pet = 1,
  Feed = 2,
  Play = 3,
  ListenOnce = 4,
  // Value 5 historically named this exact action. Restoring the same value
  // and semantics is safe; the withdrawn implementation never made it
  // available and therefore could not persist an accepted replay record.
  AdvertiseOnce = 5,
  // Keep SendMessage at 6 so an update cannot reinterpret a still-live
  // persisted replay digest.
  SendMessage = 6,
};

enum class BleActionDecodeResult : uint8_t {
  Ok = 0,
  InvalidArgument,
  MalformedJson,
  DuplicateField,
  UnknownField,
  InvalidActionId,
  InvalidKind,
  InvalidExpiry,
  InvalidParams,
};

const char* bleActionDecodeResultName(BleActionDecodeResult result);
const char* bleActionKindName(BleActionKind kind);
bool bleActionKindAvailable(BleActionKind kind);

struct BleActionCommand {
  uint8_t actionId[kBleActionUuidBytes]{};
  char actionIdText[kBleActionUuidTextBytes + 1U]{};
  BleActionKind kind = BleActionKind::Pet;
  // Absolute Unix deadline. The receiver must compare this with a trusted
  // clock before reserving or executing the command. An absolute deadline is
  // deliberately non-refreshable when the same encrypted frame is replayed.
  uint32_t expiresAtEpoch = 0U;
  // Populated only for listen_once. It stays zero for every other kind.
  uint32_t durationMs = 0U;
  // Populated only for advertise_once. The scope is explicit on the wire so
  // zero-hop and flood requests have distinct durable replay bindings.
  BleAdvertScope advertScope = BleAdvertScope::None;
  // Populated only for send_message. Direct targets are canonical unpadded
  // base64url 32-byte MeshCore public keys (43 characters). Channel targets
  // are canonical decimal slots 0..3. Text is decoded UTF-8, not JSON source.
  BleMessageRoute messageRoute = BleMessageRoute::None;
  uint8_t messageTargetBytes = 0U;
  uint8_t messageTextBytes = 0U;
  char messageTarget[kBleActionMessageTargetBytes + 1U]{};
  char messageText[kBleActionMessageTextBytes + 1U]{};
  bool actionIdValid = false;
};

// Decodes the direct action.apply body. The outer object and the params object
// both use exact schemas: duplicate and unknown properties are rejected, as
// are non-integer numeric spellings and escaped protocol identifiers.
BleActionDecodeResult decodeBleActionCommand(
    const uint8_t* json, size_t jsonBytes, BleActionCommand& output);

// SHA-256 over a domain-separated, length-delimited canonical encoding of
// every semantic command field. This is the only command binding persisted by
// the replay ledger; message text and target identifiers are never retained.
bool bleActionCommandDigest(
    const BleActionCommand& command,
    uint8_t output[kBleActionCommandDigestBytes]);

// Emits the direct operation result consumed by Android/iOS ActionReceipt:
// {"action_id":...,"accepted":...,"state":...,"error_code":...}.
// errorCode must be null for an accepted receipt and non-null for a rejection.
bool encodeBleActionReceipt(
    const BleActionCommand& command, bool accepted, const char* state,
    const char* errorCode, uint8_t* output, size_t outputCapacity,
    size_t& outputBytes);

enum class BleActionReplayDecision : uint8_t {
  Fresh = 0,
  DuplicateApplied,
  DuplicateIndeterminate,
  Conflict,
  Expired,
  TimeUnavailable,
  InvalidExpiry,
};

// A compact persistent, CRC-protected at-most-once ledger. Only accepted safe
// actions are recorded. A reservation is durably Pending before the side
// effect and durably Applied afterward. A retry of Pending is reported as
// indeterminate and never executes again; reusing a UUID for a different
// command returns Conflict. Callers must provide trusted Unix time. A missing
// clock fails closed, and protected records are never evicted for capacity.
class BleActionReplayCache {
 public:
  BleActionReplayCache();

  void reset();
  bool load(const uint8_t* serialized, size_t serializedBytes);
  const uint8_t* serialized(size_t& serializedBytes) const;

  BleActionReplayDecision inspect(
      const BleActionCommand& command, uint32_t trustedNowEpoch) const;
  bool remember(const BleActionCommand& command,
                uint32_t trustedNowEpoch);
  bool markApplied(const BleActionCommand& command);

 private:
#pragma pack(push, 1)
  struct Record {
    uint8_t actionId[kBleActionUuidBytes]{};
    uint8_t commandDigest[kBleActionCommandDigestBytes]{};
    uint32_t expiresAtEpoch = 0U;
    uint8_t outcome = 0U;
  };

  struct Blob {
    uint32_t magic = 0U;
    uint16_t bytes = 0U;
    uint8_t count = 0U;
    uint8_t reserved = 0U;
    Record records[kBleActionReplayCapacity]{};
    uint32_t crc32 = 0U;
  };
#pragma pack(pop)

  bool valid() const;
  void seal();

  alignas(4) Blob blob_{};
};

constexpr size_t kBleActionReplaySerializedBytes =
    sizeof(BleActionReplayCache);

}  // namespace connectivity
}  // namespace kitsu868
