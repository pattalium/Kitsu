#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

#include "Stream.h"

using std::size_t;

#define PROGMEM
#define pgm_read_byte(address) (*reinterpret_cast<const uint8_t*>(address))
#define pgm_read_word(address) (*reinterpret_cast<const uint16_t*>(address))
#define pgm_read_dword(address) (*reinterpret_cast<const uint32_t*>(address))
#define pgm_read_qword(address) (*reinterpret_cast<const uint64_t*>(address))

constexpr int LOW = 0;
constexpr int HIGH = 1;
constexpr int INPUT = 0;
constexpr int OUTPUT = 1;
constexpr int INPUT_PULLUP = 2;
constexpr int ADC_2_5db = 0;
constexpr int RISING = 1;

extern "C" {
uint32_t kitsu_hal_millis();
uint32_t kitsu_hal_micros();
void kitsu_hal_advance_millis(uint32_t milliseconds);
int kitsu_hal_digital_read(uint8_t pin);
void kitsu_hal_digital_write(uint8_t pin, int value);
uint32_t kitsu_hal_random();
uint32_t kitsu_hal_random_fill(void* output, size_t bytes);
uint64_t kitsu_hal_device_id();
void kitsu_hal_serial_write(const char* text, size_t bytes);
int kitsu_hal_serial_available();
int kitsu_hal_serial_read();
uint32_t kitsu_hal_epoch_valid();
uint32_t kitsu_hal_epoch_seconds();
void kitsu_hal_restart_requested();
}

template <typename T>
constexpr T min(T left, T right) {
  return left < right ? left : right;
}

template <typename T>
constexpr T max(T left, T right) {
  return left > right ? left : right;
}

class String {
 public:
  String() = default;
  String(const char* value) : value_(value ? value : "") {}
  String(char value) : value_(1, value) {}
  String(const std::string& value) : value_(value) {}
  String(float value, unsigned decimals = 2) { formatFloat(value, decimals); }
  String(double value, unsigned decimals = 2) { formatFloat(value, decimals); }

  template <typename T,
            typename = std::enable_if_t<std::is_integral_v<T>>>
  String(T value) : value_(std::to_string(value)) {}

  unsigned length() const { return static_cast<unsigned>(value_.size()); }
  bool isEmpty() const { return value_.empty(); }
  const char* c_str() const { return value_.c_str(); }
  char charAt(unsigned index) const {
    return index < value_.size() ? value_[index] : 0;
  }
  char operator[](unsigned index) const { return charAt(index); }
  void reserve(unsigned bytes) { value_.reserve(bytes); }
  void clear() { value_.clear(); }

  void trim() {
    const auto first = value_.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      value_.clear();
      return;
    }
    const auto last = value_.find_last_not_of(" \t\r\n");
    value_ = value_.substr(first, last - first + 1U);
  }

  void toLowerCase() {
    for (char& value : value_) {
      value = static_cast<char>(
          std::tolower(static_cast<unsigned char>(value)));
    }
  }

  void toUpperCase() {
    for (char& value : value_) {
      value = static_cast<char>(
          std::toupper(static_cast<unsigned char>(value)));
    }
  }

  bool startsWith(const char* prefix) const {
    return value_.rfind(prefix ? prefix : "", 0U) == 0U;
  }

  int indexOf(char needle) const {
    const auto found = value_.find(needle);
    return found == std::string::npos ? -1 : static_cast<int>(found);
  }

  int indexOf(char needle, unsigned fromIndex) const {
    const auto found = value_.find(needle, fromIndex);
    return found == std::string::npos ? -1 : static_cast<int>(found);
  }

  int indexOf(const char* needle) const {
    const auto found = value_.find(needle ? needle : "");
    return found == std::string::npos ? -1 : static_cast<int>(found);
  }

  int indexOf(const char* needle, unsigned fromIndex) const {
    const auto found = value_.find(needle ? needle : "", fromIndex);
    return found == std::string::npos ? -1 : static_cast<int>(found);
  }

  void replace(const char* find, const char* replacement) {
    const std::string needle = find ? find : "";
    if (needle.empty()) return;
    const std::string value = replacement ? replacement : "";
    size_t offset = 0U;
    while ((offset = value_.find(needle, offset)) != std::string::npos) {
      value_.replace(offset, needle.size(), value);
      offset += value.size();
    }
  }

  int lastIndexOf(char needle) const {
    const auto found = value_.rfind(needle);
    return found == std::string::npos ? -1 : static_cast<int>(found);
  }

  int lastIndexOf(char needle, unsigned fromIndex) const {
    if (value_.empty()) return -1;
    const auto found = value_.rfind(
        needle, std::min<size_t>(fromIndex, value_.size() - 1U));
    return found == std::string::npos ? -1 : static_cast<int>(found);
  }

  String substring(unsigned begin) const {
    return begin < value_.size() ? String(value_.substr(begin)) : String();
  }

  String substring(unsigned begin, unsigned end) const {
    return begin < value_.size()
               ? String(value_.substr(begin, end > begin ? end - begin : 0U))
               : String();
  }

  void remove(unsigned index, unsigned count = static_cast<unsigned>(-1)) {
    if (index < value_.size()) value_.erase(index, count);
  }

  String& operator+=(const String& other) {
    value_ += other.value_;
    return *this;
  }
  String& operator+=(const char* other) {
    value_ += other ? other : "";
    return *this;
  }
  String& operator+=(char other) {
    value_ += other;
    return *this;
  }

  friend String operator+(String left, const String& right) {
    left += right;
    return left;
  }
  friend String operator+(String left, const char* right) {
    left += right;
    return left;
  }
  friend String operator+(const char* left, const String& right) {
    String output(left);
    output += right;
    return output;
  }
  friend bool operator==(const String& left, const String& right) {
    return left.value_ == right.value_;
  }
  friend bool operator==(const String& left, const char* right) {
    return left.value_ == (right ? right : "");
  }
  friend bool operator!=(const String& left, const char* right) {
    return !(left == right);
  }

 private:
  void formatFloat(double value, unsigned decimals) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.*f",
                  static_cast<int>(decimals), value);
    value_ = buffer;
  }

  std::string value_{};
};

class HardwareSerial : public Stream {
 public:
  void begin(unsigned long) {}
  int available() const { return kitsu_hal_serial_available(); }
  int read() { return kitsu_hal_serial_read(); }

  template <typename... Args>
  int printf(const char* format, Args... args) {
    char buffer[1024]{};
    const int written = std::snprintf(buffer, sizeof(buffer), format, args...);
    if (written > 0) {
      kitsu_hal_serial_write(
          buffer, std::min<size_t>(static_cast<size_t>(written),
                                   sizeof(buffer) - 1U));
    }
    return written;
  }

  size_t readBytes(uint8_t*, size_t) override { return 0U; }
  size_t write(const uint8_t* value, size_t bytes) override {
    if (value && bytes != 0U) {
      kitsu_hal_serial_write(reinterpret_cast<const char*>(value), bytes);
    }
    return value ? bytes : 0U;
  }

  void print(const char* value) override {
    if (value) kitsu_hal_serial_write(value, std::strlen(value));
  }
  void print(char value) override { kitsu_hal_serial_write(&value, 1U); }
  void print(const String& value) { print(value.c_str()); }
  void print(float value, int decimals) {
    const String text(value, static_cast<unsigned>(decimals));
    print(text);
  }

  template <typename T>
  void print(const T& value) {
    print(String(value));
  }

  void println() override { kitsu_hal_serial_write("\n", 1U); }
  void println(const char* value) {
    print(value);
    println();
  }
  void println(const String& value) {
    print(value);
    println();
  }

  template <typename T>
  void println(const T& value) {
    print(value);
    println();
  }
};

inline HardwareSerial Serial;

struct EspClass {
  uint64_t getEfuseMac() const { return kitsu_hal_device_id(); }
  void restart() const { kitsu_hal_restart_requested(); }
};

inline EspClass ESP;

inline uint32_t millis() { return kitsu_hal_millis(); }
inline uint32_t micros() { return kitsu_hal_micros(); }
inline void delay(uint32_t milliseconds) {
  kitsu_hal_advance_millis(milliseconds);
}
inline void yield() {}
inline void pinMode(uint8_t, int) {}
inline void digitalWrite(uint8_t pin, int value) {
  kitsu_hal_digital_write(pin, value);
}
inline int digitalRead(uint8_t pin) { return kitsu_hal_digital_read(pin); }
inline uint32_t analogReadMilliVolts(uint8_t) { return 3800U; }
inline void analogReadResolution(uint8_t) {}
inline void analogSetPinAttenuation(uint8_t, int) {}
inline uint32_t esp_random() { return kitsu_hal_random(); }
inline void randomSeed(unsigned long) {}
inline long random(long minimum, long maximum) {
  if (maximum <= minimum) return minimum;
  return minimum + static_cast<long>(
                       kitsu_hal_random() %
                       static_cast<uint32_t>(maximum - minimum));
}
