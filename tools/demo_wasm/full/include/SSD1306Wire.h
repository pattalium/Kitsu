#pragma once

#include "Arduino.h"

constexpr int GEOMETRY_128_64 = 0;

extern "C" {
void kitsu_hal_oled_clear();
void kitsu_hal_oled_present();
void kitsu_hal_oled_power(uint32_t on);
void kitsu_hal_oled_pixel(int16_t physicalX, int16_t physicalY);
}

class SSD1306Wire {
 public:
  SSD1306Wire(uint8_t, uint8_t, uint8_t, int) {}
  bool init() { return true; }
  void clear() { kitsu_hal_oled_clear(); }
  void display() { kitsu_hal_oled_present(); }
  void displayOn() { kitsu_hal_oled_power(1U); }
  void displayOff() { kitsu_hal_oled_power(0U); }
  void flipScreenVertically() {}
  void setPixel(int16_t x, int16_t y) { kitsu_hal_oled_pixel(x, y); }
};
