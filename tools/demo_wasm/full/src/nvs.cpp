#include "nvs.h"

#include <cstring>

namespace {

constexpr size_t kHandleCapacity = 8U;
constexpr size_t kRecordCapacity = 32U;
constexpr size_t kValueCapacity = 4096U;
constexpr uint32_t kExportMagic = 0x3153564eU;  // "NVS1".

struct Handle {
  bool used = false;
  bool readOnly = true;
  char nameSpace[24]{};
};

struct Record {
  bool used = false;
  char nameSpace[24]{};
  char key[24]{};
  uint32_t bytes = 0U;
  uint8_t value[kValueCapacity]{};
};

Handle handles[kHandleCapacity]{};
Record records[kRecordCapacity]{};

Handle* resolve(nvs_handle_t handle) {
  if (handle == 0U || handle > kHandleCapacity) return nullptr;
  Handle& result = handles[handle - 1U];
  return result.used ? &result : nullptr;
}

Record* find(const char* nameSpace, const char* key) {
  for (Record& record : records) {
    if (record.used && std::strcmp(record.nameSpace, nameSpace) == 0 &&
        std::strcmp(record.key, key) == 0) {
      return &record;
    }
  }
  return nullptr;
}

bool namespaceExists(const char* nameSpace) {
  for (const Record& record : records) {
    if (record.used && std::strcmp(record.nameSpace, nameSpace) == 0) {
      return true;
    }
  }
  return false;
}

Record* allocate(const char* nameSpace, const char* key) {
  if (Record* existing = find(nameSpace, key)) return existing;
  for (Record& record : records) {
    if (record.used) continue;
    record = Record{};
    record.used = true;
    std::strncpy(record.nameSpace, nameSpace,
                 sizeof(record.nameSpace) - 1U);
    std::strncpy(record.key, key, sizeof(record.key) - 1U);
    return &record;
  }
  return nullptr;
}

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

}  // namespace

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode,
                   nvs_handle_t* output) {
  if (!name || !output || name[0] == '\0' ||
      std::strlen(name) >= sizeof(Handle::nameSpace)) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  if (mode == NVS_READONLY && !namespaceExists(name)) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  for (size_t index = 0U; index < kHandleCapacity; ++index) {
    if (handles[index].used) continue;
    handles[index] = Handle{};
    handles[index].used = true;
    handles[index].readOnly = mode == NVS_READONLY;
    std::strncpy(handles[index].nameSpace, name,
                 sizeof(handles[index].nameSpace) - 1U);
    *output = static_cast<nvs_handle_t>(index + 1U);
    return ESP_OK;
  }
  return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
}

void nvs_close(nvs_handle_t handle) {
  if (Handle* resolved = resolve(handle)) *resolved = Handle{};
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* output,
                       size_t* length) {
  Handle* resolved = resolve(handle);
  if (!resolved || !key || !length) return ESP_ERR_NVS_INVALID_HANDLE;
  Record* record = find(resolved->nameSpace, key);
  if (!record) return ESP_ERR_NVS_NOT_FOUND;
  if (!output) {
    *length = record->bytes;
    return ESP_OK;
  }
  if (*length < record->bytes) {
    *length = record->bytes;
    return ESP_ERR_NVS_INVALID_LENGTH;
  }
  if (record->bytes != 0U) {
    std::memcpy(output, record->value, record->bytes);
  }
  *length = record->bytes;
  return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key,
                       const void* input, size_t length) {
  Handle* resolved = resolve(handle);
  if (!resolved || !key || (!input && length != 0U)) {
    return ESP_ERR_NVS_INVALID_HANDLE;
  }
  if (resolved->readOnly) return ESP_ERR_NVS_READ_ONLY;
  if (length > kValueCapacity || std::strlen(key) >= sizeof(Record::key)) {
    return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
  }
  Record* record = allocate(resolved->nameSpace, key);
  if (!record) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
  if (length != 0U) std::memcpy(record->value, input, length);
  record->bytes = static_cast<uint32_t>(length);
  return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key) {
  Handle* resolved = resolve(handle);
  if (!resolved || !key) return ESP_ERR_NVS_INVALID_HANDLE;
  if (resolved->readOnly) return ESP_ERR_NVS_READ_ONLY;
  Record* record = find(resolved->nameSpace, key);
  if (!record) return ESP_ERR_NVS_NOT_FOUND;
  *record = Record{};
  return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle) {
  Handle* resolved = resolve(handle);
  if (!resolved) return ESP_ERR_NVS_INVALID_HANDLE;
  if (resolved->readOnly) return ESP_ERR_NVS_READ_ONLY;
  for (Record& record : records) {
    if (record.used &&
        std::strcmp(record.nameSpace, resolved->nameSpace) == 0) {
      record = Record{};
    }
  }
  return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
  Handle* resolved = resolve(handle);
  if (!resolved) return ESP_ERR_NVS_INVALID_HANDLE;
  return resolved->readOnly ? ESP_ERR_NVS_READ_ONLY : ESP_OK;
}

extern "C" uint32_t kitsu_nvs_export(uint8_t* output, uint32_t capacity) {
  uint32_t count = 0U;
  uint32_t needed = 8U;
  for (const Record& record : records) {
    if (!record.used) continue;
    ++count;
    needed += 4U + sizeof(record.nameSpace) + sizeof(record.key) +
        record.bytes;
  }
  if (!output || capacity < needed) return UINT32_MAX;
  writeU32(output, kExportMagic);
  writeU32(output + 4U, count);
  uint32_t cursor = 8U;
  for (const Record& record : records) {
    if (!record.used) continue;
    writeU32(output + cursor, record.bytes);
    cursor += 4U;
    std::memcpy(output + cursor, record.nameSpace, sizeof(record.nameSpace));
    cursor += sizeof(record.nameSpace);
    std::memcpy(output + cursor, record.key, sizeof(record.key));
    cursor += sizeof(record.key);
    std::memcpy(output + cursor, record.value, record.bytes);
    cursor += record.bytes;
  }
  return cursor;
}

extern "C" uint32_t kitsu_nvs_import(const uint8_t* input, uint32_t bytes) {
  if (!input || bytes < 8U || readU32(input) != kExportMagic) return 0U;
  Record imported[kRecordCapacity]{};
  const uint32_t count = readU32(input + 4U);
  if (count > kRecordCapacity) return 0U;
  uint32_t cursor = 8U;
  for (uint32_t index = 0U; index < count; ++index) {
    constexpr uint32_t metadata =
        4U + sizeof(Record::nameSpace) + sizeof(Record::key);
    if (cursor > bytes || metadata > bytes - cursor) return 0U;
    const uint32_t valueBytes = readU32(input + cursor);
    cursor += 4U;
    if (valueBytes > kValueCapacity ||
        sizeof(Record::nameSpace) + sizeof(Record::key) + valueBytes >
            bytes - cursor) {
      return 0U;
    }
    Record& record = imported[index];
    record.used = true;
    record.bytes = valueBytes;
    std::memcpy(record.nameSpace, input + cursor,
                sizeof(record.nameSpace));
    cursor += sizeof(record.nameSpace);
    std::memcpy(record.key, input + cursor, sizeof(record.key));
    cursor += sizeof(record.key);
    if (record.nameSpace[sizeof(record.nameSpace) - 1U] != '\0' ||
        record.key[sizeof(record.key) - 1U] != '\0') {
      return 0U;
    }
    std::memcpy(record.value, input + cursor, valueBytes);
    cursor += valueBytes;
  }
  if (cursor != bytes) return 0U;
  std::memcpy(records, imported, sizeof(records));
  std::memset(handles, 0, sizeof(handles));
  return 1U;
}

extern "C" void kitsu_nvs_clear_all() {
  std::memset(records, 0, sizeof(records));
  std::memset(handles, 0, sizeof(handles));
}
