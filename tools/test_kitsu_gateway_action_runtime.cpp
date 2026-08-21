#include "../src/kitsu_gateway_action_runtime.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace kitsu868::connectivity;

class MemoryStorage final : public GatewayLanReplayStorage {
 public:
  GatewayLanReplayStorageResult readSlot(
      uint8_t slot, uint8_t* output, size_t capacity,
      size_t& outputBytes) override {
    outputBytes = 0U;
    if (failRead || slot >= 2U || !output) {
      return GatewayLanReplayStorageResult::Failed;
    }
    if (slots[slot].empty()) return GatewayLanReplayStorageResult::Missing;
    if (slots[slot].size() != capacity) {
      return GatewayLanReplayStorageResult::Corrupt;
    }
    memcpy(output, slots[slot].data(), capacity);
    outputBytes = capacity;
    return GatewayLanReplayStorageResult::Ok;
  }

  bool writeSlot(uint8_t slot, const uint8_t* input,
                 size_t inputBytes) override {
    ++writes;
    if (failWrite || slot >= 2U || !input || inputBytes == 0U) return false;
    slots[slot].assign(input, input + inputBytes);
    if (corruptWrite) slots[slot][inputBytes / 2U] ^= 0x5aU;
    return true;
  }

  std::vector<uint8_t> slots[2];
  unsigned writes = 0U;
  bool failRead = false;
  bool failWrite = false;
  bool corruptWrite = false;
};

void actionId(unsigned ordinal, uint8_t output[16]) {
  memset(output, 0, 16U);
  output[0] = 0x10U;
  output[14] = static_cast<uint8_t>(ordinal >> 8U);
  output[15] = static_cast<uint8_t>(ordinal);
}

LanGatewayFrame action(unsigned ordinal, const char* type,
                       int64_t expires = 1800000120LL) {
  LanGatewayFrame output{};
  output.kind = LanFrameKind::RemoteAction;
  actionId(ordinal, output.actionId);
  output.createdEpoch = 1800000000LL;
  output.expiresEpoch = expires;
  strcpy_s(output.actionType, type);
  return output;
}

void testDurableReservationOutcomeAndRecovery() {
  MemoryStorage storage;
  DurableGatewayLanActionReplayStore replay;
  assert(replay.begin(storage));
  uint8_t first[16]{};
  actionId(1U, first);
  assert(replay.acceptAction(first, 1800000120LL, 1800000010LL) ==
         LanReplayDecision::Fresh);
  assert(storage.writes == 1U);
  assert(replay.acceptAction(first, 1800000120LL, 1800000011LL) ==
         LanReplayDecision::Duplicate);
  assert(replay.recordOutcome(first, GatewayLanActionOutcomeStatus::Succeeded,
                              1800000011LL, "applied"));
  GatewayLanStoredActionOutcome stored{};
  assert(replay.outcome(first, stored));
  assert(stored.status == GatewayLanActionOutcomeStatus::Succeeded);
  assert(strcmp(stored.code, "applied") == 0);

  DurableGatewayLanActionReplayStore restored;
  assert(restored.begin(storage));
  assert(restored.acceptAction(first, 1800000120LL, 1800000012LL) ==
         LanReplayDecision::Duplicate);
  assert(restored.outcome(first, stored));
  assert(stored.completedEpoch == 1800000011LL);

  uint8_t second[16]{};
  actionId(2U, second);
  assert(restored.acceptAction(second, 1800000120LL, 1800000012LL) ==
         LanReplayDecision::Fresh);
  const uint8_t newest = static_cast<uint8_t>(restored.status().activeSlot);
  storage.slots[newest][storage.slots[newest].size() / 3U] ^= 0x80U;
  DurableGatewayLanActionReplayStore recovered;
  assert(recovered.begin(storage));
  assert(recovered.acceptAction(first, 1800000120LL, 1800000013LL) ==
         LanReplayDecision::Duplicate);
  // The torn newest generation is discarded, so the second reservation was
  // never durably returned from the recovered point of view.
  assert(recovered.acceptAction(second, 1800000120LL, 1800000013LL) ==
         LanReplayDecision::Fresh);
}

void testWriteFailureAndBoundedNonEviction() {
  MemoryStorage storage;
  DurableGatewayLanActionReplayStore replay;
  assert(replay.begin(storage));
  uint8_t id[16]{};
  actionId(1U, id);
  storage.failWrite = true;
  assert(replay.acceptAction(id, 1800000100LL, 1800000001LL) ==
         LanReplayDecision::Failed);
  storage.failWrite = false;
  assert(replay.acceptAction(id, 1800000100LL, 1800000001LL) ==
         LanReplayDecision::Fresh);

  for (unsigned ordinal = 2U;
       ordinal <= kGatewayLanActionReplayCapacity; ++ordinal) {
    actionId(ordinal, id);
    assert(replay.acceptAction(id, 1800000100LL, 1800000002LL) ==
           LanReplayDecision::Fresh);
  }
  actionId(99U, id);
  assert(replay.acceptAction(id, 1800000100LL, 1800000003LL) ==
         LanReplayDecision::Failed);
  // Only expired records may be recycled.
  assert(replay.acceptAction(id, 1800000200LL, 1800000101LL) ==
         LanReplayDecision::Fresh);
}

void testDirectBridgeAllowlistAndMessageMapping() {
  uint8_t output[1024]{};
  size_t outputBytes = 0U;
  LanGatewayFrame pet = action(1U, "companion.pet");
  static const uint8_t empty[] = "{}";
  assert(encodeGatewayLanDirectActionRequest(
             pet, empty, sizeof(empty) - 1U, pet.expiresEpoch, output,
             sizeof(output), outputBytes) == GatewayLanActionBridgeResult::Ok);
  BleActionCommand decoded{};
  assert(decodeBleActionCommand(output, outputBytes, decoded) ==
         BleActionDecodeResult::Ok);
  assert(decoded.kind == BleActionKind::Pet);

  LanGatewayFrame message = action(2U, "message.send");
  static const uint8_t messageParams[] =
      "{\"route\":\"direct\","
      "\"target\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
      "\"text\":\"hello\"}";
  assert(encodeGatewayLanDirectActionRequest(
             message, messageParams, sizeof(messageParams) - 1U,
             message.expiresEpoch, output, sizeof(output), outputBytes) ==
         GatewayLanActionBridgeResult::Ok);
  const std::string encoded(reinterpret_cast<char*>(output), outputBytes);
  assert(encoded.find("\"target_id\":") != std::string::npos);
  assert(encoded.find("\"target\":") == std::string::npos);
  assert(decodeBleActionCommand(output, outputBytes, decoded) ==
         BleActionDecodeResult::Ok);
  assert(decoded.kind == BleActionKind::SendMessage);

  LanGatewayFrame unsupported = action(3U, "sync.pull");
  assert(encodeGatewayLanDirectActionRequest(
             unsupported, empty, sizeof(empty) - 1U,
             unsupported.expiresEpoch, output, sizeof(output), outputBytes) ==
         GatewayLanActionBridgeResult::UnsupportedAction);
}

class Executor final : public GatewayLanDirectActionExecutor {
 public:
  bool executeDirectAction(
      const uint8_t* request, size_t requestBytes,
      GatewayLanDirectActionOutcome& outcome) override {
    ++calls;
    BleActionCommand decoded{};
    if (decodeBleActionCommand(request, requestBytes, decoded) !=
        BleActionDecodeResult::Ok) {
      return false;
    }
    lastExpiresEpoch = decoded.expiresAtEpoch;
    outcome.status = GatewayLanActionOutcomeStatus::Succeeded;
    strcpy_s(outcome.code, "applied");
    return true;
  }
  unsigned calls = 0U;
  uint32_t lastExpiresEpoch = 0U;
};

class PayloadQueue final : public GatewayLanDevicePayloadQueue {
 public:
  bool canEnqueue(size_t count, size_t) const override {
    return allow && bodies.size() + count <= 16U;
  }
  bool enqueue(const char* type, const uint8_t* payload,
               size_t payloadBytes, int64_t) override {
    if (!allow) return false;
    types.push_back(type);
    bodies.emplace_back(reinterpret_cast<const char*>(payload), payloadBytes);
    return true;
  }
  bool allow = true;
  std::vector<std::string> types;
  std::vector<std::string> bodies;
};

void testDispatcherExecutesOnceAndReemitsDurableOutcome() {
  MemoryStorage storage;
  DurableGatewayLanActionReplayStore replay;
  assert(replay.begin(storage));
  Executor executor;
  PayloadQueue payloads;
  GatewayLanActionDispatcher dispatcher;
  assert(dispatcher.begin(replay, executor, payloads));
  LanGatewayFrame pet = action(1U, "companion.pet");
  assert(replay.acceptAction(pet.actionId, pet.expiresEpoch,
                             1800000010LL) == LanReplayDecision::Fresh);
  static const uint8_t frame[] = "{signed-frame}";
  static const uint8_t params[] = "{}";
  assert(dispatcher.acceptAuthenticatedAction(
      frame, sizeof(frame) - 1U, pet, params, sizeof(params) - 1U,
      1800000010LL));
  assert(executor.calls == 1U);
  assert(payloads.types.size() == 2U);
  assert(payloads.types[0] == "action_acceptance");
  assert(payloads.types[1] == "action_result");
  assert(payloads.bodies[1].find("\"status\":\"succeeded\"") !=
         std::string::npos);
  assert(payloads.bodies[1].find("\"code\":\"applied\"") !=
         std::string::npos);

  assert(replay.acceptAction(pet.actionId, pet.expiresEpoch,
                             1800000011LL) == LanReplayDecision::Duplicate);
  assert(dispatcher.repeatAuthenticatedAction(
      frame, sizeof(frame) - 1U, pet, params, sizeof(params) - 1U,
      1800000011LL));
  assert(executor.calls == 1U);
  assert(payloads.types.size() == 4U);
  assert(payloads.bodies[3].find("\"code\":\"applied\"") !=
         std::string::npos);
}

void testPendingDuplicateNeverExecutesAndUnsupportedIsRejected() {
  static const uint8_t frame[] = "{signed-frame}";
  static const uint8_t params[] = "{}";
  MemoryStorage storage;
  DurableGatewayLanActionReplayStore replay;
  assert(replay.begin(storage));
  Executor executor;
  PayloadQueue payloads;
  GatewayLanActionDispatcher dispatcher;
  assert(dispatcher.begin(replay, executor, payloads));

  LanGatewayFrame pending = action(1U, "companion.pet");
  assert(replay.acceptAction(pending.actionId, pending.expiresEpoch,
                             1800000010LL) == LanReplayDecision::Fresh);
  assert(dispatcher.repeatAuthenticatedAction(
      frame, sizeof(frame) - 1U, pending, params, sizeof(params) - 1U,
      1800000011LL));
  assert(executor.calls == 0U);
  assert(payloads.bodies[1].find("\"status\":\"failed\"") !=
         std::string::npos);
  assert(payloads.bodies[1].find("result_unknown") != std::string::npos);

  LanGatewayFrame unsupported = action(2U, "sync.pull");
  assert(replay.acceptAction(unsupported.actionId, unsupported.expiresEpoch,
                             1800000012LL) == LanReplayDecision::Fresh);
  assert(dispatcher.acceptAuthenticatedAction(
      frame, sizeof(frame) - 1U, unsupported, params,
      sizeof(params) - 1U, 1800000012LL));
  assert(executor.calls == 0U);
  assert(payloads.bodies.back().find("\"status\":\"rejected\"") !=
         std::string::npos);
  assert(payloads.bodies.back().find("action_unavailable") !=
         std::string::npos);
}

void testLongSignedDeadlinesUseNarrowLocalWindowAndNeverRerun() {
  static const uint8_t frame[] = "{signed-frame}";
  static const uint8_t params[] = "{}";
  MemoryStorage storage;
  DurableGatewayLanActionReplayStore replay;
  assert(replay.begin(storage));
  Executor executor;
  PayloadQueue payloads;
  GatewayLanActionDispatcher dispatcher;
  assert(dispatcher.begin(replay, executor, payloads));

  static const int64_t ttls[] = {300LL, 86400LL};
  for (unsigned i = 0U; i < 2U; ++i) {
    const int64_t acceptedEpoch = 1800000010LL + i;
    LanGatewayFrame pet = action(
        20U + i, "companion.pet", acceptedEpoch + ttls[i]);
    assert(replay.acceptAction(pet.actionId, pet.expiresEpoch,
                               acceptedEpoch) == LanReplayDecision::Fresh);
    assert(dispatcher.acceptAuthenticatedAction(
        frame, sizeof(frame) - 1U, pet, params, sizeof(params) - 1U,
        acceptedEpoch));
    assert(executor.calls == i + 1U);
    assert(executor.lastExpiresEpoch ==
           static_cast<uint32_t>(acceptedEpoch +
                                 kBleActionMaximumExpirySeconds));

    GatewayLanStoredActionOutcome stored{};
    assert(replay.outcome(pet.actionId, stored));
    // The at-most-once ledger keeps the backend's original signed deadline;
    // only the local direct-execution envelope is narrowed to KRA3's window.
    assert(stored.expiresEpoch == acceptedEpoch + ttls[i]);
    assert(replay.acceptAction(pet.actionId, pet.expiresEpoch,
                               acceptedEpoch + 1LL) ==
           LanReplayDecision::Duplicate);
    assert(dispatcher.repeatAuthenticatedAction(
        frame, sizeof(frame) - 1U, pet, params, sizeof(params) - 1U,
        acceptedEpoch + 1LL));
    assert(executor.calls == i + 1U);
  }
}

}  // namespace

int main() {
  testDurableReservationOutcomeAndRecovery();
  testWriteFailureAndBoundedNonEviction();
  testDirectBridgeAllowlistAndMessageMapping();
  testDispatcherExecutesOnceAndReemitsDurableOutcome();
  testPendingDuplicateNeverExecutesAndUnsupportedIsRejected();
  testLongSignedDeadlinesUseNarrowLocalWindowAndNeverRerun();
  std::cout << "Kitsu gateway action runtime tests passed.\n";
  return 0;
}
