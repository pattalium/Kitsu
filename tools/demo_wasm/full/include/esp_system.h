#pragma once

#include <cstddef>
#include <cstdint>

#include "Arduino.h"

inline void esp_fill_random(void* output, size_t bytes) {
  (void)kitsu_hal_random_fill(output, bytes);
}
