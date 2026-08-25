#include "Arduino.h"
#include "SHA256.h"
#include "esp_partition.h"

#include <sys/time.h>

namespace {

constexpr uint32_t kPortraitWidth = 64U;
constexpr uint32_t kPortraitHeight = 128U;
constexpr uint32_t kFramebufferBytes = kPortraitWidth * kPortraitHeight;
constexpr uint32_t kPackPartitionBytes = 0x140000U;
constexpr uint32_t kSerialCapacity = 64U * 1024U;
constexpr uint32_t kSerialInputCapacity = 8U * 1024U;
constexpr uint32_t kSerialInputStagingCapacity = 4U * 1024U;
constexpr uint32_t kFramebufferHistoryCapacity = 16U;
constexpr uint32_t kEntropyMinimumBytes = 32U;
constexpr uint32_t kEntropyCapacity = 64U;

uint32_t clockMillis = 0U;
uint32_t clockMicrosRemainder = 0U;
bool epochValid = false;
uint32_t epochSecondsAtSync = 0U;
uint32_t epochMillisAtSync = 0U;
uint64_t deviceId = 0x0011223344556677ULL;
uint8_t entropyStaging[kEntropyCapacity]{};
uint8_t drbgKey[32]{};
uint8_t drbgValue[32]{};
bool entropyReady = false;
uint8_t pinValues[64]{};
bool pinValuesInitialized = false;
bool oledOn = true;
bool restartRequested = false;
uint8_t oledDrawing[kFramebufferBytes]{};
uint8_t oledPresented[kFramebufferBytes]{};
uint32_t oledPresentationRevision = 0U;
uint8_t oledHistory[kFramebufferHistoryCapacity][kFramebufferBytes]{};
uint32_t oledHistoryMillis[kFramebufferHistoryCapacity]{};
uint32_t oledHistoryRevision[kFramebufferHistoryCapacity]{};
uint8_t oledHistoryPower[kFramebufferHistoryCapacity]{};
uint32_t oledHistoryRead = 0U;
uint32_t oledHistoryWrite = 0U;
uint32_t oledHistoryCount = 0U;
char serialBuffer[kSerialCapacity]{};
uint32_t serialBytes = 0U;
uint8_t serialInput[kSerialInputCapacity]{};
uint8_t serialInputStaging[kSerialInputStagingCapacity]{};
uint32_t serialInputRead = 0U;
uint32_t serialInputWrite = 0U;
uint32_t serialInputCount = 0U;
uint8_t packBytes[kPackPartitionBytes]{};
bool packInitialized = false;
uint32_t packCommittedBytes = 0U;
esp_partition_t packPartition{};

void initializePins() {
  if (pinValuesInitialized) return;
  std::memset(pinValues, HIGH, sizeof(pinValues));
  pinValuesInitialized = true;
}

void initializePack() {
  if (packInitialized) return;
  std::memset(packBytes, 0xff, sizeof(packBytes));
  packPartition.address = 0x670000U;
  packPartition.size = sizeof(packBytes);
  packPartition.type = ESP_PARTITION_TYPE_DATA;
  packPartition.subtype = ESP_PARTITION_SUBTYPE_DATA_SPIFFS;
  std::strncpy(packPartition.label, "spiffs", sizeof(packPartition.label) - 1U);
  packInitialized = true;
}

void captureOledPresentation() {
  ++oledPresentationRevision;
  if (oledPresentationRevision == 0U) ++oledPresentationRevision;
  std::memcpy(oledHistory[oledHistoryWrite], oledPresented,
              sizeof(oledPresented));
  oledHistoryMillis[oledHistoryWrite] = clockMillis;
  oledHistoryRevision[oledHistoryWrite] = oledPresentationRevision;
  oledHistoryPower[oledHistoryWrite] = oledOn ? 1U : 0U;
  oledHistoryWrite =
      (oledHistoryWrite + 1U) % kFramebufferHistoryCapacity;
  if (oledHistoryCount < kFramebufferHistoryCapacity) {
    ++oledHistoryCount;
  } else {
    oledHistoryRead =
        (oledHistoryRead + 1U) % kFramebufferHistoryCapacity;
  }
}

uint32_t oledHistorySlot(uint32_t index) {
  return (oledHistoryRead + index) % kFramebufferHistoryCapacity;
}

void hmacSha256(const uint8_t key[32], const uint8_t* first,
                size_t firstBytes, const uint8_t* second,
                size_t secondBytes, const uint8_t* third,
                size_t thirdBytes, uint8_t output[32]) {
  SHA256 hash;
  hash.resetHMAC(key, 32U);
  if (firstBytes != 0U) hash.update(first, firstBytes);
  if (secondBytes != 0U) hash.update(second, secondBytes);
  if (thirdBytes != 0U) hash.update(third, thirdBytes);
  hash.finalizeHMAC(key, 32U, output, 32U);
}

void updateDrbg(const uint8_t* provided, size_t providedBytes) {
  const uint8_t separatorZero = 0U;
  uint8_t nextKey[32]{};
  hmacSha256(drbgKey, drbgValue, sizeof(drbgValue), &separatorZero, 1U,
             provided, providedBytes, nextKey);
  std::memcpy(drbgKey, nextKey, sizeof(drbgKey));
  hmacSha256(drbgKey, drbgValue, sizeof(drbgValue), nullptr, 0U, nullptr,
             0U, drbgValue);
  if (providedBytes != 0U) {
    const uint8_t separatorOne = 1U;
    hmacSha256(drbgKey, drbgValue, sizeof(drbgValue), &separatorOne, 1U,
               provided, providedBytes, nextKey);
    std::memcpy(drbgKey, nextKey, sizeof(drbgKey));
    hmacSha256(drbgKey, drbgValue, sizeof(drbgValue), nullptr, 0U, nullptr,
               0U, drbgValue);
  }
  std::memset(nextKey, 0, sizeof(nextKey));
}

bool instantiateDrbg(const uint8_t* entropy, size_t entropyBytes) {
  if (!entropy || entropyBytes < kEntropyMinimumBytes ||
      entropyBytes > kEntropyCapacity) {
    return false;
  }
  std::memset(drbgKey, 0, sizeof(drbgKey));
  std::memset(drbgValue, 1, sizeof(drbgValue));
  updateDrbg(entropy, entropyBytes);
  entropyReady = true;
  return true;
}

bool generateRandom(uint8_t* output, size_t outputBytes) {
  if (!entropyReady || (!output && outputBytes != 0U)) return false;
  size_t written = 0U;
  while (written < outputBytes) {
    hmacSha256(drbgKey, drbgValue, sizeof(drbgValue), nullptr, 0U, nullptr,
               0U, drbgValue);
    const size_t remaining = outputBytes - written;
    const size_t copying = remaining < sizeof(drbgValue)
        ? remaining
        : sizeof(drbgValue);
    std::memcpy(output + written, drbgValue, copying);
    written += copying;
  }
  updateDrbg(nullptr, 0U);
  return true;
}

}  // namespace

extern "C" uint32_t kitsu_hal_millis() { return clockMillis; }

extern "C" uint32_t kitsu_hal_micros() {
  return clockMillis * 1000U + clockMicrosRemainder;
}

extern "C" void kitsu_hal_advance_millis(uint32_t milliseconds) {
  clockMillis += milliseconds;
}

extern "C" int kitsu_hal_digital_read(uint8_t pin) {
  initializePins();
  return pin < sizeof(pinValues) ? pinValues[pin] : HIGH;
}

extern "C" void kitsu_hal_digital_write(uint8_t pin, int value) {
  initializePins();
  if (pin < sizeof(pinValues)) pinValues[pin] = value == LOW ? LOW : HIGH;
}

extern "C" uint32_t kitsu_hal_random_fill(void* output, size_t bytes) {
  return generateRandom(static_cast<uint8_t*>(output), bytes) ? 1U : 0U;
}

extern "C" uint32_t kitsu_hal_random() {
  uint32_t value = 0U;
  (void)generateRandom(reinterpret_cast<uint8_t*>(&value), sizeof(value));
  return value;
}

extern "C" uint64_t kitsu_hal_device_id() {
  return deviceId;
}

extern "C" int kitsu_hal_serial_available() {
  return static_cast<int>(serialInputCount);
}

extern "C" int kitsu_hal_serial_read() {
  if (serialInputCount == 0U) return -1;
  const uint8_t value = serialInput[serialInputRead];
  serialInputRead = (serialInputRead + 1U) % kSerialInputCapacity;
  --serialInputCount;
  return value;
}

extern "C" uint32_t kitsu_hal_epoch_valid() {
  return epochValid ? 1U : 0U;
}

extern "C" uint32_t kitsu_hal_epoch_seconds() {
  if (!epochValid) return 0U;
  return epochSecondsAtSync +
      static_cast<uint32_t>(clockMillis - epochMillisAtSync) / 1000U;
}

extern "C" void kitsu_hal_serial_write(const char* text, size_t bytes) {
  if (!text || bytes == 0U || serialBytes >= sizeof(serialBuffer)) return;
  const size_t available = sizeof(serialBuffer) - serialBytes;
  const size_t copying = bytes < available ? bytes : available;
  std::memcpy(serialBuffer + serialBytes, text, copying);
  serialBytes += static_cast<uint32_t>(copying);
}

extern "C" void kitsu_hal_restart_requested() { restartRequested = true; }

extern "C" void kitsu_hal_oled_clear() {
  std::memset(oledDrawing, 0, sizeof(oledDrawing));
}

extern "C" void kitsu_hal_oled_present() {
  std::memcpy(oledPresented, oledDrawing, sizeof(oledPresented));
  captureOledPresentation();
}

extern "C" void kitsu_hal_oled_power(uint32_t on) {
  const bool next = on != 0U;
  if (next == oledOn) return;
  oledOn = next;
  captureOledPresentation();
}

extern "C" void kitsu_hal_oled_pixel(int16_t physicalX,
                                       int16_t physicalY) {
  // main.cpp rotates portrait coordinates before handing them to the
  // landscape SSD1306 driver: physical=(127-y,x). Undo that here so the web
  // renderer receives the firmware's native 64x128 portrait canvas.
  const int16_t portraitX = physicalY;
  const int16_t portraitY = 127 - physicalX;
  if (portraitX < 0 || portraitX >= static_cast<int16_t>(kPortraitWidth) ||
      portraitY < 0 || portraitY >= static_cast<int16_t>(kPortraitHeight)) {
    return;
  }
  oledDrawing[static_cast<size_t>(portraitY) * kPortraitWidth +
              static_cast<size_t>(portraitX)] = 1U;
}

extern "C" int settimeofday(const timeval* value, const struct timezone*) {
  if (!value || value->tv_sec < 0 ||
      static_cast<uint64_t>(value->tv_sec) > UINT32_MAX ||
      value->tv_usec < 0 || value->tv_usec >= 1000000) {
    return -1;
  }
  epochSecondsAtSync = static_cast<uint32_t>(value->tv_sec);
  epochMillisAtSync = clockMillis;
  epochValid = true;
  return 0;
}

const esp_partition_t* esp_partition_find_first(uint8_t type,
                                                uint8_t subtype,
                                                const char* label) {
  initializePack();
  if (type != ESP_PARTITION_TYPE_DATA ||
      (subtype != ESP_PARTITION_SUBTYPE_ANY &&
       subtype != ESP_PARTITION_SUBTYPE_DATA_SPIFFS) ||
      (label && std::strcmp(label, "spiffs") != 0)) {
    return nullptr;
  }
  return &packPartition;
}

esp_err_t esp_partition_read(const esp_partition_t* partition, size_t offset,
                             void* output, size_t bytes) {
  initializePack();
  if (partition != &packPartition || (!output && bytes != 0U) ||
      offset > sizeof(packBytes) || bytes > sizeof(packBytes) - offset) {
    return -1;
  }
  if (bytes != 0U) std::memcpy(output, packBytes + offset, bytes);
  return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t* partition, size_t offset,
                              const void* input, size_t bytes) {
  initializePack();
  if (partition != &packPartition || (!input && bytes != 0U) ||
      offset > sizeof(packBytes) || bytes > sizeof(packBytes) - offset) {
    return -1;
  }
  if (bytes != 0U) std::memcpy(packBytes + offset, input, bytes);
  return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t* partition,
                                    size_t offset, size_t bytes) {
  initializePack();
  if (partition != &packPartition || offset > sizeof(packBytes) ||
      bytes > sizeof(packBytes) - offset) {
    return -1;
  }
  std::memset(packBytes + offset, 0xff, bytes);
  return ESP_OK;
}

extern "C" uint32_t kitsu_emulator_abi_version() { return 2U; }
extern "C" uint32_t kitsu_emulator_booted();
extern "C" uint32_t kitsu_emulator_set_device_id(uint32_t low,
                                                    uint32_t high) {
  if (kitsu_emulator_booted() != 0U) return 0U;
  deviceId = (static_cast<uint64_t>(high) << 32U) | low;
  return 1U;
}
extern "C" uint8_t* kitsu_emulator_entropy_buffer() {
  return entropyStaging;
}
extern "C" uint32_t kitsu_emulator_entropy_capacity() {
  return sizeof(entropyStaging);
}
extern "C" uint32_t kitsu_emulator_entropy_commit(uint32_t bytes) {
  if (kitsu_emulator_booted() != 0U || entropyReady ||
      !instantiateDrbg(entropyStaging, bytes)) {
    std::memset(entropyStaging, 0, sizeof(entropyStaging));
    return 0U;
  }
  std::memset(entropyStaging, 0, sizeof(entropyStaging));
  return 1U;
}
extern "C" uint32_t kitsu_emulator_entropy_ready() {
  return entropyReady ? 1U : 0U;
}
extern "C" uint32_t kitsu_emulator_clock_millis() { return clockMillis; }
extern "C" void kitsu_emulator_set_clock(uint32_t milliseconds) {
  clockMillis = milliseconds;
}
extern "C" void kitsu_emulator_advance_clock(uint32_t milliseconds) {
  clockMillis += milliseconds;
}
extern "C" void kitsu_emulator_set_prg(uint32_t pressed) {
  initializePins();
  pinValues[0] = pressed != 0U ? LOW : HIGH;
}
extern "C" uint8_t* kitsu_emulator_framebuffer() {
  return oledPresented;
}
extern "C" uint32_t kitsu_emulator_framebuffer_bytes() {
  return sizeof(oledPresented);
}
extern "C" uint32_t kitsu_emulator_framebuffer_width() {
  return kPortraitWidth;
}
extern "C" uint32_t kitsu_emulator_framebuffer_height() {
  return kPortraitHeight;
}
extern "C" uint32_t kitsu_emulator_display_on() { return oledOn ? 1U : 0U; }
extern "C" uint32_t kitsu_emulator_framebuffer_revision() {
  return oledPresentationRevision;
}
extern "C" uint32_t kitsu_emulator_frame_history_capacity() {
  return kFramebufferHistoryCapacity;
}
extern "C" uint32_t kitsu_emulator_frame_history_count() {
  return oledHistoryCount;
}
extern "C" uint8_t* kitsu_emulator_frame_history_frame(uint32_t index) {
  return index < oledHistoryCount ? oledHistory[oledHistorySlot(index)]
                                  : nullptr;
}
extern "C" uint32_t kitsu_emulator_frame_history_millis(uint32_t index) {
  return index < oledHistoryCount
             ? oledHistoryMillis[oledHistorySlot(index)]
             : 0U;
}
extern "C" uint32_t kitsu_emulator_frame_history_revision(uint32_t index) {
  return index < oledHistoryCount
             ? oledHistoryRevision[oledHistorySlot(index)]
             : 0U;
}
extern "C" uint32_t kitsu_emulator_frame_history_display_on(
    uint32_t index) {
  return index < oledHistoryCount
             ? oledHistoryPower[oledHistorySlot(index)]
             : 0U;
}
extern "C" void kitsu_emulator_frame_history_clear() {
  oledHistoryRead = 0U;
  oledHistoryWrite = 0U;
  oledHistoryCount = 0U;
}
extern "C" uint8_t* kitsu_emulator_pack_buffer() {
  initializePack();
  return packBytes;
}
extern "C" uint32_t kitsu_emulator_pack_capacity() {
  return sizeof(packBytes);
}
extern "C" uint32_t kitsu_emulator_pack_commit(uint32_t bytes) {
  initializePack();
  if (bytes > sizeof(packBytes)) return 0U;
  packCommittedBytes = bytes;
  if (bytes < sizeof(packBytes)) {
    std::memset(packBytes + bytes, 0xff, sizeof(packBytes) - bytes);
  }
  return 1U;
}
extern "C" uint32_t kitsu_emulator_pack_bytes() {
  return packCommittedBytes;
}
extern "C" char* kitsu_emulator_serial_buffer() { return serialBuffer; }
extern "C" uint32_t kitsu_emulator_serial_bytes() { return serialBytes; }
extern "C" void kitsu_emulator_serial_clear() { serialBytes = 0U; }
extern "C" uint8_t* kitsu_emulator_serial_input_buffer() {
  return serialInputStaging;
}
extern "C" uint32_t kitsu_emulator_serial_input_capacity() {
  return sizeof(serialInputStaging);
}
extern "C" uint32_t kitsu_emulator_serial_input_commit(uint32_t bytes) {
  if (bytes > sizeof(serialInputStaging) ||
      bytes > sizeof(serialInput) - serialInputCount) {
    return 0U;
  }
  for (uint32_t index = 0U; index < bytes; ++index) {
    serialInput[serialInputWrite] = serialInputStaging[index];
    serialInputWrite = (serialInputWrite + 1U) % kSerialInputCapacity;
  }
  serialInputCount += bytes;
  return 1U;
}
extern "C" uint32_t kitsu_emulator_serial_input_queued() {
  return serialInputCount;
}
extern "C" void kitsu_emulator_serial_input_clear() {
  serialInputRead = 0U;
  serialInputWrite = 0U;
  serialInputCount = 0U;
}
extern "C" uint32_t kitsu_emulator_epoch_valid() {
  return kitsu_hal_epoch_valid();
}
extern "C" uint32_t kitsu_emulator_epoch_seconds() {
  return kitsu_hal_epoch_seconds();
}
extern "C" uint32_t kitsu_emulator_restart_pending() {
  return restartRequested ? 1U : 0U;
}

extern "C" uint32_t kitsu_hal_pack_export(uint8_t* output,
                                             uint32_t capacity) {
  initializePack();
  if ((!output && packCommittedBytes != 0U) ||
      capacity < packCommittedBytes) {
    return UINT32_MAX;
  }
  if (packCommittedBytes != 0U) {
    std::memcpy(output, packBytes, packCommittedBytes);
  }
  return packCommittedBytes;
}

extern "C" uint32_t kitsu_hal_pack_import(const uint8_t* input,
                                             uint32_t bytes) {
  initializePack();
  if ((!input && bytes != 0U) || bytes > sizeof(packBytes)) return 0U;
  if (bytes != 0U) std::memcpy(packBytes, input, bytes);
  if (bytes < sizeof(packBytes)) {
    std::memset(packBytes + bytes, 0xff, sizeof(packBytes) - bytes);
  }
  packCommittedBytes = bytes;
  return 1U;
}

extern "C" void kitsu_hal_pack_clear() {
  initializePack();
  std::memset(packBytes, 0xff, sizeof(packBytes));
  packCommittedBytes = 0U;
}
