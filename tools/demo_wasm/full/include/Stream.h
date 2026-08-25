#pragma once

#include <cstddef>
#include <cstdint>

class Stream {
 public:
  virtual ~Stream() = default;
  virtual size_t readBytes(uint8_t*, size_t) { return 0U; }
  virtual size_t write(const uint8_t*, size_t) { return 0U; }
  virtual size_t write(uint8_t value) { return write(&value, 1U); }
  virtual void print(const char*) {}
  virtual void print(char) {}
  virtual void println() {}
};
