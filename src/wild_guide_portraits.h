#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wild_creature_catalog.h"

namespace kitsu868 {
namespace wild {

// Full native OLED portraits used only by the on-device guide and encounter
// screens. The compact 16x18 catalog portraits remain the radio/app protocol
// authority and are intentionally independent from these flash-only frames.
constexpr uint8_t kGuidePortraitWidth = 64U;
constexpr uint8_t kGuidePortraitHeight = 80U;
constexpr size_t kGuidePortraitBytes =
    static_cast<size_t>(kGuidePortraitWidth / 8U) * kGuidePortraitHeight;

bool guidePortraitBitmap(Portrait portrait, const uint8_t*& output,
                         size_t& outputBytes);

}  // namespace wild
}  // namespace kitsu868
