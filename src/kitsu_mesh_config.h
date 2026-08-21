#pragma once

#include <stddef.h>
#include <stdint.h>

// Pure configuration and identity policy for the Kitsu MeshCore
// transport.  This module deliberately does not encode MeshCore packets,
// manage keys, initialize a radio, or transmit.  Those security- and
// protocol-sensitive jobs must remain in the pinned upstream transport.
namespace kitsu868 {
namespace mesh {

constexpr size_t kShortUidBytes = 7;  // "KT" + four hex digits + NUL.
constexpr size_t kAdvertisedNameBytes = 32;
constexpr size_t kPersistedSettingsBytes = 64;
constexpr uint16_t kSettingsSchemaVersion = 1;
constexpr char kUkEuNarrowProfileName[] = "UK/EU Narrow";
// Firmware-owned stable identifier for the pinned MeshCore v1.17.1 profile.
// It is configuration identity, not a cryptographic or over-the-air value.
constexpr uint32_t kUkEuNarrowProfileId = 0x4B55454EUL;  // "KUEN"

// MeshCore advertisement role 0x01 is the normal chat/client role.  Kitsu is
// a branded client capability, never a new routing role.
enum class Role : uint8_t {
  Client = 0x01,
};

enum class LocationMode : uint8_t {
  Hidden = 0,
  Fixed = 1,
  CurrentOnce = 2,
};

// ExplicitSession remembers that the owner allows TX to be unlocked, but it
// never persists an open gate.  A new TxGate starts locked after every boot.
enum class TxPolicy : uint8_t {
  Locked = 0,
  ExplicitSession = 1,
};

enum class LocationSource : uint8_t {
  None = 0,
  Fixed = 1,
  CurrentOnce = 2,
};

struct Coordinates {
  int32_t latitudeE6 = 0;
  int32_t longitudeE6 = 0;
};

// An exact, selected MeshCore network profile.  profileId is a stable,
// non-zero firmware-owned identifier; an app supplies a named profile or its
// validated PHY values, never an authoritative profileId.  The identifier is
// not a security value.  An all-zero structure means that no profile has been
// selected yet.
struct RadioProfile {
  uint32_t profileId = 0;
  uint32_t frequencyHz = 0;
  uint32_t bandwidthHz = 0;
  uint8_t spreadingFactor = 0;
  uint8_t codingRate = 0;  // LoRa denominator: 5 means 4/5, 8 means 4/8.
  uint8_t syncWord = 0;
  int8_t txPowerDbm = 0;
  uint16_t preambleSymbols = 0;

  bool selected() const { return profileId != 0; }
};

// Exact UK/EU Narrow values pinned from MeshCore companion-v1.17.1.  Keeping
// this as one helper prevents the app, persistence defaults, and radio
// adapter from drifting on sync word or preamble settings not exposed as normal
// user inputs.  At SF8, v1.17.1's radio wrapper replaces the SX1262's initial
// 16-symbol setting with the actual 32-symbol runtime preamble.
RadioProfile ukEuNarrowProfile();

struct Settings {
  // Master adapter switch.  False is the safe default.  Enabling the adapter
  // does not open TxGate or otherwise grant permission to transmit.
  bool enabled = false;
  LocationMode locationMode = LocationMode::Hidden;
  Coordinates fixedLocation{};
  TxPolicy txPolicy = TxPolicy::Locked;
  RadioProfile radio = ukEuNarrowProfile();
};

struct ClientIdentity {
  Role role = Role::Client;
  char shortUid[kShortUidBytes]{};
  char advertisedName[kAdvertisedNameBytes]{};
};

// Phone-provided coordinates live only in RAM.  The persistence codec has no
// field for them, and CurrentOnce consent is restored as Hidden after reboot.
struct CurrentLocationOnce {
  bool pending = false;
  Coordinates coordinates{};
};

struct AdvertLocation {
  LocationSource source = LocationSource::None;
  Coordinates coordinates{};
};

enum class Status : uint8_t {
  Ok = 0,
  NullArgument,
  WrongLength,
  InvalidShortUid,
  InvalidLocationMode,
  InvalidCoordinates,
  LocationPrivacyResidue,
  InvalidEnabledFlag,
  InvalidTxPolicy,
  IncompleteRadioProfile,
  InvalidFrequency,
  InvalidBandwidth,
  InvalidSpreadingFactor,
  InvalidCodingRate,
  InvalidTxPower,
  InvalidPreamble,
  BadMagic,
  UnsupportedSchema,
  UnexpectedRole,
  ReservedNotZero,
  IntegrityMismatch,
};

const char* statusName(Status status);

Settings defaultSettings();
Status validateCoordinates(const Coordinates& coordinates);
Status validateRadioProfile(const RadioProfile& profile);
Status validateSettings(const Settings& settings);

bool validShortUid(const char* shortUid);
Status makeClientIdentity(const char* shortUid, ClientIdentity& output);

void hideLocation(Settings& settings);
Status setFixedLocation(Settings& settings, const Coordinates& coordinates);
void requestCurrentLocationOnce(Settings& settings);
Status stageCurrentLocationOnce(CurrentLocationOnce& current,
                                const Coordinates& coordinates);
void clearCurrentLocationOnce(CurrentLocationOnce& current);

// Returns true only when the next standard MeshCore advertisement should
// carry coordinates.  Call clearCurrentLocationOnce() only after the one-shot
// advertisement or map upload has actually been accepted for sending.
bool selectAdvertLocation(const Settings& settings,
                          const CurrentLocationOnce& current,
                          AdvertLocation& output);

// Stable, padding-independent NVS representation.  Encoding CurrentOnce is
// intentionally canonicalized to Hidden so one-time consent cannot survive a
// restart.  Decode is transactional and does not touch output on failure.
Status encodeSettings(const Settings& settings, uint8_t* output,
                      size_t outputBytes);
Status decodeSettings(const uint8_t* input, size_t inputBytes,
                      Settings& output);

bool sameRadioProfile(const RadioProfile& a, const RadioProfile& b);

// A second safety gate above the transport.  It begins locked, is never
// persisted, requires explicit user approval plus a valid selected profile,
// and binds the unlock to that exact profile.  Regulatory checks, airtime
// accounting, and the transport's own TX controls remain additional gates.
class TxGate {
 public:
  bool unlockForSession(const Settings& settings, bool explicitUserApproval);
  void lock();
  bool allowsTransmit(const Settings& settings) const;

 private:
  bool unlocked_ = false;
  RadioProfile unlockedProfile_{};
};

enum class StoreStatus : uint8_t {
  Loaded = 0,
  Saved,
  Missing,
  Invalid,
  Unavailable,
  WriteFailed,
};

const char* storeStatusName(StoreStatus status);

// Thin ESP32 Preferences adapter.  Host builds can still compile and test the
// complete pure policy/codec; on a host these methods return Unavailable.
class SettingsStore {
 public:
  StoreStatus load(Settings& output) const;
  StoreStatus save(const Settings& settings) const;
};

}  // namespace mesh
}  // namespace kitsu868
