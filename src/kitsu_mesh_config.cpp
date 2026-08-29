#include "kitsu_mesh_config.h"

#include <string.h>

#ifdef ARDUINO
#include <Preferences.h>
#endif

namespace kitsu868 {
namespace mesh {
namespace {

constexpr uint8_t kMagic[4] = {'K', 'M', 'C', '1'};
constexpr char kAdvertisedPrefix[] = "\xF0\x9F\xA6\x8A Kitsu ";
constexpr char kPreferencesNamespace[] = "kitsu_mesh";
constexpr char kPreferencesKey[] = "client_v1";

constexpr size_t kOffsetMagic = 0;
constexpr size_t kOffsetSchema = 4;
constexpr size_t kOffsetBytes = 6;
constexpr size_t kOffsetCrc32 = 8;
constexpr size_t kOffsetRole = 12;
constexpr size_t kOffsetLocationMode = 13;
constexpr size_t kOffsetTxPolicy = 14;
constexpr size_t kOffsetEnabled = 15;
constexpr size_t kOffsetFixedLatitude = 16;
constexpr size_t kOffsetFixedLongitude = 20;
constexpr size_t kOffsetProfileId = 24;
constexpr size_t kOffsetFrequency = 28;
constexpr size_t kOffsetBandwidth = 32;
constexpr size_t kOffsetSpreadingFactor = 36;
constexpr size_t kOffsetCodingRate = 37;
constexpr size_t kOffsetSyncWord = 38;
constexpr size_t kOffsetTxPower = 39;
constexpr size_t kOffsetPreamble = 40;
constexpr size_t kOffsetReserved = 42;

static_assert(kOffsetReserved < kPersistedSettingsBytes,
              "settings record offsets exceed record size");
static_assert(sizeof(kAdvertisedPrefix) - 1 + 6 + 1 <=
                  kAdvertisedNameBytes,
              "Kitsu advertised name buffer is too small");

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

uint32_t crc32WithZeroedField(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xffffffffUL;
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value =
        index >= kOffsetCrc32 && index < kOffsetCrc32 + 4 ? 0 : bytes[index];
    crc ^= value;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^
          (0xedb88320UL & (0U - static_cast<uint32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

bool profileIsAllZero(const RadioProfile& profile) {
  return profile.profileId == 0 && profile.frequencyHz == 0 &&
      profile.bandwidthHz == 0 && profile.spreadingFactor == 0 &&
      profile.codingRate == 0 && profile.syncWord == 0 &&
      profile.txPowerDbm == 0 && profile.preambleSymbols == 0;
}

}  // namespace

const char* statusName(Status status) {
  switch (status) {
    case Status::Ok: return "ok";
    case Status::NullArgument: return "null_argument";
    case Status::WrongLength: return "wrong_length";
    case Status::InvalidShortUid: return "invalid_short_uid";
    case Status::InvalidLocationMode: return "invalid_location_mode";
    case Status::InvalidCoordinates: return "invalid_coordinates";
    case Status::LocationPrivacyResidue: return "location_privacy_residue";
    case Status::InvalidEnabledFlag: return "invalid_enabled_flag";
    case Status::InvalidTxPolicy: return "invalid_tx_policy";
    case Status::IncompleteRadioProfile: return "incomplete_radio_profile";
    case Status::InvalidFrequency: return "invalid_frequency";
    case Status::InvalidBandwidth: return "invalid_bandwidth";
    case Status::InvalidSpreadingFactor: return "invalid_spreading_factor";
    case Status::InvalidCodingRate: return "invalid_coding_rate";
    case Status::InvalidTxPower: return "invalid_tx_power";
    case Status::InvalidPreamble: return "invalid_preamble";
    case Status::BadMagic: return "bad_magic";
    case Status::UnsupportedSchema: return "unsupported_schema";
    case Status::UnexpectedRole: return "unexpected_role";
    case Status::ReservedNotZero: return "reserved_not_zero";
    case Status::IntegrityMismatch: return "integrity_mismatch";
  }
  return "unknown";
}

Settings defaultSettings() {
  return Settings{};
}

RadioProfile ukEuNarrowProfile() {
  RadioProfile profile{};
  profile.profileId = kUkEuNarrowProfileId;
  profile.frequencyHz = 869618000UL;
  profile.bandwidthHz = 62500UL;
  profile.spreadingFactor = 8;
  profile.codingRate = 5;
  profile.syncWord = 0x12;
  profile.txPowerDbm = 22;
  profile.preambleSymbols = 32;
  return profile;
}

Status validateCoordinates(const Coordinates& coordinates) {
  if (coordinates.latitudeE6 < -90000000L ||
      coordinates.latitudeE6 > 90000000L ||
      coordinates.longitudeE6 < -180000000L ||
      coordinates.longitudeE6 > 180000000L) {
    return Status::InvalidCoordinates;
  }
  return Status::Ok;
}

Status validateRadioProfile(const RadioProfile& profile) {
  if (!profile.selected()) {
    return profileIsAllZero(profile) ? Status::Ok
                                     : Status::IncompleteRadioProfile;
  }

  // Kitsu868 is the EU/868 MHz build for the Heltec V3 SX1262.  This range is
  // a hardware/product sanity bound, not a claim that every frequency inside
  // it is legal for every modulation, duty cycle, power, or country.
  if (profile.frequencyHz < 863000000UL ||
      profile.frequencyHz > 870000000UL) {
    return Status::InvalidFrequency;
  }
  switch (profile.bandwidthHz) {
    case 7800UL:
    case 10400UL:
    case 15600UL:
    case 20800UL:
    case 31250UL:
    case 41700UL:
    case 62500UL:
    case 125000UL:
    case 250000UL:
    case 500000UL: break;
    default: return Status::InvalidBandwidth;
  }
  if (profile.spreadingFactor < 5 || profile.spreadingFactor > 12) {
    return Status::InvalidSpreadingFactor;
  }
  if (profile.codingRate < 5 || profile.codingRate > 8) {
    return Status::InvalidCodingRate;
  }
  if (profile.txPowerDbm < -9 || profile.txPowerDbm > 22) {
    return Status::InvalidTxPower;
  }
  if (profile.preambleSymbols < 4) return Status::InvalidPreamble;
  return Status::Ok;
}

Status validateSettings(const Settings& settings) {
  switch (settings.locationMode) {
    case LocationMode::Hidden:
    case LocationMode::CurrentOnce:
      if (settings.fixedLocation.latitudeE6 != 0 ||
          settings.fixedLocation.longitudeE6 != 0) {
        return Status::LocationPrivacyResidue;
      }
      break;
    case LocationMode::Fixed: {
      const Status coordinates = validateCoordinates(settings.fixedLocation);
      if (coordinates != Status::Ok) return coordinates;
      break;
    }
    default: return Status::InvalidLocationMode;
  }

  if (settings.txPolicy != TxPolicy::Locked &&
      settings.txPolicy != TxPolicy::ExplicitSession) {
    return Status::InvalidTxPolicy;
  }
  return validateRadioProfile(settings.radio);
}

bool validShortUid(const char* shortUid) {
  if (!shortUid || shortUid[0] != 'K' || shortUid[1] != 'T') {
    return false;
  }
  for (size_t index = 2; index < 6; ++index) {
    const char value = shortUid[index];
    if (!((value >= '0' && value <= '9') ||
          (value >= 'A' && value <= 'F'))) {
      return false;
    }
  }
  return shortUid[6] == '\0';
}

Status makeClientIdentity(const char* shortUid, ClientIdentity& output) {
  if (!shortUid) return Status::NullArgument;
  if (!validShortUid(shortUid)) return Status::InvalidShortUid;

  ClientIdentity candidate{};
  candidate.role = Role::Client;
  memcpy(candidate.shortUid, shortUid, kShortUidBytes);
  const size_t prefixBytes = sizeof(kAdvertisedPrefix) - 1;
  memcpy(candidate.advertisedName, kAdvertisedPrefix, prefixBytes);
  memcpy(candidate.advertisedName + prefixBytes, shortUid, 6);
  candidate.advertisedName[prefixBytes + 6] = '\0';
  output = candidate;
  return Status::Ok;
}

void hideLocation(Settings& settings) {
  settings.locationMode = LocationMode::Hidden;
  settings.fixedLocation = Coordinates{};
}

Status setFixedLocation(Settings& settings, const Coordinates& coordinates) {
  const Status status = validateCoordinates(coordinates);
  if (status != Status::Ok) return status;
  settings.locationMode = LocationMode::Fixed;
  settings.fixedLocation = coordinates;
  return Status::Ok;
}

void requestCurrentLocationOnce(Settings& settings) {
  settings.locationMode = LocationMode::CurrentOnce;
  settings.fixedLocation = Coordinates{};
}

Status stageCurrentLocationOnce(CurrentLocationOnce& current,
                                const Coordinates& coordinates) {
  const Status status = validateCoordinates(coordinates);
  if (status != Status::Ok) return status;
  current.pending = true;
  current.coordinates = coordinates;
  return Status::Ok;
}

void clearCurrentLocationOnce(CurrentLocationOnce& current) {
  current = CurrentLocationOnce{};
}

bool selectAdvertLocation(const Settings& settings,
                          const CurrentLocationOnce& current,
                          AdvertLocation& output) {
  output = AdvertLocation{};
  if (settings.locationMode == LocationMode::Fixed &&
      validateCoordinates(settings.fixedLocation) == Status::Ok) {
    output.source = LocationSource::Fixed;
    output.coordinates = settings.fixedLocation;
    return true;
  }
  if (settings.locationMode == LocationMode::CurrentOnce && current.pending &&
      validateCoordinates(current.coordinates) == Status::Ok) {
    output.source = LocationSource::CurrentOnce;
    output.coordinates = current.coordinates;
    return true;
  }
  return false;
}

Status encodeSettings(const Settings& settings, uint8_t* output,
                      size_t outputBytes) {
  if (!output) return Status::NullArgument;
  if (outputBytes != kPersistedSettingsBytes) return Status::WrongLength;
  const Status validation = validateSettings(settings);
  if (validation != Status::Ok) return validation;

  uint8_t candidate[kPersistedSettingsBytes]{};
  memcpy(candidate + kOffsetMagic, kMagic, sizeof(kMagic));
  writeLe16(candidate + kOffsetSchema, kSettingsSchemaVersion);
  writeLe16(candidate + kOffsetBytes,
            static_cast<uint16_t>(kPersistedSettingsBytes));
  candidate[kOffsetRole] = static_cast<uint8_t>(Role::Client);
  candidate[kOffsetLocationMode] = static_cast<uint8_t>(
      settings.locationMode == LocationMode::CurrentOnce
          ? LocationMode::Hidden
          : settings.locationMode);
  candidate[kOffsetTxPolicy] = static_cast<uint8_t>(settings.txPolicy);
  candidate[kOffsetEnabled] = settings.enabled ? 1U : 0U;
  writeLe32(candidate + kOffsetFixedLatitude,
            static_cast<uint32_t>(settings.fixedLocation.latitudeE6));
  writeLe32(candidate + kOffsetFixedLongitude,
            static_cast<uint32_t>(settings.fixedLocation.longitudeE6));
  writeLe32(candidate + kOffsetProfileId, settings.radio.profileId);
  writeLe32(candidate + kOffsetFrequency, settings.radio.frequencyHz);
  writeLe32(candidate + kOffsetBandwidth, settings.radio.bandwidthHz);
  candidate[kOffsetSpreadingFactor] = settings.radio.spreadingFactor;
  candidate[kOffsetCodingRate] = settings.radio.codingRate;
  candidate[kOffsetSyncWord] = settings.radio.syncWord;
  candidate[kOffsetTxPower] = static_cast<uint8_t>(settings.radio.txPowerDbm);
  writeLe16(candidate + kOffsetPreamble, settings.radio.preambleSymbols);
  writeLe32(candidate + kOffsetCrc32,
            crc32WithZeroedField(candidate, sizeof(candidate)));
  memcpy(output, candidate, sizeof(candidate));
  return Status::Ok;
}

Status decodeSettings(const uint8_t* input, size_t inputBytes,
                      Settings& output) {
  if (!input) return Status::NullArgument;
  if (inputBytes != kPersistedSettingsBytes) return Status::WrongLength;
  if (memcmp(input + kOffsetMagic, kMagic, sizeof(kMagic)) != 0) {
    return Status::BadMagic;
  }
  if (readLe16(input + kOffsetSchema) != kSettingsSchemaVersion ||
      readLe16(input + kOffsetBytes) != kPersistedSettingsBytes) {
    return Status::UnsupportedSchema;
  }
  if (input[kOffsetRole] != static_cast<uint8_t>(Role::Client)) {
    return Status::UnexpectedRole;
  }
  if (input[kOffsetEnabled] > 1U) return Status::InvalidEnabledFlag;
  for (size_t index = kOffsetReserved; index < kPersistedSettingsBytes;
       ++index) {
    if (input[index] != 0) return Status::ReservedNotZero;
  }
  if (readLe32(input + kOffsetCrc32) !=
      crc32WithZeroedField(input, inputBytes)) {
    return Status::IntegrityMismatch;
  }

  Settings candidate{};
  candidate.enabled = input[kOffsetEnabled] != 0;
  candidate.locationMode =
      static_cast<LocationMode>(input[kOffsetLocationMode]);
  candidate.txPolicy = static_cast<TxPolicy>(input[kOffsetTxPolicy]);
  candidate.fixedLocation.latitudeE6 =
      static_cast<int32_t>(readLe32(input + kOffsetFixedLatitude));
  candidate.fixedLocation.longitudeE6 =
      static_cast<int32_t>(readLe32(input + kOffsetFixedLongitude));
  candidate.radio.profileId = readLe32(input + kOffsetProfileId);
  candidate.radio.frequencyHz = readLe32(input + kOffsetFrequency);
  candidate.radio.bandwidthHz = readLe32(input + kOffsetBandwidth);
  candidate.radio.spreadingFactor = input[kOffsetSpreadingFactor];
  candidate.radio.codingRate = input[kOffsetCodingRate];
  candidate.radio.syncWord = input[kOffsetSyncWord];
  candidate.radio.txPowerDbm = static_cast<int8_t>(input[kOffsetTxPower]);
  candidate.radio.preambleSymbols = readLe16(input + kOffsetPreamble);

  const Status validation = validateSettings(candidate);
  if (validation != Status::Ok) return validation;
  // Accepting a clean v1 CurrentOnce record is safe, but consent must not
  // survive a restart even if a future writer emits one instead of
  // canonicalizing it.  Validation happens first so stale coordinates cannot
  // be hidden by normalization and silently accepted.
  if (candidate.locationMode == LocationMode::CurrentOnce) {
    hideLocation(candidate);
  }
  output = candidate;
  return Status::Ok;
}

bool sameRadioProfile(const RadioProfile& a, const RadioProfile& b) {
  return a.profileId == b.profileId && a.frequencyHz == b.frequencyHz &&
      a.bandwidthHz == b.bandwidthHz &&
      a.spreadingFactor == b.spreadingFactor &&
      a.codingRate == b.codingRate && a.syncWord == b.syncWord &&
      a.txPowerDbm == b.txPowerDbm &&
      a.preambleSymbols == b.preambleSymbols;
}

bool sameSettings(const Settings& a, const Settings& b) {
  return a.enabled == b.enabled && a.locationMode == b.locationMode &&
      a.fixedLocation.latitudeE6 == b.fixedLocation.latitudeE6 &&
      a.fixedLocation.longitudeE6 == b.fixedLocation.longitudeE6 &&
      a.txPolicy == b.txPolicy && sameRadioProfile(a.radio, b.radio);
}

bool TxGate::unlockForSession(const Settings& settings,
                              bool explicitUserApproval) {
  lock();
  if (!explicitUserApproval || settings.txPolicy != TxPolicy::ExplicitSession ||
      !settings.enabled || !settings.radio.selected() ||
      validateSettings(settings) != Status::Ok) {
    return false;
  }
  unlockedProfile_ = settings.radio;
  unlocked_ = true;
  return true;
}

void TxGate::lock() {
  unlocked_ = false;
  unlockedProfile_ = RadioProfile{};
}

bool TxGate::allowsTransmit(const Settings& settings) const {
  return unlocked_ && settings.enabled &&
      settings.txPolicy == TxPolicy::ExplicitSession &&
      validateSettings(settings) == Status::Ok &&
      sameRadioProfile(settings.radio, unlockedProfile_);
}

const char* storeStatusName(StoreStatus status) {
  switch (status) {
    case StoreStatus::Loaded: return "loaded";
    case StoreStatus::Saved: return "saved";
    case StoreStatus::Missing: return "missing";
    case StoreStatus::Invalid: return "invalid";
    case StoreStatus::Unavailable: return "unavailable";
    case StoreStatus::WriteFailed: return "write_failed";
  }
  return "unknown";
}

StoreStatus SettingsStore::load(Settings& output) const {
#ifdef ARDUINO
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    return StoreStatus::Unavailable;
  }
  const size_t storedBytes = preferences.getBytesLength(kPreferencesKey);
  if (storedBytes == 0) {
    preferences.end();
    return StoreStatus::Missing;
  }
  if (storedBytes != kPersistedSettingsBytes) {
    preferences.end();
    return StoreStatus::Invalid;
  }
  uint8_t bytes[kPersistedSettingsBytes]{};
  const size_t read = preferences.getBytes(kPreferencesKey, bytes,
                                           sizeof(bytes));
  preferences.end();
  if (read != sizeof(bytes)) return StoreStatus::Invalid;

  Settings candidate{};
  if (decodeSettings(bytes, sizeof(bytes), candidate) != Status::Ok) {
    return StoreStatus::Invalid;
  }
  output = candidate;
  return StoreStatus::Loaded;
#else
  (void)output;
  return StoreStatus::Unavailable;
#endif
}

StoreStatus SettingsStore::save(const Settings& settings) const {
#ifdef ARDUINO
  uint8_t bytes[kPersistedSettingsBytes]{};
  if (encodeSettings(settings, bytes, sizeof(bytes)) != Status::Ok) {
    return StoreStatus::Invalid;
  }

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    return StoreStatus::Unavailable;
  }
  const size_t written =
      preferences.putBytes(kPreferencesKey, bytes, sizeof(bytes));
  uint8_t verification[kPersistedSettingsBytes]{};
  const bool readBack = written == sizeof(bytes) &&
      preferences.getBytesLength(kPreferencesKey) == sizeof(verification) &&
      preferences.getBytes(kPreferencesKey, verification,
                           sizeof(verification)) == sizeof(verification);
  preferences.end();
  if (!readBack || memcmp(verification, bytes, sizeof(bytes)) != 0) {
    return StoreStatus::WriteFailed;
  }

  Settings decoded{};
  return decodeSettings(verification, sizeof(verification), decoded) ==
             Status::Ok
      ? StoreStatus::Saved
      : StoreStatus::WriteFailed;
#else
  (void)settings;
  return StoreStatus::Unavailable;
#endif
}

}  // namespace mesh
}  // namespace kitsu868
