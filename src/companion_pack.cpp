#include "companion_pack.h"

#include <cstring>

namespace {

constexpr char PACK_MAGIC[8] = {'K', '8', '6', '8', 'P', 'K', '1', '\0'};
constexpr uint16_t MAX_CLIPS = 512;
constexpr uint32_t MAX_STEPS = 65535;
constexpr uint16_t MAX_STEPS_PER_CLIP = 256;
constexpr uint16_t MIN_STEP_MS = 100;
constexpr uint16_t MAX_STEP_MS = 60000;

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length) {
  while (length--) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320UL & (0U - (crc & 1U)));
    }
  }
  return crc;
}

bool isPrintableName(const char* name, size_t length) {
  bool foundTerminator = false;
  bool foundCharacter = false;
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value = static_cast<uint8_t>(name[index]);
    if (value == 0) {
      foundTerminator = true;
      continue;
    }
    if (foundTerminator || value < 0x20 || value > 0x7e) return false;
    foundCharacter = true;
  }
  return foundCharacter;
}

}  // namespace

bool CompanionPack::readAt(uint32_t offset, void* destination, size_t length) const {
  if (!partition_) return false;
  const uint64_t end = static_cast<uint64_t>(offset) + length;
  if (end > partition_->size) return false;
  return esp_partition_read(partition_, offset, destination, length) == ESP_OK;
}

void CompanionPack::fail(const char* reason) {
  valid_ = false;
  activeClipValid_ = false;
  cachedFrameIndex_ = 0xffff;
  activeForwardDurationMs_ = 0;
  activeCycleDurationMs_ = 0;
  error_ = reason;
}

bool CompanionPack::validateHeader() {
  if (header_.formatVersion != KITSU_PACK_VERSION ||
      header_.headerBytes != KITSU_PACK_HEADER_BYTES) {
    fail("version");
    return false;
  }
  if (header_.width != KITSU_FRAME_WIDTH ||
      header_.height != KITSU_FRAME_HEIGHT ||
      header_.flags != 0 || header_.packId == 0 || header_.revision == 0) {
    fail("format");
    return false;
  }
  if (header_.frameCount == 0 || header_.clipCount == 0 ||
      header_.clipCount > MAX_CLIPS || header_.stepCount == 0 ||
      header_.stepCount > MAX_STEPS) {
    fail("counts");
    return false;
  }

  const uint64_t expected = KITSU_PACK_HEADER_BYTES +
      static_cast<uint64_t>(header_.clipCount) * sizeof(KitsuPackClip) +
      static_cast<uint64_t>(header_.stepCount) * sizeof(KitsuPackStep) +
      static_cast<uint64_t>(header_.frameCount) * KITSU_FRAME_BYTES;
  if (expected != header_.totalBytes || expected > partition_->size) {
    fail("size");
    return false;
  }
  if (!isPrintableName(header_.displayName, sizeof(header_.displayName))) {
    fail("name");
    return false;
  }

  uint8_t copy[sizeof(KitsuPackHeader)];
  memcpy(copy, &header_, sizeof(copy));
  memset(copy + 0x14, 0, sizeof(uint32_t));
  uint32_t crc = crc32Update(0xffffffffUL, copy + 8, sizeof(copy) - 8);
  crc = ~crc;
  if (crc != header_.headerCrc32) {
    fail("header-crc");
    return false;
  }
  return true;
}

bool CompanionPack::validatePayload() {
  uint8_t scratch[512];
  uint32_t remaining = header_.totalBytes - KITSU_PACK_HEADER_BYTES;
  uint32_t offset = KITSU_PACK_HEADER_BYTES;
  uint32_t crc = 0xffffffffUL;
  while (remaining) {
    const size_t amount = remaining < sizeof(scratch) ? remaining : sizeof(scratch);
    if (!readAt(offset, scratch, amount)) {
      fail("payload-read");
      return false;
    }
    crc = crc32Update(crc, scratch, amount);
    remaining -= amount;
    offset += amount;
    yield();
  }
  crc = ~crc;
  if (crc != header_.payloadCrc32) {
    fail("payload-crc");
    return false;
  }
  return true;
}

bool CompanionPack::readClip(uint16_t index, KitsuPackClip& clip) const {
  if (index >= header_.clipCount) return false;
  const uint32_t offset = KITSU_PACK_HEADER_BYTES +
      static_cast<uint32_t>(index) * sizeof(KitsuPackClip);
  return readAt(offset, &clip, sizeof(clip));
}

bool CompanionPack::readStep(uint32_t index, KitsuPackStep& step) const {
  if (index >= header_.stepCount) return false;
  const uint32_t stepsOffset = KITSU_PACK_HEADER_BYTES +
      static_cast<uint32_t>(header_.clipCount) * sizeof(KitsuPackClip);
  return readAt(stepsOffset + index * sizeof(KitsuPackStep), &step, sizeof(step));
}

bool CompanionPack::validateTables() {
  bool hasIdle = false;
  for (uint16_t index = 0; index < header_.clipCount; ++index) {
    if ((index & 0x3fU) == 0) yield();
    KitsuPackClip clip{};
    if (!readClip(index, clip)) {
      fail("clip-read");
      return false;
    }
    if (clip.role > static_cast<uint8_t>(CompanionRole::Evolve) ||
        clip.mode > static_cast<uint8_t>(PackPlayback::PingPong) ||
        clip.weight == 0 || clip.stepCount == 0 ||
        clip.stepCount > MAX_STEPS_PER_CLIP || clip.reserved != 0) {
      fail("clip");
      return false;
    }
    const uint64_t stepEnd = static_cast<uint64_t>(clip.firstStep) + clip.stepCount;
    if (stepEnd > header_.stepCount ||
        (clip.mode == static_cast<uint8_t>(PackPlayback::Hold) && clip.stepCount != 1)) {
      fail("clip-range");
      return false;
    }
    if (clip.role == static_cast<uint8_t>(CompanionRole::Idle) &&
        clip.appearanceVariant == 0) {
      hasIdle = true;
    }
  }
  if (!hasIdle) {
    fail("no-idle");
    return false;
  }

  for (uint32_t index = 0; index < header_.stepCount; ++index) {
    if ((index & 0x3fU) == 0) yield();
    KitsuPackStep step{};
    if (!readStep(index, step)) {
      fail("step-read");
      return false;
    }
    if (step.frameIndex >= header_.frameCount ||
        step.durationMs < MIN_STEP_MS || step.durationMs > MAX_STEP_MS) {
      fail("step");
      return false;
    }
  }
  return true;
}

bool CompanionPack::begin() {
  partition_ = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!partition_) {
    fail("no-partition");
    return false;
  }
  if (!readAt(0, &header_, sizeof(header_))) {
    fail("header-read");
    return false;
  }

  bool erased = true;
  for (uint8_t value : header_.magic) {
    if (value != 0xff) {
      erased = false;
      break;
    }
  }
  if (memcmp(header_.magic, PACK_MAGIC, sizeof(PACK_MAGIC)) != 0) {
    present_ = false;
    fail(erased ? "empty" : "magic");
    return false;
  }
  present_ = true;
  if (!validateHeader() || !validatePayload() || !validateTables()) return false;

  memcpy(displayName_, header_.displayName, sizeof(header_.displayName));
  displayName_[sizeof(header_.displayName)] = '\0';
  valid_ = true;
  error_ = "none";
  activeClipValid_ = false;
  cachedFrameIndex_ = 0xffff;
  return true;
}

bool CompanionPack::chooseExactClip(CompanionRole role, uint8_t appearanceVariant,
                                    uint8_t playbackMask,
                                    KitsuPackClip& result) const {
  uint32_t totalWeight = 0;
  for (uint16_t index = 0; index < header_.clipCount; ++index) {
    if ((index & 0x3fU) == 0) yield();
    KitsuPackClip clip{};
    if (!readClip(index, clip)) return false;
    if (clip.role == static_cast<uint8_t>(role) &&
        clip.appearanceVariant == appearanceVariant &&
        (playbackMask & (1U << clip.mode))) {
      totalWeight += clip.weight;
    }
  }
  if (!totalWeight) return false;

  uint32_t choice = esp_random() % totalWeight;
  for (uint16_t index = 0; index < header_.clipCount; ++index) {
    if ((index & 0x3fU) == 0) yield();
    KitsuPackClip clip{};
    if (!readClip(index, clip)) return false;
    if (clip.role != static_cast<uint8_t>(role) ||
        clip.appearanceVariant != appearanceVariant ||
        !(playbackMask & (1U << clip.mode))) continue;
    if (choice < clip.weight) {
      result = clip;
      return true;
    }
    choice -= clip.weight;
  }
  return false;
}

bool CompanionPack::chooseClip(CompanionRole role, uint8_t appearanceVariant,
                               uint8_t playbackMask, bool allowIdleFallback,
                               KitsuPackClip& result) const {
  if (chooseExactClip(role, appearanceVariant, playbackMask, result)) return true;
  if (appearanceVariant &&
      chooseExactClip(role, 0, playbackMask, result)) return true;
  if (!allowIdleFallback || role == CompanionRole::Idle) return false;

  // A missing optional role falls back to Idle regardless of its playback
  // mode. This keeps a valid companion coherent without mixing in artwork
  // from another source.
  if (chooseExactClip(CompanionRole::Idle, appearanceVariant,
                      KITSU_PLAYBACK_ANY, result)) return true;
  return chooseExactClip(CompanionRole::Idle, 0, KITSU_PLAYBACK_ANY, result);
}

uint32_t CompanionPack::clipDurationMs(const KitsuPackClip& clip) const {
  uint64_t duration = 0;
  for (uint16_t index = 0; index < clip.stepCount; ++index) {
    KitsuPackStep step{};
    if (!readStep(clip.firstStep + index, step)) return 0;
    duration += step.durationMs;
  }
  return duration > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(duration);
}

uint32_t CompanionPack::clipCycleDurationMs(const KitsuPackClip& clip) const {
  const uint32_t forwardDuration = clipDurationMs(clip);
  if (!forwardDuration ||
      static_cast<PackPlayback>(clip.mode) != PackPlayback::PingPong ||
      clip.stepCount <= 2) {
    return forwardDuration;
  }

  uint64_t duration = forwardDuration;
  for (uint16_t index = 1; index + 1 < clip.stepCount; ++index) {
    KitsuPackStep step{};
    if (!readStep(clip.firstStep + index, step)) return 0;
    duration += step.durationMs;
  }
  return duration > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(duration);
}

bool CompanionPack::stepForElapsed(const KitsuPackClip& clip, uint32_t elapsedMs,
                                   uint32_t forwardDuration,
                                   uint32_t cycleDuration,
                                   KitsuPackStep& result) const {
  const PackPlayback mode = static_cast<PackPlayback>(clip.mode);
  if (mode == PackPlayback::Hold) return readStep(clip.firstStep, result);

  if (!forwardDuration) return false;

  uint32_t cursor = elapsedMs;
  if (mode == PackPlayback::Loop) {
    cursor %= forwardDuration;
  } else if (mode == PackPlayback::PingPong && clip.stepCount > 1) {
    if (!cycleDuration) return false;
    cursor %= cycleDuration;
    if (cursor >= forwardDuration) {
      cursor -= forwardDuration;
      for (int32_t index = static_cast<int32_t>(clip.stepCount) - 2; index >= 1; --index) {
        KitsuPackStep step{};
        if (!readStep(clip.firstStep + index, step)) return false;
        if (cursor < step.durationMs) {
          result = step;
          return true;
        }
        cursor -= step.durationMs;
      }
    }
  } else if (cursor >= forwardDuration) {
    return readStep(clip.firstStep + clip.stepCount - 1, result);
  }

  for (uint16_t index = 0; index < clip.stepCount; ++index) {
    KitsuPackStep step{};
    if (!readStep(clip.firstStep + index, step)) return false;
    if (cursor < step.durationMs) {
      result = step;
      return true;
    }
    cursor -= step.durationMs;
  }
  return readStep(clip.firstStep + clip.stepCount - 1, result);
}

bool CompanionPack::activateClip(CompanionRole role, uint32_t token,
                                 CompanionClipActivation& activation,
                                 uint8_t appearanceVariant,
                                 uint8_t playbackMask,
                                 bool allowIdleFallback) {
  if (!valid_) return false;

  KitsuPackClip selected{};
  if (!chooseClip(role, appearanceVariant, playbackMask,
                  allowIdleFallback, selected)) {
    return false;
  }

  const uint32_t forwardDuration = clipDurationMs(selected);
  const uint32_t cycleDuration = clipCycleDurationMs(selected);
  if (!forwardDuration || !cycleDuration) {
    fail("clip-runtime");
    return false;
  }

  activeClip_ = selected;
  activeClipValid_ = true;
  activeRole_ = static_cast<CompanionRole>(selected.role);
  activeVariant_ = selected.appearanceVariant;
  activeToken_ = token;
  activeForwardDurationMs_ = forwardDuration;
  activeCycleDurationMs_ = cycleDuration;
  cachedFrameIndex_ = 0xffff;

  activation.requestedRole = role;
  activation.clipRole = activeRole_;
  activation.mode = static_cast<PackPlayback>(selected.mode);
  activation.token = token;
  activation.forwardDurationMs = forwardDuration;
  activation.cycleDurationMs = cycleDuration;
  return true;
}

const uint8_t* CompanionPack::activeFrame(uint32_t elapsedMs) {
  if (!valid_) return nullptr;
  if (!activeClipValid_) return nullptr;

  KitsuPackStep step{};
  if (!stepForElapsed(activeClip_, elapsedMs, activeForwardDurationMs_,
                      activeCycleDurationMs_, step)) {
    fail("step-runtime");
    return nullptr;
  }
  if (step.frameIndex == cachedFrameIndex_) return frameBuffer_;

  const uint32_t frameOffset = KITSU_PACK_HEADER_BYTES +
      static_cast<uint32_t>(header_.clipCount) * sizeof(KitsuPackClip) +
      header_.stepCount * sizeof(KitsuPackStep) +
      static_cast<uint32_t>(step.frameIndex) * KITSU_FRAME_BYTES;
  if (!readAt(frameOffset, frameBuffer_, sizeof(frameBuffer_))) {
    fail("frame-read");
    return nullptr;
  }
  cachedFrameIndex_ = step.frameIndex;
  return frameBuffer_;
}
