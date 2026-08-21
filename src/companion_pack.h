#pragma once

#include <Arduino.h>
#include <esp_partition.h>

constexpr uint16_t KITSU_FRAME_WIDTH = 64;
constexpr uint16_t KITSU_FRAME_HEIGHT = 64;
constexpr size_t KITSU_FRAME_BYTES = 512;
constexpr uint16_t KITSU_PACK_VERSION = 1;
constexpr uint16_t KITSU_PACK_HEADER_BYTES = 64;

enum class CompanionRole : uint8_t {
  Idle = 0,
  Blink = 1,
  Pet = 2,
  Sleep = 3,
  Listen = 4,
  Surprise = 5,
  Play = 6,
  Tired = 7,
  Feed = 8,
  Wake = 9,
  Meet = 10,
  Evolve = 11,
};

enum class PackPlayback : uint8_t {
  Hold = 0,
  Once = 1,
  Loop = 2,
  PingPong = 3,
};

constexpr uint8_t KITSU_PLAYBACK_HOLD = 1U << static_cast<uint8_t>(PackPlayback::Hold);
constexpr uint8_t KITSU_PLAYBACK_ONCE = 1U << static_cast<uint8_t>(PackPlayback::Once);
constexpr uint8_t KITSU_PLAYBACK_LOOP = 1U << static_cast<uint8_t>(PackPlayback::Loop);
constexpr uint8_t KITSU_PLAYBACK_PINGPONG =
    1U << static_cast<uint8_t>(PackPlayback::PingPong);
constexpr uint8_t KITSU_PLAYBACK_ANY = KITSU_PLAYBACK_HOLD |
    KITSU_PLAYBACK_ONCE | KITSU_PLAYBACK_LOOP | KITSU_PLAYBACK_PINGPONG;

#pragma pack(push, 1)
struct KitsuPackHeader {
  char magic[8];
  uint16_t formatVersion;
  uint16_t headerBytes;
  uint32_t totalBytes;
  uint32_t payloadCrc32;
  uint32_t headerCrc32;
  uint32_t packId;
  uint32_t revision;
  uint16_t width;
  uint16_t height;
  uint16_t frameCount;
  uint16_t clipCount;
  uint32_t stepCount;
  uint32_t flags;
  char displayName[16];
};

struct KitsuPackClip {
  uint8_t role;
  uint8_t appearanceVariant;
  uint8_t mode;
  uint8_t weight;
  uint32_t firstStep;
  uint16_t stepCount;
  uint16_t reserved;
};

struct KitsuPackStep {
  uint16_t frameIndex;
  uint16_t durationMs;
};
#pragma pack(pop)

static_assert(sizeof(KitsuPackHeader) == 64, "Kitsu pack header must be 64 bytes");
static_assert(sizeof(KitsuPackClip) == 12, "Kitsu pack clip must be 12 bytes");
static_assert(sizeof(KitsuPackStep) == 4, "Kitsu pack step must be 4 bytes");

struct CompanionClipActivation {
  CompanionRole requestedRole = CompanionRole::Idle;
  CompanionRole clipRole = CompanionRole::Idle;
  PackPlayback mode = PackPlayback::Hold;
  uint32_t token = 0;
  uint32_t forwardDurationMs = 0;
  uint32_t cycleDurationMs = 0;
};

class CompanionPack {
 public:
  bool begin();

  bool present() const { return present_; }
  bool valid() const { return valid_; }
  const char* error() const { return error_; }
  const char* name() const { return valid_ ? displayName_ : "None"; }
  uint32_t id() const { return valid_ ? header_.packId : 0; }
  uint32_t revision() const { return valid_ ? header_.revision : 0; }
  uint16_t frameCount() const { return valid_ ? header_.frameCount : 0; }
  uint32_t bytes() const { return valid_ ? header_.totalBytes : 0; }
  size_t capacity() const { return partition_ ? partition_->size : 0; }

  // Selects one exact weighted clip and keeps it active until the next call.
  // Rendering and timing therefore always refer to the same selected clip.
  bool activateClip(CompanionRole role, uint32_t token,
                    CompanionClipActivation& activation,
                    uint8_t appearanceVariant = 0,
                    uint8_t playbackMask = KITSU_PLAYBACK_ANY,
                    bool allowIdleFallback = true);
  const uint8_t* activeFrame(uint32_t elapsedMs);

 private:
  bool readAt(uint32_t offset, void* destination, size_t length) const;
  bool validateHeader();
  bool validatePayload();
  bool validateTables();
  bool chooseClip(CompanionRole role, uint8_t appearanceVariant,
                  uint8_t playbackMask, bool allowIdleFallback,
                  KitsuPackClip& result) const;
  bool chooseExactClip(CompanionRole role, uint8_t appearanceVariant,
                       uint8_t playbackMask, KitsuPackClip& result) const;
  bool readClip(uint16_t index, KitsuPackClip& clip) const;
  bool readStep(uint32_t index, KitsuPackStep& step) const;
  uint32_t clipDurationMs(const KitsuPackClip& clip) const;
  uint32_t clipCycleDurationMs(const KitsuPackClip& clip) const;
  bool stepForElapsed(const KitsuPackClip& clip, uint32_t elapsedMs,
                      uint32_t forwardDurationMs, uint32_t cycleDurationMs,
                      KitsuPackStep& result) const;
  void fail(const char* reason);

  const esp_partition_t* partition_ = nullptr;
  KitsuPackHeader header_{};
  char displayName_[17] = "None";
  bool present_ = false;
  bool valid_ = false;
  const char* error_ = "not-started";

  bool activeClipValid_ = false;
  CompanionRole activeRole_ = CompanionRole::Idle;
  uint8_t activeVariant_ = 0;
  uint32_t activeToken_ = 0;
  KitsuPackClip activeClip_{};
  uint32_t activeForwardDurationMs_ = 0;
  uint32_t activeCycleDurationMs_ = 0;
  uint16_t cachedFrameIndex_ = 0xffff;
  uint8_t frameBuffer_[KITSU_FRAME_BYTES]{};
};
