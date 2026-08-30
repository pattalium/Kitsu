#pragma once

#include <stddef.h>
#include <stdint.h>

// Read-only presentation snapshots for accessibility and photo/video preview.
// The caller supplies values from the live CompanionPack/display/animation
// objects. This module never selects an animation, reads flash, changes care
// state or talks to any transport. It only freezes one bounded transfer copy.
namespace kitsu868 {
namespace presentation {

constexpr uint8_t kPresentationSchemaVersion = 1U;
constexpr size_t kFrameDigestBytes = 32U;
constexpr size_t kMaximumPackNameBytes = 16U;
constexpr size_t kMaximumFrameBytes = 640U;
constexpr size_t kMaximumChunkBytes = 192U;
constexpr uint16_t kPackV1Format = 1U;
constexpr uint16_t kPackV2Format = 2U;
constexpr uint16_t kPackFrameWidth = 64U;
constexpr uint16_t kPackV1FrameHeight = 64U;
constexpr uint16_t kPackV2FrameHeight = 80U;
constexpr size_t kPackV1FrameBytes = 512U;
constexpr size_t kPackV2FrameBytes = 640U;

// Values deliberately match main.cpp's existing Screen order. Unknown is
// explicit so future firmware never has to mislabel an unmapped screen.
enum class Surface : uint8_t {
  Pet = 0U,
  Menu,
  Connect,
  Inbox,
  GameMenu,
  Game,
  Listen,
  Sleep,
  Status,
  PairPhone,
  ControllerManager,
  ControllerConfirm,
  ControllerResult,
  WildEncounter,
  FieldGuide,
  Goals,
  Clock,
  Adventure,
  Activity,
  Unknown = 0xFFU,
};

// Values deliberately match CompanionRole. Both requestedRole and resolvedRole
// are retained because a pack can truthfully resolve a missing role to Idle.
enum class Role : uint8_t {
  Idle = 0U,
  Blink,
  Pet,
  Sleep,
  Listen,
  Surprise,
  Play,
  Tired,
  Feed,
  Wake,
  Meet,
  Evolve,
  Unknown = 0xFFU,
};

// Values deliberately match PackPlayback.
enum class Playback : uint8_t {
  Hold = 0U,
  Once,
  Loop,
  PingPong,
  Unknown = 0xFFU,
};

enum class FrameEncoding : uint8_t {
  None = 0U,
  // CompanionPack bytes as consumed by uiXbm(): row-major, one bit per pixel,
  // least-significant bit first within each byte, with no OLED rotation.
  XbmRowMajorLsbFirst,
};

enum class Status : uint8_t {
  Ok = 0U,
  Duplicate,
  InvalidArgument,
  Busy,
  StaleSession,
  NoSession,
  WrongSession,
  DigestMismatch,
  NoFrame,
  OutOfRange,
  BufferTooSmall,
};

struct Capture {
  uint32_t capturedAtMs = 0U;
  Surface surface = Surface::Unknown;
  bool displayAwake = false;
  bool frameVisible = false;

  bool packValid = false;
  const char* packName = nullptr;
  uint32_t packId = 0U;
  uint32_t packRevision = 0U;
  uint32_t packTotalBytes = 0U;
  uint32_t packPayloadCrc32 = 0U;
  uint32_t packHeaderCrc32 = 0U;
  uint16_t packFormatVersion = 0U;
  uint16_t frameWidth = 0U;
  uint16_t frameHeight = 0U;
  uint16_t frameCount = 0U;
  uint8_t appearanceVariant = 0U;

  bool animationActive = false;
  bool animationFinite = false;
  Role requestedRole = Role::Unknown;
  Role resolvedRole = Role::Unknown;
  Playback playback = Playback::Unknown;
  uint32_t animationToken = 0U;
  uint32_t animationElapsedMs = 0U;

  // Pass the exact pointer and size already returned by
  // CompanionPack::activeFrame() for this presentation instant. The module
  // copies it immediately; it never retains this pointer.
  const uint8_t* frameData = nullptr;
  size_t frameBytes = 0U;
};

struct PresentationState {
  uint8_t schemaVersion = kPresentationSchemaVersion;
  // Boot-scoped request identifiers use the same nonzero uint32 contract as
  // the rest of the companion API.
  uint32_t sessionId = 0U;
  uint32_t capturedAtMs = 0U;
  Surface surface = Surface::Unknown;
  bool displayAwake = false;
  bool frameVisible = false;

  bool packValid = false;
  char packName[kMaximumPackNameBytes + 1U]{};
  uint32_t packId = 0U;
  uint32_t packRevision = 0U;
  uint32_t packTotalBytes = 0U;
  uint32_t packPayloadCrc32 = 0U;
  uint32_t packHeaderCrc32 = 0U;
  uint16_t packFormatVersion = 0U;
  uint16_t frameWidth = 0U;
  uint16_t frameHeight = 0U;
  uint16_t frameCount = 0U;
  uint8_t appearanceVariant = 0U;

  bool animationActive = false;
  bool animationFinite = false;
  Role requestedRole = Role::Unknown;
  Role resolvedRole = Role::Unknown;
  Playback playback = Playback::Unknown;
  uint32_t animationToken = 0U;
  uint32_t animationElapsedMs = 0U;

  bool frameAvailable = false;
  FrameEncoding frameEncoding = FrameEncoding::None;
  uint16_t frameBytes = 0U;
  uint8_t frameSha256[kFrameDigestBytes]{};
};

struct ChunkRequest {
  uint32_t sessionId = 0U;
  uint16_t offset = 0U;
  uint16_t bytes = 0U;
  uint8_t expectedFrameSha256[kFrameDigestBytes]{};
};

struct ChunkResult {
  uint16_t offset = 0U;
  uint16_t bytes = 0U;
  uint16_t nextOffset = 0U;
  bool complete = false;
};

bool validCapture(const Capture& capture);
bool frameSha256(const uint8_t* frame, size_t frameBytes,
                 uint8_t output[kFrameDigestBytes]);
const char* surfaceName(Surface surface);
const char* roleName(Role role);
const char* playbackName(Playback playback);
const char* statusName(Status status);

class PetPresentationPreview {
 public:
  PetPresentationPreview();

  void reset();

  // Opens one immutable, boot-scoped read session. An exact retry with the
  // active id returns Duplicate and the original state, never a newer frame.
  // A closed id is retired so a delayed request cannot silently reopen it.
  Status open(uint32_t sessionId, const Capture& capture,
              PresentationState& state);
  Status describe(uint32_t sessionId, PresentationState& state) const;

  // Reads are side-effect free and may be retried or requested out of order.
  // Every request must bind both the active session and its exact frame digest.
  Status readChunk(const ChunkRequest& request, uint8_t* destination,
                   size_t destinationCapacity, ChunkResult& result) const;
  Status close(uint32_t sessionId);

  bool active() const { return active_; }
  uint32_t sessionId() const {
    return active_ ? state_.sessionId : 0U;
  }

 private:
  PresentationState state_{};
  uint8_t frame_[kMaximumFrameBytes]{};
  uint32_t lastSessionId_ = 0U;
  bool active_ = false;
};

static_assert(sizeof(PetPresentationPreview) <= 1024U,
              "presentation preview must remain fixed and small");

}  // namespace presentation
}  // namespace kitsu868
