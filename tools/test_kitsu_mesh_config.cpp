#include "../src/kitsu_mesh_config.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace mesh = kitsu868::mesh;

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL " << description << '\n';
  }
}

mesh::RadioProfile euTestProfile() {
  return mesh::ukEuNarrowProfile();
}

void testClientIdentity() {
  mesh::ClientIdentity identity{};
  check(mesh::makeClientIdentity("KTDEAD", identity) == mesh::Status::Ok,
        "valid short UID builds an identity");
  check(identity.role == mesh::Role::Client,
        "Kitsu wire role is always standard Client");
  const uint8_t expected[] = {
      0xF0, 0x9F, 0xA6, 0x8A, ' ', 'K', 'i', 't', 's', 'u', ' ',
      'K',  'T',  'D',  'E',  'A', 'D', 0,
  };
  check(std::memcmp(identity.advertisedName, expected, sizeof(expected)) == 0,
        "advertised name is exact UTF-8 fox plus Kitsu short UID");
  check(std::strcmp(identity.shortUid, "KTDEAD") == 0,
        "identity preserves the short UID");

  mesh::ClientIdentity unchanged = identity;
  check(mesh::makeClientIdentity("ktdead", identity) ==
            mesh::Status::InvalidShortUid &&
            std::memcmp(&identity, &unchanged, sizeof(identity)) == 0,
        "lowercase/invalid UID is rejected transactionally");
  check(mesh::makeClientIdentity(nullptr, identity) ==
            mesh::Status::NullArgument,
        "null UID is rejected");
  check(!mesh::validShortUid("KT123") &&
            !mesh::validShortUid("KT12345") &&
            !mesh::validShortUid("KT12G4"),
        "short UID requires exactly four uppercase hex digits");
}

void testLocationPrivacy() {
  mesh::Settings settings = mesh::defaultSettings();
  mesh::CurrentLocationOnce current{};
  mesh::AdvertLocation advertised{};
  check(settings.locationMode == mesh::LocationMode::Hidden &&
            !mesh::selectAdvertLocation(settings, current, advertised) &&
            advertised.source == mesh::LocationSource::None,
        "location is hidden by default");

  const mesh::Coordinates bucharest{44426300L, 26102600L};
  check(mesh::setFixedLocation(settings, bucharest) == mesh::Status::Ok &&
            mesh::selectAdvertLocation(settings, current, advertised) &&
            advertised.source == mesh::LocationSource::Fixed &&
            advertised.coordinates.latitudeE6 == bucharest.latitudeE6,
        "valid fixed location is selected");
  mesh::hideLocation(settings);
  check(settings.fixedLocation.latitudeE6 == 0 &&
            settings.fixedLocation.longitudeE6 == 0,
        "hiding location scrubs fixed coordinates");

  const mesh::Coordinates impossible{90000001L, 0};
  check(mesh::setFixedLocation(settings, impossible) ==
            mesh::Status::InvalidCoordinates &&
            settings.locationMode == mesh::LocationMode::Hidden,
        "invalid fixed location does not mutate settings");

  mesh::requestCurrentLocationOnce(settings);
  check(settings.locationMode == mesh::LocationMode::CurrentOnce &&
            !mesh::selectAdvertLocation(settings, current, advertised),
        "current-once mode needs a phone-supplied coordinate");
  check(mesh::stageCurrentLocationOnce(current, bucharest) ==
            mesh::Status::Ok &&
            mesh::selectAdvertLocation(settings, current, advertised) &&
            advertised.source == mesh::LocationSource::CurrentOnce,
        "staged phone location is exposed for one send");
  mesh::clearCurrentLocationOnce(current);
  check(!current.pending && current.coordinates.latitudeE6 == 0 &&
            !mesh::selectAdvertLocation(settings, current, advertised),
        "consuming current-once scrubs it from RAM");
}

void testRadioAndTxGate() {
  mesh::Settings settings = mesh::defaultSettings();
  check(mesh::validateSettings(settings) == mesh::Status::Ok &&
            !settings.enabled &&
            settings.radio.selected() &&
            settings.radio.profileId == mesh::kUkEuNarrowProfileId &&
            settings.radio.frequencyHz == 869618000UL &&
            settings.radio.bandwidthHz == 62500UL &&
            settings.radio.spreadingFactor == 8 &&
            settings.radio.codingRate == 5 &&
            settings.radio.syncWord == 0x12 &&
            settings.radio.txPowerDbm == 22 &&
            settings.radio.preambleSymbols == 32 &&
            settings.txPolicy == mesh::TxPolicy::Locked,
        "default config is disabled, UK/EU Narrow, and TX locked");
  check(std::strcmp(mesh::kUkEuNarrowProfileName, "UK/EU Narrow") == 0,
        "built-in profile has its pinned display name");

  mesh::TxGate gate;
  check(!gate.allowsTransmit(settings) &&
            !gate.unlockForSession(settings, true),
        "locked policy cannot be unlocked");

  settings.txPolicy = mesh::TxPolicy::ExplicitSession;
  settings.radio = mesh::RadioProfile{};
  check(!gate.unlockForSession(settings, true),
        "TX cannot unlock without a selected radio profile");
  settings.radio = euTestProfile();
  settings.enabled = true;
  check(mesh::validateSettings(settings) == mesh::Status::Ok,
        "complete 868 MHz profile validates");
  check(!gate.unlockForSession(settings, false) &&
            !gate.allowsTransmit(settings),
        "TX cannot unlock without explicit user approval");
  check(gate.unlockForSession(settings, true) &&
            gate.allowsTransmit(settings),
        "explicit session unlock opens the gate for the selected profile");

  mesh::Settings changed = settings;
  ++changed.radio.frequencyHz;
  check(!gate.allowsTransmit(changed),
        "profile changes invalidate an existing session unlock");
  gate.lock();
  check(!gate.allowsTransmit(settings), "manual gate lock is immediate");

  check(gate.unlockForSession(settings, true),
        "enabled config can establish another explicit TX session");
  settings.enabled = false;
  check(!gate.allowsTransmit(settings),
        "disabling the mesh closes an existing TX session logically");

  mesh::RadioProfile partial{};
  partial.frequencyHz = 868000000UL;
  check(mesh::validateRadioProfile(partial) ==
            mesh::Status::IncompleteRadioProfile,
        "partially populated unselected profile is rejected");
  mesh::RadioProfile invalid = euTestProfile();
  invalid.frequencyHz = 915000000UL;
  check(mesh::validateRadioProfile(invalid) ==
            mesh::Status::InvalidFrequency,
        "non-868 profile is rejected by the Kitsu868 build policy");
  invalid = euTestProfile();
  invalid.bandwidthHz = 12345UL;
  check(mesh::validateRadioProfile(invalid) ==
            mesh::Status::InvalidBandwidth,
        "unsupported SX1262 bandwidth is rejected");
}

void testPersistenceCodec() {
  mesh::Settings source{};
  source.enabled = true;
  source.txPolicy = mesh::TxPolicy::ExplicitSession;
  source.radio = euTestProfile();
  check(mesh::setFixedLocation(source, {44426300L, 26102600L}) ==
            mesh::Status::Ok,
        "persistence fixture location is valid");

  std::array<uint8_t, mesh::kPersistedSettingsBytes> encoded{};
  check(mesh::encodeSettings(source, encoded.data(), encoded.size()) ==
            mesh::Status::Ok,
        "valid settings encode");
  mesh::Settings decoded{};
  check(mesh::decodeSettings(encoded.data(), encoded.size(), decoded) ==
            mesh::Status::Ok &&
            decoded.locationMode == mesh::LocationMode::Fixed &&
            decoded.enabled &&
            decoded.fixedLocation.latitudeE6 == 44426300L &&
            decoded.fixedLocation.longitudeE6 == 26102600L &&
            decoded.txPolicy == mesh::TxPolicy::ExplicitSession &&
            mesh::sameRadioProfile(decoded.radio, source.radio),
        "persisted settings round-trip every durable field");

  for (size_t index = 0; index < encoded.size(); ++index) {
    auto corrupted = encoded;
    corrupted[index] ^= 0x01;
    mesh::Settings sentinel = decoded;
    sentinel.radio.profileId = 0xDEADBEEFUL;
    const mesh::Status result =
        mesh::decodeSettings(corrupted.data(), corrupted.size(), sentinel);
    if (result == mesh::Status::Ok ||
        sentinel.radio.profileId != 0xDEADBEEFUL) {
      ++failures;
      std::cerr << "FAIL corruption accepted/mutated output at byte "
                << index << '\n';
    }
  }

  mesh::Settings once = source;
  mesh::requestCurrentLocationOnce(once);
  check(mesh::encodeSettings(once, encoded.data(), encoded.size()) ==
            mesh::Status::Ok,
        "current-once settings encode without ephemeral coordinates");
  decoded = source;
  check(mesh::decodeSettings(encoded.data(), encoded.size(), decoded) ==
            mesh::Status::Ok &&
            decoded.locationMode == mesh::LocationMode::Hidden &&
            decoded.fixedLocation.latitudeE6 == 0 &&
            decoded.fixedLocation.longitudeE6 == 0,
        "current-once consent returns as hidden after restart");

  mesh::Settings residue{};
  residue.locationMode = mesh::LocationMode::Hidden;
  residue.fixedLocation = {1, 2};
  check(mesh::encodeSettings(residue, encoded.data(), encoded.size()) ==
            mesh::Status::LocationPrivacyResidue,
        "hidden mode cannot persist stale coordinates");
  check(mesh::decodeSettings(nullptr, encoded.size(), decoded) ==
            mesh::Status::NullArgument &&
            mesh::decodeSettings(encoded.data(), encoded.size() - 1, decoded) ==
                mesh::Status::WrongLength,
        "persistence decoder is strict about input and length");
}

void testHostStoreBoundary() {
  mesh::SettingsStore store;
  mesh::Settings settings{};
  check(store.load(settings) == mesh::StoreStatus::Unavailable &&
            store.save(settings) == mesh::StoreStatus::Unavailable,
        "host build cleanly excludes ESP32 Preferences access");
}

}  // namespace

int main() {
  testClientIdentity();
  testLocationPrivacy();
  testRadioAndTxGate();
  testPersistenceCodec();
  testHostStoreBoundary();

  if (failures != 0) {
    std::cerr << "TEST_FAIL kitsu_mesh_config failures=" << failures << '\n';
    return 1;
  }
  std::cout
      << "TEST_PASS kitsu_mesh_config role=client name=fox-kitsu-KTDEAD "
         "profile=uk-eu-narrow location=hidden-fixed-current-once "
         "tx=boot-locked "
         "persisted_bytes="
      << mesh::kPersistedSettingsBytes << " corruption_bytes="
      << mesh::kPersistedSettingsBytes << '\n';
  return 0;
}
