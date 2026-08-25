#pragma once

#include "Arduino.h"

class Preferences {
 public:
  bool begin(const char* name, bool readOnly = false,
             const char* partitionLabel = nullptr);
  void end();
  bool clear();
  bool remove(const char* key);

  size_t getBytesLength(const char* key) const;
  size_t getBytes(const char* key, void* output, size_t capacity) const;
  size_t putBytes(const char* key, const void* input, size_t bytes);

  uint32_t getUInt(const char* key, uint32_t fallback = 0U) const;
  size_t putUInt(const char* key, uint32_t value);
  uint16_t getUShort(const char* key, uint16_t fallback = 0U) const;
  uint8_t getUChar(const char* key, uint8_t fallback = 0U) const;
  bool getBool(const char* key, bool fallback = false) const;
  String getString(const char* key, const char* fallback = "") const;

 private:
  char namespace_[24]{};
  bool begun_ = false;
  bool readOnly_ = false;
};
