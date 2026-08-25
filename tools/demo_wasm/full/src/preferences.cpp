#include "Preferences.h"

namespace {

constexpr size_t kRecordCapacity = 48U;
constexpr size_t kValueCapacity = 4096U;

struct PreferenceRecord {
  bool used = false;
  char nameSpace[24]{};
  char key[24]{};
  size_t bytes = 0U;
  uint8_t value[kValueCapacity]{};
};

PreferenceRecord records[kRecordCapacity]{};

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

bool boundedAdvance(size_t& cursor, size_t adding, size_t limit) {
  if (cursor > limit || adding > limit - cursor) return false;
  cursor += adding;
  return true;
}

PreferenceRecord* findRecord(const char* nameSpace, const char* key) {
  if (!nameSpace || !key) return nullptr;
  for (PreferenceRecord& record : records) {
    if (record.used && std::strcmp(record.nameSpace, nameSpace) == 0 &&
        std::strcmp(record.key, key) == 0) {
      return &record;
    }
  }
  return nullptr;
}

PreferenceRecord* allocateRecord(const char* nameSpace, const char* key) {
  if (PreferenceRecord* existing = findRecord(nameSpace, key)) {
    return existing;
  }
  for (PreferenceRecord& record : records) {
    if (record.used) continue;
    record = PreferenceRecord{};
    record.used = true;
    std::strncpy(record.nameSpace, nameSpace, sizeof(record.nameSpace) - 1U);
    std::strncpy(record.key, key, sizeof(record.key) - 1U);
    return &record;
  }
  return nullptr;
}

template <typename T>
T scalar(const Preferences& preferences, const char* key, T fallback) {
  T value{};
  return preferences.getBytes(key, &value, sizeof(value)) == sizeof(value)
             ? value
             : fallback;
}

}  // namespace

bool Preferences::begin(const char* name, bool readOnly,
                        const char*) {
  end();
  if (!name || name[0] == '\0' ||
      std::strlen(name) >= sizeof(namespace_)) {
    return false;
  }
  std::strncpy(namespace_, name, sizeof(namespace_) - 1U);
  begun_ = true;
  readOnly_ = readOnly;
  return true;
}

void Preferences::end() {
  std::memset(namespace_, 0, sizeof(namespace_));
  begun_ = false;
  readOnly_ = false;
}

bool Preferences::clear() {
  if (!begun_ || readOnly_) return false;
  for (PreferenceRecord& record : records) {
    if (record.used && std::strcmp(record.nameSpace, namespace_) == 0) {
      record = PreferenceRecord{};
    }
  }
  return true;
}

bool Preferences::remove(const char* key) {
  if (!begun_ || readOnly_) return false;
  PreferenceRecord* record = findRecord(namespace_, key);
  if (!record) return false;
  *record = PreferenceRecord{};
  return true;
}

size_t Preferences::getBytesLength(const char* key) const {
  if (!begun_) return 0U;
  const PreferenceRecord* record = findRecord(namespace_, key);
  return record ? record->bytes : 0U;
}

size_t Preferences::getBytes(const char* key, void* output,
                             size_t capacity) const {
  if (!begun_ || !output) return 0U;
  const PreferenceRecord* record = findRecord(namespace_, key);
  if (!record || capacity < record->bytes) return 0U;
  if (record->bytes != 0U) {
    std::memcpy(output, record->value, record->bytes);
  }
  return record->bytes;
}

size_t Preferences::putBytes(const char* key, const void* input,
                             size_t bytes) {
  if (!begun_ || readOnly_ || !key || (!input && bytes != 0U) ||
      std::strlen(key) >= sizeof(PreferenceRecord::key) ||
      bytes > kValueCapacity) {
    return 0U;
  }
  PreferenceRecord* record = allocateRecord(namespace_, key);
  if (!record) return 0U;
  if (bytes != 0U) std::memcpy(record->value, input, bytes);
  record->bytes = bytes;
  return bytes;
}

uint32_t Preferences::getUInt(const char* key, uint32_t fallback) const {
  return scalar(*this, key, fallback);
}

size_t Preferences::putUInt(const char* key, uint32_t value) {
  return putBytes(key, &value, sizeof(value));
}

uint16_t Preferences::getUShort(const char* key, uint16_t fallback) const {
  return scalar(*this, key, fallback);
}

uint8_t Preferences::getUChar(const char* key, uint8_t fallback) const {
  return scalar(*this, key, fallback);
}

bool Preferences::getBool(const char* key, bool fallback) const {
  const uint8_t encoded = scalar<uint8_t>(*this, key, fallback ? 1U : 0U);
  return encoded != 0U;
}

String Preferences::getString(const char* key, const char* fallback) const {
  const size_t bytes = getBytesLength(key);
  if (bytes == 0U || bytes > kValueCapacity) return String(fallback);
  char value[kValueCapacity + 1U]{};
  if (getBytes(key, value, bytes) != bytes) return String(fallback);
  value[bytes] = '\0';
  return String(value);
}

extern "C" uint32_t kitsu_preferences_export(uint8_t* output,
                                                uint32_t capacity) {
  uint32_t count = 0U;
  size_t required = 4U;
  for (const PreferenceRecord& record : records) {
    if (!record.used) continue;
    const size_t namespaceBytes = std::strlen(record.nameSpace);
    const size_t keyBytes = std::strlen(record.key);
    required += 8U + namespaceBytes + keyBytes + record.bytes;
    ++count;
  }
  if (required > UINT32_MAX || !output || capacity < required) {
    return UINT32_MAX;
  }

  writeU32(output, count);
  size_t cursor = 4U;
  for (const PreferenceRecord& record : records) {
    if (!record.used) continue;
    const uint8_t namespaceBytes =
        static_cast<uint8_t>(std::strlen(record.nameSpace));
    const uint8_t keyBytes = static_cast<uint8_t>(std::strlen(record.key));
    output[cursor++] = namespaceBytes;
    output[cursor++] = keyBytes;
    output[cursor++] = 0U;
    output[cursor++] = 0U;
    writeU32(output + cursor, static_cast<uint32_t>(record.bytes));
    cursor += 4U;
    std::memcpy(output + cursor, record.nameSpace, namespaceBytes);
    cursor += namespaceBytes;
    std::memcpy(output + cursor, record.key, keyBytes);
    cursor += keyBytes;
    if (record.bytes != 0U) {
      std::memcpy(output + cursor, record.value, record.bytes);
      cursor += record.bytes;
    }
  }
  return static_cast<uint32_t>(cursor);
}

extern "C" uint32_t kitsu_preferences_import(const uint8_t* input,
                                                uint32_t bytes) {
  if (!input || bytes < 4U) return 0U;
  const uint32_t count = readU32(input);
  if (count > kRecordCapacity) return 0U;

  size_t cursor = 4U;
  for (uint32_t index = 0U; index < count; ++index) {
    if (!boundedAdvance(cursor, 8U, bytes)) return 0U;
    const size_t header = cursor - 8U;
    const uint8_t namespaceBytes = input[header];
    const uint8_t keyBytes = input[header + 1U];
    const uint32_t valueBytes = readU32(input + header + 4U);
    if (namespaceBytes == 0U || namespaceBytes >= sizeof(PreferenceRecord::nameSpace) ||
        keyBytes == 0U || keyBytes >= sizeof(PreferenceRecord::key) ||
        valueBytes > kValueCapacity ||
        !boundedAdvance(cursor,
                        static_cast<size_t>(namespaceBytes) + keyBytes +
                            valueBytes,
                        bytes)) {
      return 0U;
    }

    // Reject duplicate namespace/key pairs before mutating the live store.
    size_t previous = 4U;
    for (uint32_t earlier = 0U; earlier < index; ++earlier) {
      const uint8_t earlierNamespaceBytes = input[previous];
      const uint8_t earlierKeyBytes = input[previous + 1U];
      const uint32_t earlierValueBytes = readU32(input + previous + 4U);
      const size_t earlierNames = previous + 8U;
      const size_t names = header + 8U;
      if (earlierNamespaceBytes == namespaceBytes &&
          earlierKeyBytes == keyBytes &&
          std::memcmp(input + earlierNames, input + names,
                      namespaceBytes) == 0 &&
          std::memcmp(input + earlierNames + earlierNamespaceBytes,
                      input + names + namespaceBytes, keyBytes) == 0) {
        return 0U;
      }
      previous = earlierNames + earlierNamespaceBytes + earlierKeyBytes +
          earlierValueBytes;
    }
  }
  if (cursor != bytes) return 0U;

  std::memset(records, 0, sizeof(records));
  cursor = 4U;
  for (uint32_t index = 0U; index < count; ++index) {
    const uint8_t namespaceBytes = input[cursor];
    const uint8_t keyBytes = input[cursor + 1U];
    const uint32_t valueBytes = readU32(input + cursor + 4U);
    cursor += 8U;
    PreferenceRecord& record = records[index];
    record.used = true;
    std::memcpy(record.nameSpace, input + cursor, namespaceBytes);
    cursor += namespaceBytes;
    std::memcpy(record.key, input + cursor, keyBytes);
    cursor += keyBytes;
    record.bytes = valueBytes;
    if (valueBytes != 0U) {
      std::memcpy(record.value, input + cursor, valueBytes);
      cursor += valueBytes;
    }
  }
  return 1U;
}

extern "C" void kitsu_preferences_clear_all() {
  std::memset(records, 0, sizeof(records));
}
