#include "kitsu_clock.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

using kitsu868::timekeeping::ClockEditorEvent;
using kitsu868::timekeeping::ClockEditorField;
using kitsu868::timekeeping::ClockReading;
using kitsu868::timekeeping::ClockResult;
using kitsu868::timekeeping::ClockSnapshot;
using kitsu868::timekeeping::ClockSource;
using kitsu868::timekeeping::ClockTrust;
using kitsu868::timekeeping::KitsuClock;
using kitsu868::timekeeping::NetworkTimeSample;
using kitsu868::timekeeping::OneButtonClockEditor;
using kitsu868::timekeeping::ParsedIso8601;
using kitsu868::timekeeping::clockResultName;
using kitsu868::timekeeping::clockSourceName;
using kitsu868::timekeeping::clockTrustName;
using kitsu868::timekeeping::kClockIso8601BufferBytes;
using kitsu868::timekeeping::kClockSnapshotBytes;
using kitsu868::timekeeping::kMaximumClockUnixSeconds;
using kitsu868::timekeeping::kMinimumClockUnixSeconds;

namespace {

uint32_t snapshotCrc(const ClockSnapshot& snapshot) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&snapshot);
  uint32_t crc = UINT32_C(0xFFFFFFFF);
  for (size_t index = 0U; index < offsetof(ClockSnapshot, crc32); ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
    }
  }
  return ~crc;
}

uint64_t parseUtc(const char* text) {
  ParsedIso8601 parsed{};
  assert(KitsuClock::parseIso8601(text, parsed) == ClockResult::Ok);
  return parsed.unixSeconds;
}

void testLabelsAndInitialState() {
  KitsuClock clock;
  assert(!clock.set());
  assert(!clock.trusted());
  assert(clock.source() == ClockSource::None);
  assert(clock.trust() == ClockTrust::Unset);
  assert(clock.setUtcOffsetMinutes(60) == ClockResult::Ok);
  assert(clock.utcOffsetMinutes() == 60);
  assert(!clock.set());
  assert(strcmp(clockSourceName(ClockSource::ManualSerial),
                "manual_serial") == 0);
  assert(strcmp(clockSourceName(ClockSource::ManualDevice),
                "manual_device") == 0);
  assert(strcmp(clockSourceName(ClockSource::NetworkTime),
                "network_time") == 0);
  assert(strcmp(clockSourceName(ClockSource::AuthenticatedApp),
                "authenticated_app") == 0);
  assert(strcmp(clockTrustName(ClockTrust::RestoredStale),
                "restored_stale") == 0);
  assert(strcmp(clockResultName(ClockResult::MonotonicDiscontinuity),
                "monotonic_discontinuity") == 0);

  ClockReading reading{};
  assert(clock.read(123U, reading) == ClockResult::NotSet);
  assert(!reading.set());
  assert(!reading.trusted());
  assert(!reading.monotonicHealthy);

  ClockSnapshot snapshot{};
  assert(clock.makeSnapshot(123U, 1U, snapshot) == ClockResult::NotSet);
}

void testIsoParsingAndFormatting() {
  ParsedIso8601 parsed{};
  assert(KitsuClock::parseIso8601("2024-01-01T00:00:00Z", parsed) ==
         ClockResult::Ok);
  assert(parsed.unixSeconds == kMinimumClockUnixSeconds);
  assert(parsed.millisecond == 0U);
  assert(parsed.utcOffsetMinutes == 0);
  ParsedIso8601 edge{};
  assert(KitsuClock::parseIso8601("2024-01-01T14:00:00+14:00", edge) ==
         ClockResult::Ok);
  assert(edge.unixSeconds == kMinimumClockUnixSeconds);
  assert(KitsuClock::parseIso8601("2023-12-31T10:00:00-14:00", edge) ==
         ClockResult::Ok);
  assert(edge.unixSeconds == kMinimumClockUnixSeconds);

  ParsedIso8601 offset{};
  assert(KitsuClock::parseIso8601(
             "2026-08-29T15:45:12.7+03:00", offset) == ClockResult::Ok);
  assert(offset.unixSeconds ==
         parseUtc("2026-08-29T12:45:12.700Z"));
  assert(offset.millisecond == 700U);
  assert(offset.utcOffsetMinutes == 180);

  ParsedIso8601 west{};
  assert(KitsuClock::parseIso8601(
             "2026-08-29T05:15:12.045-07:30", west) == ClockResult::Ok);
  assert(west.unixSeconds == offset.unixSeconds);
  assert(west.millisecond == 45U);
  assert(west.utcOffsetMinutes == -450);

  assert(KitsuClock::parseIso8601("2024-02-29T23:59:59Z", parsed) ==
         ClockResult::Ok);
  assert(KitsuClock::parseIso8601("2100-02-29T00:00:00Z", parsed) ==
         ClockResult::InvalidIso8601);

  const char* invalid[] = {
      nullptr,
      "",
      "2026-08-29 12:45:12Z",
      "2026-08-29T12:45:12",
      "2026-8-29T12:45:12Z",
      "2026-08-29T24:00:00Z",
      "2026-08-29T12:60:00Z",
      "2026-08-29T12:00:60Z",
      "2026-04-31T12:00:00Z",
      "2025-02-29T12:00:00Z",
      "2026-08-29T12:00:00.Z",
      "2026-08-29T12:00:00.1234Z",
      "2026-08-29T12:00:00+14:01",
      "2026-08-29T12:00:00+15:00",
      "2026-08-29T12:00:00+03",
      "2026-08-29T12:00:00z",
      "2026-08-29T12:00:00Zjunk",
  };
  for (size_t index = 0U; index < sizeof(invalid) / sizeof(invalid[0]);
       ++index) {
    assert(KitsuClock::parseIso8601(invalid[index], parsed) !=
           ClockResult::Ok);
  }
  assert(KitsuClock::parseIso8601("2023-12-31T23:59:59Z", parsed) ==
         ClockResult::OutOfRange);
  assert(KitsuClock::parseIso8601("2100-01-01T00:00:00.001Z", parsed) ==
         ClockResult::OutOfRange);

  char text[kClockIso8601BufferBytes]{};
  assert(KitsuClock::formatIso8601(offset.unixSeconds, offset.millisecond,
                                   offset.utcOffsetMinutes, text,
                                   sizeof(text)) == ClockResult::Ok);
  assert(strcmp(text, "2026-08-29T15:45:12.700+03:00") == 0);
  ParsedIso8601 roundTrip{};
  assert(KitsuClock::parseIso8601(text, roundTrip) == ClockResult::Ok);
  assert(roundTrip.unixSeconds == offset.unixSeconds);
  assert(roundTrip.millisecond == offset.millisecond);
  assert(roundTrip.utcOffsetMinutes == offset.utcOffsetMinutes);

  assert(KitsuClock::formatIso8601(offset.unixSeconds, 0U, -450, text,
                                   sizeof(text)) == ClockResult::Ok);
  assert(strcmp(text, "2026-08-29T05:15:12-07:30") == 0);
  assert(KitsuClock::formatIso8601(kMaximumClockUnixSeconds, 0U, 0,
                                   text, sizeof(text)) == ClockResult::Ok);
  assert(strcmp(text, "2100-01-01T00:00:00Z") == 0);

  char tooSmall[8] = "dirty";
  assert(KitsuClock::formatIso8601(offset.unixSeconds, 0U, 0,
                                   tooSmall, sizeof(tooSmall)) ==
         ClockResult::InvalidArgument);
  assert(tooSmall[0] == '\0');
}

void testCalendarRoundTrips() {
  const int16_t offsets[] = {-840, -345, 0, 345, 840};
  char text[kClockIso8601BufferBytes]{};
  const uint64_t step = UINT64_C(1234567);
  for (uint64_t epoch = kMinimumClockUnixSeconds;
       epoch < kMaximumClockUnixSeconds; epoch += step) {
    for (size_t index = 0U; index < sizeof(offsets) / sizeof(offsets[0]);
         ++index) {
      const uint16_t millisecond =
          static_cast<uint16_t>((epoch / 17U) % 1000U);
      assert(KitsuClock::formatIso8601(epoch, millisecond, offsets[index],
                                       text, sizeof(text)) ==
             ClockResult::Ok);
      ParsedIso8601 parsed{};
      assert(KitsuClock::parseIso8601(text, parsed) == ClockResult::Ok);
      assert(parsed.unixSeconds == epoch);
      assert(parsed.millisecond == millisecond);
      assert(parsed.utcOffsetMinutes == offsets[index]);
    }
  }
}

void testUnixInputsSourcesAndNetworkGate() {
  uint64_t unixSeconds = 99U;
  assert(KitsuClock::parseUnixText("1800000000", unixSeconds) ==
         ClockResult::Ok);
  assert(unixSeconds == UINT64_C(1800000000));
  assert(KitsuClock::parseUnixText(nullptr, unixSeconds) ==
         ClockResult::InvalidUnixText);
  assert(KitsuClock::parseUnixText("", unixSeconds) ==
         ClockResult::InvalidUnixText);
  assert(KitsuClock::parseUnixText(" 1800000000", unixSeconds) ==
         ClockResult::InvalidUnixText);
  assert(KitsuClock::parseUnixText("+1800000000", unixSeconds) ==
         ClockResult::InvalidUnixText);
  assert(KitsuClock::parseUnixText("1800000000x", unixSeconds) ==
         ClockResult::InvalidUnixText);
  assert(KitsuClock::parseUnixText("18446744073709551616", unixSeconds) ==
         ClockResult::OutOfRange);
  assert(KitsuClock::parseUnixText("1", unixSeconds) ==
         ClockResult::OutOfRange);

  KitsuClock clock;
  assert(clock.setFromUnixText("1800000000", ClockSource::ManualSerial,
                               180, 100U) == ClockResult::Ok);
  assert(clock.source() == ClockSource::ManualSerial);
  assert(clock.trusted());
  assert(clock.utcOffsetMinutes() == 180);

  // Rejected input is transactional: the prior anchor stays available.
  assert(clock.setFromUnixText("bad", ClockSource::ManualSerial, 0, 100U) ==
         ClockResult::InvalidUnixText);
  ClockReading reading{};
  assert(clock.read(100U, reading) == ClockResult::Ok);
  assert(reading.unixSeconds == UINT64_C(1800000000));
  assert(reading.utcOffsetMinutes == 180);

  assert(clock.setFromUnixSeconds(UINT64_C(1800000100), 22U,
                                  ClockSource::NetworkTime, 0, 200U) ==
         ClockResult::InvalidSource);
  assert(clock.setFromUnixSeconds(UINT64_C(1800000100), 22U,
                                  ClockSource::None, 0, 200U) ==
         ClockResult::InvalidSource);

  NetworkTimeSample ntp{};
  ntp.unixSeconds = UINT64_C(1800000200);
  ntp.millisecond = 333U;
  assert(clock.acceptNtp(ntp, 300U) ==
         ClockResult::NetworkTimeNotSynchronized);
  assert(clock.source() == ClockSource::ManualSerial);
  ntp.synchronized = true;
  assert(clock.acceptNtp(ntp, 300U) == ClockResult::Ok);
  assert(clock.source() == ClockSource::NetworkTime);
  assert(clock.trusted());
  // Network UTC refresh does not discard the user's display offset.
  assert(clock.utcOffsetMinutes() == 180);

  assert(clock.setUtcOffsetMinutes(-345) == ClockResult::Ok);
  assert(clock.utcOffsetMinutes() == -345);
  assert(clock.setUtcOffsetMinutes(841) == ClockResult::OutOfRange);
  assert(clock.utcOffsetMinutes() == -345);

  assert(clock.setFromIso8601("2026-08-29T12:45:12Z",
                              ClockSource::AuthenticatedApp, 400U) ==
         ClockResult::Ok);
  assert(clock.source() == ClockSource::AuthenticatedApp);
  assert(clock.setFromIso8601("2026-08-29T15:45:12+03:00",
                              ClockSource::ManualDevice, 500U) ==
         ClockResult::Ok);
  assert(clock.source() == ClockSource::ManualDevice);
  assert(clock.utcOffsetMinutes() == 180);
}

void testMonotonicProgressAndRollover() {
  KitsuClock clock;
  assert(clock.setFromUnixSeconds(UINT64_C(1800000000), 250U,
                                  ClockSource::ManualSerial, 0, 1000U) ==
         ClockResult::Ok);
  ClockReading reading{};
  assert(clock.read(1749U, reading) == ClockResult::Ok);
  assert(reading.unixSeconds == UINT64_C(1800000000));
  assert(reading.millisecond == 999U);
  assert(clock.read(1750U, reading) == ClockResult::Ok);
  assert(reading.unixSeconds == UINT64_C(1800000001));
  assert(reading.millisecond == 0U);

  KitsuClock rollover;
  assert(rollover.setFromUnixSeconds(UINT64_C(1800000000), 900U,
                                     ClockSource::ManualSerial, 0,
                                     UINT32_C(0xFFFFFF00)) ==
         ClockResult::Ok);
  assert(rollover.read(UINT32_C(0x000003E8), reading) == ClockResult::Ok);
  assert(reading.unixSeconds == UINT64_C(1800000002));
  assert(reading.millisecond == 156U);
  assert(reading.trusted());
  assert(reading.monotonicHealthy);

  // A backwards/reset-like millis sample is not mistaken for 49.7 days.
  KitsuClock discontinuous;
  assert(discontinuous.setFromUnixSeconds(UINT64_C(1800000000), 0U,
                                          ClockSource::ManualDevice, 0,
                                          1000U) == ClockResult::Ok);
  assert(discontinuous.read(999U, reading) ==
         ClockResult::MonotonicDiscontinuity);
  assert(reading.unixSeconds == UINT64_C(1800000000));
  assert(reading.trust == ClockTrust::RestoredStale);
  assert(!reading.monotonicHealthy);
  assert(discontinuous.read(1999U, reading) == ClockResult::Ok);
  assert(reading.unixSeconds == UINT64_C(1800000001));
  assert(!reading.trusted());

  // Half-range-spaced observations can progress across repeated rollovers.
  KitsuClock longRunning;
  assert(longRunning.setFromUnixSeconds(UINT64_C(1800000000), 0U,
                                        ClockSource::AuthenticatedApp, 0,
                                        0U) == ClockResult::Ok);
  assert(longRunning.read(UINT32_C(0x7FFFFFFF), reading) ==
         ClockResult::Ok);
  assert(longRunning.read(UINT32_C(0xFFFFFFFE), reading) ==
         ClockResult::Ok);
  assert(longRunning.read(UINT32_C(0x000003E8), reading) ==
         ClockResult::Ok);
  assert(reading.trusted());

  KitsuClock boundary;
  assert(boundary.setFromUnixSeconds(kMaximumClockUnixSeconds, 0U,
                                     ClockSource::ManualSerial, 0, 0U) ==
         ClockResult::Ok);
  assert(boundary.read(1U, reading) == ClockResult::OutOfRange);
  assert(reading.unixSeconds == kMaximumClockUnixSeconds);
  assert(!reading.trusted());
}

void testSnapshotCrcRestoreAndGeneration() {
  KitsuClock clock;
  assert(clock.setFromUnixSeconds(UINT64_C(1800000000), 125U,
                                  ClockSource::ManualSerial, 345, 100U) ==
         ClockResult::Ok);
  ClockSnapshot snapshot{};
  assert(clock.makeSnapshot(2100U, UINT32_C(0xFFFFFFFE), snapshot) ==
         ClockResult::Ok);
  assert(snapshot.unixSeconds == UINT64_C(1800000002));
  assert(snapshot.millisecond == 125U);
  assert(snapshot.utcOffsetMinutes == 345);
  assert(snapshot.generation == UINT32_C(0xFFFFFFFE));
  assert(KitsuClock::validateSnapshot(snapshot) == ClockResult::Ok);

  // Any single-bit corruption in the complete fixed record is rejected.
  for (size_t byte = 0U; byte < sizeof(snapshot); ++byte) {
    ClockSnapshot corrupt = snapshot;
    reinterpret_cast<uint8_t*>(&corrupt)[byte] ^= 0x01U;
    assert(KitsuClock::validateSnapshot(corrupt) != ClockResult::Ok);
  }

  KitsuClock restored;
  assert(restored.restore(snapshot, 900000U) == ClockResult::Ok);
  assert(restored.restoredGeneration() == UINT32_C(0xFFFFFFFE));
  assert(restored.source() == ClockSource::ManualSerial);
  assert(restored.trust() == ClockTrust::RestoredStale);
  assert(!restored.trusted());
  ClockReading reading{};
  // No elapsed-off time is fabricated: restore begins at the saved instant.
  assert(restored.read(900000U, reading) == ClockResult::Ok);
  assert(reading.unixSeconds == snapshot.unixSeconds);
  assert(reading.millisecond == snapshot.millisecond);
  assert(restored.read(902000U, reading) == ClockResult::Ok);
  assert(reading.unixSeconds == snapshot.unixSeconds + 2U);
  assert(reading.trust == ClockTrust::RestoredStale);

  ClockSnapshot staleSnapshot{};
  assert(restored.makeSnapshot(902000U, UINT32_C(0xFFFFFFFF),
                               staleSnapshot) == ClockResult::Ok);
  assert(staleSnapshot.savedTrust ==
         static_cast<uint8_t>(ClockTrust::RestoredStale));
  assert(KitsuClock::validateSnapshot(staleSnapshot) == ClockResult::Ok);

  // CRC-valid but semantically impossible records still fail closed.
  ClockSnapshot impossible = snapshot;
  impossible.utcOffsetMinutes = 900;
  impossible.crc32 = snapshotCrc(impossible);
  assert(KitsuClock::validateSnapshot(impossible) ==
         ClockResult::InvalidSnapshot);
  impossible = snapshot;
  impossible.source = static_cast<uint8_t>(ClockSource::None);
  impossible.crc32 = snapshotCrc(impossible);
  assert(KitsuClock::validateSnapshot(impossible) ==
         ClockResult::InvalidSnapshot);
  impossible = snapshot;
  impossible.reserved[2] = 1U;
  impossible.crc32 = snapshotCrc(impossible);
  assert(KitsuClock::validateSnapshot(impossible) ==
         ClockResult::InvalidSnapshot);

  ClockSnapshot badMagic = snapshot;
  badMagic.magic ^= 1U;
  assert(KitsuClock::validateSnapshot(badMagic) ==
         ClockResult::BadSnapshotMagic);
  ClockSnapshot unsupported = snapshot;
  ++unsupported.schemaVersion;
  assert(KitsuClock::validateSnapshot(unsupported) ==
         ClockResult::UnsupportedSnapshot);

  // Restoring a bad record does not erase the current live anchor.
  assert(restored.restore(impossible, 1U) == ClockResult::InvalidSnapshot);
  assert(restored.set());
  assert(restored.source() == ClockSource::ManualSerial);

  assert(KitsuClock::generationAfter(1U, 0U));
  assert(!KitsuClock::generationAfter(0U, 1U));
  assert(KitsuClock::generationAfter(0U, UINT32_MAX));
  assert(!KitsuClock::generationAfter(UINT32_MAX, 0U));
  assert(!KitsuClock::generationAfter(7U, 7U));
}

void advanceToReview(OneButtonClockEditor& editor) {
  for (uint8_t step = 0U; step < 6U; ++step) {
    assert(editor.longPress() == ClockEditorEvent::Advanced);
  }
  assert(editor.view().field == ClockEditorField::Review);
}

void testOneButtonEditor() {
  OneButtonClockEditor editor;
  assert(editor.shortPress() == ClockEditorEvent::Ignored);
  assert(editor.longPress() == ClockEditorEvent::Ignored);

  const uint64_t seed = parseUtc("2026-08-29T12:45:37Z");
  assert(editor.begin(seed, 180) == ClockResult::Ok);
  auto view = editor.view();
  assert(view.active);
  assert(!view.commitReady);
  assert(view.field == ClockEditorField::Year);
  assert(view.year == 2026U && view.month == 8U && view.day == 29U);
  assert(view.hour == 15U && view.minute == 45U);
  assert(view.utcOffsetMinutes == 180);

  advanceToReview(editor);
  uint64_t value = 0U;
  int16_t offset = 0;
  assert(!editor.value(value, offset));
  assert(editor.longPress() == ClockEditorEvent::CommitRequested);
  assert(editor.view().commitReady);
  assert(editor.value(value, offset));
  assert(value == parseUtc("2026-08-29T12:45:00Z"));
  assert(offset == 180);

  KitsuClock clock;
  assert(clock.setFromUnixSeconds(value, 0U, ClockSource::ManualDevice,
                                  offset, 50U) == ClockResult::Ok);
  assert(clock.source() == ClockSource::ManualDevice);
  assert(clock.trusted());

  // Review short-press allows corrections instead of trapping the user.
  assert(editor.shortPress() == ClockEditorEvent::Advanced);
  assert(editor.view().field == ClockEditorField::Year);
  assert(!editor.view().commitReady);

  // Incrementing year clamps leap day to the target month's valid maximum.
  assert(editor.begin(parseUtc("2024-02-29T10:00:00Z"), 0) ==
         ClockResult::Ok);
  assert(editor.shortPress() == ClockEditorEvent::Changed);
  view = editor.view();
  assert(view.year == 2025U && view.month == 2U && view.day == 28U);

  // The device editor exposes the complete real-world -12:00..+14:00 range.
  assert(editor.begin(seed, 840) == ClockResult::Ok);
  for (uint8_t step = 0U; step < 5U; ++step) {
    assert(editor.longPress() == ClockEditorEvent::Advanced);
  }
  assert(editor.view().field == ClockEditorField::UtcOffset);
  assert(editor.shortPress() == ClockEditorEvent::Changed);
  assert(editor.view().utcOffsetMinutes == -720);

  editor.cancel();
  assert(!editor.view().active);
  assert(!editor.value(value, offset));
  assert(editor.begin(kMinimumClockUnixSeconds, -840) ==
         ClockResult::OutOfRange);
}

}  // namespace

int main() {
  static_assert(sizeof(ClockSnapshot) == kClockSnapshotBytes,
                "Clock snapshot budget changed");
  static_assert(kClockSnapshotBytes == 32U,
                "Clock persistence contract changed");
  static_assert(sizeof(KitsuClock) <= 48U,
                "Clock runtime state is no longer lean");
  static_assert(sizeof(OneButtonClockEditor) <= 24U,
                "Clock editor state is no longer lean");
  testLabelsAndInitialState();
  testIsoParsingAndFormatting();
  testCalendarRoundTrips();
  testUnixInputsSourcesAndNetworkGate();
  testMonotonicProgressAndRollover();
  testSnapshotCrcRestoreAndGeneration();
  testOneButtonEditor();
  puts("PASS kitsu_clock_host_test");
  puts("  sources: serial, device, network, authenticated app");
  puts("  persistence: 32-byte CRC record; restore is always stale");
  puts("  editor: short increment, long advance, long Review commit");
  return 0;
}
