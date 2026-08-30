#include "kitsu_pet_presentation.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

using kitsu868::presentation::Capture;
using kitsu868::presentation::ChunkRequest;
using kitsu868::presentation::ChunkResult;
using kitsu868::presentation::FrameEncoding;
using kitsu868::presentation::PetPresentationPreview;
using kitsu868::presentation::Playback;
using kitsu868::presentation::PresentationState;
using kitsu868::presentation::Role;
using kitsu868::presentation::Status;
using kitsu868::presentation::Surface;

int failures = 0;

#define EXPECT_TRUE(condition)                                                \
  do {                                                                        \
    if (!(condition)) {                                                       \
      printf("FAIL line %d: %s\n", __LINE__, #condition);                    \
      ++failures;                                                             \
    }                                                                         \
  } while (false)

#define EXPECT_EQ(expected, actual) EXPECT_TRUE((expected) == (actual))

void fillPattern(uint8_t* bytes, size_t length) {
  for (size_t index = 0U; index < length; ++index) {
    bytes[index] = static_cast<uint8_t>(index % 251U);
  }
}

Capture validV2Capture(const uint8_t* frame) {
  Capture capture{};
  capture.capturedAtMs = UINT32_C(0xFFFFFF00);
  capture.surface = Surface::Pet;
  capture.displayAwake = true;
  capture.frameVisible = true;
  capture.packValid = true;
  capture.packName = "Fox";
  capture.packId = UINT32_C(0xF0123456);
  capture.packRevision = 3U;
  capture.packTotalBytes = 50000U;
  capture.packPayloadCrc32 = UINT32_C(0x10203040);
  capture.packHeaderCrc32 = UINT32_C(0x50607080);
  capture.packFormatVersion = 2U;
  capture.frameWidth = 64U;
  capture.frameHeight = 80U;
  capture.frameCount = 14U;
  capture.appearanceVariant = 2U;
  capture.animationActive = true;
  capture.animationFinite = true;
  capture.requestedRole = Role::Pet;
  capture.resolvedRole = Role::Idle;
  capture.playback = Playback::PingPong;
  capture.animationToken = 77U;
  capture.animationElapsedMs = 1234U;
  capture.frameData = frame;
  capture.frameBytes = 640U;
  return capture;
}

void bindDigest(ChunkRequest& request, const PresentationState& state) {
  memcpy(request.expectedFrameSha256, state.frameSha256,
         sizeof(request.expectedFrameSha256));
}

void expectSourceUnchanged(const Capture& before, const Capture& after) {
  EXPECT_EQ(before.capturedAtMs, after.capturedAtMs);
  EXPECT_EQ(before.surface, after.surface);
  EXPECT_EQ(before.displayAwake, after.displayAwake);
  EXPECT_EQ(before.frameVisible, after.frameVisible);
  EXPECT_EQ(before.packValid, after.packValid);
  EXPECT_TRUE(before.packName == after.packName);
  EXPECT_EQ(before.packId, after.packId);
  EXPECT_EQ(before.packRevision, after.packRevision);
  EXPECT_EQ(before.packTotalBytes, after.packTotalBytes);
  EXPECT_EQ(before.packPayloadCrc32, after.packPayloadCrc32);
  EXPECT_EQ(before.packHeaderCrc32, after.packHeaderCrc32);
  EXPECT_EQ(before.packFormatVersion, after.packFormatVersion);
  EXPECT_EQ(before.frameWidth, after.frameWidth);
  EXPECT_EQ(before.frameHeight, after.frameHeight);
  EXPECT_EQ(before.frameCount, after.frameCount);
  EXPECT_EQ(before.appearanceVariant, after.appearanceVariant);
  EXPECT_EQ(before.animationActive, after.animationActive);
  EXPECT_EQ(before.animationFinite, after.animationFinite);
  EXPECT_EQ(before.requestedRole, after.requestedRole);
  EXPECT_EQ(before.resolvedRole, after.resolvedRole);
  EXPECT_EQ(before.playback, after.playback);
  EXPECT_EQ(before.animationToken, after.animationToken);
  EXPECT_EQ(before.animationElapsedMs, after.animationElapsedMs);
  EXPECT_TRUE(before.frameData == after.frameData);
  EXPECT_EQ(before.frameBytes, after.frameBytes);
}

void testTruthfulStateAndExactSnapshot() {
  uint8_t sourceFrame[640]{};
  uint8_t originalFrame[640]{};
  fillPattern(sourceFrame, sizeof(sourceFrame));
  memcpy(originalFrame, sourceFrame, sizeof(sourceFrame));
  const Capture source = validV2Capture(sourceFrame);
  const Capture sourceBefore = source;

  PetPresentationPreview preview;
  PresentationState state{};
  EXPECT_EQ(Status::Ok, preview.open(UINT32_MAX, source, state));
  EXPECT_TRUE(preview.active());
  EXPECT_EQ(UINT32_MAX, state.sessionId);
  EXPECT_EQ(Surface::Pet, state.surface);
  EXPECT_TRUE(state.displayAwake);
  EXPECT_TRUE(state.frameVisible);
  EXPECT_TRUE(state.packValid);
  EXPECT_TRUE(strcmp("Fox", state.packName) == 0);
  EXPECT_EQ(UINT32_C(0xF0123456), state.packId);
  EXPECT_EQ(3U, state.packRevision);
  EXPECT_EQ(2U, state.packFormatVersion);
  EXPECT_EQ(64U, state.frameWidth);
  EXPECT_EQ(80U, state.frameHeight);
  EXPECT_EQ(Role::Pet, state.requestedRole);
  EXPECT_EQ(Role::Idle, state.resolvedRole);
  EXPECT_EQ(Playback::PingPong, state.playback);
  EXPECT_TRUE(state.frameAvailable);
  EXPECT_EQ(FrameEncoding::XbmRowMajorLsbFirst, state.frameEncoding);
  EXPECT_EQ(640U, state.frameBytes);

  const uint8_t expectedDigest[32] = {
      0xc7U, 0xfbU, 0x9eU, 0xc6U, 0xabU, 0x8fU, 0x31U, 0xc5U,
      0x04U, 0x5fU, 0xcfU, 0xaeU, 0xe6U, 0xa9U, 0x1dU, 0x89U,
      0xf5U, 0xc0U, 0xc4U, 0xbaU, 0xf6U, 0x2dU, 0x1eU, 0xe8U,
      0x4aU, 0x90U, 0x5fU, 0x6fU, 0x34U, 0x08U, 0x78U, 0xb5U};
  EXPECT_TRUE(memcmp(expectedDigest, state.frameSha256,
                     sizeof(expectedDigest)) == 0);

  memset(sourceFrame, 0xA5, sizeof(sourceFrame));
  uint8_t output[192]{};
  ChunkRequest request{};
  request.sessionId = state.sessionId;
  request.offset = 0U;
  request.bytes = sizeof(output);
  bindDigest(request, state);
  ChunkResult result{};
  EXPECT_EQ(Status::Ok,
            preview.readChunk(request, output, sizeof(output), result));
  EXPECT_TRUE(memcmp(output, originalFrame, sizeof(output)) == 0);
  EXPECT_TRUE(memcmp(output, sourceFrame, sizeof(output)) != 0);
  expectSourceUnchanged(sourceBefore, source);
}

void testChunkBoundsDigestAndReassembly() {
  uint8_t frame[640]{};
  fillPattern(frame, sizeof(frame));
  PetPresentationPreview preview;
  PresentationState state{};
  EXPECT_EQ(Status::Ok,
            preview.open(200U, validV2Capture(frame), state));

  uint8_t reconstructed[640]{};
  uint16_t offset = 0U;
  while (offset < sizeof(reconstructed)) {
    const uint16_t remaining =
        static_cast<uint16_t>(sizeof(reconstructed) - offset);
    const uint16_t amount = remaining > 192U ? 192U : remaining;
    ChunkRequest request{};
    request.sessionId = 200U;
    request.offset = offset;
    request.bytes = amount;
    bindDigest(request, state);
    ChunkResult result{};
    EXPECT_EQ(Status::Ok,
              preview.readChunk(request, reconstructed + offset, amount,
                                result));
    EXPECT_EQ(offset, result.offset);
    EXPECT_EQ(amount, result.bytes);
    EXPECT_EQ(static_cast<uint16_t>(offset + amount), result.nextOffset);
    EXPECT_EQ(result.nextOffset == sizeof(reconstructed), result.complete);
    offset = result.nextOffset;
  }
  EXPECT_TRUE(memcmp(frame, reconstructed, sizeof(frame)) == 0);

  uint8_t output[193];
  memset(output, 0x5A, sizeof(output));
  ChunkRequest request{};
  request.sessionId = 200U;
  bindDigest(request, state);
  ChunkResult result{};

  request.bytes = 0U;
  EXPECT_EQ(Status::InvalidArgument,
            preview.readChunk(request, output, sizeof(output), result));
  request.bytes = 193U;
  EXPECT_EQ(Status::InvalidArgument,
            preview.readChunk(request, output, sizeof(output), result));
  request.offset = 640U;
  request.bytes = 1U;
  EXPECT_EQ(Status::OutOfRange,
            preview.readChunk(request, output, sizeof(output), result));
  request.offset = 639U;
  request.bytes = 2U;
  EXPECT_EQ(Status::OutOfRange,
            preview.readChunk(request, output, sizeof(output), result));
  request.offset = 0U;
  request.bytes = 10U;
  EXPECT_EQ(Status::BufferTooSmall,
            preview.readChunk(request, output, 9U, result));
  EXPECT_EQ(Status::BufferTooSmall,
            preview.readChunk(request, nullptr, 10U, result));

  request.sessionId = 201U;
  EXPECT_EQ(Status::WrongSession,
            preview.readChunk(request, output, sizeof(output), result));
  request.sessionId = 200U;
  request.expectedFrameSha256[0] ^= 1U;
  EXPECT_EQ(Status::DigestMismatch,
            preview.readChunk(request, output, sizeof(output), result));
  EXPECT_EQ(0x5AU, output[0]);
}

void testSessionSemantics() {
  uint8_t frame[640]{};
  fillPattern(frame, sizeof(frame));
  Capture capture = validV2Capture(frame);
  PetPresentationPreview preview;
  PresentationState original{};
  EXPECT_EQ(Status::InvalidArgument, preview.open(0U, capture, original));
  EXPECT_EQ(Status::Ok, preview.open(300U, capture, original));

  capture.surface = Surface::Sleep;
  capture.animationElapsedMs = 9999U;
  memset(frame, 0xEE, sizeof(frame));
  PresentationState duplicate{};
  EXPECT_EQ(Status::Duplicate, preview.open(300U, capture, duplicate));
  EXPECT_EQ(Surface::Pet, duplicate.surface);
  EXPECT_EQ(1234U, duplicate.animationElapsedMs);
  EXPECT_TRUE(memcmp(original.frameSha256, duplicate.frameSha256,
                     sizeof(original.frameSha256)) == 0);

  PresentationState busy{};
  EXPECT_EQ(Status::Busy, preview.open(301U, capture, busy));
  EXPECT_EQ(Status::WrongSession, preview.close(301U));
  EXPECT_EQ(Status::Ok, preview.close(300U));
  EXPECT_TRUE(!preview.active());
  EXPECT_EQ(Status::Duplicate, preview.close(300U));
  EXPECT_EQ(Status::StaleSession, preview.open(300U, capture, busy));
  EXPECT_EQ(Status::Ok, preview.open(301U, capture, busy));

  PresentationState described{};
  EXPECT_EQ(Status::WrongSession, preview.describe(302U, described));
  EXPECT_EQ(Status::Ok, preview.describe(301U, described));
  EXPECT_EQ(301U, described.sessionId);
  EXPECT_EQ(Status::Ok, preview.close(301U));
  EXPECT_EQ(Status::NoSession, preview.describe(301U, described));
}

void testNoFrameAndAbsentPackTruth() {
  Capture validNoFrame{};
  validNoFrame.capturedAtMs = 42U;
  validNoFrame.surface = Surface::Menu;
  validNoFrame.displayAwake = true;
  validNoFrame.packValid = true;
  validNoFrame.packName = "Cat";
  validNoFrame.packId = 9U;
  validNoFrame.packRevision = 1U;
  validNoFrame.packTotalBytes = 1024U;
  validNoFrame.packPayloadCrc32 = 1U;
  validNoFrame.packHeaderCrc32 = 2U;
  validNoFrame.packFormatVersion = 1U;
  validNoFrame.frameWidth = 64U;
  validNoFrame.frameHeight = 64U;
  validNoFrame.frameCount = 1U;

  PetPresentationPreview preview;
  PresentationState state{};
  EXPECT_EQ(Status::Ok, preview.open(400U, validNoFrame, state));
  EXPECT_TRUE(state.packValid);
  EXPECT_TRUE(!state.animationActive);
  EXPECT_TRUE(!state.frameAvailable);
  EXPECT_EQ(FrameEncoding::None, state.frameEncoding);
  EXPECT_EQ(0U, state.frameBytes);
  uint8_t output[1]{};
  ChunkRequest request{};
  request.sessionId = 400U;
  request.bytes = 1U;
  ChunkResult result{};
  EXPECT_EQ(Status::NoFrame,
            preview.readChunk(request, output, sizeof(output), result));
  EXPECT_EQ(Status::Ok, preview.close(400U));

  Capture absent{};
  absent.surface = Surface::Connect;
  absent.displayAwake = true;
  absent.packName = "None";
  EXPECT_TRUE(kitsu868::presentation::validCapture(absent));
  EXPECT_EQ(Status::Ok, preview.open(401U, absent, state));
  EXPECT_TRUE(!state.packValid);
  EXPECT_TRUE(strcmp("None", state.packName) == 0);
  EXPECT_EQ(Surface::Connect, state.surface);
  EXPECT_TRUE(!state.frameAvailable);
}

void testInvalidCaptures() {
  uint8_t frame[640]{};
  Capture capture = validV2Capture(frame);
  EXPECT_TRUE(kitsu868::presentation::validCapture(capture));

  Capture invalid = capture;
  invalid.surface = static_cast<Surface>(200U);
  EXPECT_TRUE(!kitsu868::presentation::validCapture(invalid));
  invalid = capture;
  invalid.packName = "";
  EXPECT_TRUE(!kitsu868::presentation::validCapture(invalid));
  invalid = capture;
  invalid.packId = 0U;
  EXPECT_TRUE(!kitsu868::presentation::validCapture(invalid));
  invalid = capture;
  invalid.packRevision = 0U;
  EXPECT_TRUE(!kitsu868::presentation::validCapture(invalid));
  invalid = capture;
  invalid.packFormatVersion = 3U;
  EXPECT_TRUE(!kitsu868::presentation::validCapture(invalid));
  invalid = capture;
  invalid.frameWidth = 63U;
  EXPECT_TRUE(!kitsu868::presentation::validCapture(invalid));
  invalid = capture;
  invalid.frameBytes = 639U;
  EXPECT_TRUE(!kitsu868::presentation::validCapture(invalid));
  invalid = capture;
  invalid.frameData = nullptr;
  EXPECT_TRUE(!kitsu868::presentation::validCapture(invalid));
  invalid = capture;
  invalid.requestedRole = Role::Unknown;
  EXPECT_TRUE(!kitsu868::presentation::validCapture(invalid));
  invalid = capture;
  invalid.playback = Playback::Unknown;
  EXPECT_TRUE(!kitsu868::presentation::validCapture(invalid));
  invalid = capture;
  invalid.animationToken = 0U;
  EXPECT_TRUE(!kitsu868::presentation::validCapture(invalid));
  invalid = capture;
  invalid.displayAwake = false;
  EXPECT_TRUE(!kitsu868::presentation::validCapture(invalid));

  Capture absent{};
  absent.surface = Surface::Pet;
  absent.packName = "None";
  absent.packId = 1U;
  EXPECT_TRUE(!kitsu868::presentation::validCapture(absent));
}

void testV1DigestAndLabels() {
  uint8_t frame[512]{};
  Capture capture{};
  capture.surface = Surface::Status;
  capture.displayAwake = false;
  capture.packValid = true;
  capture.packName = "Dog";
  capture.packId = 5U;
  capture.packRevision = 1U;
  capture.packTotalBytes = 2000U;
  capture.packFormatVersion = 1U;
  capture.frameWidth = 64U;
  capture.frameHeight = 64U;
  capture.frameCount = 2U;
  capture.animationActive = true;
  capture.requestedRole = Role::Sleep;
  capture.resolvedRole = Role::Sleep;
  capture.playback = Playback::Hold;
  capture.animationToken = 1U;
  capture.frameData = frame;
  capture.frameBytes = sizeof(frame);

  PetPresentationPreview preview;
  PresentationState state{};
  EXPECT_EQ(Status::Ok, preview.open(500U, capture, state));
  const uint8_t expected[32] = {
      0x07U, 0x6aU, 0x27U, 0xc7U, 0x9eU, 0x5aU, 0xceU, 0x2aU,
      0x3dU, 0x47U, 0xf9U, 0xddU, 0x2eU, 0x83U, 0xe4U, 0xffU,
      0x6eU, 0xa8U, 0x87U, 0x2bU, 0x3cU, 0x22U, 0x18U, 0xf6U,
      0x6cU, 0x92U, 0xb8U, 0x9bU, 0x55U, 0xf3U, 0x65U, 0x60U};
  EXPECT_TRUE(memcmp(expected, state.frameSha256, sizeof(expected)) == 0);
  EXPECT_TRUE(strcmp("status",
                     kitsu868::presentation::surfaceName(state.surface)) == 0);
  EXPECT_TRUE(strcmp("sleep",
                     kitsu868::presentation::roleName(state.resolvedRole)) ==
              0);
  EXPECT_TRUE(strcmp("hold",
                     kitsu868::presentation::playbackName(state.playback)) ==
              0);
  EXPECT_TRUE(strcmp("digest_mismatch",
                     kitsu868::presentation::statusName(
                         Status::DigestMismatch)) == 0);
}

}  // namespace

int main() {
  testTruthfulStateAndExactSnapshot();
  testChunkBoundsDigestAndReassembly();
  testSessionSemantics();
  testNoFrameAndAbsentPackTruth();
  testInvalidCaptures();
  testV1DigestAndLabels();

  if (failures != 0) {
    printf("kitsu_pet_presentation: %d failure(s)\n", failures);
    return 1;
  }
  printf("kitsu_pet_presentation: all tests passed\n");
  return 0;
}
