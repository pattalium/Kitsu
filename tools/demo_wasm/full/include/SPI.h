#pragma once

#include <cstdint>

class SPIClass {
 public:
  void begin(int8_t = -1, int8_t = -1, int8_t = -1, int8_t = -1) {}
};

inline SPIClass SPI;
