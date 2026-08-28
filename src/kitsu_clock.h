#pragma once

#include <stddef.h>
#include <stdint.h>

// Portable wall-clock state for Kitsu. This module deliberately owns no
// network, libc clock, display, button, Arduino String, or Preferences code.
// Callers provide a uint32 millis() sample and persist ClockSnapshot bytes in
// their existing authenticated/double-buffered storage adapter.
namespace kitsu868 {
namespace timekeeping {

constexpr uint64_t kMinimumClockUnixSeconds = UINT64_C(1704067200);
constexpr uint64_t kMaximumClockUnixSeconds = UINT64_C(4102444800);
constexpr int16_t kMinimumUtcOffsetMinutes = -840;
constexpr int16_t kMaximumUtcOffsetMinutes = 840;
constexpr size_t kClockIso8601BufferBytes = 30U;
constexpr uint8_t kClockSnapshotSchemaVersion = 1U;
constexpr uint8_t kClockSnapshotBytes = 32U;

enum class ClockSource : uint8_t {
  None = 0,
  ManualSerial,
  ManualDevice,
  NetworkTime,
  AuthenticatedApp,
};

enum class ClockTrust : uint8_t {
  Unset = 0,
  RestoredStale,
  Trusted,
};

enum class ClockResult : uint8_t {
  Ok = 0,
  NotSet,
  InvalidArgument,
  InvalidSource,
  InvalidIso8601,
  InvalidUnixText,
  OutOfRange,
  NetworkTimeNotSynchronized,
  MonotonicDiscontinuity,
  BadSnapshotMagic,
  UnsupportedSnapshot,
  CorruptSnapshot,
  InvalidSnapshot,
};

const char* clockSourceName(ClockSource source);
const char* clockTrustName(ClockTrust trust);
const char* clockResultName(ClockResult result);

struct ParsedIso8601 {
  uint64_t unixSeconds = 0U;
  uint16_t millisecond = 0U;
  int16_t utcOffsetMinutes = 0;
};

// synchronized must only be true after the platform NTP/SNTP implementation
// has completed its own validity and stability checks. The clock core does no
// DNS, UDP, server selection, or packet authentication itself.
struct NetworkTimeSample {
  uint64_t unixSeconds = 0U;
  uint16_t millisecond = 0U;
  bool synchronized = false;
};

struct ClockReading {
  uint64_t unixSeconds = 0U;
  uint16_t millisecond = 0U;
  int16_t utcOffsetMinutes = 0;
  ClockSource source = ClockSource::None;
  ClockTrust trust = ClockTrust::Unset;
  bool monotonicHealthy = false;

  bool set() const { return trust != ClockTrust::Unset; }
  bool trusted() const { return trust == ClockTrust::Trusted; }
};

// Fixed, canonical little-endian persistence record. A CRC catches torn or
// corrupt storage; it is not an authenticity mechanism. Store this record in
// Kitsu's existing authenticated storage envelope. On restore, a valid record
// is always downgraded to RestoredStale because millis() cannot reveal how
// long power was absent.
#pragma pack(push, 1)
struct ClockSnapshot {
  uint32_t magic = UINT32_C(0x314C434B);  // "KCL1" little-endian.
  uint8_t schemaVersion = kClockSnapshotSchemaVersion;
  uint8_t bytes = kClockSnapshotBytes;
  uint8_t source = static_cast<uint8_t>(ClockSource::None);
  uint8_t savedTrust = static_cast<uint8_t>(ClockTrust::Unset);
  uint32_t generation = 0U;
  uint16_t millisecond = 0U;
  int16_t utcOffsetMinutes = 0;
  uint64_t unixSeconds = 0U;
  uint8_t reserved[4]{};
  uint32_t crc32 = 0U;
};
#pragma pack(pop)

static_assert(sizeof(ClockSnapshot) == kClockSnapshotBytes,
              "Clock persistence schema changed unexpectedly");

class KitsuClock {
 public:
  KitsuClock();

  void reset();

  // ISO-8601 accepts exactly YYYY-MM-DDTHH:MM:SS[.sss](Z|+HH:MM|-HH:MM),
  // with one to three fractional digits. A zone is mandatory so local input
  // is never silently interpreted as UTC.
  ClockResult setFromIso8601(const char* input, ClockSource source,
                             uint32_t nowMillis);

  // Decimal Unix text is unsigned and unadorned: no sign or whitespace.
  // ManualSerial, ManualDevice and AuthenticatedApp are accepted here;
  // NetworkTime must enter through acceptNtp()/acceptNetworkTime().
  // Source values are provenance assertions: the integration must authorize
  // its serial/app command before calling these setters.
  ClockResult setFromUnixText(const char* input, ClockSource source,
                              int16_t utcOffsetMinutes,
                              uint32_t nowMillis);
  ClockResult setFromUnixSeconds(uint64_t unixSeconds,
                                 uint16_t millisecond,
                                 ClockSource source,
                                 int16_t utcOffsetMinutes,
                                 uint32_t nowMillis);

  ClockResult acceptNtp(const NetworkTimeSample& sample,
                        uint32_t nowMillis);
  // Semantic alias for integrations whose platform service is named SNTP or
  // NetworkTime rather than NTP.
  ClockResult acceptNetworkTime(const NetworkTimeSample& sample,
                                uint32_t nowMillis) {
    return acceptNtp(sample, nowMillis);
  }

  // Changes display/editor offset only. UTC continuity is unaffected.
  ClockResult setUtcOffsetMinutes(int16_t utcOffsetMinutes);

  // read() advances from the last millis sample. Successive samples may span
  // uint32 rollover. Calls must be less than 2^31 ms apart (about 24.8 days),
  // the only unambiguous ordering window for a wrapping 32-bit counter.
  ClockResult read(uint32_t nowMillis, ClockReading& output);

  bool set() const { return trust_ != ClockTrust::Unset; }
  bool trusted() const { return trust_ == ClockTrust::Trusted; }
  ClockSource source() const { return source_; }
  ClockTrust trust() const { return trust_; }
  int16_t utcOffsetMinutes() const { return utcOffsetMinutes_; }
  bool monotonicHealthy() const { return monotonicHealthy_; }
  uint32_t restoredGeneration() const { return restoredGeneration_; }
  ClockResult lastResult() const { return lastResult_; }

  // generation is owned by the storage adapter, allowing it to select and
  // atomically replace redundant slots without hidden writes in this module.
  ClockResult makeSnapshot(uint32_t nowMillis, uint32_t generation,
                           ClockSnapshot& output);
  ClockResult restore(const ClockSnapshot& snapshot,
                      uint32_t nowMillis);

  static ClockResult validateSnapshot(const ClockSnapshot& snapshot);
  static bool generationAfter(uint32_t candidate, uint32_t reference);
  static ClockResult parseIso8601(const char* input,
                                  ParsedIso8601& output);
  static ClockResult parseUnixText(const char* input,
                                   uint64_t& unixSeconds);
  static ClockResult formatIso8601(uint64_t unixSeconds,
                                   uint16_t millisecond,
                                   int16_t utcOffsetMinutes,
                                   char* output, size_t outputBytes);

 private:
  ClockResult setAnchor(uint64_t unixSeconds, uint16_t millisecond,
                        ClockSource source, int16_t utcOffsetMinutes,
                        uint32_t nowMillis);
  ClockResult advance(uint32_t nowMillis);

  uint64_t unixSeconds_ = 0U;
  uint16_t millisecond_ = 0U;
  int16_t utcOffsetMinutes_ = 0;
  uint32_t lastMillis_ = 0U;
  uint32_t restoredGeneration_ = 0U;
  ClockSource source_ = ClockSource::None;
  ClockTrust trust_ = ClockTrust::Unset;
  ClockResult lastResult_ = ClockResult::NotSet;
  bool monotonicHealthy_ = false;
};

constexpr uint16_t kClockEditorMinimumYear = 2024U;
constexpr uint16_t kClockEditorMaximumYear = 2099U;
constexpr int16_t kClockEditorMinimumOffsetMinutes = -720;
constexpr int16_t kClockEditorMaximumOffsetMinutes = 840;
constexpr int16_t kClockEditorOffsetStepMinutes = 15;

enum class ClockEditorField : uint8_t {
  Inactive = 0,
  Year,
  Month,
  Day,
  Hour,
  Minute,
  UtcOffset,
  Review,
};

enum class ClockEditorEvent : uint8_t {
  Ignored = 0,
  Changed,
  Advanced,
  CommitRequested,
};

struct ClockEditorView {
  bool active = false;
  bool commitReady = false;
  ClockEditorField field = ClockEditorField::Inactive;
  uint16_t year = kClockEditorMinimumYear;
  uint8_t month = 1U;
  uint8_t day = 1U;
  uint8_t hour = 0U;
  uint8_t minute = 0U;
  int16_t utcOffsetMinutes = 0;
};

// One physical button is enough: a short press increments the selected field,
// a long press advances it, and a long press on Review requests commit. A
// short press on Review returns to Year for corrections. The caller owns
// debounce/gesture timing and may cancel on timeout or a separate menu exit.
class OneButtonClockEditor {
 public:
  OneButtonClockEditor();

  ClockResult begin(uint64_t seedUnixSeconds,
                    int16_t utcOffsetMinutes);
  void cancel();
  ClockEditorEvent shortPress();
  ClockEditorEvent longPress();
  ClockEditorView view() const { return view_; }

  // Available only after CommitRequested. The returned epoch has seconds and
  // milliseconds set to zero and is ready for ManualDevice anchoring.
  bool value(uint64_t& unixSeconds, int16_t& utcOffsetMinutes) const;

 private:
  void clampDay();

  ClockEditorView view_{};
};

}  // namespace timekeeping
}  // namespace kitsu868
