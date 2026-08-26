#pragma once

#include <stddef.h>
#include <stdint.h>

#include "signal_encounter.h"

namespace kitsu868 {
namespace wild {

// Stable catalog IDs use the same CRC32("kitsu868:<species>") convention as
// the existing Cat, Fox, and Dog packs. A catalog entry reserves identity; it
// does not claim that a downloadable .k868 has been published yet.
enum class Portrait : uint8_t {
  Frog = 0,
  Hamster,
  Turtle,
  Rabbit,
  Hedgehog,
  Ferret,
  Otter,
  Axolotl,
  Chinchilla,
  Raccoon,
  Capybara,
  SugarGlider,
  RedPanda,
  Pangolin,
  TasmanianDevil,
  SnowLeopard,
  Okapi,
  Shoebill,
  CatGirl,
  RabbitGirl,
  DeerGirl,
};

constexpr size_t kCreaturesPerRarity = 3U;
constexpr size_t kCatalogCreatureCount =
    signal::kRarityCount * kCreaturesPerRarity;

// Compact XBM-style 1-bit portraits. Bits are least-significant-bit first in
// each byte and rows are tightly packed. The OLED renderer scales these 2x to
// 32x36 so every catalog creature has a distinct static encounter portrait
// without depending on an installed animation pack.
constexpr uint8_t kPortraitWidth = 16U;
constexpr uint8_t kPortraitHeight = 18U;
constexpr size_t kPortraitBytes =
    static_cast<size_t>(kPortraitWidth / 8U) * kPortraitHeight;

struct Creature {
  uint32_t packId = 0U;
  const char* name = nullptr;
  signal::Rarity rarity = signal::Rarity::Common;
  Portrait portrait = Portrait::Frog;
  bool packPublished = false;

  constexpr Creature()
      : packId(0U),
        name(nullptr),
        rarity(signal::Rarity::Common),
        portrait(Portrait::Frog),
        packPublished(false) {}

  constexpr Creature(uint32_t valuePackId, const char* valueName,
                     signal::Rarity valueRarity, Portrait valuePortrait,
                     bool valuePackPublished)
      : packId(valuePackId),
        name(valueName),
        rarity(valueRarity),
        portrait(valuePortrait),
        packPublished(valuePackPublished) {}
};

size_t creatureCount();
bool creatureAt(size_t index, Creature& output);
bool creatureByPackId(uint32_t packId, Creature& output);
bool creatureForRarity(signal::Rarity rarity, uint32_t selectionEntropy,
                       Creature& output);
bool portraitBitmap(Portrait portrait, const uint8_t*& output,
                    size_t& outputBytes);

}  // namespace wild
}  // namespace kitsu868
