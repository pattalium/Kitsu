#include "kitsu_gateway_action_runtime.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr uint32_t kReplayMagic = 0x3152414cUL;  // "LAR1" on the wire.
constexpr uint16_t kReplayVersion = 1U;

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

bool allZero(const uint8_t* input, size_t bytes) {
  if (!input) return true;
  uint8_t combined = 0U;
  for (size_t i = 0U; i < bytes; ++i) combined |= input[i];
  return combined == 0U;
}

uint32_t crc32(const uint8_t* input, size_t bytes) {
  uint32_t crc = 0xffffffffUL;
  for (size_t i = 0U; i < bytes; ++i) {
    crc ^= input[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320UL & mask);
    }
  }
  return ~crc;
}

bool validEpoch(int64_t value) {
  return value >= kLanMinimumKnownEpoch && value <= kLanMaximumKnownEpoch;
}

bool validResultCode(const char* value) {
  if (!value) return false;
  const size_t bytes = strlen(value);
  if (bytes == 0U || bytes > kGatewayLanActionResultCodeBytes ||
      value[0] < 'a' || value[0] > 'z') {
    return false;
  }
  for (size_t i = 1U; i < bytes; ++i) {
    const char c = value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == '_' || c == '-' || c == '.')) {
      return false;
    }
  }
  return true;
}

bool validOutcomeStatus(uint8_t value) {
  return value <=
      static_cast<uint8_t>(GatewayLanActionOutcomeStatus::Rejected);
}

void formatUuid(const uint8_t value[kLanUuidBytes], char output[37]) {
  static const char hex[] = "0123456789abcdef";
  size_t cursor = 0U;
  for (size_t i = 0U; i < kLanUuidBytes; ++i) {
    if (i == 4U || i == 6U || i == 8U || i == 10U) output[cursor++] = '-';
    output[cursor++] = hex[value[i] >> 4U];
    output[cursor++] = hex[value[i] & 0x0fU];
  }
  output[cursor] = '\0';
}

const char* outcomeStatusName(GatewayLanActionOutcomeStatus status) {
  switch (status) {
    case GatewayLanActionOutcomeStatus::Succeeded: return "succeeded";
    case GatewayLanActionOutcomeStatus::Failed: return "failed";
    case GatewayLanActionOutcomeStatus::Rejected: return "rejected";
    case GatewayLanActionOutcomeStatus::Pending: break;
  }
  return nullptr;
}

bool copyResultCode(const char* input,
                    char output[kGatewayLanActionResultCodeBytes + 1U]) {
  memset(output, 0, kGatewayLanActionResultCodeBytes + 1U);
  if (!validResultCode(input)) return false;
  memcpy(output, input, strlen(input));
  return true;
}

bool appendBytes(uint8_t* output, size_t outputCapacity, size_t& cursor,
                 const uint8_t* input, size_t inputBytes) {
  if (!output || (!input && inputBytes != 0U) ||
      cursor > outputCapacity || inputBytes > outputCapacity - cursor) {
    return false;
  }
  if (inputBytes != 0U) memcpy(output + cursor, input, inputBytes);
  cursor += inputBytes;
  return true;
}

bool exactBytes(const uint8_t* input, size_t inputBytes,
                const char* expected) {
  const size_t expectedBytes = expected ? strlen(expected) : 0U;
  return input && inputBytes == expectedBytes &&
         memcmp(input, expected, expectedBytes) == 0;
}

}  // namespace

DurableGatewayLanActionReplayStore::DurableGatewayLanActionReplayStore() {
  secureZero(&blob_, sizeof(blob_));
  blob_.magic = kReplayMagic;
  blob_.version = kReplayVersion;
  blob_.bytes = static_cast<uint16_t>(sizeof(blob_));
  seal(blob_);
}

bool DurableGatewayLanActionReplayStore::generationAfter(
    uint32_t candidate, uint32_t reference) {
  return static_cast<int32_t>(candidate - reference) > 0;
}

void DurableGatewayLanActionReplayStore::seal(Blob& blob) {
  blob.crc32 = 0U;
  blob.crc32 = crc32(reinterpret_cast<const uint8_t*>(&blob),
                     offsetof(Blob, crc32));
}

bool DurableGatewayLanActionReplayStore::validBlob(const Blob& blob) {
  if (blob.magic != kReplayMagic || blob.version != kReplayVersion ||
      blob.bytes != sizeof(blob) ||
      blob.count > kGatewayLanActionReplayCapacity ||
      !allZero(blob.reserved, sizeof(blob.reserved)) ||
      crc32(reinterpret_cast<const uint8_t*>(&blob),
            offsetof(Blob, crc32)) != blob.crc32) {
    return false;
  }
  for (size_t i = 0U; i < kGatewayLanActionReplayCapacity; ++i) {
    const Record& record = blob.records[i];
    if (i >= blob.count) {
      if (!allZero(reinterpret_cast<const uint8_t*>(&record),
                   sizeof(record))) {
        return false;
      }
      continue;
    }
    if (allZero(record.actionId, sizeof(record.actionId)) ||
        !validEpoch(record.expiresEpoch) ||
        !validEpoch(record.acceptedEpoch) ||
        record.acceptedEpoch > record.expiresEpoch ||
        !validOutcomeStatus(record.outcome)) {
      return false;
    }
    const GatewayLanActionOutcomeStatus status =
        static_cast<GatewayLanActionOutcomeStatus>(record.outcome);
    if (status == GatewayLanActionOutcomeStatus::Pending) {
      if (record.completedEpoch != 0 || record.code[0] != '\0') return false;
    } else if (!validEpoch(record.completedEpoch) ||
               !validResultCode(record.code)) {
      return false;
    }
    for (size_t prior = 0U; prior < i; ++prior) {
      if (memcmp(blob.records[prior].actionId, record.actionId,
                 kLanUuidBytes) == 0) {
        return false;
      }
    }
  }
  return true;
}

bool DurableGatewayLanActionReplayStore::begin(
    GatewayLanReplayStorage& storage) {
  stop();
  Blob* slots[2] = {nullptr, nullptr};
  GatewayLanReplayStorageResult results[2] = {
      GatewayLanReplayStorageResult::Failed,
      GatewayLanReplayStorageResult::Failed};
  bool valid[2] = {false, false};
  for (uint8_t slot = 0U; slot < 2U; ++slot) {
    slots[slot] = static_cast<Blob*>(malloc(sizeof(Blob)));
    if (!slots[slot]) {
      for (uint8_t i = 0U; i < 2U; ++i) {
        if (slots[i]) {
          secureZero(slots[i], sizeof(Blob));
          free(slots[i]);
        }
      }
      return false;
    }
    secureZero(slots[slot], sizeof(Blob));
    size_t bytes = 0U;
    results[slot] = storage.readSlot(
        slot, reinterpret_cast<uint8_t*>(slots[slot]), sizeof(Blob), bytes);
    valid[slot] = results[slot] == GatewayLanReplayStorageResult::Ok &&
                  bytes == sizeof(Blob) && validBlob(*slots[slot]);
  }

  bool ok = true;
  if (results[0] == GatewayLanReplayStorageResult::Failed ||
      results[1] == GatewayLanReplayStorageResult::Failed) {
    ok = false;
  } else if (valid[0] || valid[1]) {
    uint8_t selected = valid[0] ? 0U : 1U;
    if (valid[0] && valid[1] &&
        generationAfter(slots[1]->generation, slots[0]->generation)) {
      selected = 1U;
    }
    memcpy(&blob_, slots[selected], sizeof(blob_));
    activeSlot_ = static_cast<int8_t>(selected);
  } else if (results[0] == GatewayLanReplayStorageResult::Missing &&
             results[1] == GatewayLanReplayStorageResult::Missing) {
    secureZero(&blob_, sizeof(blob_));
    blob_.magic = kReplayMagic;
    blob_.version = kReplayVersion;
    blob_.bytes = static_cast<uint16_t>(sizeof(blob_));
    seal(blob_);
    // No generation exists yet. persist() chooses slot zero for the first
    // commit while status() truthfully reports that neither slot is active.
    activeSlot_ = -1;
  } else {
    ok = false;
  }

  for (uint8_t slot = 0U; slot < 2U; ++slot) {
    secureZero(slots[slot], sizeof(Blob));
    free(slots[slot]);
  }
  if (!ok) {
    stop();
    return false;
  }
  storage_ = &storage;
  begun_ = true;
  return true;
}

void DurableGatewayLanActionReplayStore::stop() {
  storage_ = nullptr;
  activeSlot_ = -1;
  begun_ = false;
  secureZero(&blob_, sizeof(blob_));
  blob_.magic = kReplayMagic;
  blob_.version = kReplayVersion;
  blob_.bytes = static_cast<uint16_t>(sizeof(blob_));
  seal(blob_);
}

bool DurableGatewayLanActionReplayStore::persist(Blob& candidate) {
  if (!begun_ || !storage_) return false;
  candidate.magic = kReplayMagic;
  candidate.version = kReplayVersion;
  candidate.bytes = static_cast<uint16_t>(sizeof(candidate));
  candidate.generation = blob_.generation + 1U;
  seal(candidate);
  const uint8_t target = activeSlot_ < 0
      ? 0U
      : static_cast<uint8_t>(1 - activeSlot_);
  if (!storage_->writeSlot(target,
                           reinterpret_cast<const uint8_t*>(&candidate),
                           sizeof(candidate))) {
    return false;
  }
  Blob* verification = static_cast<Blob*>(malloc(sizeof(Blob)));
  if (!verification) return false;
  secureZero(verification, sizeof(Blob));
  size_t verificationBytes = 0U;
  const GatewayLanReplayStorageResult read = storage_->readSlot(
      target, reinterpret_cast<uint8_t*>(verification), sizeof(Blob),
      verificationBytes);
  const bool verified = read == GatewayLanReplayStorageResult::Ok &&
      verificationBytes == sizeof(Blob) && validBlob(*verification) &&
      memcmp(verification, &candidate, sizeof(candidate)) == 0;
  secureZero(verification, sizeof(Blob));
  free(verification);
  if (!verified) return false;
  memcpy(&blob_, &candidate, sizeof(blob_));
  activeSlot_ = static_cast<int8_t>(target);
  return true;
}

LanReplayDecision DurableGatewayLanActionReplayStore::acceptAction(
    const uint8_t actionId[kLanUuidBytes], int64_t expiresEpoch,
    int64_t acceptedEpoch) {
  if (!begun_ || !storage_ || !actionId ||
      allZero(actionId, kLanUuidBytes) || !validEpoch(expiresEpoch) ||
      !validEpoch(acceptedEpoch) || acceptedEpoch > expiresEpoch) {
    return LanReplayDecision::Failed;
  }
  for (size_t i = 0U; i < blob_.count; ++i) {
    if (memcmp(blob_.records[i].actionId, actionId, kLanUuidBytes) == 0) {
      return LanReplayDecision::Duplicate;
    }
  }
  size_t selected = kGatewayLanActionReplayCapacity;
  for (size_t i = 0U; i < blob_.count; ++i) {
    if (blob_.records[i].expiresEpoch < acceptedEpoch) {
      selected = i;
      break;
    }
  }
  if (selected == kGatewayLanActionReplayCapacity &&
      blob_.count < kGatewayLanActionReplayCapacity) {
    selected = blob_.count;
  }
  if (selected == kGatewayLanActionReplayCapacity) {
    return LanReplayDecision::Failed;
  }

  Blob* candidate = static_cast<Blob*>(malloc(sizeof(Blob)));
  if (!candidate) return LanReplayDecision::Failed;
  memcpy(candidate, &blob_, sizeof(Blob));
  if (selected == candidate->count) ++candidate->count;
  Record& record = candidate->records[selected];
  secureZero(&record, sizeof(record));
  memcpy(record.actionId, actionId, kLanUuidBytes);
  record.expiresEpoch = expiresEpoch;
  record.acceptedEpoch = acceptedEpoch;
  record.outcome =
      static_cast<uint8_t>(GatewayLanActionOutcomeStatus::Pending);
  const bool committed = persist(*candidate);
  secureZero(candidate, sizeof(Blob));
  free(candidate);
  return committed ? LanReplayDecision::Fresh : LanReplayDecision::Failed;
}

bool DurableGatewayLanActionReplayStore::recordOutcome(
    const uint8_t actionId[kLanUuidBytes],
    GatewayLanActionOutcomeStatus status, int64_t completedEpoch,
    const char* resultCode) {
  if (!begun_ || !storage_ || !actionId ||
      status == GatewayLanActionOutcomeStatus::Pending ||
      !validEpoch(completedEpoch) || !validResultCode(resultCode)) {
    return false;
  }
  size_t selected = kGatewayLanActionReplayCapacity;
  for (size_t i = 0U; i < blob_.count; ++i) {
    if (memcmp(blob_.records[i].actionId, actionId, kLanUuidBytes) == 0) {
      selected = i;
      break;
    }
  }
  if (selected == kGatewayLanActionReplayCapacity) return false;
  const Record& current = blob_.records[selected];
  if (current.outcome !=
      static_cast<uint8_t>(GatewayLanActionOutcomeStatus::Pending)) {
    return current.outcome == static_cast<uint8_t>(status) &&
        current.completedEpoch == completedEpoch &&
        strcmp(current.code, resultCode) == 0;
  }

  Blob* candidate = static_cast<Blob*>(malloc(sizeof(Blob)));
  if (!candidate) return false;
  memcpy(candidate, &blob_, sizeof(Blob));
  Record& record = candidate->records[selected];
  record.outcome = static_cast<uint8_t>(status);
  record.completedEpoch = completedEpoch;
  memset(record.code, 0, sizeof(record.code));
  memcpy(record.code, resultCode, strlen(resultCode));
  const bool committed = persist(*candidate);
  secureZero(candidate, sizeof(Blob));
  free(candidate);
  return committed;
}

bool DurableGatewayLanActionReplayStore::outcome(
    const uint8_t actionId[kLanUuidBytes],
    GatewayLanStoredActionOutcome& output) const {
  output = GatewayLanStoredActionOutcome{};
  if (!begun_ || !actionId) return false;
  for (size_t i = 0U; i < blob_.count; ++i) {
    const Record& record = blob_.records[i];
    if (memcmp(record.actionId, actionId, kLanUuidBytes) != 0) continue;
    memcpy(output.actionId, record.actionId, kLanUuidBytes);
    output.expiresEpoch = record.expiresEpoch;
    output.acceptedEpoch = record.acceptedEpoch;
    output.completedEpoch = record.completedEpoch;
    output.status =
        static_cast<GatewayLanActionOutcomeStatus>(record.outcome);
    memcpy(output.code, record.code, sizeof(output.code));
    return true;
  }
  return false;
}

GatewayLanActionReplayStatus DurableGatewayLanActionReplayStore::status()
    const {
  GatewayLanActionReplayStatus output{};
  output.begun = begun_;
  output.records = begun_ ? blob_.count : 0U;
  output.generation = begun_ ? blob_.generation : 0U;
  output.activeSlot = begun_ ? activeSlot_ : -1;
  return output;
}

const char* gatewayLanActionBridgeResultName(
    GatewayLanActionBridgeResult result) {
  switch (result) {
    case GatewayLanActionBridgeResult::Ok: return "ok";
    case GatewayLanActionBridgeResult::InvalidArgument:
      return "invalid_argument";
    case GatewayLanActionBridgeResult::UnsupportedAction:
      return "unsupported_action";
    case GatewayLanActionBridgeResult::InvalidParameters:
      return "invalid_parameters";
    case GatewayLanActionBridgeResult::OutputTooSmall:
      return "output_too_small";
  }
  return "invalid_argument";
}

GatewayLanActionBridgeResult encodeGatewayLanDirectActionRequest(
    const LanGatewayFrame& action, const uint8_t* paramsJson,
    size_t paramsJsonBytes, int64_t executionExpiresEpoch, uint8_t* output,
    size_t outputCapacity, size_t& outputBytes) {
  outputBytes = 0U;
  if (!paramsJson || paramsJsonBytes == 0U ||
      paramsJsonBytes > kLanMaximumActionParamsBytes || !output ||
      outputCapacity == 0U ||
      action.kind != LanFrameKind::RemoteAction ||
      allZero(action.actionId, sizeof(action.actionId)) ||
      executionExpiresEpoch < kBleActionMinimumTrustedEpoch ||
      executionExpiresEpoch > kBleActionMaximumTrustedEpoch ||
      executionExpiresEpoch > action.expiresEpoch) {
    return GatewayLanActionBridgeResult::InvalidArgument;
  }

  const char* directKind = nullptr;
  bool message = false;
  if (strcmp(action.actionType, "companion.pet") == 0) {
    directKind = "pet";
  } else if (strcmp(action.actionType, "companion.feed") == 0) {
    directKind = "feed";
  } else if (strcmp(action.actionType, "companion.play") == 0) {
    directKind = "play";
  } else if (strcmp(action.actionType, "companion.listen_once") == 0) {
    directKind = "listen_once";
  } else if (strcmp(action.actionType, "message.send") == 0) {
    directKind = "send_message";
    message = true;
  } else {
    return GatewayLanActionBridgeResult::UnsupportedAction;
  }
  if ((strcmp(directKind, "pet") == 0 || strcmp(directKind, "feed") == 0 ||
       strcmp(directKind, "play") == 0) &&
      !exactBytes(paramsJson, paramsJsonBytes, "{}")) {
    return GatewayLanActionBridgeResult::InvalidParameters;
  }

  char actionId[37]{};
  formatUuid(action.actionId, actionId);
  const int prefixBytes = snprintf(
      reinterpret_cast<char*>(output), outputCapacity,
      "{\"action_id\":\"%s\",\"kind\":\"%s\"," 
      "\"expires_at_epoch\":%lu,\"params\":",
      actionId, directKind,
      static_cast<unsigned long>(executionExpiresEpoch));
  if (prefixBytes <= 0 ||
      static_cast<size_t>(prefixBytes) >= outputCapacity) {
    if (outputCapacity != 0U) output[0] = 0U;
    return GatewayLanActionBridgeResult::OutputTooSmall;
  }
  size_t cursor = static_cast<size_t>(prefixBytes);
  bool appended = false;
  if (!message) {
    appended = appendBytes(output, outputCapacity, cursor, paramsJson,
                           paramsJsonBytes);
  } else {
    static const uint8_t targetToken[] = "\"target\":";
    size_t targetAt = paramsJsonBytes;
    size_t matches = 0U;
    for (size_t i = 0U; i + sizeof(targetToken) - 1U <= paramsJsonBytes;
         ++i) {
      if (memcmp(paramsJson + i, targetToken,
                 sizeof(targetToken) - 1U) == 0) {
        targetAt = i;
        ++matches;
      }
    }
    static const uint8_t directTargetToken[] = "\"target_id\":";
    appended = matches == 1U &&
        appendBytes(output, outputCapacity, cursor, paramsJson, targetAt) &&
        appendBytes(output, outputCapacity, cursor, directTargetToken,
                    sizeof(directTargetToken) - 1U) &&
        appendBytes(output, outputCapacity, cursor,
                    paramsJson + targetAt + sizeof(targetToken) - 1U,
                    paramsJsonBytes - targetAt -
                        (sizeof(targetToken) - 1U));
  }
  static const uint8_t close = '}';
  if (!appended ||
      !appendBytes(output, outputCapacity, cursor, &close, 1U)) {
    secureZero(output, outputCapacity);
    return GatewayLanActionBridgeResult::OutputTooSmall;
  }

  BleActionCommand verified{};
  const BleActionDecodeResult decoded =
      decodeBleActionCommand(output, cursor, verified);
  secureZero(&verified, sizeof(verified));
  if (decoded != BleActionDecodeResult::Ok) {
    secureZero(output, outputCapacity);
    return GatewayLanActionBridgeResult::InvalidParameters;
  }
  outputBytes = cursor;
  return GatewayLanActionBridgeResult::Ok;
}

bool GatewayLanActionDispatcher::begin(
    DurableGatewayLanActionReplayStore& outcomes,
    GatewayLanDirectActionExecutor& executor,
    GatewayLanDevicePayloadQueue& payloads) {
  stop();
  if (!outcomes.status().begun) return false;
  outcomes_ = &outcomes;
  executor_ = &executor;
  payloads_ = &payloads;
  return true;
}

void GatewayLanActionDispatcher::stop() {
  outcomes_ = nullptr;
  executor_ = nullptr;
  payloads_ = nullptr;
}

bool GatewayLanActionDispatcher::acceptAuthenticatedAction(
    const uint8_t* framedJson, size_t framedJsonBytes,
    const LanGatewayFrame& metadata, const uint8_t* paramsJson,
    size_t paramsJsonBytes, int64_t acceptedEpoch) {
  return handle(framedJson, framedJsonBytes, metadata, paramsJson,
                paramsJsonBytes, acceptedEpoch, false);
}

bool GatewayLanActionDispatcher::repeatAuthenticatedAction(
    const uint8_t* framedJson, size_t framedJsonBytes,
    const LanGatewayFrame& metadata, const uint8_t* paramsJson,
    size_t paramsJsonBytes, int64_t repeatedEpoch) {
  return handle(framedJson, framedJsonBytes, metadata, paramsJson,
                paramsJsonBytes, repeatedEpoch, true);
}

bool GatewayLanActionDispatcher::handle(
    const uint8_t* framedJson, size_t framedJsonBytes,
    const LanGatewayFrame& metadata, const uint8_t* paramsJson,
    size_t paramsJsonBytes, int64_t nowEpoch, bool duplicate) {
  if (!outcomes_ || !executor_ || !payloads_ || !framedJson ||
      framedJsonBytes == 0U || framedJsonBytes > kLanMaximumFrameBytes ||
      !paramsJson || paramsJsonBytes == 0U ||
      paramsJsonBytes > kLanMaximumActionParamsBytes ||
      !validEpoch(nowEpoch) ||
      metadata.kind != LanFrameKind::RemoteAction) {
    return false;
  }

  GatewayLanStoredActionOutcome stored{};
  if (!outcomes_->outcome(metadata.actionId, stored)) return false;
  if (duplicate) {
    if (stored.status == GatewayLanActionOutcomeStatus::Pending) {
      if (!outcomes_->recordOutcome(
              metadata.actionId, GatewayLanActionOutcomeStatus::Failed,
              nowEpoch, "result_unknown") ||
          !outcomes_->outcome(metadata.actionId, stored)) {
        return false;
      }
    }
    return emit(stored, nowEpoch);
  }
  if (stored.status != GatewayLanActionOutcomeStatus::Pending) {
    // The decoder said Fresh, so an already-final outcome indicates a broken
    // replay-store contract. Never execute through contradictory state.
    return false;
  }

  if (!payloads_->canEnqueue(2U, 2U * kGatewayLanActionPayloadBytes)) {
    (void)outcomes_->recordOutcome(
        metadata.actionId, GatewayLanActionOutcomeStatus::Rejected,
        nowEpoch, "queue_full");
    return false;
  }

  const size_t requestCapacity = paramsJsonBytes + 256U;
  uint8_t* request = static_cast<uint8_t*>(malloc(requestCapacity));
  GatewayLanDirectActionOutcome direct{};
  if (!request) {
    direct.status = GatewayLanActionOutcomeStatus::Failed;
    copyResultCode("out_of_memory", direct.code);
  } else {
    secureZero(request, requestCapacity);
    size_t requestBytes = 0U;
    // The backend's signed transport deadline may legitimately be longer
    // than the direct action path's deliberately narrow replay window. Keep
    // the original signed expiry in the durable LAN ledger, but derive a
    // non-refreshable local execution deadline accepted by KRA3.
    const int64_t directWindowEnd =
        nowEpoch + static_cast<int64_t>(kBleActionMaximumExpirySeconds);
    const int64_t executionExpiresEpoch =
        metadata.expiresEpoch < directWindowEnd
            ? metadata.expiresEpoch
            : directWindowEnd;
    const GatewayLanActionBridgeResult bridged =
        encodeGatewayLanDirectActionRequest(
            metadata, paramsJson, paramsJsonBytes, executionExpiresEpoch,
            request, requestCapacity, requestBytes);
    if (bridged == GatewayLanActionBridgeResult::Ok) {
      if (!executor_->executeDirectAction(request, requestBytes, direct) ||
          direct.status == GatewayLanActionOutcomeStatus::Pending ||
          !validResultCode(direct.code)) {
        direct = GatewayLanDirectActionOutcome{};
        direct.status = GatewayLanActionOutcomeStatus::Failed;
        copyResultCode("result_unknown", direct.code);
      }
    } else {
      direct.status = GatewayLanActionOutcomeStatus::Rejected;
      copyResultCode(
          bridged == GatewayLanActionBridgeResult::UnsupportedAction
              ? "action_unavailable"
              : "invalid_params",
          direct.code);
    }
    secureZero(request, requestCapacity);
    free(request);
  }

  if (!outcomes_->recordOutcome(metadata.actionId, direct.status, nowEpoch,
                                direct.code) ||
      !outcomes_->outcome(metadata.actionId, stored)) {
    return false;
  }
  return emit(stored, nowEpoch);
}

bool GatewayLanActionDispatcher::emit(
    const GatewayLanStoredActionOutcome& outcome, int64_t issuedEpoch) {
  const char* statusName = outcomeStatusName(outcome.status);
  if (!payloads_ || !validEpoch(issuedEpoch) || !statusName ||
      !validEpoch(outcome.acceptedEpoch) ||
      !validEpoch(outcome.completedEpoch) ||
      !validResultCode(outcome.code) ||
      !payloads_->canEnqueue(2U, 2U * kGatewayLanActionPayloadBytes)) {
    return false;
  }
  char actionId[37]{};
  formatUuid(outcome.actionId, actionId);
  uint8_t acceptance[kGatewayLanActionPayloadBytes]{};
  uint8_t result[kGatewayLanActionPayloadBytes]{};
  const int acceptanceBytes = snprintf(
      reinterpret_cast<char*>(acceptance), sizeof(acceptance),
      "{\"type\":\"action_acceptance\",\"action_id\":\"%s\"," 
      "\"accepted_epoch\":%lld}",
      actionId, static_cast<long long>(outcome.acceptedEpoch));
  const int resultBytes = snprintf(
      reinterpret_cast<char*>(result), sizeof(result),
      "{\"type\":\"action_result\",\"action_id\":\"%s\"," 
      "\"status\":\"%s\",\"completed_epoch\":%lld," 
      "\"result\":{\"code\":\"%s\"}}",
      actionId, statusName, static_cast<long long>(outcome.completedEpoch),
      outcome.code);
  const bool encoded = acceptanceBytes > 0 && resultBytes > 0 &&
      static_cast<size_t>(acceptanceBytes) < sizeof(acceptance) &&
      static_cast<size_t>(resultBytes) < sizeof(result);
  const bool queued = encoded &&
      payloads_->enqueue("action_acceptance", acceptance,
                         static_cast<size_t>(acceptanceBytes), issuedEpoch) &&
      payloads_->enqueue("action_result", result,
                         static_cast<size_t>(resultBytes), issuedEpoch);
  secureZero(acceptance, sizeof(acceptance));
  secureZero(result, sizeof(result));
  return queued;
}

}  // namespace connectivity
}  // namespace kitsu868
