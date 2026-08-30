#include "kitsu_pet_presentation.h"

#include <string.h>

namespace kitsu868 {
namespace presentation {
namespace {

struct Sha256Context {
  uint32_t state[8]{};
  uint64_t bitLength = 0U;
  uint8_t block[64]{};
  size_t blockBytes = 0U;
};

constexpr uint32_t kSha256RoundConstants[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2),
};

uint32_t rotateRight(uint32_t value, uint8_t amount) {
  return (value >> amount) | (value << (32U - amount));
}

void sha256Transform(Sha256Context& context) {
  uint32_t schedule[64]{};
  for (size_t index = 0U; index < 16U; ++index) {
    const size_t offset = index * 4U;
    schedule[index] =
        (static_cast<uint32_t>(context.block[offset]) << 24U) |
        (static_cast<uint32_t>(context.block[offset + 1U]) << 16U) |
        (static_cast<uint32_t>(context.block[offset + 2U]) << 8U) |
        static_cast<uint32_t>(context.block[offset + 3U]);
  }
  for (size_t index = 16U; index < 64U; ++index) {
    const uint32_t first = schedule[index - 15U];
    const uint32_t second = schedule[index - 2U];
    const uint32_t sigma0 = rotateRight(first, 7U) ^
                            rotateRight(first, 18U) ^ (first >> 3U);
    const uint32_t sigma1 = rotateRight(second, 17U) ^
                            rotateRight(second, 19U) ^ (second >> 10U);
    schedule[index] = schedule[index - 16U] + sigma0 +
                      schedule[index - 7U] + sigma1;
  }

  uint32_t a = context.state[0];
  uint32_t b = context.state[1];
  uint32_t c = context.state[2];
  uint32_t d = context.state[3];
  uint32_t e = context.state[4];
  uint32_t f = context.state[5];
  uint32_t g = context.state[6];
  uint32_t h = context.state[7];
  for (size_t index = 0U; index < 64U; ++index) {
    const uint32_t sum1 = rotateRight(e, 6U) ^ rotateRight(e, 11U) ^
                          rotateRight(e, 25U);
    const uint32_t choose = (e & f) ^ ((~e) & g);
    const uint32_t temporary1 = h + sum1 + choose +
                                kSha256RoundConstants[index] + schedule[index];
    const uint32_t sum0 = rotateRight(a, 2U) ^ rotateRight(a, 13U) ^
                          rotateRight(a, 22U);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }

  context.state[0] += a;
  context.state[1] += b;
  context.state[2] += c;
  context.state[3] += d;
  context.state[4] += e;
  context.state[5] += f;
  context.state[6] += g;
  context.state[7] += h;
}

void sha256Start(Sha256Context& context) {
  context = Sha256Context{};
  context.state[0] = UINT32_C(0x6a09e667);
  context.state[1] = UINT32_C(0xbb67ae85);
  context.state[2] = UINT32_C(0x3c6ef372);
  context.state[3] = UINT32_C(0xa54ff53a);
  context.state[4] = UINT32_C(0x510e527f);
  context.state[5] = UINT32_C(0x9b05688c);
  context.state[6] = UINT32_C(0x1f83d9ab);
  context.state[7] = UINT32_C(0x5be0cd19);
}

void sha256Update(Sha256Context& context, const uint8_t* bytes,
                  size_t length) {
  for (size_t index = 0U; index < length; ++index) {
    context.block[context.blockBytes++] = bytes[index];
    if (context.blockBytes == sizeof(context.block)) {
      sha256Transform(context);
      context.bitLength += UINT64_C(512);
      context.blockBytes = 0U;
    }
  }
}

void sha256Finish(Sha256Context& context,
                  uint8_t output[kFrameDigestBytes]) {
  size_t cursor = context.blockBytes;
  context.block[cursor++] = 0x80U;
  if (cursor > 56U) {
    while (cursor < sizeof(context.block)) context.block[cursor++] = 0U;
    sha256Transform(context);
    memset(context.block, 0, sizeof(context.block));
    cursor = 0U;
  }
  while (cursor < 56U) context.block[cursor++] = 0U;

  context.bitLength += static_cast<uint64_t>(context.blockBytes) * 8U;
  for (uint8_t index = 0U; index < 8U; ++index) {
    context.block[63U - index] =
        static_cast<uint8_t>(context.bitLength >> (index * 8U));
  }
  sha256Transform(context);

  for (size_t index = 0U; index < 8U; ++index) {
    output[index * 4U] = static_cast<uint8_t>(context.state[index] >> 24U);
    output[index * 4U + 1U] =
        static_cast<uint8_t>(context.state[index] >> 16U);
    output[index * 4U + 2U] =
        static_cast<uint8_t>(context.state[index] >> 8U);
    output[index * 4U + 3U] = static_cast<uint8_t>(context.state[index]);
  }
  context = Sha256Context{};
}

bool validSurface(Surface surface) {
  return static_cast<uint8_t>(surface) <=
             static_cast<uint8_t>(Surface::Activity) ||
         surface == Surface::Unknown;
}

bool validRole(Role role) {
  return static_cast<uint8_t>(role) <= static_cast<uint8_t>(Role::Evolve);
}

bool validPlayback(Playback playback) {
  return static_cast<uint8_t>(playback) <=
         static_cast<uint8_t>(Playback::PingPong);
}

bool validPackName(const char* name) {
  if (!name) return false;
  for (size_t index = 0U; index <= kMaximumPackNameBytes; ++index) {
    const uint8_t value = static_cast<uint8_t>(name[index]);
    if (value == 0U) return index != 0U;
    if (index == kMaximumPackNameBytes || value < 0x20U || value > 0x7EU) {
      return false;
    }
  }
  return false;
}

size_t expectedFrameBytes(const Capture& capture) {
  if (capture.packFormatVersion == kPackV1Format &&
      capture.frameWidth == kPackFrameWidth &&
      capture.frameHeight == kPackV1FrameHeight) {
    return kPackV1FrameBytes;
  }
  if (capture.packFormatVersion == kPackV2Format &&
      capture.frameWidth == kPackFrameWidth &&
      capture.frameHeight == kPackV2FrameHeight) {
    return kPackV2FrameBytes;
  }
  return 0U;
}

bool sameDigest(const uint8_t* left, const uint8_t* right) {
  uint8_t difference = 0U;
  for (size_t index = 0U; index < kFrameDigestBytes; ++index) {
    difference |= static_cast<uint8_t>(left[index] ^ right[index]);
  }
  return difference == 0U;
}

void copyPackName(char output[kMaximumPackNameBytes + 1U],
                  const char* input) {
  memset(output, 0, kMaximumPackNameBytes + 1U);
  if (!input) {
    memcpy(output, "None", 5U);
    return;
  }
  size_t length = 0U;
  while (length < kMaximumPackNameBytes && input[length] != '\0') ++length;
  memcpy(output, input, length);
}

PresentationState makeState(uint32_t sessionId, const Capture& capture) {
  PresentationState state{};
  state.sessionId = sessionId;
  state.capturedAtMs = capture.capturedAtMs;
  state.surface = capture.surface;
  state.displayAwake = capture.displayAwake;
  state.frameVisible = capture.frameVisible;
  state.packValid = capture.packValid;
  copyPackName(state.packName, capture.packName);
  state.packId = capture.packId;
  state.packRevision = capture.packRevision;
  state.packTotalBytes = capture.packTotalBytes;
  state.packPayloadCrc32 = capture.packPayloadCrc32;
  state.packHeaderCrc32 = capture.packHeaderCrc32;
  state.packFormatVersion = capture.packFormatVersion;
  state.frameWidth = capture.frameWidth;
  state.frameHeight = capture.frameHeight;
  state.frameCount = capture.frameCount;
  state.appearanceVariant = capture.appearanceVariant;
  state.animationActive = capture.animationActive;
  state.animationFinite = capture.animationFinite;
  state.requestedRole = capture.requestedRole;
  state.resolvedRole = capture.resolvedRole;
  state.playback = capture.playback;
  state.animationToken = capture.animationToken;
  state.animationElapsedMs = capture.animationElapsedMs;
  state.frameAvailable = capture.frameData != nullptr;
  if (state.frameAvailable) {
    state.frameEncoding = FrameEncoding::XbmRowMajorLsbFirst;
    state.frameBytes = static_cast<uint16_t>(capture.frameBytes);
    (void)frameSha256(capture.frameData, capture.frameBytes,
                      state.frameSha256);
  }
  return state;
}

}  // namespace

bool frameSha256(const uint8_t* frame, size_t frameBytes,
                 uint8_t output[kFrameDigestBytes]) {
  if (!frame || frameBytes == 0U || frameBytes > kMaximumFrameBytes ||
      !output) {
    return false;
  }
  Sha256Context context{};
  sha256Start(context);
  sha256Update(context, frame, frameBytes);
  sha256Finish(context, output);
  return true;
}

bool validCapture(const Capture& capture) {
  if (!validSurface(capture.surface)) return false;
  if (!capture.packValid) {
    const bool validAbsentName = !capture.packName ||
        strcmp(capture.packName, "None") == 0;
    return validAbsentName && capture.packId == 0U &&
           capture.packRevision == 0U && capture.packTotalBytes == 0U &&
           capture.packPayloadCrc32 == 0U &&
           capture.packHeaderCrc32 == 0U &&
           capture.packFormatVersion == 0U && capture.frameWidth == 0U &&
           capture.frameHeight == 0U && capture.frameCount == 0U &&
           capture.appearanceVariant == 0U && !capture.animationActive &&
           !capture.animationFinite && capture.requestedRole == Role::Unknown &&
           capture.resolvedRole == Role::Unknown &&
           capture.playback == Playback::Unknown &&
           capture.animationToken == 0U &&
           capture.animationElapsedMs == 0U && !capture.frameData &&
           capture.frameBytes == 0U && !capture.frameVisible;
  }

  const size_t expectedBytes = expectedFrameBytes(capture);
  const uint64_t minimumPackBytes =
      UINT64_C(64) + static_cast<uint64_t>(capture.frameCount) *
                         static_cast<uint64_t>(expectedBytes);
  if (!validPackName(capture.packName) || capture.packId == 0U ||
      capture.packRevision == 0U || capture.packTotalBytes <= 64U ||
      capture.frameCount == 0U || expectedBytes == 0U ||
      capture.packTotalBytes < minimumPackBytes) {
    return false;
  }
  if ((capture.frameData == nullptr) != (capture.frameBytes == 0U)) {
    return false;
  }
  if (capture.frameData && capture.frameBytes != expectedBytes) return false;

  if (capture.animationActive) {
    if (!validRole(capture.requestedRole) ||
        !validRole(capture.resolvedRole) ||
        !validPlayback(capture.playback) || capture.animationToken == 0U) {
      return false;
    }
  } else if (capture.animationFinite ||
             capture.requestedRole != Role::Unknown ||
             capture.resolvedRole != Role::Unknown ||
             capture.playback != Playback::Unknown ||
             capture.animationToken != 0U ||
             capture.animationElapsedMs != 0U || capture.frameData) {
    return false;
  }
  return !capture.frameVisible ||
         (capture.displayAwake && capture.animationActive &&
          capture.frameData != nullptr);
}

const char* surfaceName(Surface surface) {
  switch (surface) {
    case Surface::Pet: return "pet";
    case Surface::Menu: return "menu";
    case Surface::Connect: return "connect";
    case Surface::Inbox: return "inbox";
    case Surface::GameMenu: return "game_menu";
    case Surface::Game: return "game";
    case Surface::Listen: return "listen";
    case Surface::Sleep: return "sleep";
    case Surface::Status: return "status";
    case Surface::PairPhone: return "pair_phone";
    case Surface::ControllerManager: return "controller_manager";
    case Surface::ControllerConfirm: return "controller_confirm";
    case Surface::ControllerResult: return "controller_result";
    case Surface::WildEncounter: return "wild_encounter";
    case Surface::FieldGuide: return "field_guide";
    case Surface::Goals: return "goals";
    case Surface::Clock: return "clock";
    case Surface::Adventure: return "adventure";
    case Surface::Activity: return "activity";
    case Surface::Unknown: return "unknown";
  }
  return "unknown";
}

const char* roleName(Role role) {
  switch (role) {
    case Role::Idle: return "idle";
    case Role::Blink: return "blink";
    case Role::Pet: return "pet";
    case Role::Sleep: return "sleep";
    case Role::Listen: return "listen";
    case Role::Surprise: return "surprise";
    case Role::Play: return "play";
    case Role::Tired: return "tired";
    case Role::Feed: return "feed";
    case Role::Wake: return "wake";
    case Role::Meet: return "meet";
    case Role::Evolve: return "evolve";
    case Role::Unknown: return "unknown";
  }
  return "unknown";
}

const char* playbackName(Playback playback) {
  switch (playback) {
    case Playback::Hold: return "hold";
    case Playback::Once: return "once";
    case Playback::Loop: return "loop";
    case Playback::PingPong: return "ping_pong";
    case Playback::Unknown: return "unknown";
  }
  return "unknown";
}

const char* statusName(Status status) {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::Duplicate: return "duplicate";
    case Status::InvalidArgument: return "invalid_argument";
    case Status::Busy: return "busy";
    case Status::StaleSession: return "stale_session";
    case Status::NoSession: return "no_session";
    case Status::WrongSession: return "wrong_session";
    case Status::DigestMismatch: return "digest_mismatch";
    case Status::NoFrame: return "no_frame";
    case Status::OutOfRange: return "out_of_range";
    case Status::BufferTooSmall: return "buffer_too_small";
  }
  return "invalid_argument";
}

PetPresentationPreview::PetPresentationPreview() { reset(); }

void PetPresentationPreview::reset() {
  memset(frame_, 0, sizeof(frame_));
  state_ = PresentationState{};
  lastSessionId_ = 0U;
  active_ = false;
}

Status PetPresentationPreview::open(uint32_t sessionId,
                                    const Capture& capture,
                                    PresentationState& state) {
  state = PresentationState{};
  if (sessionId == 0U) return Status::InvalidArgument;
  if (active_) {
    if (sessionId != state_.sessionId) return Status::Busy;
    state = state_;
    return Status::Duplicate;
  }
  if (sessionId == lastSessionId_) return Status::StaleSession;
  if (!validCapture(capture)) return Status::InvalidArgument;

  const PresentationState captured = makeState(sessionId, capture);
  memset(frame_, 0, sizeof(frame_));
  if (capture.frameData) {
    memcpy(frame_, capture.frameData, capture.frameBytes);
  }
  state_ = captured;
  lastSessionId_ = sessionId;
  active_ = true;
  state = state_;
  return Status::Ok;
}

Status PetPresentationPreview::describe(uint32_t sessionId,
                                        PresentationState& state) const {
  state = PresentationState{};
  if (!active_) return Status::NoSession;
  if (sessionId == 0U || sessionId != state_.sessionId) {
    return Status::WrongSession;
  }
  state = state_;
  return Status::Ok;
}

Status PetPresentationPreview::readChunk(const ChunkRequest& request,
                                         uint8_t* destination,
                                         size_t destinationCapacity,
                                         ChunkResult& result) const {
  result = ChunkResult{};
  if (!active_) return Status::NoSession;
  if (request.sessionId == 0U || request.sessionId != state_.sessionId) {
    return Status::WrongSession;
  }
  if (!sameDigest(request.expectedFrameSha256, state_.frameSha256)) {
    return Status::DigestMismatch;
  }
  if (!state_.frameAvailable) return Status::NoFrame;
  if (request.bytes == 0U || request.bytes > kMaximumChunkBytes) {
    return Status::InvalidArgument;
  }
  const uint32_t end = static_cast<uint32_t>(request.offset) + request.bytes;
  if (request.offset >= state_.frameBytes || end > state_.frameBytes) {
    return Status::OutOfRange;
  }
  if (!destination || destinationCapacity < request.bytes) {
    return Status::BufferTooSmall;
  }

  memcpy(destination, frame_ + request.offset, request.bytes);
  result.offset = request.offset;
  result.bytes = request.bytes;
  result.nextOffset = static_cast<uint16_t>(end);
  result.complete = end == state_.frameBytes;
  return Status::Ok;
}

Status PetPresentationPreview::close(uint32_t sessionId) {
  if (!active_) {
    return sessionId != 0U && sessionId == lastSessionId_
               ? Status::Duplicate
               : Status::NoSession;
  }
  if (sessionId == 0U || sessionId != state_.sessionId) {
    return Status::WrongSession;
  }
  memset(frame_, 0, sizeof(frame_));
  state_ = PresentationState{};
  active_ = false;
  return Status::Ok;
}

}  // namespace presentation
}  // namespace kitsu868
