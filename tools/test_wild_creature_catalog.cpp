#include "../src/wild_creature_catalog.h"

#include <cstring>
#include <iostream>

int main() {
  using namespace kitsu868;
  int failures = 0;
  struct ExpectedCreature {
    uint32_t packId;
    const char* name;
    signal::Rarity rarity;
  };
  const ExpectedCreature expected[] = {
      {0x5cac86a3UL, "Frog", signal::Rarity::Common},
      {0x13793dc7UL, "Hamster", signal::Rarity::Common},
      {0x7495dbfbUL, "Turtle", signal::Rarity::Common},
      {0x68d9554eUL, "Rabbit", signal::Rarity::Uncommon},
      {0x5df6be74UL, "Hedgehog", signal::Rarity::Uncommon},
      {0xe59408e0UL, "Ferret", signal::Rarity::Uncommon},
      {0x29b4b2f7UL, "Otter", signal::Rarity::Rare},
      {0x69276d0cUL, "Axolotl", signal::Rarity::Rare},
      {0x2dfb0797UL, "Chinchilla", signal::Rarity::Rare},
      {0xc163efedUL, "Raccoon", signal::Rarity::VeryRare},
      {0x374d2540UL, "Capybara", signal::Rarity::VeryRare},
      {0x39fc5b1aUL, "Sugar Glider", signal::Rarity::VeryRare},
      {0x91a2de7bUL, "Red Panda", signal::Rarity::Epic},
      {0xe04ec405UL, "Pangolin", signal::Rarity::Epic},
      {0x8e0e1b03UL, "Tasmanian Devil", signal::Rarity::Epic},
      {0x533b9b30UL, "Snow Leopard", signal::Rarity::Legendary},
      {0x86f3bb5dUL, "Okapi", signal::Rarity::Legendary},
      {0x2d1d89afUL, "Shoebill", signal::Rarity::Legendary},
      {0xa52160c5UL, "Cat Girl", signal::Rarity::Mythical},
      {0xf0f750bdUL, "Rabbit Girl", signal::Rarity::Mythical},
      {0x52a1c03aUL, "Deer Girl", signal::Rarity::Mythical},
  };
  if (wild::creatureCount() != wild::kCatalogCreatureCount ||
      wild::creatureCount() != sizeof(expected) / sizeof(expected[0])) {
    ++failures;
  }

  const uint8_t* portraits[wild::kCatalogCreatureCount]{};
  for (uint8_t raw = 0U; raw < static_cast<uint8_t>(signal::Rarity::Count);
       ++raw) {
    const signal::Rarity rarity = static_cast<signal::Rarity>(raw);
    for (uint32_t selection = 0U;
         selection < wild::kCreaturesPerRarity; ++selection) {
      wild::Creature creature{};
      if (!wild::creatureForRarity(rarity, selection, creature) ||
          creature.packId == 0U || creature.name == nullptr ||
          creature.name[0] == '\0' || creature.rarity != rarity ||
          !creature.packPublished) {
        ++failures;
      }
      wild::Creature wrapped{};
      if (!wild::creatureForRarity(
              rarity, selection + wild::kCreaturesPerRarity, wrapped) ||
          wrapped.packId != creature.packId) {
        ++failures;
      }
    }
  }
  for (size_t index = 0U; index < wild::kCatalogCreatureCount; ++index) {
    wild::Creature creature{};
    if (!wild::creatureAt(index, creature) ||
        creature.packId != expected[index].packId ||
        std::strcmp(creature.name, expected[index].name) != 0 ||
        creature.rarity != expected[index].rarity) {
      ++failures;
    }
    wild::Creature roundtrip{};
    if (!wild::creatureByPackId(creature.packId, roundtrip) ||
        std::strcmp(roundtrip.name, creature.name) != 0) {
      ++failures;
    }
    size_t portraitBytes = 0U;
    if (!wild::portraitBitmap(creature.portrait, portraits[index],
                              portraitBytes) ||
        portraits[index] == nullptr ||
        portraitBytes != wild::kPortraitBytes) {
      ++failures;
    }
    for (size_t prior = 0U; prior < index; ++prior) {
      if (portraits[index] && portraits[prior] &&
          std::memcmp(portraits[index], portraits[prior],
                      wild::kPortraitBytes) == 0) {
        ++failures;
      }
      if (creature.packId == expected[prior].packId ||
          std::strcmp(creature.name, expected[prior].name) == 0) {
        ++failures;
      }
    }
  }
  if (failures != 0) {
    std::cerr << "TEST_FAIL wild_creature_catalog failures=" << failures
              << '\n';
    return 1;
  }
  std::cout << "TEST_PASS wild_creature_catalog creatures="
            << wild::creatureCount()
            << " per_rarity=" << wild::kCreaturesPerRarity
            << " distinct_portraits=21 private_companions=0\n";
  return 0;
}
