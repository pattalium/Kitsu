#pragma once

#include <stdint.h>

struct portMUX_TYPE {};

#define portMUX_INITIALIZER_UNLOCKED {}

inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}

namespace fake_arduino {
inline uint32_t nowMillis = 0U;
}

inline uint32_t millis() { return fake_arduino::nowMillis; }

inline void setFakeMillis(uint32_t nowMillis) {
  fake_arduino::nowMillis = nowMillis;
}
