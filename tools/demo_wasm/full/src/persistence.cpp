#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" uint32_t kitsu_emulator_booted();
extern "C" uint32_t kitsu_hal_pack_export(uint8_t* output,
                                             uint32_t capacity);
extern "C" uint32_t kitsu_hal_pack_import(const uint8_t* input,
                                             uint32_t bytes);
extern "C" void kitsu_hal_pack_clear();
extern "C" uint32_t kitsu_preferences_export(uint8_t* output,
                                                uint32_t capacity);
extern "C" uint32_t kitsu_preferences_import(const uint8_t* input,
                                                uint32_t bytes);
extern "C" void kitsu_preferences_clear_all();
extern "C" uint32_t kitsu_platform_persistence_export(uint8_t* output,
                                                         uint32_t capacity);
extern "C" uint32_t kitsu_platform_persistence_import(const uint8_t* input,
                                                         uint32_t bytes);
extern "C" void kitsu_platform_persistence_clear();
extern "C" void kitsu_emulator_ota_clear();
extern "C" uint32_t kitsu_nvs_export(uint8_t* output, uint32_t capacity);
extern "C" uint32_t kitsu_nvs_import(const uint8_t* input, uint32_t bytes);
extern "C" void kitsu_nvs_clear_all();

namespace {

constexpr uint32_t kPersistenceMagic = 0x3253504bU;  // "KPS2" little-endian.
constexpr uint32_t kPersistenceVersion = 2U;
constexpr uint32_t kPersistenceHeaderBytes = 36U;
constexpr uint32_t kPersistenceCapacity = 0x1c0000U;

uint8_t persistenceBuffer[kPersistenceCapacity]{};

void writeU32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
  output[2] = static_cast<uint8_t>(value >> 16U);
  output[3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t readU32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) |
      (static_cast<uint32_t>(input[1]) << 8U) |
      (static_cast<uint32_t>(input[2]) << 16U) |
      (static_cast<uint32_t>(input[3]) << 24U);
}

uint32_t crc32(const uint8_t* input, size_t bytes) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0U; index < bytes; ++index) {
    crc ^= input[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      crc = (crc >> 1U) ^
          (0xedb88320U &
           static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

void clearPersistentStores() {
  kitsu_hal_pack_clear();
  kitsu_preferences_clear_all();
  kitsu_platform_persistence_clear();
  kitsu_nvs_clear_all();
  kitsu_emulator_ota_clear();
}

}  // namespace

extern "C" uint8_t* kitsu_emulator_persistence_buffer() {
  return persistenceBuffer;
}

extern "C" uint32_t kitsu_emulator_persistence_capacity() {
  return sizeof(persistenceBuffer);
}

extern "C" uint32_t kitsu_emulator_persistence_schema_version() {
  return kPersistenceVersion;
}

extern "C" uint32_t kitsu_emulator_persistence_export() {
  uint32_t cursor = kPersistenceHeaderBytes;
  const uint32_t packBytes = kitsu_hal_pack_export(
      persistenceBuffer + cursor, sizeof(persistenceBuffer) - cursor);
  if (packBytes == UINT32_MAX) return 0U;
  cursor += packBytes;

  const uint32_t preferenceBytes = kitsu_preferences_export(
      persistenceBuffer + cursor, sizeof(persistenceBuffer) - cursor);
  if (preferenceBytes == UINT32_MAX) return 0U;
  cursor += preferenceBytes;

  const uint32_t platformBytes = kitsu_platform_persistence_export(
      persistenceBuffer + cursor, sizeof(persistenceBuffer) - cursor);
  if (platformBytes == UINT32_MAX) return 0U;
  cursor += platformBytes;

  const uint32_t nvsBytes = kitsu_nvs_export(
      persistenceBuffer + cursor, sizeof(persistenceBuffer) - cursor);
  if (nvsBytes == UINT32_MAX) return 0U;
  cursor += nvsBytes;

  std::memset(persistenceBuffer, 0, kPersistenceHeaderBytes);
  writeU32(persistenceBuffer, kPersistenceMagic);
  writeU32(persistenceBuffer + 4U, kPersistenceVersion);
  writeU32(persistenceBuffer + 8U, cursor);
  writeU32(persistenceBuffer + 12U, packBytes);
  writeU32(persistenceBuffer + 16U, preferenceBytes);
  writeU32(persistenceBuffer + 20U, platformBytes);
  writeU32(persistenceBuffer + 24U, nvsBytes);
  writeU32(persistenceBuffer + 28U,
           crc32(persistenceBuffer + kPersistenceHeaderBytes,
                 cursor - kPersistenceHeaderBytes));
  writeU32(persistenceBuffer + 32U, crc32(persistenceBuffer, 32U));
  return cursor;
}

extern "C" uint32_t kitsu_emulator_persistence_import(uint32_t bytes) {
  if (kitsu_emulator_booted() != 0U || bytes < kPersistenceHeaderBytes ||
      bytes > sizeof(persistenceBuffer) ||
      readU32(persistenceBuffer) != kPersistenceMagic ||
      readU32(persistenceBuffer + 4U) != kPersistenceVersion ||
      readU32(persistenceBuffer + 8U) != bytes ||
      readU32(persistenceBuffer + 32U) !=
          crc32(persistenceBuffer, 32U)) {
    return 0U;
  }

  const uint32_t packBytes = readU32(persistenceBuffer + 12U);
  const uint32_t preferenceBytes = readU32(persistenceBuffer + 16U);
  const uint32_t platformBytes = readU32(persistenceBuffer + 20U);
  const uint32_t nvsBytes = readU32(persistenceBuffer + 24U);
  const uint32_t payloadBytes = bytes - kPersistenceHeaderBytes;
  uint32_t remaining = payloadBytes;
  if (packBytes > remaining) return 0U;
  remaining -= packBytes;
  if (preferenceBytes > remaining) return 0U;
  remaining -= preferenceBytes;
  if (platformBytes > remaining) return 0U;
  remaining -= platformBytes;
  if (nvsBytes != remaining ||
      readU32(persistenceBuffer + 28U) !=
          crc32(persistenceBuffer + kPersistenceHeaderBytes, payloadBytes)) {
    return 0U;
  }

  const uint8_t* pack = persistenceBuffer + kPersistenceHeaderBytes;
  const uint8_t* preferences = pack + packBytes;
  const uint8_t* platform = preferences + preferenceBytes;
  const uint8_t* nvs = platform + platformBytes;
  if (kitsu_hal_pack_import(pack, packBytes) == 0U ||
      kitsu_preferences_import(preferences, preferenceBytes) == 0U ||
      kitsu_platform_persistence_import(platform, platformBytes) == 0U ||
      kitsu_nvs_import(nvs, nvsBytes) == 0U) {
    clearPersistentStores();
    return 0U;
  }
  return 1U;
}

extern "C" uint32_t kitsu_emulator_persistence_clear() {
  if (kitsu_emulator_booted() != 0U) return 0U;
  clearPersistentStores();
  std::memset(persistenceBuffer, 0, sizeof(persistenceBuffer));
  return 1U;
}
