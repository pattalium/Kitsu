#include "kitsu_device_security.h"

#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr uint8_t kOuterMagic[4] = {'K', 'S', 'E', 'C'};
constexpr uint8_t kPlainMagic[4] = {'K', 'M', 'A', 'T'};
constexpr uint16_t kSecurityVersion = 2U;
constexpr size_t kOuterHeaderBytes = 44U;
constexpr size_t kPlainBytes = 320U;
constexpr size_t kPlainCrcOffset = kPlainBytes - 4U;

static_assert(kOuterHeaderBytes + kPlainBytes <= kSecurityBlobCapacity,
              "security blob capacity is too small");

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

void putU16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
  output[2] = static_cast<uint8_t>(value >> 16U);
  output[3] = static_cast<uint8_t>(value >> 24U);
}

void putU64(uint8_t* output, uint64_t value) {
  for (uint8_t i = 0U; i < 8U; ++i) {
    output[i] = static_cast<uint8_t>(value >> (i * 8U));
  }
}

uint16_t getU16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U);
}

uint32_t getU32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) |
         (static_cast<uint32_t>(input[1]) << 8U) |
         (static_cast<uint32_t>(input[2]) << 16U) |
         (static_cast<uint32_t>(input[3]) << 24U);
}

uint64_t getU64(const uint8_t* input) {
  uint64_t value = 0U;
  for (uint8_t i = 0U; i < 8U; ++i) {
    value |= static_cast<uint64_t>(input[i]) << (i * 8U);
  }
  return value;
}

uint32_t crc32(const uint8_t* input, size_t bytes) {
  uint32_t crc = 0xffffffffUL;
  for (size_t i = 0U; i < bytes; ++i) {
    crc ^= input[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(
          -static_cast<int32_t>(crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320UL & mask);
    }
  }
  return ~crc;
}

}  // namespace

const char* securityResultName(SecurityResult result) {
  switch (result) {
    case SecurityResult::Ok: return "ok";
    case SecurityResult::OkReflashable: return "ok_reflashable";
    case SecurityResult::NotBegun: return "not_begun";
    case SecurityResult::InvalidArgument: return "invalid_argument";
    case SecurityResult::SecurityModeUnavailable:
      return "security_mode_unavailable";
    case SecurityResult::WrappingRootUnavailable:
      return "wrapping_root_unavailable";
    case SecurityResult::StorageReadFailed: return "storage_read_failed";
    case SecurityResult::StorageWriteFailed: return "storage_write_failed";
    case SecurityResult::ReadbackFailed: return "readback_failed";
    case SecurityResult::CorruptStorage: return "corrupt_storage";
    case SecurityResult::CryptoFailed: return "crypto_failed";
    case SecurityResult::AuthorizationRequired:
      return "authorization_required";
    case SecurityResult::ControllerNotProvisioned:
      return "controller_not_provisioned";
    case SecurityResult::ControllerTableFull: return "controller_table_full";
    case SecurityResult::SequenceRejected: return "sequence_rejected";
    case SecurityResult::SequenceExhausted: return "sequence_exhausted";
    case SecurityResult::LegacyMaterialRejected:
      return "legacy_material_rejected";
    default: return "unknown";
  }
}

KitsuDeviceSecurity::KitsuDeviceSecurity() { clear(); }

KitsuDeviceSecurity::~KitsuDeviceSecurity() { clear(); }

void KitsuDeviceSecurity::clear() {
  secureZero(&material_, sizeof(material_));
  secureZero(hardwareId_, sizeof(hardwareId_));
  secureZero(wrappingKey_, sizeof(wrappingKey_));
  secureZero(scratch_, sizeof(scratch_));
  secureZero(cryptScratch_, sizeof(cryptScratch_));
  storage_ = nullptr;
  platform_ = nullptr;
  status_ = DeviceSecurityStatus{};
}

SecurityResult KitsuDeviceSecurity::setResult(SecurityResult result) {
  status_.lastResult = result;
  return result;
}

bool KitsuDeviceSecurity::generationAfter(uint32_t candidate,
                                          uint32_t reference) {
  return static_cast<int32_t>(candidate - reference) > 0;
}

bool KitsuDeviceSecurity::validateSlot(uint8_t slot, uint32_t& generation,
                                       size_t& bytes, bool& nonempty) {
  generation = 0U;
  bytes = 0U;
  nonempty = false;
  size_t loaded = 0U;
  if (!storage_->readSlot(slot, scratch_, sizeof(scratch_), loaded)) {
    status_.lastResult = SecurityResult::StorageReadFailed;
    return false;
  }
  bytes = loaded;
  nonempty = loaded != 0U;
  if (!nonempty) return false;
  if (loaded != kOuterHeaderBytes + kPlainBytes ||
      memcmp(scratch_, kOuterMagic, sizeof(kOuterMagic)) != 0 ||
      getU16(scratch_ + 4U) != kSecurityVersion ||
      getU16(scratch_ + 6U) != kOuterHeaderBytes ||
      getU16(scratch_ + 12U) != kPlainBytes ||
      getU16(scratch_ + 14U) != kPlainBytes) {
    status_.lastResult = SecurityResult::CorruptStorage;
    return false;
  }
  generation = getU32(scratch_ + 8U);
  if (!platform_->open(wrappingKey_, generation, scratch_ + 16U,
                       scratch_ + kOuterHeaderBytes, kPlainBytes,
                       scratch_ + 28U, cryptScratch_)) {
    status_.lastResult = SecurityResult::CorruptStorage;
    return false;
  }
  if (memcmp(cryptScratch_, kPlainMagic, sizeof(kPlainMagic)) != 0 ||
      getU16(cryptScratch_ + 4U) != kSecurityVersion ||
      getU16(cryptScratch_ + 6U) != kPlainBytes ||
      crc32(cryptScratch_, kPlainCrcOffset) !=
          getU32(cryptScratch_ + kPlainCrcOffset)) {
    status_.lastResult = SecurityResult::CorruptStorage;
    return false;
  }
  return true;
}

bool KitsuDeviceSecurity::decodeLoaded(size_t bytes) {
  if (bytes != kOuterHeaderBytes + kPlainBytes) return false;
  const uint32_t flags = getU32(cryptScratch_ + 8U);
  size_t cursor = 12U;
  memcpy(material_.deviceId, cryptScratch_ + cursor,
         sizeof(material_.deviceId));
  cursor += sizeof(material_.deviceId);
  memcpy(material_.deviceSecret, cryptScratch_ + cursor,
         sizeof(material_.deviceSecret));
  cursor += sizeof(material_.deviceSecret);
  memcpy(material_.lanAuthKey, cryptScratch_ + cursor,
         sizeof(material_.lanAuthKey));
  cursor += sizeof(material_.lanAuthKey);
  for (size_t i = 0U; i < kKitsuControllerCapacity; ++i) {
    material_.controllers[i].valid = cryptScratch_[cursor] != 0U;
    cursor += 4U;
    memcpy(material_.controllers[i].id, cryptScratch_ + cursor,
           sizeof(material_.controllers[i].id));
    cursor += sizeof(material_.controllers[i].id);
    memcpy(material_.controllers[i].root, cryptScratch_ + cursor,
           sizeof(material_.controllers[i].root));
    cursor += sizeof(material_.controllers[i].root);
  }
  // Version 2 used this bit for the old development profile. That profile was
  // already application-encrypted with the same recoverable derivation, so it
  // is safe to adopt as reflashable owner material. A zero flag is also
  // accepted to let an owner restore/reset a historical image without a
  // hardware-security lockout; the next write normalizes the flag.
  (void)flags;
  material_.reflashableMaterial = true;
  material_.lanRxHighWater = getU64(cryptScratch_ + cursor);
  cursor += 8U;
  material_.lanTxReservedHigh = getU64(cryptScratch_ + cursor);
  cursor += 8U;
  return cursor == kPlainCrcOffset;
}

bool KitsuDeviceSecurity::encode(uint32_t generation, size_t& bytes) {
  memset(cryptScratch_, 0, sizeof(cryptScratch_));
  memcpy(cryptScratch_, kPlainMagic, sizeof(kPlainMagic));
  putU16(cryptScratch_ + 4U, kSecurityVersion);
  putU16(cryptScratch_ + 6U, static_cast<uint16_t>(kPlainBytes));
  uint32_t flags = 0U;
  if (material_.reflashableMaterial) flags |= 0x01U;
  putU32(cryptScratch_ + 8U, flags);
  size_t cursor = 12U;
  memcpy(cryptScratch_ + cursor, material_.deviceId,
         sizeof(material_.deviceId));
  cursor += sizeof(material_.deviceId);
  memcpy(cryptScratch_ + cursor, material_.deviceSecret,
         sizeof(material_.deviceSecret));
  cursor += sizeof(material_.deviceSecret);
  memcpy(cryptScratch_ + cursor, material_.lanAuthKey,
         sizeof(material_.lanAuthKey));
  cursor += sizeof(material_.lanAuthKey);
  for (size_t i = 0U; i < kKitsuControllerCapacity; ++i) {
    cryptScratch_[cursor] = material_.controllers[i].valid ? 1U : 0U;
    cursor += 4U;
    memcpy(cryptScratch_ + cursor, material_.controllers[i].id,
           sizeof(material_.controllers[i].id));
    cursor += sizeof(material_.controllers[i].id);
    memcpy(cryptScratch_ + cursor, material_.controllers[i].root,
           sizeof(material_.controllers[i].root));
    cursor += sizeof(material_.controllers[i].root);
  }
  putU64(cryptScratch_ + cursor, material_.lanRxHighWater);
  cursor += 8U;
  putU64(cryptScratch_ + cursor, material_.lanTxReservedHigh);
  cursor += 8U;
  if (cursor != kPlainCrcOffset) return false;
  putU32(cryptScratch_ + cursor, crc32(cryptScratch_, cursor));

  memset(scratch_, 0, sizeof(scratch_));
  memcpy(scratch_, kOuterMagic, sizeof(kOuterMagic));
  putU16(scratch_ + 4U, kSecurityVersion);
  putU16(scratch_ + 6U, static_cast<uint16_t>(kOuterHeaderBytes));
  putU32(scratch_ + 8U, generation);
  putU16(scratch_ + 12U, static_cast<uint16_t>(kPlainBytes));
  putU16(scratch_ + 14U, static_cast<uint16_t>(kPlainBytes));
  if (!platform_->randomBytes(scratch_ + 16U, kSecurityNonceBytes) ||
      !platform_->seal(wrappingKey_, generation, scratch_ + 16U,
                       cryptScratch_, kPlainBytes,
                       scratch_ + kOuterHeaderBytes, scratch_ + 28U)) {
    return false;
  }
  bytes = kOuterHeaderBytes + kPlainBytes;
  return true;
}

SecurityResult KitsuDeviceSecurity::persist() {
  const uint32_t nextGeneration = status_.generation + 1U;
  size_t bytes = 0U;
  if (!encode(nextGeneration, bytes)) {
    return setResult(SecurityResult::CryptoFailed);
  }
  const uint8_t target = status_.activeSlot < 0
      ? 0U
      : static_cast<uint8_t>(status_.activeSlot ^ 1);
  if (!storage_->writeSlot(target, scratch_, bytes)) {
    return setResult(SecurityResult::StorageWriteFailed);
  }
  uint32_t generation = 0U;
  size_t loaded = 0U;
  bool nonempty = false;
  status_.lastResult = SecurityResult::Ok;
  if (!validateSlot(target, generation, loaded, nonempty) || !nonempty ||
      generation != nextGeneration || loaded != bytes) {
    return setResult(SecurityResult::ReadbackFailed);
  }
  status_.activeSlot = static_cast<int8_t>(target);
  status_.generation = nextGeneration;
  status_.controllerCount = 0U;
  for (size_t i = 0U; i < kKitsuControllerCapacity; ++i) {
    if (material_.controllers[i].valid) ++status_.controllerCount;
  }
  status_.lanRxHighWater = material_.lanRxHighWater;
  status_.lanTxReservedHigh = material_.lanTxReservedHigh;
  return setResult(SecurityResult::Ok);
}

SecurityResult KitsuDeviceSecurity::begin(DeviceSecurityStorage& storage,
                                           DeviceSecurityPlatform& platform,
                                           const uint8_t hardwareId[8]) {
  clear();
  if (!hardwareId) return setResult(SecurityResult::InvalidArgument);
  storage_ = &storage;
  platform_ = &platform;
  memcpy(hardwareId_, hardwareId, sizeof(hardwareId_));
  status_.securityMode = platform.securityMode();
  status_.applicationEncrypted = kApplicationRecordsEncrypted;
  status_.hardwareRootProtected = kHardwareRootProtected;
  if (status_.securityMode != SecurityMode::Reflashable) {
    return setResult(SecurityResult::SecurityModeUnavailable);
  }
  if (!platform.deriveWrappingKey(hardwareId_, wrappingKey_)) {
    return setResult(SecurityResult::WrappingRootUnavailable);
  }

  uint32_t generation[kSecuritySlots]{};
  size_t bytes[kSecuritySlots]{};
  bool valid[kSecuritySlots]{};
  bool nonempty[kSecuritySlots]{};
  bool anyNonempty = false;
  for (uint8_t slot = 0U; slot < kSecuritySlots; ++slot) {
    status_.lastResult = SecurityResult::Ok;
    valid[slot] = validateSlot(slot, generation[slot], bytes[slot],
                               nonempty[slot]);
    anyNonempty = anyNonempty || nonempty[slot];
    if (!valid[slot] &&
        status_.lastResult == SecurityResult::StorageReadFailed) {
      return setResult(SecurityResult::StorageReadFailed);
    }
  }

  int chosen = -1;
  if (valid[0]) chosen = 0;
  if (valid[1] &&
      (chosen < 0 || generationAfter(generation[1], generation[0]))) {
    chosen = 1;
  }

  if (chosen >= 0) {
    uint32_t loadedGeneration = 0U;
    size_t loadedBytes = 0U;
    bool loadedNonempty = false;
    if (!validateSlot(static_cast<uint8_t>(chosen), loadedGeneration,
                      loadedBytes, loadedNonempty) || !loadedNonempty ||
        !decodeLoaded(loadedBytes)) {
      return setResult(SecurityResult::CorruptStorage);
    }
    material_.reflashableMaterial = true;
    status_.activeSlot = static_cast<int8_t>(chosen);
    status_.generation = loadedGeneration;
  } else {
    if (anyNonempty) return setResult(SecurityResult::CorruptStorage);
    if (!platform_->randomBytes(material_.deviceId,
                                sizeof(material_.deviceId)) ||
        !platform_->randomBytes(material_.deviceSecret,
                                sizeof(material_.deviceSecret)) ||
        !platform_->randomBytes(material_.lanAuthKey,
                                sizeof(material_.lanAuthKey))) {
      return setResult(SecurityResult::CryptoFailed);
    }
    material_.reflashableMaterial = true;
    status_.begun = true;
    const SecurityResult persisted = persist();
    if (persisted != SecurityResult::Ok) {
      status_.begun = false;
      return persisted;
    }
  }

  status_.begun = true;
  status_.controllerCount = 0U;
  for (size_t i = 0U; i < kKitsuControllerCapacity; ++i) {
    if (material_.controllers[i].valid) ++status_.controllerCount;
  }
  status_.lanRxHighWater = material_.lanRxHighWater;
  status_.lanTxReservedHigh = material_.lanTxReservedHigh;
  return setResult(SecurityResult::OkReflashable);
}

bool KitsuDeviceSecurity::ready() const { return status_.begun; }

bool KitsuDeviceSecurity::remoteConnectivityAllowed() const {
  return status_.begun && status_.securityMode == SecurityMode::Reflashable &&
         material_.reflashableMaterial;
}

DeviceSecurityStatus KitsuDeviceSecurity::status() const { return status_; }

bool KitsuDeviceSecurity::copyDeviceId(
    uint8_t output[kKitsuDeviceIdBytes]) const {
  if (!status_.begun || !output) return false;
  memcpy(output, material_.deviceId, kKitsuDeviceIdBytes);
  return true;
}

bool KitsuDeviceSecurity::copyLanAuthKey(
    uint8_t output[kKitsuSecretBytes]) const {
  if (!status_.begun || !output) return false;
  memcpy(output, material_.lanAuthKey, kKitsuSecretBytes);
  return true;
}

bool KitsuDeviceSecurity::controllerAt(
    size_t ordinal, uint8_t controllerId[kKitsuControllerIdBytes]) const {
  if (!status_.begun || !controllerId) return false;
  size_t found = 0U;
  for (size_t i = 0U; i < kKitsuControllerCapacity; ++i) {
    if (!material_.controllers[i].valid) continue;
    if (found == ordinal) {
      memcpy(controllerId, material_.controllers[i].id,
             kKitsuControllerIdBytes);
      return true;
    }
    ++found;
  }
  return false;
}

bool KitsuDeviceSecurity::findControllerRoot(
    const uint8_t controllerId[kKitsuControllerIdBytes],
    uint8_t outputRoot[kKitsuSecretBytes]) const {
  if (!status_.begun || !controllerId || !outputRoot) return false;
  memset(outputRoot, 0, kKitsuSecretBytes);
  for (size_t i = 0U; i < kKitsuControllerCapacity; ++i) {
    if (material_.controllers[i].valid &&
        memcmp(material_.controllers[i].id, controllerId,
               kKitsuControllerIdBytes) == 0) {
      memcpy(outputRoot, material_.controllers[i].root,
             kKitsuSecretBytes);
      return true;
    }
  }
  return false;
}

SecurityResult KitsuDeviceSecurity::deriveJournalKey(
    uint8_t output[kKitsuSecretBytes]) {
  static const uint8_t info[] =
      "kitsu868/journal/aes256gcm/v1";
  if (!status_.begun) return setResult(SecurityResult::NotBegun);
  if (!output) return setResult(SecurityResult::InvalidArgument);
  if (!platform_->hkdfSha256(material_.deviceSecret,
                             sizeof(material_.deviceSecret),
                             material_.deviceId, sizeof(material_.deviceId),
                             info, sizeof(info) - 1U, output,
                             kKitsuSecretBytes)) {
    secureZero(output, kKitsuSecretBytes);
    return setResult(SecurityResult::CryptoFailed);
  }
  return setResult(SecurityResult::Ok);
}

SecurityResult KitsuDeviceSecurity::deriveConnectionStoreKey(
    uint8_t output[kKitsuSecretBytes]) {
  static const uint8_t info[] =
      "kitsu868/connectivity-store/aes256gcm/v1";
  if (!status_.begun) return setResult(SecurityResult::NotBegun);
  if (!output) return setResult(SecurityResult::InvalidArgument);
  if (!platform_->hkdfSha256(material_.deviceSecret,
                             sizeof(material_.deviceSecret),
                             material_.deviceId, sizeof(material_.deviceId),
                             info, sizeof(info) - 1U, output,
                             kKitsuSecretBytes)) {
    secureZero(output, kKitsuSecretBytes);
    return setResult(SecurityResult::CryptoFailed);
  }
  return setResult(SecurityResult::Ok);
}

SecurityResult KitsuDeviceSecurity::generatePendingControllerRoot(
    bool secureConnections, bool linkEncrypted, bool bonded,
    bool physicalConfirmed, uint8_t outputRoot[kKitsuSecretBytes]) {
  if (!status_.begun) return setResult(SecurityResult::NotBegun);
  if (!outputRoot) return setResult(SecurityResult::InvalidArgument);
  memset(outputRoot, 0, kKitsuSecretBytes);
  if (!secureConnections || !linkEncrypted || !bonded ||
      !physicalConfirmed) {
    return setResult(SecurityResult::AuthorizationRequired);
  }
  if (!platform_->randomBytes(outputRoot, kKitsuSecretBytes)) {
    secureZero(outputRoot, kKitsuSecretBytes);
    return setResult(SecurityResult::CryptoFailed);
  }
  return setResult(SecurityResult::Ok);
}

SecurityResult KitsuDeviceSecurity::commitControllerAfterPairing(
    const uint8_t controllerId[kKitsuControllerIdBytes],
    const uint8_t pendingRoot[kKitsuSecretBytes], bool secureConnections,
    bool linkEncrypted, bool bonded, bool physicalConfirmed,
    bool pairCommitVerified) {
  if (!status_.begun) return setResult(SecurityResult::NotBegun);
  if (!controllerId || !pendingRoot) {
    return setResult(SecurityResult::InvalidArgument);
  }
  if (!secureConnections || !linkEncrypted || !bonded ||
      !physicalConfirmed || !pairCommitVerified) {
    return setResult(SecurityResult::AuthorizationRequired);
  }
  bool idAllZero = true;
  bool rootAllZero = true;
  for (size_t i = 0U; i < kKitsuControllerIdBytes; ++i) {
    idAllZero = idAllZero && controllerId[i] == 0U;
  }
  for (size_t i = 0U; i < kKitsuSecretBytes; ++i) {
    rootAllZero = rootAllZero && pendingRoot[i] == 0U;
  }
  if (idAllZero || rootAllZero) {
    return setResult(SecurityResult::InvalidArgument);
  }

  int target = -1;
  for (size_t i = 0U; i < kKitsuControllerCapacity; ++i) {
    if (material_.controllers[i].valid &&
        memcmp(material_.controllers[i].id, controllerId,
               kKitsuControllerIdBytes) == 0) {
      target = static_cast<int>(i);
      break;
    }
  }
  if (target < 0) {
    for (size_t i = 0U; i < kKitsuControllerCapacity; ++i) {
      if (!material_.controllers[i].valid) {
        target = static_cast<int>(i);
        break;
      }
    }
  }
  if (target < 0) return setResult(SecurityResult::ControllerTableFull);

  Material::Controller old = material_.controllers[static_cast<size_t>(target)];
  Material::Controller& controller =
      material_.controllers[static_cast<size_t>(target)];
  controller.valid = true;
  memcpy(controller.id, controllerId, sizeof(controller.id));
  memcpy(controller.root, pendingRoot, sizeof(controller.root));
  const SecurityResult persisted = persist();
  if (persisted != SecurityResult::Ok) {
    controller = old;
  }
  secureZero(&old, sizeof(old));
  return persisted;
}

SecurityResult KitsuDeviceSecurity::revokeControllerAfterPhysicalConfirmation(
    const uint8_t controllerId[kKitsuControllerIdBytes],
    bool physicalConfirmed) {
  if (!status_.begun) return setResult(SecurityResult::NotBegun);
  if (!controllerId) return setResult(SecurityResult::InvalidArgument);
  if (!physicalConfirmed) {
    return setResult(SecurityResult::AuthorizationRequired);
  }
  int target = -1;
  for (size_t i = 0U; i < kKitsuControllerCapacity; ++i) {
    if (material_.controllers[i].valid &&
        memcmp(material_.controllers[i].id, controllerId,
               kKitsuControllerIdBytes) == 0) {
      target = static_cast<int>(i);
      break;
    }
  }
  if (target < 0) return setResult(SecurityResult::ControllerNotProvisioned);
  Material::Controller old = material_.controllers[static_cast<size_t>(target)];
  secureZero(&material_.controllers[static_cast<size_t>(target)],
             sizeof(Material::Controller));
  const SecurityResult persisted = persist();
  if (persisted != SecurityResult::Ok) {
    material_.controllers[static_cast<size_t>(target)] = old;
  }
  secureZero(&old, sizeof(old));
  return persisted;
}

SecurityResult KitsuDeviceSecurity::rotateLanKeyAfterPhysicalConfirmation(
    bool secureConnectionsBonded, bool physicalConfirmed,
    uint8_t outputKey[kKitsuSecretBytes]) {
  if (!status_.begun) return setResult(SecurityResult::NotBegun);
  if (!outputKey) return setResult(SecurityResult::InvalidArgument);
  memset(outputKey, 0, kKitsuSecretBytes);
  if (!secureConnectionsBonded || !physicalConfirmed) {
    return setResult(SecurityResult::AuthorizationRequired);
  }
  uint8_t oldKey[kKitsuSecretBytes]{};
  memcpy(oldKey, material_.lanAuthKey, sizeof(oldKey));
  const uint64_t oldRx = material_.lanRxHighWater;
  const uint64_t oldTx = material_.lanTxReservedHigh;
  if (!platform_->randomBytes(material_.lanAuthKey,
                              sizeof(material_.lanAuthKey))) {
    secureZero(oldKey, sizeof(oldKey));
    return setResult(SecurityResult::CryptoFailed);
  }
  material_.lanRxHighWater = 0U;
  material_.lanTxReservedHigh = 0U;
  const SecurityResult persisted = persist();
  if (persisted != SecurityResult::Ok) {
    memcpy(material_.lanAuthKey, oldKey, sizeof(oldKey));
    material_.lanRxHighWater = oldRx;
    material_.lanTxReservedHigh = oldTx;
  } else {
    memcpy(outputKey, material_.lanAuthKey, kKitsuSecretBytes);
  }
  secureZero(oldKey, sizeof(oldKey));
  return persisted;
}

SecurityResult KitsuDeviceSecurity::acceptLanRxSequence(uint64_t sequence) {
  if (!status_.begun) return setResult(SecurityResult::NotBegun);
  uint64_t* highWater = &material_.lanRxHighWater;
  if (*highWater == UINT64_MAX) {
    return setResult(SecurityResult::SequenceExhausted);
  }
  if (sequence != *highWater + 1U) {
    return setResult(SecurityResult::SequenceRejected);
  }
  const uint64_t previous = *highWater;
  *highWater = sequence;
  const SecurityResult persisted = persist();
  if (persisted != SecurityResult::Ok) *highWater = previous;
  return persisted;
}

SecurityResult KitsuDeviceSecurity::reserveLanTxSequenceBlock(
    uint16_t blockSize, uint64_t& firstSequence,
    uint64_t& lastSequence) {
  firstSequence = 0U;
  lastSequence = 0U;
  if (!status_.begun) return setResult(SecurityResult::NotBegun);
  if (blockSize == 0U || blockSize > 1024U) {
    return setResult(SecurityResult::InvalidArgument);
  }
  uint64_t* reservedHigh = &material_.lanTxReservedHigh;
  if (*reservedHigh > UINT64_MAX - blockSize) {
    return setResult(SecurityResult::SequenceExhausted);
  }
  const uint64_t previous = *reservedHigh;
  const uint64_t proposedLast = previous + blockSize;
  *reservedHigh = proposedLast;
  const SecurityResult persisted = persist();
  if (persisted != SecurityResult::Ok) {
    *reservedHigh = previous;
    return persisted;
  }
  firstSequence = previous + 1U;
  lastSequence = proposedLast;
  return persisted;
}

}  // namespace connectivity
}  // namespace kitsu868
