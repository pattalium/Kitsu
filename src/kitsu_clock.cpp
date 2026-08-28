#include "kitsu_clock.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace kitsu868 {
namespace timekeeping {
namespace {

constexpr uint32_t kSnapshotMagic = UINT32_C(0x314C434B);

bool validSource(ClockSource source) {
  return source >= ClockSource::ManualSerial &&
         source <= ClockSource::AuthenticatedApp;
}

bool validDirectSource(ClockSource source) {
  return source == ClockSource::ManualSerial ||
         source == ClockSource::ManualDevice ||
         source == ClockSource::AuthenticatedApp;
}

bool validOffset(int16_t offset) {
  return offset >= kMinimumUtcOffsetMinutes &&
         offset <= kMaximumUtcOffsetMinutes;
}

bool validInstant(uint64_t unixSeconds, uint16_t millisecond) {
  if (unixSeconds < kMinimumClockUnixSeconds ||
      unixSeconds > kMaximumClockUnixSeconds || millisecond > 999U) {
    return false;
  }
  return unixSeconds != kMaximumClockUnixSeconds || millisecond == 0U;
}

bool leapYear(uint16_t year) {
  return (year % 4U) == 0U &&
         ((year % 100U) != 0U || (year % 400U) == 0U);
}

uint8_t monthDays(uint16_t year, uint8_t month) {
  static const uint8_t kDays[12] = {
      31U, 28U, 31U, 30U, 31U, 30U,
      31U, 31U, 30U, 31U, 30U, 31U,
  };
  if (month < 1U || month > 12U) return 0U;
  if (month == 2U && leapYear(year)) return 29U;
  return kDays[month - 1U];
}

// Howard Hinnant's civil-calendar transformations, expressed with fixed-width
// integers. Epoch day zero is 1970-01-01. No libc timezone state is consulted.
int64_t daysFromCivil(int32_t year, uint8_t month, uint8_t day) {
  year -= month <= 2U ? 1 : 0;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const uint32_t yearOfEra =
      static_cast<uint32_t>(year - era * 400);
  const uint32_t adjustedMonth = month > 2U
                                     ? static_cast<uint32_t>(month - 3U)
                                     : static_cast<uint32_t>(month + 9U);
  const uint32_t dayOfYear =
      (153U * adjustedMonth + 2U) / 5U + day - 1U;
  const uint32_t dayOfEra = yearOfEra * 365U + yearOfEra / 4U -
                            yearOfEra / 100U + dayOfYear;
  return static_cast<int64_t>(era) * 146097LL +
         static_cast<int64_t>(dayOfEra) - 719468LL;
}

void civilFromDays(int64_t days, uint16_t& year, uint8_t& month,
                   uint8_t& day) {
  days += 719468LL;
  const int64_t era = (days >= 0 ? days : days - 146096LL) / 146097LL;
  const uint32_t dayOfEra =
      static_cast<uint32_t>(days - era * 146097LL);
  const uint32_t yearOfEra =
      (dayOfEra - dayOfEra / 1460U + dayOfEra / 36524U -
       dayOfEra / 146096U) /
      365U;
  int32_t resultYear =
      static_cast<int32_t>(yearOfEra) + static_cast<int32_t>(era * 400LL);
  const uint32_t dayOfYear =
      dayOfEra - (365U * yearOfEra + yearOfEra / 4U -
                  yearOfEra / 100U);
  const uint32_t monthPrime = (5U * dayOfYear + 2U) / 153U;
  day = static_cast<uint8_t>(
      dayOfYear - (153U * monthPrime + 2U) / 5U + 1U);
  const int32_t resultMonth =
      static_cast<int32_t>(monthPrime) + (monthPrime < 10U ? 3 : -9);
  resultYear += resultMonth <= 2 ? 1 : 0;
  year = static_cast<uint16_t>(resultYear);
  month = static_cast<uint8_t>(resultMonth);
}

bool civilToUnix(uint16_t year, uint8_t month, uint8_t day,
                 uint8_t hour, uint8_t minute, uint8_t second,
                 int16_t utcOffsetMinutes, uint64_t& unixSeconds) {
  if (month < 1U || month > 12U || day < 1U ||
      day > monthDays(year, month) || hour > 23U || minute > 59U ||
      second > 59U || !validOffset(utcOffsetMinutes)) {
    return false;
  }
  const int64_t localSeconds =
      daysFromCivil(static_cast<int32_t>(year), month, day) * 86400LL +
      static_cast<int64_t>(hour) * 3600LL +
      static_cast<int64_t>(minute) * 60LL + second;
  const int64_t utcSeconds =
      localSeconds - static_cast<int64_t>(utcOffsetMinutes) * 60LL;
  if (utcSeconds < 0) return false;
  unixSeconds = static_cast<uint64_t>(utcSeconds);
  return true;
}

void unixToCivil(uint64_t unixSeconds, int16_t utcOffsetMinutes,
                 uint16_t& year, uint8_t& month, uint8_t& day,
                 uint8_t& hour, uint8_t& minute, uint8_t& second) {
  const int64_t localSeconds =
      static_cast<int64_t>(unixSeconds) +
      static_cast<int64_t>(utcOffsetMinutes) * 60LL;
  int64_t days = localSeconds / 86400LL;
  int64_t secondsOfDay = localSeconds % 86400LL;
  if (secondsOfDay < 0) {
    secondsOfDay += 86400LL;
    --days;
  }
  civilFromDays(days, year, month, day);
  hour = static_cast<uint8_t>(secondsOfDay / 3600LL);
  secondsOfDay %= 3600LL;
  minute = static_cast<uint8_t>(secondsOfDay / 60LL);
  second = static_cast<uint8_t>(secondsOfDay % 60LL);
}

bool parseDigits(const char*& cursor, uint8_t count, uint32_t& value) {
  value = 0U;
  for (uint8_t index = 0U; index < count; ++index) {
    const char digit = cursor[index];
    if (digit < '0' || digit > '9') return false;
    value = value * 10U + static_cast<uint32_t>(digit - '0');
  }
  cursor += count;
  return true;
}

bool consume(const char*& cursor, char expected) {
  if (*cursor != expected) return false;
  ++cursor;
  return true;
}

uint32_t snapshotCrc(const ClockSnapshot& snapshot) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&snapshot);
  const size_t length = offsetof(ClockSnapshot, crc32);
  uint32_t crc = UINT32_C(0xFFFFFFFF);
  for (size_t index = 0U; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
    }
  }
  return ~crc;
}

}  // namespace

const char* clockSourceName(ClockSource source) {
  switch (source) {
    case ClockSource::ManualSerial: return "manual_serial";
    case ClockSource::ManualDevice: return "manual_device";
    case ClockSource::NetworkTime: return "network_time";
    case ClockSource::AuthenticatedApp: return "authenticated_app";
    case ClockSource::None: break;
  }
  return "none";
}

const char* clockTrustName(ClockTrust trust) {
  switch (trust) {
    case ClockTrust::RestoredStale: return "restored_stale";
    case ClockTrust::Trusted: return "trusted";
    case ClockTrust::Unset: break;
  }
  return "unset";
}

const char* clockResultName(ClockResult result) {
  switch (result) {
    case ClockResult::Ok: return "ok";
    case ClockResult::NotSet: return "not_set";
    case ClockResult::InvalidArgument: return "invalid_argument";
    case ClockResult::InvalidSource: return "invalid_source";
    case ClockResult::InvalidIso8601: return "invalid_iso8601";
    case ClockResult::InvalidUnixText: return "invalid_unix_text";
    case ClockResult::OutOfRange: return "out_of_range";
    case ClockResult::NetworkTimeNotSynchronized:
      return "network_time_not_synchronized";
    case ClockResult::MonotonicDiscontinuity:
      return "monotonic_discontinuity";
    case ClockResult::BadSnapshotMagic: return "bad_snapshot_magic";
    case ClockResult::UnsupportedSnapshot: return "unsupported_snapshot";
    case ClockResult::CorruptSnapshot: return "corrupt_snapshot";
    case ClockResult::InvalidSnapshot: return "invalid_snapshot";
  }
  return "invalid_argument";
}

KitsuClock::KitsuClock() { reset(); }

void KitsuClock::reset() {
  unixSeconds_ = 0U;
  millisecond_ = 0U;
  utcOffsetMinutes_ = 0;
  lastMillis_ = 0U;
  restoredGeneration_ = 0U;
  source_ = ClockSource::None;
  trust_ = ClockTrust::Unset;
  lastResult_ = ClockResult::NotSet;
  monotonicHealthy_ = false;
}

ClockResult KitsuClock::setAnchor(uint64_t unixSeconds,
                                  uint16_t millisecond,
                                  ClockSource source,
                                  int16_t utcOffsetMinutes,
                                  uint32_t nowMillis) {
  if (!validDirectSource(source)) {
    lastResult_ = ClockResult::InvalidSource;
    return lastResult_;
  }
  if (!validOffset(utcOffsetMinutes)) {
    lastResult_ = ClockResult::OutOfRange;
    return lastResult_;
  }
  if (!validInstant(unixSeconds, millisecond)) {
    lastResult_ = ClockResult::OutOfRange;
    return lastResult_;
  }
  unixSeconds_ = unixSeconds;
  millisecond_ = millisecond;
  utcOffsetMinutes_ = utcOffsetMinutes;
  lastMillis_ = nowMillis;
  source_ = source;
  trust_ = ClockTrust::Trusted;
  monotonicHealthy_ = true;
  lastResult_ = ClockResult::Ok;
  return lastResult_;
}

ClockResult KitsuClock::setFromIso8601(const char* input,
                                       ClockSource source,
                                       uint32_t nowMillis) {
  if (!validDirectSource(source)) {
    lastResult_ = ClockResult::InvalidSource;
    return lastResult_;
  }
  ParsedIso8601 parsed{};
  const ClockResult parsedResult = parseIso8601(input, parsed);
  if (parsedResult != ClockResult::Ok) {
    lastResult_ = parsedResult;
    return lastResult_;
  }
  return setAnchor(parsed.unixSeconds, parsed.millisecond, source,
                   parsed.utcOffsetMinutes, nowMillis);
}

ClockResult KitsuClock::setFromUnixText(const char* input,
                                        ClockSource source,
                                        int16_t utcOffsetMinutes,
                                        uint32_t nowMillis) {
  if (!validDirectSource(source)) {
    lastResult_ = ClockResult::InvalidSource;
    return lastResult_;
  }
  uint64_t unixSeconds = 0U;
  const ClockResult parsedResult = parseUnixText(input, unixSeconds);
  if (parsedResult != ClockResult::Ok) {
    lastResult_ = parsedResult;
    return lastResult_;
  }
  return setAnchor(unixSeconds, 0U, source, utcOffsetMinutes, nowMillis);
}

ClockResult KitsuClock::setFromUnixSeconds(uint64_t unixSeconds,
                                           uint16_t millisecond,
                                           ClockSource source,
                                           int16_t utcOffsetMinutes,
                                           uint32_t nowMillis) {
  return setAnchor(unixSeconds, millisecond, source, utcOffsetMinutes,
                   nowMillis);
}

ClockResult KitsuClock::acceptNtp(const NetworkTimeSample& sample,
                                  uint32_t nowMillis) {
  if (!sample.synchronized) {
    lastResult_ = ClockResult::NetworkTimeNotSynchronized;
    return lastResult_;
  }
  if (!validInstant(sample.unixSeconds, sample.millisecond)) {
    lastResult_ = ClockResult::OutOfRange;
    return lastResult_;
  }
  unixSeconds_ = sample.unixSeconds;
  millisecond_ = sample.millisecond;
  lastMillis_ = nowMillis;
  source_ = ClockSource::NetworkTime;
  trust_ = ClockTrust::Trusted;
  monotonicHealthy_ = true;
  lastResult_ = ClockResult::Ok;
  return lastResult_;
}

ClockResult KitsuClock::setUtcOffsetMinutes(int16_t utcOffsetMinutes) {
  if (!validOffset(utcOffsetMinutes)) {
    lastResult_ = ClockResult::OutOfRange;
    return lastResult_;
  }
  utcOffsetMinutes_ = utcOffsetMinutes;
  lastResult_ = ClockResult::Ok;
  return lastResult_;
}

ClockResult KitsuClock::advance(uint32_t nowMillis) {
  if (!set()) {
    lastResult_ = ClockResult::NotSet;
    return lastResult_;
  }

  const uint32_t delta = nowMillis - lastMillis_;
  // Serial-number arithmetic is unambiguous only inside half the uint32
  // range. A larger delta looks like a reset/backwards jump, not a rollover.
  if (delta > static_cast<uint32_t>(INT32_MAX)) {
    lastMillis_ = nowMillis;
    trust_ = ClockTrust::RestoredStale;
    monotonicHealthy_ = false;
    lastResult_ = ClockResult::MonotonicDiscontinuity;
    return lastResult_;
  }

  const uint64_t totalMilliseconds =
      static_cast<uint64_t>(millisecond_) + delta;
  const uint64_t addedSeconds = totalMilliseconds / 1000U;
  const uint16_t nextMillisecond =
      static_cast<uint16_t>(totalMilliseconds % 1000U);
  if (addedSeconds > kMaximumClockUnixSeconds - unixSeconds_ ||
      (unixSeconds_ + addedSeconds == kMaximumClockUnixSeconds &&
       nextMillisecond != 0U)) {
    unixSeconds_ = kMaximumClockUnixSeconds;
    millisecond_ = 0U;
    lastMillis_ = nowMillis;
    trust_ = ClockTrust::RestoredStale;
    monotonicHealthy_ = false;
    lastResult_ = ClockResult::OutOfRange;
    return lastResult_;
  }

  unixSeconds_ += addedSeconds;
  millisecond_ = nextMillisecond;
  lastMillis_ = nowMillis;
  lastResult_ = ClockResult::Ok;
  return lastResult_;
}

ClockResult KitsuClock::read(uint32_t nowMillis, ClockReading& output) {
  const ClockResult result = advance(nowMillis);
  output = ClockReading{};
  output.utcOffsetMinutes = utcOffsetMinutes_;
  output.source = source_;
  output.trust = trust_;
  output.monotonicHealthy = monotonicHealthy_;
  if (set()) {
    output.unixSeconds = unixSeconds_;
    output.millisecond = millisecond_;
  }
  return result;
}

ClockResult KitsuClock::makeSnapshot(uint32_t nowMillis,
                                     uint32_t generation,
                                     ClockSnapshot& output) {
  const ClockResult advanced = advance(nowMillis);
  if (advanced != ClockResult::Ok) return advanced;

  ClockSnapshot candidate{};
  candidate.source = static_cast<uint8_t>(source_);
  candidate.savedTrust = static_cast<uint8_t>(trust_);
  candidate.generation = generation;
  candidate.millisecond = millisecond_;
  candidate.utcOffsetMinutes = utcOffsetMinutes_;
  candidate.unixSeconds = unixSeconds_;
  candidate.crc32 = snapshotCrc(candidate);
  output = candidate;
  lastResult_ = ClockResult::Ok;
  return lastResult_;
}

ClockResult KitsuClock::validateSnapshot(const ClockSnapshot& snapshot) {
  if (snapshot.magic != kSnapshotMagic) {
    return ClockResult::BadSnapshotMagic;
  }
  if (snapshot.schemaVersion != kClockSnapshotSchemaVersion ||
      snapshot.bytes != kClockSnapshotBytes) {
    return ClockResult::UnsupportedSnapshot;
  }
  if (snapshot.crc32 != snapshotCrc(snapshot)) {
    return ClockResult::CorruptSnapshot;
  }
  const ClockSource source = static_cast<ClockSource>(snapshot.source);
  const ClockTrust trust = static_cast<ClockTrust>(snapshot.savedTrust);
  if (!validSource(source) ||
      (trust != ClockTrust::Trusted &&
       trust != ClockTrust::RestoredStale) ||
      !validInstant(snapshot.unixSeconds, snapshot.millisecond) ||
      !validOffset(snapshot.utcOffsetMinutes) ||
      snapshot.reserved[0] != 0U || snapshot.reserved[1] != 0U ||
      snapshot.reserved[2] != 0U || snapshot.reserved[3] != 0U) {
    return ClockResult::InvalidSnapshot;
  }
  return ClockResult::Ok;
}

ClockResult KitsuClock::restore(const ClockSnapshot& snapshot,
                                uint32_t nowMillis) {
  const ClockResult valid = validateSnapshot(snapshot);
  if (valid != ClockResult::Ok) {
    lastResult_ = valid;
    return lastResult_;
  }
  unixSeconds_ = snapshot.unixSeconds;
  millisecond_ = snapshot.millisecond;
  utcOffsetMinutes_ = snapshot.utcOffsetMinutes;
  lastMillis_ = nowMillis;
  restoredGeneration_ = snapshot.generation;
  source_ = static_cast<ClockSource>(snapshot.source);
  // Never invent elapsed-off time or claim restored wall time is current.
  trust_ = ClockTrust::RestoredStale;
  monotonicHealthy_ = true;
  lastResult_ = ClockResult::Ok;
  return lastResult_;
}

bool KitsuClock::generationAfter(uint32_t candidate, uint32_t reference) {
  return static_cast<int32_t>(candidate - reference) > 0;
}

ClockResult KitsuClock::parseUnixText(const char* input,
                                      uint64_t& unixSeconds) {
  if (!input || input[0] == '\0') return ClockResult::InvalidUnixText;
  uint64_t candidate = 0U;
  for (const char* cursor = input; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') {
      return ClockResult::InvalidUnixText;
    }
    const uint8_t digit = static_cast<uint8_t>(*cursor - '0');
    if (candidate > (UINT64_MAX - digit) / 10U) {
      return ClockResult::OutOfRange;
    }
    candidate = candidate * 10U + digit;
  }
  if (!validInstant(candidate, 0U)) return ClockResult::OutOfRange;
  unixSeconds = candidate;
  return ClockResult::Ok;
}

ClockResult KitsuClock::parseIso8601(const char* input,
                                     ParsedIso8601& output) {
  if (!input) return ClockResult::InvalidIso8601;
  const char* cursor = input;
  uint32_t year = 0U;
  uint32_t month = 0U;
  uint32_t day = 0U;
  uint32_t hour = 0U;
  uint32_t minute = 0U;
  uint32_t second = 0U;
  if (!parseDigits(cursor, 4U, year) || !consume(cursor, '-') ||
      !parseDigits(cursor, 2U, month) || !consume(cursor, '-') ||
      !parseDigits(cursor, 2U, day) || !consume(cursor, 'T') ||
      !parseDigits(cursor, 2U, hour) || !consume(cursor, ':') ||
      !parseDigits(cursor, 2U, minute) || !consume(cursor, ':') ||
      !parseDigits(cursor, 2U, second)) {
    return ClockResult::InvalidIso8601;
  }

  uint16_t millisecond = 0U;
  if (*cursor == '.') {
    ++cursor;
    uint8_t digits = 0U;
    while (*cursor >= '0' && *cursor <= '9') {
      if (digits >= 3U) return ClockResult::InvalidIso8601;
      millisecond = static_cast<uint16_t>(
          millisecond * 10U + static_cast<uint16_t>(*cursor - '0'));
      ++digits;
      ++cursor;
    }
    if (digits == 0U) return ClockResult::InvalidIso8601;
    while (digits < 3U) {
      millisecond = static_cast<uint16_t>(millisecond * 10U);
      ++digits;
    }
  }

  int16_t offsetMinutes = 0;
  if (*cursor == 'Z') {
    ++cursor;
  } else if (*cursor == '+' || *cursor == '-') {
    const bool negative = *cursor == '-';
    ++cursor;
    uint32_t offsetHour = 0U;
    uint32_t offsetMinute = 0U;
    if (!parseDigits(cursor, 2U, offsetHour) || !consume(cursor, ':') ||
        !parseDigits(cursor, 2U, offsetMinute) || offsetHour > 14U ||
        offsetMinute > 59U ||
        (offsetHour == 14U && offsetMinute != 0U)) {
      return ClockResult::InvalidIso8601;
    }
    const int16_t magnitude = static_cast<int16_t>(
        offsetHour * 60U + offsetMinute);
    offsetMinutes = negative ? static_cast<int16_t>(-magnitude) : magnitude;
  } else {
    return ClockResult::InvalidIso8601;
  }
  if (*cursor != '\0' || year > UINT16_MAX || month > UINT8_MAX ||
      day > UINT8_MAX || hour > UINT8_MAX || minute > UINT8_MAX ||
      second > UINT8_MAX) {
    return ClockResult::InvalidIso8601;
  }

  uint64_t unixSeconds = 0U;
  if (!civilToUnix(static_cast<uint16_t>(year),
                   static_cast<uint8_t>(month),
                   static_cast<uint8_t>(day),
                   static_cast<uint8_t>(hour),
                   static_cast<uint8_t>(minute),
                   static_cast<uint8_t>(second), offsetMinutes,
                   unixSeconds)) {
    return ClockResult::InvalidIso8601;
  }
  if (!validInstant(unixSeconds, millisecond)) {
    return ClockResult::OutOfRange;
  }

  ParsedIso8601 candidate{};
  candidate.unixSeconds = unixSeconds;
  candidate.millisecond = millisecond;
  candidate.utcOffsetMinutes = offsetMinutes;
  output = candidate;
  return ClockResult::Ok;
}

ClockResult KitsuClock::formatIso8601(uint64_t unixSeconds,
                                      uint16_t millisecond,
                                      int16_t utcOffsetMinutes,
                                      char* output, size_t outputBytes) {
  if (!output || outputBytes == 0U) return ClockResult::InvalidArgument;
  output[0] = '\0';
  if (!validOffset(utcOffsetMinutes) ||
      !validInstant(unixSeconds, millisecond)) {
    return ClockResult::OutOfRange;
  }

  uint16_t year = 0U;
  uint8_t month = 0U;
  uint8_t day = 0U;
  uint8_t hour = 0U;
  uint8_t minute = 0U;
  uint8_t second = 0U;
  unixToCivil(unixSeconds, utcOffsetMinutes, year, month, day, hour,
              minute, second);

  int written = -1;
  if (utcOffsetMinutes == 0) {
    if (millisecond == 0U) {
      written = snprintf(output, outputBytes,
                         "%04u-%02u-%02uT%02u:%02u:%02uZ",
                         static_cast<unsigned>(year),
                         static_cast<unsigned>(month),
                         static_cast<unsigned>(day),
                         static_cast<unsigned>(hour),
                         static_cast<unsigned>(minute),
                         static_cast<unsigned>(second));
    } else {
      written = snprintf(output, outputBytes,
                         "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                         static_cast<unsigned>(year),
                         static_cast<unsigned>(month),
                         static_cast<unsigned>(day),
                         static_cast<unsigned>(hour),
                         static_cast<unsigned>(minute),
                         static_cast<unsigned>(second),
                         static_cast<unsigned>(millisecond));
    }
  } else {
    const char sign = utcOffsetMinutes < 0 ? '-' : '+';
    const uint16_t magnitude = static_cast<uint16_t>(
        utcOffsetMinutes < 0 ? -static_cast<int32_t>(utcOffsetMinutes)
                             : utcOffsetMinutes);
    const unsigned offsetHour = magnitude / 60U;
    const unsigned offsetMinute = magnitude % 60U;
    if (millisecond == 0U) {
      written = snprintf(output, outputBytes,
                         "%04u-%02u-%02uT%02u:%02u:%02u%c%02u:%02u",
                         static_cast<unsigned>(year),
                         static_cast<unsigned>(month),
                         static_cast<unsigned>(day),
                         static_cast<unsigned>(hour),
                         static_cast<unsigned>(minute),
                         static_cast<unsigned>(second), sign,
                         offsetHour, offsetMinute);
    } else {
      written = snprintf(
          output, outputBytes,
          "%04u-%02u-%02uT%02u:%02u:%02u.%03u%c%02u:%02u",
          static_cast<unsigned>(year), static_cast<unsigned>(month),
          static_cast<unsigned>(day), static_cast<unsigned>(hour),
          static_cast<unsigned>(minute), static_cast<unsigned>(second),
          static_cast<unsigned>(millisecond), sign, offsetHour,
          offsetMinute);
    }
  }
  if (written < 0 || static_cast<size_t>(written) >= outputBytes) {
    output[0] = '\0';
    return ClockResult::InvalidArgument;
  }
  return ClockResult::Ok;
}

OneButtonClockEditor::OneButtonClockEditor() { cancel(); }

ClockResult OneButtonClockEditor::begin(uint64_t seedUnixSeconds,
                                        int16_t utcOffsetMinutes) {
  if (!validOffset(utcOffsetMinutes) ||
      !validInstant(seedUnixSeconds, 0U)) {
    cancel();
    return ClockResult::OutOfRange;
  }
  uint16_t year = 0U;
  uint8_t month = 0U;
  uint8_t day = 0U;
  uint8_t hour = 0U;
  uint8_t minute = 0U;
  uint8_t second = 0U;
  unixToCivil(seedUnixSeconds, utcOffsetMinutes, year, month, day, hour,
              minute, second);
  (void)second;
  if (year < kClockEditorMinimumYear ||
      year > kClockEditorMaximumYear ||
      utcOffsetMinutes < kClockEditorMinimumOffsetMinutes ||
      utcOffsetMinutes > kClockEditorMaximumOffsetMinutes) {
    cancel();
    return ClockResult::OutOfRange;
  }

  view_ = ClockEditorView{};
  view_.active = true;
  view_.field = ClockEditorField::Year;
  view_.year = year;
  view_.month = month;
  view_.day = day;
  view_.hour = hour;
  view_.minute = minute;
  view_.utcOffsetMinutes = utcOffsetMinutes;
  return ClockResult::Ok;
}

void OneButtonClockEditor::cancel() { view_ = ClockEditorView{}; }

void OneButtonClockEditor::clampDay() {
  const uint8_t maximum = monthDays(view_.year, view_.month);
  if (view_.day > maximum) view_.day = maximum;
}

ClockEditorEvent OneButtonClockEditor::shortPress() {
  if (!view_.active) return ClockEditorEvent::Ignored;
  view_.commitReady = false;
  switch (view_.field) {
    case ClockEditorField::Year:
      view_.year = view_.year >= kClockEditorMaximumYear
                       ? kClockEditorMinimumYear
                       : static_cast<uint16_t>(view_.year + 1U);
      clampDay();
      break;
    case ClockEditorField::Month:
      view_.month = view_.month >= 12U
                        ? 1U
                        : static_cast<uint8_t>(view_.month + 1U);
      clampDay();
      break;
    case ClockEditorField::Day: {
      const uint8_t maximum = monthDays(view_.year, view_.month);
      view_.day = view_.day >= maximum
                      ? 1U
                      : static_cast<uint8_t>(view_.day + 1U);
      break;
    }
    case ClockEditorField::Hour:
      view_.hour = view_.hour >= 23U
                       ? 0U
                       : static_cast<uint8_t>(view_.hour + 1U);
      break;
    case ClockEditorField::Minute:
      view_.minute = view_.minute >= 59U
                         ? 0U
                         : static_cast<uint8_t>(view_.minute + 1U);
      break;
    case ClockEditorField::UtcOffset:
      if (view_.utcOffsetMinutes >
          kClockEditorMaximumOffsetMinutes -
              kClockEditorOffsetStepMinutes) {
        view_.utcOffsetMinutes = kClockEditorMinimumOffsetMinutes;
      } else {
        view_.utcOffsetMinutes = static_cast<int16_t>(
            view_.utcOffsetMinutes + kClockEditorOffsetStepMinutes);
      }
      break;
    case ClockEditorField::Review:
      view_.field = ClockEditorField::Year;
      return ClockEditorEvent::Advanced;
    case ClockEditorField::Inactive:
      return ClockEditorEvent::Ignored;
  }
  return ClockEditorEvent::Changed;
}

ClockEditorEvent OneButtonClockEditor::longPress() {
  if (!view_.active) return ClockEditorEvent::Ignored;
  if (view_.field == ClockEditorField::Review) {
    view_.commitReady = true;
    return ClockEditorEvent::CommitRequested;
  }
  if (view_.field == ClockEditorField::Inactive) {
    return ClockEditorEvent::Ignored;
  }
  view_.commitReady = false;
  view_.field = static_cast<ClockEditorField>(
      static_cast<uint8_t>(view_.field) + 1U);
  return ClockEditorEvent::Advanced;
}

bool OneButtonClockEditor::value(uint64_t& unixSeconds,
                                 int16_t& utcOffsetMinutes) const {
  if (!view_.active || !view_.commitReady ||
      view_.field != ClockEditorField::Review) {
    return false;
  }
  uint64_t candidate = 0U;
  if (!civilToUnix(view_.year, view_.month, view_.day, view_.hour,
                   view_.minute, 0U, view_.utcOffsetMinutes, candidate) ||
      !validInstant(candidate, 0U)) {
    return false;
  }
  unixSeconds = candidate;
  utcOffsetMinutes = view_.utcOffsetMinutes;
  return true;
}

}  // namespace timekeeping
}  // namespace kitsu868
