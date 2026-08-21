#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_ble_action.h"
#include "kitsu_gateway_lan_runtime.h"

namespace kitsu868 {
namespace connectivity {

constexpr size_t kGatewayLanActionReplayCapacity = 32U;
constexpr size_t kGatewayLanActionResultCodeBytes = 32U;
constexpr size_t kGatewayLanActionPayloadBytes = 256U;

enum class GatewayLanReplayStorageResult : uint8_t {
  Ok = 0,
  Missing,
  Corrupt,
  Failed,
};

// Two independently replaceable blobs are used so an interrupted write can
// fall back to the prior CRC-valid generation. Action IDs and result codes are
// not credentials, but the adapter must still provide durable read-after-write
// semantics before an action is reported Fresh.
class GatewayLanReplayStorage {
 public:
  virtual ~GatewayLanReplayStorage() = default;
  virtual GatewayLanReplayStorageResult readSlot(
      uint8_t slot, uint8_t* output, size_t outputCapacity,
      size_t& outputBytes) = 0;
  virtual bool writeSlot(uint8_t slot, const uint8_t* input,
                         size_t inputBytes) = 0;
};

enum class GatewayLanActionOutcomeStatus : uint8_t {
  Pending = 0,
  Succeeded,
  Failed,
  Rejected,
};

struct GatewayLanStoredActionOutcome {
  uint8_t actionId[kLanUuidBytes]{};
  int64_t expiresEpoch = 0;
  int64_t acceptedEpoch = 0;
  int64_t completedEpoch = 0;
  GatewayLanActionOutcomeStatus status =
      GatewayLanActionOutcomeStatus::Pending;
  char code[kGatewayLanActionResultCodeBytes + 1U]{};
};

struct GatewayLanActionReplayStatus {
  bool begun = false;
  size_t records = 0U;
  uint32_t generation = 0U;
  int8_t activeSlot = -1;
};

// Durable, bounded at-most-once ledger. Full capacity fails closed while all
// records are still inside their signed deadline. Expired records alone may
// be recycled. An outcome is committed after execution; a Pending record seen
// after reconnect/reboot is indeterminate and must never execute again.
class DurableGatewayLanActionReplayStore final
    : public LanActionReplayStore {
 public:
  DurableGatewayLanActionReplayStore();

  bool begin(GatewayLanReplayStorage& storage);
  void stop();
  LanReplayDecision acceptAction(
      const uint8_t actionId[kLanUuidBytes], int64_t expiresEpoch,
      int64_t acceptedEpoch) override;
  bool recordOutcome(
      const uint8_t actionId[kLanUuidBytes],
      GatewayLanActionOutcomeStatus status, int64_t completedEpoch,
      const char* resultCode);
  bool outcome(const uint8_t actionId[kLanUuidBytes],
               GatewayLanStoredActionOutcome& output) const;
 GatewayLanActionReplayStatus status() const;

 private:
#pragma pack(push, 1)
  struct Record {
    uint8_t actionId[kLanUuidBytes]{};
    int64_t expiresEpoch = 0;
    int64_t acceptedEpoch = 0;
    int64_t completedEpoch = 0;
    uint8_t outcome = 0U;
    char code[kGatewayLanActionResultCodeBytes + 1U]{};
  };

  struct Blob {
    uint32_t magic = 0U;
    uint16_t version = 0U;
    uint16_t bytes = 0U;
    uint32_t generation = 0U;
    uint8_t count = 0U;
    uint8_t reserved[3]{};
    Record records[kGatewayLanActionReplayCapacity]{};
    uint32_t crc32 = 0U;
  };
#pragma pack(pop)

  static_assert(sizeof(Record) == 74U,
                "durable action record wire layout changed");
  static_assert(sizeof(Blob) == 2388U,
                "durable action replay blob wire layout changed");

  bool persist(Blob& candidate);
  static bool validBlob(const Blob& blob);
  static void seal(Blob& blob);
  static bool generationAfter(uint32_t candidate, uint32_t reference);

  GatewayLanReplayStorage* storage_ = nullptr;
  Blob blob_{};
  int8_t activeSlot_ = -1;
  bool begun_ = false;
};

enum class GatewayLanActionBridgeResult : uint8_t {
  Ok = 0,
  InvalidArgument,
  UnsupportedAction,
  InvalidParameters,
  OutputTooSmall,
};

const char* gatewayLanActionBridgeResultName(
    GatewayLanActionBridgeResult result);

// Converts only the existing direct-firmware care/message allowlist into the
// exact action.apply body. It never emits serial commands or raw controls.
// The direct decoder remains the final independent parameter validator.
GatewayLanActionBridgeResult encodeGatewayLanDirectActionRequest(
    const LanGatewayFrame& action, const uint8_t* paramsJson,
    size_t paramsJsonBytes, int64_t executionExpiresEpoch, uint8_t* output,
    size_t outputCapacity, size_t& outputBytes);

struct GatewayLanDirectActionOutcome {
  GatewayLanActionOutcomeStatus status =
      GatewayLanActionOutcomeStatus::Failed;
  char code[kGatewayLanActionResultCodeBytes + 1U]{};
};

class GatewayLanDirectActionExecutor {
 public:
  virtual ~GatewayLanDirectActionExecutor() = default;
  virtual bool executeDirectAction(
      const uint8_t* actionRequest, size_t actionRequestBytes,
      GatewayLanDirectActionOutcome& outcome) = 0;
};

class GatewayLanDevicePayloadQueue {
 public:
  virtual ~GatewayLanDevicePayloadQueue() = default;
  virtual bool canEnqueue(size_t payloadCount,
                          size_t worstCasePayloadBytes) const = 0;
  virtual bool enqueue(const char* payloadType, const uint8_t* payload,
                       size_t payloadBytes, int64_t issuedEpoch) = 0;
};

// Fresh actions execute through GatewayLanDirectActionExecutor exactly once.
// Signed duplicates only re-emit a durable result; Pending duplicates become
// a fail-closed result_unknown and never run the side effect again.
class GatewayLanActionDispatcher final : public GatewayLanActionSink {
 public:
  bool begin(DurableGatewayLanActionReplayStore& outcomes,
             GatewayLanDirectActionExecutor& executor,
             GatewayLanDevicePayloadQueue& payloads);
  void stop();

  bool acceptAuthenticatedAction(
      const uint8_t* framedJson, size_t framedJsonBytes,
      const LanGatewayFrame& metadata, const uint8_t* paramsJson,
      size_t paramsJsonBytes, int64_t acceptedEpoch) override;
  bool repeatAuthenticatedAction(
      const uint8_t* framedJson, size_t framedJsonBytes,
      const LanGatewayFrame& metadata, const uint8_t* paramsJson,
      size_t paramsJsonBytes, int64_t repeatedEpoch) override;

 private:
  bool handle(const uint8_t* framedJson, size_t framedJsonBytes,
              const LanGatewayFrame& metadata, const uint8_t* paramsJson,
              size_t paramsJsonBytes, int64_t nowEpoch, bool duplicate);
  bool emit(const GatewayLanStoredActionOutcome& outcome,
            int64_t issuedEpoch);

  DurableGatewayLanActionReplayStore* outcomes_ = nullptr;
  GatewayLanDirectActionExecutor* executor_ = nullptr;
  GatewayLanDevicePayloadQueue* payloads_ = nullptr;
};

static_assert(sizeof(DurableGatewayLanActionReplayStore) < 3U * 1024U,
              "LAN action replay ledger exceeded permanent RAM budget");
static_assert(sizeof(GatewayLanActionDispatcher) <= 32U,
              "LAN action dispatcher unexpectedly became resident-heavy");

}  // namespace connectivity
}  // namespace kitsu868
