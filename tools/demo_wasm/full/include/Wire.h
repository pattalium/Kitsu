#pragma once

#include "Arduino.h"

class TwoWire {
 public:
  bool begin(int = -1, int = -1) { return true; }
  void beginTransmission(uint8_t address) { address_ = address; }
  uint8_t endTransmission() { return address_ == 0x3cU ? 0U : 2U; }

 private:
  uint8_t address_ = 0U;
};

inline TwoWire Wire;
