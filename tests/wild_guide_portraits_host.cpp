#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wild_guide_portraits.h"

int main() {
  using kitsu868::wild::Portrait;
  constexpr Portrait portraits[] = {
      Portrait::Frog,
      Portrait::Hamster,
      Portrait::Turtle,
      Portrait::Rabbit,
      Portrait::Hedgehog,
      Portrait::Ferret,
      Portrait::Otter,
      Portrait::Axolotl,
      Portrait::Chinchilla,
      Portrait::Raccoon,
      Portrait::Capybara,
      Portrait::SugarGlider,
      Portrait::RedPanda,
      Portrait::Pangolin,
      Portrait::TasmanianDevil,
      Portrait::SnowLeopard,
      Portrait::Okapi,
      Portrait::Shoebill,
      Portrait::CatGirl,
      Portrait::RabbitGirl,
      Portrait::DeerGirl,
  };
  static_assert(sizeof(portraits) / sizeof(portraits[0]) ==
                    kitsu868::wild::kCatalogCreatureCount,
                "host coverage must match the public catalog");

  const uint8_t* bitmaps[kitsu868::wild::kCatalogCreatureCount] = {};
  for (size_t index = 0U;
       index < kitsu868::wild::kCatalogCreatureCount; ++index) {
    size_t bytes = 0U;
    assert(kitsu868::wild::guidePortraitBitmap(portraits[index],
                                              bitmaps[index], bytes));
    assert(bitmaps[index] != nullptr);
    assert(bytes == kitsu868::wild::kGuidePortraitBytes);

    bool hasInk = false;
    for (size_t byte = 0U; byte < bytes; ++byte) {
      hasInk = hasInk || bitmaps[index][byte] != 0U;
    }
    assert(hasInk);
    for (size_t earlier = 0U; earlier < index; ++earlier) {
      assert(memcmp(bitmaps[earlier], bitmaps[index], bytes) != 0);
    }
  }

  const uint8_t* invalid = reinterpret_cast<const uint8_t*>(1U);
  size_t invalidBytes = 123U;
  assert(!kitsu868::wild::guidePortraitBitmap(
      static_cast<Portrait>(0xffU), invalid, invalidBytes));
  assert(invalid == nullptr);
  assert(invalidBytes == 0U);

  puts("PASS wild_guide_portraits_host");
  return 0;
}
