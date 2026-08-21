#include "kitsu_mesh_transport.h"

#include <Arduino.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <esp_system.h>

#include <Identity.h>
#include <Mesh.h>
#include <Packet.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/radiolib/CustomSX1262.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>

#include <string.h>

namespace kitsu868 {
namespace mesh {
namespace {

constexpr uint8_t kLoraCs = 8;
constexpr uint8_t kLoraSck = 9;
constexpr uint8_t kLoraMosi = 10;
constexpr uint8_t kLoraMiso = 11;
constexpr uint8_t kLoraReset = 12;
constexpr uint8_t kLoraBusy = 13;
constexpr uint8_t kLoraDio1 = 14;
constexpr size_t kPacketPoolSize = 10;
constexpr size_t kAdvertQueueSize = 8;
constexpr float kTcxoVoltage = 1.8f;
constexpr float kConservativeAirtimeFactor = 99.0f;  // 1% long-term TX.

constexpr uint32_t kMinimumEpoch = 1704067200UL;  // 2024-01-01 UTC.
constexpr uint32_t kMaximumEpoch = 4102444800UL;  // 2100-01-01 UTC.

constexpr char kIdentityNamespace[] = "kitsu_mcid";
constexpr char kIdentityKey[] = "identity1";
constexpr uint8_t kIdentityMagic[4] = {'K', 'M', 'I', '1'};
constexpr uint16_t kIdentitySchema = 1;
constexpr size_t kIdentityBytes = 96;
constexpr size_t kIdentityRecordBytes = 108;
constexpr size_t kIdentityOffsetData = 8;
constexpr size_t kIdentityOffsetCrc = 104;

uint16_t readLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) |
      static_cast<uint16_t>(bytes[1]) << 8U;
}

uint32_t readLe32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) |
      static_cast<uint32_t>(bytes[1]) << 8U |
      static_cast<uint32_t>(bytes[2]) << 16U |
      static_cast<uint32_t>(bytes[3]) << 24U;
}

void writeLe16(uint8_t* bytes, uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
}

void writeLe32(uint8_t* bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
  bytes[2] = static_cast<uint8_t>(value >> 16U);
  bytes[3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t crc32WithZeroedIdentityCrc(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffUL;
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value =
        index >= kIdentityOffsetCrc && index < kIdentityOffsetCrc + 4U
            ? 0
            : bytes[index];
    crc ^= value;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^
          (0xedb88320UL & (0U - static_cast<uint32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

size_t sanitizeUtf8Name(char* destination, size_t capacity,
                        const uint8_t* source, size_t sourceBytes) {
  if (!destination || capacity == 0) return 0;
  size_t input = 0;
  size_t output = 0;
  while (input < sourceBytes && source[input] != 0) {
    const uint8_t first = source[input];
    size_t sequenceBytes = 1;
    uint32_t codePoint = first;
    bool valid = true;

    if (first < 0x80U) {
      valid = first >= 0x20U && first != 0x7fU;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      sequenceBytes = 2;
    } else if (first >= 0xe0U && first <= 0xefU) {
      sequenceBytes = 3;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      sequenceBytes = 4;
    } else {
      valid = false;
    }

    if (valid && sequenceBytes > 1U) {
      if (input + sequenceBytes > sourceBytes) {
        valid = false;
      } else {
        for (size_t index = 1; index < sequenceBytes; ++index) {
          if ((source[input + index] & 0xc0U) != 0x80U) {
            valid = false;
            break;
          }
        }
      }
      if (valid) {
        if (sequenceBytes == 2U) {
          codePoint = ((first & 0x1fU) << 6U) |
              (source[input + 1] & 0x3fU);
        } else if (sequenceBytes == 3U) {
          codePoint = ((first & 0x0fU) << 12U) |
              ((source[input + 1] & 0x3fU) << 6U) |
              (source[input + 2] & 0x3fU);
          if (codePoint < 0x800U ||
              (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
            valid = false;
          }
        } else {
          codePoint = ((first & 0x07U) << 18U) |
              ((source[input + 1] & 0x3fU) << 12U) |
              ((source[input + 2] & 0x3fU) << 6U) |
              (source[input + 3] & 0x3fU);
          if (codePoint < 0x10000U || codePoint > 0x10ffffU) valid = false;
        }
        // C1 controls are legal Unicode but unsafe in terminal-facing names.
        if (codePoint >= 0x80U && codePoint <= 0x9fU) valid = false;
      }
    }

    if (!valid) {
      if (output + 1U >= capacity) break;
      destination[output++] = '?';
      ++input;
      continue;
    }
    if (output + sequenceBytes >= capacity) break;
    memcpy(destination + output, source + input, sequenceBytes);
    output += sequenceBytes;
    input += sequenceBytes;
  }
  destination[output] = '\0';
  return output;
}

class EspRng final : public ::mesh::RNG {
 public:
  void random(uint8_t* destination, size_t bytes) override {
    esp_fill_random(destination, bytes);
  }
};

class SessionRtc final : public ::mesh::RTCClock {
 public:
  uint32_t getCurrentTime() override {
    if (!valid_) return 0;
    return baseEpoch_ + (millis() - setAtMillis_) / 1000UL;
  }

  void setCurrentTime(uint32_t epoch) override {
    baseEpoch_ = epoch;
    setAtMillis_ = millis();
    valid_ = true;
  }

  bool valid() const { return valid_; }

 private:
  bool valid_ = false;
  uint32_t baseEpoch_ = 0;
  uint32_t setAtMillis_ = 0;
};

class KitsuBoard final : public ::mesh::MainBoard {
 public:
  uint16_t getBattMilliVolts() override { return 0; }
  const char* getManufacturerName() const override { return "Heltec"; }
  void reboot() override { ESP.restart(); }
  uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

// Last-line TX interlock. Dispatcher callbacks may parse incoming MeshCore
// packets while the owner gate is locked, but only an explicitly armed owner
// packet or a rate-limited reply to authenticated direct TXT can key the
// SX1262. API checks and outbound-queue purge remain useful earlier layers;
// this is the radio boundary that none of them can bypass.
class GatedCustomSX1262Wrapper final : public CustomSX1262Wrapper {
 public:
  GatedCustomSX1262Wrapper(CustomSX1262& radio, ::mesh::MainBoard& board,
                           const TxGate& gate, const Settings& settings)
      : CustomSX1262Wrapper(radio, board), gate_(&gate),
        settings_(&settings) {}

  bool startSendRaw(const uint8_t* bytes, int length) override {
    const bool sessionAllowed = gate_->allowsTransmit(*settings_);
    const bool oneShotAllowed = oneShotArmed_ &&
        static_cast<uint32_t>(millis() - oneShotArmedAt_) <=
            kOneShotPermitLifetimeMs;
    if (!sessionAllowed && !oneShotAllowed) {
      if (oneShotArmed_) revokeOneShot();
      return false;
    }
    // Consume before touching the radio. A failed physical start must never
    // leave a reusable authorization behind for a different packet.
    if (!sessionAllowed) revokeOneShot();
    return CustomSX1262Wrapper::startSendRaw(bytes, length);
  }

  bool armOneShot(const Settings& requested, bool explicitUserApproval) {
    revokeOneShot();
    if (!explicitUserApproval || requested.txPolicy != TxPolicy::ExplicitSession ||
        !requested.enabled || validateSettings(requested) != Status::Ok ||
        !settings_->enabled ||
        settings_->txPolicy != TxPolicy::ExplicitSession ||
        !sameRadioProfile(requested.radio, settings_->radio)) {
      return false;
    }
    oneShotArmed_ = true;
    oneShotArmedAt_ = millis();
    return true;
  }

  // A successfully decrypted direct TXT packet is already authenticated to a
  // known MeshCore contact. When the owner has persisted ExplicitSession for
  // the active profile, permit its single standards-required ACK/PATH reply
  // without opening the general session gate. The global limiter prevents a
  // known-but-hostile contact from turning authenticated traffic into an
  // unbounded transmit oracle; Dispatcher's 1% airtime budget remains the
  // stronger long-term regulatory ceiling.
  bool armAuthenticatedReply(const Settings& requested) {
    const bool sessionAllowed = gate_->allowsTransmit(*settings_);
    if (!requested.enabled ||
        requested.txPolicy != TxPolicy::ExplicitSession ||
        validateSettings(requested) != Status::Ok || !settings_->enabled ||
        settings_->txPolicy != TxPolicy::ExplicitSession ||
        !sameRadioProfile(requested.radio, settings_->radio) ||
        (!sessionAllowed && oneShotArmed_) || !takeProtocolReplyToken()) {
      return false;
    }
    if (!sessionAllowed) {
      oneShotArmed_ = true;
      oneShotArmedAt_ = millis();
    }
    return true;
  }

  void revokeOneShot() {
    oneShotArmed_ = false;
    oneShotArmedAt_ = 0U;
  }

  int recvRaw(uint8_t* bytes, int capacity) override {
    const int length = CustomSX1262Wrapper::recvRaw(bytes, capacity);
    if (length <= 0) return length;
    // Dispatcher::tryParsePacket() reads header, optional transport codes and
    // path_len before doing its first bounds check in v1.17.1.  Drop a short
    // physical frame here so those reads can never touch stale stack bytes.
    if (!bytes || length < 2) return 0;
    const uint8_t route = bytes[0] & PH_ROUTE_MASK;
    const bool hasTransportCodes = route == ROUTE_TYPE_TRANSPORT_FLOOD ||
        route == ROUTE_TYPE_TRANSPORT_DIRECT;
    if (hasTransportCodes && length < 6) return 0;
    return length;
  }

 private:
  static constexpr uint32_t kOneShotPermitLifetimeMs = 5000UL;
  static constexpr uint8_t kProtocolReplyBurst = 8U;
  static constexpr uint32_t kProtocolReplyRefillMs = 10000UL;
  const TxGate* gate_;
  const Settings* settings_;
  bool oneShotArmed_ = false;
  uint32_t oneShotArmedAt_ = 0U;
  bool protocolReplyRateStarted_ = false;
  uint8_t protocolReplyTokens_ = kProtocolReplyBurst;
  uint32_t protocolReplyRefilledAt_ = 0U;

  bool takeProtocolReplyToken() {
    const uint32_t now = millis();
    if (!protocolReplyRateStarted_) {
      protocolReplyRateStarted_ = true;
      protocolReplyRefilledAt_ = now;
    } else {
      const uint32_t elapsed = now - protocolReplyRefilledAt_;
      const uint32_t refill = elapsed / kProtocolReplyRefillMs;
      if (refill != 0U) {
        const uint32_t replenished =
            static_cast<uint32_t>(protocolReplyTokens_) + refill;
        protocolReplyTokens_ = static_cast<uint8_t>(
            replenished < kProtocolReplyBurst ? replenished
                                              : kProtocolReplyBurst);
        protocolReplyRefilledAt_ =
            now - (elapsed % kProtocolReplyRefillMs);
      }
    }
    if (protocolReplyTokens_ == 0U) return false;
    --protocolReplyTokens_;
    return true;
  }
};

constexpr uint8_t kUnknownPath = 0xffU;
constexpr size_t kMessageQueueSize = 8;
constexpr size_t kDeliveryQueueSize = 4;
constexpr uint32_t kTextAckDelayMillis = 200UL;
constexpr uint32_t kSendTimeoutBaseMillis = 500UL;
constexpr char kMessagingNamespace[] = "kitsu_msg";
constexpr char kMessagingKey[] = "state1";
constexpr uint8_t kMessagingMagic[4] = {'K', 'M', 'S', '1'};
constexpr uint16_t kMessagingSchema = 1;

const uint8_t kPublicChannelSecret[32] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

struct ContactEntry {
  bool used = false;
  bool pinned = false;
  uint8_t type = 0;
  uint8_t publicKey[PUB_KEY_SIZE]{};
  char name[33]{};
  uint8_t outPathLen = kUnknownPath;
  uint8_t outPath[MAX_PATH_SIZE]{};
  uint32_t lastAdvertTimestamp = 0;
};

struct ChannelEntry {
  bool used = false;
  char name[33]{};
  ::mesh::GroupChannel channel{};
};

#pragma pack(push, 1)
struct PersistedContact {
  uint8_t used;
  uint8_t pinned;
  uint8_t type;
  uint8_t outPathLen;
  uint8_t publicKey[PUB_KEY_SIZE];
  char name[33];
  uint32_t lastAdvertTimestamp;
  uint8_t outPath[MAX_PATH_SIZE];
};

struct PersistedChannel {
  uint8_t used;
  char name[33];
  uint8_t secret[PUB_KEY_SIZE];
};

struct PersistedMessagingState {
  uint8_t magic[4];
  uint16_t schema;
  uint16_t recordBytes;
  uint32_t crc32;
  PersistedContact contacts[kMeshContactCapacity];
  PersistedChannel channels[kMeshChannelCapacity];
};
#pragma pack(pop)

static_assert(sizeof(PersistedMessagingState) == 1920U,
              "messaging NVS schema must remain byte-stable");

uint32_t messagingCrc(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffUL;
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value = index >= 8U && index < 12U ? 0U : bytes[index];
    crc ^= value;
    for (uint8_t bit = 0; bit < 8U; ++bit) {
      crc = (crc >> 1U) ^
          (0xedb88320UL & (0U - static_cast<uint32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

bool validPublicKey(const uint8_t* publicKey) {
  if (!publicKey) return false;
  bool anyNonzero = false;
  bool anyNotFf = false;
  for (size_t index = 0; index < PUB_KEY_SIZE; ++index) {
    anyNonzero = anyNonzero || publicKey[index] != 0U;
    anyNotFf = anyNotFf || publicKey[index] != 0xffU;
  }
  return anyNonzero && anyNotFf;
}

bool validContactType(uint8_t type) {
  return type >= ADV_TYPE_CHAT && type <= ADV_TYPE_SENSOR;
}

bool validChannelSecret(const uint8_t* secret) {
  if (!secret) return false;
  uint8_t combined = 0;
  for (size_t index = 0; index < PUB_KEY_SIZE; ++index) {
    combined |= secret[index];
  }
  return combined != 0U;
}

bool validPathEncoding(uint8_t encodedLength) {
  if (encodedLength == kUnknownPath) return true;
  if (!::mesh::Packet::isValidPathLen(encodedLength)) return false;
  const size_t hashSize = (encodedLength >> 6U) + 1U;
  const size_t hashCount = encodedLength & 63U;
  return hashSize * hashCount <= MAX_PATH_SIZE;
}

bool validStoredName(const char* name) {
  if (!name) return false;
  const size_t length = strnlen(name, 33U);
  return length > 0U && length < 33U && validMeshTextUtf8(name, length);
}

void deriveChannelHash(::mesh::GroupChannel& channel) {
  bool upperHalfZero = true;
  for (size_t index = 16U; index < PUB_KEY_SIZE; ++index) {
    upperHalfZero = upperHalfZero && channel.secret[index] == 0U;
  }
  ::mesh::Utils::sha256(channel.hash, sizeof(channel.hash), channel.secret,
                        upperHalfZero ? 16U : PUB_KEY_SIZE);
}

class MessagingState {
 public:
  bool begin() {
    clearRam();
    Preferences preferences;
    if (!preferences.begin(kMessagingNamespace, false)) return false;
    const size_t storedBytes = preferences.getBytesLength(kMessagingKey);
    if (storedBytes == 0U) {
      preferences.end();
      installPublicChannel();
      storageReady_ = save();
      return storageReady_;
    }
    if (storedBytes != sizeof(PersistedMessagingState)) {
      preferences.end();
      installPublicChannel();
      return false;
    }
    PersistedMessagingState& record = persistedScratch_;
    memset(&record, 0, sizeof(record));
    const size_t bytesRead = preferences.getBytes(kMessagingKey, &record,
                                                  sizeof(record));
    preferences.end();
    if (bytesRead != sizeof(record) ||
        memcmp(record.magic, kMessagingMagic, sizeof(kMessagingMagic)) != 0 ||
        record.schema != kMessagingSchema ||
        record.recordBytes != sizeof(record) ||
        record.crc32 != messagingCrc(
                            reinterpret_cast<const uint8_t*>(&record),
                            sizeof(record)) ||
        !decode(record)) {
      clearRam();
      installPublicChannel();
      return false;
    }
    storageReady_ = true;
    return true;
  }

  bool reset() {
    clearRam();
    installPublicChannel();
    storageReady_ = save();
    return storageReady_;
  }

  bool storageReady() const { return storageReady_; }

  size_t contactCount() const {
    size_t count = 0;
    for (const ContactEntry& entry : contacts_) count += entry.used ? 1U : 0U;
    return count;
  }

  ContactEntry* findContact(const uint8_t* publicKey) {
    for (ContactEntry& entry : contacts_) {
      if (entry.used && memcmp(entry.publicKey, publicKey, PUB_KEY_SIZE) == 0) {
        return &entry;
      }
    }
    return nullptr;
  }

  const ContactEntry* findContact(const uint8_t* publicKey) const {
    for (const ContactEntry& entry : contacts_) {
      if (entry.used && memcmp(entry.publicKey, publicKey, PUB_KEY_SIZE) == 0) {
        return &entry;
      }
    }
    return nullptr;
  }

  ContactEntry* contactAt(size_t ordinal) {
    for (ContactEntry& entry : contacts_) {
      if (!entry.used) continue;
      if (ordinal == 0U) return &entry;
      --ordinal;
    }
    return nullptr;
  }

  const ContactEntry* contactAt(size_t ordinal) const {
    for (const ContactEntry& entry : contacts_) {
      if (!entry.used) continue;
      if (ordinal == 0U) return &entry;
      --ordinal;
    }
    return nullptr;
  }

  TransportStatus upsertContact(const uint8_t* publicKey, const char* name,
                                uint8_t type, bool pinned,
                                uint32_t advertTimestamp = 0U) {
    if (!storageReady_) return TransportStatus::MessagingStorageFailed;
    if (!validPublicKey(publicKey) || !validContactType(type) ||
        !validStoredName(name)) {
      return TransportStatus::InvalidArgument;
    }
    ContactEntry* entry = findContact(publicKey);
    // Explicit provisioning never evicts a discovered contact behind the
    // phone's back. The owner can list/drop one and retry if all slots are in
    // use.
    if (!entry) entry = allocationSlot();
    if (!entry) return TransportStatus::ContactTableFull;
    const ContactEntry previous = *entry;

    const uint8_t savedPathLen = entry->used ? entry->outPathLen : kUnknownPath;
    uint8_t savedPath[MAX_PATH_SIZE]{};
    if (entry->used) memcpy(savedPath, entry->outPath, sizeof(savedPath));
    const bool wasPinned = entry->used && entry->pinned;
    *entry = ContactEntry{};
    entry->used = true;
    entry->pinned = pinned || wasPinned;
    entry->type = type;
    memcpy(entry->publicKey, publicKey, PUB_KEY_SIZE);
    const size_t nameBytes = strnlen(name, 32U);
    memcpy(entry->name, name, nameBytes);
    entry->name[nameBytes] = '\0';
    entry->outPathLen = savedPathLen;
    memcpy(entry->outPath, savedPath, sizeof(savedPath));
    entry->lastAdvertTimestamp = advertTimestamp != 0U
        ? advertTimestamp
        : previous.lastAdvertTimestamp;
    if (!save()) {
      *entry = previous;
      return TransportStatus::MessagingStorageFailed;
    }
    return TransportStatus::Ok;
  }

  TransportStatus stageObservedContact(const uint8_t* publicKey,
                                       const char* name, uint8_t type,
                                       uint32_t timestamp) {
    // Only normal Client adverts become chat contacts automatically.  Other
    // standard advert roles remain visible in the advert queue/map.
    if (!storageReady_ || type != ADV_TYPE_CHAT || !validPublicKey(publicKey) ||
        !validStoredName(name)) {
      return !storageReady_ ? TransportStatus::MessagingStorageFailed
                            : TransportStatus::InvalidArgument;
    }
    ContactEntry* existing = findContact(publicKey);
    if (existing) {
      if (timestamp > existing->lastAdvertTimestamp) {
        existing->lastAdvertTimestamp = timestamp;
      }
      existing->type = type;
      const size_t bytes = strnlen(name, 32U);
      memset(existing->name, 0, sizeof(existing->name));
      memcpy(existing->name, name, bytes);
      return TransportStatus::Ok;
    }
    ContactEntry* entry = allocationSlot();
    if (!entry) return TransportStatus::ContactTableFull;
    *entry = ContactEntry{};
    entry->used = true;
    entry->type = type;
    entry->outPathLen = kUnknownPath;
    entry->lastAdvertTimestamp = timestamp;
    memcpy(entry->publicKey, publicKey, PUB_KEY_SIZE);
    const size_t bytes = strnlen(name, 32U);
    memcpy(entry->name, name, bytes);
    return TransportStatus::Ok;
  }

  void learnAdvert(const uint8_t* publicKey, const char* name, uint8_t type,
                   uint32_t timestamp) {
    (void)stageObservedContact(publicKey, name, type, timestamp);
  }

  TransportStatus removeContact(const uint8_t* publicKey) {
    if (!storageReady_) return TransportStatus::MessagingStorageFailed;
    ContactEntry* entry = findContact(publicKey);
    if (!entry) return TransportStatus::ContactNotFound;
    const ContactEntry previous = *entry;
    *entry = ContactEntry{};
    entry->outPathLen = kUnknownPath;
    if (save()) return TransportStatus::Ok;
    *entry = previous;
    return TransportStatus::MessagingStorageFailed;
  }

  bool updatePath(ContactEntry& entry, const uint8_t* path,
                  uint8_t pathLen) {
    if (!path || !validPathEncoding(pathLen) || pathLen == kUnknownPath) {
      return false;
    }
    const size_t pathBytes = ((pathLen >> 6U) + 1U) * (pathLen & 63U);
    entry.outPathLen = pathLen;
    memset(entry.outPath, 0, sizeof(entry.outPath));
    memcpy(entry.outPath, path, pathBytes);
    // Route learning is message-driven and intentionally RAM-only.  Rebooting
    // falls back to a standards-compatible flood and learns a fresh path,
    // without turning incoming RF into flash writes.
    return true;
  }

  ChannelEntry* channel(uint8_t slot) {
    return slot < kMeshChannelCapacity && channels_[slot].used
        ? &channels_[slot]
        : nullptr;
  }

  const ChannelEntry* channel(uint8_t slot) const {
    return slot < kMeshChannelCapacity && channels_[slot].used
        ? &channels_[slot]
        : nullptr;
  }

  TransportStatus setChannel(uint8_t slot, const char* name,
                             const uint8_t* secret) {
    if (!storageReady_) return TransportStatus::MessagingStorageFailed;
    if (slot == 0U || slot >= kMeshChannelCapacity ||
        !validStoredName(name) || !validChannelSecret(secret)) {
      return TransportStatus::InvalidArgument;
    }
    ChannelEntry next{};
    next.used = true;
    const size_t nameBytes = strnlen(name, 32U);
    memcpy(next.name, name, nameBytes);
    memcpy(next.channel.secret, secret, PUB_KEY_SIZE);
    deriveChannelHash(next.channel);
    const ChannelEntry previous = channels_[slot];
    channels_[slot] = next;
    if (save()) return TransportStatus::Ok;
    channels_[slot] = previous;
    return TransportStatus::MessagingStorageFailed;
  }

  TransportStatus clearChannel(uint8_t slot) {
    if (!storageReady_) return TransportStatus::MessagingStorageFailed;
    if (slot == 0U || slot >= kMeshChannelCapacity) {
      return TransportStatus::InvalidArgument;
    }
    const ChannelEntry previous = channels_[slot];
    channels_[slot] = ChannelEntry{};
    if (save()) return TransportStatus::Ok;
    channels_[slot] = previous;
    return TransportStatus::MessagingStorageFailed;
  }

  int findChannelSlot(const ::mesh::GroupChannel& channel) const {
    for (size_t slot = 0; slot < kMeshChannelCapacity; ++slot) {
      if (channels_[slot].used &&
          memcmp(channels_[slot].channel.secret, channel.secret,
                 PUB_KEY_SIZE) == 0) {
        return static_cast<int>(slot);
      }
    }
    return -1;
  }

  bool enqueueMessage(const ReceivedMessage& event) {
    if (messageCount_ == kMessageQueueSize) {
      messageRead_ = static_cast<uint8_t>(
          (messageRead_ + 1U) % kMessageQueueSize);
      --messageCount_;
      ++droppedMessages_;
    }
    messages_[messageWrite_] = event;
    messageWrite_ = static_cast<uint8_t>(
        (messageWrite_ + 1U) % kMessageQueueSize);
    ++messageCount_;
    return true;
  }

  bool takeMessage(ReceivedMessage& output) {
    if (messageCount_ == 0U) return false;
    output = messages_[messageRead_];
    messageRead_ = static_cast<uint8_t>(
        (messageRead_ + 1U) % kMessageQueueSize);
    --messageCount_;
    return true;
  }

  void beginPending(uint32_t expectedAck, const ContactEntry& recipient,
                    MessageRoute route, uint32_t timeoutMillis) {
    pending_ = true;
    pendingTimerStarted_ = false;
    pendingAck_ = expectedAck;
    pendingRoute_ = route;
    pendingTimeoutMillis_ = timeoutMillis;
    pendingExpiresAt_ = 0;
    memcpy(pendingRecipient_, recipient.publicKey, PUB_KEY_SIZE);
  }

  bool pending() const { return pending_; }

  void markPendingSent() {
    if (!pending_) return;
    pendingTimerStarted_ = true;
    pendingExpiresAt_ = millis() + pendingTimeoutMillis_;
    DeliveryEvent event{};
    event.kind = MessageKind::Direct;
    event.state = DeliveryState::Sent;
    event.route = pendingRoute_;
    event.expectedAck = pendingAck_;
    memcpy(event.recipientPublicKey, pendingRecipient_, PUB_KEY_SIZE);
    enqueueDelivery(event);
  }

  void markPendingTxFailed() {
    if (pending_) completePending(DeliveryState::TxFailed);
  }

  bool acceptAck(const uint8_t* ack, size_t ackBytes) {
    if (!pending_ || !ack || ackBytes < sizeof(pendingAck_) ||
        memcmp(&pendingAck_, ack, sizeof(pendingAck_)) != 0) {
      return false;
    }
    completePending(DeliveryState::Delivered);
    return true;
  }

  void cancelPending(bool sentBecomesUnconfirmed = false) {
    if (!pending_) return;
    // Once logTx() has reported Sent, no state change can retract the RF
    // packet.  Invalidating its ACK tracker is therefore "unconfirmed", not
    // "cancelled".  Work that never reached the radio remains cancellable.
    completePending(sentBecomesUnconfirmed && pendingTimerStarted_
                        ? DeliveryState::TimedOut
                        : DeliveryState::Cancelled);
  }

  void checkTimeout() {
    if (pending_ && pendingTimerStarted_ &&
        static_cast<int32_t>(millis() - pendingExpiresAt_) >= 0) {
      completePending(DeliveryState::TimedOut);
    }
  }

  bool takeDelivery(DeliveryEvent& output) {
    if (deliveryCount_ == 0U) return false;
    output = deliveries_[deliveryRead_];
    deliveryRead_ = static_cast<uint8_t>(
        (deliveryRead_ + 1U) % kDeliveryQueueSize);
    --deliveryCount_;
    return true;
  }

  void enqueueChannelDelivery(DeliveryState state, uint8_t slot) {
    DeliveryEvent event{};
    event.kind = MessageKind::Channel;
    event.state = state;
    event.route = MessageRoute::Flood;
    event.channelSlot = slot;
    enqueueDelivery(event);
  }

  uint32_t droppedMessageCount() const { return droppedMessages_; }
  uint32_t droppedDeliveryCount() const { return droppedDeliveries_; }

 private:
  ContactEntry contacts_[kMeshContactCapacity]{};
  ChannelEntry channels_[kMeshChannelCapacity]{};
  ReceivedMessage messages_[kMessageQueueSize]{};
  DeliveryEvent deliveries_[kDeliveryQueueSize]{};
  uint8_t messageRead_ = 0;
  uint8_t messageWrite_ = 0;
  uint8_t messageCount_ = 0;
  uint8_t deliveryRead_ = 0;
  uint8_t deliveryWrite_ = 0;
  uint8_t deliveryCount_ = 0;
  uint32_t droppedMessages_ = 0;
  uint32_t droppedDeliveries_ = 0;
  bool storageReady_ = false;
  bool pending_ = false;
  bool pendingTimerStarted_ = false;
  uint32_t pendingAck_ = 0;
  uint32_t pendingTimeoutMillis_ = 0;
  uint32_t pendingExpiresAt_ = 0;
  MessageRoute pendingRoute_ = MessageRoute::Flood;
  uint8_t pendingRecipient_[PUB_KEY_SIZE]{};
  // The Arduino loop task has a small stack. Keep the 1,920-byte NVS image in
  // this long-lived heap-owned state object rather than nesting three copies
  // across begin()->save().
  PersistedMessagingState persistedScratch_{};

  void clearRam() {
    memset(contacts_, 0, sizeof(contacts_));
    for (ContactEntry& entry : contacts_) entry.outPathLen = kUnknownPath;
    memset(channels_, 0, sizeof(channels_));
    messageRead_ = messageWrite_ = messageCount_ = 0;
    deliveryRead_ = deliveryWrite_ = deliveryCount_ = 0;
    droppedMessages_ = droppedDeliveries_ = 0;
    pending_ = false;
    pendingTimerStarted_ = false;
    storageReady_ = false;
  }

  void installPublicChannel() {
    ChannelEntry& channel = channels_[0];
    channel.used = true;
    memcpy(channel.name, "Public", 7U);
    memcpy(channel.channel.secret, kPublicChannelSecret,
           sizeof(kPublicChannelSecret));
    deriveChannelHash(channel.channel);
  }

  ContactEntry* allocationSlot() {
    for (ContactEntry& entry : contacts_) {
      if (!entry.used) return &entry;
    }
    return nullptr;
  }

  bool decode(const PersistedMessagingState& record) {
    for (size_t index = 0; index < kMeshContactCapacity; ++index) {
      const PersistedContact& source = record.contacts[index];
      if (source.used > 1U || source.pinned > 1U) return false;
      if (!source.used) continue;
      if (source.pinned == 0U) return false;
      if (!validPublicKey(source.publicKey) ||
          !validContactType(source.type) ||
          !validStoredName(source.name) ||
          !validPathEncoding(source.outPathLen)) {
        return false;
      }
      ContactEntry& destination = contacts_[index];
      destination.used = true;
      destination.pinned = source.pinned != 0U;
      destination.type = source.type;
      // v1 schema reserves path bytes, but Kitsu never restores them: paths
      // are topology hints learned per boot, not durable contact identity.
      destination.outPathLen = kUnknownPath;
      memcpy(destination.publicKey, source.publicKey, PUB_KEY_SIZE);
      memcpy(destination.name, source.name, sizeof(destination.name));
      destination.name[32] = '\0';
      destination.lastAdvertTimestamp = source.lastAdvertTimestamp;
    }
    for (size_t index = 0; index < kMeshChannelCapacity; ++index) {
      const PersistedChannel& source = record.channels[index];
      if (source.used > 1U) return false;
      if (index == 0U &&
          (!source.used || memcmp(source.name, "Public", 7U) != 0 ||
           memcmp(source.secret, kPublicChannelSecret, PUB_KEY_SIZE) != 0)) {
        return false;
      }
      if (!source.used) continue;
      if (!validStoredName(source.name) ||
          !validChannelSecret(source.secret)) return false;
      ChannelEntry& destination = channels_[index];
      destination.used = true;
      memcpy(destination.name, source.name, sizeof(destination.name));
      destination.name[32] = '\0';
      memcpy(destination.channel.secret, source.secret, PUB_KEY_SIZE);
      deriveChannelHash(destination.channel);
    }
    return true;
  }

  bool save() {
    PersistedMessagingState& record = persistedScratch_;
    memset(&record, 0, sizeof(record));
    memcpy(record.magic, kMessagingMagic, sizeof(kMessagingMagic));
    record.schema = kMessagingSchema;
    record.recordBytes = sizeof(record);
    for (size_t index = 0; index < kMeshContactCapacity; ++index) {
      const ContactEntry& source = contacts_[index];
      PersistedContact& destination = record.contacts[index];
      destination.used = source.used && source.pinned ? 1U : 0U;
      if (!destination.used) continue;
      destination.pinned = source.pinned ? 1U : 0U;
      destination.type = source.type;
      destination.outPathLen = kUnknownPath;
      memcpy(destination.publicKey, source.publicKey, PUB_KEY_SIZE);
      memcpy(destination.name, source.name, sizeof(destination.name));
      destination.lastAdvertTimestamp = source.lastAdvertTimestamp;
    }
    for (size_t index = 0; index < kMeshChannelCapacity; ++index) {
      const ChannelEntry& source = channels_[index];
      PersistedChannel& destination = record.channels[index];
      destination.used = source.used ? 1U : 0U;
      if (!source.used) continue;
      memcpy(destination.name, source.name, sizeof(destination.name));
      memcpy(destination.secret, source.channel.secret, PUB_KEY_SIZE);
    }
    record.crc32 = messagingCrc(
        reinterpret_cast<const uint8_t*>(&record), sizeof(record));
    const uint32_t expectedCrc = record.crc32;

    Preferences preferences;
    if (!preferences.begin(kMessagingNamespace, false)) return false;
    const size_t written = preferences.putBytes(kMessagingKey, &record,
                                                sizeof(record));
    memset(&record, 0, sizeof(record));
    const bool ok = written == sizeof(record) &&
        preferences.getBytesLength(kMessagingKey) == sizeof(record) &&
        preferences.getBytes(kMessagingKey, &record, sizeof(record)) ==
            sizeof(record) &&
        memcmp(record.magic, kMessagingMagic, sizeof(kMessagingMagic)) == 0 &&
        record.schema == kMessagingSchema &&
        record.recordBytes == sizeof(record) &&
        record.crc32 == expectedCrc &&
        record.crc32 == messagingCrc(
                            reinterpret_cast<const uint8_t*>(&record),
                            sizeof(record));
    preferences.end();
    storageReady_ = ok;
    return ok;
  }

  void completePending(DeliveryState state) {
    DeliveryEvent event{};
    event.kind = MessageKind::Direct;
    event.state = state;
    event.route = pendingRoute_;
    event.expectedAck = pendingAck_;
    memcpy(event.recipientPublicKey, pendingRecipient_, PUB_KEY_SIZE);
    enqueueDelivery(event);
    pending_ = false;
    pendingTimerStarted_ = false;
    pendingAck_ = 0;
    pendingTimeoutMillis_ = 0;
    pendingExpiresAt_ = 0;
    memset(pendingRecipient_, 0, sizeof(pendingRecipient_));
  }

  void enqueueDelivery(const DeliveryEvent& event) {
    if (deliveryCount_ == kDeliveryQueueSize) {
      deliveryRead_ = static_cast<uint8_t>(
          (deliveryRead_ + 1U) % kDeliveryQueueSize);
      --deliveryCount_;
      ++droppedDeliveries_;
    }
    deliveries_[deliveryWrite_] = event;
    deliveryWrite_ = static_cast<uint8_t>(
        (deliveryWrite_ + 1U) % kDeliveryQueueSize);
    ++deliveryCount_;
  }
};

class AdvertSink {
 public:
  virtual ~AdvertSink() = default;
  virtual void captureAdvert(::mesh::Packet* packet,
                             const ::mesh::Identity& identity,
                             uint32_t timestamp,
                             const uint8_t* appData,
                             size_t appDataBytes) = 0;
};

class KitsuClient final : public ::mesh::Mesh {
 public:
  KitsuClient(GatedCustomSX1262Wrapper& radio,
              ::mesh::MillisecondClock& clock,
              ::mesh::RNG& rng, ::mesh::RTCClock& rtc,
              ::mesh::PacketManager& packets, ::mesh::MeshTables& tables,
              AdvertSink& sink, MessagingState& messaging,
              const TxGate& gate, const Settings& settings)
      : ::mesh::Mesh(radio, clock, rng, rtc, packets, tables), sink_(&sink),
        messaging_(&messaging), gate_(&gate), settings_(&settings),
        driver_(&radio) {}

  TransportStatus sendDirectText(ContactEntry& recipient, uint32_t timestamp,
                                 const char* text, uint8_t attempt,
                                 uint32_t& expectedAck,
                                 MessageRoute& route) {
    if (messaging_->pending()) return TransportStatus::SendBusy;
    uint8_t plaintext[kMeshPlaintextCapacity]{};
    size_t plaintextBytes = 0;
    const TextCodecStatus encoded = encodeDirectTextPayload(
        timestamp, attempt, text, plaintext, sizeof(plaintext),
        plaintextBytes);
    if (encoded == TextCodecStatus::TextTooLong) {
      return TransportStatus::TextTooLong;
    }
    if (encoded != TextCodecStatus::Ok) {
      return TransportStatus::InvalidArgument;
    }

    uint8_t secret[PUB_KEY_SIZE]{};
    self_id.calcSharedSecret(secret, recipient.publicKey);
    ::mesh::Identity destination(recipient.publicKey);
    ::mesh::Packet* packet = createDatagram(PAYLOAD_TYPE_TXT_MSG, destination,
                                            secret, plaintext,
                                            plaintextBytes);
    if (!packet) return TransportStatus::PacketPoolFull;

    const size_t textBytes = strnlen(text, kMeshTextCapacity);
    ::mesh::Utils::sha256(reinterpret_cast<uint8_t*>(&expectedAck), 4,
                          plaintext, 5U + textBytes,
                          self_id.pub_key, PUB_KEY_SIZE);
    const uint32_t airtime = _radio->getEstAirtimeFor(packet->getRawLength());
    uint32_t timeout = 0;
    pendingPacket_ = packet;
    if (recipient.outPathLen == kUnknownPath) {
      route = MessageRoute::Flood;
      timeout = kSendTimeoutBaseMillis + 16UL * airtime;
      sendFlood(packet);
    } else {
      route = MessageRoute::Direct;
      const uint32_t hops = (recipient.outPathLen & 63U) + 1U;
      timeout = kSendTimeoutBaseMillis +
          (airtime * 6UL + 250UL) * hops;
      sendDirect(packet, recipient.outPath, recipient.outPathLen);
    }
    messaging_->beginPending(expectedAck, recipient, route, timeout);
    return TransportStatus::Ok;
  }

  // A session lock revokes packets that have not completed RF transmission,
  // but it must preserve a direct message's receive-only ACK timer after
  // logTx() has reported Sent.  Accepting that ACK cannot transmit anything.
  void cancelQueuedSends() {
    const ::mesh::Packet* inFlight = currentOutboundPacket();
    if (pendingPacket_ && pendingPacket_ != inFlight) {
      pendingPacket_ = nullptr;
      messaging_->cancelPending();
    }
    if (channelPacket_ && channelPacket_ != inFlight) {
      channelPacket_ = nullptr;
      messaging_->enqueueChannelDelivery(DeliveryState::Cancelled,
                                          channelSlot_);
      channelSlot_ = 0xffU;
    }
  }

  // Profile replacement, contact removal and messaging reset invalidate even
  // an already-sent direct message's receive context.  Those explicit state
  // changes cancel pre-RF work and mark already-sent direct work unconfirmed.
  void cancelAllSends() {
    pendingPacket_ = nullptr;
    messaging_->cancelPending(true);
    if (channelPacket_) {
      channelPacket_ = nullptr;
      messaging_->enqueueChannelDelivery(DeliveryState::Cancelled,
                                          channelSlot_);
      channelSlot_ = 0xffU;
    }
  }

  bool trackedSendInProgress() const {
    const ::mesh::Packet* inFlight = currentOutboundPacket();
    return inFlight &&
        (inFlight == pendingPacket_ || inFlight == channelPacket_);
  }

  TransportStatus sendChannelText(const ChannelEntry& channel,
                                  uint8_t channelSlot,
                                  uint32_t timestamp,
                                  const char* senderName,
                                  const char* text) {
    if (channelPacket_) return TransportStatus::SendBusy;
    const size_t textBytes = strnlen(text, kChannelOutboundTextBytes + 1U);
    if (textBytes > kChannelOutboundTextBytes) {
      return TransportStatus::TextTooLong;
    }
    uint8_t plaintext[kMeshPlaintextCapacity]{};
    size_t plaintextBytes = 0;
    const TextCodecStatus encoded = encodeChannelTextPayload(
        timestamp, senderName, text, textBytes, plaintext, sizeof(plaintext),
        plaintextBytes);
    if (encoded == TextCodecStatus::TextTooLong) {
      return TransportStatus::TextTooLong;
    }
    if (encoded != TextCodecStatus::Ok) {
      return TransportStatus::InvalidArgument;
    }
    ::mesh::Packet* packet = createGroupDatagram(
        PAYLOAD_TYPE_GRP_TXT, channel.channel, plaintext, plaintextBytes);
    if (!packet) return TransportStatus::PacketPoolFull;
    channelPacket_ = packet;
    channelSlot_ = channelSlot;
    sendFlood(packet);
    return TransportStatus::Ok;
  }

 protected:
  ::mesh::DispatcherAction onRecvPacket(::mesh::Packet* packet) override {
    // Keep the accepted protocol surface deliberately small.  MeshCore REQ,
    // CLI, signed text, anon, control, raw, trace and multipart packets are not
    // needed for Kitsu chat and never reach general dispatch.
    if (!packet || packet->getPayloadVer() != PAYLOAD_VER_1) {
      return ACTION_RELEASE;
    }
    switch (packet->getPayloadType()) {
      case PAYLOAD_TYPE_ADVERT:
        if (packet->payload_len < PUB_KEY_SIZE + 4U + SIGNATURE_SIZE ||
            packet->payload_len > PUB_KEY_SIZE + 4U + SIGNATURE_SIZE +
                                      MAX_ADVERT_DATA_SIZE) {
          return ACTION_RELEASE;
        }
        break;
      case PAYLOAD_TYPE_TXT_MSG:
      case PAYLOAD_TYPE_PATH:
        if (packet->payload_len < 2U + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE) {
          return ACTION_RELEASE;
        }
        break;
      case PAYLOAD_TYPE_GRP_TXT:
        if (packet->payload_len < 1U + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE) {
          return ACTION_RELEASE;
        }
        break;
      case PAYLOAD_TYPE_ACK:
        if (packet->payload_len < 4U) return ACTION_RELEASE;
        break;
      default:
        return ACTION_RELEASE;
    }
    return ::mesh::Mesh::onRecvPacket(packet);
  }

  float getAirtimeBudgetFactor() const override {
    return kConservativeAirtimeFactor;
  }

  bool allowPacketForward(const ::mesh::Packet*) override {
    return false;  // Kitsu is a Client, never a repeater.
  }

  void logTx(::mesh::Packet* packet, int) override {
    if (packet == pendingPacket_) {
      pendingPacket_ = nullptr;
      messaging_->markPendingSent();
    }
    if (packet == channelPacket_) {
      channelPacket_ = nullptr;
      messaging_->enqueueChannelDelivery(DeliveryState::Sent, channelSlot_);
      channelSlot_ = 0xffU;
    }
  }

  void logTxFail(::mesh::Packet* packet, int) override {
    if (packet == pendingPacket_) {
      pendingPacket_ = nullptr;
      messaging_->markPendingTxFailed();
    }
    if (packet == channelPacket_) {
      channelPacket_ = nullptr;
      messaging_->enqueueChannelDelivery(DeliveryState::TxFailed,
                                          channelSlot_);
      channelSlot_ = 0xffU;
    }
  }

  void onAdvertRecv(::mesh::Packet* packet, const ::mesh::Identity& identity,
                    uint32_t timestamp, const uint8_t* appData,
                    size_t appDataBytes) override {
    sink_->captureAdvert(packet, identity, timestamp, appData, appDataBytes);
  }

  int searchPeersByHash(const uint8_t* hash) override {
    matchingCount_ = 0;
    const size_t count = messaging_->contactCount();
    for (size_t ordinal = 0; ordinal < count && matchingCount_ < 4U;
         ++ordinal) {
      ContactEntry* contact = messaging_->contactAt(ordinal);
      if (contact && contact->publicKey[0] == hash[0]) {
        matching_[matchingCount_++] = contact;
      }
    }
    return matchingCount_;
  }

  void getPeerSharedSecret(uint8_t* destination, int peerIndex) override {
    if (!destination || peerIndex < 0 ||
        peerIndex >= static_cast<int>(matchingCount_)) {
      return;
    }
    self_id.calcSharedSecret(destination, matching_[peerIndex]->publicKey);
  }

  void onPeerDataRecv(::mesh::Packet* packet, uint8_t type, int senderIndex,
                      const uint8_t* secret, uint8_t* data,
                      size_t dataBytes) override {
    if (!packet || !secret || !data || type != PAYLOAD_TYPE_TXT_MSG ||
        senderIndex < 0 || senderIndex >= static_cast<int>(matchingCount_)) {
      return;
    }
    DecodedTextPayload decoded{};
    if (decodeDirectTextPayload(data, dataBytes, decoded) !=
        TextCodecStatus::Ok) {
      return;
    }
    ContactEntry& sender = *matching_[senderIndex];
    ReceivedMessage event{};
    event.kind = MessageKind::Direct;
    event.route = packet->isRouteDirect() ? MessageRoute::Direct
                                           : MessageRoute::Flood;
    event.senderAuthenticated = true;
    event.timestamp = decoded.timestamp;
    memcpy(event.publicKey, sender.publicKey, PUB_KEY_SIZE);
    memcpy(event.senderName, sender.name, sizeof(event.senderName));
    memcpy(event.text, decoded.text, decoded.textBytes + 1U);
    event.hopCount = packet->getPathHashCount();
    event.rssi = _radio->getLastRSSI();
    event.snr = packet->getSNR();
    messaging_->enqueueMessage(event);

    uint8_t ack[6]{};
    ::mesh::Utils::sha256(ack, 4, data, 5U + decoded.textBytes,
                          sender.publicKey, PUB_KEY_SIZE);
    const size_t terminator = 5U + decoded.textBytes;
    if (terminator + 1U < dataBytes) ack[4] = data[terminator + 1U];
    getRNG()->random(&ack[5], 1U);

    if (packet->isRouteFlood()) {
      ::mesh::Identity destination(sender.publicKey);
      ::mesh::Packet* path = createPathReturn(
          destination, secret, packet->path, packet->path_len,
          PAYLOAD_TYPE_ACK, ack, sizeof(ack));
      if (authorizeAuthenticatedReply(path)) {
        sendFlood(path, kTextAckDelayMillis);
      }
    } else {
      sendAckTo(sender, ack, sizeof(ack));
    }
  }

  bool onPeerPathRecv(::mesh::Packet*, int senderIndex, const uint8_t*,
                      uint8_t* path, uint8_t pathLen, uint8_t extraType,
                      uint8_t* extra, uint8_t extraBytes) override {
    if (senderIndex < 0 || senderIndex >= static_cast<int>(matchingCount_) ||
        !path || !validPathEncoding(pathLen) || pathLen == kUnknownPath) {
      return false;
    }
    // createPathReturn pads plaintext to AES blocks.  A no-extra return uses
    // masked type 0x0F plus four random bytes (4..19 bytes after the type); a
    // text delivery return uses ACK plus six bytes (6..21 after the type).
    // Reject every other PATH application and impossible padded remainder.
    const bool noExtra = extraType == 0x0fU &&
        extraBytes >= 4U && extraBytes <= 19U;
    const bool deliveryAck = extraType == PAYLOAD_TYPE_ACK && extra &&
        extraBytes >= 6U && extraBytes <= 21U;
    if (!noExtra && !deliveryAck) return false;
    messaging_->updatePath(*matching_[senderIndex], path, pathLen);
    if (deliveryAck) {
      messaging_->acceptAck(extra, extraBytes);
    }
    return gate_->allowsTransmit(*settings_);
  }

  void onAckRecv(::mesh::Packet* packet, uint32_t ack) override {
    if (messaging_->acceptAck(reinterpret_cast<const uint8_t*>(&ack), 4U) &&
        packet) {
      packet->markDoNotRetransmit();
    }
  }

  int searchChannelsByHash(const uint8_t* hash,
                           ::mesh::GroupChannel output[],
                           int maxMatches) override {
    if (!hash || !output || maxMatches <= 0) return 0;
    int matches = 0;
    for (uint8_t slot = 0; slot < kMeshChannelCapacity && matches < maxMatches;
         ++slot) {
      const ChannelEntry* channel = messaging_->channel(slot);
      if (channel && channel->channel.hash[0] == hash[0]) {
        output[matches++] = channel->channel;
      }
    }
    return matches;
  }

  void onGroupDataRecv(::mesh::Packet* packet, uint8_t type,
                       const ::mesh::GroupChannel& channel, uint8_t* data,
                       size_t dataBytes) override {
    if (!packet || !data || type != PAYLOAD_TYPE_GRP_TXT) return;
    const int slot = messaging_->findChannelSlot(channel);
    if (slot < 0) return;
    DecodedTextPayload decoded{};
    if (decodeChannelTextPayload(data, dataBytes, decoded) !=
        TextCodecStatus::Ok) {
      return;
    }

    ReceivedMessage event{};
    event.kind = MessageKind::Channel;
    event.route = packet->isRouteDirect() ? MessageRoute::Direct
                                           : MessageRoute::Flood;
    event.senderAuthenticated = false;
    event.timestamp = decoded.timestamp;
    event.channelSlot = static_cast<uint8_t>(slot);
    event.hopCount = packet->getPathHashCount();
    event.rssi = _radio->getLastRSSI();
    event.snr = packet->getSNR();

    const char* separator = strstr(decoded.text, ": ");
    if (separator) {
      const size_t nameBytes = static_cast<size_t>(separator - decoded.text);
      sanitizeUtf8Name(event.senderName, sizeof(event.senderName),
                       reinterpret_cast<const uint8_t*>(decoded.text),
                       nameBytes);
      const char* message = separator + 2;
      const size_t messageBytes = strnlen(message, kMeshTextBytes + 1U);
      memcpy(event.text, message, messageBytes);
      event.text[messageBytes] = '\0';
    } else {
      memcpy(event.text, decoded.text, decoded.textBytes + 1U);
    }
    messaging_->enqueueMessage(event);
  }

 private:
  AdvertSink* sink_;
  MessagingState* messaging_;
  const TxGate* gate_;
  const Settings* settings_;
  GatedCustomSX1262Wrapper* driver_;
  ContactEntry* matching_[4]{};
  uint8_t matchingCount_ = 0;
  ::mesh::Packet* pendingPacket_ = nullptr;
  ::mesh::Packet* channelPacket_ = nullptr;
  uint8_t channelSlot_ = 0xffU;

  bool authorizeAuthenticatedReply(::mesh::Packet* packet) {
    if (!packet) return false;
    // Under a locked session, the just-created reply must be the only packet
    // eligible to consume the permit. Arduino dispatch is single-threaded, so
    // this check plus immediate queueing binds the next radio start to it.
    if (!gate_->allowsTransmit(*settings_) &&
        (currentOutboundPacket() || _mgr->getOutboundTotal() != 0)) {
      releasePacket(packet);
      return false;
    }
    if (!driver_->armAuthenticatedReply(*settings_)) {
      releasePacket(packet);
      return false;
    }
    return true;
  }

  void sendAckTo(const ContactEntry& destination, const uint8_t* ack,
                 uint8_t ackBytes) {
    ::mesh::Packet* packet = createAck(ack, ackBytes);
    if (!authorizeAuthenticatedReply(packet)) return;
    if (destination.outPathLen == kUnknownPath) {
      sendFlood(packet, kTextAckDelayMillis);
    } else {
      sendDirect(packet, destination.outPath, destination.outPathLen,
                 kTextAckDelayMillis);
    }
  }
};

bool writeIdentityRecord(const ::mesh::LocalIdentity& identity) {
  uint8_t record[kIdentityRecordBytes]{};
  memcpy(record, kIdentityMagic, sizeof(kIdentityMagic));
  writeLe16(record + 4, kIdentitySchema);
  writeLe16(record + 6, static_cast<uint16_t>(kIdentityRecordBytes));
  ::mesh::LocalIdentity copy = identity;
  if (copy.writeTo(record + kIdentityOffsetData, kIdentityBytes) !=
      kIdentityBytes) {
    return false;
  }
  writeLe32(record + kIdentityOffsetCrc,
            crc32WithZeroedIdentityCrc(record, sizeof(record)));

  Preferences preferences;
  if (!preferences.begin(kIdentityNamespace, false)) return false;
  const size_t written =
      preferences.putBytes(kIdentityKey, record, sizeof(record));
  uint8_t verification[kIdentityRecordBytes]{};
  const bool verified = written == sizeof(record) &&
      preferences.getBytesLength(kIdentityKey) == sizeof(verification) &&
      preferences.getBytes(kIdentityKey, verification,
                           sizeof(verification)) == sizeof(verification) &&
      memcmp(record, verification, sizeof(record)) == 0;
  preferences.end();
  return verified;
}

enum class IdentityLoadResult : uint8_t { Loaded, Missing, Invalid, Error };

IdentityLoadResult loadIdentityRecord(::mesh::LocalIdentity& output) {
  Preferences preferences;
  // Open read/write so a first boot can create the namespace.  No bytes are
  // written on the load path itself.
  if (!preferences.begin(kIdentityNamespace, false)) {
    return IdentityLoadResult::Error;
  }
  const size_t storedBytes = preferences.getBytesLength(kIdentityKey);
  if (storedBytes == 0) {
    preferences.end();
    return IdentityLoadResult::Missing;
  }
  if (storedBytes != kIdentityRecordBytes) {
    preferences.end();
    return IdentityLoadResult::Invalid;
  }
  uint8_t record[kIdentityRecordBytes]{};
  const size_t read =
      preferences.getBytes(kIdentityKey, record, sizeof(record));
  preferences.end();
  if (read != sizeof(record) ||
      memcmp(record, kIdentityMagic, sizeof(kIdentityMagic)) != 0 ||
      readLe16(record + 4) != kIdentitySchema ||
      readLe16(record + 6) != kIdentityRecordBytes ||
      readLe32(record + kIdentityOffsetCrc) !=
          crc32WithZeroedIdentityCrc(record, sizeof(record))) {
    return IdentityLoadResult::Invalid;
  }

  const uint8_t* privateKey = record + kIdentityOffsetData;
  const uint8_t* storedPublicKey = privateKey + PRV_KEY_SIZE;
  if (!::mesh::LocalIdentity::validatePrivateKey(privateKey)) {
    return IdentityLoadResult::Invalid;
  }
  ::mesh::LocalIdentity candidate;
  candidate.readFrom(privateKey, PRV_KEY_SIZE);
  if (memcmp(candidate.pub_key, storedPublicKey, PUB_KEY_SIZE) != 0) {
    return IdentityLoadResult::Invalid;
  }
  output = candidate;
  return IdentityLoadResult::Loaded;
}

void bytesToUpperHex(char* destination, const uint8_t* source,
                     size_t sourceBytes) {
  constexpr char digits[] = "0123456789ABCDEF";
  for (size_t index = 0; index < sourceBytes; ++index) {
    destination[index * 2U] = digits[source[index] >> 4U];
    destination[index * 2U + 1U] = digits[source[index] & 0x0fU];
  }
  destination[sourceBytes * 2U] = '\0';
}

bool sameActiveProfile(const Settings& a, const Settings& b) {
  return a.enabled == b.enabled && sameRadioProfile(a.radio, b.radio);
}

}  // namespace

struct KitsuMeshTransport::Impl final : public AdvertSink {
  KitsuBoard board{};
  TxGate txGate{};
  Settings settings = defaultSettings();
  CustomSX1262 physical{new Module(kLoraCs, kLoraDio1, kLoraReset,
                                    kLoraBusy)};
  GatedCustomSX1262Wrapper driver{physical, board, txGate, settings};
  ArduinoMillis millisClock{};
  EspRng rng{};
  SessionRtc rtc{};
  StaticPoolPacketManager packets{kPacketPoolSize};
  SimpleMeshTables tables{};
  MessagingState messaging{};
  KitsuClient client{driver, millisClock, rng, rtc, packets, tables, *this,
                     messaging, txGate, settings};

  ClientIdentity advertisedIdentity{};
  bool identityReady = false;
  bool hardwareInitialized = false;
  bool active = false;
  int16_t radioCode = RADIOLIB_ERR_UNKNOWN;
  ReceivedAdvert advertQueue[kAdvertQueueSize]{};
  uint8_t advertRead = 0;
  uint8_t advertWrite = 0;
  uint8_t advertCount = 0;
  uint32_t receivedAdverts = 0;
  uint32_t droppedAdverts = 0;

  bool ensureIdentity() {
    const IdentityLoadResult loaded = loadIdentityRecord(client.self_id);
    if (loaded == IdentityLoadResult::Loaded) {
      identityReady = true;
      return true;
    }
    if (loaded != IdentityLoadResult::Missing) return false;

    ::mesh::LocalIdentity candidate(&rng);
    for (uint8_t attempt = 0;
         attempt < 10U &&
         (candidate.pub_key[0] == 0x00 || candidate.pub_key[0] == 0xff);
         ++attempt) {
      candidate = ::mesh::LocalIdentity(&rng);
    }
    if (candidate.pub_key[0] == 0x00 || candidate.pub_key[0] == 0xff ||
        !writeIdentityRecord(candidate)) {
      return false;
    }
    client.self_id = candidate;
    identityReady = true;
    return true;
  }

  void clearOutboundQueue() {
    while (packets.getOutboundTotal() > 0) {
      ::mesh::Packet* packet = packets.removeOutboundByIdx(0);
      if (packet) packets.free(packet);
    }
  }

  void clearPacketQueues() {
    clearOutboundQueue();
    // MeshCore delays weak flood packets by at most 32 seconds.  A time far
    // enough in the future drains those stale packets before a profile swap.
    const uint32_t future = millis() + 60000UL;
    for (;;) {
      ::mesh::Packet* packet = packets.getNextInbound(future);
      if (!packet) break;
      packets.free(packet);
    }
  }

  TransportStatus configureRadio(const Settings& next) {
    txGate.lock();
    driver.revokeOneShot();
    client.cancelQueuedSends();
    clearOutboundQueue();

    // An asynchronous frame that already owns the radio cannot be recalled.
    // Leave its tracker intact and reject the profile mutation until the next
    // command, after logTx/logTxFail has reported the honest outcome.
    if (active && !driver.isInRecvMode()) {
      return TransportStatus::InvalidArgument;
    }
    client.cancelAllSends();
    clearPacketQueues();
    if (hardwareInitialized) physical.standby();
    active = false;
    settings = next;

    if (!next.enabled) {
      if (hardwareInitialized) physical.sleep(false);
      radioCode = RADIOLIB_ERR_NONE;
      return TransportStatus::Disabled;
    }
    if (!next.radio.selected()) return TransportStatus::NoProfile;

    SPI.begin(kLoraSck, kLoraMiso, kLoraMosi, kLoraCs);
    radioCode = physical.begin(
        static_cast<float>(next.radio.frequencyHz) / 1000000.0f,
        static_cast<float>(next.radio.bandwidthHz) / 1000.0f,
        next.radio.spreadingFactor, next.radio.codingRate,
        next.radio.syncWord, next.radio.txPowerDbm,
        next.radio.preambleSymbols, kTcxoVoltage);
    if (radioCode != RADIOLIB_ERR_NONE) return TransportStatus::RadioInitFailed;
    hardwareInitialized = true;

    const int16_t crc = physical.setCRC(1);
    const int16_t current = physical.setCurrentLimit(140.0f);
    const int16_t rfSwitch = physical.setDio2AsRfSwitch(true);
    const int16_t boosted = physical.setRxBoostedGainMode(true);
    if (crc != RADIOLIB_ERR_NONE || current != RADIOLIB_ERR_NONE ||
        rfSwitch != RADIOLIB_ERR_NONE || boosted != RADIOLIB_ERR_NONE) {
      radioCode = crc != RADIOLIB_ERR_NONE
                      ? crc
                      : current != RADIOLIB_ERR_NONE
                            ? current
                            : rfSwitch != RADIOLIB_ERR_NONE ? rfSwitch
                                                            : boosted;
      physical.sleep(false);
      return TransportStatus::RadioInitFailed;
    }

    client.begin();
    // v1.17.1's wrapper also derives SX1262 receive deadlines here.  Direct
    // register setters alone leave the generic 66 ms preamble window active;
    // UK/EU Narrow SF8 with its 32-symbol runtime preamble needs ~182 ms.
    driver.setParams(
        static_cast<float>(next.radio.frequencyHz) / 1000000.0f,
        static_cast<float>(next.radio.bandwidthHz) / 1000.0f,
        next.radio.spreadingFactor, next.radio.codingRate);
    driver.setTxPower(next.radio.txPowerDbm);
    client.loop();  // Enter RX immediately; no transmission is queued.
    active = true;
    return TransportStatus::Ok;
  }

  TransportStatus makeAdvert(const Settings& requested,
                             const CurrentLocationOnce& current,
                             ::mesh::Packet*& output) {
    output = nullptr;
    if (!identityReady) return TransportStatus::IdentityStorageFailed;
    if (!rtc.valid()) return TransportStatus::TimeUnset;
    if (validateSettings(requested) != Status::Ok) {
      return TransportStatus::InvalidSettings;
    }

    AdvertLocation location{};
    const bool includeLocation = selectAdvertLocation(requested, current,
                                                       location);
    if (requested.locationMode == LocationMode::CurrentOnce &&
        !includeLocation) {
      return TransportStatus::LocationUnavailable;
    }

    uint8_t appData[MAX_ADVERT_DATA_SIZE]{};
    size_t appBytes = 1;
    appData[0] = ADV_TYPE_CHAT;
    if (includeLocation) {
      appData[0] |= ADV_LATLON_MASK;
      memcpy(appData + appBytes, &location.coordinates.latitudeE6, 4);
      appBytes += 4;
      memcpy(appData + appBytes, &location.coordinates.longitudeE6, 4);
      appBytes += 4;
    }
    appData[0] |= ADV_NAME_MASK;
    const size_t remaining = MAX_ADVERT_DATA_SIZE - appBytes;
    const size_t nameBytes = strnlen(advertisedIdentity.advertisedName,
                                     remaining);
    memcpy(appData + appBytes, advertisedIdentity.advertisedName, nameBytes);
    appBytes += nameBytes;

    output = client.createAdvert(client.self_id, appData, appBytes);
    return output ? TransportStatus::Ok : TransportStatus::PacketPoolFull;
  }

  void captureAdvert(::mesh::Packet* packet,
                     const ::mesh::Identity& identity,
                     uint32_t timestamp, const uint8_t* appData,
                     size_t appDataBytes) override {
    if (!appData || appDataBytes < 1U || appDataBytes > MAX_ADVERT_DATA_SIZE) {
      return;
    }

    ReceivedAdvert event{};
    event.type = appData[0] & 0x0fU;
    event.timestamp = timestamp;
    memcpy(event.publicKey, identity.pub_key, sizeof(event.publicKey));
    memcpy(event.publicKeyPrefix, identity.pub_key,
           sizeof(event.publicKeyPrefix));
    event.rssi = driver.getLastRSSI();
    event.snr = packet ? packet->getSNR() : 0.0f;

    size_t offset = 1;
    if ((appData[0] & ADV_LATLON_MASK) != 0) {
      if (offset + 8U > appDataBytes) return;
      memcpy(&event.location.latitudeE6, appData + offset, 4);
      offset += 4;
      memcpy(&event.location.longitudeE6, appData + offset, 4);
      offset += 4;
      if (validateCoordinates(event.location) != Status::Ok) return;
      event.hasLocation = true;
    }
    if ((appData[0] & ADV_FEAT1_MASK) != 0) {
      if (offset + 2U > appDataBytes) return;
      offset += 2;
    }
    if ((appData[0] & ADV_FEAT2_MASK) != 0) {
      if (offset + 2U > appDataBytes) return;
      offset += 2;
    }
    if ((appData[0] & ADV_NAME_MASK) != 0) {
      const size_t available = appDataBytes - offset;
      sanitizeUtf8Name(event.name, sizeof(event.name), appData + offset,
                       available);
    }
    event.kitsuNamed = strstr(event.name, "Kitsu KT") != nullptr;
    if (event.name[0] != '\0') {
      messaging.learnAdvert(identity.pub_key, event.name, event.type,
                            timestamp);
    }

    if (advertCount == kAdvertQueueSize) {
      // Preserve the newest view of a busy mesh while making loss explicit.
      advertRead = static_cast<uint8_t>((advertRead + 1U) % kAdvertQueueSize);
      --advertCount;
      ++droppedAdverts;
    }
    advertQueue[advertWrite] = event;
    advertWrite = static_cast<uint8_t>((advertWrite + 1U) % kAdvertQueueSize);
    ++advertCount;
    ++receivedAdverts;
  }
};

const char* transportStatusName(TransportStatus status) {
  switch (status) {
    case TransportStatus::Ok: return "ok";
    case TransportStatus::Disabled: return "disabled";
    case TransportStatus::NoProfile: return "no_profile";
    case TransportStatus::InvalidSettings: return "invalid_settings";
    case TransportStatus::InvalidIdentity: return "invalid_identity";
    case TransportStatus::IdentityStorageFailed:
      return "identity_storage_failed";
    case TransportStatus::RadioInitFailed: return "radio_init_failed";
    case TransportStatus::TimeUnset: return "time_unset";
    case TransportStatus::InvalidTime: return "invalid_time";
    case TransportStatus::LocationUnavailable:
      return "location_unavailable";
    case TransportStatus::TxLocked: return "tx_locked";
    case TransportStatus::PacketPoolFull: return "packet_pool_full";
    case TransportStatus::OutputTooSmall: return "output_too_small";
    case TransportStatus::InvalidArgument: return "invalid_argument";
    case TransportStatus::ContactNotFound: return "contact_not_found";
    case TransportStatus::ContactNotClient: return "contact_not_client";
    case TransportStatus::ContactTableFull: return "contact_table_full";
    case TransportStatus::ChannelNotFound: return "channel_not_found";
    case TransportStatus::TextTooLong: return "text_too_long";
    case TransportStatus::SendBusy: return "send_busy";
    case TransportStatus::MessagingStorageFailed:
      return "messaging_storage_failed";
  }
  return "unknown";
}

KitsuMeshTransport::KitsuMeshTransport() : impl_(new Impl()) {}

KitsuMeshTransport::~KitsuMeshTransport() {
  delete impl_;
  impl_ = nullptr;
}

TransportStatus KitsuMeshTransport::begin(const Settings& settings,
                                          const ClientIdentity& identity) {
  if (!validShortUid(identity.shortUid) ||
      identity.role != Role::Client || identity.advertisedName[0] == '\0') {
    return TransportStatus::InvalidIdentity;
  }
  if (validateSettings(settings) != Status::Ok) {
    return TransportStatus::InvalidSettings;
  }
  impl_->advertisedIdentity = identity;
  if (!impl_->ensureIdentity()) {
    return TransportStatus::IdentityStorageFailed;
  }
  // Messaging storage is an optional capability: corruption must not stop
  // passive adverts/RX.  Operations fail closed until an explicit app reset.
  impl_->messaging.begin();
  return impl_->configureRadio(settings);
}

TransportStatus KitsuMeshTransport::applySettings(const Settings& settings) {
  if (validateSettings(settings) != Status::Ok) {
    return TransportStatus::InvalidSettings;
  }
  return impl_->configureRadio(settings);
}

void KitsuMeshTransport::loop() {
  if (impl_->active) impl_->client.loop();
  impl_->messaging.checkTimeout();
}

bool KitsuMeshTransport::identityReady() const {
  return impl_->identityReady;
}

bool KitsuMeshTransport::active() const { return impl_->active; }

int16_t KitsuMeshTransport::radioCode() const { return impl_->radioCode; }

uint32_t KitsuMeshTransport::profileId() const {
  return impl_->settings.radio.profileId;
}

TransportStatus KitsuMeshTransport::setEpoch(uint32_t epochSeconds) {
  if (epochSeconds < kMinimumEpoch || epochSeconds > kMaximumEpoch) {
    return TransportStatus::InvalidTime;
  }
  impl_->rtc.setCurrentTime(epochSeconds);
  return TransportStatus::Ok;
}

bool KitsuMeshTransport::timeValid() const { return impl_->rtc.valid(); }

uint32_t KitsuMeshTransport::currentEpoch() const {
  return impl_->rtc.valid() ? impl_->rtc.getCurrentTime() : 0;
}

bool KitsuMeshTransport::unlockTransmit(const Settings& settings,
                                        bool explicitUserApproval) {
  return impl_->active && sameActiveProfile(settings, impl_->settings) &&
      impl_->txGate.unlockForSession(settings, explicitUserApproval);
}

void KitsuMeshTransport::lockTransmit() {
  impl_->txGate.lock();
  impl_->driver.revokeOneShot();
  impl_->client.cancelQueuedSends();
  // A lock is an immediate revocation for anything not already on air.  This
  // also makes `introduce` followed by `tx lock` in the same serial drain safe.
  impl_->clearOutboundQueue();
}

bool KitsuMeshTransport::transmitAllowed(const Settings& settings) const {
  return impl_->active && sameActiveProfile(settings, impl_->settings) &&
      impl_->txGate.allowsTransmit(settings);
}

TransportStatus KitsuMeshTransport::exportSignedAdvert(
    const Settings& settings, const CurrentLocationOnce& current,
    char* outputHex, size_t outputCapacity, size_t& outputLength) {
  outputLength = 0;
  if (!outputHex || outputCapacity == 0) {
    return TransportStatus::InvalidArgument;
  }
  ::mesh::Packet* packet = nullptr;
  const TransportStatus status = impl_->makeAdvert(settings, current, packet);
  if (status != TransportStatus::Ok) return status;

  packet->header &= ~PH_ROUTE_MASK;
  packet->header |= ROUTE_TYPE_FLOOD;
  packet->setPathHashSizeAndCount(1, 0);
  uint8_t wire[MAX_TRANS_UNIT]{};
  const uint8_t wireBytes = packet->writeTo(wire);
  impl_->client.releasePacket(packet);
  if (outputCapacity < static_cast<size_t>(wireBytes) * 2U + 1U) {
    return TransportStatus::OutputTooSmall;
  }
  bytesToUpperHex(outputHex, wire, wireBytes);
  outputLength = static_cast<size_t>(wireBytes) * 2U;
  return TransportStatus::Ok;
}

TransportStatus KitsuMeshTransport::introduce(
    AdvertScope scope, const Settings& settings,
    const CurrentLocationOnce& current) {
  if (!impl_->active || !settings.enabled ||
      !sameActiveProfile(settings, impl_->settings)) {
    return TransportStatus::Disabled;
  }
  if (!impl_->txGate.allowsTransmit(settings)) {
    return TransportStatus::TxLocked;
  }
  ::mesh::Packet* packet = nullptr;
  const TransportStatus status = impl_->makeAdvert(settings, current, packet);
  if (status != TransportStatus::Ok) return status;

  if (scope == AdvertScope::Nearby) {
    impl_->client.sendZeroHop(packet);
  } else if (scope == AdvertScope::Flood) {
    impl_->client.sendFlood(packet);
  } else {
    impl_->client.releasePacket(packet);
    return TransportStatus::InvalidArgument;
  }
  return TransportStatus::Ok;
}

bool KitsuMeshTransport::takeAdvert(ReceivedAdvert& output) {
  if (impl_->advertCount == 0) return false;
  output = impl_->advertQueue[impl_->advertRead];
  impl_->advertRead = static_cast<uint8_t>(
      (impl_->advertRead + 1U) % kAdvertQueueSize);
  --impl_->advertCount;
  return true;
}

bool KitsuMeshTransport::publicKeyHex(char* output,
                                      size_t outputCapacity) const {
  if (!output || outputCapacity < PUB_KEY_SIZE * 2U + 1U ||
      !impl_->identityReady) {
    return false;
  }
  bytesToUpperHex(output, impl_->client.self_id.pub_key, PUB_KEY_SIZE);
  return true;
}

size_t KitsuMeshTransport::contactCount() const {
  return impl_->messaging.contactCount();
}

bool KitsuMeshTransport::contactAt(size_t index, ContactRecord& output) const {
  const ContactEntry* entry = impl_->messaging.contactAt(index);
  if (!entry) return false;
  ContactRecord record{};
  memcpy(record.publicKey, entry->publicKey, PUB_KEY_SIZE);
  memcpy(record.name, entry->name, sizeof(record.name));
  record.type = entry->type;
  record.pathKnown = entry->outPathLen != kUnknownPath;
  record.lastAdvertTimestamp = entry->lastAdvertTimestamp;
  record.pinned = entry->pinned;
  output = record;
  return true;
}

TransportStatus KitsuMeshTransport::upsertContact(
    const uint8_t publicKey[32], const char* name, uint8_t type) {
  if (!publicKey || !name ||
      (impl_->identityReady &&
       memcmp(publicKey, impl_->client.self_id.pub_key, PUB_KEY_SIZE) == 0)) {
    return TransportStatus::InvalidArgument;
  }
  if (impl_->messaging.pending()) return TransportStatus::SendBusy;
  return impl_->messaging.upsertContact(publicKey, name, type, true);
}

TransportStatus KitsuMeshTransport::stageObservedContact(
    const uint8_t publicKey[32], const char* name, uint8_t type,
    uint32_t advertTimestamp) {
  if (!publicKey || !name ||
      (impl_->identityReady &&
       memcmp(publicKey, impl_->client.self_id.pub_key, PUB_KEY_SIZE) == 0)) {
    return TransportStatus::InvalidArgument;
  }
  return impl_->messaging.stageObservedContact(publicKey, name, type,
                                               advertTimestamp);
}

TransportStatus KitsuMeshTransport::removeContact(
    const uint8_t publicKey[32]) {
  if (!publicKey) return TransportStatus::InvalidArgument;
  lockTransmit();
  if (impl_->client.trackedSendInProgress()) {
    return TransportStatus::SendBusy;
  }
  impl_->client.cancelAllSends();
  return impl_->messaging.removeContact(publicKey);
}

bool KitsuMeshTransport::channelAt(uint8_t slot, ChannelRecord& output) const {
  if (slot >= kMeshChannelCapacity) return false;
  const ChannelEntry* entry = impl_->messaging.channel(slot);
  ChannelRecord record{};
  record.slot = slot;
  if (entry) {
    record.configured = true;
    memcpy(record.name, entry->name, sizeof(record.name));
    record.hash = entry->channel.hash[0];
  }
  output = record;
  return true;
}

TransportStatus KitsuMeshTransport::setChannel(
    uint8_t slot, const char* name, const uint8_t secret[32]) {
  if (!name || !secret) return TransportStatus::InvalidArgument;
  lockTransmit();
  return impl_->messaging.setChannel(slot, name, secret);
}

TransportStatus KitsuMeshTransport::clearChannel(uint8_t slot) {
  lockTransmit();
  return impl_->messaging.clearChannel(slot);
}

TransportStatus KitsuMeshTransport::sendDirectText(
    const Settings& settings, const uint8_t recipientPublicKey[32],
    const char* text, uint8_t attempt, uint32_t& queuedTimestamp,
    uint32_t& expectedAck, MessageRoute& route) {
  queuedTimestamp = 0;
  expectedAck = 0;
  route = MessageRoute::Flood;
  if (!recipientPublicKey || !text || text[0] == '\0') {
    return TransportStatus::InvalidArgument;
  }
  if (!impl_->messaging.storageReady()) {
    return TransportStatus::MessagingStorageFailed;
  }
  if (!impl_->active || !sameActiveProfile(settings, impl_->settings)) {
    return TransportStatus::Disabled;
  }
  if (!impl_->txGate.allowsTransmit(settings)) {
    return TransportStatus::TxLocked;
  }
  if (!impl_->rtc.valid()) return TransportStatus::TimeUnset;
  if (strnlen(text, kDirectOutboundTextBytes + 1U) >
      kDirectOutboundTextBytes) {
    return TransportStatus::TextTooLong;
  }
  ContactEntry* recipient = impl_->messaging.findContact(recipientPublicKey);
  if (!recipient) return TransportStatus::ContactNotFound;
  if (recipient->type != ADV_TYPE_CHAT) {
    return TransportStatus::ContactNotClient;
  }
  const uint32_t timestamp = impl_->rtc.getCurrentTimeUnique();
  const TransportStatus status = impl_->client.sendDirectText(
      *recipient, timestamp, text, attempt, expectedAck, route);
  if (status == TransportStatus::Ok) queuedTimestamp = timestamp;
  return status;
}

TransportStatus KitsuMeshTransport::sendChannelText(
    const Settings& settings, uint8_t slot, const char* text,
    uint32_t& queuedTimestamp) {
  queuedTimestamp = 0;
  if (!text || text[0] == '\0') return TransportStatus::InvalidArgument;
  if (!impl_->messaging.storageReady()) {
    return TransportStatus::MessagingStorageFailed;
  }
  if (!impl_->active || !sameActiveProfile(settings, impl_->settings)) {
    return TransportStatus::Disabled;
  }
  if (!impl_->txGate.allowsTransmit(settings)) {
    return TransportStatus::TxLocked;
  }
  if (!impl_->rtc.valid()) return TransportStatus::TimeUnset;
  const ChannelEntry* channel = impl_->messaging.channel(slot);
  if (!channel) return TransportStatus::ChannelNotFound;
  const uint32_t timestamp = impl_->rtc.getCurrentTimeUnique();
  const TransportStatus status = impl_->client.sendChannelText(
      *channel, slot, timestamp, impl_->advertisedIdentity.advertisedName,
      text);
  if (status == TransportStatus::Ok) queuedTimestamp = timestamp;
  return status;
}

TransportStatus KitsuMeshTransport::sendDirectTextOnce(
    const Settings& settings, const uint8_t recipientPublicKey[32],
    const char* text, uint8_t attempt, bool explicitUserApproval,
    uint32_t& queuedTimestamp, uint32_t& expectedAck,
    MessageRoute& route) {
  queuedTimestamp = 0U;
  expectedAck = 0U;
  route = MessageRoute::Flood;
  if (!recipientPublicKey || !text || text[0] == '\0') {
    return TransportStatus::InvalidArgument;
  }
  // This entry point is reserved for a single authenticated owner action.
  // An unrelated, already-open serial/session gate is not a substitute for
  // approval of this request.
  if (!explicitUserApproval) return TransportStatus::TxLocked;
  if (!impl_->messaging.storageReady()) {
    return TransportStatus::MessagingStorageFailed;
  }
  if (!impl_->active || !sameActiveProfile(settings, impl_->settings)) {
    return TransportStatus::Disabled;
  }
  if (!impl_->rtc.valid()) return TransportStatus::TimeUnset;
  if (strnlen(text, kDirectOutboundTextBytes + 1U) >
      kDirectOutboundTextBytes) {
    return TransportStatus::TextTooLong;
  }
  ContactEntry* recipient = impl_->messaging.findContact(recipientPublicKey);
  if (!recipient) return TransportStatus::ContactNotFound;
  if (recipient->type != ADV_TYPE_CHAT) {
    return TransportStatus::ContactNotClient;
  }
  if (impl_->messaging.pending() || impl_->client.trackedSendInProgress() ||
      impl_->packets.getOutboundTotal() != 0) {
    return TransportStatus::SendBusy;
  }

  const bool sessionAllowed = impl_->txGate.allowsTransmit(settings);
  if (!sessionAllowed &&
      !impl_->driver.armOneShot(settings, explicitUserApproval)) {
    return TransportStatus::TxLocked;
  }
  const uint32_t timestamp = impl_->rtc.getCurrentTimeUnique();
  const TransportStatus status = impl_->client.sendDirectText(
      *recipient, timestamp, text, attempt, expectedAck, route);
  if (status != TransportStatus::Ok) {
    if (!sessionAllowed) impl_->driver.revokeOneShot();
    return status;
  }
  queuedTimestamp = timestamp;
  return TransportStatus::Ok;
}

TransportStatus KitsuMeshTransport::sendChannelTextOnce(
    const Settings& settings, uint8_t slot, const char* text,
    bool explicitUserApproval, uint32_t& queuedTimestamp) {
  queuedTimestamp = 0U;
  if (!text || text[0] == '\0') return TransportStatus::InvalidArgument;
  // Fail before observing the broader session gate: a one-shot API always
  // requires authorization for this exact owner action.
  if (!explicitUserApproval) return TransportStatus::TxLocked;
  if (!impl_->messaging.storageReady()) {
    return TransportStatus::MessagingStorageFailed;
  }
  if (!impl_->active || !sameActiveProfile(settings, impl_->settings)) {
    return TransportStatus::Disabled;
  }
  if (!impl_->rtc.valid()) return TransportStatus::TimeUnset;
  if (strnlen(text, kChannelOutboundTextBytes + 1U) >
      kChannelOutboundTextBytes) {
    return TransportStatus::TextTooLong;
  }
  const ChannelEntry* channel = impl_->messaging.channel(slot);
  if (!channel) return TransportStatus::ChannelNotFound;
  if (impl_->client.trackedSendInProgress() ||
      impl_->packets.getOutboundTotal() != 0) {
    return TransportStatus::SendBusy;
  }

  const bool sessionAllowed = impl_->txGate.allowsTransmit(settings);
  if (!sessionAllowed &&
      !impl_->driver.armOneShot(settings, explicitUserApproval)) {
    return TransportStatus::TxLocked;
  }
  const uint32_t timestamp = impl_->rtc.getCurrentTimeUnique();
  const TransportStatus status = impl_->client.sendChannelText(
      *channel, slot, timestamp, impl_->advertisedIdentity.advertisedName,
      text);
  if (status != TransportStatus::Ok) {
    if (!sessionAllowed) impl_->driver.revokeOneShot();
    return status;
  }
  queuedTimestamp = timestamp;
  return TransportStatus::Ok;
}

bool KitsuMeshTransport::takeMessage(ReceivedMessage& output) {
  return impl_->messaging.takeMessage(output);
}

bool KitsuMeshTransport::takeDelivery(DeliveryEvent& output) {
  return impl_->messaging.takeDelivery(output);
}

bool KitsuMeshTransport::directSendPending() const {
  return impl_->messaging.pending();
}

bool KitsuMeshTransport::sendInProgress() const {
  return impl_->client.trackedSendInProgress();
}

uint32_t KitsuMeshTransport::droppedMessageCount() const {
  return impl_->messaging.droppedMessageCount();
}

uint32_t KitsuMeshTransport::droppedDeliveryCount() const {
  return impl_->messaging.droppedDeliveryCount();
}

bool KitsuMeshTransport::messagingStorageReady() const {
  return impl_->messaging.storageReady();
}

TransportStatus KitsuMeshTransport::resetMessagingState() {
  lockTransmit();
  if (impl_->client.trackedSendInProgress()) {
    return TransportStatus::SendBusy;
  }
  impl_->client.cancelAllSends();
  return impl_->messaging.reset() ? TransportStatus::Ok
                                  : TransportStatus::MessagingStorageFailed;
}

uint32_t KitsuMeshTransport::receivedAdvertCount() const {
  return impl_->receivedAdverts;
}

uint32_t KitsuMeshTransport::droppedAdvertCount() const {
  return impl_->droppedAdverts;
}

uint32_t KitsuMeshTransport::queuedAdvertCount() const {
  const int queued = impl_->packets.getOutboundTotal();
  return queued > 0 ? static_cast<uint32_t>(queued) : 0U;
}

}  // namespace mesh
}  // namespace kitsu868
