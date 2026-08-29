#include "wasm_platform_decl.h"

#include "AES.h"
#include "GCM.h"
#include "SHA256.h"
#include "ed25519/ed_25519.h"

namespace kitsu868 {
namespace connectivity {
namespace {

Esp32DeviceSecurityStorage* activeSecurityStorage = nullptr;
Esp32JournalStorage* activeJournalStorage = nullptr;
Esp32KitsuBleOtaPlatform* activeOtaPlatform = nullptr;

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
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

struct BytePart {
  const uint8_t* data;
  size_t bytes;
};

void sha256Parts(const BytePart* parts, size_t partCount,
                 uint8_t output[32]) {
  SHA256 hash;
  for (size_t index = 0U; index < partCount; ++index) {
    if (parts[index].bytes != 0U) {
      hash.update(parts[index].data, parts[index].bytes);
    }
  }
  hash.finalize(output, 32U);
}

void hmacParts(const uint8_t* key, size_t keyBytes, const BytePart* parts,
               size_t partCount, uint8_t output[32]) {
  SHA256 hash;
  hash.resetHMAC(key, keyBytes);
  for (size_t index = 0U; index < partCount; ++index) {
    if (parts[index].bytes != 0U) {
      hash.update(parts[index].data, parts[index].bytes);
    }
  }
  hash.finalizeHMAC(key, keyBytes, output, 32U);
}

bool hkdf(const uint8_t* inputKey, size_t inputKeyBytes,
          const uint8_t* salt, size_t saltBytes, const uint8_t* info,
          size_t infoBytes, uint8_t* output, size_t outputBytes) {
  if ((!inputKey && inputKeyBytes != 0U) || (!salt && saltBytes != 0U) ||
      (!info && infoBytes != 0U) || (!output && outputBytes != 0U) ||
      outputBytes > 255U * 32U) {
    return false;
  }
  uint8_t zeroSalt[32]{};
  const uint8_t* actualSalt = saltBytes == 0U ? zeroSalt : salt;
  const size_t actualSaltBytes = saltBytes == 0U ? sizeof(zeroSalt)
                                                  : saltBytes;
  const BytePart extractParts[] = {{inputKey, inputKeyBytes}};
  uint8_t pseudoRandomKey[32]{};
  hmacParts(actualSalt, actualSaltBytes, extractParts, 1U,
            pseudoRandomKey);

  uint8_t previous[32]{};
  size_t previousBytes = 0U;
  size_t written = 0U;
  uint8_t counter = 1U;
  while (written < outputBytes) {
    const BytePart expandParts[] = {{previous, previousBytes},
                                    {info, infoBytes},
                                    {&counter, 1U}};
    hmacParts(pseudoRandomKey, sizeof(pseudoRandomKey), expandParts, 3U,
              previous);
    previousBytes = sizeof(previous);
    const size_t remaining = outputBytes - written;
    const size_t copying = remaining < sizeof(previous) ? remaining
                                                         : sizeof(previous);
    std::memcpy(output + written, previous, copying);
    written += copying;
    ++counter;
  }
  secureZero(zeroSalt, sizeof(zeroSalt));
  secureZero(pseudoRandomKey, sizeof(pseudoRandomKey));
  secureZero(previous, sizeof(previous));
  return true;
}

void generationAad(uint32_t generation, uint8_t output[4]) {
  output[0] = static_cast<uint8_t>(generation >> 24U);
  output[1] = static_cast<uint8_t>(generation >> 16U);
  output[2] = static_cast<uint8_t>(generation >> 8U);
  output[3] = static_cast<uint8_t>(generation);
}

bool aesGcmSeal(const uint8_t key[kKitsuSecretBytes], uint32_t generation,
                const uint8_t nonce[kSecurityNonceBytes],
                const uint8_t* plaintext, size_t plaintextBytes,
                uint8_t* ciphertext, uint8_t tag[kSecurityTagBytes]) {
  static_assert(kKitsuSecretBytes == 32U,
                "Kitsu local security requires AES-256");
  static_assert(kSecurityNonceBytes == 12U,
                "Kitsu local security requires a 96-bit GCM nonce");
  static_assert(kSecurityTagBytes == 16U,
                "Kitsu local security requires a 128-bit GCM tag");
  if (!key || !nonce || (!plaintext && plaintextBytes != 0U) ||
      (!ciphertext && plaintextBytes != 0U) || !tag) {
    return false;
  }
  uint8_t aad[4]{};
  generationAad(generation, aad);
  GCM<AES256> gcm;
  const bool ready = gcm.setKey(key, kKitsuSecretBytes) &&
      gcm.setIV(nonce, kSecurityNonceBytes);
  if (ready) {
    gcm.addAuthData(aad, sizeof(aad));
    if (plaintextBytes != 0U) {
      gcm.encrypt(ciphertext, plaintext, plaintextBytes);
    }
    gcm.computeTag(tag, kSecurityTagBytes);
  } else {
    std::memset(tag, 0, kSecurityTagBytes);
  }
  gcm.clear();
  secureZero(aad, sizeof(aad));
  return ready;
}

bool aesGcmOpen(const uint8_t key[kKitsuSecretBytes], uint32_t generation,
                const uint8_t nonce[kSecurityNonceBytes],
                const uint8_t* ciphertext, size_t ciphertextBytes,
                const uint8_t tag[kSecurityTagBytes], uint8_t* plaintext) {
  if (!key || !nonce || (!ciphertext && ciphertextBytes != 0U) || !tag ||
      (!plaintext && ciphertextBytes != 0U)) {
    return false;
  }
  uint8_t aad[4]{};
  generationAad(generation, aad);
  GCM<AES256> gcm;
  const bool ready = gcm.setKey(key, kKitsuSecretBytes) &&
      gcm.setIV(nonce, kSecurityNonceBytes);
  bool valid = false;
  if (ready) {
    gcm.addAuthData(aad, sizeof(aad));
    if (ciphertextBytes != 0U) {
      gcm.decrypt(plaintext, ciphertext, ciphertextBytes);
    }
    valid = gcm.checkTag(tag, kSecurityTagBytes);
  }
  gcm.clear();
  secureZero(aad, sizeof(aad));
  if (!valid && plaintext && ciphertextBytes != 0U) {
    std::memset(plaintext, 0, ciphertextBytes);
  }
  return valid;
}

}  // namespace

Esp32DeviceSecurityStorage::Esp32DeviceSecurityStorage() {
  activeSecurityStorage = this;
}

bool Esp32DeviceSecurityStorage::begin(const char*) {
  begun_ = true;
  return true;
}

void Esp32DeviceSecurityStorage::end() { begun_ = false; }

bool Esp32DeviceSecurityStorage::readSlot(uint8_t slot, uint8_t* output,
                                            size_t capacity,
                                            size_t& outputBytes) {
  outputBytes = 0U;
  if (!begun_ || slot >= kSecuritySlots ||
      (!output && sizes_[slot] != 0U) || capacity < sizes_[slot]) {
    return false;
  }
  if (sizes_[slot] != 0U) {
    std::memcpy(output, slots_[slot], sizes_[slot]);
  }
  outputBytes = sizes_[slot];
  return true;
}

bool Esp32DeviceSecurityStorage::writeSlot(uint8_t slot,
                                             const uint8_t* input,
                                             size_t inputBytes) {
  if (!begun_ || slot >= kSecuritySlots ||
      (!input && inputBytes != 0U) ||
      inputBytes > sizeof(slots_[slot])) {
    return false;
  }
  if (inputBytes != 0U) std::memcpy(slots_[slot], input, inputBytes);
  sizes_[slot] = inputBytes;
  return true;
}

bool Esp32DeviceSecurityStorage::clearSlot(uint8_t slot) {
  if (!begun_ || slot >= kSecuritySlots) return false;
  std::memset(slots_[slot], 0, sizeof(slots_[slot]));
  sizes_[slot] = 0U;
  return true;
}

uint32_t Esp32DeviceSecurityStorage::exportPersistent(
    uint8_t* output, uint32_t capacity) const {
  size_t required = 4U;
  for (size_t size : sizes_) required += 4U + size;
  if (!output || capacity < required || required > UINT32_MAX) {
    return UINT32_MAX;
  }
  writeU32(output, kSecuritySlots);
  size_t cursor = 4U;
  for (size_t slot = 0U; slot < kSecuritySlots; ++slot) {
    writeU32(output + cursor, static_cast<uint32_t>(sizes_[slot]));
    cursor += 4U;
    if (sizes_[slot] != 0U) {
      std::memcpy(output + cursor, slots_[slot], sizes_[slot]);
      cursor += sizes_[slot];
    }
  }
  return static_cast<uint32_t>(cursor);
}

bool Esp32DeviceSecurityStorage::importPersistent(const uint8_t* input,
                                                   uint32_t bytes) {
  if (!input || bytes < 4U || readU32(input) != kSecuritySlots) return false;
  size_t cursor = 4U;
  size_t importedSizes[kSecuritySlots]{};
  for (size_t slot = 0U; slot < kSecuritySlots; ++slot) {
    if (cursor > bytes || bytes - cursor < 4U) return false;
    importedSizes[slot] = readU32(input + cursor);
    cursor += 4U;
    if (importedSizes[slot] > sizeof(slots_[slot]) ||
        cursor > bytes || importedSizes[slot] > bytes - cursor) {
      return false;
    }
    cursor += importedSizes[slot];
  }
  if (cursor != bytes) return false;

  cursor = 4U;
  clearPersistent();
  for (size_t slot = 0U; slot < kSecuritySlots; ++slot) {
    cursor += 4U;
    sizes_[slot] = importedSizes[slot];
    if (sizes_[slot] != 0U) {
      std::memcpy(slots_[slot], input + cursor, sizes_[slot]);
      cursor += sizes_[slot];
    }
  }
  return true;
}

void Esp32DeviceSecurityStorage::clearPersistent() {
  std::memset(slots_, 0, sizeof(slots_));
  std::memset(sizes_, 0, sizeof(sizes_));
}

SecurityMode Esp32DeviceSecurityPlatform::securityMode() const {
  return SecurityMode::Reflashable;
}

bool Esp32DeviceSecurityPlatform::deriveWrappingKey(
    const uint8_t hardwareId[8], uint8_t output[kKitsuSecretBytes]) {
  if (!hardwareId || !output) return false;
  uint8_t challenge[32]{};
  static constexpr uint8_t domain[] = "Kitsu868 wrap root v1";
  std::memcpy(challenge, domain, sizeof(domain) - 1U);
  std::memcpy(challenge + sizeof(challenge) - 8U, hardwareId, 8U);
  const BytePart parts[] = {{challenge, sizeof(challenge)}};
  sha256Parts(parts, 1U, output);
  secureZero(challenge, sizeof(challenge));
  return true;
}

bool Esp32DeviceSecurityPlatform::randomBytes(uint8_t* output,
                                               size_t outputBytes) {
  if (!output && outputBytes != 0U) return false;
  return kitsu_hal_random_fill(output, outputBytes) != 0U;
}

bool Esp32DeviceSecurityPlatform::seal(
    const uint8_t key[kKitsuSecretBytes], uint32_t generation,
    const uint8_t nonce[kSecurityNonceBytes], const uint8_t* plaintext,
    size_t plaintextBytes, uint8_t* ciphertext,
    uint8_t tag[kSecurityTagBytes]) {
  if (!key || !nonce || (!plaintext && plaintextBytes != 0U) ||
      (!ciphertext && plaintextBytes != 0U) || !tag) {
    return false;
  }
  return aesGcmSeal(key, generation, nonce, plaintext, plaintextBytes,
                    ciphertext, tag);
}

bool Esp32DeviceSecurityPlatform::open(
    const uint8_t key[kKitsuSecretBytes], uint32_t generation,
    const uint8_t nonce[kSecurityNonceBytes], const uint8_t* ciphertext,
    size_t ciphertextBytes, const uint8_t tag[kSecurityTagBytes],
    uint8_t* plaintext) {
  if (!key || !nonce || (!ciphertext && ciphertextBytes != 0U) || !tag ||
      (!plaintext && ciphertextBytes != 0U)) {
    return false;
  }
  return aesGcmOpen(key, generation, nonce, ciphertext, ciphertextBytes, tag,
                    plaintext);
}

bool Esp32DeviceSecurityPlatform::hkdfSha256(
    const uint8_t* inputKey, size_t inputKeyBytes, const uint8_t* salt,
    size_t saltBytes, const uint8_t* info, size_t infoBytes,
    uint8_t* output, size_t outputBytes) {
  return hkdf(inputKey, inputKeyBytes, salt, saltBytes, info, infoBytes,
              output, outputBytes);
}

bool Esp32LegacyConnectivityRetirementPlatform::inspectPartition(
    LegacyConnectivityPartition& output) {
  std::strncpy(output.label, kLegacyConnectivityPartitionLabel,
               sizeof(output.label) - 1U);
  output.type = kLegacyConnectivityPartitionType;
  output.subtype = kLegacyConnectivityPartitionSubtype;
  output.address = kLegacyConnectivityPartitionAddress;
  output.size = kLegacyConnectivityPartitionBytes;
  return true;
}

bool Esp32LegacyConnectivityRetirementPlatform::readPartition(
    size_t, uint8_t* output, size_t outputBytes) {
  if (!output && outputBytes != 0U) return false;
  std::memset(output, 0xff, outputBytes);
  return true;
}

bool Esp32LegacyConnectivityRetirementPlatform::eraseEntirePartition() {
  return true;
}

bool Esp32LegacyConnectivityRetirementPlatform::
    eraseAfterReplacementPrepared() {
  return true;
}

bool Esp32LegacyConnectivityRetirementPlatform::
    eraseAfterReplacementTransaction() {
  return true;
}

bool Esp32LegacyConnectivityRetirementPlatform::clearLegacyReplayNamespace(
    bool& changed) {
  changed = false;
  return true;
}

Esp32KitsuBleOtaPlatform::Esp32KitsuBleOtaPlatform() {
  activeOtaPlatform = this;
}

void Esp32KitsuBleOtaPlatform::initialize() {
  if (initialized_) return;
  std::memset(app0_, 0xff, sizeof(app0_));
  std::memset(app1_, 0xff, sizeof(app1_));
  // A normal ESP image begins with 0xe9. This keeps the running-side virtual
  // partition distinguishable from erased flash for status checks.
  app0_[0] = 0xe9U;
  initialized_ = true;
}

bool Esp32KitsuBleOtaPlatform::resolvePartitions(
    BleOtaPartition& running, BleOtaPartition& inactive) {
  initialize();
  BleOtaPartition app0{};
  std::strncpy(app0.label, "app0", sizeof(app0.label) - 1U);
  app0.address = kKitsuApp0Offset;
  app0.size = kBleOtaAppPartitionBytes;
  app0.subtype = 0x10U;
  BleOtaPartition app1{};
  std::strncpy(app1.label, "app1", sizeof(app1.label) - 1U);
  app1.address = kKitsuApp1Offset;
  app1.size = kBleOtaAppPartitionBytes;
  app1.subtype = 0x11U;
  running = app0Running_ ? app0 : app1;
  inactive = app0Running_ ? app1 : app0;
  return true;
}

uint8_t* Esp32KitsuBleOtaPlatform::bytesFor(
    const BleOtaPartition& partition) {
  if (std::strcmp(partition.label, "app0") == 0) return app0_;
  if (std::strcmp(partition.label, "app1") == 0) return app1_;
  return nullptr;
}

const uint8_t* Esp32KitsuBleOtaPlatform::bytesFor(
    const BleOtaPartition& partition) const {
  if (std::strcmp(partition.label, "app0") == 0) return app0_;
  if (std::strcmp(partition.label, "app1") == 0) return app1_;
  return nullptr;
}

bool Esp32KitsuBleOtaPlatform::readPartition(
    const BleOtaPartition& partition, uint32_t offset, uint8_t* output,
    size_t outputBytes) {
  initialize();
  const uint8_t* bytes = bytesFor(partition);
  if (!bytes || (!output && outputBytes != 0U) ||
      offset > kBleOtaAppPartitionBytes ||
      outputBytes > kBleOtaAppPartitionBytes - offset) {
    return false;
  }
  if (outputBytes != 0U) std::memcpy(output, bytes + offset, outputBytes);
  return true;
}

bool Esp32KitsuBleOtaPlatform::erasePartition(
    const BleOtaPartition& partition, uint32_t offset,
    uint32_t eraseBytes) {
  initialize();
  uint8_t* bytes = bytesFor(partition);
  if (!bytes || offset > kBleOtaAppPartitionBytes ||
      eraseBytes > kBleOtaAppPartitionBytes - offset) {
    return false;
  }
  std::memset(bytes + offset, 0xff, eraseBytes);
  return true;
}

bool Esp32KitsuBleOtaPlatform::writePartition(
    const BleOtaPartition& partition, uint32_t offset,
    const uint8_t* input, size_t inputBytes) {
  initialize();
  uint8_t* bytes = bytesFor(partition);
  if (!bytes || (!input && inputBytes != 0U) ||
      offset > kBleOtaAppPartitionBytes ||
      inputBytes > kBleOtaAppPartitionBytes - offset) {
    return false;
  }
  for (size_t index = 0U; index < inputBytes; ++index) {
    bytes[offset + index] &= input[index];
  }
  return true;
}

bool Esp32KitsuBleOtaPlatform::verifyEspApplication(
    const BleOtaPartition& partition, uint32_t imageBytes) {
  initialize();
  const uint8_t* bytes = bytesFor(partition);
  return bytes && imageBytes != 0U && imageBytes <= kBleOtaMaximumImageBytes &&
         bytes[0] == 0xe9U;
}

bool Esp32KitsuBleOtaPlatform::verifyEd25519(
    const uint8_t publicKey[kBleOtaDigestBytes], const uint8_t* message,
    size_t messageBytes,
    const uint8_t signature[kBleOtaSignatureBytes]) {
  if (!publicKey || (!message && messageBytes != 0U) || !signature) {
    return false;
  }
  return ed25519_verify(signature, message, messageBytes, publicKey) == 1;
}

BleOtaBootState Esp32KitsuBleOtaPlatform::bootState(
    const BleOtaPartition& partition) {
  return (app0Running_ && std::strcmp(partition.label, "app0") == 0) ||
                 (!app0Running_ && std::strcmp(partition.label, "app1") == 0)
             ? BleOtaBootState::Valid
             : BleOtaBootState::Unknown;
}

bool Esp32KitsuBleOtaPlatform::setBootPartition(
    const BleOtaPartition& partition) {
  if (std::strcmp(partition.label, "app0") == 0) {
    app0Running_ = true;
    return true;
  }
  if (std::strcmp(partition.label, "app1") == 0) {
    app0Running_ = false;
    return true;
  }
  return false;
}

bool Esp32KitsuBleOtaPlatform::markRunningValid() { return true; }

bool Esp32KitsuBleOtaPlatform::rollbackRunningAndRestart() {
  app0Running_ = !app0Running_;
  kitsu_hal_restart_requested();
  return true;
}

void Esp32KitsuBleOtaPlatform::restart() { kitsu_hal_restart_requested(); }

uint8_t* Esp32KitsuBleOtaPlatform::persistentPartition(uint32_t slot) {
  initialize();
  if (slot == 0U) return app0_;
  if (slot == 1U) return app1_;
  return nullptr;
}

uint32_t Esp32KitsuBleOtaPlatform::activeBootSlot() const {
  return app0Running_ ? 0U : 1U;
}

bool Esp32KitsuBleOtaPlatform::restoreActiveBootSlot(uint32_t slot) {
  initialize();
  if (slot > 1U) return false;
  app0Running_ = slot == 0U;
  return true;
}

void Esp32KitsuBleOtaPlatform::clearPersistent() {
  initialized_ = false;
  app0Running_ = true;
  initialize();
}

extern "C" uint8_t* kitsu_emulator_ota_partition_buffer(uint32_t slot) {
  return activeOtaPlatform ? activeOtaPlatform->persistentPartition(slot)
                           : nullptr;
}

extern "C" uint32_t kitsu_emulator_ota_partition_bytes() {
  return kBleOtaAppPartitionBytes;
}

extern "C" uint32_t kitsu_emulator_ota_active_boot_slot() {
  return activeOtaPlatform ? activeOtaPlatform->activeBootSlot() : UINT32_MAX;
}

extern "C" uint32_t kitsu_emulator_ota_restore_active_boot_slot(
    uint32_t slot) {
  return activeOtaPlatform && activeOtaPlatform->restoreActiveBootSlot(slot)
             ? 1U
             : 0U;
}

extern "C" void kitsu_emulator_ota_clear() {
  if (activeOtaPlatform) activeOtaPlatform->clearPersistent();
}

Esp32JournalStorage::Esp32JournalStorage() {
  activeJournalStorage = this;
}

bool Esp32JournalStorage::begin(const char*) {
  begun_ = true;
  return true;
}

void Esp32JournalStorage::end() { begun_ = false; }

bool Esp32JournalStorage::readSlot(uint8_t slot, uint8_t* output,
                                     size_t capacity,
                                     size_t& outputBytes) {
  outputBytes = 0U;
  if (!begun_ || slot >= discovery::kDiscoveryJournalSlots ||
      (!output && sizes_[slot] != 0U) || capacity < sizes_[slot]) {
    return false;
  }
  if (sizes_[slot] != 0U) {
    std::memcpy(output, slots_[slot], sizes_[slot]);
  }
  outputBytes = sizes_[slot];
  return true;
}

bool Esp32JournalStorage::writeSlot(uint8_t slot, const uint8_t* input,
                                      size_t inputBytes) {
  if (!begun_ || slot >= discovery::kDiscoveryJournalSlots ||
      (!input && inputBytes != 0U) || inputBytes > sizeof(slots_[slot])) {
    return false;
  }
  if (inputBytes != 0U) std::memcpy(slots_[slot], input, inputBytes);
  sizes_[slot] = inputBytes;
  return true;
}

uint32_t Esp32JournalStorage::exportPersistent(
    uint8_t* output, uint32_t capacity) const {
  size_t required = 4U;
  for (size_t size : sizes_) required += 4U + size;
  if (!output || capacity < required || required > UINT32_MAX) {
    return UINT32_MAX;
  }
  writeU32(output, discovery::kDiscoveryJournalSlots);
  size_t cursor = 4U;
  for (size_t slot = 0U; slot < discovery::kDiscoveryJournalSlots; ++slot) {
    writeU32(output + cursor, static_cast<uint32_t>(sizes_[slot]));
    cursor += 4U;
    if (sizes_[slot] != 0U) {
      std::memcpy(output + cursor, slots_[slot], sizes_[slot]);
      cursor += sizes_[slot];
    }
  }
  return static_cast<uint32_t>(cursor);
}

bool Esp32JournalStorage::importPersistent(const uint8_t* input,
                                            uint32_t bytes) {
  if (!input || bytes < 4U ||
      readU32(input) != discovery::kDiscoveryJournalSlots) {
    return false;
  }
  size_t cursor = 4U;
  size_t importedSizes[discovery::kDiscoveryJournalSlots]{};
  for (size_t slot = 0U; slot < discovery::kDiscoveryJournalSlots; ++slot) {
    if (cursor > bytes || bytes - cursor < 4U) return false;
    importedSizes[slot] = readU32(input + cursor);
    cursor += 4U;
    if (importedSizes[slot] > sizeof(slots_[slot]) ||
        cursor > bytes || importedSizes[slot] > bytes - cursor) {
      return false;
    }
    cursor += importedSizes[slot];
  }
  if (cursor != bytes) return false;

  cursor = 4U;
  clearPersistent();
  for (size_t slot = 0U; slot < discovery::kDiscoveryJournalSlots; ++slot) {
    cursor += 4U;
    sizes_[slot] = importedSizes[slot];
    if (sizes_[slot] != 0U) {
      std::memcpy(slots_[slot], input + cursor, sizes_[slot]);
      cursor += sizes_[slot];
    }
  }
  return true;
}

void Esp32JournalStorage::clearPersistent() {
  std::memset(slots_, 0, sizeof(slots_));
  std::memset(sizes_, 0, sizeof(sizes_));
}

extern "C" uint32_t kitsu_platform_persistence_export(
    uint8_t* output, uint32_t capacity) {
  if (!activeSecurityStorage || !activeJournalStorage || !output ||
      capacity < 12U) {
    return UINT32_MAX;
  }
  writeU32(output, 1U);
  const uint32_t securityBytes = activeSecurityStorage->exportPersistent(
      output + 12U, capacity - 12U);
  if (securityBytes == UINT32_MAX) return UINT32_MAX;
  const uint32_t journalOffset = 12U + securityBytes;
  if (journalOffset > capacity) return UINT32_MAX;
  const uint32_t journalBytes = activeJournalStorage->exportPersistent(
      output + journalOffset, capacity - journalOffset);
  if (journalBytes == UINT32_MAX) return UINT32_MAX;
  writeU32(output + 4U, securityBytes);
  writeU32(output + 8U, journalBytes);
  return journalOffset + journalBytes;
}

extern "C" uint32_t kitsu_platform_persistence_import(
    const uint8_t* input, uint32_t bytes) {
  if (!activeSecurityStorage || !activeJournalStorage || !input ||
      bytes < 12U || readU32(input) != 1U) {
    return 0U;
  }
  const uint32_t securityBytes = readU32(input + 4U);
  const uint32_t journalBytes = readU32(input + 8U);
  if (securityBytes > bytes - 12U ||
      journalBytes != bytes - 12U - securityBytes) {
    return 0U;
  }
  if (!activeSecurityStorage->importPersistent(input + 12U,
                                                securityBytes) ||
      !activeJournalStorage->importPersistent(
          input + 12U + securityBytes, journalBytes)) {
    activeSecurityStorage->clearPersistent();
    activeJournalStorage->clearPersistent();
    return 0U;
  }
  return 1U;
}

extern "C" void kitsu_platform_persistence_clear() {
  if (activeSecurityStorage) activeSecurityStorage->clearPersistent();
  if (activeJournalStorage) activeJournalStorage->clearPersistent();
}

bool Esp32JournalCrypto::setKey(const uint8_t key[kKitsuSecretBytes]) {
  if (!key) return false;
  std::memcpy(key_, key, sizeof(key_));
  keyed_ = true;
  return true;
}

bool Esp32JournalCrypto::randomNonce(
    uint8_t output[discovery::kDiscoveryNonceBytes]) {
  return keyed_ && output &&
      kitsu_hal_random_fill(output, discovery::kDiscoveryNonceBytes) != 0U;
}

bool Esp32JournalCrypto::seal(
    uint32_t generation,
    const uint8_t nonce[discovery::kDiscoveryNonceBytes],
    const uint8_t* plaintext, size_t plaintextBytes, uint8_t* ciphertext,
    uint8_t tag[discovery::kDiscoveryTagBytes]) {
  if (!keyed_) return false;
  return Esp32DeviceSecurityPlatform{}.seal(key_, generation, nonce, plaintext,
                                            plaintextBytes, ciphertext, tag);
}

bool Esp32JournalCrypto::open(
    uint32_t generation,
    const uint8_t nonce[discovery::kDiscoveryNonceBytes],
    const uint8_t* ciphertext, size_t ciphertextBytes,
    const uint8_t tag[discovery::kDiscoveryTagBytes], uint8_t* plaintext) {
  if (!keyed_) return false;
  return Esp32DeviceSecurityPlatform{}.open(
      key_, generation, nonce, ciphertext, ciphertextBytes, tag, plaintext);
}

bool Esp32CompanionCrypto::randomBytes(uint8_t* output,
                                         size_t outputBytes) {
  if (!output && outputBytes != 0U) return false;
  return kitsu_hal_random_fill(output, outputBytes) != 0U;
}

bool Esp32CompanionCrypto::sha256(const companion::CryptoPart* parts,
                                    size_t partCount, uint8_t output[32]) {
  if ((!parts && partCount != 0U) || !output ||
      partCount > companion::kMaximumCryptoParts) {
    return false;
  }
  SHA256 hash;
  for (size_t index = 0U; index < partCount; ++index) {
    if (!parts[index].data && parts[index].bytes != 0U) {
      secureZero(output, 32U);
      return false;
    }
    hash.update(parts[index].data, parts[index].bytes);
  }
  hash.finalize(output, 32U);
  return true;
}

bool Esp32CompanionCrypto::hmacSha256(
    const uint8_t key[companion::kEnvelopeKeyBytes],
    const companion::CryptoPart* parts, size_t partCount,
    uint8_t output[companion::kEnvelopeMacBytes]) {
  if (!key || (!parts && partCount != 0U) || !output ||
      partCount > companion::kMaximumCryptoParts) {
    return false;
  }
  SHA256 hash;
  hash.resetHMAC(key, companion::kEnvelopeKeyBytes);
  for (size_t index = 0U; index < partCount; ++index) {
    if (!parts[index].data && parts[index].bytes != 0U) {
      secureZero(output, companion::kEnvelopeMacBytes);
      return false;
    }
    hash.update(parts[index].data, parts[index].bytes);
  }
  hash.finalizeHMAC(key, companion::kEnvelopeKeyBytes, output,
                    companion::kEnvelopeMacBytes);
  return true;
}

bool Esp32CompanionCrypto::hkdfSha256(
    const uint8_t inputKey[companion::kEnvelopeKeyBytes],
    const uint8_t* salt, size_t saltBytes, const uint8_t* info,
    size_t infoBytes, uint8_t output[companion::kEnvelopeKeyBytes]) {
  return hkdf(inputKey, companion::kEnvelopeKeyBytes, salt, saltBytes, info,
              infoBytes, output, companion::kEnvelopeKeyBytes);
}

}  // namespace connectivity
}  // namespace kitsu868
