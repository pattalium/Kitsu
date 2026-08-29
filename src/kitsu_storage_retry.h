#pragma once

#include <stdint.h>

namespace kitsu868 {
namespace connectivity {

constexpr uint32_t kStorageRetryDelaysMs[] = {
    5000UL,
    30000UL,
    2UL * 60UL * 1000UL,
    10UL * 60UL * 1000UL,
    30UL * 60UL * 1000UL,
};
constexpr uint8_t kStorageRetryDelayCount =
    sizeof(kStorageRetryDelaysMs) / sizeof(kStorageRetryDelaysMs[0]);

enum class StorageRetryFailure : uint8_t {
  Transient = 0,
  NoSpace,
};

struct StorageRetryStatus {
  bool dirty = false;
  bool blockedNoSpace = false;
  uint8_t failureCount = 0U;
  uint32_t dueAt = 0U;
  uint32_t lastAttemptAt = 0U;
};

// A wrap-safe, bounded write retry schedule. New data can bring an untouched
// debounce deadline forward, but can never bypass an existing failure backoff
// or a latched no-space result.
class StorageRetrySchedule {
 public:
  void reset();
  void markDirty(uint32_t now, uint32_t initialDelayMs);
  bool attemptDue(uint32_t now) const;
  void recordAttempt(uint32_t now);
  void recordSuccess();
  void recordFailure(uint32_t now, StorageRetryFailure failure);
  void rearmAfterHeadroom(uint32_t now, uint32_t initialDelayMs);
  StorageRetryStatus status() const;

 private:
  bool dirty_ = false;
  bool blockedNoSpace_ = false;
  uint8_t failureCount_ = 0U;
  uint32_t dueAt_ = 0U;
  uint32_t lastAttemptAt_ = 0U;
};

}  // namespace connectivity
}  // namespace kitsu868
