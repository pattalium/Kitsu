#include "../src/wild_creature_catalog.h"

#include <cstring>
#include <iostream>

int main() {
  using namespace kitsu868;
  int failures = 0;
  for (uint8_t raw = 0U; raw < static_cast<uint8_t>(signal::Rarity::Count);
       ++raw) {
    wild::Creature creature{};
    const signal::Rarity rarity = static_cast<signal::Rarity>(raw);
    const bool expectedPublished = rarity == signal::Rarity::Common;
    if (!wild::creatureForRarity(rarity, 0U, creature) ||
        creature.packId == 0U || creature.name == nullptr ||
        creature.name[0] == '\0' || creature.rarity != rarity ||
        creature.packPublished != expectedPublished) {
      ++failures;
    }
    const uint8_t* portrait = nullptr;
    size_t portraitBytes = 0U;
    if (!wild::portraitBitmap(creature.portrait, portrait, portraitBytes) ||
        portrait == nullptr || portraitBytes != wild::kPortraitBytes) {
      ++failures;
    }
    wild::Creature roundtrip{};
    if (!wild::creatureByPackId(creature.packId, roundtrip) ||
        std::strcmp(roundtrip.name, creature.name) != 0) {
      ++failures;
    }
  }
  for (size_t index = 0U; index < wild::creatureCount(); ++index) {
    wild::Creature creature{};
    if (!wild::creatureAt(index, creature) ||
        std::strcmp(creature.name, "Fox Girl") == 0) {
      ++failures;
    }
  }
  if (failures != 0) {
    std::cerr << "TEST_FAIL wild_creature_catalog failures=" << failures
              << '\n';
    return 1;
  }
  std::cout << "TEST_PASS wild_creature_catalog creatures="
            << wild::creatureCount() << " public_fox_girl=0\n";
  return 0;
}
