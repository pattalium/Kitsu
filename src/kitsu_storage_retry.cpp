#include "kitsu_storage_retry.h"

namespace kitsu868 {
namespace connectivity {
namespace {

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace

void StorageRetrySchedule::reset() {
  dirty_ = false;
  blockedNoSpace_ = false;
  failureCount_ = 0U;
  dueAt_ = 0U;
  lastAttemptAt_ = 0U;
}

void StorageRetrySchedule::markDirty(uint32_t now,
                                     uint32_t initialDelayMs) {
  const uint32_t candidate = now + initialDelayMs;
  if (!dirty_) {
    dirty_ = true;
    dueAt_ = candidate;
    return;
  }
  if (blockedNoSpace_ || failureCount_ != 0U) return;
  const uint32_t remaining = dueAt_ - now;
  if (deadlineReached(now, dueAt_) || initialDelayMs < remaining) {
    dueAt_ = candidate;
  }
}

bool StorageRetrySchedule::attemptDue(uint32_t now) const {
  return dirty_ && !blockedNoSpace_ && deadlineReached(now, dueAt_);
}

void StorageRetrySchedule::recordAttempt(uint32_t now) {
  lastAttemptAt_ = now;
}

void StorageRetrySchedule::recordSuccess() { reset(); }

void StorageRetrySchedule::recordFailure(uint32_t now,
                                         StorageRetryFailure failure) {
  dirty_ = true;
  if (failure == StorageRetryFailure::NoSpace) {
    blockedNoSpace_ = true;
    dueAt_ = 0U;
    return;
  }
  if (failureCount_ < kStorageRetryDelayCount) ++failureCount_;
  const uint8_t delayIndex = failureCount_ == 0U
      ? 0U
      : static_cast<uint8_t>(failureCount_ - 1U);
  dueAt_ = now + kStorageRetryDelaysMs[delayIndex];
}

void StorageRetrySchedule::rearmAfterHeadroom(
    uint32_t now, uint32_t initialDelayMs) {
  if (!dirty_ || !blockedNoSpace_) return;
  blockedNoSpace_ = false;
  failureCount_ = 0U;
  dueAt_ = now + initialDelayMs;
}

StorageRetryStatus StorageRetrySchedule::status() const {
  StorageRetryStatus output{};
  output.dirty = dirty_;
  output.blockedNoSpace = blockedNoSpace_;
  output.failureCount = failureCount_;
  output.dueAt = dueAt_;
  output.lastAttemptAt = lastAttemptAt_;
  return output;
}

}  // namespace connectivity
}  // namespace kitsu868
