#include <Arduino.h>
#include <cstring>
#include <Preferences.h>
#include <SSD1306Wire.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>
#include <Wire.h>

#include "activity_suite.h"
#include "adventure_progression.h"
#include "companion_brain.h"
#include "companion_dialogue.h"
#include "companion_fun.h"
#include "companion_progression.h"
#include "companion_pack.h"
#include "companion_replacement_intent.h"
#include "encounter_protocol.h"
#include "expedition_core.h"
#include "kitsu_chat_contract.h"
#include "kitsu_clock.h"
#include "kitsu_ble_action.h"
#include "kitsu_ble_bond_recovery.h"
#include "kitsu_ble_gatt.h"
#include "kitsu_ble_ota.h"
#include "kitsu_ble_session.h"
#include "kitsu_device_security.h"
#include "kitsu_esp32_security.h"
#include "kitsu_flash_layout.h"
#include "kitsu_focus_session.h"
#include "kitsu_legacy_connectivity_retirement.h"
#include "kitsu_nvs_headroom.h"
#include "kitsu_nvs_erase_guard.h"
#include "kitsu_mesh_config.h"
#include "kitsu_message_read_contract.h"
#include "kitsu_mesh_transport.h"
#include "kitsu_nearby_protocol.h"
#include "kitsu_pet_presence.h"
#include "kitsu_pet_presentation.h"
#include "kitsu_party_hotspot.h"
#include "kitsu_party_modes.h"
#include "kitsu_rx_rearm_policy.h"
#include "kitsu_storage_retry.h"
#include "kitsu_transport_scope.h"
#include "kitsu_unlock_codes.h"
#include "mesh_discovery_journal.h"
#include "mini_games.h"
#include "portrait_font.h"
#include "portrait_ui_layout.h"
#include "signal_encounter.h"
#include "signal_trail.h"
#include "social_progression.h"
#include "wild_creature_catalog.h"
#include "wild_guide_portraits.h"

namespace {

constexpr char FIRMWARE_NAME[] = "Kitsu868";
#define KITSU_FIRMWARE_VERSION_LITERAL "0.20.5"
constexpr char FIRMWARE_VERSION[] = KITSU_FIRMWARE_VERSION_LITERAL;
// Signed update tooling locates exactly one copy of this fixed-format marker
// in the final ESP application.  Keeping it referenced by the boot diagnostic
// prevents linker garbage collection, while the compile-time layout assertions
// below bind the textual identity to the constants enforced at runtime.
constexpr char FIRMWARE_IDENTITY[] =
    "KITSU-ID1|schema=1|length=0331|version="
    KITSU_FIRMWARE_VERSION_LITERAL
    "|device_class=heltec-v3.2|layout=kitsu-8m-dual-ota-3m-v1"
    "|flash=00800000|nvs=00009000/00040000"
    "|otadata=00049000/00002000"
    "|app0=00050000|app1=00350000|slot=00300000"
    "|journal=00001000|max=002ff000"
    "|spiffs=00670000/00140000|conn=007b0000/00040000"
    "|coredump=007f0000/00010000|crc32=2275f192|end";
static_assert(sizeof(FIRMWARE_IDENTITY) == 331U,
              "firmware identity length field changed");
static_assert(kitsu868::connectivity::kKitsuFlashBytes == 0x800000UL,
              "firmware identity flash size changed");
static_assert(kitsu868::connectivity::kKitsuNvsOffset == 0x009000UL &&
                  kitsu868::connectivity::kKitsuNvsBytes == 0x040000UL,
              "firmware identity NVS geometry changed");
static_assert(kitsu868::connectivity::kKitsuOtaDataOffset == 0x049000UL &&
                  kitsu868::connectivity::kKitsuOtaDataBytes == 0x002000UL,
              "firmware identity OTA-data geometry changed");
static_assert(kitsu868::connectivity::kKitsuApp0Offset == 0x050000UL,
              "firmware identity app0 address changed");
static_assert(kitsu868::connectivity::kKitsuApp1Offset == 0x350000UL,
              "firmware identity app1 address changed");
static_assert(kitsu868::connectivity::kKitsuAppPartitionBytes == 0x300000UL,
              "firmware identity slot size changed");
static_assert(kitsu868::connectivity::kBleOtaJournalBytes == 0x1000UL,
              "firmware identity journal size changed");
static_assert(kitsu868::connectivity::kBleOtaMaximumImageBytes == 0x2ff000UL,
              "firmware identity image limit changed");
static_assert(kitsu868::connectivity::kKitsuSpiffsOffset == 0x670000UL &&
                  kitsu868::connectivity::kKitsuSpiffsBytes == 0x140000UL,
              "firmware identity SPIFFS geometry changed");
static_assert(
    kitsu868::connectivity::kKitsuConnectivityOffset == 0x7b0000UL &&
        kitsu868::connectivity::kKitsuConnectivityBytes == 0x040000UL,
    "firmware identity connectivity geometry changed");
static_assert(kitsu868::connectivity::kKitsuCoredumpOffset == 0x7f0000UL &&
                  kitsu868::connectivity::kKitsuCoredumpBytes == 0x010000UL,
              "firmware identity coredump geometry changed");
#undef KITSU_FIRMWARE_VERSION_LITERAL
constexpr uint32_t LEGACY_STATE_MAGIC = 0x57535031;
constexpr uint32_t CORE_STATE_MAGIC = 0x4b433732;  // "KC72"
constexpr uint32_t SIGNAL_STATE_MAGIC = 0x4b534731;  // "KSG1"
constexpr uint32_t SIGNAL_STATE_V2_MAGIC = 0x4b534732;  // "KSG2"
constexpr uint32_t FUN_STATE_MAGIC = 0x4b465531;  // "KFU1"
constexpr uint32_t PENDING_WILD_STATE_MAGIC = 0x4b505731;  // "KPW1"
constexpr uint32_t DIALOGUE_STATE_MAGIC = 0x4b444731;  // "KDG1"
constexpr uint32_t PARTY_REWARD_STATE_MAGIC = 0x4b505231;  // "KPR1"
constexpr uint32_t CLOCK_NTP_STABILITY_MS = 2000UL;
constexpr uint32_t CLOCK_NTP_RESYNC_MS = 6UL * 60UL * 60UL * 1000UL;
constexpr uint64_t CLOCK_SYNC_TOLERANCE_SECONDS = 2ULL;

constexpr int16_t UI_WIDTH = 64;
constexpr int16_t UI_HEIGHT = 128;
static_assert(UI_WIDTH == kitsu868::portrait::kCanvasWidth,
              "portrait layout width mismatch");
static_assert(UI_HEIGHT == kitsu868::portrait::kCanvasHeight,
              "portrait layout height mismatch");

// Heltec WiFi LoRa 32 V3.2 pin map.
constexpr uint8_t PIN_BUTTON = 0;       // PRG, active-low, also boot strap
constexpr uint8_t PIN_VEXT = 36;        // peripheral rail, active-low
constexpr uint8_t PIN_OLED_SDA = 17;
constexpr uint8_t PIN_OLED_SCL = 18;
constexpr uint8_t PIN_OLED_RST = 21;
constexpr uint8_t PIN_BATTERY_ADC = 1;
constexpr uint8_t PIN_BATTERY_CTRL = 37;  // Divider enable, active-low

constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t BUTTON_HOLD_MS = 750;
constexpr uint32_t CONTROLLER_RECOVERY_HOLD_MS = 5000UL;
constexpr uint32_t CONTROLLER_RECOVERY_CONFIRM_TIMEOUT_MS = 15000UL;
constexpr uint32_t CONTROLLER_RECOVERY_BROWSE_TIMEOUT_MS = 30000UL;
constexpr uint32_t CONTROLLER_RECOVERY_RESULT_TIMEOUT_MS = 8000UL;
constexpr uint32_t SCREEN_TIMEOUT_MS = 9000;
constexpr uint32_t LISTEN_TIME_MS = 60UL * 1000UL;
constexpr uint32_t ENERGY_TICK_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t BRAIN_MINUTE_MS = 60UL * 1000UL;
constexpr uint32_t BATTERY_SAMPLE_MS = 60UL * 1000UL;
constexpr uint32_t AWAKE_DISPLAY_SLEEP_MS = 2UL * 60UL * 1000UL;
constexpr uint32_t DREAM_DISPLAY_SLEEP_MS = 20UL * 1000UL;
constexpr uint32_t DREAM_MINIMUM_SLEEP_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t MOMENT_DISPLAY_MS = 6500UL;
constexpr uint32_t MESH_INTRODUCE_MIN_INTERVAL_MS = 1000UL;
constexpr uint32_t DISCOVERY_JOURNAL_DEBOUNCE_MS = 5000UL;
constexpr uint32_t DISCOVERY_URGENT_DEFER_MS = 250UL;
constexpr uint32_t STORAGE_HEADROOM_RECHECK_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t BLE_REFRESH_INTERVAL_MS = 30UL * 1000UL;
constexpr uint32_t BLE_REFRESH_RETRY_MS = 1000UL;
constexpr uint32_t FOCUS_TICK_INTERVAL_MS = 1000UL;
constexpr uint32_t FOCUS_CHECKPOINT_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t NEARBY_NEIGHBOR_TTL_MS = 2UL * 60UL * 1000UL;
constexpr uint32_t NEARBY_PRESENCE_RETRY_MS = 1000UL;
constexpr uint32_t NEARBY_PRESENCE_HEARTBEAT_MIN_MS = 5500UL;
constexpr uint32_t NEARBY_PRESENCE_HEARTBEAT_MAX_MS = 7000UL;
constexpr uint32_t PARTY_LISTEN_TIME_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t PARTY_BEACON_INTERVAL_MS = 5000UL;
constexpr uint32_t PARTY_JOIN_RETRY_MS = 5000UL;
constexpr uint32_t PARTY_REPLAY_INTERVAL_MS = 1500UL;
constexpr uint32_t PARTY_REWARD_RETRY_MS = 10000UL;
constexpr uint32_t PARTY_DISCOVERY_TTL_MS = 2UL * 60UL * 1000UL;
constexpr uint32_t PARTY_HOST_PACKET_GRACE_MS = 10000UL;
constexpr uint16_t PARTY_LOBBY_WINDOW_SECONDS = 600U;
constexpr uint16_t PARTY_ROUND_WINDOW_SECONDS = 45U;
constexpr uint8_t PARTY_PACKET_REPLAYS = 2U;
static_assert(kitsu868::mesh::kMeshChannelCapacity == 4U,
              "companion channel contract requires four slots");

SSD1306Wire display(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);
Preferences preferences;
CompanionPack companionPack;
kitsu868::presentation::PetPresentationPreview petPresentationPreview;
kitsu868::CompanionBrain companionBrain;
kitsu868::SignalCatchGame signalCatchGame;
kitsu868::PounceFetchGame pounceFetchGame;
kitsu868::EchoBeatGame echoBeatGame;
kitsu868::activities::ActivitySuite activitySuite;
kitsu868::focus::FocusSession focusSession;
kitsu868::timekeeping::KitsuClock kitsuClock;
kitsu868::timekeeping::OneButtonClockEditor clockEditor;
kitsu868::mesh::KitsuMeshTransport meshTransport;
kitsu868::mesh::SettingsStore meshSettingsStore;
kitsu868::mesh::Settings meshSettings = kitsu868::mesh::defaultSettings();
kitsu868::mesh::ClientIdentity meshIdentity{};
kitsu868::mesh::CurrentLocationOnce meshCurrentLocation{};
kitsu868::mesh::TransportStatus meshInitStatus =
    kitsu868::mesh::TransportStatus::Disabled;
kitsu868::unlocks::CodeStore encounterCodes;

kitsu868::signal::Configuration makeSignalEncounterConfiguration() {
  kitsu868::signal::Configuration configuration{};
  // Repeater discovery is guaranteed by the coordinator. Other values are
  // the chance per successful, deduplicated logical operation. The appended
  // nearby-Kitsu trigger is deliberately lower than every ordinary MeshCore
  // activity and never travels through MeshCore.
  const uint16_t encounterChances[] = {
      0U, 1500U, 1200U, 1500U, 800U, 500U, 500U, 300U};
  // Common through Mythical. Mythical is 0.5%, below the 1% product cap.
  const uint16_t rarityWeights[] = {
      5500U, 2500U, 1200U, 500U, 200U, 50U, 50U};
  // Every catalog tier has a real publishable .k868 pack. Code resolution is
  // still independent of the encounter roll, with rarer creatures more
  // likely to resolve their install code after they are actually encountered.
  const uint16_t codeChances[] = {
      1000U, 1200U, 1500U, 2000U, 3000U, 5000U, 10000U};
  for (size_t index = 0U; index < kitsu868::signal::kMeshOperationKindCount;
       ++index) {
    configuration.encounterChanceBasisPoints[index] = encounterChances[index];
  }
  for (size_t index = 0U; index < kitsu868::signal::kRarityCount; ++index) {
    configuration.rarityWeightBasisPoints[index] = rarityWeights[index];
    configuration.codeChanceBasisPoints[index] = codeChances[index];
  }
  return configuration;
}

const kitsu868::signal::Configuration SIGNAL_ENCOUNTER_CONFIGURATION =
    makeSignalEncounterConfiguration();
kitsu868::signal::SignalEncounterCoordinator signalEncounterCoordinator(
    SIGNAL_ENCOUNTER_CONFIGURATION);
kitsu868::signal::SignalTrail signalTrail;
kitsu868::connectivity::Esp32DeviceSecurityStorage connectivityStorage;
kitsu868::connectivity::Esp32DeviceSecurityPlatform connectivityPlatform;
kitsu868::connectivity::KitsuDeviceSecurity deviceSecurity;
kitsu868::connectivity::Esp32LegacyConnectivityRetirementPlatform
    legacyConnectivityRetirementPlatform;
kitsu868::connectivity::Esp32NvsHeadroomPlatform pairingNvsHeadroomPlatform;
kitsu868::connectivity::Esp32FlashLayoutPlatform flashLayoutPlatform;
kitsu868::connectivity::Esp32KitsuBleOtaPlatform bleOtaPlatform;
kitsu868::connectivity::KitsuBleOta bleOta;
kitsu868::connectivity::BleActionReplayCache bleActionReplayCache;
alignas(4) uint8_t bleActionReplayScratch[
    kitsu868::connectivity::kBleActionReplaySerializedBytes]{};
kitsu868::connectivity::Esp32JournalStorage discoveryStorage;
kitsu868::connectivity::Esp32JournalCrypto discoveryCrypto;
kitsu868::discovery::MeshDiscoveryJournal discoveryJournal;
bool connectivitySecurityReady = false;
bool legacyConnectivityRetirementReady = false;
bool bleOtaReady = false;
bool bleActionReplayReady = false;
bool discoveryJournalReady = false;
uint32_t discoveryBootId = 0U;
uint32_t discoveryJournalDirtyAt = 0U;
kitsu868::connectivity::StorageRetrySchedule discoveryStorageRetry;
kitsu868::connectivity::StorageRetrySchedule brainStorageRetry;
uint32_t discoveryHeadroomRecheckAt = 0U;
bool backgroundStorageTransactionUsed = false;

enum class Screen : uint8_t {
  Pet,
  Menu,
  Connect,
  Inbox,
  GameMenu,
  Game,
  Listen,
  Sleep,
  Status,
  PairPhone,
  ControllerManager,
  ControllerConfirm,
  ControllerResult,
  WildEncounter,
  FieldGuide,
  Goals,
  Clock,
  Adventure,
  Activity,
};
static_assert(static_cast<uint8_t>(Screen::Pet) ==
                  static_cast<uint8_t>(
                      kitsu868::presentation::Surface::Pet) &&
              static_cast<uint8_t>(Screen::Activity) ==
                  static_cast<uint8_t>(
                      kitsu868::presentation::Surface::Activity),
              "presentation surface mapping changed");

enum class ConnectionAction : uint8_t {
  Bluetooth = 0,
  PairCaretaker,
  Controllers,
  Back,
};

enum class ControllerRecoveryResult : uint8_t {
  None = 0,
  RemovedSlot,
  RemovedAll,
  Unchanged,
  StorageNeedsReboot,
  BleBondsCleared,
  BleBondStoreError,
  ControllerAuthorityChanged,
};

enum class ActiveGame : uint8_t { None, SignalCatch, PounceFetch, EchoBeat };

struct WispState {
  String uid;
  uint8_t energy = 72;
  uint8_t curiosity = 14;
  uint8_t affection = 5;
  uint32_t pets = 0;
  uint32_t boots = 0;
  bool sleeping = false;
};

#pragma pack(push, 1)
struct CoreStateV2 {
  uint32_t magic = CORE_STATE_MAGIC;
  uint16_t bytes = 37;
  uint16_t reserved = 0;
  char uid[7]{};
  uint8_t energy = 72;
  uint8_t curiosity = 14;
  uint8_t affection = 5;
  uint8_t sleeping = 0;
  uint8_t lastTrait = 0xff;
  uint8_t lastGift = 0xff;
  uint32_t pets = 0;
  uint32_t collectiblePackId = 0;
  uint16_t unlockedTraits = 0;
  uint16_t collectedGifts = 0;
  uint32_t crc32 = 0;
};

struct SignalStateV1 {
  uint32_t magic = SIGNAL_STATE_MAGIC;
  uint16_t schema = 1U;
  uint16_t bytes = 0U;
  uint64_t lastOperationId = 0U;
  uint8_t hasLastRecord = 0U;
  uint8_t operationKind = 0U;
  uint8_t encounterOccurred = 0U;
  uint8_t guaranteed = 0U;
  uint8_t rarity = 0U;
  uint8_t codeOutcome = 0U;
  uint16_t encounterRoll = 0U;
  uint16_t rarityRoll = 0U;
  uint16_t codeRoll = 0U;
  uint64_t recordOperationId = 0U;
  uint32_t entropy = 0U;
  uint32_t crc32 = 0U;
};

struct SignalStateV2 {
  uint32_t magic = SIGNAL_STATE_V2_MAGIC;
  uint16_t schema = 2U;
  uint16_t bytes = 0U;
  uint64_t lastOperationId = 0U;
  uint8_t hasLastRecord = 0U;
  uint8_t operationKind = 0U;
  uint8_t encounterOccurred = 0U;
  uint8_t guaranteed = 0U;
  uint8_t rarity = 0U;
  uint8_t codeOutcome = 0U;
  uint16_t encounterRoll = 0U;
  uint16_t rarityRoll = 0U;
  uint16_t codeRoll = 0U;
  uint64_t recordOperationId = 0U;
  uint32_t entropy = 0U;
  uint8_t trailSchema = kitsu868::signal::kSignalTrailStateSchemaVersion;
  uint8_t trailMissCount = 0U;
  uint8_t trailHasLastOperation = 0U;
  uint8_t trailReserved = 0U;
  uint64_t trailLastOperationId = 0U;
  uint32_t crc32 = 0U;
};

struct FunStateV1 {
  uint32_t magic = FUN_STATE_MAGIC;
  uint16_t schema = 1U;
  uint16_t bytes = 0U;
  uint32_t seenMask = 0U;
  uint16_t encounterCounts[kitsu868::fun::kCatalogCreatureCount]{};
  uint8_t lastSources[kitsu868::fun::kCatalogCreatureCount]{};
  uint16_t completedDreams = 0U;
  uint16_t rareReactions = 0U;
  uint8_t dreamHistory[kitsu868::fun::kDreamHistoryCapacity]{};
  uint8_t dreamHead = 0U;
  uint8_t dreamHistoryCount = 0U;
  uint64_t lastEncounterOperationId = 0U;
  uint32_t crc32 = 0U;
};

struct PendingWildStateV1 {
  uint32_t magic = PENDING_WILD_STATE_MAGIC;
  uint16_t schema = 1U;
  uint16_t bytes = 0U;
  uint64_t operationId = 0U;
  uint32_t entropy = 0U;
  uint32_t packId = 0U;
  uint8_t active = 0U;
  uint8_t source = 0U;
  uint8_t rarity = 0U;
  uint8_t codeOutcome = 0U;
  uint8_t guaranteed = 0U;
  uint8_t reserved[3]{};
  uint32_t crc32 = 0U;
};
#pragma pack(pop)

static_assert(sizeof(CoreStateV2) == 37, "core persistence layout changed");
static_assert(sizeof(SignalStateV1) == 44,
              "signal encounter persistence layout changed");
static_assert(sizeof(SignalStateV2) == 56,
              "signal trail persistence layout changed");
static_assert(sizeof(FunStateV1) == 101,
              "fun persistence layout changed");
static_assert(sizeof(PendingWildStateV1) == 36,
              "pending wild encounter persistence layout changed");

// These two records wrap storage-free semantic state owned by the new fun
// modules. They remain separate from core care state so a damaged optional
// feature never prevents the companion itself from booting.
#pragma pack(push, 1)
struct DialogueStateV1 {
  uint32_t magic = DIALOGUE_STATE_MAGIC;
  uint16_t schema = 1U;
  uint16_t bytes = 0U;
  uint32_t actionSelections = 0U;
  uint16_t actionRecent[kitsu868::dialogue::kActionRecentCapacity]{};
  uint8_t actionRecentHead = 0U;
  uint8_t actionRecentCount = 0U;
  uint32_t storyStarts = 0U;
  uint32_t storyCompletedMask = 0U;
  uint16_t storyCompletions = 0U;
  uint8_t activeStory = kitsu868::dialogue::kNoActiveStory;
  uint8_t storyScene = 0U;
  uint8_t storyRecent[kitsu868::dialogue::kStoryRecentCapacity]{};
  uint8_t storyRecentHead = 0U;
  uint8_t storyRecentCount = 0U;
  uint32_t lastClaimedExpeditionId = 0U;
  uint32_t crc32 = 0U;
};

struct PartyRewardStateV1 {
  uint32_t magic = PARTY_REWARD_STATE_MAGIC;
  uint16_t schema = 1U;
  uint16_t bytes = 0U;
  kitsu868::party::RewardState rewards{};
  uint32_t crc32 = 0U;
};
#pragma pack(pop)

static_assert(sizeof(DialogueStateV1) == 47U,
              "dialogue persistence layout changed");
static_assert(sizeof(kitsu868::party::PeerRewardState) == 12U,
              "party peer persistence layout changed");
static_assert(sizeof(kitsu868::party::RewardState) == 212U,
              "party reward semantic layout changed");
static_assert(sizeof(PartyRewardStateV1) == 224U,
              "party reward persistence layout changed");

WispState wisp;
Screen screen = Screen::Pet;
const char* const MENU_ITEMS[] = {
    "CONNECT", "FEED", "PLAY", "GAMES", "ADVENTURE", "CREATURES",
    "GOALS", "INBOX", "RADIO", "CLOCK", "SLEEP", "INFO", "BACK"};
const char* const GAME_ITEMS[] = {
    "SIGNAL", "POUNCE", "ECHO", "MORSE", "TUNER", "FLASH",
    "STEADY", "BREATHE", "DAILY", "GHOST", "BACK"};
const char* const ADVENTURE_ITEMS[] = {
    "SHORT TRIP", "MEDIUM TRIP", "LONG TRIP", "START ROUTE",
    "+250 STEPS", "CONTINUE", "DETOUR", "HELP", "RETURN EARLY",
    "TERRAIN", "OBJECTIVE", "RISK", "WEATHER", "REPORT", "BACK"};
uint8_t menuIndex = 0;
uint8_t gameMenuIndex = 0;
uint8_t statusPage = 0;
uint8_t fieldGuideIndex = 0;
ConnectionAction connectionAction = ConnectionAction::Bluetooth;
kitsu868::connectivity::ControllerRole pairingScreenRole =
    kitsu868::connectivity::ControllerRole::Owner;
ActiveGame activeGame = ActiveGame::None;
bool gameRewarded = false;
bool gameEvolved = false;
bool pendingEvolutionReaction = false;
uint8_t gamePerfectRounds = 0;
kitsu868::MiniGamePhase lastGamePhase = kitsu868::MiniGamePhase::Inactive;

bool firstHatch = false;
bool oledDetected = false;
bool storageReady = false;
bool radioReady = false;
bool radioListening = false;
// The legacy 19-byte encounter frame remains available only through the
// serial `inject` QA command.  It is never transmitted on MeshCore.
constexpr bool ENCOUNTER_TX_ENABLED = false;
int16_t radioInitCode = 0;

String lastMemory = "A quiet signal awakens.";
float lastRssi = 0.0f;
float lastSnr = 0.0f;
bool hasEncounterDuplicate = false;
kitsu868::encounter::DuplicateToken lastEncounterDuplicate{};
uint16_t lastPeerUid = 0;
uint8_t lastEncounterTrait = 0xff;
uint8_t lastEncounterGift = 0xff;
uint16_t unlockedTraits = 0;
uint16_t collectedGifts = 0;
uint32_t collectiblePackId = 0;
bool radioProgressDirty = false;

struct WildEncounterView {
  bool available = false;
  bool codeRevealed = false;
  bool guaranteed = false;
  kitsu868::wild::Creature creature{};
  kitsu868::signal::MeshOperationKind source =
      kitsu868::signal::MeshOperationKind::OtherCompleted;
};

WildEncounterView wildEncounterView{};
uint64_t pendingWildOperationId = 0U;
uint32_t pendingWildEntropy = 0U;
kitsu868::signal::CodeOutcome pendingWildCodeOutcome =
    kitsu868::signal::CodeOutcome::NotApplicable;
bool pendingWildMaterialized = false;

constexpr uint8_t NEARBY_NEIGHBOR_CAPACITY = 8U;
struct NearbyNeighbor {
  bool used = false;
  uint16_t uid = 0U;
  uint32_t sessionNonce = 0U;
  uint32_t packId = 0U;
  uint8_t appearance = 0U;
  uint8_t evolutionStage = 0U;
  uint8_t bond = 0U;
  uint8_t mood = 0U;
  uint8_t emote = 0U;
  float rssi = 0.0f;
  float snr = 0.0f;
  uint32_t lastSeenAt = 0U;
  uint32_t lastAcceptedActionAt = 0U;
  uint16_t nextSequence = 1U;
  kitsu868::nearby::DuplicateToken lastInbound{};
  bool hasInboundResult = false;
  kitsu868::nearby::ActionResult lastInboundResult =
      kitsu868::nearby::ActionResult::None;
};

NearbyNeighbor nearbyNeighbors[NEARBY_NEIGHBOR_CAPACITY]{};
kitsu868::presence::PetPresenceTracker petPresenceTracker;
uint32_t nextNearbyPresenceAt = 0U;
uint32_t nearbySessionNonce = 0U;
uint16_t nearbySequenceCursor = 1U;
bool nearbySequenceReady = false;
struct PendingNearbyAction {
  bool active = false;
  kitsu868::nearby::Packet request{};
  uint32_t sentAt = 0U;
};
PendingNearbyAction pendingNearbyAction{};
uint32_t lastSignaledFloodAdvert = 0U;
uint32_t lastSignaledNearbyAdvert = 0U;
constexpr char UNLOCK_CODES_STORAGE_KEY[] = "unlock_v2";
constexpr char LEGACY_UNLOCK_CODES_STORAGE_KEY[] = "unlock_v1";
alignas(4) uint8_t encounterCodeScratch[
    kitsu868::unlocks::kStoreSerializedBytes]{};
alignas(4) uint8_t encounterCodeRollback[
    kitsu868::unlocks::kStoreSerializedBytes]{};
bool encounterCodesReady = false;
bool signalEncounterStateReady = false;
bool funStateReady = false;
kitsu868::fun::DiscoveryState funDiscovery{};
uint64_t lastFunEncounterOperationId = 0U;
kitsu868::fun::SessionChallenges sessionChallenges{};
bool sessionAuraActive = false;

kitsu868::dialogue::ActionState dialogueActions{};
kitsu868::dialogue::StoryState dialogueStories{};
kitsu868::expedition::ExpeditionCore expeditionCore;
kitsu868::progression::CompanionProgression companionProgression;
kitsu868::adventure::AdventureProgression adventureProgression;
kitsu868::social::SocialProgression socialProgression;
kitsu868::party::HostSession partyHost;
kitsu868::party::ParticipantSession partyParticipant;
kitsu868::party_modes::HostSession modePartyHost;
kitsu868::party_modes::ParticipantSession modePartyParticipant;
kitsu868::party::PartyRewardLedger partyRewards;
kitsu868::dialogue::StoryResolution lastStoryResolution{};
kitsu868::party::RewardOutcome lastPartyReward{};
bool storyResolutionAvailable = false;
bool partyRewardAvailable = false;
uint32_t lastClaimedExpeditionId = 0U;
bool dialogueStateReady = false;
bool expeditionStateReady = false;
bool partyRewardStateReady = false;
bool clockStateReady = false;
bool companionProgressionReady = false;
bool adventureProgressionReady = false;
bool socialProgressionReady = false;
bool activityStateReady = false;
bool focusStateReady = false;
bool focusPersistPending = false;
bool activityResumePending = false;
uint32_t focusNextTickAt = 0U;
uint32_t focusPersistRetryAt = 0U;
uint32_t focusPersistedCheckpoint = 0U;
uint32_t clockSnapshotGeneration = 0U;
uint8_t clockSnapshotSlot = 0U;
bool clockNtpStarted = false;
bool clockNtpCandidate = false;
uint32_t clockNtpCandidateAt = 0U;
int64_t clockNtpCandidateEpoch = 0;
uint32_t clockNtpLastAcceptedAt = 0U;
uint8_t adventureMenuIndex = 0U;
kitsu868::adventure::Terrain adventureTerrain =
    kitsu868::adventure::Terrain::Meadow;
kitsu868::adventure::Objective adventureObjective =
    kitsu868::adventure::Objective::Explore;
kitsu868::adventure::Risk adventureRisk =
    kitsu868::adventure::Risk::Balanced;
kitsu868::adventure::Weather adventureWeather =
    kitsu868::adventure::Weather::Unknown;
bool activityRewarded = false;
bool activityPressAccepted = false;
uint32_t progressionLastSessionDay = 0U;
uint16_t progressionLastMinute = UINT16_MAX;
constexpr uint8_t PROGRESSION_LINE_CAPACITY = 10U;
kitsu868::progression::DisplayLine
    progressionPendingLines[PROGRESSION_LINE_CAPACITY]{};
uint8_t progressionPendingLineHead = 0U;
uint8_t progressionPendingLineCount = 0U;
uint32_t lastPartyBeaconAt = 0U;
uint32_t lastPartyTxAt = 0U;
uint32_t lastPartyRewardAttemptAt = 0U;
uint32_t lastPartyRewardSessionNonce = 0U;
uint32_t lastPartyCelebratedSessionNonce = 0U;
uint32_t lastPartyBeaconTxAt = 0U;
bool partyScanActive = false;
bool partyJoinRequested = false;
bool partyWelcomeAccepted = false;
kitsu868::party::Packet partyReplayPacket{};
uint8_t partyReplayRemaining = 0U;
uint32_t partyReplayAt = 0U;
kitsu868::party_modes::Packet modePartyReplayPacket{};
uint8_t modePartyReplayRemaining = 0U;
uint32_t modePartyReplayAt = 0U;
bool modePartyScanActive = false;
bool modePartyJoinRequested = false;
bool modePartyAutoReadyPending = false;
uint32_t lastModePartyHandledSessionNonce = 0U;
uint32_t lastModePartyBeaconTxAt = 0U;
uint32_t lastModePartyTxAt = 0U;
float partyHostRssi = 0.0f;
float partyHostSnr = 0.0f;

struct MomentView {
  bool active = false;
  const char* line1 = nullptr;
  const char* line2 = nullptr;
  uint32_t until = 0U;
};
MomentView momentView{};
uint32_t sleepStartedAt = 0U;
uint32_t nextRareReactionAt = 0U;

struct BatteryState {
  uint16_t millivolts = 0;
  uint8_t percent = 0xff;
  bool present = false;
};

BatteryState battery;
bool displaySleeping = false;

struct ActiveAnimation {
  bool active = false;
  bool finite = false;
  CompanionRole role = CompanionRole::Idle;
  CompanionRole clipRole = CompanionRole::Idle;
  PackPlayback mode = PackPlayback::Hold;
  uint32_t token = 0;
  uint32_t startedAt = 0;
  uint32_t spanMs = 0;
};
static_assert(static_cast<uint8_t>(CompanionRole::Idle) ==
                  static_cast<uint8_t>(
                      kitsu868::presentation::Role::Idle) &&
              static_cast<uint8_t>(CompanionRole::Evolve) ==
                  static_cast<uint8_t>(
                      kitsu868::presentation::Role::Evolve) &&
              static_cast<uint8_t>(PackPlayback::Hold) ==
                  static_cast<uint8_t>(
                      kitsu868::presentation::Playback::Hold) &&
              static_cast<uint8_t>(PackPlayback::PingPong) ==
                  static_cast<uint8_t>(
                      kitsu868::presentation::Playback::PingPong),
              "presentation animation mapping changed");

uint32_t screenEnteredAt = 0;
uint32_t listenUntil = 0;
uint32_t nextAmbientAnimationAt = 0;
uint32_t animationToken = 0;
ActiveAnimation activeAnimation;
const uint8_t* lastRenderedPetFrame = nullptr;
size_t lastRenderedPetFrameBytes = 0U;
uint32_t lastRenderedPetFrameAt = 0U;
Screen lastRenderedPetFrameSurface = Screen::Pet;
uint32_t lastRenderedPetAnimationToken = 0U;
uint32_t petPresentationOpenedAt = 0U;
uint32_t lastRenderAt = 0;
uint32_t lastEnergyTickAt = 0;
uint32_t lastBrainMinuteAt = 0;
uint8_t brainMinutesSinceFlush = 0;
uint32_t lastBatterySampleAt = 0;
uint32_t lastInteractionAt = 0;
uint32_t lastMeshIntroduceAt = 0;
bool hasMeshIntroduced = false;

enum class ChatJournalState : uint8_t {
  Received = 0,
  Queued,
  Sent,
  Delivered,
  Unconfirmed,
  Failed,
  Cancelled,
};

struct ChatJournalEntry {
  uint32_t id = 0;
  // Entries retain their journal generation so a uint32 ID/revision rollover
  // can never relabel old rows under the new response-level session.
  uint32_t journalSession = 0;
  // Stable message identity and mutable record revision are deliberately
  // separate. Delivery transitions keep id unchanged and advance revision.
  uint32_t revision = 0;
  uint32_t timestamp = 0;
  uint32_t expectedAck = 0;
  uint32_t queuedAt = 0;
  uint32_t sentAt = 0;
  bool inbound = true;
  kitsu868::mesh::MessageKind kind = kitsu868::mesh::MessageKind::Direct;
  kitsu868::mesh::MessageRoute route = kitsu868::mesh::MessageRoute::Flood;
  ChatJournalState state = ChatJournalState::Received;
  bool authenticated = false;
  bool unread = false;
  bool repeaterCountKnown = false;
  uint8_t repeaterCount = 0;
  // Outbound channel rebroadcast copies observed locally through MeshCore's
  // pre-dedup receive hook. Not a unique-repeater or delivery count.
  bool repeatCountKnown = false;
  uint8_t repeatCount = 0;
  bool repeatObservationOpen = false;
  uint8_t repeatSourceCount = 0U;
  kitsu868::mesh::ChannelRepeatSource
      repeatSources[kitsu868::mesh::kChannelRepeatSourceCapacity]{};
  bool repeatSourcesTruncated = false;
  uint8_t contactPublicKey[32]{};
  uint8_t channelSlot = 0xff;
  char senderName[33]{};
  char text[kitsu868::mesh::kMeshTextCapacity]{};
  float rssi = 0.0f;
  float snr = 0.0f;
};

ChatJournalEntry chatJournal[kitsu868::chat::kInboxCapacity]{};
static_assert(kitsu868::chat::kInboxCapacity ==
                  kitsu868::message_read::kMaximumMessageIds,
              "BLE read batch must cover the bounded message journal");
static_assert(kitsu868::chat::kInboxCapacity ==
                  kitsu868::mesh::kChannelRepeatTrackerCapacity,
              "every visible channel row needs a repeat lifecycle slot");
uint8_t chatJournalStart = 0;
uint8_t chatJournalCount = 0;
uint32_t chatJournalDropped = 0;
uint32_t nextChatMessageId = 1;
uint32_t chatJournalRevision = 0;
uint32_t chatSession = 0;
uint8_t unreadChatMessages = 0;
uint8_t inboxSelection = 0;
bool pendingChatReaction = false;
bool companionBleRefreshDirty = true;
uint32_t companionBleRefreshAt = 0U;
uint32_t companionBleRefreshSequence = 0U;

bool rawButton = false;
bool stableButton = false;
bool buttonHoldConsumed = false;
uint32_t buttonChangedAt = 0;
uint32_t buttonPressedAt = 0;
constexpr uint8_t CONTROLLER_RECOVERY_RESET_INDEX =
    kitsu868::connectivity::kKitsuControllerCapacity;
constexpr uint8_t CONTROLLER_RECOVERY_BLE_BONDS_INDEX =
    CONTROLLER_RECOVERY_RESET_INDEX + 1U;
constexpr uint8_t CONTROLLER_RECOVERY_BACK_INDEX =
    CONTROLLER_RECOVERY_BLE_BONDS_INDEX + 1U;
constexpr uint8_t CONTROLLER_RECOVERY_OPTION_COUNT =
    CONTROLLER_RECOVERY_BACK_INDEX + 1U;
static_assert(kitsu868::connectivity::kKitsuControllerCapacity == 4U,
              "physical controller manager requires four bounded slots");
uint8_t controllerRecoverySelection = 0U;
uint8_t controllerRecoveryTargetSlot = 0U;
uint8_t controllerRecoveryTargetId[
    kitsu868::connectivity::kKitsuControllerIdBytes]{};
uint8_t controllerRecoveryOriginalCount = 0U;
uint32_t controllerRecoveryDeadline = 0U;
ControllerRecoveryResult controllerRecoveryResult =
    ControllerRecoveryResult::None;
kitsu868::connectivity::ControllerAuthoritySnapshot
    controllerRecoveryAuthoritySnapshot{};
bool controllerRecoveryAuthoritySnapshotValid = false;
String serialLine;
bool serialOverflow = false;
bool serialControlRejected = false;

__attribute__((noinline)) bool handleCompanionBleRequest(
    const kitsu868::companion::DecodedEnvelope& request,
    const uint8_t* payload, size_t payloadBytes, uint8_t* responsePayload,
    size_t responseCapacity, size_t& responseBytes);
bool petWisp();
bool feedKitsu();
bool playKitsu();
bool saveState();
uint8_t addClampedStat(uint8_t value, int8_t delta);
uint16_t ownUidSuffix();
bool startListening(uint32_t durationMs = LISTEN_TIME_MS);
bool sendNearbyPresence();
void processNearbyRadio();
void tickPetPresence(uint32_t now);
bool startExpedition(kitsu868::expedition::Duration duration,
                     const char*& error);
bool claimExpedition(const char*& error);
void tickExpedition(uint32_t now);
void tickPartyHotspot(uint32_t now);
void tickModeParty(uint32_t now);
bool startPartyScan(const char*& error);
bool startPartyHost(const char*& error);
bool joinObservedParty(uint16_t hostUid, uint32_t sessionNonce,
                       const char*& error);
bool beginHostedParty(const char*& error);
bool choosePartySignal(kitsu868::party::SignalChoice choice,
                       uint8_t round, const char*& error);
void leavePartyHotspot();
void stopListeningSafely();
bool partyRuntimeBusy();
bool persistDialogueState();
bool persistExpeditionState();
bool persistPartyRewardState();
bool persistClockState();
bool clockReading(uint32_t now,
                  kitsu868::timekeeping::ClockReading& output,
                  bool requireTrusted);
bool commitClockMutation(
    const kitsu868::timekeeping::KitsuClock& before,
    uint32_t previousGeneration, uint8_t previousSlot, uint32_t now,
    bool applyRuntime, const char** error = nullptr);
bool persistCompanionProgression();
bool captureCompanionProgression(
    uint8_t (&snapshot)[kitsu868::progression::kSnapshotCapacity]);
void restoreCompanionProgression(
    const uint8_t (&snapshot)[kitsu868::progression::kSnapshotCapacity]);
bool persistAdventureProgression();
bool persistSocialProgression();
bool persistActivityState();
bool persistFocusState();
kitsu868::focus::ClockSample focusClock(uint32_t now);
bool restoreFocusSnapshot(const kitsu868::focus::FocusState& state,
                          uint32_t now);
void loadClockState();
void loadCompanionProgression();
void loadAdventureProgression();
void loadSocialProgression();
void loadActivityState();
void loadFocusState();
void serviceClockNetworkTime(uint32_t now);
bool applyClockToRuntime(uint32_t now);
bool currentLocalDayMinute(uint32_t& dayId, uint16_t& minuteOfDay,
                           bool requireTrusted = true);
bool executeClockCommand(const String& command);
bool executeAdventureCommand(const String& command);
bool executeProfileCommand(const String& command);
bool executeActivityCommand(const String& command);
bool executeSocialCommand(const String& command);
void beginClockEditor();
bool commitClockEditor();
void tickActivity(uint32_t now);
void tickFocus(uint32_t now);
bool activityRuntimeBusy();
bool foregroundTransitionBlocked(const char* action);
void tickCompanionProgression(uint32_t now);
void recordCompanionAction(kitsu868::dialogue::Action action);
void queueProgressionSession(
    const kitsu868::progression::SessionResult& result, uint32_t day);
void replayLastDialogue();
void executeQuickAction();
kitsu868::adventure::ClockSample adventureClock(uint32_t now);
void recordSuccessfulEncounterTrigger(
    kitsu868::signal::MeshOperationKind kind);
bool materializePendingWildEncounter();
void loadPendingWildEncounter();
kitsu868::mesh::TransportStatus queueNearbyAction(
    uint16_t targetUid, uint32_t targetSessionNonce, uint16_t sequence,
    kitsu868::nearby::PositiveAction action);
kitsu868::CompanionVitals companionVitals();
ChatJournalEntry& appendChatJournal();
void advanceChatJournalGeneration();
void touchChatJournal(ChatJournalEntry& entry);
uint8_t applyChatJournalReadPlan(const uint8_t* journalIndexes,
                                 uint8_t indexCount);
bool commitMeshRadioSettings(const kitsu868::mesh::Settings& candidate,
                             const char*& error);
void enterScreen(Screen next);
const char* signalRarityName(kitsu868::signal::Rarity rarity);
const char* signalOperationName(kitsu868::signal::MeshOperationKind kind);

bool isFirmwareUpdateOperation(const char* operation) {
  if (!operation) return false;
  static const char* const operations[] = {
      "firmware.update.status", "firmware.update.begin",
      "firmware.update.write",  "firmware.update.finish",
      "firmware.update.reboot", "firmware.update.abort",
  };
  for (const char* candidate : operations) {
    if (strcmp(operation, candidate) == 0) return true;
  }
  return false;
}

class FirmwareBleBridge final
    : public kitsu868::connectivity::BleFrameDelegate,
      public kitsu868::connectivity::BleSessionTransport,
      public kitsu868::connectivity::BleOperationDelegate {
 public:
  bool begin() {
    begun_ = false;
    localControllerRecoveryLocked_ = false;
    if (!connectivitySecurityReady || wisp.uid.length() != 6U ||
        !session_.begin(deviceSecurity, crypto_, *this, *this,
                        wisp.uid.c_str())) {
      return false;
    }
    char advertisedName[24]{};
    snprintf(advertisedName, sizeof(advertisedName), "Kitsu %s",
             wisp.uid.c_str());
    begun_ = link_.begin(advertisedName, *this);
    return begun_;
  }

  void loop(uint32_t now) {
    if (!begun_) return;
    link_.loop(now);
    session_.loop(now);
  }

  bool preparePairingStorage() {
    pairingHeadroom_ =
        kitsu868::connectivity::KitsuNvsHeadroom::preparePairing(
            pairingNvsHeadroomPlatform);
    pairingStorageChecked_ = true;
    pairingStorageReady_ =
        kitsu868::connectivity::pairingHeadroomReady(pairingHeadroom_);
    Serial.printf(
        "KITSU_PAIR_STORAGE result=%s ready=%s free=%u usable=%u "
        "required=%u reclaimed_action=%s reclaimed_bluedroid=%s\n",
        kitsu868::connectivity::nvsHeadroomResultName(
            pairingHeadroom_.result),
        pairingStorageReady_ ? "true" : "false",
        static_cast<unsigned>(pairingHeadroom_.stats.freeEntries),
        static_cast<unsigned>(
            kitsu868::connectivity::pairingUsableEntries(pairingHeadroom_)),
        static_cast<unsigned>(
            kitsu868::connectivity::kPairingRequiredFreeEntries),
        pairingHeadroom_.retiredActionReplay ? "true" : "false",
        pairingHeadroom_.retiredBluedroid ? "true" : "false");
    return pairingStorageReady_;
  }

  bool openPairing(
      uint32_t now,
      kitsu868::connectivity::ControllerRole role =
          kitsu868::connectivity::ControllerRole::Owner) {
    if ((role != kitsu868::connectivity::ControllerRole::Owner &&
         role != kitsu868::connectivity::ControllerRole::Caretaker) ||
        !begun_ || !preparePairingStorage() || !link_.openPairingWindow(
                       now, kitsu868::connectivity::kBlePairingWindowMaximumMs)) {
      return false;
    }
    session_.setPairingWindow(
        true, kitsu868::connectivity::kBlePairingWindowMaximumMs, now, role);
    return true;
  }

  void closePairing() {
    if (!begun_) return;
    link_.closePairingWindow();
    session_.setPairingWindow(false, 0U, millis());
    session_.cancelPendingPairing();
  }

  bool disconnectForLocalControllerRecovery() {
    if (!begun_) return true;
    link_.closePairingWindow();
    session_.setPairingWindow(false, 0U, millis());
    session_.cancelPendingPairing();
    localControllerRecoveryLocked_ =
        link_.setLocalControllerRecoveryLocked(true);
    return localControllerRecoveryLocked_;
  }

  bool endLocalControllerRecovery() {
    if (!begun_) return true;
    if (!link_.setLocalControllerRecoveryLocked(false)) return false;
    localControllerRecoveryLocked_ = false;
    return true;
  }

  bool confirmNumeric() { return begun_ && link_.confirmNumericComparison(true); }
  bool confirmController(uint32_t now) {
    return begun_ && session_.confirmPendingPairing(now);
  }
  bool ready() const { return begun_; }
  bool pairingStorageBlocked() const {
    return pairingStorageChecked_ && !pairingStorageReady_;
  }
  bool pairingStorageReserved(uint32_t now) const {
    if (!begun_ || !pairingStorageReady_) return false;
    const kitsu868::connectivity::BleLinkStatus link = link_.status(now);
    const kitsu868::connectivity::BleSessionStatus session =
        session_.status(now);
    return link.pairingWindowOpen && !session.applicationAuthenticated;
  }
  bool localControllerRecoveryLocked() const {
    return !begun_ || localControllerRecoveryLocked_;
  }
  int bleBondCount() const { return begun_ ? link_.bondCount() : -1; }
  bool clearBleBondsForLocalRecovery(
      kitsu868::connectivity::BleBondClearStatus& status) {
    return begun_ && localControllerRecoveryLocked_ &&
        link_.clearAllBondsForLocalRecovery(status);
  }
  kitsu868::connectivity::BleLinkStatus linkStatus(uint32_t now) const {
    return link_.status(now);
  }
  kitsu868::connectivity::BleSessionStatus sessionStatus(uint32_t now) const {
    return session_.status(now);
  }

  void onBleFrame(const uint8_t* json, size_t jsonBytes) override {
    session_.onFrame(json, jsonBytes, millis());
  }

  void onBleLinkEvent(
      kitsu868::connectivity::BleLinkEvent event,
      const kitsu868::connectivity::BleLinkStatus& status) override {
    switch (event) {
      case kitsu868::connectivity::BleLinkEvent::Connected:
        // A fresh GATT generation must never inherit the previous
        // application session while link security is still negotiating.
        if (status.eventMatchesCurrentConnection) {
          petPresentationPreview.reset();
          petPresentationOpenedAt = 0U;
          session_.onLinkClosed(millis());
        }
        break;
      case kitsu868::connectivity::BleLinkEvent::LinkAuthenticated:
        session_.onSecureLinkEstablished(
            status.secureConnections, status.encrypted,
            status.authenticated, status.bonded, millis());
        break;
      case kitsu868::connectivity::BleLinkEvent::Disconnected:
        if (status.disconnectReasonAvailable) {
          Serial.printf(
              "KITSU_BLE_DISCONNECT {\"reason\":%ld,\"at_ms\":%lu,"
              "\"handle\":%u,\"generation\":%lu,\"cause\":\"%s\","
              "\"local\":%s}\n",
              static_cast<long>(status.lastDisconnectReason),
              static_cast<unsigned long>(status.lastDisconnectAtMillis),
              static_cast<unsigned>(status.lastDisconnectedHandle),
              static_cast<unsigned long>(
                  status.lastDisconnectedGeneration),
              kitsu868::connectivity::bleCloseCauseName(
                  status.lastCloseCause),
              status.lastCloseWasLocal ? "true" : "false");
        }
        if (!status.connected &&
            status.eventConnectionGeneration != 0U &&
            status.eventConnectionGeneration == status.connectionGeneration) {
          petPresentationPreview.reset();
          petPresentationOpenedAt = 0U;
          session_.onLinkClosed(millis());
        }
        break;
      case kitsu868::connectivity::BleLinkEvent::LinkRejected:
        petPresentationPreview.reset();
        petPresentationOpenedAt = 0U;
        session_.onLinkClosed(millis());
        break;
      case kitsu868::connectivity::BleLinkEvent::TransportFailure:
        if (status.notifyStatusAvailable) {
          Serial.printf(
              "KITSU_BLE_TRANSPORT {\"notify_status\":%ld,"
              "\"at_ms\":%lu,\"handle\":%u,\"generation\":%lu}\n",
              static_cast<long>(status.lastNotifyStatus),
              static_cast<unsigned long>(status.lastNotifyStatusAtMillis),
              static_cast<unsigned>(status.connectionHandle),
              static_cast<unsigned long>(status.connectionGeneration));
        }
        break;
      default:
        break;
    }
  }

  bool sendBleJson(const uint8_t* json, size_t jsonBytes) override {
    return link_.queueFrame(json, jsonBytes);
  }
  bool setBleApplicationAuthenticated(bool authenticated) override {
    return link_.setApplicationAuthenticated(authenticated);
  }
  bool bleTransmitIdle() const override {
    return !link_.status(millis()).requestInFlight;
  }
  bool sendEvent(const char* operation, const uint8_t* payload,
                 size_t payloadBytes) {
    return begun_ && session_.sendEvent(operation, payload, payloadBytes);
  }
  void disconnectBle(
      kitsu868::connectivity::BleCloseCause cause) override {
    link_.disconnect(cause);
  }
  bool handleBleRequest(
      const kitsu868::companion::DecodedEnvelope& request,
      const uint8_t* payload, size_t payloadBytes, uint8_t* responsePayload,
      size_t responseCapacity, size_t& responseBytes) override {
    if (isFirmwareUpdateOperation(request.operation)) {
      return bleOta.handleRequest(
          request.operation, payload, payloadBytes, responsePayload,
          responseCapacity, responseBytes);
    }
    return handleCompanionBleRequest(request, payload, payloadBytes,
                                     responsePayload, responseCapacity,
                                     responseBytes);
  }

 private:
  kitsu868::connectivity::KitsuBleGattLink link_{};
  kitsu868::connectivity::KitsuBleSession session_{};
  kitsu868::connectivity::Esp32CompanionCrypto crypto_{};
  kitsu868::connectivity::NvsHeadroomStatus pairingHeadroom_{};
  bool pairingStorageChecked_ = false;
  bool pairingStorageReady_ = false;
  bool begun_ = false;
  bool localControllerRecoveryLocked_ = false;
};

FirmwareBleBridge companionBle;

size_t configuredChannelCount();

String deviceSuffix() {
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X",
           static_cast<uint16_t>(ESP.getEfuseMac() & 0xffff));
  return String(suffix);
}

String deviceUid() {
  return "KT" + deviceSuffix();
}

String companionName() {
  return companionPack.valid() ? String(companionPack.name()) : String("None");
}

String jsonEscaped(const String& value) {
  String escaped;
  escaped.reserve(value.length() * 2U);
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value.charAt(index);
    switch (character) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\b': escaped += "\\b"; break;
      case '\f': escaped += "\\f"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: {
        const uint8_t byte = static_cast<uint8_t>(character);
        if (byte < 0x20U || byte == 0x7fU) {
          char unicodeEscape[7];
          snprintf(unicodeEscape, sizeof(unicodeEscape), "\\u%04X", byte);
          escaped += unicodeEscape;
        } else {
          escaped += character;
        }
        break;
      }
    }
  }
  return escaped;
}

const char* chatStateName(ChatJournalState state);
const char* chatLocalTxName(const ChatJournalEntry& entry);
const char* chatDeliveryAckName(const ChatJournalEntry& entry);

namespace companion_api {

struct CursorQuery {
  bool hasAfter = false;
  uint32_t after = 0U;
  uint8_t limit = 0U;
};

struct EncounterCodeQuery {
  bool hasAfter = false;
  uint32_t after = 0U;
  uint8_t limit = 0U;
};

void skipWhitespace(const uint8_t* input, size_t bytes, size_t& cursor) {
  while (cursor < bytes &&
         (input[cursor] == ' ' || input[cursor] == '\t' ||
          input[cursor] == '\r' || input[cursor] == '\n')) {
    ++cursor;
  }
}

bool consume(const uint8_t* input, size_t bytes, size_t& cursor,
             uint8_t expected) {
  skipWhitespace(input, bytes, cursor);
  if (cursor >= bytes || input[cursor] != expected) return false;
  ++cursor;
  return true;
}

bool parseAsciiString(const uint8_t* input, size_t bytes, size_t& cursor,
                      const uint8_t*& output, size_t& outputBytes) {
  skipWhitespace(input, bytes, cursor);
  if (cursor >= bytes || input[cursor++] != '"') return false;
  const size_t start = cursor;
  while (cursor < bytes && input[cursor] != '"') {
    // Cursor and property strings are deliberately escape-free ASCII.  This
    // gives duplicate/unknown-key rejection unambiguous byte semantics.
    if (input[cursor] < 0x20U || input[cursor] > 0x7eU ||
        input[cursor] == '\\') {
      return false;
    }
    ++cursor;
  }
  if (cursor >= bytes) return false;
  output = input + start;
  outputBytes = cursor - start;
  ++cursor;
  return true;
}

bool sameToken(const uint8_t* input, size_t bytes, const char* expected) {
  const size_t expectedBytes = strlen(expected);
  return bytes == expectedBytes &&
         memcmp(input, expected, expectedBytes) == 0;
}

bool parseUint32Token(const uint8_t* input, size_t bytes, uint32_t& output) {
  if (!input || bytes == 0U || bytes > 10U ||
      (bytes > 1U && input[0] == '0')) {
    return false;
  }
  uint64_t value = 0U;
  for (size_t i = 0U; i < bytes; ++i) {
    if (input[i] < '0' || input[i] > '9') return false;
    value = value * 10U + static_cast<uint8_t>(input[i] - '0');
    if (value > UINT32_MAX) return false;
  }
  output = static_cast<uint32_t>(value);
  return true;
}

bool parseCursorQuery(const uint8_t* input, size_t bytes,
                      CursorQuery& output) {
  output = CursorQuery{};
  if (!input || !kitsu868::companion::validUtf8(input, bytes)) return false;
  size_t cursor = 0U;
  if (!consume(input, bytes, cursor, '{')) return false;
  bool sawAfter = false;
  bool sawLimit = false;
  for (uint8_t field = 0U; field < 2U; ++field) {
    const uint8_t* key = nullptr;
    size_t keyBytes = 0U;
    if (!parseAsciiString(input, bytes, cursor, key, keyBytes) ||
        !consume(input, bytes, cursor, ':')) {
      return false;
    }
    if (sameToken(key, keyBytes, "after")) {
      if (sawAfter) return false;
      sawAfter = true;
      skipWhitespace(input, bytes, cursor);
      if (cursor + 4U <= bytes &&
          memcmp(input + cursor, "null", 4U) == 0) {
        cursor += 4U;
      } else {
        const uint8_t* value = nullptr;
        size_t valueBytes = 0U;
        if (!parseAsciiString(input, bytes, cursor, value, valueBytes) ||
            !parseUint32Token(value, valueBytes, output.after)) {
          return false;
        }
        output.hasAfter = true;
      }
    } else if (sameToken(key, keyBytes, "limit")) {
      if (sawLimit) return false;
      sawLimit = true;
      skipWhitespace(input, bytes, cursor);
      const size_t start = cursor;
      while (cursor < bytes && input[cursor] >= '0' &&
             input[cursor] <= '9') {
        ++cursor;
      }
      uint32_t limit = 0U;
      if (!parseUint32Token(input + start, cursor - start, limit) ||
          limit == 0U || limit > 100U) {
        return false;
      }
      output.limit = static_cast<uint8_t>(limit);
    } else {
      return false;
    }
    skipWhitespace(input, bytes, cursor);
    if (field == 0U) {
      if (cursor >= bytes || input[cursor++] != ',') return false;
    }
  }
  if (!sawAfter || !sawLimit || !consume(input, bytes, cursor, '}')) {
    return false;
  }
  skipWhitespace(input, bytes, cursor);
  return cursor == bytes;
}

bool parseEncounterCodeQuery(const uint8_t* input, size_t bytes,
                             EncounterCodeQuery& output) {
  output = EncounterCodeQuery{};
  if (!input || !kitsu868::companion::validUtf8(input, bytes)) return false;
  size_t cursor = 0U;
  if (!consume(input, bytes, cursor, '{')) return false;
  bool sawAfter = false;
  bool sawLimit = false;
  uint8_t fields = 0U;
  for (;;) {
    skipWhitespace(input, bytes, cursor);
    if (cursor < bytes && input[cursor] == '}') {
      ++cursor;
      break;
    }
    if (fields != 0U && !consume(input, bytes, cursor, ',')) return false;
    if (++fields > 2U) return false;

    const uint8_t* key = nullptr;
    size_t keyBytes = 0U;
    if (!parseAsciiString(input, bytes, cursor, key, keyBytes) ||
        !consume(input, bytes, cursor, ':')) {
      return false;
    }
    if (sameToken(key, keyBytes, "after")) {
      if (sawAfter) return false;
      sawAfter = true;
      const uint8_t* value = nullptr;
      size_t valueBytes = 0U;
      if (!parseAsciiString(input, bytes, cursor, value, valueBytes) ||
          !parseUint32Token(value, valueBytes, output.after)) {
        return false;
      }
      output.hasAfter = true;
    } else if (sameToken(key, keyBytes, "limit")) {
      if (sawLimit) return false;
      sawLimit = true;
      skipWhitespace(input, bytes, cursor);
      const size_t start = cursor;
      while (cursor < bytes && input[cursor] >= '0' &&
             input[cursor] <= '9') {
        ++cursor;
      }
      uint32_t limit = 0U;
      if (!parseUint32Token(input + start, cursor - start, limit) ||
          limit == 0U || limit > 100U) {
        return false;
      }
      output.limit = static_cast<uint8_t>(limit);
    } else {
      return false;
    }
  }
  skipWhitespace(input, bytes, cursor);
  return cursor == bytes && sawLimit;
}

bool emptyObject(const uint8_t* input, size_t bytes) {
  if (!input || !kitsu868::companion::validUtf8(input, bytes)) return false;
  size_t cursor = 0U;
  if (!consume(input, bytes, cursor, '{') ||
      !consume(input, bytes, cursor, '}')) {
    return false;
  }
  skipWhitespace(input, bytes, cursor);
  return cursor == bytes;
}

bool parseClockSync(const uint8_t* input, size_t bytes, uint32_t& epoch) {
  epoch = 0U;
  if (!input || !kitsu868::companion::validUtf8(input, bytes)) return false;
  size_t cursor = 0U;
  const uint8_t* key = nullptr;
  size_t keyBytes = 0U;
  if (!consume(input, bytes, cursor, '{') ||
      !parseAsciiString(input, bytes, cursor, key, keyBytes) ||
      !sameToken(key, keyBytes, "epoch") ||
      !consume(input, bytes, cursor, ':')) {
    return false;
  }
  skipWhitespace(input, bytes, cursor);
  const size_t start = cursor;
  while (cursor < bytes && input[cursor] >= '0' && input[cursor] <= '9') {
    ++cursor;
  }
  if (!parseUint32Token(input + start, cursor - start, epoch) ||
      !consume(input, bytes, cursor, '}')) {
    return false;
  }
  skipWhitespace(input, bytes, cursor);
  return cursor == bytes;
}

bool parseMeshEnabled(const uint8_t* input, size_t bytes, bool& enabled) {
  enabled = false;
  if (!input || !kitsu868::companion::validUtf8(input, bytes)) return false;
  size_t cursor = 0U;
  const uint8_t* key = nullptr;
  size_t keyBytes = 0U;
  if (!consume(input, bytes, cursor, '{') ||
      !parseAsciiString(input, bytes, cursor, key, keyBytes) ||
      !sameToken(key, keyBytes, "enabled") ||
      !consume(input, bytes, cursor, ':')) {
    return false;
  }
  skipWhitespace(input, bytes, cursor);
  if (cursor + 4U <= bytes &&
      memcmp(input + cursor, "true", 4U) == 0) {
    enabled = true;
    cursor += 4U;
  } else if (cursor + 5U <= bytes &&
             memcmp(input + cursor, "false", 5U) == 0) {
    cursor += 5U;
  } else {
    return false;
  }
  if (!consume(input, bytes, cursor, '}')) return false;
  skipWhitespace(input, bytes, cursor);
  return cursor == bytes;
}

bool sequenceAfter(uint32_t candidate, uint32_t reference) {
  return candidate != reference &&
         static_cast<int32_t>(candidate - reference) > 0;
}

String publicKeyBase64(const uint8_t publicKey[32]) {
  char encoded[44]{};
  size_t encodedBytes = 0U;
  if (!kitsu868::companion::encodeBase64Url(
          publicKey, 32U, encoded, sizeof(encoded), encodedBytes) ||
      encodedBytes != 43U) {
    return String();
  }
  return String(encoded);
}

void appendCursor(String& output, bool hasCursor, uint32_t cursor) {
  if (!hasCursor) {
    output += "null";
    return;
  }
  output += '"';
  output += String(cursor);
  output += '"';
}

bool copyResponse(const String& response, uint8_t* output, size_t capacity,
                  size_t& outputBytes) {
  outputBytes = 0U;
  if (!output || response.length() == 0U || response.length() > capacity ||
      response.length() > kitsu868::companion::kMaximumEnvelopePayloadBytes) {
    return false;
  }
  memcpy(output, response.c_str(), response.length());
  outputBytes = response.length();
  return true;
}

bool buildEncounterCodes(const uint8_t* payload, size_t payloadBytes,
                         String& output) {
  EncounterCodeQuery query{};
  if (!encounterCodesReady ||
      !parseEncounterCodeQuery(payload, payloadBytes, query)) {
    return false;
  }
  const size_t count = encounterCodes.count();
  const size_t start = query.hasAfter
                           ? min<size_t>(query.after, count)
                           : 0U;
  const size_t end = min<size_t>(
      count, start + static_cast<size_t>(query.limit));

  output = "{\"schema\":\"kitsu.encounter-codes.v1\",\"items\":[";
  output.reserve(256U + (end - start) * 260U);
  bool first = true;
  for (size_t index = start; index < end; ++index) {
    kitsu868::unlocks::CodeRecord record{};
    if (!encounterCodes.at(index, record)) return false;
    if (!first) output += ',';
    first = false;
    char codeId[9]{};
    snprintf(codeId, sizeof(codeId), "%08lX",
             static_cast<unsigned long>(record.codeId));
    output += "{\"device_id\":\"";
    output += wisp.uid;
    output += "\",\"code_id\":\"";
    output += codeId;
    output += "\",\"code\":\"";
    // Raw codes leave the device only inside this already-authenticated BLE
    // response. They are never written to the serial/log stream.
    output += record.code;
    output += "\",\"pack_id\":";
    output += String(static_cast<unsigned long>(record.packId));
    output += ",\"creature_name\":\"";
    output += jsonEscaped(String(record.creatureName));
    output += "\",\"rarity\":\"";
    output += kitsu868::unlocks::rarityName(record.rarity);
    output += "\",\"source\":\"";
    output += jsonEscaped(String(record.source));
    output += "\",\"acquired_at_epoch\":";
    output += String(static_cast<unsigned long>(record.acquiredAtEpoch));
    output += ",\"redeemed\":";
    output += record.redeemed ? "true" : "false";
    output += ",\"installed\":";
    output += record.installed ? "true" : "false";
    output += '}';
    memset(codeId, 0, sizeof(codeId));
  }
  output += "],\"cursor\":";
  if (end > start) {
    output += '"';
    output += String(static_cast<unsigned long>(end));
    output += '"';
  } else {
    output += "null";
  }
  output += ",\"has_more\":";
  output += end < count ? "true" : "false";
  output += '}';
  return output.length() <=
      kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

void loadActionReplay() {
  bleActionReplayReady = false;
  bleActionReplayCache.reset();
  if (!storageReady) return;

  size_t expectedBytes = 0U;
  bleActionReplayCache.serialized(expectedBytes);
  if (expectedBytes !=
      kitsu868::connectivity::kBleActionReplaySerializedBytes) {
    Serial.println("KITSU_WARN ble_action_replay=layout");
    return;
  }
  const size_t storedBytes = preferences.getBytesLength("ble_act_v3");
  if (storedBytes == 0U) {
    // A missing key is the clean first-boot state. The first accepted action
    // atomically creates it before executing the side effect.
    bleActionReplayReady = true;
    return;
  }
  if (storedBytes != expectedBytes) {
    Serial.println("KITSU_WARN ble_action_replay=invalid_size");
    return;
  }
  memset(bleActionReplayScratch, 0, sizeof(bleActionReplayScratch));
  if (preferences.getBytes("ble_act_v3", bleActionReplayScratch,
                           sizeof(bleActionReplayScratch)) !=
          sizeof(bleActionReplayScratch) ||
      !bleActionReplayCache.load(bleActionReplayScratch,
                                 sizeof(bleActionReplayScratch))) {
    memset(bleActionReplayScratch, 0, sizeof(bleActionReplayScratch));
    Serial.println("KITSU_WARN ble_action_replay=invalid_crc");
    return;
  }
  memset(bleActionReplayScratch, 0, sizeof(bleActionReplayScratch));
  bleActionReplayReady = true;
}

bool persistActionReplay() {
  if (!storageReady || !bleActionReplayReady) return false;
  size_t serializedBytes = 0U;
  const uint8_t* serialized =
      bleActionReplayCache.serialized(serializedBytes);
  return serialized && serializedBytes != 0U &&
         preferences.putBytes("ble_act_v3", serialized, serializedBytes) ==
             serializedBytes;
}

bool snapshotActionReplay() {
  size_t serializedBytes = 0U;
  const uint8_t* serialized =
      bleActionReplayCache.serialized(serializedBytes);
  if (!serialized || serializedBytes != sizeof(bleActionReplayScratch)) {
    return false;
  }
  memcpy(bleActionReplayScratch, serialized, serializedBytes);
  return true;
}

bool restoreActionReplaySnapshot() {
  const bool loaded = bleActionReplayCache.load(
      bleActionReplayScratch, sizeof(bleActionReplayScratch));
  memset(bleActionReplayScratch, 0, sizeof(bleActionReplayScratch));
  return loaded && persistActionReplay();
}

bool restoreActionReplaySnapshotInMemory() {
  const bool loaded = bleActionReplayCache.load(
      bleActionReplayScratch, sizeof(bleActionReplayScratch));
  memset(bleActionReplayScratch, 0, sizeof(bleActionReplayScratch));
  return loaded;
}

const char* bleMeshErrorName(kitsu868::mesh::TransportStatus status) {
  using kitsu868::mesh::TransportStatus;
  switch (status) {
    case TransportStatus::Disabled: return "mesh_disabled";
    case TransportStatus::IdentityStorageFailed:
      return "mesh_identity_unavailable";
    case TransportStatus::RadioInitFailed:
      return "mesh_radio_unavailable";
    case TransportStatus::TimeUnset: return "time_unset";
    case TransportStatus::TxLocked: return "tx_policy_locked";
    case TransportStatus::PacketPoolFull: return "queue_full";
    case TransportStatus::ContactNotFound: return "unknown_contact";
    case TransportStatus::ContactNotClient: return "contact_not_client";
    case TransportStatus::ContactTableFull: return "contact_full";
    case TransportStatus::ChannelNotFound: return "unknown_channel";
    case TransportStatus::TextTooLong: return "text_too_long";
    case TransportStatus::SendBusy: return "send_busy";
    case TransportStatus::MessagingStorageFailed: return "storage_failed";
    case TransportStatus::InvalidArgument: return "invalid_argument";
    default: return kitsu868::mesh::transportStatusName(status);
  }
}

const char* bleAdvertiseReadinessError(uint32_t& retryAfterMs) {
  retryAfterMs = 0U;
  if (!bleActionReplayReady) return "idempotency_unavailable";
  if (!companionPack.valid()) return "companion_unavailable";
  if (!meshTransport.identityReady()) return "mesh_identity_unavailable";
  if (!meshSettings.enabled) return "mesh_disabled";
  if (!meshTransport.active()) return "mesh_radio_unavailable";
  const kitsu868::mesh::TransportStatus status =
      meshTransport.advertiseReadiness(
          meshSettings, meshCurrentLocation, retryAfterMs);
  return status == kitsu868::mesh::TransportStatus::Ok
             ? nullptr
             : bleMeshErrorName(status);
}

bool decodeCanonicalPeerKey(
    const kitsu868::connectivity::BleActionCommand& command,
    uint8_t output[32]) {
  size_t decodedBytes = 0U;
  if (!kitsu868::companion::decodeBase64Url(
          command.messageTarget, command.messageTargetBytes, output, 32U,
          decodedBytes) || decodedBytes != 32U) {
    return false;
  }
  char canonical[44]{};
  size_t canonicalBytes = 0U;
  return kitsu868::companion::encodeBase64Url(
             output, 32U, canonical, sizeof(canonical), canonicalBytes) &&
         canonicalBytes == command.messageTargetBytes &&
         memcmp(canonical, command.messageTarget, canonicalBytes) == 0;
}

kitsu868::mesh::TransportStatus ensureObservedContact(
    const uint8_t publicKey[32]) {
  for (size_t index = 0U; index < meshTransport.contactCount(); ++index) {
    kitsu868::mesh::ContactRecord contact{};
    if (meshTransport.contactAt(index, contact) &&
        memcmp(contact.publicKey, publicKey, 32U) == 0) {
      return contact.type == 1U
                 ? kitsu868::mesh::TransportStatus::Ok
                 : kitsu868::mesh::TransportStatus::ContactNotClient;
    }
  }
  if (!discoveryJournalReady) {
    return kitsu868::mesh::TransportStatus::ContactNotFound;
  }
  for (size_t ordinal = 0U;
       ordinal < kitsu868::discovery::kDiscoveryPeerCapacity; ++ordinal) {
    kitsu868::discovery::DiscoveryPeer peer{};
    if (!discoveryJournal.peerAt(ordinal, peer) ||
        memcmp(peer.publicKey, publicKey, 32U) != 0) {
      continue;
    }
    if (peer.type != 1U) {
      return kitsu868::mesh::TransportStatus::ContactNotClient;
    }
    const char* const name = peer.name[0] != '\0'
                                 ? peer.name
                                 : "MeshCore peer";
    return meshTransport.stageObservedContact(
        peer.publicKey, name, peer.type, peer.senderAdvertTimestamp);
  }
  return kitsu868::mesh::TransportStatus::ContactNotFound;
}

kitsu868::mesh::TransportStatus queueBleMessage(
    const kitsu868::connectivity::BleActionCommand& command,
    ChatJournalEntry*& journalEntry) {
  using kitsu868::connectivity::BleMessageRoute;
  using kitsu868::mesh::MessageKind;
  using kitsu868::mesh::MessageRoute;
  using kitsu868::mesh::TransportStatus;
  journalEntry = nullptr;
  uint32_t queuedTimestamp = 0U;

  if (command.messageRoute == BleMessageRoute::Direct) {
    uint8_t publicKey[32]{};
    if (!decodeCanonicalPeerKey(command, publicKey)) {
      return TransportStatus::InvalidArgument;
    }
    TransportStatus status = ensureObservedContact(publicKey);
    if (status != TransportStatus::Ok) return status;
    uint32_t expectedAck = 0U;
    MessageRoute route = MessageRoute::Flood;
    status = meshTransport.sendDirectTextOnce(
        meshSettings, publicKey, command.messageText, 0U, true,
        queuedTimestamp, expectedAck, route);
    if (status != TransportStatus::Ok) return status;

    ChatJournalEntry& entry = appendChatJournal();
    entry.inbound = false;
    entry.kind = MessageKind::Direct;
    entry.route = route;
    entry.state = ChatJournalState::Queued;
    entry.authenticated = true;
    entry.timestamp = queuedTimestamp;
    entry.expectedAck = expectedAck;
    entry.queuedAt = millis();
    memcpy(entry.contactPublicKey, publicKey, sizeof(entry.contactPublicKey));
    memcpy(entry.text, command.messageText,
           static_cast<size_t>(command.messageTextBytes) + 1U);
    journalEntry = &entry;
    return TransportStatus::Ok;
  }

  if (command.messageRoute != BleMessageRoute::Channel ||
      command.messageTargetBytes != 1U ||
      command.messageTarget[0] < '0' || command.messageTarget[0] > '3') {
    return TransportStatus::InvalidArgument;
  }
  const uint8_t slot =
      static_cast<uint8_t>(command.messageTarget[0] - '0');
  const TransportStatus status = meshTransport.sendChannelTextOnce(
      meshSettings, slot, command.messageText, true, queuedTimestamp);
  if (status != TransportStatus::Ok) return status;

  ChatJournalEntry& entry = appendChatJournal();
  entry.inbound = false;
  entry.kind = MessageKind::Channel;
  entry.route = MessageRoute::Flood;
  entry.state = ChatJournalState::Queued;
  entry.authenticated = false;
  entry.timestamp = queuedTimestamp;
  entry.channelSlot = slot;
  entry.queuedAt = millis();
  memcpy(entry.text, command.messageText,
         static_cast<size_t>(command.messageTextBytes) + 1U);
  journalEntry = &entry;
  return TransportStatus::Ok;
}

kitsu868::mesh::TransportStatus queueBleAdvert(
    const kitsu868::connectivity::BleActionCommand& command) {
  using kitsu868::connectivity::BleAdvertScope;
  using kitsu868::mesh::AdvertScope;
  using kitsu868::mesh::TransportStatus;

  AdvertScope scope = AdvertScope::Nearby;
  if (command.advertScope == BleAdvertScope::Mesh) {
    scope = AdvertScope::Flood;
  } else if (command.advertScope != BleAdvertScope::Nearby) {
    return TransportStatus::InvalidArgument;
  }
  const TransportStatus status = meshTransport.introduceOnce(
      scope, meshSettings, meshCurrentLocation, true);
  if (status != TransportStatus::Ok) return status;
  if (meshSettings.locationMode ==
      kitsu868::mesh::LocationMode::CurrentOnce) {
    kitsu868::mesh::clearCurrentLocationOnce(meshCurrentLocation);
  }
  companionBleRefreshDirty = true;
  return TransportStatus::Ok;
}

bool rejectAction(const kitsu868::connectivity::BleActionCommand& command,
                  const char* errorCode, uint8_t* output, size_t capacity,
                  size_t& outputBytes) {
  return kitsu868::connectivity::encodeBleActionReceipt(
      command, false, "rejected", errorCode, output, capacity, outputBytes);
}

bool applyAction(const uint8_t* payload, size_t payloadBytes,
                 uint8_t* output, size_t capacity, size_t& outputBytes) {
  using kitsu868::connectivity::BleActionCommand;
  using kitsu868::connectivity::BleActionDecodeResult;
  using kitsu868::connectivity::BleActionKind;
  using kitsu868::connectivity::BleActionReplayDecision;

  BleActionCommand command{};
  const auto rejected = [&](const char* errorCode) -> bool {
    return rejectAction(command, errorCode, output, capacity, outputBytes);
  };
  const auto accepted = [&](const char* state) -> bool {
    return kitsu868::connectivity::encodeBleActionReceipt(
        command, true, state, nullptr, output, capacity, outputBytes);
  };
  const BleActionDecodeResult decoded =
      kitsu868::connectivity::decodeBleActionCommand(
          payload, payloadBytes, command);
  if (decoded != BleActionDecodeResult::Ok) {
    const char* error =
        kitsu868::connectivity::bleActionDecodeResultName(decoded);
    if (command.actionIdValid) {
      return rejected(error);
    }
    String response = "{\"ok\":false,\"error\":\"";
    response += error;
    response += "\"}";
    return copyResponse(response, output, capacity, outputBytes);
  }

  if (!kitsu868::connectivity::bleActionKindAvailable(command.kind)) {
    return rejected("action_unavailable");
  }
  const bool careAction = command.kind == BleActionKind::Pet ||
      command.kind == BleActionKind::Feed ||
      command.kind == BleActionKind::Play ||
      command.kind == BleActionKind::ListenOnce;
  if (careAction && !companionPack.valid()) {
    return rejected("companion_unavailable");
  }
  if (command.kind == BleActionKind::ListenOnce) {
    if (activeGame != ActiveGame::None) {
      return rejected("busy_game");
    }
    if (!radioReady) {
      return rejected("radio_unavailable");
    }
  }
  if (!bleActionReplayReady) {
    return rejected("idempotency_unavailable");
  }

  const uint32_t actionNow = meshTransport.timeValid()
                                 ? meshTransport.currentEpoch()
                                 : 0U;
  const BleActionReplayDecision replay =
      bleActionReplayCache.inspect(command, actionNow);
  if (replay == BleActionReplayDecision::TimeUnavailable) {
    return rejected("time_unset");
  }
  if (replay == BleActionReplayDecision::Expired) {
    return rejected("action_expired");
  }
  if (replay == BleActionReplayDecision::InvalidExpiry) {
    return rejected("invalid_expiry");
  }
  if (replay == BleActionReplayDecision::Conflict) {
    return rejected("action_id_conflict");
  }
  if (replay == BleActionReplayDecision::DuplicateApplied) {
    return accepted(command.kind == BleActionKind::SendMessage ||
                            command.kind == BleActionKind::AdvertiseOnce
                        ? "queued" : "applied");
  }
  if (replay == BleActionReplayDecision::DuplicateIndeterminate) {
    return rejected("action_result_unknown");
  }

  if (command.kind == BleActionKind::AdvertiseOnce) {
    uint32_t retryAfterMs = 0U;
    const char* const readiness =
        bleAdvertiseReadinessError(retryAfterMs);
    if (readiness) return rejected(readiness);
  }

  // Persist the idempotency reservation before invoking a side effect. Keep
  // the previous blob in global scratch rather than placing multi-kilobyte
  // cache copies on the 8 KiB Arduino loop task stack.
  if (!snapshotActionReplay()) {
    return rejected("idempotency_unavailable");
  }
  if (!bleActionReplayCache.remember(command, actionNow)) {
    // A full replay window is normal, transient backpressure: all eight
    // records can still be inside their caller-supplied protection window.
    // No durable write or action side effect has happened yet. Restore the
    // exact pre-attempt RAM image and invite the client to retry after an old
    // reservation expires. A failed restore is an actual replay-storage
    // integrity failure and must remain fail-closed under the durable error.
    if (!restoreActionReplaySnapshotInMemory() || !bleActionReplayReady) {
      bleActionReplayReady = false;
      Serial.println("KITSU_WARN ble_action_replay=capacity_restore_failed");
      return rejected("idempotency_unavailable");
    }
    Serial.println("KITSU_WARN ble_action_replay=capacity_busy");
    return rejected("idempotency_busy");
  }
  if (!persistActionReplay()) {
    // The failed reservation did not authorize a side effect. Restore RAM
    // only: immediately rewriting the old blob just repeats the same NVS
    // pressure and can damage the previously durable version on IDF 4.4.
    (void)restoreActionReplaySnapshotInMemory();
    bleActionReplayReady = false;
    Serial.println("KITSU_WARN ble_action_replay=storage_write_failed");
    return rejected("idempotency_unavailable");
  }

  bool applied = false;
  ChatJournalEntry* queuedMessage = nullptr;
  switch (command.kind) {
    case BleActionKind::Pet: applied = petWisp(); break;
    case BleActionKind::Feed: applied = feedKitsu(); break;
    case BleActionKind::Play: applied = playKitsu(); break;
    case BleActionKind::ListenOnce:
      applied = startListening(command.durationMs);
      break;
    case BleActionKind::AdvertiseOnce: {
      const kitsu868::mesh::TransportStatus status =
          queueBleAdvert(command);
      if (status != kitsu868::mesh::TransportStatus::Ok) {
        if (!restoreActionReplaySnapshot()) {
          bleActionReplayReady = false;
          Serial.println("KITSU_WARN ble_action_rollback=flush_failed");
          return rejected("action_result_unknown");
        }
        return rejected(bleMeshErrorName(status));
      }
      applied = true;
      break;
    }
    case BleActionKind::SendMessage: {
      const kitsu868::mesh::TransportStatus status =
          queueBleMessage(command, queuedMessage);
      if (status != kitsu868::mesh::TransportStatus::Ok) {
        if (!restoreActionReplaySnapshot()) {
          bleActionReplayReady = false;
          Serial.println("KITSU_WARN ble_action_rollback=flush_failed");
          return rejected("action_result_unknown");
        }
        return rejected(bleMeshErrorName(status));
      }
      applied = queuedMessage != nullptr;
      break;
    }
  }
  if (!applied) {
    // Preconditions were checked immediately above, so this is an
    // indeterminate internal failure. Keep the replay reservation: retrying a
    // non-idempotent action would be less safe than reporting uncertainty.
    memset(bleActionReplayScratch, 0, sizeof(bleActionReplayScratch));
    return rejected("action_result_unknown");
  }
  // Capture the durable Pending interpretation before advancing it. If the
  // Applied commit fails after the side effect, restoring this snapshot keeps
  // every reboot and retry fail-closed as indeterminate.
  if (!snapshotActionReplay()) {
    return rejected("action_result_unknown");
  }
  if (!bleActionReplayCache.markApplied(command) ||
      !persistActionReplay()) {
    (void)bleActionReplayCache.load(bleActionReplayScratch,
                                    sizeof(bleActionReplayScratch));
    memset(bleActionReplayScratch, 0, sizeof(bleActionReplayScratch));
    Serial.println("KITSU_WARN ble_action_outcome=flush_failed");
    return rejected("action_result_unknown");
  }
  memset(bleActionReplayScratch, 0, sizeof(bleActionReplayScratch));
  return accepted(command.kind == BleActionKind::SendMessage ||
                          command.kind == BleActionKind::AdvertiseOnce
                      ? "queued" : "applied");
}

const char* advertTransmitStateName(
    kitsu868::mesh::AdvertTransmitState state) {
  using kitsu868::mesh::AdvertTransmitState;
  switch (state) {
    case AdvertTransmitState::Queued: return "queued";
    case AdvertTransmitState::Sent: return "sent";
    case AdvertTransmitState::TxFailed: return "tx_failed";
  }
  return nullptr;
}

bool validFloodAdvertStatus(const kitsu868::mesh::FloodAdvertStatus& status) {
  using kitsu868::mesh::AdvertTransmitState;
  if (!status.available ||
      status.emittedAt < kitsu868::mesh::kAdvertMinimumEmittedAt ||
      status.emittedAt > kitsu868::mesh::kAdvertMaximumEmittedAt) {
    return false;
  }
  switch (status.state) {
    case AdvertTransmitState::Sent:
      if (!status.repeatCountKnown ||
          status.sourceCount >
              kitsu868::mesh::kAdvertRepeatSourceCapacity ||
          status.sourceCount > status.repeatCount ||
          (status.sourcesTruncated &&
           (status.sourceCount !=
                kitsu868::mesh::kAdvertRepeatSourceCapacity ||
            status.repeatCount <
                kitsu868::mesh::kAdvertRepeatSourceCapacity + 1U)) ||
          (status.repeatCount == 0U && status.sourcesTruncated)) {
        return false;
      }
      for (uint8_t index = 0U; index < status.sourceCount; ++index) {
        const kitsu868::mesh::AdvertRepeatSource& source =
            status.sources[index];
        if (source.tokenBytes == 0U ||
            source.tokenBytes >
                kitsu868::mesh::kAdvertRepeatSourceTokenBytes) {
          return false;
        }
        for (uint8_t prior = 0U; prior < index; ++prior) {
          const kitsu868::mesh::AdvertRepeatSource& candidate =
              status.sources[prior];
          if (candidate.tokenBytes == source.tokenBytes &&
              memcmp(candidate.token, source.token, source.tokenBytes) == 0) {
            return false;
          }
        }
      }
      return true;
    case AdvertTransmitState::Queued:
    case AdvertTransmitState::TxFailed:
      return !status.repeatCountKnown && status.repeatCount == 0U &&
          !status.observationOpen && status.sourceCount == 0U &&
          !status.sourcesTruncated;
  }
  return false;
}

bool validNearbyAdvertStatus(
    const kitsu868::mesh::NearbyAdvertStatus& status) {
  if (!status.available ||
      status.emittedAt < kitsu868::mesh::kAdvertMinimumEmittedAt ||
      status.emittedAt > kitsu868::mesh::kAdvertMaximumEmittedAt) {
    return false;
  }
  return advertTransmitStateName(status.state) != nullptr;
}

const char* repeatDiagnosticResultName(
    kitsu868::mesh::RepeatDiagnosticResult result) {
  using kitsu868::mesh::RepeatDiagnosticResult;
  switch (result) {
    case RepeatDiagnosticResult::None: return "none";
    case RepeatDiagnosticResult::NoActiveHash: return "no_active_hash";
    case RepeatDiagnosticResult::WireMismatch: return "wire_mismatch";
    case RepeatDiagnosticResult::DigestMismatch: return "digest_mismatch";
    case RepeatDiagnosticResult::Recorded: return "recorded";
    case RepeatDiagnosticResult::Saturated: return "saturated";
  }
  return nullptr;
}

void appendUpperHex(String& output, const uint8_t* bytes, size_t byteCount) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (size_t index = 0U; index < byteCount; ++index) {
    output += kHex[(bytes[index] >> 4U) & 0x0fU];
    output += kHex[bytes[index] & 0x0fU];
  }
}

void appendIrqFlagsOrNull(String& output, uint32_t samples, uint16_t flags) {
  if (samples == 0U) {
    output += "null";
    return;
  }
  const uint8_t bytes[2] = {
      static_cast<uint8_t>(flags >> 8U), static_cast<uint8_t>(flags)};
  output += '"';
  appendUpperHex(output, bytes, sizeof(bytes));
  output += '"';
}

void appendReceiveObservability(
    String& output, const kitsu868::mesh::RepeatDiagnostics& diagnostics) {
  output += ",\"dio1_poll_calls\":";
  output += String(diagnostics.dio1Polls);
  output += ",\"dio1_high_polls\":";
  output += String(diagnostics.dio1HighPolls);
  output += ",\"dio1_high_edges\":";
  output += String(diagnostics.dio1HighEdges);
  output += ",\"dio1_callback_calls\":";
  output += String(diagnostics.dio1Callbacks);
  output += ",\"irq_samples\":";
  output += String(diagnostics.irqSamples);
  output += ",\"irq_dio_asserted_samples\":";
  output += String(diagnostics.irqDioAssertedSamples);
  output += ",\"irq_low_rate_samples\":";
  output += String(diagnostics.irqLowRateSamples);
  output += ",\"irq_observation_open\":";
  output += diagnostics.irqObservationOpen ? "true" : "false";
  output += ",\"irq_observation_remaining_ms\":";
  output += String(diagnostics.irqObservationRemainingMs);
  output += ",\"last_irq_flags\":";
  appendIrqFlagsOrNull(output, diagnostics.irqSamples,
                       diagnostics.lastIrqFlags);
  output += ",\"last_dio_irq_flags\":";
  appendIrqFlagsOrNull(output, diagnostics.irqDioAssertedSamples,
                       diagnostics.lastDioIrqFlags);
  output += ",\"last_low_rate_irq_flags\":";
  appendIrqFlagsOrNull(output, diagnostics.irqLowRateSamples,
                       diagnostics.lastLowRateIrqFlags);
  output += ",\"irq_rx_done_observations\":";
  output += String(diagnostics.irqRxDoneObservations);
  output += ",\"irq_crc_error_observations\":";
  output += String(diagnostics.irqCrcErrorObservations);
  output += ",\"irq_header_error_observations\":";
  output += String(diagnostics.irqHeaderErrorObservations);
  output += ",\"irq_timeout_observations\":";
  output += String(diagnostics.irqTimeoutObservations);
  output += ",\"irq_preamble_observations\":";
  output += String(diagnostics.irqPreambleObservations);
  output += ",\"irq_header_valid_observations\":";
  output += String(diagnostics.irqHeaderValidObservations);
  output += ",\"irq_sync_word_valid_observations\":";
  output += String(diagnostics.irqSyncWordValidObservations);
  output += ",\"dio_irq_rx_done_observations\":";
  output += String(diagnostics.dioIrqRxDoneObservations);
  output += ",\"dio_irq_crc_error_observations\":";
  output += String(diagnostics.dioIrqCrcErrorObservations);
  output += ",\"dio_irq_header_error_observations\":";
  output += String(diagnostics.dioIrqHeaderErrorObservations);
  output += ",\"dio_irq_timeout_observations\":";
  output += String(diagnostics.dioIrqTimeoutObservations);
  output += ",\"dio_irq_preamble_observations\":";
  output += String(diagnostics.dioIrqPreambleObservations);
  output += ",\"dio_irq_header_valid_observations\":";
  output += String(diagnostics.dioIrqHeaderValidObservations);
  output += ",\"dio_irq_sync_word_valid_observations\":";
  output += String(diagnostics.dioIrqSyncWordValidObservations);
  output += ",\"low_rate_irq_rx_done_observations\":";
  output += String(diagnostics.lowRateIrqRxDoneObservations);
  output += ",\"low_rate_irq_crc_error_observations\":";
  output += String(diagnostics.lowRateIrqCrcErrorObservations);
  output += ",\"low_rate_irq_header_error_observations\":";
  output += String(diagnostics.lowRateIrqHeaderErrorObservations);
  output += ",\"low_rate_irq_timeout_observations\":";
  output += String(diagnostics.lowRateIrqTimeoutObservations);
  output += ",\"low_rate_irq_preamble_observations\":";
  output += String(diagnostics.lowRateIrqPreambleObservations);
  output += ",\"low_rate_irq_header_valid_observations\":";
  output += String(diagnostics.lowRateIrqHeaderValidObservations);
  output += ",\"low_rate_irq_sync_word_valid_observations\":";
  output += String(diagnostics.lowRateIrqSyncWordValidObservations);
  output += ",\"recv_raw_attempts\":";
  output += String(diagnostics.recvRawAttempts);
  output += ",\"recv_interrupt_ready_attempts\":";
  output += String(diagnostics.recvInterruptReadyAttempts);
  output += ",\"recv_packet_length_samples\":";
  output += String(diagnostics.recvPacketLengthSamples);
  output += ",\"recv_packet_length_zero\":";
  output += String(diagnostics.recvPacketLengthZero);
  output += ",\"last_recv_packet_length\":";
  if (diagnostics.lastRecvPacketLengthAvailable) {
    output += String(diagnostics.lastRecvPacketLength);
  } else {
    output += "null";
  }
  output += ",\"recv_read_data_attempts\":";
  output += String(diagnostics.recvReadDataAttempts);
  output += ",\"recv_successful_reads\":";
  output += String(diagnostics.recvSuccessfulReads);
  output += ",\"recv_read_data_errors\":";
  output += String(diagnostics.recvReadDataErrors);
  output += ",\"last_recv_read_data_error\":";
  if (diagnostics.lastRecvReadDataErrorAvailable) {
    output += String(diagnostics.lastRecvReadDataError);
  } else {
    output += "null";
  }
  output += ",\"recv_rx_restart_attempts\":";
  output += String(diagnostics.recvRxRestartAttempts);
  output += ",\"recv_rx_restart_successes\":";
  output += String(diagnostics.recvRxRestartSuccesses);
  output += ",\"recv_rx_restart_errors\":";
  output += String(diagnostics.recvRxRestartErrors);
  output += ",\"last_recv_rx_restart_result\":";
  if (diagnostics.lastRecvRxRestartResultAvailable) {
    output += String(diagnostics.lastRecvRxRestartResult);
  } else {
    output += "null";
  }
  output += ",\"last_recv_rx_restart_error\":";
  if (diagnostics.lastRecvRxRestartErrorAvailable) {
    output += String(diagnostics.lastRecvRxRestartError);
  } else {
    output += "null";
  }
  output += ",\"short_frame_rejected\":";
  output += String(diagnostics.shortFrameRejected);
  output += ",\"last_short_frame_length\":";
  if (diagnostics.lastShortFrameLengthAvailable) {
    output += String(diagnostics.lastShortFrameLength);
  } else {
    output += "null";
  }
  output += ",\"max_mesh_loop_gap_ms\":";
  output += String(diagnostics.maxMeshLoopGapMs);
}

bool buildState(const uint8_t* payload, size_t payloadBytes, String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  uint32_t advertiseRetryAfterMs = 0U;
  const char* const advertiseError =
      bleAdvertiseReadinessError(advertiseRetryAfterMs);
  kitsu868::mesh::FloodAdvertStatus lastFloodAdvert{};
  const bool hasLastFloodAdvert =
      meshTransport.lastFloodAdvertStatus(lastFloodAdvert);
  if (hasLastFloodAdvert && !validFloodAdvertStatus(lastFloodAdvert)) {
    return false;
  }
  kitsu868::mesh::NearbyAdvertStatus lastNearbyAdvert{};
  const bool hasLastNearbyAdvert =
      meshTransport.lastNearbyAdvertStatus(lastNearbyAdvert);
  if (hasLastNearbyAdvert && !validNearbyAdvertStatus(lastNearbyAdvert)) {
    return false;
  }
  kitsu868::mesh::RepeatDiagnostics repeatDiagnostics{};
  if (!meshTransport.repeatDiagnostics(repeatDiagnostics)) return false;
  if (repeatDiagnostics.lastFloodTxAvailable &&
      ((repeatDiagnostics.lastFloodTxScoped &&
        (repeatDiagnostics.lastFloodTxTransportCode == 0U ||
         repeatDiagnostics.lastFloodTxTransportCode == 0xffffU)) ||
       (!repeatDiagnostics.lastFloodTxScoped &&
        repeatDiagnostics.lastFloodTxTransportCode != 0U))) {
    return false;
  }
  if (repeatDiagnostics.lastChannelAvailable &&
      (repeatDiagnostics.lastPathHashSize < 1U ||
       repeatDiagnostics.lastPathHashSize > 3U ||
       repeatDiagnostics.lastPathCount == 0U ||
       static_cast<size_t>(repeatDiagnostics.lastPathHashSize) *
               repeatDiagnostics.lastPathCount !=
           repeatDiagnostics.lastPathBytes ||
       repeatDiagnostics.lastPathBytes >
           kitsu868::mesh::kRepeatDiagnosticPathBytes ||
       repeatDiagnosticResultName(repeatDiagnostics.lastResult) == nullptr)) {
    return false;
  }
  if (repeatDiagnostics.lastAdvertAvailable &&
      (repeatDiagnostics.lastAdvertPathHashSize < 1U ||
       repeatDiagnostics.lastAdvertPathHashSize > 3U ||
       repeatDiagnostics.lastAdvertPathCount == 0U ||
       static_cast<size_t>(repeatDiagnostics.lastAdvertPathHashSize) *
               repeatDiagnostics.lastAdvertPathCount !=
           repeatDiagnostics.lastAdvertPathBytes ||
       repeatDiagnostics.lastAdvertPathBytes >
           kitsu868::mesh::kRepeatDiagnosticPathBytes ||
       repeatDiagnosticResultName(repeatDiagnostics.lastAdvertResult) ==
           nullptr)) {
    return false;
  }
  const kitsu868::discovery::JournalStatus journal = discoveryJournal.status();
  const kitsu868::connectivity::DeviceSecurityStatus security =
      deviceSecurity.status();
  const kitsu868::CompanionMood mood =
      companionBrain.mood(companionVitals());
  output.reserve(5200U);
  output = "{\"schema\":\"kitsu.state.v1\",\"device_uid\":\"";
  output += wisp.uid;
  output += "\",\"firmware_version\":\"";
  output += FIRMWARE_VERSION;
  output += "\",\"companion\":\"";
  output += jsonEscaped(companionName());
  output += "\",\"energy\":";
  output += String(wisp.energy);
  output += ",\"curiosity\":";
  output += String(wisp.curiosity);
  output += ",\"affection\":";
  output += String(wisp.affection);
  output += ",\"sleeping\":";
  output += wisp.sleeping ? "true" : "false";
  output += ",\"listening\":";
  output += radioListening ? "true" : "false";
  output += ",\"mood\":\"";
  output += kitsu868::CompanionBrain::moodLabel(mood);
  output += "\",\"battery_percent\":";
  if (battery.present) {
    output += String(battery.percent);
  } else {
    output += "null";
  }
  output += ",\"battery_mv\":";
  if (battery.present) {
    output += String(battery.millivolts);
  } else {
    output += "null";
  }
  output += ",\"pack_ready\":";
  output += companionPack.valid() ? "true" : "false";
  output += ",\"pack_id\":";
  output += String(companionPack.id());
  output += ",\"pack_revision\":";
  output += String(companionPack.revision());
  output += ",\"bond_level\":";
  output += String(companionBrain.bondLevel());
  output += ",\"bond_xp\":";
  output += String(companionBrain.bondXp());
  output += ",\"bond_progress_percent\":";
  output += String(companionBrain.bondProgressPercent());
  output += ",\"evolution_stage\":\"";
  output += kitsu868::CompanionBrain::stageLabel(
      companionBrain.evolutionStage());
  output += "\",\"appearance_variant\":";
  output += String(companionBrain.appearanceVariant());
  output += ",\"personality\":\"";
  output += kitsu868::CompanionBrain::personalityLabel(
      companionBrain.personality().kind);
  output += "\",\"unlock_mask\":";
  output += String(companionBrain.unlockMask());
  output += ",\"signal_trail_misses\":";
  output += String(signalTrail.missCount());
  output += ",\"signal_trail_maximum\":";
  output += String(kitsu868::signal::kSignalTrailMaximumMisses);
  output += ",\"signal_trail_hint\":\"";
  output += kitsu868::signal::signalTrailHintName(signalTrail.hint());
  output += "\",\"signal_trail_guaranteed_next\":";
  output += signalTrail.nextEligibleGuaranteed() ? "true" : "false";
  output += ",\"catalog_seen\":";
  output += String(kitsu868::fun::seenCreatureCount(funDiscovery));
  output += ",\"dream_count\":";
  output += String(funDiscovery.completedDreams);
  output += ",\"rare_reaction_count\":";
  output += String(funDiscovery.rareReactions);
  output += ",\"session_goals_complete_mask\":";
  output += String(sessionChallenges.completedMask);
  output += ",\"session_aura_active\":";
  output += sessionAuraActive ? "true" : "false";
  output += ",\"memory_count\":";
  output += String(companionBrain.memoryCount());
  output += ",\"mesh_rx_ready\":";
  output += meshTransport.active() ? "true" : "false";
  output += ",\"mesh_enabled\":";
  output += meshSettings.enabled ? "true" : "false";
  output += ",\"mesh_time_valid\":";
  output += meshTransport.timeValid() ? "true" : "false";
  output += ",\"mesh_identity_ready\":";
  output += meshTransport.identityReady() ? "true" : "false";
  output += ",\"mesh_advertise_ready\":";
  output += advertiseError ? "false" : "true";
  output += ",\"mesh_advertise_retry_after_ms\":";
  output += String(advertiseRetryAfterMs);
  output += ",\"mesh_advertise_error\":";
  if (advertiseError) {
    output += '"';
    output += advertiseError;
    output += '"';
  } else {
    output += "null";
  }
  output += ",\"mesh_last_flood_advert\":";
  if (!hasLastFloodAdvert) {
    output += "null";
  } else {
    output += "{\"emitted_at\":";
    output += String(lastFloodAdvert.emittedAt);
    output += ",\"state\":\"";
    output += advertTransmitStateName(lastFloodAdvert.state);
    output += "\",\"repeat_count\":";
    if (lastFloodAdvert.repeatCountKnown) {
      output += String(lastFloodAdvert.repeatCount);
    } else {
      output += "null";
    }
    output += ",\"observation_open\":";
    output += lastFloodAdvert.observationOpen ? "true" : "false";
    output += '}';
  }
  // Preserve mesh_last_flood_advert's strict legacy object byte shape. The
  // versioned sibling adds bounded returned-path evidence for 0.16.1+ clients.
  output += ",\"mesh_last_flood_advert_v2\":";
  if (!hasLastFloodAdvert) {
    output += "null";
  } else {
    output += "{\"emitted_at\":";
    output += String(lastFloodAdvert.emittedAt);
    output += ",\"state\":\"";
    output += advertTransmitStateName(lastFloodAdvert.state);
    output += "\",\"repeat_count\":";
    if (lastFloodAdvert.repeatCountKnown) {
      output += String(lastFloodAdvert.repeatCount);
    } else {
      output += "null";
    }
    output += ",\"observation_open\":";
    output += lastFloodAdvert.observationOpen ? "true" : "false";
    output += ",\"repeat_sources\":";
    if (!lastFloodAdvert.repeatCountKnown) {
      output += "null,\"repeat_sources_truncated\":null";
    } else {
      output += '[';
      for (uint8_t index = 0U; index < lastFloodAdvert.sourceCount;
           ++index) {
        if (index != 0U) output += ',';
        output += "{\"last_hop_token\":\"";
        appendUpperHex(output, lastFloodAdvert.sources[index].token,
                       lastFloodAdvert.sources[index].tokenBytes);
        output += "\"}";
      }
      output += "],\"repeat_sources_truncated\":";
      output += lastFloodAdvert.sourcesTruncated ? "true" : "false";
    }
    output += '}';
  }
  output += ",\"mesh_last_nearby_advert\":";
  if (!hasLastNearbyAdvert) {
    output += "null";
  } else {
    output += "{\"emitted_at\":";
    output += String(lastNearbyAdvert.emittedAt);
    output += ",\"state\":\"";
    output += advertTransmitStateName(lastNearbyAdvert.state);
    output += "\",\"repeat_count\":null,\"observation_open\":false}";
  }
  output += ",\"mesh_repeat_diagnostics\":{\"tx_done\":";
  output += String(repeatDiagnostics.txDoneFrames);
  output += ",\"tx_failed\":";
  output += String(repeatDiagnostics.txFailedFrames);
  output += ",\"rx_ready_after_tx\":";
  output += String(repeatDiagnostics.rxReadyAfterTx);
  output += ",\"physical_rx_confirmed_after_tx\":";
  output += String(repeatDiagnostics.physicalRxConfirmedAfterTx);
  output += ",\"sync_turnaround_completed\":";
  output += String(repeatDiagnostics.syncTurnaroundCompleted);
  output += ",\"sync_turnaround_start_failures\":";
  output += String(repeatDiagnostics.syncTurnaroundStartFailures);
  output += ",\"sync_turnaround_timeouts\":";
  output += String(repeatDiagnostics.syncTurnaroundTimeouts);
  output += ",\"rx_rearm_attempts\":";
  output += String(repeatDiagnostics.rxRearmAttempts);
  output += ",\"rx_rearm_retries\":";
  output += String(repeatDiagnostics.rxRearmRetries);
  output += ",\"rx_rearm_failures\":";
  output += String(repeatDiagnostics.rxRearmFailures);
  output += ",\"last_rx_start_attempts\":";
  output += String(repeatDiagnostics.lastRxStartAttempts);
  output += ",\"last_rx_start_code\":";
  if (!repeatDiagnostics.lastRxStartCodeAvailable) {
    output += "null";
  } else {
    output += String(repeatDiagnostics.lastRxStartCode);
  }
  output += ",\"last_rx_start_software_state\":";
  if (!repeatDiagnostics.lastRxStartCodeAvailable) {
    output += "null";
  } else {
    output += repeatDiagnostics.lastRxStartSoftwareState ? "true" : "false";
  }
  output += ",\"last_rx_chip_status_available\":";
  output += repeatDiagnostics.lastRxChipStatusAvailable ? "true" : "false";
  output += ",\"last_rx_chip_status\":";
  if (!repeatDiagnostics.lastRxChipStatusAvailable) {
    output += "null";
  } else {
    const uint8_t status = repeatDiagnostics.lastRxChipStatus;
    output += "\"";
    appendUpperHex(output, &status, 1U);
    output += "\"";
  }
  output += ",\"last_rx_chip_mode\":";
  if (!repeatDiagnostics.lastRxChipStatusAvailable) {
    output += "null";
  } else {
    output += String(kitsu868::mesh::sx126xChipMode(
        repeatDiagnostics.lastRxChipStatus));
  }
  output += ",\"last_tx_done_to_start_receive_us\":";
  if (!repeatDiagnostics.lastTxDoneToStartReceiveMicrosAvailable) {
    output += "null";
  } else {
    output += String(repeatDiagnostics.lastTxDoneToStartReceiveMicros);
  }
  output += ",\"last_tx_done_to_rx_confirmed_us\":";
  if (!repeatDiagnostics.lastTxDoneToRxConfirmedMicrosAvailable) {
    output += "null";
  } else {
    output += String(repeatDiagnostics.lastTxDoneToRxConfirmedMicros);
  }
  output += ",\"current_rx_software_state\":";
  output += repeatDiagnostics.currentRxSoftwareState ? "true" : "false";
  output += ",\"current_rx_chip_status_available\":";
  output += repeatDiagnostics.currentRxChipStatusAvailable ? "true" : "false";
  output += ",\"current_rx_chip_status\":";
  if (!repeatDiagnostics.currentRxChipStatusAvailable) {
    output += "null";
  } else {
    const uint8_t status = repeatDiagnostics.currentRxChipStatus;
    output += "\"";
    appendUpperHex(output, &status, 1U);
    output += "\"";
  }
  output += ",\"current_rx_chip_mode\":";
  if (!repeatDiagnostics.currentRxChipStatusAvailable) {
    output += "null";
  } else {
    output += String(kitsu868::mesh::sx126xChipMode(
        repeatDiagnostics.currentRxChipStatus));
  }
  appendReceiveObservability(output, repeatDiagnostics);
  output += ",\"scoped_flood_tx_done\":";
  output += String(repeatDiagnostics.scopedFloodTxDoneFrames);
  output += ",\"unscoped_flood_tx_done\":";
  output += String(repeatDiagnostics.unscopedFloodTxDoneFrames);
  output += ",\"raw_frames\":";
  output += String(repeatDiagnostics.rawFrames);
  output += ",\"parsed_frames\":";
  output += String(repeatDiagnostics.parsedFrames);
  output += ",\"raw_rejected\":";
  output += String(repeatDiagnostics.rawRejected);
  output += ",\"channel_forward_candidates\":";
  output += String(repeatDiagnostics.channelForwardCandidates);
  output += ",\"channel_hash_matches\":";
  output += String(repeatDiagnostics.channelHashMatches);
  output += ",\"channel_wire_mismatches\":";
  output += String(repeatDiagnostics.channelWireMismatches);
  output += ",\"channel_digest_mismatches\":";
  output += String(repeatDiagnostics.channelDigestMismatches);
  output += ",\"channel_exact_matches\":";
  output += String(repeatDiagnostics.channelExactMatches);
  output += ",\"channel_recorded\":";
  output += String(repeatDiagnostics.channelRecorded);
  output += ",\"channel_saturated\":";
  output += String(repeatDiagnostics.channelSaturated);
  output += ",\"advert_forward_candidates\":";
  output += String(repeatDiagnostics.advertForwardCandidates);
  output += ",\"advert_hash_matches\":";
  output += String(repeatDiagnostics.advertHashMatches);
  output += ",\"advert_wire_mismatches\":";
  output += String(repeatDiagnostics.advertWireMismatches);
  output += ",\"advert_digest_mismatches\":";
  output += String(repeatDiagnostics.advertDigestMismatches);
  output += ",\"advert_exact_matches\":";
  output += String(repeatDiagnostics.advertExactMatches);
  output += ",\"advert_recorded\":";
  output += String(repeatDiagnostics.advertRecorded);
  output += ",\"advert_saturated\":";
  output += String(repeatDiagnostics.advertSaturated);
  output += ",\"last_flood_tx\":";
  if (!repeatDiagnostics.lastFloodTxAvailable) {
    output += "null";
  } else {
    output += "{\"payload_type\":";
    output += String(repeatDiagnostics.lastFloodTxPayloadType);
    output += ",\"scoped\":";
    output += repeatDiagnostics.lastFloodTxScoped ? "true" : "false";
    output += ",\"scope\":";
    if (repeatDiagnostics.lastFloodTxScoped) {
      output += "\"";
      output += kitsu868::mesh::kDefaultTransportScopeName;
      output += "\"";
    } else {
      output += "null";
    }
    output += ",\"transport_code\":";
    if (repeatDiagnostics.lastFloodTxScoped) {
      const uint8_t codeBytes[2] = {
          static_cast<uint8_t>(repeatDiagnostics.lastFloodTxTransportCode),
          static_cast<uint8_t>(repeatDiagnostics.lastFloodTxTransportCode >>
                               8U)};
      output += "\"";
      appendUpperHex(output, codeBytes, sizeof(codeBytes));
      output += "\"";
    } else {
      output += "null";
    }
    output += ",\"scope_tag\":";
    if (repeatDiagnostics.lastFloodTxScoped) {
      output += "\"";
      output += kitsu868::mesh::kDefaultTransportScopeTag;
      output += "\"";
    } else {
      output += "null";
    }
    output += ",\"scope_key_fingerprint\":";
    if (repeatDiagnostics.lastFloodTxScoped) {
      output += "\"";
      output += kitsu868::mesh::kDefaultTransportScopeKeyFingerprint;
      output += "\"";
    } else {
      output += "null";
    }
    output += '}';
  }
  output += ",\"last_channel\":";
  if (!repeatDiagnostics.lastChannelAvailable) {
    output += "null";
  } else {
    output += "{\"packet_hash\":\"";
    appendUpperHex(output, repeatDiagnostics.lastChannelHash,
                   sizeof(repeatDiagnostics.lastChannelHash));
    output += "\",\"path\":\"";
    appendUpperHex(output, repeatDiagnostics.lastPath,
                   repeatDiagnostics.lastPathBytes);
    output += "\",\"last_hop_token\":\"";
    const size_t lastTokenAt = repeatDiagnostics.lastPathBytes -
        repeatDiagnostics.lastPathHashSize;
    appendUpperHex(output, repeatDiagnostics.lastPath + lastTokenAt,
                   repeatDiagnostics.lastPathHashSize);
    output += "\",\"path_hash_bytes\":";
    output += String(repeatDiagnostics.lastPathHashSize);
    output += ",\"path_count\":";
    output += String(repeatDiagnostics.lastPathCount);
    output += ",\"rssi_dbm\":";
    output += String(repeatDiagnostics.lastRssi, 1);
    output += ",\"snr_db\":";
    output += String(repeatDiagnostics.lastSnr, 1);
    output += ",\"result\":\"";
    output += repeatDiagnosticResultName(repeatDiagnostics.lastResult);
    output += "\"}";
  }
  output += ",\"last_advert\":";
  if (!repeatDiagnostics.lastAdvertAvailable) {
    output += "null";
  } else {
    output += "{\"packet_hash\":\"";
    appendUpperHex(output, repeatDiagnostics.lastAdvertHash,
                   sizeof(repeatDiagnostics.lastAdvertHash));
    output += "\",\"path\":\"";
    appendUpperHex(output, repeatDiagnostics.lastAdvertPath,
                   repeatDiagnostics.lastAdvertPathBytes);
    output += "\",\"last_hop_token\":\"";
    const size_t lastTokenAt = repeatDiagnostics.lastAdvertPathBytes -
        repeatDiagnostics.lastAdvertPathHashSize;
    appendUpperHex(output, repeatDiagnostics.lastAdvertPath + lastTokenAt,
                   repeatDiagnostics.lastAdvertPathHashSize);
    output += "\",\"path_hash_bytes\":";
    output += String(repeatDiagnostics.lastAdvertPathHashSize);
    output += ",\"path_count\":";
    output += String(repeatDiagnostics.lastAdvertPathCount);
    output += ",\"rssi_dbm\":";
    output += String(repeatDiagnostics.lastAdvertRssi, 1);
    output += ",\"snr_db\":";
    output += String(repeatDiagnostics.lastAdvertSnr, 1);
    output += ",\"result\":\"";
    output += repeatDiagnosticResultName(repeatDiagnostics.lastAdvertResult);
    output += "\"}";
  }
  output += '}';
  output += ",\"mesh_tx_unlocked\":";
  output += meshTransport.transmitAllowed(meshSettings) ? "true" : "false";
  output += ",\"mesh_one_shot_ready\":";
  output += meshTransport.active() && meshSettings.enabled &&
                    meshSettings.txPolicy ==
                        kitsu868::mesh::TxPolicy::ExplicitSession &&
                    meshTransport.timeValid()
                ? "true"
                : "false";
  output += ",\"journal_ready\":";
  output += discoveryJournalReady ? "true" : "false";
  output += ",\"peer_count\":";
  output += String(journal.peerCount);
  output += ",\"event_count\":";
  output += String(journal.eventCount);
  output += ",\"controller_count\":";
  output += String(security.controllerCount);
  output += ",\"security_mode\":\"";
  output += kitsu868::connectivity::kSelectedSecurityModeName;
  output += "\",\"secure_boot\":false,\"flash_encryption\":false";
  output += ",\"nvs_encryption\":false";
  output += ",\"hardware_root_protected\":false";
  output += ",\"application_encrypted\":true";
  output += '}';
  return output.length() <=
      kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

void appendObservationTime(String& output,
                           const kitsu868::discovery::ObservationTime& time) {
  output += "{\"epoch_valid\":";
  output += time.epochValid ? "true" : "false";
  output += ",\"epoch\":";
  output += String(time.epoch);
  output += ",\"boot_id\":\"";
  output += String(time.bootId);
  output += "\",\"millis\":";
  output += String(time.millis);
  output += '}';
}

void appendSignal(String& output,
                  const kitsu868::discovery::LastHopSignal& signal) {
  output += "{\"valid\":";
  output += signal.valid ? "true" : "false";
  output += ",\"rssi\":";
  output += String(signal.rssi, 1);
  output += ",\"snr\":";
  output += String(signal.snr, 1);
  output += '}';
}

bool buildHistory(const uint8_t* payload, size_t payloadBytes,
                  String& output) {
  CursorQuery query{};
  if (!discoveryJournalReady ||
      !parseCursorQuery(payload, payloadBytes, query)) {
    return false;
  }
  output.reserve(7000U);
  output = "{\"schema\":\"kitsu.history.v1\",\"items\":[";
  const kitsu868::discovery::JournalStatus journal = discoveryJournal.status();
  // A null cursor starts immediately before the oldest retained serial.  This
  // keeps first-page reads correct across uint32 rollover as well as at boot.
  uint32_t cursor = query.hasAfter
                        ? query.after
                        : journal.newestSequence - journal.eventCount;
  bool hasCursor = query.hasAfter;
  uint32_t firstSequence = 0U;
  uint8_t count = 0U;
  kitsu868::discovery::DiscoveryEvent event{};
  while (count < query.limit && journal.eventCount != 0U &&
         discoveryJournal.eventAfter(cursor, event)) {
    if (count == 0U) firstSequence = event.sequence;
    if (count != 0U) output += ',';
    const String key = publicKeyBase64(event.publicKey);
    if (key.length() != 43U) return false;
    output += "{\"sequence\":\"";
    output += String(event.sequence);
    output += "\",\"public_key_b64\":\"";
    output += key;
    output += "\",\"sender_advert_timestamp\":";
    output += String(event.senderAdvertTimestamp);
    output += ",\"observed\":";
    appendObservationTime(output, event.observed);
    output += ",\"last_hop\":";
    appendSignal(output, event.lastHop);
    output += '}';
    cursor = event.sequence;
    hasCursor = true;
    ++count;
  }
  kitsu868::discovery::DiscoveryEvent next{};
  const bool hasMore = hasCursor && discoveryJournal.eventAfter(cursor, next);
  const bool gap = query.hasAfter && count != 0U &&
                   firstSequence != query.after + 1U;
  output += "],\"cursor\":";
  appendCursor(output, hasCursor, cursor);
  output += ",\"has_more\":";
  output += hasMore ? "true" : "false";
  output += ",\"gap\":";
  output += gap ? "true" : "false";
  output += '}';
  return true;
}

bool nextPeerAfter(bool hasCursor, uint32_t cursor,
                   kitsu868::discovery::DiscoveryPeer& output) {
  bool found = false;
  for (size_t ordinal = 0U;
       ordinal < kitsu868::discovery::kDiscoveryPeerCapacity; ++ordinal) {
    kitsu868::discovery::DiscoveryPeer candidate{};
    if (!discoveryJournal.peerAt(ordinal, candidate) ||
        (hasCursor && !sequenceAfter(candidate.lastSequence, cursor))) {
      continue;
    }
    if (!found || sequenceAfter(output.lastSequence,
                                candidate.lastSequence)) {
      output = candidate;
      found = true;
    }
  }
  return found;
}

bool buildPeers(const uint8_t* payload, size_t payloadBytes, String& output) {
  CursorQuery query{};
  if (!discoveryJournalReady ||
      !parseCursorQuery(payload, payloadBytes, query)) {
    return false;
  }
  output.reserve(8000U);
  output = "{\"schema\":\"kitsu.peers.v1\",\"items\":[";
  uint32_t cursor = query.after;
  bool hasCursor = query.hasAfter;
  uint8_t count = 0U;
  kitsu868::discovery::DiscoveryPeer peer{};
  while (count < query.limit && nextPeerAfter(hasCursor, cursor, peer)) {
    if (count != 0U) output += ',';
    const String key = publicKeyBase64(peer.publicKey);
    if (key.length() != 43U) return false;
    output += "{\"public_key_b64\":\"";
    output += key;
    output += "\",\"name\":\"";
    output += jsonEscaped(String(peer.name));
    output += "\",\"type\":";
    output += String(peer.type);
    output += ",\"kitsu_named\":";
    output += peer.kitsuNamed ? "true" : "false";
    output += ",\"has_location\":";
    output += peer.hasLocation ? "true" : "false";
    output += ",\"lat_e6\":";
    output += String(peer.latitudeE6);
    output += ",\"lon_e6\":";
    output += String(peer.longitudeE6);
    output += ",\"sender_advert_timestamp\":";
    output += String(peer.senderAdvertTimestamp);
    output += ",\"last_observed\":";
    appendObservationTime(output, peer.lastObserved);
    output += ",\"last_hop\":";
    appendSignal(output, peer.lastHop);
    output += ",\"last_sequence\":\"";
    output += String(peer.lastSequence);
    output += "\",\"sighting_count\":";
    output += String(peer.sightingCount);
    output += '}';
    cursor = peer.lastSequence;
    hasCursor = true;
    ++count;
  }
  kitsu868::discovery::DiscoveryPeer next{};
  const bool hasMore = nextPeerAfter(hasCursor, cursor, next);
  output += "],\"cursor\":";
  appendCursor(output, hasCursor, cursor);
  output += ",\"has_more\":";
  output += hasMore ? "true" : "false";
  output += ",\"gap\":false}";
  return true;
}

bool buildChannels(const uint8_t* payload, size_t payloadBytes,
                   String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  output.reserve(360U);
  output = "{\"schema\":\"kitsu.channels.v1\",\"items\":[";
  for (uint8_t slot = 0U; slot < kitsu868::mesh::kMeshChannelCapacity;
       ++slot) {
    if (slot != 0U) output += ',';
    kitsu868::mesh::ChannelRecord channel{};
    const bool available = meshTransport.channelAt(slot, channel);
    output += "{\"slot\":";
    output += String(slot);
    output += ",\"configured\":";
    output += available && channel.configured ? "true" : "false";
    output += ",\"name\":";
    if (available && channel.configured) {
      output += '"';
      output += jsonEscaped(String(channel.name));
      output += '"';
    } else {
      output += "null";
    }
    output += '}';
  }
  output += "]}";
  return true;
}

bool buildChannelsV2(const uint8_t* payload, size_t payloadBytes,
                     String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  output.reserve(440U);
  output = "{\"schema\":\"kitsu.channels.v2\",\"items\":[";
  for (uint8_t slot = 0U; slot < kitsu868::mesh::kMeshChannelCapacity;
       ++slot) {
    if (slot != 0U) output += ',';
    kitsu868::mesh::ChannelRecord channel{};
    const bool available = meshTransport.channelAt(slot, channel);
    output += "{\"slot\":";
    output += String(slot);
    output += ",\"configured\":";
    output += available && channel.configured ? "true" : "false";
    output += ",\"name\":";
    if (available && channel.configured) {
      output += '"';
      output += jsonEscaped(String(channel.name));
      output += '"';
    } else {
      output += "null";
    }
    output += ",\"region_scope\":";
    if (available && channel.configured &&
        channel.regionScope == kitsu868::mesh::ChannelRegionScope::Eu) {
      output += "\"EU\"";
    } else {
      output += "null";
    }
    output += '}';
  }
  output += "]}";
  return true;
}

bool buildChatStorage(const uint8_t* payload, size_t payloadBytes,
                      String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  kitsu868::mesh::MessagingStorageStatus status{};
  if (!meshTransport.messagingStorageStatus(status)) return false;
  output.reserve(280U);
  output = "{\"schema\":\"kitsu.chat-storage.v1\",\"usable\":";
  output += status.usable ? "true" : "false";
  output += ",\"persisted_schema\":";
  if (status.persistedSchema == 0U) output += "null";
  else output += String(status.persistedSchema);
  output += ",\"migration_pending\":";
  output += status.migrationPending ? "true" : "false";
  output += ",\"cleanup_pending\":";
  output += status.cleanupPending ? "true" : "false";
  output += ",\"generation\":";
  if (status.generation == 0U) output += "null";
  else output += String(static_cast<unsigned long>(status.generation));
  output += ",\"writable_last_result\":";
  if (status.lastWriteResult ==
      kitsu868::mesh::MessagingStorageWriteResult::NotAttempted) {
    output += "null";
  } else {
    output += '"';
    output += kitsu868::mesh::messagingStorageWriteResultName(
        status.lastWriteResult);
    output += '"';
  }
  output += ",\"reason\":\"";
  output += kitsu868::mesh::messagingStorageReasonName(status.reason);
  output += "\"}";
  return true;
}

bool syncClock(const uint8_t* payload, size_t payloadBytes, String& output) {
  uint32_t epoch = 0U;
  if (!parseClockSync(payload, payloadBytes, epoch)) return false;
  const uint32_t now = millis();
  const kitsu868::timekeeping::KitsuClock before = kitsuClock;
  const uint32_t previousGeneration = clockSnapshotGeneration;
  const uint8_t previousSlot = clockSnapshotSlot;
  kitsu868::timekeeping::ClockReading current{};
  const bool currentTrusted = clockReading(now, current, true);
  const uint64_t requested = static_cast<uint64_t>(epoch);
  const uint64_t delta = currentTrusted
      ? (current.unixSeconds >= requested
             ? current.unixSeconds - requested
             : requested - current.unixSeconds)
      : UINT64_MAX;
  const kitsu868::timekeeping::ClockResult setResult =
      kitsuClock.setFromUnixSeconds(
          epoch, 0U, kitsu868::timekeeping::ClockSource::AuthenticatedApp,
          kitsuClock.utcOffsetMinutes(), now);
  if (setResult != kitsu868::timekeeping::ClockResult::Ok) {
    kitsuClock = before;
    output = "{\"ok\":false,\"error\":\"clock_value_rejected\"}";
    return true;
  }

  if (delta <= CLOCK_SYNC_TOLERANCE_SECONDS) {
    // Keep the runtime anchor exact, but do not burn another alternating NVS
    // slot for the same trusted time on every Android reconnect/action.
    if (!applyClockToRuntime(now)) {
      kitsuClock = before;
      if (before.trusted() && !applyClockToRuntime(now)) {
        Serial.println("KITSU_WARN clock_runtime=rollback_failed");
      }
      output = "{\"ok\":false,\"error\":\"clock_runtime_failed\"}";
      return true;
    }
  } else {
    if (!clockStateReady) {
      kitsuClock = before;
      output = "{\"ok\":false,\"error\":\"clock_storage_failed\"}";
      return true;
    }
    const char* error = nullptr;
    if (!commitClockMutation(before, previousGeneration, previousSlot, now,
                             true, &error)) {
      output = "{\"ok\":false,\"error\":\"";
      output += error ? error : "clock_runtime_failed";
      output += "\"}";
      return true;
    }
  }
  output = "{\"schema\":\"kitsu.clock.v1\",\"epoch\":";
  output += String(epoch);
  output += '}';
  return true;
}

bool configureMesh(const uint8_t* payload, size_t payloadBytes,
                   String& output) {
  bool enabled = false;
  if (!parseMeshEnabled(payload, payloadBytes, enabled)) return false;
  kitsu868::mesh::Settings candidate = meshSettings;
  candidate.enabled = enabled;
  candidate.radio = kitsu868::mesh::ukEuNarrowProfile();
  candidate.txPolicy = enabled
                           ? kitsu868::mesh::TxPolicy::ExplicitSession
                           : kitsu868::mesh::TxPolicy::Locked;
  if (!enabled) kitsu868::mesh::hideLocation(candidate);
  const char* error = nullptr;
  if (!commitMeshRadioSettings(candidate, error)) {
    output = "{\"ok\":false,\"error\":\"";
    output += error ? error : "mesh_config_failed";
    output += "\"}";
    return true;
  }
  if (!enabled) {
    kitsu868::mesh::clearCurrentLocationOnce(meshCurrentLocation);
  }
  output = "{\"schema\":\"kitsu.mesh-config.v1\",\"enabled\":";
  output += enabled ? "true" : "false";
  output += ",\"profile\":\"uk_eu_narrow\",\"tx_power_dbm\":";
  output += String(meshSettings.radio.txPowerDbm);
  output += '}';
  return true;
}

constexpr uint8_t kMessagesV1PageItems = 8U;
constexpr uint8_t kMessagesV2PageItems = 12U;
constexpr uint8_t kMessagesV3PageItems = 12U;
constexpr uint8_t kMessagesV4PageItems = 8U;
// The final cursor/has_more/gap object is shorter than this even with a
// UINT32_MAX cursor. Keeping the reserve explicit lets item admission remain
// byte-bounded rather than relying only on a record-count estimate.
constexpr size_t kMessagesPageTailReserveBytes = 96U;

bool buildMessagesV1(const uint8_t* payload, size_t payloadBytes,
                      String& output) {
  CursorQuery query{};
  if (!parseCursorQuery(payload, payloadBytes, query)) return false;
  output.reserve(6000U);
  output = "{\"schema\":\"kitsu.messages.v1\",\"items\":[";
  uint32_t cursor = query.after;
  uint32_t firstReturnedId = 0U;
  bool hasCursor = query.hasAfter;
  uint8_t count = 0U;
  bool hasMore = false;
  for (uint8_t ordinal = 0U; ordinal < chatJournalCount; ++ordinal) {
    const uint8_t index = static_cast<uint8_t>(
        (chatJournalStart + ordinal) % kitsu868::chat::kInboxCapacity);
    const ChatJournalEntry& entry = chatJournal[index];
    if (query.hasAfter && !sequenceAfter(entry.id, query.after)) continue;
    if (count >= query.limit || count >= kMessagesV1PageItems) {
      hasMore = true;
      break;
    }
    if (count != 0U) output += ',';
    if (count == 0U) firstReturnedId = entry.id;
    output += "{\"message_id\":\"";
    output += String(entry.id);
    output += "\",\"timestamp\":";
    output += String(entry.timestamp);
    output += ",\"inbound\":";
    output += entry.inbound ? "true" : "false";
    output += ",\"kind\":\"";
    output += entry.kind == kitsu868::mesh::MessageKind::Direct
                  ? "direct"
                  : "channel";
    output += "\",\"peer_id\":";
    if (entry.kind == kitsu868::mesh::MessageKind::Direct) {
      const String key = publicKeyBase64(entry.contactPublicKey);
      if (key.length() != 43U) return false;
      output += '"';
      output += key;
      output += '"';
    } else {
      output += "null";
    }
    output += ",\"channel_slot\":";
    if (entry.kind == kitsu868::mesh::MessageKind::Channel) {
      output += String(entry.channelSlot);
    } else {
      output += "null";
    }
    output += ",\"authenticated\":";
    output += entry.authenticated ? "true" : "false";
    output += ",\"sender_name\":\"";
    output += jsonEscaped(String(entry.senderName));
    output += "\",\"text\":\"";
    output += jsonEscaped(String(entry.text));
    output += "\",\"state\":\"";
    output += chatStateName(entry.state);
    output += "\"}";
    cursor = entry.id;
    hasCursor = true;
    ++count;
  }
  output += "],\"cursor\":";
  appendCursor(output, hasCursor, cursor);
  output += ",\"has_more\":";
  output += hasMore ? "true" : "false";
  output += ",\"gap\":";
  uint32_t expectedFirstId = query.after + 1U;
  if (expectedFirstId == 0U) expectedFirstId = 1U;
  const bool gap = query.hasAfter && count != 0U &&
      firstReturnedId != expectedFirstId;
  output += gap ? "true" : "false";
  output += '}';
  return output.length() <=
      kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

const ChatJournalEntry* nextMessageById(bool hasAfter, uint32_t after) {
  const ChatJournalEntry* selected = nullptr;
  for (uint8_t ordinal = 0U; ordinal < chatJournalCount; ++ordinal) {
    const uint8_t index = static_cast<uint8_t>(
        (chatJournalStart + ordinal) % kitsu868::chat::kInboxCapacity);
    const ChatJournalEntry& candidate = chatJournal[index];
    if (candidate.journalSession != chatSession) continue;
    if (hasAfter && !sequenceAfter(candidate.id, after)) continue;
    if (!selected || sequenceAfter(selected->id, candidate.id)) {
      selected = &candidate;
    }
  }
  return selected;
}

bool appendMessageV2Item(const ChatJournalEntry& entry, String& output) {
  output = "{\"message_id\":\"";
  output += String(entry.id);
  output += "\",\"revision\":\"";
  output += String(entry.revision);
  output += "\",\"timestamp\":";
  output += String(entry.timestamp);
  output += ",\"inbound\":";
  output += entry.inbound ? "true" : "false";
  output += ",\"kind\":\"";
  output += entry.kind == kitsu868::mesh::MessageKind::Direct
                ? "direct"
                : "channel";
  output += "\",\"peer_id\":";
  if (entry.kind == kitsu868::mesh::MessageKind::Direct) {
    const String key = publicKeyBase64(entry.contactPublicKey);
    if (key.length() != 43U) return false;
    output += '"';
    output += key;
    output += '"';
  } else {
    output += "null";
  }
  output += ",\"channel_slot\":";
  if (entry.kind == kitsu868::mesh::MessageKind::Channel) {
    output += String(entry.channelSlot);
  } else {
    output += "null";
  }
  output += ",\"authenticated\":";
  output += entry.authenticated ? "true" : "false";
  output += ",\"unread\":";
  output += entry.unread ? "true" : "false";
  output += ",\"sender_name\":\"";
  output += jsonEscaped(String(entry.senderName));
  output += "\",\"text\":\"";
  output += jsonEscaped(String(entry.text));
  output += "\",\"state\":\"";
  output += chatStateName(entry.state);
  output += "\",\"route\":\"";
  output += entry.route == kitsu868::mesh::MessageRoute::Direct
                ? "direct"
                : "flood";
  output += "\",\"local_tx\":\"";
  output += chatLocalTxName(entry);
  output += "\",\"delivery_ack\":\"";
  output += chatDeliveryAckName(entry);
  output += "\",\"repeater_count\":";
  if (entry.repeaterCountKnown) output += String(entry.repeaterCount);
  else output += "null";
  // MeshCore has no fan-out receipt. A confirmed route count above says how
  // many path relays carried one delivered direct message; it cannot say how
  // many other repeaters heard the RF packet.
  output += ",\"repeaters_heard\":null,\"rssi_dbm\":";
  if (entry.inbound && isfinite(entry.rssi)) output += String(entry.rssi, 1);
  else output += "null";
  output += ",\"snr_db\":";
  if (entry.inbound && isfinite(entry.snr)) output += String(entry.snr, 1);
  else output += "null";
  output += '}';
  return true;
}

bool buildMessagesV2(const uint8_t* payload, size_t payloadBytes,
                     String& output) {
  CursorQuery query{};
  if (!parseCursorQuery(payload, payloadBytes, query)) return false;
  const uint32_t snapshotRevision = chatJournalRevision;
  bool hasCursor = query.hasAfter;
  uint32_t cursor = query.after;
  uint32_t firstReturnedId = 0U;
  const uint8_t pageLimit = query.limit < kMessagesV2PageItems
      ? query.limit
      : kMessagesV2PageItems;

  output.reserve(kitsu868::companion::kMaximumEnvelopePayloadBytes);
  output = "{\"schema\":\"kitsu.messages.v2\",\"journal_session\":\"";
  output += String(chatSession);
  output += "\",\"journal_revision\":\"";
  output += String(snapshotRevision);
  output += "\",\"items\":[";

  uint8_t count = 0U;
  while (count < pageLimit) {
    const ChatJournalEntry* entry = nextMessageById(hasCursor, cursor);
    if (!entry) break;
    String item;
    item.reserve(900U);
    if (!appendMessageV2Item(*entry, item)) return false;
    const size_t commaBytes = count == 0U ? 0U : 1U;
    if (output.length() + commaBytes + item.length() +
            kMessagesPageTailReserveBytes >
        kitsu868::companion::kMaximumEnvelopePayloadBytes) {
      // One protocol-valid item is proven to fit by the host worst-case
      // contract test. Failing here therefore means the bound drifted and is
      // safer than returning a truncated or unparseable snapshot.
      if (count == 0U) return false;
      break;
    }
    if (count != 0U) output += ',';
    if (count == 0U) firstReturnedId = entry->id;
    output += item;
    // Page traversal remains in stable journal/message identity order.
    // Per-item revision is mutation evidence, never a pagination cursor.
    cursor = entry->id;
    hasCursor = true;
    ++count;
  }

  const bool hasMore = nextMessageById(hasCursor, cursor) != nullptr;
  uint32_t expectedFirstId = query.after + 1U;
  if (expectedFirstId == 0U) expectedFirstId = 1U;
  const bool gap = query.hasAfter && count != 0U &&
      firstReturnedId != expectedFirstId;
  output += "],\"cursor\":";
  appendCursor(output, hasCursor, cursor);
  output += ",\"has_more\":";
  output += hasMore ? "true" : "false";
  output += ",\"gap\":";
  output += gap ? "true" : "false";
  output += '}';
  return output.length() <=
      kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

bool appendMessageV3Item(const ChatJournalEntry& entry, String& output) {
  // Keep the v2 serializer untouched for strict 2.1.3 clients. v3 is exactly
  // that item plus one negotiated field placed before the existing
  // repeaters_heard:null compatibility field.
  if (!appendMessageV2Item(entry, output)) return false;
  constexpr char marker[] = ",\"repeaters_heard\":";
  const int markerAt = output.indexOf(marker);
  if (markerAt < 0) return false;
  const String suffix = output.substring(static_cast<unsigned>(markerAt));
  output.remove(static_cast<unsigned>(markerAt));
  output += ",\"repeat_count\":";
  if (!entry.inbound &&
      entry.kind == kitsu868::mesh::MessageKind::Channel &&
      entry.state == ChatJournalState::Sent && entry.repeatCountKnown) {
    output += String(entry.repeatCount);
  } else {
    output += "null";
  }
  output += suffix;
  return true;
}

bool buildMessagesV3(const uint8_t* payload, size_t payloadBytes,
                     String& output) {
  CursorQuery query{};
  if (!parseCursorQuery(payload, payloadBytes, query)) return false;
  const uint32_t snapshotRevision = chatJournalRevision;
  bool hasCursor = query.hasAfter;
  uint32_t cursor = query.after;
  uint32_t firstReturnedId = 0U;
  const uint8_t pageLimit = query.limit < kMessagesV3PageItems
      ? query.limit
      : kMessagesV3PageItems;

  output.reserve(kitsu868::companion::kMaximumEnvelopePayloadBytes);
  output = "{\"schema\":\"kitsu.messages.v3\",\"journal_session\":\"";
  output += String(chatSession);
  output += "\",\"journal_revision\":\"";
  output += String(snapshotRevision);
  output += "\",\"items\":[";

  uint8_t count = 0U;
  while (count < pageLimit) {
    const ChatJournalEntry* entry = nextMessageById(hasCursor, cursor);
    if (!entry) break;
    String item;
    item.reserve(920U);
    if (!appendMessageV3Item(*entry, item)) return false;
    const size_t commaBytes = count == 0U ? 0U : 1U;
    if (output.length() + commaBytes + item.length() +
            kMessagesPageTailReserveBytes >
        kitsu868::companion::kMaximumEnvelopePayloadBytes) {
      if (count == 0U) return false;
      break;
    }
    if (count != 0U) output += ',';
    if (count == 0U) firstReturnedId = entry->id;
    output += item;
    cursor = entry->id;
    hasCursor = true;
    ++count;
  }

  const bool hasMore = nextMessageById(hasCursor, cursor) != nullptr;
  uint32_t expectedFirstId = query.after + 1U;
  if (expectedFirstId == 0U) expectedFirstId = 1U;
  const bool gap = query.hasAfter && count != 0U &&
      firstReturnedId != expectedFirstId;
  output += "],\"cursor\":";
  appendCursor(output, hasCursor, cursor);
  output += ",\"has_more\":";
  output += hasMore ? "true" : "false";
  output += ",\"gap\":";
  output += gap ? "true" : "false";
  output += '}';
  return output.length() <=
      kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

bool validMessageV4RepeatSources(const ChatJournalEntry& entry) {
  if (entry.repeatSourceCount >
      kitsu868::mesh::kChannelRepeatSourceCapacity) {
    return false;
  }
  if (entry.repeatSourceCount > entry.repeatCount) return false;
  if (entry.repeatSourcesTruncated &&
      (entry.repeatSourceCount !=
           kitsu868::mesh::kChannelRepeatSourceCapacity ||
       entry.repeatCount <
           kitsu868::mesh::kChannelRepeatSourceCapacity + 1U)) {
    return false;
  }
  if (entry.repeatCount == 0U && entry.repeatSourcesTruncated) return false;
  for (uint8_t index = 0U; index < entry.repeatSourceCount; ++index) {
    const kitsu868::mesh::ChannelRepeatSource& source =
        entry.repeatSources[index];
    if (source.tokenBytes == 0U ||
        source.tokenBytes >
            kitsu868::mesh::kChannelRepeatSourceTokenBytes) {
      return false;
    }
    for (uint8_t prior = 0U; prior < index; ++prior) {
      const kitsu868::mesh::ChannelRepeatSource& candidate =
          entry.repeatSources[prior];
      if (candidate.tokenBytes == source.tokenBytes &&
          memcmp(candidate.token, source.token, source.tokenBytes) == 0) {
        return false;
      }
    }
  }
  return true;
}

bool appendMessageV4Item(const ChatJournalEntry& entry, String& output) {
  // v1/v2/v3 are immutable compatibility surfaces. v4 extends the exact v3
  // object only at the negotiated operation/schema pair.
  if (!appendMessageV3Item(entry, output)) return false;
  constexpr char marker[] = ",\"repeaters_heard\":";
  const int markerAt = output.indexOf(marker);
  if (markerAt < 0) return false;
  const String suffix = output.substring(static_cast<unsigned>(markerAt));
  output.remove(static_cast<unsigned>(markerAt));

  const bool applicable = !entry.inbound &&
      entry.kind == kitsu868::mesh::MessageKind::Channel &&
      entry.state == ChatJournalState::Sent && entry.repeatCountKnown;
  output += ",\"repeat_observation_open\":";
  if (!applicable) {
    output += "null,\"repeat_sources\":null,"
              "\"repeat_sources_truncated\":null";
    output += suffix;
    return true;
  }
  if (!validMessageV4RepeatSources(entry)) return false;
  output += entry.repeatObservationOpen ? "true" : "false";
  output += ",\"repeat_sources\":[";
  for (uint8_t index = 0U; index < entry.repeatSourceCount; ++index) {
    if (index != 0U) output += ',';
    output += "{\"last_hop_token\":\"";
    appendUpperHex(output, entry.repeatSources[index].token,
                   entry.repeatSources[index].tokenBytes);
    output += "\"}";
  }
  output += "],\"repeat_sources_truncated\":";
  output += entry.repeatSourcesTruncated ? "true" : "false";
  output += suffix;
  return true;
}

bool buildMessagesV4(const uint8_t* payload, size_t payloadBytes,
                     String& output) {
  CursorQuery query{};
  if (!parseCursorQuery(payload, payloadBytes, query)) return false;
  const uint32_t snapshotRevision = chatJournalRevision;
  bool hasCursor = query.hasAfter;
  uint32_t cursor = query.after;
  uint32_t firstReturnedId = 0U;
  const uint8_t pageLimit = query.limit < kMessagesV4PageItems
      ? query.limit
      : kMessagesV4PageItems;

  output.reserve(kitsu868::companion::kMaximumEnvelopePayloadBytes);
  output = "{\"schema\":\"kitsu.messages.v4\",\"journal_session\":\"";
  output += String(chatSession);
  output += "\",\"journal_revision\":\"";
  output += String(snapshotRevision);
  output += "\",\"items\":[";

  uint8_t count = 0U;
  while (count < pageLimit) {
    const ChatJournalEntry* entry = nextMessageById(hasCursor, cursor);
    if (!entry) break;
    String item;
    item.reserve(1050U);
    if (!appendMessageV4Item(*entry, item)) return false;
    const size_t commaBytes = count == 0U ? 0U : 1U;
    if (output.length() + commaBytes + item.length() +
            kMessagesPageTailReserveBytes >
        kitsu868::companion::kMaximumEnvelopePayloadBytes) {
      if (count == 0U) return false;
      break;
    }
    if (count != 0U) output += ',';
    if (count == 0U) firstReturnedId = entry->id;
    output += item;
    cursor = entry->id;
    hasCursor = true;
    ++count;
  }

  const bool hasMore = nextMessageById(hasCursor, cursor) != nullptr;
  uint32_t expectedFirstId = query.after + 1U;
  if (expectedFirstId == 0U) expectedFirstId = 1U;
  const bool gap = query.hasAfter && count != 0U &&
      firstReturnedId != expectedFirstId;
  output += "],\"cursor\":";
  appendCursor(output, hasCursor, cursor);
  output += ",\"has_more\":";
  output += hasMore ? "true" : "false";
  output += ",\"gap\":";
  output += gap ? "true" : "false";
  output += '}';
  return output.length() <=
      kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

void buildMarkReadResponse(bool accepted, const char* error,
                           uint8_t markedCount, uint8_t unchangedCount,
                           String& output) {
  output.reserve(280U);
  output = "{\"schema\":\"kitsu.messages-mark-read.v1\",\"accepted\":";
  output += accepted ? "true" : "false";
  output += ",\"error\":";
  if (error) {
    output += '"';
    output += error;
    output += '"';
  } else {
    output += "null";
  }
  output += ",\"marked_count\":";
  output += String(markedCount);
  output += ",\"unchanged_count\":";
  output += String(unchangedCount);
  output += ",\"journal_session\":\"";
  output += String(chatSession);
  output += "\",\"journal_revision\":\"";
  output += String(chatJournalRevision);
  output += "\"}";
}

bool markMessagesRead(const uint8_t* payload, size_t payloadBytes,
                      String& output) {
  using kitsu868::message_read::Command;
  using kitsu868::message_read::ParseStatus;
  using kitsu868::message_read::Plan;
  using kitsu868::message_read::PlanStatus;
  using kitsu868::message_read::Record;

  Command command{};
  if (kitsu868::message_read::parseCommand(payload, payloadBytes, command) !=
      ParseStatus::Ok) {
    buildMarkReadResponse(false, "request_rejected", 0U, 0U, output);
    return true;
  }

  Record records[kitsu868::message_read::kMaximumMessageIds]{};
  uint8_t journalIndexes[kitsu868::message_read::kMaximumMessageIds]{};
  uint8_t visibleCount = 0U;
  for (uint8_t ordinal = 0U; ordinal < chatJournalCount; ++ordinal) {
    const uint8_t journalIndex = static_cast<uint8_t>(
        (chatJournalStart + ordinal) % kitsu868::chat::kInboxCapacity);
    if (chatJournal[journalIndex].journalSession != chatSession) continue;
    records[visibleCount].messageId = chatJournal[journalIndex].id;
    records[visibleCount].inbound = chatJournal[journalIndex].inbound;
    records[visibleCount].unread = chatJournal[journalIndex].unread;
    journalIndexes[visibleCount] = journalIndex;
    ++visibleCount;
  }

  Plan plan{};
  const PlanStatus status = kitsu868::message_read::plan(
      chatSession, command, records, visibleCount, plan);
  if (status != PlanStatus::Ok) {
    buildMarkReadResponse(false,
                          kitsu868::message_read::planStatusError(status),
                          0U, 0U, output);
    return true;
  }

  uint8_t selected[kitsu868::message_read::kMaximumMessageIds]{};
  for (uint8_t index = 0U; index < plan.messageCount; ++index) {
    selected[index] = journalIndexes[plan.recordIndexes[index]];
  }
  const uint8_t marked =
      applyChatJournalReadPlan(selected, plan.messageCount);
  buildMarkReadResponse(true, nullptr, marked, plan.unchangedCount, output);
  return true;
}

struct NeighborActionRequest {
  char actionId[37]{};
  uint16_t targetUid = 0U;
  uint32_t targetSessionNonce = 0U;
  uint16_t sequence = 0U;
  uint32_t expiresAtEpoch = 0U;
  kitsu868::nearby::PositiveAction action =
      kitsu868::nearby::PositiveAction::Pet;
};

bool asciiHex(uint8_t value) {
  return (value >= '0' && value <= '9') ||
      (value >= 'a' && value <= 'f') ||
      (value >= 'A' && value <= 'F');
}

bool validNeighborActionId(const uint8_t* value, size_t bytes) {
  if (!value || bytes != 36U) return false;
  for (size_t index = 0U; index < bytes; ++index) {
    const bool separator = index == 8U || index == 13U || index == 18U ||
        index == 23U;
    if ((separator && value[index] != '-') ||
        (!separator && !asciiHex(value[index]))) {
      return false;
    }
  }
  return true;
}

bool parseNeighborDeviceId(const uint8_t* value, size_t bytes,
                           uint16_t& uid) {
  if (!value || bytes != 6U || value[0] != 'K' || value[1] != 'T') {
    return false;
  }
  uint16_t parsed = 0U;
  for (size_t index = 2U; index < bytes; ++index) {
    if (!asciiHex(value[index])) return false;
    const uint8_t digit = value[index] <= '9'
                              ? static_cast<uint8_t>(value[index] - '0')
                              : static_cast<uint8_t>(
                                    (value[index] & ~0x20U) - 'A' + 10U);
    parsed = static_cast<uint16_t>((parsed << 4U) | digit);
  }
  uid = parsed;
  return uid != 0U;
}

bool parseNeighborUint32(const uint8_t* payload, size_t payloadBytes,
                         size_t& cursor, uint32_t& output) {
  skipWhitespace(payload, payloadBytes, cursor);
  const size_t start = cursor;
  while (cursor < payloadBytes && payload[cursor] >= '0' &&
         payload[cursor] <= '9') {
    ++cursor;
  }
  return parseUint32Token(payload + start, cursor - start, output);
}

bool parseNeighborActionRequest(const uint8_t* payload, size_t payloadBytes,
                                NeighborActionRequest& output) {
  output = NeighborActionRequest{};
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) {
    return false;
  }
  size_t cursor = 0U;
  if (!consume(payload, payloadBytes, cursor, '{')) return false;
  enum : uint8_t {
    kActionId = 1U << 0U,
    kTarget = 1U << 1U,
    kSession = 1U << 2U,
    kSequence = 1U << 3U,
    kKind = 1U << 4U,
    kExpiry = 1U << 5U,
    kAll = kActionId | kTarget | kSession | kSequence | kKind | kExpiry,
  };
  uint8_t seen = 0U;
  uint8_t fields = 0U;
  for (;;) {
    skipWhitespace(payload, payloadBytes, cursor);
    if (cursor < payloadBytes && payload[cursor] == '}') {
      ++cursor;
      break;
    }
    if (fields != 0U && !consume(payload, payloadBytes, cursor, ',')) {
      return false;
    }
    if (++fields > 6U) return false;
    const uint8_t* key = nullptr;
    size_t keyBytes = 0U;
    if (!parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) ||
        !consume(payload, payloadBytes, cursor, ':')) {
      return false;
    }
    if (sameToken(key, keyBytes, "action_id")) {
      if ((seen & kActionId) != 0U) return false;
      const uint8_t* value = nullptr;
      size_t valueBytes = 0U;
      if (!parseAsciiString(payload, payloadBytes, cursor, value, valueBytes) ||
          !validNeighborActionId(value, valueBytes)) {
        return false;
      }
      memcpy(output.actionId, value, valueBytes);
      output.actionId[valueBytes] = '\0';
      seen |= kActionId;
    } else if (sameToken(key, keyBytes, "target_device_id")) {
      if ((seen & kTarget) != 0U) return false;
      const uint8_t* value = nullptr;
      size_t valueBytes = 0U;
      if (!parseAsciiString(payload, payloadBytes, cursor, value, valueBytes) ||
          !parseNeighborDeviceId(value, valueBytes, output.targetUid)) {
        return false;
      }
      seen |= kTarget;
    } else if (sameToken(key, keyBytes, "target_session_nonce")) {
      if ((seen & kSession) != 0U ||
          !parseNeighborUint32(payload, payloadBytes, cursor,
                               output.targetSessionNonce)) {
        return false;
      }
      seen |= kSession;
    } else if (sameToken(key, keyBytes, "sequence")) {
      if ((seen & kSequence) != 0U) return false;
      uint32_t sequence = 0U;
      if (!parseNeighborUint32(payload, payloadBytes, cursor, sequence) ||
          sequence == 0U || sequence > UINT16_MAX) {
        return false;
      }
      output.sequence = static_cast<uint16_t>(sequence);
      seen |= kSequence;
    } else if (sameToken(key, keyBytes, "kind")) {
      if ((seen & kKind) != 0U) return false;
      const uint8_t* value = nullptr;
      size_t valueBytes = 0U;
      if (!parseAsciiString(payload, payloadBytes, cursor, value, valueBytes)) {
        return false;
      }
      if (sameToken(value, valueBytes, "pet")) {
        output.action = kitsu868::nearby::PositiveAction::Pet;
      } else if (sameToken(value, valueBytes, "greet")) {
        output.action = kitsu868::nearby::PositiveAction::Greet;
      } else if (sameToken(value, valueBytes, "play")) {
        output.action = kitsu868::nearby::PositiveAction::Play;
      } else if (sameToken(value, valueBytes, "gift")) {
        output.action = kitsu868::nearby::PositiveAction::Gift;
      } else {
        return false;
      }
      seen |= kKind;
    } else if (sameToken(key, keyBytes, "expires_at_epoch")) {
      if ((seen & kExpiry) != 0U ||
          !parseNeighborUint32(payload, payloadBytes, cursor,
                               output.expiresAtEpoch)) {
        return false;
      }
      seen |= kExpiry;
    } else {
      return false;
    }
  }
  skipWhitespace(payload, payloadBytes, cursor);
  return cursor == payloadBytes && seen == kAll &&
      output.targetSessionNonce != 0U && output.expiresAtEpoch != 0U;
}

void buildNeighborActionReceipt(const NeighborActionRequest& request,
                                bool accepted, const char* error,
                                String& output) {
  output = "{\"schema\":\"kitsu.neighbor-action-receipt.v1\",";
  output += "\"action_id\":\"";
  output += request.actionId;
  output += "\",\"accepted\":";
  output += accepted ? "true" : "false";
  output += ",\"state\":\"";
  output += accepted ? "queued" : "rejected";
  output += '"';
  if (!accepted) {
    output += ",\"error_code\":\"";
    output += error ? error : "request_rejected";
    output += '"';
  }
  output += '}';
}

bool applyNeighborAction(const uint8_t* payload, size_t payloadBytes,
                         String& output) {
  NeighborActionRequest request{};
  if (!parseNeighborActionRequest(payload, payloadBytes, request)) {
    return false;
  }
  if (!meshTransport.timeValid()) {
    buildNeighborActionReceipt(request, false, "time_unset", output);
    return true;
  }
  const uint32_t nowEpoch = meshTransport.currentEpoch();
  if (request.expiresAtEpoch < nowEpoch ||
      request.expiresAtEpoch - nowEpoch > 300U) {
    buildNeighborActionReceipt(request, false, "action_expired", output);
    return true;
  }
  const kitsu868::mesh::TransportStatus status = queueNearbyAction(
      request.targetUid, request.targetSessionNonce, request.sequence,
      request.action);
  if (status == kitsu868::mesh::TransportStatus::Ok) {
    buildNeighborActionReceipt(request, true, nullptr, output);
    return true;
  }
  const char* error = "radio_unavailable";
  if (status == kitsu868::mesh::TransportStatus::ContactNotFound) {
    error = "neighbor_not_found";
  } else if (status == kitsu868::mesh::TransportStatus::AdvertiseCooldown) {
    error = "rate_limited";
  } else if (status == kitsu868::mesh::TransportStatus::SendBusy) {
    error = "radio_busy";
  } else if (status == kitsu868::mesh::TransportStatus::TxLocked) {
    error = "tx_policy_locked";
  }
  buildNeighborActionReceipt(request, false, error, output);
  return true;
}

bool buildEncounterNeighbors(const uint8_t* payload, size_t payloadBytes,
                             String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  const uint32_t now = millis();
  output = "{\"schema\":\"kitsu.encounter-neighbors.v1\",";
  output +=
      "\"supported_actions\":[\"pet\",\"greet\",\"play\",\"gift\"],";
  output += "\"items\":[";
  output.reserve(2048U);
  bool first = true;
  for (const NearbyNeighbor& neighbor : nearbyNeighbors) {
    if (!neighbor.used ||
        static_cast<uint32_t>(now - neighbor.lastSeenAt) >
            NEARBY_NEIGHBOR_TTL_MS) {
      continue;
    }
    if (!first) output += ',';
    first = false;
    char deviceId[7]{};
    snprintf(deviceId, sizeof(deviceId), "KT%04X", neighbor.uid);
    output += "{\"device_id\":\"";
    output += deviceId;
    output += "\",\"session_nonce\":";
    output += String(static_cast<unsigned long>(neighbor.sessionNonce));
    output += ",\"pack_id\":";
    output += String(static_cast<unsigned long>(neighbor.packId));
    output += ",\"appearance\":";
    output += String(neighbor.appearance);
    output += ",\"evolution_stage\":";
    output += String(neighbor.evolutionStage);
    output += ",\"bond\":";
    output += String(neighbor.bond);
    output += ",\"mood\":";
    output += String(neighbor.mood);
    output += ",\"emote\":";
    output += String(neighbor.emote);
    output += ",\"rssi\":";
    output += String(neighbor.rssi, 1);
    output += ",\"snr\":";
    output += String(neighbor.snr, 1);
    output += ",\"last_seen_age_ms\":";
    output += String(static_cast<unsigned long>(now - neighbor.lastSeenAt));
    output += ",\"next_sequence\":";
    output += String(neighbor.nextSequence);
    output += '}';
  }
  output += "]}";
  return output.length() <=
      kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

bool buildEncounterCatalog(const uint8_t* payload, size_t payloadBytes,
                           String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  output = "{\"schema\":\"kitsu.encounter-catalog.v1\",\"items\":[";
  output.reserve(2400U);
  for (size_t index = 0U; index < kitsu868::wild::creatureCount(); ++index) {
    kitsu868::wild::Creature creature{};
    if (!kitsu868::wild::creatureAt(index, creature) || !creature.name ||
        creature.packId == 0U) {
      return false;
    }
    if (index != 0U) output += ',';
    output += "{\"pack_id\":";
    output += String(static_cast<unsigned long>(creature.packId));
    output += ",\"creature_name\":\"";
    output += jsonEscaped(String(creature.name));
    output += "\",\"rarity\":\"";
    output += signalRarityName(creature.rarity);
    output += "\"}";
  }
  output += "]}";
  return kitsu868::wild::creatureCount() ==
             kitsu868::fun::kCatalogCreatureCount &&
      output.length() <= kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

bool buildEncounterDiscovery(const uint8_t* payload, size_t payloadBytes,
                             String& output) {
  if (!emptyObject(payload, payloadBytes) || !funStateReady ||
      kitsu868::wild::creatureCount() !=
          kitsu868::fun::kCatalogCreatureCount ||
      !kitsu868::fun::validateDiscoveryState(funDiscovery)) {
    return false;
  }

  // Validate the complete state and catalog before emitting a partial JSON
  // document. validateDiscoveryState intentionally accepts opaque source
  // bytes for persistence compatibility; this public endpoint fails closed.
  for (uint8_t index = 0U;
       index < kitsu868::fun::kCatalogCreatureCount; ++index) {
    kitsu868::wild::Creature creature{};
    const bool seen = (funDiscovery.seenMask &
        (UINT32_C(1) << index)) != 0U;
    if (!kitsu868::wild::creatureAt(index, creature) ||
        creature.packId == 0U || !creature.name ||
        (seen && !kitsu868::signal::validOperationKind(
            static_cast<kitsu868::signal::MeshOperationKind>(
                funDiscovery.lastSources[index])))) {
      return false;
    }
  }

  output = "{\"schema\":\"kitsu.encounter-discovery.v1\",\"items\":[";
  output.reserve(2400U);
  for (uint8_t index = 0U;
       index < kitsu868::fun::kCatalogCreatureCount; ++index) {
    kitsu868::wild::Creature creature{};
    if (!kitsu868::wild::creatureAt(index, creature)) return false;
    const bool seen = (funDiscovery.seenMask &
        (UINT32_C(1) << index)) != 0U;
    if (index != 0U) output += ',';
    output += "{\"pack_id\":";
    output += String(static_cast<unsigned long>(creature.packId));
    output += ",\"encounter_count\":";
    output += String(funDiscovery.encounterCounts[index]);
    output += ",\"last_source\":";
    if (!seen) {
      output += "null";
    } else {
      output += '"';
      output += signalOperationName(
          static_cast<kitsu868::signal::MeshOperationKind>(
              funDiscovery.lastSources[index]));
      output += '"';
    }
    output += '}';
  }
  output += "]}";
  return output.length() <=
      kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

const char* expeditionDurationWire(kitsu868::expedition::Duration value) {
  using kitsu868::expedition::Duration;
  switch (value) {
    case Duration::Short: return "short";
    case Duration::Medium: return "medium";
    case Duration::Long: return "long";
    case Duration::Count: break;
  }
  return "short";
}

const char* expeditionAxisWire(
    kitsu868::expedition::PersonalityAxis value) {
  using kitsu868::expedition::PersonalityAxis;
  switch (value) {
    case PersonalityAxis::Warmth: return "warmth";
    case PersonalityAxis::Playfulness: return "playfulness";
    case PersonalityAxis::Boldness: return "boldness";
    case PersonalityAxis::Curiosity: return "curiosity";
    case PersonalityAxis::None:
    case PersonalityAxis::Count: break;
  }
  return "warmth";
}

const char* storyToneWire(kitsu868::dialogue::StoryTone value) {
  using kitsu868::dialogue::StoryTone;
  switch (value) {
    case StoryTone::Warm: return "warm";
    case StoryTone::Curious: return "curious";
    case StoryTone::Brave: return "brave";
    case StoryTone::Playful: return "playful";
    case StoryTone::Calm: return "calm";
  }
  return "calm";
}

const char* partyPhaseWire(kitsu868::party::SessionPhase phase) {
  using kitsu868::party::SessionPhase;
  switch (phase) {
    case SessionPhase::Idle: return "idle";
    case SessionPhase::Joining: return "joining";
    case SessionPhase::Lobby: return "lobby";
    case SessionPhase::Round1:
    case SessionPhase::Round2:
    case SessionPhase::Round3: return "round";
    case SessionPhase::Complete: return "complete";
    case SessionPhase::Cancelled: return "cancelled";
    case SessionPhase::Expired: return "expired";
    case SessionPhase::Unavailable: return "unavailable";
  }
  return "unavailable";
}

const char* partyChoiceWire(uint8_t choice) {
  switch (static_cast<kitsu868::party::SignalChoice>(choice)) {
    case kitsu868::party::SignalChoice::Sweep: return "sweep";
    case kitsu868::party::SignalChoice::Listen: return "listen";
    case kitsu868::party::SignalChoice::Pulse: return "pulse";
    case kitsu868::party::SignalChoice::None: break;
  }
  return "none";
}

const char* partyTierWire(uint8_t tier, bool available) {
  if (!available) return "none";
  switch (static_cast<kitsu868::party::ResultTier>(tier)) {
    case kitsu868::party::ResultTier::Faded: return "faded";
    case kitsu868::party::ResultTier::Trace: return "trace";
    case kitsu868::party::ResultTier::Found: return "found";
    case kitsu868::party::ResultTier::Resonant: return "resonant";
  }
  return "none";
}

void appendPartyDeviceId(String& output, uint16_t uid) {
  char id[7]{};
  snprintf(id, sizeof(id), "KT%04X", uid);
  output += id;
}

bool buildFunStateBody(String& output) {
  output.reserve(1900U);
  output = "{\"schema\":\"kitsu.fun-state.v1\",\"expedition\":{";
  const kitsu868::expedition::ExpeditionView expedition =
      expeditionCore.view();
  const char* expeditionStatus = "idle";
  if (expedition.phase == kitsu868::expedition::Phase::Traveling) {
    expeditionStatus = "scouting";
  } else if (expedition.phase == kitsu868::expedition::Phase::Ready) {
    expeditionStatus = "returned";
  }
  output += "\"status\":\"";
  output += expeditionStatus;
  output += "\",\"duration\":";
  if (expedition.phase == kitsu868::expedition::Phase::Idle) {
    output += "null,\"expedition_id\":null";
  } else {
    output += '"';
    output += expeditionDurationWire(expedition.duration);
    output += "\",\"expedition_id\":";
    output += String(static_cast<unsigned long>(expedition.expeditionId));
  }
  output += ",\"total_seconds\":";
  output += String(static_cast<unsigned long>(expedition.totalSeconds));
  output += ",\"remaining_seconds\":";
  output += String(static_cast<unsigned long>(expedition.remainingSeconds));
  output += ",\"progress_percent\":";
  output += String(expedition.progressPercent);
  output += ",\"report\":";
  kitsu868::expedition::CompletionHooks hooks{};
  kitsu868::expedition::ExpeditionReport report{};
  if (expedition.phase != kitsu868::expedition::Phase::Ready ||
      !expeditionCore.completion(hooks) ||
      !kitsu868::expedition::reportForIndex(expedition.reportIndex, report)) {
    output += "null";
  } else {
    output += "{\"expedition_id\":";
    output += String(static_cast<unsigned long>(hooks.expeditionId));
    output += ",\"headline\":\"";
    output += jsonEscaped(String(report.headline));
    output += "\",\"detail\":\"";
    output += jsonEscaped(String(report.detail));
    output += "\",\"affection_delta\":";
    output += String(hooks.affectionDelta);
    output += ",\"personality_axis\":\"";
    output += expeditionAxisWire(hooks.personalityAxis);
    output += "\",\"personality_delta\":";
    output += String(hooks.personalityDelta);
    output += ",\"encounter_catalog_index\":";
    if (hooks.hasEncounter()) output += String(hooks.encounterCatalogIndex);
    else output += "null";
    output += '}';
  }

  output += "},\"story\":{";
  kitsu868::dialogue::StoryBeat beat{};
  const bool hasStory = kitsu868::dialogue::currentStoryBeat(
      dialogueStories, beat);
  output += "\"status\":\"";
  output += !hasStory ? "idle" : beat.awaitsChoice ? "choosing" : "reading";
  output += "\",\"beat\":";
  if (!hasStory) {
    output += "null";
  } else {
    output += "{\"story_id\":";
    output += String(static_cast<unsigned>(beat.storyId));
    output += ",\"scene\":";
    output += String(beat.scene);
    output += ",\"line1\":\"";
    output += jsonEscaped(String(beat.line1));
    output += "\",\"line2\":\"";
    output += jsonEscaped(String(beat.line2));
    output += "\",\"choices\":[";
    if (beat.awaitsChoice) {
      for (uint8_t index = 0U;
           index < kitsu868::dialogue::kStoryChoiceCount; ++index) {
        if (index != 0U) output += ',';
        output += '"';
        output += jsonEscaped(String(beat.choices[index]));
        output += '"';
      }
    }
    output += "]}";
  }
  output += ",\"resolution\":";
  if (!storyResolutionAvailable) {
    output += "null";
  } else {
    output += "{\"story_id\":";
    output += String(static_cast<unsigned>(lastStoryResolution.storyId));
    output += ",\"line1\":\"";
    output += jsonEscaped(String(lastStoryResolution.line1));
    output += "\",\"line2\":\"";
    output += jsonEscaped(String(lastStoryResolution.line2));
    output += "\",\"tone\":\"";
    output += storyToneWire(lastStoryResolution.tone);
    output += "\",\"affection_delta\":";
    output += String(lastStoryResolution.affectionDelta);
    output += ",\"energy_delta\":";
    output += String(lastStoryResolution.energyDelta);
    output += ",\"curiosity_delta\":";
    output += String(lastStoryResolution.curiosityDelta);
    output += ",\"personality_match\":";
    output += lastStoryResolution.personalityMatch ? "true" : "false";
    output += '}';
  }

  const kitsu868::party::HostState host = partyHost.state();
  const kitsu868::party::ParticipantState guest = partyParticipant.state();
  const bool hosting =
      static_cast<kitsu868::party::SessionPhase>(host.phase) !=
      kitsu868::party::SessionPhase::Idle;
  const bool guesting = !hosting &&
      static_cast<kitsu868::party::SessionPhase>(guest.phase) !=
      kitsu868::party::SessionPhase::Idle;
  const kitsu868::party::SessionPhase phase = hosting
      ? static_cast<kitsu868::party::SessionPhase>(host.phase)
      : guesting
            ? static_cast<kitsu868::party::SessionPhase>(guest.phase)
            : kitsu868::party::SessionPhase::Idle;
  const uint16_t hostUid = hosting ? host.hostUid : guesting ? guest.hostUid : 0U;
  const uint32_t sessionNonce = hosting ? host.sessionNonce
                                       : guesting ? guest.sessionNonce : 0U;
  const uint8_t participantCount = hosting
      ? host.participantCount
      : guesting
            ? (!partyWelcomeAccepted
                   ? min<uint8_t>(kitsu868::party::kMaximumParticipants,
                                  static_cast<uint8_t>(
                                      guest.participantCount + 1U))
                   : guest.participantCount)
            : 0U;
  const uint8_t currentRound = hosting ? host.currentRound
                                       : guesting ? guest.currentRound : 0U;
  const kitsu868::party::HuntResult& result = hosting ? host.result
                                                      : guest.result;
  const bool resultAvailable = phase == kitsu868::party::SessionPhase::Complete;

  output += "},\"party\":{\"role\":\"";
  output += hosting ? "host" : guesting ? "guest" : "none";
  output += "\",\"phase\":\"";
  output += partyPhaseWire(phase);
  output += "\",\"host_device_id\":";
  if (hostUid == 0U) output += "null";
  else {
    output += '"';
    appendPartyDeviceId(output, hostUid);
    output += '"';
  }
  output += ",\"session_nonce\":";
  if (sessionNonce == 0U) output += "null";
  else output += String(static_cast<unsigned long>(sessionNonce));
  output += ",\"discovered_hosts\":[";
  const uint32_t now = millis();
  if (!hosting && !partyJoinRequested &&
      phase == kitsu868::party::SessionPhase::Joining &&
      guest.sessionNonce != 0U && lastPartyBeaconAt != 0U &&
      now - lastPartyBeaconAt <= PARTY_DISCOVERY_TTL_MS &&
      static_cast<int32_t>(guest.deadlineMs - now) > 0) {
    output += "{\"host_device_id\":\"";
    appendPartyDeviceId(output, guest.hostUid);
    output += "\",\"session_nonce\":";
    output += String(static_cast<unsigned long>(guest.sessionNonce));
    output += ",\"participant_count\":";
    output += String(max<uint8_t>(1U, guest.participantCount));
    output += ",\"join_window_seconds\":";
    const uint32_t remainingMs = guest.deadlineMs - now;
    output += String(max<uint32_t>(
        1U, min<uint32_t>(PARTY_LOBBY_WINDOW_SECONDS,
                          (remainingMs + 999U) / 1000U)));
    output += ",\"rssi\":";
    output += String(partyHostRssi, 1);
    output += ",\"last_seen_age_ms\":";
    output += String(static_cast<unsigned long>(now - lastPartyBeaconAt));
    output += '}';
  }
  output += "],\"participant_count\":";
  output += String(participantCount);
  output += ",\"participants\":[";
  bool firstParticipant = true;
  if (hosting) {
    for (uint8_t index = 0U;
         index < kitsu868::party::kMaximumParticipants; ++index) {
      const kitsu868::party::HostParticipantState& participant =
          host.participants[index];
      if (participant.active == 0U) continue;
      if (!firstParticipant) output += ',';
      firstParticipant = false;
      output += "{\"device_id\":\"";
      appendPartyDeviceId(output, participant.hunt.uid);
      output += "\",\"submitted_round\":";
      uint8_t submitted = 0U;
      for (uint8_t round = 0U; round < kitsu868::party::kHuntRounds;
           ++round) {
        if ((participant.hunt.submittedMask & (1U << round)) != 0U) {
          submitted = static_cast<uint8_t>(round + 1U);
        }
      }
      output += String(submitted);
      output += ",\"local\":";
      output += participant.hunt.uid == ownUidSuffix() ? "true" : "false";
      output += '}';
    }
  } else if (guesting) {
    const uint16_t ids[2] = {guest.localUid, guest.hostUid};
    for (uint8_t index = 0U; index < 2U; ++index) {
      if (ids[index] == 0U || (index == 1U && ids[1] == ids[0])) continue;
      if (!firstParticipant) output += ',';
      firstParticipant = false;
      output += "{\"device_id\":\"";
      appendPartyDeviceId(output, ids[index]);
      output += "\",\"submitted_round\":";
      uint8_t submitted = 0U;
      if (index == 0U) {
        for (uint8_t round = 0U; round < kitsu868::party::kHuntRounds;
             ++round) {
          if ((guest.submittedMask & (1U << round)) != 0U) {
            submitted = static_cast<uint8_t>(round + 1U);
          }
        }
      }
      output += String(submitted);
      output += ",\"local\":";
      output += index == 0U ? "true" : "false";
      output += '}';
    }
  }
  output += "],\"round\":";
  output += String(currentRound);
  output += ",\"local_choice\":\"";
  uint8_t localChoice = 0U;
  if (currentRound >= 1U && currentRound <= kitsu868::party::kHuntRounds) {
    if (hosting) {
      for (uint8_t index = 0U;
           index < kitsu868::party::kMaximumParticipants; ++index) {
        if (host.participants[index].active != 0U &&
            host.participants[index].hunt.uid == ownUidSuffix()) {
          localChoice = host.participants[index].hunt.choices[currentRound - 1U];
          break;
        }
      }
    } else if (guesting) {
      localChoice = guest.choices[currentRound - 1U];
    }
  }
  output += partyChoiceWire(localChoice);
  output += "\",\"reward\":{\"tier\":\"";
  output += partyTierWire(result.tier, resultAvailable);
  output += "\",\"score\":";
  output += String(resultAvailable ? result.score : 0U);
  output += ",\"maximum_score\":";
  output += String(resultAvailable ? result.maximumScore : 0U);
  output += ",\"bond_awarded\":";
  const bool rewardForSession = resultAvailable && partyRewardAvailable &&
      lastPartyRewardSessionNonce == sessionNonce;
  output += String(rewardForSession ? lastPartyReward.bondAwarded : 0U);
  output += ",\"party_bond\":";
  output += String(static_cast<unsigned long>(partyRewards.state().partyBond));
  output += ",\"eligible_unique_peers\":";
  output += String(rewardForSession
                       ? lastPartyReward.eligibleUniquePeers : 0U);
  output += ",\"current_streak_days\":";
  output += String(partyRewards.state().currentStreakDays);
  output += ",\"longest_streak_days\":";
  output += String(partyRewards.state().longestStreakDays);
  output += "}}}";
  return output.length() <= 2048U &&
      output.length() <= kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

bool buildFunState(const uint8_t* payload, size_t payloadBytes,
                   String& output) {
  return emptyObject(payload, payloadBytes) && buildFunStateBody(output);
}

void buildFunError(const char* error, String& output) {
  output = "{\"ok\":false,\"error\":\"";
  output += error ? error : "request_rejected";
  output += "\"}";
}

bool parseSingleStringObject(const uint8_t* payload, size_t payloadBytes,
                             const char* expectedKey,
                             const uint8_t*& value, size_t& valueBytes) {
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) {
    return false;
  }
  size_t cursor = 0U;
  const uint8_t* key = nullptr;
  size_t keyBytes = 0U;
  return consume(payload, payloadBytes, cursor, '{') &&
      parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) &&
      sameToken(key, keyBytes, expectedKey) &&
      consume(payload, payloadBytes, cursor, ':') &&
      parseAsciiString(payload, payloadBytes, cursor, value, valueBytes) &&
      consume(payload, payloadBytes, cursor, '}') &&
      (skipWhitespace(payload, payloadBytes, cursor), cursor == payloadBytes);
}

bool parseSingleUintObject(const uint8_t* payload, size_t payloadBytes,
                           const char* expectedKey, uint32_t& value) {
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) {
    return false;
  }
  size_t cursor = 0U;
  const uint8_t* key = nullptr;
  size_t keyBytes = 0U;
  if (!consume(payload, payloadBytes, cursor, '{') ||
      !parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) ||
      !sameToken(key, keyBytes, expectedKey) ||
      !consume(payload, payloadBytes, cursor, ':') ||
      !parseNeighborUint32(payload, payloadBytes, cursor, value) ||
      !consume(payload, payloadBytes, cursor, '}')) {
    return false;
  }
  skipWhitespace(payload, payloadBytes, cursor);
  return cursor == payloadBytes;
}

bool parseBoolToken(const uint8_t* payload, size_t payloadBytes,
                    size_t& cursor, bool& value) {
  skipWhitespace(payload, payloadBytes, cursor);
  if (cursor + 4U <= payloadBytes &&
      memcmp(payload + cursor, "true", 4U) == 0) {
    value = true;
    cursor += 4U;
  } else if (cursor + 5U <= payloadBytes &&
             memcmp(payload + cursor, "false", 5U) == 0) {
    value = false;
    cursor += 5U;
  } else {
    return false;
  }
  return true;
}

bool parseSingleBoolObject(const uint8_t* payload, size_t payloadBytes,
                           const char* expectedKey, bool& value) {
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) {
    return false;
  }
  size_t cursor = 0U;
  const uint8_t* key = nullptr;
  size_t keyBytes = 0U;
  if (!consume(payload, payloadBytes, cursor, '{') ||
      !parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) ||
      !sameToken(key, keyBytes, expectedKey) ||
      !consume(payload, payloadBytes, cursor, ':') ||
      !parseBoolToken(payload, payloadBytes, cursor, value)) {
    return false;
  }
  if (!consume(payload, payloadBytes, cursor, '}')) return false;
  skipWhitespace(payload, payloadBytes, cursor);
  return cursor == payloadBytes;
}

const char* profileActionWire(kitsu868::dialogue::Action action) {
  using kitsu868::dialogue::Action;
  switch (action) {
    case Action::Pet: return "pet";
    case Action::Feed: return "feed";
    case Action::Play: return "play";
    case Action::Listen: return "listen";
    case Action::Sleep: return "sleep";
    case Action::Wake: return "wake";
    case Action::Meet: return "meet";
    case Action::Gift: return "gift";
  }
  return "unknown";
}

const char* profileTimeWire(kitsu868::progression::TimeBucket bucket) {
  using kitsu868::progression::TimeBucket;
  switch (bucket) {
    case TimeBucket::Morning: return "morning";
    case TimeBucket::Day: return "day";
    case TimeBucket::Evening: return "evening";
    case TimeBucket::Night: return "night";
  }
  return "unknown";
}

const char* profileRequestStateWire(
    kitsu868::progression::RequestState state) {
  using kitsu868::progression::RequestState;
  switch (state) {
    case RequestState::None: return "none";
    case RequestState::Pending: return "pending";
    case RequestState::Accepted: return "accepted";
    case RequestState::Declined: return "declined";
    case RequestState::Completed: return "completed";
  }
  return "none";
}

const char* profileQuestionWire(kitsu868::progression::QuestionKind question) {
  using kitsu868::progression::QuestionKind;
  switch (question) {
    case QuestionKind::QuietOrPlay: return "quiet_or_play";
    case QuestionKind::DawnOrNight: return "dawn_or_night";
    case QuestionKind::HomeOrExplore: return "home_or_explore";
  }
  return "unknown";
}

const char* profileComfortWire(kitsu868::progression::ComfortKind comfort) {
  using kitsu868::progression::ComfortKind;
  switch (comfort) {
    case ComfortKind::None: return "none";
    case ComfortKind::Tired: return "tired";
    case ComfortKind::Lonely: return "lonely";
    case ComfortKind::Restless: return "restless";
  }
  return "none";
}

const char* profileGoalWire(kitsu868::progression::GoalKind goal) {
  using kitsu868::progression::GoalKind;
  switch (goal) {
    case GoalKind::AnyCare: return "care";
    case GoalKind::Variety: return "variety";
    case GoalKind::Favorite: return "favorite";
  }
  return "care";
}

const char* profileQuickActionWire(kitsu868::activities::QuickAction action) {
  using kitsu868::activities::QuickAction;
  switch (action) {
    case QuickAction::Pet: return "pet";
    case QuickAction::Feed: return "feed";
    case QuickAction::Play: return "play";
    case QuickAction::Listen: return "listen";
    case QuickAction::DailyGame: return "daily";
    case QuickAction::Expedition: return "expedition";
  }
  return "pet";
}

bool buildCompanionProfileBody(String& output) {
  if (!companionProgressionReady || !activityStateReady) {
    output = "{\"ok\":false,\"error\":\"profile_unavailable\"}";
    return true;
  }

  const kitsu868::PersonalityTraits& traits = companionBrain.personality();
  const kitsu868::progression::DailyGoal goal =
      companionProgression.dailyGoal();
  const kitsu868::progression::PersonalBests bests =
      companionProgression.personalBests();
  const kitsu868::progression::RequestState requestState =
      companionProgression.requestState();
  const kitsu868::progression::ComfortKind comfort =
      companionProgression.comfortNeed();
  const kitsu868::progression::DisplayLine comfortLine =
      kitsu868::progression::CompanionProgression::comfortLine(comfort);
  const kitsu868::activities::ActivityState activity = activitySuite.snapshot();

  uint32_t day = 0U;
  uint16_t minute = 0U;
  const bool hasLocalTime = currentLocalDayMinute(day, minute, true);
  const kitsu868::progression::TimeBucket currentBucket = hasLocalTime
      ? kitsu868::progression::CompanionProgression::timeBucket(minute)
      : kitsu868::progression::TimeBucket::Day;
  kitsu868::dialogue::Action routineAction{};
  const bool hasRoutine = hasLocalTime &&
      companionProgression.recognizedRoutine(currentBucket, routineAction);
  kitsu868::progression::TimeBucket favoriteTime{};
  const bool hasFavorite = companionProgression.hasFavorite();
  const bool hasFavoriteTime = hasFavorite &&
      companionProgression.preferredTime(companionProgression.favoriteAction(),
                                          favoriteTime);
  kitsu868::progression::QuestionKind pendingQuestion{};
  const bool hasPendingQuestion =
      companionProgression.pendingQuestion(pendingQuestion);

  output.reserve(2300U);
  output = "{\"ok\":true,\"schema\":1,\"nickname\":\"";
  output += jsonEscaped(String(companionProgression.nickname()));
  output += "\",\"personality\":{\"kind\":\"";
  output += kitsu868::CompanionBrain::personalityLabel(traits.kind);
  output += "\",\"warmth\":" + String(traits.warmth);
  output += ",\"playfulness\":" + String(traits.playfulness);
  output += ",\"boldness\":" + String(traits.boldness);
  output += ",\"curiosity\":" + String(traits.curiosity) + "}";
  output += ",\"mood\":\"";
  output += kitsu868::CompanionBrain::moodLabel(
      companionBrain.mood(companionVitals()));
  output += "\",\"bond\":{\"level\":" + String(companionBrain.bondLevel());
  output += ",\"xp\":" + String(companionBrain.bondXp());
  output += ",\"speech_stage\":" +
      String(companionProgression.speechStage());
  output += ",\"dialogue_bank\":" + String(
      companionProgression.bondDialogueBank(companionBrain.bondLevel()));
  output += "}";

  output += ",\"favorite\":";
  if (hasFavorite) {
    output += "{\"action\":\"";
    output += profileActionWire(companionProgression.favoriteAction());
    output += "\",\"time\":";
    if (hasFavoriteTime) {
      output += "\"";
      output += profileTimeWire(favoriteTime);
      output += "\"";
    } else {
      output += "null";
    }
    output += "}";
  } else {
    output += "null";
  }

  output += ",\"routine\":";
  if (hasRoutine) {
    output += "{\"action\":\"";
    output += profileActionWire(routineAction);
    output += "\",\"time\":\"";
    output += profileTimeWire(currentBucket);
    output += "\"}";
  } else {
    output += "null";
  }

  output += ",\"ritual\":";
  if (companionProgression.hasRitual()) {
    output += "{\"action\":\"";
    output += profileActionWire(companionProgression.ritualAction());
    output += "\",\"time\":\"";
    output += profileTimeWire(companionProgression.ritualTime());
    output += "\",\"days\":" + String(companionProgression.ritualStreak());
    output += "}";
  } else {
    output += "null";
  }

  output += ",\"preferences\":{";
  for (uint8_t index = 0U; index < 3U; ++index) {
    if (index != 0U) output += ',';
    const auto question =
        static_cast<kitsu868::progression::QuestionKind>(index);
    uint8_t choice = 0U;
    output += "\"";
    output += profileQuestionWire(question);
    output += "\":";
    if (companionProgression.preferredQuestionChoice(question, choice)) {
      output += String(choice);
    } else {
      output += "null";
    }
  }
  output += "}";

  output += ",\"check_in\":{\"request\":{\"state\":\"";
  output += profileRequestStateWire(requestState);
  output += "\",\"action\":\"";
  output += profileActionWire(companionProgression.requestedAction());
  output += "\"},\"question\":";
  if (hasPendingQuestion) {
    output += "{\"kind\":\"";
    output += profileQuestionWire(pendingQuestion);
    output += "\",\"option0\":\"";
    output += jsonEscaped(String(
        kitsu868::progression::CompanionProgression::questionOption(
            pendingQuestion, 0U)));
    output += "\",\"option1\":\"";
    output += jsonEscaped(String(
        kitsu868::progression::CompanionProgression::questionOption(
            pendingQuestion, 1U)));
    output += "\"}";
  } else {
    output += "null";
  }
  output += ",\"comfort\":{\"kind\":\"";
  output += profileComfortWire(comfort);
  output += "\",\"line1\":\"" + jsonEscaped(String(comfortLine.line1));
  output += "\",\"line2\":\"" + jsonEscaped(String(comfortLine.line2));
  output += "\"},\"callback_ready\":";
  output += (hasLocalTime && companionProgression.callbackReady(day))
      ? "true" : "false";
  output += "}";

  output += ",\"goal\":{\"kind\":\"";
  output += profileGoalWire(goal.kind);
  output += "\",\"action\":\"";
  output += profileActionWire(goal.action);
  output += "\",\"progress\":" + String(goal.progress);
  output += ",\"target\":" + String(goal.target) + "}";
  output += ",\"development\":{\"momentum\":" +
      String(companionProgression.moodMomentum());
  output += ",\"total_actions\":" +
      String(companionProgression.totalActions());
  output += ",\"streak_days\":" +
      String(companionProgression.currentStreak());
  output += ",\"perfect_days\":" +
      String(companionProgression.perfectDays());
  output += ",\"bests\":{\"daily_actions\":" + String(bests.dailyActions);
  output += ",\"daily_variety\":" + String(bests.dailyVariety);
  output += ",\"care_rhythm\":" + String(bests.careRhythm) + "}}";

  output += ",\"settings\":{\"quick_action\":\"";
  output += profileQuickActionWire(activitySuite.quickAction());
  output += "\",\"quiet_hours\":{\"enabled\":";
  output += activity.quietHoursEnabled ? "true" : "false";
  output += ",\"start_minute\":" + String(activity.quietStartMinute);
  output += ",\"end_minute\":" + String(activity.quietEndMinute) + "}}";

  kitsu868::MemoryEntry memory{};
  output += ",\"latest_memory\":";
  if (companionBrain.recentMemory(0U, memory)) {
    const kitsu868::MemoryText text =
        kitsu868::CompanionBrain::memoryText(memory);
    output += "{\"sequence\":" + String(memory.sequence);
    output += ",\"event\":" + String(static_cast<unsigned>(memory.event));
    output += ",\"line1\":\"" + jsonEscaped(String(text.line1));
    output += "\",\"line2\":\"" + jsonEscaped(String(text.line2));
    output += "\"}";
  } else {
    output += "null";
  }
  output += "}";
  return output.length() <= kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

bool buildCompanionProfile(const uint8_t* payload, size_t payloadBytes,
                           String& output) {
  return emptyObject(payload, payloadBytes) &&
      buildCompanionProfileBody(output);
}

bool setCompanionNickname(const uint8_t* payload, size_t payloadBytes,
                          String& output) {
  const uint8_t* value = nullptr;
  size_t valueBytes = 0U;
  if (!parseSingleStringObject(payload, payloadBytes, "nickname", value,
                               valueBytes) ||
      valueBytes > kitsu868::progression::kNicknameCapacity) {
    return false;
  }
  String nickname;
  nickname.reserve(valueBytes);
  for (size_t i = 0U; i < valueBytes; ++i) {
    nickname += static_cast<char>(value[i]);
  }
  uint8_t before[kitsu868::progression::kSnapshotCapacity]{};
  if (!captureCompanionProgression(before) ||
      !companionProgression.setNickname(nickname.c_str())) {
    output = "{\"ok\":false,\"error\":\"invalid_nickname\"}";
    return true;
  }
  if (!persistCompanionProgression()) {
    restoreCompanionProgression(before);
    output = "{\"ok\":false,\"error\":\"storage_failed\"}";
    return true;
  }
  return buildCompanionProfileBody(output);
}

bool answerCompanionRequest(const uint8_t* payload, size_t payloadBytes,
                            String& output) {
  bool accept = false;
  if (!parseSingleBoolObject(payload, payloadBytes, "accept", accept)) {
    return false;
  }
  uint8_t before[kitsu868::progression::kSnapshotCapacity]{};
  if (!captureCompanionProgression(before)) {
    output = "{\"ok\":false,\"error\":\"profile_unavailable\"}";
    return true;
  }
  const kitsu868::progression::RequestResult result =
      companionProgression.answerRequest(accept);
  if (!result.valid) {
    output = "{\"ok\":false,\"error\":\"request_unavailable\"}";
    return true;
  }
  if (!persistCompanionProgression()) {
    restoreCompanionProgression(before);
    output = "{\"ok\":false,\"error\":\"storage_failed\"}";
    return true;
  }
  return buildCompanionProfileBody(output);
}

bool answerCompanionQuestion(const uint8_t* payload, size_t payloadBytes,
                             String& output) {
  uint32_t choice = 0U;
  if (!parseSingleUintObject(payload, payloadBytes, "choice", choice) ||
      choice > 1U) {
    return false;
  }
  uint8_t before[kitsu868::progression::kSnapshotCapacity]{};
  if (!captureCompanionProgression(before)) {
    output = "{\"ok\":false,\"error\":\"profile_unavailable\"}";
    return true;
  }
  const kitsu868::progression::QuestionResult result =
      companionProgression.answerQuestion(static_cast<uint8_t>(choice));
  if (!result.valid) {
    output = "{\"ok\":false,\"error\":\"question_unavailable\"}";
    return true;
  }
  if (!persistCompanionProgression()) {
    restoreCompanionProgression(before);
    output = "{\"ok\":false,\"error\":\"storage_failed\"}";
    return true;
  }
  return buildCompanionProfileBody(output);
}

bool parseHexDigest(const uint8_t* value, size_t bytes,
                    uint8_t output[kitsu868::presentation::kFrameDigestBytes]) {
  if (!value || bytes != kitsu868::presentation::kFrameDigestBytes * 2U) {
    return false;
  }
  for (size_t index = 0U;
       index < kitsu868::presentation::kFrameDigestBytes; ++index) {
    const uint8_t high = value[index * 2U];
    const uint8_t low = value[index * 2U + 1U];
    if (!asciiHex(high) || !asciiHex(low)) return false;
    const uint8_t highValue = high <= '9'
        ? static_cast<uint8_t>(high - '0')
        : static_cast<uint8_t>((high & ~0x20U) - 'A' + 10U);
    const uint8_t lowValue = low <= '9'
        ? static_cast<uint8_t>(low - '0')
        : static_cast<uint8_t>((low & ~0x20U) - 'A' + 10U);
    output[index] = static_cast<uint8_t>((highValue << 4U) | lowValue);
  }
  return true;
}

void buildPresentationError(kitsu868::presentation::Status status,
                            String& output) {
  output = "{\"ok\":false,\"error\":\"";
  output += kitsu868::presentation::statusName(status);
  output += "\"}";
}

bool buildPresentationStateBody(
    const kitsu868::presentation::PresentationState& state,
    String& output) {
  output.reserve(920U);
  output = "{\"ok\":true,\"schema\":1,\"session_id\":";
  output += String(static_cast<unsigned long>(state.sessionId));
  output += ",\"captured_at_ms\":" + String(state.capturedAtMs);
  output += ",\"surface\":\"";
  output += kitsu868::presentation::surfaceName(state.surface);
  output += "\",\"display_awake\":";
  output += state.displayAwake ? "true" : "false";
  output += ",\"frame_visible\":";
  output += state.frameVisible ? "true" : "false";
  output += ",\"pack\":{";
  output += "\"valid\":";
  output += state.packValid ? "true" : "false";
  output += ",\"name\":\"" + jsonEscaped(String(state.packName));
  output += "\",\"id\":" + String(state.packId);
  output += ",\"revision\":" + String(state.packRevision);
  output += ",\"total_bytes\":" + String(state.packTotalBytes);
  output += ",\"payload_crc32\":" + String(state.packPayloadCrc32);
  output += ",\"header_crc32\":" + String(state.packHeaderCrc32);
  output += ",\"format\":" + String(state.packFormatVersion);
  output += ",\"width\":" + String(state.frameWidth);
  output += ",\"height\":" + String(state.frameHeight);
  output += ",\"frame_count\":" + String(state.frameCount);
  output += ",\"appearance\":" + String(state.appearanceVariant) + "}";
  output += ",\"animation\":{";
  output += "\"active\":";
  output += state.animationActive ? "true" : "false";
  output += ",\"finite\":";
  output += state.animationFinite ? "true" : "false";
  output += ",\"requested_role\":\"";
  output += kitsu868::presentation::roleName(state.requestedRole);
  output += "\",\"resolved_role\":\"";
  output += kitsu868::presentation::roleName(state.resolvedRole);
  output += "\",\"playback\":\"";
  output += kitsu868::presentation::playbackName(state.playback);
  output += "\",\"token\":" + String(state.animationToken);
  output += ",\"elapsed_ms\":" + String(state.animationElapsedMs) + "}";
  output += ",\"frame\":{";
  output += "\"available\":";
  output += state.frameAvailable ? "true" : "false";
  output += ",\"encoding\":\"";
  output += state.frameAvailable ? "xbm_row_major_lsb_first" : "none";
  output += "\",\"bytes\":" + String(state.frameBytes);
  output += ",\"sha256\":\"";
  if (state.frameAvailable) {
    appendUpperHex(output, state.frameSha256,
                   kitsu868::presentation::kFrameDigestBytes);
  }
  output += "\"}}";
  return output.length() <= kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

kitsu868::presentation::Capture capturePetPresentation(uint32_t now) {
  kitsu868::presentation::Capture capture{};
  const bool packValid = companionPack.valid();
  const bool animationActive = packValid && activeAnimation.active;
  const bool currentFrame = animationActive && lastRenderedPetFrame &&
      lastRenderedPetAnimationToken == activeAnimation.token;
  const uint32_t capturedAt = currentFrame ? lastRenderedPetFrameAt : now;
  capture.capturedAtMs = capturedAt;
  capture.surface = static_cast<kitsu868::presentation::Surface>(screen);
  capture.displayAwake = oledDetected && !displaySleeping;
  capture.frameVisible = currentFrame && capture.displayAwake &&
      lastRenderedPetFrameSurface == screen &&
      static_cast<uint32_t>(now - lastRenderedPetFrameAt) <= 500UL;
  capture.packValid = packValid;
  capture.packName = companionPack.name();
  if (!packValid) return capture;

  capture.packId = companionPack.id();
  capture.packRevision = companionPack.revision();
  capture.packTotalBytes = companionPack.bytes();
  capture.packPayloadCrc32 = companionPack.payloadCrc32();
  capture.packHeaderCrc32 = companionPack.headerCrc32();
  capture.packFormatVersion = companionPack.formatVersion();
  capture.frameWidth = companionPack.frameWidth();
  capture.frameHeight = companionPack.frameHeight();
  capture.frameCount = companionPack.frameCount();
  capture.appearanceVariant = companionBrain.appearanceVariant();
  if (!animationActive) return capture;

  capture.animationActive = true;
  capture.animationFinite = activeAnimation.finite;
  capture.requestedRole =
      static_cast<kitsu868::presentation::Role>(activeAnimation.role);
  capture.resolvedRole =
      static_cast<kitsu868::presentation::Role>(activeAnimation.clipRole);
  capture.playback =
      static_cast<kitsu868::presentation::Playback>(activeAnimation.mode);
  capture.animationToken = activeAnimation.token;
  capture.animationElapsedMs = capturedAt - activeAnimation.startedAt;
  if (currentFrame) {
    capture.frameData = lastRenderedPetFrame;
    capture.frameBytes = lastRenderedPetFrameBytes;
  }
  return capture;
}

bool openPetPresentation(const uint8_t* payload, size_t payloadBytes,
                         String& output) {
  uint32_t sessionId = 0U;
  if (!parseSingleUintObject(payload, payloadBytes, "session_id", sessionId) ||
      sessionId == 0U) {
    return false;
  }
  const uint32_t now = millis();
  if (petPresentationPreview.active() &&
      static_cast<uint32_t>(now - petPresentationOpenedAt) > 30000UL) {
    (void)petPresentationPreview.close(petPresentationPreview.sessionId());
  }
  kitsu868::presentation::PresentationState state{};
  const kitsu868::presentation::Status status = petPresentationPreview.open(
      sessionId, capturePetPresentation(now), state);
  if (status != kitsu868::presentation::Status::Ok &&
      status != kitsu868::presentation::Status::Duplicate) {
    buildPresentationError(status, output);
    return true;
  }
  if (status == kitsu868::presentation::Status::Ok) {
    petPresentationOpenedAt = now;
  }
  return buildPresentationStateBody(state, output);
}

struct PresentationReadRequest {
  uint32_t sessionId = 0U;
  uint16_t offset = 0U;
  uint16_t bytes = 0U;
  uint8_t digest[kitsu868::presentation::kFrameDigestBytes]{};
};

bool parsePresentationRead(const uint8_t* payload, size_t payloadBytes,
                           PresentationReadRequest& request) {
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) {
    return false;
  }
  size_t cursor = 0U;
  uint8_t seen = 0U;
  if (!consume(payload, payloadBytes, cursor, '{')) return false;
  for (uint8_t field = 0U; field < 4U; ++field) {
    if (field != 0U && !consume(payload, payloadBytes, cursor, ',')) {
      return false;
    }
    const uint8_t* key = nullptr;
    size_t keyBytes = 0U;
    if (!parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) ||
        !consume(payload, payloadBytes, cursor, ':')) {
      return false;
    }
    if (sameToken(key, keyBytes, "session_id") && (seen & 1U) == 0U) {
      if (!parseNeighborUint32(payload, payloadBytes, cursor,
                               request.sessionId) ||
          request.sessionId == 0U) {
        return false;
      }
      seen |= 1U;
    } else if (sameToken(key, keyBytes, "offset") && (seen & 2U) == 0U) {
      uint32_t value = 0U;
      if (!parseNeighborUint32(payload, payloadBytes, cursor, value) ||
          value > kitsu868::presentation::kMaximumFrameBytes) {
        return false;
      }
      request.offset = static_cast<uint16_t>(value);
      seen |= 2U;
    } else if (sameToken(key, keyBytes, "bytes") && (seen & 4U) == 0U) {
      uint32_t value = 0U;
      if (!parseNeighborUint32(payload, payloadBytes, cursor, value) ||
          value == 0U || value > kitsu868::presentation::kMaximumChunkBytes) {
        return false;
      }
      request.bytes = static_cast<uint16_t>(value);
      seen |= 4U;
    } else if (sameToken(key, keyBytes, "frame_sha256") &&
               (seen & 8U) == 0U) {
      const uint8_t* value = nullptr;
      size_t valueBytes = 0U;
      if (!parseAsciiString(payload, payloadBytes, cursor, value, valueBytes) ||
          !parseHexDigest(value, valueBytes, request.digest)) {
        return false;
      }
      seen |= 8U;
    } else {
      return false;
    }
  }
  if (!consume(payload, payloadBytes, cursor, '}')) return false;
  skipWhitespace(payload, payloadBytes, cursor);
  return seen == 15U && cursor == payloadBytes;
}

bool readPetPresentation(const uint8_t* payload, size_t payloadBytes,
                         String& output) {
  PresentationReadRequest parsed{};
  if (!parsePresentationRead(payload, payloadBytes, parsed)) return false;
  kitsu868::presentation::ChunkRequest request{};
  request.sessionId = parsed.sessionId;
  request.offset = parsed.offset;
  request.bytes = parsed.bytes;
  memcpy(request.expectedFrameSha256, parsed.digest, sizeof(parsed.digest));
  uint8_t chunk[kitsu868::presentation::kMaximumChunkBytes]{};
  kitsu868::presentation::ChunkResult result{};
  const kitsu868::presentation::Status status =
      petPresentationPreview.readChunk(request, chunk, sizeof(chunk), result);
  if (status != kitsu868::presentation::Status::Ok) {
    buildPresentationError(status, output);
    return true;
  }
  char encoded[kitsu868::presentation::kMaximumChunkBytes * 4U / 3U + 4U]{};
  size_t encodedBytes = 0U;
  if (!kitsu868::companion::encodeBase64Url(
          chunk, result.bytes, encoded, sizeof(encoded), encodedBytes)) {
    return false;
  }
  output.reserve(500U);
  output = "{\"ok\":true,\"schema\":1,\"session_id\":";
  output += String(parsed.sessionId);
  output += ",\"offset\":" + String(result.offset);
  output += ",\"bytes\":" + String(result.bytes);
  output += ",\"next_offset\":" + String(result.nextOffset);
  output += ",\"complete\":";
  output += result.complete ? "true" : "false";
  output += ",\"frame_sha256\":\"";
  appendUpperHex(output, parsed.digest, sizeof(parsed.digest));
  output += "\",\"data_b64\":\"";
  output.concat(encoded, encodedBytes);
  output += "\"}";
  memset(chunk, 0, sizeof(chunk));
  return output.length() <= kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

bool closePetPresentation(const uint8_t* payload, size_t payloadBytes,
                          String& output) {
  uint32_t sessionId = 0U;
  if (!parseSingleUintObject(payload, payloadBytes, "session_id", sessionId) ||
      sessionId == 0U) {
    return false;
  }
  const kitsu868::presentation::Status status =
      petPresentationPreview.close(sessionId);
  if (status != kitsu868::presentation::Status::Ok &&
      status != kitsu868::presentation::Status::Duplicate) {
    buildPresentationError(status, output);
    return true;
  }
  petPresentationOpenedAt = 0U;
  output = "{\"ok\":true,\"schema\":1,\"closed\":true}";
  return true;
}

const char* focusPhaseWire(kitsu868::focus::Phase phase) {
  using kitsu868::focus::Phase;
  switch (phase) {
    case Phase::Idle: return "idle";
    case Phase::Focus: return "focus";
    case Phase::Break: return "break";
    case Phase::Completed: return "completed";
  }
  return "idle";
}

const char* focusCompletionWire(kitsu868::focus::CompletionKind completion) {
  using kitsu868::focus::CompletionKind;
  switch (completion) {
    case CompletionKind::None: return "none";
    case CompletionKind::Natural: return "natural";
    case CompletionKind::Stopped: return "stopped";
    case CompletionKind::Cancelled: return "cancelled";
  }
  return "none";
}

const char* focusStatusWire(kitsu868::focus::Status status) {
  using kitsu868::focus::Status;
  switch (status) {
    case Status::Ok: return "ok";
    case Status::Duplicate: return "duplicate";
    case Status::InvalidArgument: return "invalid_argument";
    case Status::Busy: return "focus_busy";
    case Status::Conflict: return "session_conflict";
    case Status::WrongSession: return "wrong_session";
    case Status::WrongPhase: return "wrong_phase";
    case Status::ClockRollback: return "clock_rollback";
    case Status::InvalidClock: return "invalid_clock";
  }
  return "request_rejected";
}

bool buildFocusStateBody(String& output) {
  if (!focusStateReady) {
    output = "{\"ok\":false,\"error\":\"focus_unavailable\"}";
    return true;
  }
  const kitsu868::focus::View view = focusSession.view();
  output.reserve(520U);
  output = "{\"ok\":true,\"schema\":1,\"phase\":\"";
  output += focusPhaseWire(view.phase);
  output += "\",\"completion\":\"";
  output += focusCompletionWire(view.completion);
  output += "\",\"session_id\":";
  output += String(static_cast<unsigned long>(view.sessionId));
  output += ",\"focus_minutes\":" + String(view.focusMinutes);
  output += ",\"break_minutes\":" + String(view.breakMinutes);
  output += ",\"elapsed_ms\":" + String(view.elapsedMs);
  output += ",\"remaining_ms\":" + String(view.remainingMs);
  output += ",\"sequence\":" + String(view.sequence);
  output += ",\"prompt\":{";
  output += "\"title\":\"" + jsonEscaped(String(view.prompt.title));
  output += "\",\"detail\":\"" + jsonEscaped(String(view.prompt.detail));
  output += "\",\"recommend_pulse_breathing\":";
  output += view.prompt.recommendPulseBreathing ? "true" : "false";
  output += "}}";
  return output.length() <= kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

bool buildFocusState(const uint8_t* payload, size_t payloadBytes,
                     String& output) {
  return emptyObject(payload, payloadBytes) && buildFocusStateBody(output);
}

void buildFocusError(kitsu868::focus::Status status, String& output) {
  output = "{\"ok\":false,\"error\":\"";
  output += focusStatusWire(status);
  output += "\"}";
}

bool persistFocusOrRestore(const kitsu868::focus::FocusState& before,
                           String& output) {
  if (persistFocusState()) {
    focusPersistedCheckpoint =
        focusSession.view().elapsedMs / FOCUS_CHECKPOINT_MS;
    focusPersistPending = false;
    companionBleRefreshDirty = true;
    return buildFocusStateBody(output);
  }
  (void)restoreFocusSnapshot(before, millis());
  focusPersistedCheckpoint =
      focusSession.view().elapsedMs / FOCUS_CHECKPOINT_MS;
  focusPersistPending = false;
  output = "{\"ok\":false,\"error\":\"storage_failed\"}";
  return true;
}

bool parseFocusStart(const uint8_t* payload, size_t payloadBytes,
                     uint32_t& sessionId, uint16_t& minutes) {
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) {
    return false;
  }
  size_t cursor = 0U;
  uint8_t seen = 0U;
  uint32_t parsedMinutes = 0U;
  if (!consume(payload, payloadBytes, cursor, '{')) return false;
  for (uint8_t field = 0U; field < 2U; ++field) {
    if (field != 0U && !consume(payload, payloadBytes, cursor, ',')) {
      return false;
    }
    const uint8_t* key = nullptr;
    size_t keyBytes = 0U;
    if (!parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) ||
        !consume(payload, payloadBytes, cursor, ':')) {
      return false;
    }
    if (sameToken(key, keyBytes, "session_id") && (seen & 1U) == 0U) {
      if (!parseNeighborUint32(payload, payloadBytes, cursor, sessionId) ||
          sessionId == 0U) {
        return false;
      }
      seen |= 1U;
    } else if (sameToken(key, keyBytes, "minutes") &&
               (seen & 2U) == 0U) {
      if (!parseNeighborUint32(payload, payloadBytes, cursor, parsedMinutes) ||
          parsedMinutes < kitsu868::focus::kMinimumFocusMinutes ||
          parsedMinutes > kitsu868::focus::kMaximumFocusMinutes) {
        return false;
      }
      minutes = static_cast<uint16_t>(parsedMinutes);
      seen |= 2U;
    } else {
      return false;
    }
  }
  if (!consume(payload, payloadBytes, cursor, '}')) return false;
  skipWhitespace(payload, payloadBytes, cursor);
  return seen == 3U && cursor == payloadBytes;
}

bool startFocus(const uint8_t* payload, size_t payloadBytes, String& output) {
  uint32_t sessionId = 0U;
  uint16_t minutes = 0U;
  if (!parseFocusStart(payload, payloadBytes, sessionId, minutes)) return false;
  if (!focusStateReady) return buildFocusStateBody(output);

  kitsu868::focus::StartRequest request{};
  request.sessionId = sessionId;
  if (minutes == kitsu868::focus::kTwentyFiveMinutePreset) {
    request.duration = kitsu868::focus::DurationKind::TwentyFiveMinutes;
  } else if (minutes == kitsu868::focus::kFiftyMinutePreset) {
    request.duration = kitsu868::focus::DurationKind::FiftyMinutes;
  } else {
    request.duration = kitsu868::focus::DurationKind::Custom;
    request.customMinutes = minutes;
  }

  const kitsu868::focus::FocusState before = focusSession.snapshot();
  kitsu868::focus::Update update{};
  const kitsu868::focus::Status status =
      focusSession.start(request, focusClock(millis()), update);
  if (status == kitsu868::focus::Status::Duplicate) {
    return buildFocusStateBody(output);
  }
  if (status != kitsu868::focus::Status::Ok) {
    buildFocusError(status, output);
    return true;
  }
  return persistFocusOrRestore(before, output);
}

bool mutateFocusSession(const uint8_t* payload, size_t payloadBytes,
                        const char* operation, String& output) {
  uint32_t sessionId = 0U;
  if (!parseSingleUintObject(payload, payloadBytes, "session_id", sessionId) ||
      sessionId == 0U) {
    return false;
  }
  if (!focusStateReady) return buildFocusStateBody(output);
  const kitsu868::focus::FocusState before = focusSession.snapshot();
  kitsu868::focus::Update update{};
  kitsu868::focus::Status status = kitsu868::focus::Status::InvalidArgument;
  if (strcmp(operation, "stop") == 0) {
    status = focusSession.stop(sessionId, focusClock(millis()), update);
  } else if (strcmp(operation, "cancel") == 0) {
    status = focusSession.cancel(sessionId, focusClock(millis()), update);
  } else if (strcmp(operation, "ack") == 0) {
    status = focusSession.acknowledge(sessionId);
  }
  if (status != kitsu868::focus::Status::Ok) {
    buildFocusError(status, output);
    return true;
  }
  return persistFocusOrRestore(before, output);
}

const char* walkTerrainWire(kitsu868::adventure::Terrain value) {
  using kitsu868::adventure::Terrain;
  switch (value) {
    case Terrain::Meadow: return "meadow";
    case Terrain::Forest: return "forest";
    case Terrain::Ridge: return "ridge";
    case Terrain::Waterfront: return "waterfront";
    case Terrain::Town: return "town";
    case Terrain::Count: break;
  }
  return "meadow";
}

const char* walkObjectiveWire(kitsu868::adventure::Objective value) {
  using kitsu868::adventure::Objective;
  switch (value) {
    case Objective::Explore: return "explore";
    case Objective::FollowSignal: return "follow_signal";
    case Objective::MeetCreature: return "meet_creature";
    case Objective::Community: return "community";
    case Objective::ReturnHome: return "return_home";
    case Objective::Count: break;
  }
  return "explore";
}

const char* walkRiskWire(kitsu868::adventure::Risk value) {
  using kitsu868::adventure::Risk;
  switch (value) {
    case Risk::Careful: return "careful";
    case Risk::Balanced: return "balanced";
    case Risk::Bold: return "bold";
    case Risk::Count: break;
  }
  return "balanced";
}

const char* walkWeatherWire(kitsu868::adventure::Weather value) {
  using kitsu868::adventure::Weather;
  switch (value) {
    case Weather::Unknown: return "unknown";
    case Weather::Clear: return "clear";
    case Weather::Rain: return "rain";
    case Weather::Wind: return "wind";
    case Weather::Snow: return "snow";
    case Weather::Count: break;
  }
  return "unknown";
}

const char* walkPhaseWire(kitsu868::adventure::RoutePhase value) {
  using kitsu868::adventure::RoutePhase;
  switch (value) {
    case RoutePhase::Idle: return "idle";
    case RoutePhase::Active: return "active";
    case RoutePhase::AwaitingRescue: return "awaiting_rescue";
    case RoutePhase::Returned: return "returned";
    case RoutePhase::Count: break;
  }
  return "idle";
}

const char* walkOutcomeWire(kitsu868::adventure::Outcome value) {
  using kitsu868::adventure::Outcome;
  switch (value) {
    case Outcome::None: return "none";
    case Outcome::Partial: return "partial";
    case Outcome::Complete: return "complete";
    case Outcome::EarlyReturn: return "early_return";
    case Outcome::Rescued: return "rescued";
    case Outcome::Count: break;
  }
  return "none";
}

const char* walkPrivacyWire(kitsu868::adventure::PrivacyMode value) {
  using kitsu868::adventure::PrivacyMode;
  switch (value) {
    case PrivacyMode::Off: return "off";
    case PrivacyMode::Coarse: return "coarse";
    case PrivacyMode::PreciseTransient: return "precise_transient";
    case PrivacyMode::Count: break;
  }
  return "off";
}

bool buildWalkStateBody(String& output) {
  if (!adventureProgressionReady) {
    output = "{\"ok\":false,\"error\":\"walk_unavailable\"}";
    return true;
  }
  const kitsu868::adventure::RouteView route = adventureProgression.view();
  const kitsu868::adventure::ProgressState state =
      adventureProgression.snapshot();
  output.reserve(1400U);
  output = "{\"ok\":true,\"schema\":1,\"phase\":\"";
  output += walkPhaseWire(route.phase);
  output += "\",\"outcome\":\"";
  output += walkOutcomeWire(route.outcome);
  output += "\",\"route_id\":" + String(route.routeId);
  output += ",\"steps\":" + String(route.steps);
  output += ",\"target_steps\":" + String(route.targetSteps);
  output += ",\"progress_percent\":" + String(route.progressPercent);
  output += ",\"distance_meters\":" +
      String(route.routeDistanceMeters);
  output += ",\"terrain\":\"";
  output += walkTerrainWire(route.terrain);
  output += "\",\"objective\":\"";
  output += walkObjectiveWire(route.objective);
  output += "\",\"risk\":\"";
  output += walkRiskWire(route.risk);
  output += "\",\"weather\":\"";
  output += walkWeatherWire(route.weather);
  output += "\",\"personality\":\"";
  output += kitsu868::CompanionBrain::personalityLabel(route.personality);
  output += "\",\"decision_count\":" + String(route.decisionCount);
  output += ",\"branch\":" + String(route.branchCode);
  output += ",\"privacy\":\"";
  output += walkPrivacyWire(static_cast<kitsu868::adventure::PrivacyMode>(
      state.privacyMode));
  output += "\",\"current_zone\":" + String(state.currentZoneToken);
  output += ",\"home_zone\":" + String(state.homeZoneToken);
  output += ",\"known_zones\":" + String(state.zoneCount);
  output += ",\"total_distance_meters\":" +
      String(state.totalDistanceMeters);
  output += ",\"journal_count\":" +
      String(adventureProgression.journalCount());
  output += ",\"postcard\":";
  kitsu868::adventure::TextPostcard postcard{};
  if (adventureProgression.currentPostcard(postcard)) {
    output += "{\"title\":\"" + jsonEscaped(String(postcard.title));
    output += "\",\"line\":\"" + jsonEscaped(String(postcard.line));
    output += "\"}";
  } else {
    output += "null";
  }
  output += "}";
  return output.length() <= kitsu868::companion::kMaximumEnvelopePayloadBytes;
}

bool buildWalkState(const uint8_t* payload, size_t payloadBytes,
                    String& output) {
  return emptyObject(payload, payloadBytes) && buildWalkStateBody(output);
}

bool parseWalkTerrain(const uint8_t* value, size_t bytes,
                      kitsu868::adventure::Terrain& output) {
  if (sameToken(value, bytes, "meadow")) output = kitsu868::adventure::Terrain::Meadow;
  else if (sameToken(value, bytes, "forest")) output = kitsu868::adventure::Terrain::Forest;
  else if (sameToken(value, bytes, "ridge")) output = kitsu868::adventure::Terrain::Ridge;
  else if (sameToken(value, bytes, "waterfront")) output = kitsu868::adventure::Terrain::Waterfront;
  else if (sameToken(value, bytes, "town")) output = kitsu868::adventure::Terrain::Town;
  else return false;
  return true;
}

bool parseWalkObjective(const uint8_t* value, size_t bytes,
                        kitsu868::adventure::Objective& output) {
  if (sameToken(value, bytes, "explore")) output = kitsu868::adventure::Objective::Explore;
  else if (sameToken(value, bytes, "follow_signal")) output = kitsu868::adventure::Objective::FollowSignal;
  else if (sameToken(value, bytes, "meet_creature")) output = kitsu868::adventure::Objective::MeetCreature;
  else if (sameToken(value, bytes, "community")) output = kitsu868::adventure::Objective::Community;
  else if (sameToken(value, bytes, "return_home")) output = kitsu868::adventure::Objective::ReturnHome;
  else return false;
  return true;
}

bool parseWalkRisk(const uint8_t* value, size_t bytes,
                   kitsu868::adventure::Risk& output) {
  if (sameToken(value, bytes, "careful")) output = kitsu868::adventure::Risk::Careful;
  else if (sameToken(value, bytes, "balanced")) output = kitsu868::adventure::Risk::Balanced;
  else if (sameToken(value, bytes, "bold")) output = kitsu868::adventure::Risk::Bold;
  else return false;
  return true;
}

bool parseWalkWeather(const uint8_t* value, size_t bytes,
                      kitsu868::adventure::Weather& output) {
  if (sameToken(value, bytes, "unknown")) output = kitsu868::adventure::Weather::Unknown;
  else if (sameToken(value, bytes, "clear")) output = kitsu868::adventure::Weather::Clear;
  else if (sameToken(value, bytes, "rain")) output = kitsu868::adventure::Weather::Rain;
  else if (sameToken(value, bytes, "wind")) output = kitsu868::adventure::Weather::Wind;
  else if (sameToken(value, bytes, "snow")) output = kitsu868::adventure::Weather::Snow;
  else return false;
  return true;
}

struct WalkStartRequest {
  kitsu868::adventure::Terrain terrain = kitsu868::adventure::Terrain::Meadow;
  kitsu868::adventure::Objective objective = kitsu868::adventure::Objective::Explore;
  kitsu868::adventure::Risk risk = kitsu868::adventure::Risk::Balanced;
  kitsu868::adventure::Weather weather = kitsu868::adventure::Weather::Unknown;
  uint32_t targetSteps = 0U;
  bool commuteSafe = false;
};

bool parseWalkStart(const uint8_t* payload, size_t payloadBytes,
                    WalkStartRequest& request) {
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) {
    return false;
  }
  size_t cursor = 0U;
  uint8_t seen = 0U;
  if (!consume(payload, payloadBytes, cursor, '{')) return false;
  for (uint8_t field = 0U; field < 6U; ++field) {
    if (field != 0U && !consume(payload, payloadBytes, cursor, ',')) return false;
    const uint8_t* key = nullptr;
    size_t keyBytes = 0U;
    if (!parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) ||
        !consume(payload, payloadBytes, cursor, ':')) return false;
    if (sameToken(key, keyBytes, "terrain") && (seen & 1U) == 0U) {
      const uint8_t* value = nullptr; size_t bytes = 0U;
      if (!parseAsciiString(payload, payloadBytes, cursor, value, bytes) ||
          !parseWalkTerrain(value, bytes, request.terrain)) return false;
      seen |= 1U;
    } else if (sameToken(key, keyBytes, "objective") && (seen & 2U) == 0U) {
      const uint8_t* value = nullptr; size_t bytes = 0U;
      if (!parseAsciiString(payload, payloadBytes, cursor, value, bytes) ||
          !parseWalkObjective(value, bytes, request.objective)) return false;
      seen |= 2U;
    } else if (sameToken(key, keyBytes, "risk") && (seen & 4U) == 0U) {
      const uint8_t* value = nullptr; size_t bytes = 0U;
      if (!parseAsciiString(payload, payloadBytes, cursor, value, bytes) ||
          !parseWalkRisk(value, bytes, request.risk)) return false;
      seen |= 4U;
    } else if (sameToken(key, keyBytes, "weather") && (seen & 8U) == 0U) {
      const uint8_t* value = nullptr; size_t bytes = 0U;
      if (!parseAsciiString(payload, payloadBytes, cursor, value, bytes) ||
          !parseWalkWeather(value, bytes, request.weather)) return false;
      seen |= 8U;
    } else if (sameToken(key, keyBytes, "target_steps") && (seen & 16U) == 0U) {
      if (!parseNeighborUint32(payload, payloadBytes, cursor,
                               request.targetSteps) ||
          request.targetSteps < 100U || request.targetSteps > 100000U) {
        return false;
      }
      seen |= 16U;
    } else if (sameToken(key, keyBytes, "commute_safe") && (seen & 32U) == 0U) {
      if (!parseBoolToken(payload, payloadBytes, cursor, request.commuteSafe)) {
        return false;
      }
      seen |= 32U;
    } else {
      return false;
    }
  }
  if (!consume(payload, payloadBytes, cursor, '}')) return false;
  skipWhitespace(payload, payloadBytes, cursor);
  return seen == 63U && cursor == payloadBytes;
}

void buildWalkMutationError(kitsu868::adventure::Status status,
                            String& output) {
  output = "{\"ok\":false,\"error\":\"";
  output += kitsu868::adventure::statusName(status);
  output += "\"}";
}

bool persistWalkOrRestore(const kitsu868::adventure::ProgressState& before,
                          String& output) {
  if (persistAdventureProgression()) return buildWalkStateBody(output);
  (void)adventureProgression.restore(before);
  output = "{\"ok\":false,\"error\":\"storage_failed\"}";
  return true;
}

bool startWalk(const uint8_t* payload, size_t payloadBytes, String& output) {
  WalkStartRequest parsed{};
  if (!parseWalkStart(payload, payloadBytes, parsed)) return false;
  if (!adventureProgressionReady) return buildWalkStateBody(output);
  kitsu868::adventure::RouteRequest request{};
  request.terrain = parsed.terrain;
  request.objective = parsed.objective;
  request.risk = parsed.commuteSafe ? kitsu868::adventure::Risk::Careful
                                    : parsed.risk;
  request.personality = companionBrain.personality().kind;
  request.weather = parsed.weather;
  request.baseTargetSteps = parsed.targetSteps;
  request.commuteSafe = parsed.commuteSafe ? 1U : 0U;
  const kitsu868::adventure::ProgressState before =
      adventureProgression.snapshot();
  const kitsu868::adventure::Status status = adventureProgression.begin(
      request, adventureClock(millis()), esp_random());
  if (status != kitsu868::adventure::Status::Ok) {
    buildWalkMutationError(status, output);
    return true;
  }
  return persistWalkOrRestore(before, output);
}

struct WalkSyncRequest {
  uint32_t routeId = 0U;
  uint32_t stepsTotal = 0U;
};

bool parseWalkSync(const uint8_t* payload, size_t payloadBytes,
                   WalkSyncRequest& request) {
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) return false;
  size_t cursor = 0U; uint8_t seen = 0U;
  if (!consume(payload, payloadBytes, cursor, '{')) return false;
  for (uint8_t field = 0U; field < 2U; ++field) {
    if (field != 0U && !consume(payload, payloadBytes, cursor, ',')) return false;
    const uint8_t* key = nullptr; size_t keyBytes = 0U;
    if (!parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) ||
        !consume(payload, payloadBytes, cursor, ':')) return false;
    if (sameToken(key, keyBytes, "route_id") && (seen & 1U) == 0U) {
      if (!parseNeighborUint32(payload, payloadBytes, cursor, request.routeId) ||
          request.routeId == 0U) return false;
      seen |= 1U;
    } else if (sameToken(key, keyBytes, "steps_total") && (seen & 2U) == 0U) {
      if (!parseNeighborUint32(payload, payloadBytes, cursor,
                               request.stepsTotal) ||
          request.stepsTotal > 100000U) return false;
      seen |= 2U;
    } else return false;
  }
  if (!consume(payload, payloadBytes, cursor, '}')) return false;
  skipWhitespace(payload, payloadBytes, cursor);
  return seen == 3U && cursor == payloadBytes;
}

bool syncWalk(const uint8_t* payload, size_t payloadBytes, String& output) {
  WalkSyncRequest request{};
  if (!parseWalkSync(payload, payloadBytes, request)) return false;
  const kitsu868::adventure::RouteView current = adventureProgression.view();
  if (current.routeId != request.routeId ||
      current.phase != kitsu868::adventure::RoutePhase::Active) {
    buildWalkMutationError(kitsu868::adventure::Status::WrongRoute, output);
    return true;
  }
  if (request.stepsTotal < current.steps) {
    output = "{\"ok\":false,\"error\":\"stale_steps\"}";
    return true;
  }
  if (request.stepsTotal == current.steps) return buildWalkStateBody(output);
  const kitsu868::adventure::ProgressState before =
      adventureProgression.snapshot();
  const kitsu868::adventure::Status status = adventureProgression.addSteps(
      request.stepsTotal - current.steps);
  if (status != kitsu868::adventure::Status::Ok) {
    buildWalkMutationError(status, output);
    return true;
  }
  return persistWalkOrRestore(before, output);
}

struct WalkLocationRequest {
  uint32_t routeId = 0U;
  uint32_t zoneToken = 0U;
  uint32_t stepsTotal = 0U;
  uint32_t distanceTotal = 0U;
};

bool parseWalkLocation(const uint8_t* payload, size_t payloadBytes,
                       WalkLocationRequest& request) {
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) return false;
  size_t cursor = 0U; uint8_t seen = 0U;
  if (!consume(payload, payloadBytes, cursor, '{')) return false;
  for (uint8_t field = 0U; field < 4U; ++field) {
    if (field != 0U && !consume(payload, payloadBytes, cursor, ',')) return false;
    const uint8_t* key = nullptr; size_t keyBytes = 0U; uint32_t value = 0U;
    if (!parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) ||
        !consume(payload, payloadBytes, cursor, ':') ||
        !parseNeighborUint32(payload, payloadBytes, cursor, value)) return false;
    if (sameToken(key, keyBytes, "route_id") && (seen & 1U) == 0U) {
      request.routeId = value; seen |= 1U;
    } else if (sameToken(key, keyBytes, "zone_token") && (seen & 2U) == 0U) {
      request.zoneToken = value; seen |= 2U;
    } else if (sameToken(key, keyBytes, "steps_total") && (seen & 4U) == 0U) {
      request.stepsTotal = value; seen |= 4U;
    } else if (sameToken(key, keyBytes, "distance_meters_total") &&
               (seen & 8U) == 0U) {
      request.distanceTotal = value; seen |= 8U;
    } else return false;
  }
  if (!consume(payload, payloadBytes, cursor, '}')) return false;
  skipWhitespace(payload, payloadBytes, cursor);
  return seen == 15U && request.routeId != 0U && request.zoneToken != 0U &&
      request.stepsTotal <= 100000U && request.distanceTotal <= 10000000U &&
      cursor == payloadBytes;
}

bool updateWalkLocation(const uint8_t* payload, size_t payloadBytes,
                        String& output) {
  WalkLocationRequest request{};
  if (!parseWalkLocation(payload, payloadBytes, request)) return false;
  const kitsu868::adventure::RouteView current = adventureProgression.view();
  if (current.routeId != request.routeId ||
      current.phase != kitsu868::adventure::RoutePhase::Active) {
    buildWalkMutationError(kitsu868::adventure::Status::WrongRoute, output);
    return true;
  }
  if (request.stepsTotal < current.steps ||
      request.distanceTotal < current.routeDistanceMeters) {
    output = "{\"ok\":false,\"error\":\"stale_walk_sample\"}";
    return true;
  }
  if (request.stepsTotal == current.steps &&
      request.distanceTotal == current.routeDistanceMeters) {
    return buildWalkStateBody(output);
  }
  const kitsu868::adventure::ProgressState before =
      adventureProgression.snapshot();
  kitsu868::adventure::LocationUpdate ignored{};
  const kitsu868::adventure::Status status =
      adventureProgression.observeCoarseZone(
          request.zoneToken, request.stepsTotal - current.steps,
          request.distanceTotal - current.routeDistanceMeters, ignored);
  if (status != kitsu868::adventure::Status::Ok) {
    buildWalkMutationError(status, output);
    return true;
  }
  return persistWalkOrRestore(before, output);
}

bool parseWalkDecision(const uint8_t* payload, size_t payloadBytes,
                       uint32_t& routeId,
                       kitsu868::adventure::MidDecision& decision) {
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) return false;
  size_t cursor = 0U; uint8_t seen = 0U;
  if (!consume(payload, payloadBytes, cursor, '{')) return false;
  for (uint8_t field = 0U; field < 2U; ++field) {
    if (field != 0U && !consume(payload, payloadBytes, cursor, ',')) return false;
    const uint8_t* key = nullptr; size_t keyBytes = 0U;
    if (!parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) ||
        !consume(payload, payloadBytes, cursor, ':')) return false;
    if (sameToken(key, keyBytes, "route_id") && (seen & 1U) == 0U) {
      if (!parseNeighborUint32(payload, payloadBytes, cursor, routeId) ||
          routeId == 0U) return false;
      seen |= 1U;
    } else if (sameToken(key, keyBytes, "decision") && (seen & 2U) == 0U) {
      const uint8_t* value = nullptr; size_t bytes = 0U;
      if (!parseAsciiString(payload, payloadBytes, cursor, value, bytes)) return false;
      if (sameToken(value, bytes, "continue")) decision = kitsu868::adventure::MidDecision::Continue;
      else if (sameToken(value, bytes, "detour")) decision = kitsu868::adventure::MidDecision::Detour;
      else if (sameToken(value, bytes, "help")) decision = kitsu868::adventure::MidDecision::Help;
      else if (sameToken(value, bytes, "return")) decision = kitsu868::adventure::MidDecision::ReturnEarly;
      else return false;
      seen |= 2U;
    } else return false;
  }
  if (!consume(payload, payloadBytes, cursor, '}')) return false;
  skipWhitespace(payload, payloadBytes, cursor);
  return seen == 3U && cursor == payloadBytes;
}

bool decideWalk(const uint8_t* payload, size_t payloadBytes, String& output) {
  uint32_t routeId = 0U;
  kitsu868::adventure::MidDecision decision{};
  if (!parseWalkDecision(payload, payloadBytes, routeId, decision)) return false;
  const kitsu868::adventure::RouteView current = adventureProgression.view();
  if (current.routeId != routeId) {
    buildWalkMutationError(kitsu868::adventure::Status::WrongRoute, output);
    return true;
  }
  const kitsu868::adventure::ProgressState before =
      adventureProgression.snapshot();
  kitsu868::adventure::Status status = adventureProgression.decide(decision);
  if (status == kitsu868::adventure::Status::Ok &&
      decision == kitsu868::adventure::MidDecision::ReturnEarly) {
    status = adventureProgression.finish(adventureClock(millis()));
  }
  if (status != kitsu868::adventure::Status::Ok &&
      status != kitsu868::adventure::Status::RescueRequired) {
    (void)adventureProgression.restore(before);
    buildWalkMutationError(status, output);
    return true;
  }
  return persistWalkOrRestore(before, output);
}

bool finishWalk(const uint8_t* payload, size_t payloadBytes, String& output) {
  uint32_t routeId = 0U;
  if (!parseSingleUintObject(payload, payloadBytes, "route_id", routeId) ||
      routeId == 0U) return false;
  const kitsu868::adventure::RouteView current = adventureProgression.view();
  if (current.routeId != routeId) {
    buildWalkMutationError(kitsu868::adventure::Status::WrongRoute, output);
    return true;
  }
  const kitsu868::adventure::ProgressState before =
      adventureProgression.snapshot();
  const kitsu868::adventure::Status status =
      adventureProgression.finish(adventureClock(millis()));
  if (status != kitsu868::adventure::Status::Ok &&
      status != kitsu868::adventure::Status::RescueRequired) {
    buildWalkMutationError(status, output);
    return true;
  }
  return persistWalkOrRestore(before, output);
}

bool acknowledgeWalk(const uint8_t* payload, size_t payloadBytes,
                     String& output) {
  uint32_t routeId = 0U;
  if (!parseSingleUintObject(payload, payloadBytes, "route_id", routeId) ||
      routeId == 0U) return false;
  const kitsu868::adventure::ProgressState before =
      adventureProgression.snapshot();
  const kitsu868::adventure::Status status =
      adventureProgression.acknowledge(routeId);
  if (status != kitsu868::adventure::Status::Ok) {
    buildWalkMutationError(status, output);
    return true;
  }
  return persistWalkOrRestore(before, output);
}

bool setWalkPrivacy(const uint8_t* payload, size_t payloadBytes,
                    String& output) {
  const uint8_t* value = nullptr; size_t valueBytes = 0U;
  if (!parseSingleStringObject(payload, payloadBytes, "mode", value,
                               valueBytes)) return false;
  kitsu868::adventure::PrivacyMode mode = kitsu868::adventure::PrivacyMode::Off;
  if (sameToken(value, valueBytes, "coarse")) {
    mode = kitsu868::adventure::PrivacyMode::Coarse;
  } else if (sameToken(value, valueBytes, "precise_transient")) {
    mode = kitsu868::adventure::PrivacyMode::PreciseTransient;
  } else if (!sameToken(value, valueBytes, "off")) return false;
  const kitsu868::adventure::ProgressState before =
      adventureProgression.snapshot();
  const uint32_t salt = mode == kitsu868::adventure::PrivacyMode::PreciseTransient
      ? companionBrain.deviceFingerprint() ^ UINT32_C(0xA7D04319) : 0U;
  const kitsu868::adventure::Status status =
      adventureProgression.setPrivacyMode(mode, salt);
  if (status == kitsu868::adventure::Status::NoChange) {
    return buildWalkStateBody(output);
  }
  if (status != kitsu868::adventure::Status::Ok) {
    buildWalkMutationError(status, output);
    return true;
  }
  return persistWalkOrRestore(before, output);
}

bool setWalkHome(const uint8_t* payload, size_t payloadBytes,
                 String& output) {
  uint32_t zoneToken = 0U;
  if (!parseSingleUintObject(payload, payloadBytes, "zone_token", zoneToken) ||
      zoneToken == 0U) return false;
  const kitsu868::adventure::ProgressState before =
      adventureProgression.snapshot();
  const kitsu868::adventure::Status status =
      adventureProgression.setHomeZone(zoneToken);
  if (status == kitsu868::adventure::Status::NoChange) {
    return buildWalkStateBody(output);
  }
  if (status != kitsu868::adventure::Status::Ok) {
    buildWalkMutationError(status, output);
    return true;
  }
  return persistWalkOrRestore(before, output);
}

bool startFunExpedition(const uint8_t* payload, size_t payloadBytes,
                        String& output) {
  const uint8_t* value = nullptr;
  size_t valueBytes = 0U;
  if (!parseSingleStringObject(payload, payloadBytes, "duration", value,
                               valueBytes)) {
    return false;
  }
  kitsu868::expedition::Duration duration =
      kitsu868::expedition::Duration::Short;
  if (sameToken(value, valueBytes, "medium")) {
    duration = kitsu868::expedition::Duration::Medium;
  } else if (sameToken(value, valueBytes, "long")) {
    duration = kitsu868::expedition::Duration::Long;
  } else if (!sameToken(value, valueBytes, "short")) {
    return false;
  }
  const char* error = nullptr;
  if (!startExpedition(duration, error)) {
    buildFunError(error, output);
    return true;
  }
  return buildFunStateBody(output);
}

bool claimFunExpedition(const uint8_t* payload, size_t payloadBytes,
                        String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  const char* error = nullptr;
  if (!claimExpedition(error)) {
    buildFunError(error, output);
    return true;
  }
  return buildFunStateBody(output);
}

bool startFunStory(const uint8_t* payload, size_t payloadBytes,
                   String& output) {
  const uint8_t* value = nullptr;
  size_t valueBytes = 0U;
  if (!parseSingleStringObject(payload, payloadBytes, "trigger", value,
                               valueBytes)) {
    return false;
  }
  kitsu868::dialogue::StoryTrigger trigger =
      kitsu868::dialogue::StoryTrigger::QuietMoment;
  if (sameToken(value, valueBytes, "expedition")) {
    trigger = kitsu868::dialogue::StoryTrigger::ExpeditionReturn;
  } else if (sameToken(value, valueBytes, "nearby")) {
    trigger = kitsu868::dialogue::StoryTrigger::NearbySignal;
  } else if (!sameToken(value, valueBytes, "quiet")) {
    return false;
  }
  if (dialogueStories.activeStory != kitsu868::dialogue::kNoActiveStory) {
    buildFunError("story_busy", output);
    return true;
  }
  const kitsu868::dialogue::StoryState before = dialogueStories;
  kitsu868::dialogue::StoryBeat beat{};
  if (!kitsu868::dialogue::startStory(
          trigger, companionBrain.personality().kind,
          companionBrain.deviceFingerprint(), dialogueStories, beat) ||
      !persistDialogueState()) {
    dialogueStories = before;
    buildFunError("story_start_failed", output);
    return true;
  }
  storyResolutionAvailable = false;
  momentView.active = true;
  momentView.line1 = beat.line1;
  momentView.line2 = beat.line2;
  momentView.until = millis() + MOMENT_DISPLAY_MS;
  companionBleRefreshDirty = true;
  return buildFunStateBody(output);
}

bool advanceFunStory(const uint8_t* payload, size_t payloadBytes,
                     String& output) {
  uint32_t storyId = 0U;
  if (!parseSingleUintObject(payload, payloadBytes, "story_id", storyId) ||
      storyId == 0U || storyId > kitsu868::dialogue::kStoryCount) {
    return false;
  }
  kitsu868::dialogue::StoryBeat current{};
  if (!kitsu868::dialogue::currentStoryBeat(dialogueStories, current) ||
      storyId != static_cast<uint32_t>(current.storyId) ||
      current.awaitsChoice) {
    buildFunError("story_state_changed", output);
    return true;
  }
  const kitsu868::dialogue::StoryState before = dialogueStories;
  kitsu868::dialogue::StoryBeat next{};
  if (!kitsu868::dialogue::advanceStory(dialogueStories, next) ||
      !persistDialogueState()) {
    dialogueStories = before;
    buildFunError("story_advance_failed", output);
    return true;
  }
  momentView.active = true;
  momentView.line1 = next.line1;
  momentView.line2 = next.line2;
  momentView.until = millis() + MOMENT_DISPLAY_MS;
  companionBleRefreshDirty = true;
  return buildFunStateBody(output);
}

bool parseStoryChoice(const uint8_t* payload, size_t payloadBytes,
                      uint8_t& storyId, uint8_t& choice) {
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) {
    return false;
  }
  size_t cursor = 0U;
  uint8_t seen = 0U;
  if (!consume(payload, payloadBytes, cursor, '{')) return false;
  for (uint8_t fields = 0U; fields < 2U; ++fields) {
    if (fields != 0U && !consume(payload, payloadBytes, cursor, ',')) {
      return false;
    }
    const uint8_t* key = nullptr;
    size_t keyBytes = 0U;
    uint32_t value = 0U;
    if (!parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) ||
        !consume(payload, payloadBytes, cursor, ':') ||
        !parseNeighborUint32(payload, payloadBytes, cursor, value)) {
      return false;
    }
    if (sameToken(key, keyBytes, "story_id") && (seen & 1U) == 0U &&
        value >= 1U && value <= kitsu868::dialogue::kStoryCount) {
      storyId = static_cast<uint8_t>(value);
      seen |= 1U;
    } else if (sameToken(key, keyBytes, "choice") && (seen & 2U) == 0U &&
               value < kitsu868::dialogue::kStoryChoiceCount) {
      choice = static_cast<uint8_t>(value);
      seen |= 2U;
    } else {
      return false;
    }
  }
  if (!consume(payload, payloadBytes, cursor, '}')) return false;
  skipWhitespace(payload, payloadBytes, cursor);
  return seen == 3U && cursor == payloadBytes;
}

bool chooseFunStory(const uint8_t* payload, size_t payloadBytes,
                    String& output) {
  uint8_t storyId = 0U;
  uint8_t choice = 0U;
  if (!parseStoryChoice(payload, payloadBytes, storyId, choice)) return false;
  kitsu868::dialogue::StoryBeat current{};
  if (!kitsu868::dialogue::currentStoryBeat(dialogueStories, current) ||
      storyId != current.storyId ||
      !current.awaitsChoice) {
    buildFunError("story_state_changed", output);
    return true;
  }
  const kitsu868::dialogue::StoryState beforeStories = dialogueStories;
  const uint8_t beforeEnergy = wisp.energy;
  const uint8_t beforeCuriosity = wisp.curiosity;
  const uint8_t beforeAffection = wisp.affection;
  kitsu868::dialogue::StoryResolution resolution{};
  if (!kitsu868::dialogue::resolveStory(
          static_cast<kitsu868::dialogue::StoryChoice>(choice),
          companionBrain.personality().kind, dialogueStories, resolution)) {
    buildFunError("story_choice_failed", output);
    return true;
  }
  // The phone contract and care-state policy cap one story's energy cost at
  // one point. Keep the displayed outcome identical to what is actually
  // applied even if a future catalogue supplies a larger negative request.
  if (resolution.energyDelta < -1) resolution.energyDelta = -1;
  wisp.energy = addClampedStat(wisp.energy, resolution.energyDelta);
  wisp.curiosity = addClampedStat(wisp.curiosity,
                                  resolution.curiosityDelta);
  wisp.affection = addClampedStat(wisp.affection,
                                  resolution.affectionDelta);
  const bool careStored = saveState();
  const bool storyStored = careStored && persistDialogueState();
  if (!storyStored) {
    dialogueStories = beforeStories;
    wisp.energy = beforeEnergy;
    wisp.curiosity = beforeCuriosity;
    wisp.affection = beforeAffection;
    if (careStored && !saveState()) {
      Serial.println("KITSU_WARN story_core_rollback=failed");
    }
    if (dialogueStateReady && !persistDialogueState()) {
      Serial.println("KITSU_WARN story_state_rollback=failed");
    }
    buildFunError("story_reward_store_failed", output);
    return true;
  }
  lastStoryResolution = resolution;
  storyResolutionAvailable = true;
  momentView.active = true;
  momentView.line1 = resolution.line1;
  momentView.line2 = resolution.line2;
  momentView.until = millis() + MOMENT_DISPLAY_MS;
  lastMemory = String(resolution.line1) + ": " + resolution.line2;
  companionBleRefreshDirty = true;
  return buildFunStateBody(output);
}

bool scanFunParty(const uint8_t* payload, size_t payloadBytes,
                  String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  const char* error = nullptr;
  if (!startPartyScan(error)) {
    buildFunError(error, output);
    return true;
  }
  return buildFunStateBody(output);
}

bool hostFunParty(const uint8_t* payload, size_t payloadBytes,
                  String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  const char* error = nullptr;
  if (!startPartyHost(error)) {
    buildFunError(error, output);
    return true;
  }
  return buildFunStateBody(output);
}

bool parsePartyJoin(const uint8_t* payload, size_t payloadBytes,
                    uint16_t& hostUid, uint32_t& sessionNonce) {
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) {
    return false;
  }
  size_t cursor = 0U;
  uint8_t seen = 0U;
  if (!consume(payload, payloadBytes, cursor, '{')) return false;
  for (uint8_t fields = 0U; fields < 2U; ++fields) {
    if (fields != 0U && !consume(payload, payloadBytes, cursor, ',')) {
      return false;
    }
    const uint8_t* key = nullptr;
    size_t keyBytes = 0U;
    if (!parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) ||
        !consume(payload, payloadBytes, cursor, ':')) {
      return false;
    }
    if (sameToken(key, keyBytes, "host_device_id") && (seen & 1U) == 0U) {
      const uint8_t* value = nullptr;
      size_t valueBytes = 0U;
      if (!parseAsciiString(payload, payloadBytes, cursor, value, valueBytes) ||
          !parseNeighborDeviceId(value, valueBytes, hostUid)) {
        return false;
      }
      seen |= 1U;
    } else if (sameToken(key, keyBytes, "session_nonce") &&
               (seen & 2U) == 0U &&
               parseNeighborUint32(payload, payloadBytes, cursor,
                                   sessionNonce) && sessionNonce != 0U) {
      seen |= 2U;
    } else {
      return false;
    }
  }
  if (!consume(payload, payloadBytes, cursor, '}')) return false;
  skipWhitespace(payload, payloadBytes, cursor);
  return seen == 3U && cursor == payloadBytes;
}

bool joinFunParty(const uint8_t* payload, size_t payloadBytes,
                  String& output) {
  uint16_t hostUid = 0U;
  uint32_t sessionNonce = 0U;
  if (!parsePartyJoin(payload, payloadBytes, hostUid, sessionNonce)) {
    return false;
  }
  const char* error = nullptr;
  if (!joinObservedParty(hostUid, sessionNonce, error)) {
    buildFunError(error, output);
    return true;
  }
  return buildFunStateBody(output);
}

bool beginFunParty(const uint8_t* payload, size_t payloadBytes,
                   String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  const char* error = nullptr;
  if (!beginHostedParty(error)) {
    buildFunError(error, output);
    return true;
  }
  return buildFunStateBody(output);
}

bool parsePartyChoice(const uint8_t* payload, size_t payloadBytes,
                      uint8_t& round,
                      kitsu868::party::SignalChoice& choice) {
  if (!payload || !kitsu868::companion::validUtf8(payload, payloadBytes)) {
    return false;
  }
  size_t cursor = 0U;
  uint8_t seen = 0U;
  if (!consume(payload, payloadBytes, cursor, '{')) return false;
  for (uint8_t fields = 0U; fields < 2U; ++fields) {
    if (fields != 0U && !consume(payload, payloadBytes, cursor, ',')) {
      return false;
    }
    const uint8_t* key = nullptr;
    size_t keyBytes = 0U;
    if (!parseAsciiString(payload, payloadBytes, cursor, key, keyBytes) ||
        !consume(payload, payloadBytes, cursor, ':')) {
      return false;
    }
    if (sameToken(key, keyBytes, "round") && (seen & 1U) == 0U) {
      uint32_t value = 0U;
      if (!parseNeighborUint32(payload, payloadBytes, cursor, value) ||
          value < 1U || value > kitsu868::party::kHuntRounds) {
        return false;
      }
      round = static_cast<uint8_t>(value);
      seen |= 1U;
    } else if (sameToken(key, keyBytes, "choice") &&
               (seen & 2U) == 0U) {
      const uint8_t* value = nullptr;
      size_t valueBytes = 0U;
      if (!parseAsciiString(payload, payloadBytes, cursor, value,
                            valueBytes)) {
        return false;
      }
      if (sameToken(value, valueBytes, "sweep")) {
        choice = kitsu868::party::SignalChoice::Sweep;
      } else if (sameToken(value, valueBytes, "listen")) {
        choice = kitsu868::party::SignalChoice::Listen;
      } else if (sameToken(value, valueBytes, "pulse")) {
        choice = kitsu868::party::SignalChoice::Pulse;
      } else {
        return false;
      }
      seen |= 2U;
    } else {
      return false;
    }
  }
  if (!consume(payload, payloadBytes, cursor, '}')) return false;
  skipWhitespace(payload, payloadBytes, cursor);
  return seen == 3U && cursor == payloadBytes;
}

bool chooseFunParty(const uint8_t* payload, size_t payloadBytes,
                    String& output) {
  uint8_t round = 0U;
  kitsu868::party::SignalChoice choice =
      kitsu868::party::SignalChoice::None;
  if (!parsePartyChoice(payload, payloadBytes, round, choice)) return false;
  const char* error = nullptr;
  if (!choosePartySignal(choice, round, error)) {
    buildFunError(error, output);
    return true;
  }
  return buildFunStateBody(output);
}

bool leaveFunParty(const uint8_t* payload, size_t payloadBytes,
                   String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  leavePartyHotspot();
  return buildFunStateBody(output);
}

}  // namespace companion_api


__attribute__((noinline)) bool handleCompanionBleRequest(
    const kitsu868::companion::DecodedEnvelope& request,
    const uint8_t* payload, size_t payloadBytes, uint8_t* responsePayload,
    size_t responseCapacity, size_t& responseBytes) {
  if (strcmp(request.operation, "action.apply") == 0) {
    return companion_api::applyAction(payload, payloadBytes, responsePayload,
                                      responseCapacity, responseBytes);
  }
  String response;
  bool handled = false;
  if (strcmp(request.operation, "state.get") == 0) {
    handled = companion_api::buildState(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "history.get") == 0) {
    handled = companion_api::buildHistory(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "peers.get") == 0) {
    handled = companion_api::buildPeers(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "messages.get") == 0) {
    handled = companion_api::buildMessagesV1(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "messages.get.v2") == 0) {
    handled = companion_api::buildMessagesV2(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "messages.get.v3") == 0) {
    handled = companion_api::buildMessagesV3(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "messages.get.v4") == 0) {
    handled = companion_api::buildMessagesV4(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "messages.mark_read") == 0) {
    handled = companion_api::markMessagesRead(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "encounter.codes.get.v1") == 0) {
    handled = companion_api::buildEncounterCodes(payload, payloadBytes,
                                                  response);
  } else if (strcmp(request.operation, "encounter.neighbors.get.v1") == 0) {
    handled = companion_api::buildEncounterNeighbors(payload, payloadBytes,
                                                      response);
  } else if (strcmp(request.operation, "encounter.neighbor.action.v1") == 0) {
    handled = companion_api::applyNeighborAction(payload, payloadBytes,
                                                 response);
  } else if (strcmp(request.operation, "encounter.catalog.get.v1") == 0) {
    handled = companion_api::buildEncounterCatalog(payload, payloadBytes,
                                                    response);
  } else if (strcmp(request.operation, "encounter.discovery.get.v1") == 0) {
    handled = companion_api::buildEncounterDiscovery(payload, payloadBytes,
                                                      response);
  } else if (strcmp(request.operation, "companion.profile.get.v1") == 0) {
    handled = companion_api::buildCompanionProfile(payload, payloadBytes,
                                                    response);
  } else if (strcmp(request.operation,
                    "companion.profile.nickname.set.v1") == 0) {
    handled = companion_api::setCompanionNickname(payload, payloadBytes,
                                                   response);
  } else if (strcmp(request.operation,
                    "companion.request.answer.v1") == 0) {
    handled = companion_api::answerCompanionRequest(payload, payloadBytes,
                                                     response);
  } else if (strcmp(request.operation,
                    "companion.question.answer.v1") == 0) {
    handled = companion_api::answerCompanionQuestion(payload, payloadBytes,
                                                      response);
  } else if (strcmp(request.operation,
                    "companion.presentation.open.v1") == 0) {
    handled = companion_api::openPetPresentation(payload, payloadBytes,
                                                  response);
  } else if (strcmp(request.operation,
                    "companion.presentation.read.v1") == 0) {
    handled = companion_api::readPetPresentation(payload, payloadBytes,
                                                  response);
  } else if (strcmp(request.operation,
                    "companion.presentation.close.v1") == 0) {
    handled = companion_api::closePetPresentation(payload, payloadBytes,
                                                   response);
  } else if (strcmp(request.operation, "focus.state.get.v1") == 0) {
    handled = companion_api::buildFocusState(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "focus.start.v1") == 0) {
    handled = companion_api::startFocus(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "focus.stop.v1") == 0) {
    handled = companion_api::mutateFocusSession(payload, payloadBytes,
                                                 "stop", response);
  } else if (strcmp(request.operation, "focus.cancel.v1") == 0) {
    handled = companion_api::mutateFocusSession(payload, payloadBytes,
                                                 "cancel", response);
  } else if (strcmp(request.operation, "focus.ack.v1") == 0) {
    handled = companion_api::mutateFocusSession(payload, payloadBytes,
                                                 "ack", response);
  } else if (strcmp(request.operation, "adventure.state.get.v1") == 0) {
    handled = companion_api::buildWalkState(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "adventure.walk.start.v1") == 0) {
    handled = companion_api::startWalk(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "adventure.walk.sync.v1") == 0) {
    handled = companion_api::syncWalk(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "adventure.walk.location.v1") == 0) {
    handled = companion_api::updateWalkLocation(payload, payloadBytes,
                                                 response);
  } else if (strcmp(request.operation, "adventure.walk.decide.v1") == 0) {
    handled = companion_api::decideWalk(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "adventure.walk.finish.v1") == 0) {
    handled = companion_api::finishWalk(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "adventure.walk.ack.v1") == 0) {
    handled = companion_api::acknowledgeWalk(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "adventure.privacy.set.v1") == 0) {
    handled = companion_api::setWalkPrivacy(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "adventure.home.set.v1") == 0) {
    handled = companion_api::setWalkHome(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "fun.state.get.v1") == 0) {
    handled = companion_api::buildFunState(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "fun.expedition.start.v1") == 0) {
    handled = companion_api::startFunExpedition(payload, payloadBytes,
                                                response);
  } else if (strcmp(request.operation, "fun.expedition.claim.v1") == 0) {
    handled = companion_api::claimFunExpedition(payload, payloadBytes,
                                                response);
  } else if (strcmp(request.operation, "fun.story.start.v1") == 0) {
    handled = companion_api::startFunStory(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "fun.story.advance.v1") == 0) {
    handled = companion_api::advanceFunStory(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "fun.story.choose.v1") == 0) {
    handled = companion_api::chooseFunStory(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "fun.party.scan.v1") == 0) {
    handled = companion_api::scanFunParty(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "fun.party.host.v1") == 0) {
    handled = companion_api::hostFunParty(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "fun.party.join.v1") == 0) {
    handled = companion_api::joinFunParty(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "fun.party.begin.v1") == 0) {
    handled = companion_api::beginFunParty(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "fun.party.choose.v1") == 0) {
    handled = companion_api::chooseFunParty(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "fun.party.leave.v1") == 0) {
    handled = companion_api::leaveFunParty(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "channels.get") == 0) {
    handled = companion_api::buildChannels(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "channels.get.v2") == 0) {
    handled = companion_api::buildChannelsV2(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "chat.storage.get") == 0) {
    handled = companion_api::buildChatStorage(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "clock.sync") == 0) {
    handled = companion_api::syncClock(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "mesh.configure") == 0) {
    handled = companion_api::configureMesh(payload, payloadBytes, response);
  }

  if (!handled) {
    response = "{\"ok\":false,\"error\":\"request_rejected\"}";
  }
  return companion_api::copyResponse(response, responsePayload,
                                     responseCapacity, responseBytes);
}

const char* chatStateName(ChatJournalState state) {
  switch (state) {
    case ChatJournalState::Received: return "received";
    case ChatJournalState::Queued: return "queued";
    case ChatJournalState::Sent: return "sent";
    case ChatJournalState::Delivered: return "delivered";
    case ChatJournalState::Unconfirmed: return "unconfirmed";
    case ChatJournalState::Failed: return "failed";
    case ChatJournalState::Cancelled: return "cancelled";
  }
  return "failed";
}

const char* chatLocalTxName(const ChatJournalEntry& entry) {
  if (entry.inbound || entry.state == ChatJournalState::Received) {
    return "not_applicable";
  }
  switch (entry.state) {
    case ChatJournalState::Queued: return "pending";
    case ChatJournalState::Sent:
    case ChatJournalState::Delivered:
    case ChatJournalState::Unconfirmed:
      return "sent";
    case ChatJournalState::Failed: return "failed";
    case ChatJournalState::Cancelled: return "cancelled";
    case ChatJournalState::Received: return "not_applicable";
  }
  return "failed";
}

const char* chatDeliveryAckName(const ChatJournalEntry& entry) {
  if (entry.inbound ||
      entry.kind != kitsu868::mesh::MessageKind::Direct) {
    return "not_applicable";
  }
  switch (entry.state) {
    case ChatJournalState::Sent: return "pending";
    case ChatJournalState::Delivered: return "received";
    case ChatJournalState::Unconfirmed: return "not_received";
    case ChatJournalState::Received:
    case ChatJournalState::Queued:
    case ChatJournalState::Failed:
    case ChatJournalState::Cancelled:
      return "not_applicable";
  }
  return "not_applicable";
}

const char* chatKindName(kitsu868::mesh::MessageKind kind) {
  return kind == kitsu868::mesh::MessageKind::Direct ? "direct" : "channel";
}

const char* chatRouteName(kitsu868::mesh::MessageRoute route) {
  return route == kitsu868::mesh::MessageRoute::Direct ? "direct" : "flood";
}

String shortHex(const uint8_t* bytes, size_t byteCount) {
  String result;
  result.reserve(byteCount * 2U);
  for (size_t index = 0; index < byteCount; ++index) {
    char encoded[3];
    snprintf(encoded, sizeof(encoded), "%02X", bytes[index]);
    result += encoded;
  }
  return result;
}

void advanceChatJournalGeneration() {
  chatSession = kitsu868::message_read::advanceJournalSession(chatSession);
  chatJournalRevision = 0U;
  // Rows retained from the old generation remain available to the physical
  // Inbox and legacy v1 reader, but v2 never relabels them as current. Clear
  // their physical unread state so the OLED badge cannot refer to rows that a
  // current-generation Android snapshot is unable to acknowledge.
  for (uint8_t ordinal = 0U; ordinal < chatJournalCount; ++ordinal) {
    const uint8_t index = static_cast<uint8_t>(
        (chatJournalStart + ordinal) % kitsu868::chat::kInboxCapacity);
    chatJournal[index].unread = false;
  }
  unreadChatMessages = 0U;
  companionBleRefreshDirty = true;
}

uint32_t allocateChatMessageId() {
  uint32_t id = nextChatMessageId++;
  if (id == 0U) {
    id = 1U;
    nextChatMessageId = 2U;
    advanceChatJournalGeneration();
  }
  return id;
}

uint32_t allocateChatRevision() {
  uint32_t revision = chatJournalRevision + 1U;
  if (revision == 0U) {
    revision = 1U;
    advanceChatJournalGeneration();
  }
  chatJournalRevision = revision;
  return revision;
}

void touchChatJournal(ChatJournalEntry& entry) {
  entry.revision = allocateChatRevision();
  entry.journalSession = chatSession;
  companionBleRefreshDirty = true;
}

ChatJournalEntry& appendChatJournal() {
  constexpr uint8_t capacity = kitsu868::chat::kInboxCapacity;
  uint8_t writeIndex = 0;
  if (chatJournalCount < capacity) {
    writeIndex = static_cast<uint8_t>(
        (chatJournalStart + chatJournalCount) % capacity);
    ++chatJournalCount;
  } else {
    writeIndex = chatJournalStart;
    if (chatJournal[writeIndex].inbound &&
        chatJournal[writeIndex].unread && unreadChatMessages > 0U) {
      --unreadChatMessages;
    }
    chatJournalStart = static_cast<uint8_t>(
        (chatJournalStart + 1U) % capacity);
    ++chatJournalDropped;
  }
  chatJournal[writeIndex] = ChatJournalEntry{};
  chatJournal[writeIndex].id = allocateChatMessageId();
  touchChatJournal(chatJournal[writeIndex]);
  return chatJournal[writeIndex];
}

uint8_t applyChatJournalReadPlan(const uint8_t* journalIndexes,
                                 uint8_t indexCount) {
  uint8_t marked = 0U;
  if (!journalIndexes || indexCount > kitsu868::chat::kInboxCapacity) {
    return marked;
  }
  bool shouldMark[kitsu868::chat::kInboxCapacity]{};
  for (uint8_t ordinal = 0U; ordinal < indexCount; ++ordinal) {
    const uint8_t index = journalIndexes[ordinal];
    if (index >= kitsu868::chat::kInboxCapacity ||
        !chatJournal[index].inbound || !chatJournal[index].unread) {
      continue;
    }
    shouldMark[ordinal] = true;
    ++marked;
  }

  // Reserve the complete revision range before changing any unread bit. If
  // the batch would cross UINT32_MAX, rotate generation once up front; never
  // split one authenticated atomic request across two journal sessions.
  if (kitsu868::message_read::revisionBatchRequiresGenerationAdvance(
          chatJournalRevision, marked)) {
    advanceChatJournalGeneration();
  }

  for (uint8_t ordinal = 0U; ordinal < indexCount; ++ordinal) {
    if (!shouldMark[ordinal]) continue;
    const uint8_t index = journalIndexes[ordinal];
    chatJournal[index].unread = false;
    touchChatJournal(chatJournal[index]);
  }

  // Recount from the source of truth so the physical OLED badge converges
  // after either a selective Android acknowledgement or local Inbox open.
  uint8_t unread = 0U;
  for (uint8_t ordinal = 0U; ordinal < chatJournalCount; ++ordinal) {
    const uint8_t index = static_cast<uint8_t>(
        (chatJournalStart + ordinal) % kitsu868::chat::kInboxCapacity);
    if (chatJournal[index].inbound && chatJournal[index].unread) ++unread;
  }
  unreadChatMessages = unread;
  return marked;
}

void markChatJournalRead() {
  uint8_t selected[kitsu868::chat::kInboxCapacity]{};
  uint8_t selectedCount = 0U;
  for (uint8_t ordinal = 0U; ordinal < chatJournalCount; ++ordinal) {
    const uint8_t index = static_cast<uint8_t>(
        (chatJournalStart + ordinal) % kitsu868::chat::kInboxCapacity);
    if (chatJournal[index].inbound && chatJournal[index].unread) {
      selected[selectedCount++] = index;
    }
  }
  (void)applyChatJournalReadPlan(selected, selectedCount);
}

ChatJournalEntry* chatJournalNewest(uint8_t newestOffset) {
  if (newestOffset >= chatJournalCount) return nullptr;
  const uint8_t index = static_cast<uint8_t>(
      (chatJournalStart + chatJournalCount - 1U - newestOffset) %
      kitsu868::chat::kInboxCapacity);
  return &chatJournal[index];
}

ChatJournalEntry* findChatByDelivery(
    const kitsu868::mesh::DeliveryEvent& delivery) {
  for (uint8_t offset = 0; offset < chatJournalCount; ++offset) {
    ChatJournalEntry* entry = chatJournalNewest(offset);
    if (entry && !entry->inbound &&
        entry->kind == kitsu868::mesh::MessageKind::Direct &&
        entry->timestamp == delivery.messageTimestamp &&
        entry->expectedAck == delivery.expectedAck &&
        memcmp(entry->contactPublicKey, delivery.recipientPublicKey,
               sizeof(entry->contactPublicKey)) == 0 &&
        (entry->state == ChatJournalState::Queued ||
         entry->state == ChatJournalState::Sent)) {
      return entry;
    }
  }
  return nullptr;
}

String oledSafeText(const char* input) {
  String output;
  if (!input) return output;
  for (size_t index = 0; input[index] != '\0';) {
    const uint8_t first = static_cast<uint8_t>(input[index]);
    if (first >= 0x20U && first <= 0x7eU) {
      output += static_cast<char>(first);
      ++index;
      continue;
    }
    size_t sequenceBytes = 1;
    if ((first & 0xe0U) == 0xc0U) sequenceBytes = 2;
    else if ((first & 0xf0U) == 0xe0U) sequenceBytes = 3;
    else if ((first & 0xf8U) == 0xf0U) sequenceBytes = 4;
    output += '?';
    index += sequenceBytes;
  }
  return output;
}

kitsu868::CompanionVitals companionVitals() {
  kitsu868::CompanionVitals vitals;
  vitals.energy = wisp.energy;
  vitals.curiosity = wisp.curiosity;
  vitals.affection = wisp.affection;
  vitals.sleeping = wisp.sleeping;
  vitals.listening = radioListening;
  return vitals;
}

const char* animationRoleName(CompanionRole role) {
  switch (role) {
    case CompanionRole::Idle: return "idle";
    case CompanionRole::Blink: return "blink";
    case CompanionRole::Pet: return "pet";
    case CompanionRole::Sleep: return "sleep";
    case CompanionRole::Listen: return "listen";
    case CompanionRole::Surprise: return "surprise";
    case CompanionRole::Play: return "play";
    case CompanionRole::Tired: return "tired";
    case CompanionRole::Feed: return "feed";
    case CompanionRole::Wake: return "wake";
    case CompanionRole::Meet: return "meet";
    case CompanionRole::Evolve: return "evolve";
  }
  return "idle";
}

String moodText() {
  if (activeAnimation.active) {
    switch (activeAnimation.role) {
      case CompanionRole::Pet: return "delighted";
      case CompanionRole::Feed: return "satisfied";
      case CompanionRole::Play: return "playful";
      case CompanionRole::Surprise: return "startled";
      case CompanionRole::Sleep: return "dreaming";
      case CompanionRole::Listen: return "listening";
      case CompanionRole::Tired: return "drowsy";
      case CompanionRole::Wake: return "awake";
      case CompanionRole::Meet: return "hello";
      case CompanionRole::Evolve: return "radiant";
      default: break;
    }
  }
  if (wisp.sleeping) return "dreaming";
  if (radioListening) return "listening";
  return String(kitsu868::CompanionBrain::moodLabel(
      companionBrain.mood(companionVitals())));
}

uint32_t crc32Prefix(const void* value, size_t bytes) {
  const uint8_t* input = reinterpret_cast<const uint8_t*>(value);
  uint32_t crc = 0xffffffffUL;
  for (size_t index = 0; index < bytes; ++index) {
    crc ^= input[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^
          (0xedb88320UL & (0U - static_cast<uint32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

template <typename T>
bool writePreferenceRecord(const char* key, const T& value) {
  if (!storageReady || !key) return false;
  if (preferences.putBytes(key, &value, sizeof(value)) != sizeof(value) ||
      preferences.getBytesLength(key) != sizeof(value)) {
    return false;
  }
  T check{};
  return preferences.getBytes(key, &check, sizeof(check)) == sizeof(check) &&
         memcmp(&value, &check, sizeof(value)) == 0;
}

template <typename T>
bool readPreferenceRecord(const char* key, T& value) {
  return storageReady && key && preferences.getBytesLength(key) == sizeof(value) &&
         preferences.getBytes(key, &value, sizeof(value)) == sizeof(value);
}

uint32_t coreStateCrc(const CoreStateV2& state) {
  return crc32Prefix(&state, offsetof(CoreStateV2, crc32));
}

uint32_t signalStateCrc(const SignalStateV1& state) {
  return crc32Prefix(&state, offsetof(SignalStateV1, crc32));
}

uint32_t signalStateCrc(const SignalStateV2& state) {
  return crc32Prefix(&state, offsetof(SignalStateV2, crc32));
}

uint32_t funStateCrc(const FunStateV1& state) {
  return crc32Prefix(&state, offsetof(FunStateV1, crc32));
}

uint32_t pendingWildStateCrc(const PendingWildStateV1& state) {
  return crc32Prefix(&state, offsetof(PendingWildStateV1, crc32));
}

uint32_t dialogueStateCrc(const DialogueStateV1& state) {
  return crc32Prefix(&state, offsetof(DialogueStateV1, crc32));
}

uint32_t partyRewardStateCrc(const PartyRewardStateV1& state) {
  return crc32Prefix(&state, offsetof(PartyRewardStateV1, crc32));
}

bool persistDialogueState() {
  if (!storageReady || !dialogueStateReady ||
      !kitsu868::dialogue::validateActionState(dialogueActions) ||
      !kitsu868::dialogue::validateStoryState(dialogueStories)) {
    return false;
  }
  DialogueStateV1 stored{};
  stored.bytes = sizeof(stored);
  stored.actionSelections = dialogueActions.selections;
  for (uint8_t index = 0U;
       index < kitsu868::dialogue::kActionRecentCapacity; ++index) {
    stored.actionRecent[index] = dialogueActions.recent[index];
  }
  stored.actionRecentHead = dialogueActions.recentHead;
  stored.actionRecentCount = dialogueActions.recentCount;
  stored.storyStarts = dialogueStories.starts;
  stored.storyCompletedMask = dialogueStories.completedMask;
  stored.storyCompletions = dialogueStories.completions;
  stored.activeStory = dialogueStories.activeStory;
  stored.storyScene = dialogueStories.scene;
  for (uint8_t index = 0U;
       index < kitsu868::dialogue::kStoryRecentCapacity; ++index) {
    stored.storyRecent[index] = dialogueStories.recent[index];
  }
  stored.storyRecentHead = dialogueStories.recentHead;
  stored.storyRecentCount = dialogueStories.recentCount;
  stored.lastClaimedExpeditionId = lastClaimedExpeditionId;
  stored.crc32 = dialogueStateCrc(stored);
  const bool written =
      preferences.putBytes("dialog_v1", &stored, sizeof(stored)) ==
      sizeof(stored);
  DialogueStateV1 check{};
  return written &&
      preferences.getBytesLength("dialog_v1") == sizeof(check) &&
      preferences.getBytes("dialog_v1", &check, sizeof(check)) ==
          sizeof(check) &&
      memcmp(&stored, &check, sizeof(stored)) == 0;
}

void loadDialogueState() {
  kitsu868::dialogue::resetActionState(dialogueActions);
  kitsu868::dialogue::resetStoryState(dialogueStories);
  lastClaimedExpeditionId = 0U;
  dialogueStateReady = storageReady;
  if (!storageReady) return;
  const size_t bytes = preferences.getBytesLength("dialog_v1");
  if (bytes == 0U) return;
  DialogueStateV1 stored{};
  const bool recordValid = bytes == sizeof(stored) &&
      preferences.getBytes("dialog_v1", &stored, sizeof(stored)) ==
          sizeof(stored);
  if (!recordValid || stored.magic != DIALOGUE_STATE_MAGIC ||
      stored.schema != 1U || stored.bytes != sizeof(stored) ||
      stored.crc32 != dialogueStateCrc(stored)) {
    dialogueStateReady = false;
    Serial.println("KITSU_WARN dialogue_state=invalid");
    return;
  }
  kitsu868::dialogue::ActionState actions{};
  actions.selections = stored.actionSelections;
  for (uint8_t index = 0U;
       index < kitsu868::dialogue::kActionRecentCapacity; ++index) {
    actions.recent[index] = stored.actionRecent[index];
  }
  actions.recentHead = stored.actionRecentHead;
  actions.recentCount = stored.actionRecentCount;
  kitsu868::dialogue::StoryState stories{};
  stories.starts = stored.storyStarts;
  stories.completedMask = stored.storyCompletedMask;
  stories.completions = stored.storyCompletions;
  stories.activeStory = stored.activeStory;
  stories.scene = stored.storyScene;
  for (uint8_t index = 0U;
       index < kitsu868::dialogue::kStoryRecentCapacity; ++index) {
    stories.recent[index] = stored.storyRecent[index];
  }
  stories.recentHead = stored.storyRecentHead;
  stories.recentCount = stored.storyRecentCount;
  if (!kitsu868::dialogue::validateActionState(actions) ||
      !kitsu868::dialogue::validateStoryState(stories)) {
    dialogueStateReady = false;
    Serial.println("KITSU_WARN dialogue_state=invalid");
    return;
  }
  dialogueActions = actions;
  dialogueStories = stories;
  lastClaimedExpeditionId = stored.lastClaimedExpeditionId;
}

bool persistExpeditionState() {
  if (!storageReady || !expeditionStateReady) return false;
  const kitsu868::expedition::ExpeditionState stored =
      expeditionCore.snapshot();
  if (!kitsu868::expedition::validateExpeditionState(stored)) return false;
  const bool written =
      preferences.putBytes("exped_v1", &stored, sizeof(stored)) ==
      sizeof(stored);
  kitsu868::expedition::ExpeditionState check{};
  return written &&
      preferences.getBytesLength("exped_v1") == sizeof(check) &&
      preferences.getBytes("exped_v1", &check, sizeof(check)) ==
          sizeof(check) &&
      memcmp(&stored, &check, sizeof(stored)) == 0;
}

void loadExpeditionState() {
  expeditionCore.reset();
  expeditionStateReady = storageReady;
  if (!storageReady) return;
  const size_t bytes = preferences.getBytesLength("exped_v1");
  if (bytes == 0U) return;
  kitsu868::expedition::ExpeditionState stored{};
  if (bytes != sizeof(stored) ||
      preferences.getBytes("exped_v1", &stored, sizeof(stored)) !=
          sizeof(stored) ||
      expeditionCore.restore(stored) !=
          kitsu868::expedition::RestoreStatus::Ok) {
    expeditionCore.reset();
    expeditionStateReady = false;
    Serial.println("KITSU_WARN expedition_state=invalid");
  }
}

bool persistPartyRewardState() {
  if (!storageReady || !partyRewardStateReady) return false;
  PartyRewardStateV1 stored{};
  stored.bytes = sizeof(stored);
  stored.rewards = partyRewards.snapshot();
  if (!kitsu868::party::validateRewardState(stored.rewards)) return false;
  stored.crc32 = partyRewardStateCrc(stored);
  const bool written =
      preferences.putBytes("party_v1", &stored, sizeof(stored)) ==
      sizeof(stored);
  PartyRewardStateV1 check{};
  return written &&
      preferences.getBytesLength("party_v1") == sizeof(check) &&
      preferences.getBytes("party_v1", &check, sizeof(check)) ==
          sizeof(check) &&
      memcmp(&stored, &check, sizeof(stored)) == 0;
}

void loadPartyRewardState() {
  partyRewards.reset();
  partyRewardStateReady = storageReady;
  if (!storageReady) return;
  const size_t bytes = preferences.getBytesLength("party_v1");
  if (bytes == 0U) return;
  PartyRewardStateV1 stored{};
  if (bytes != sizeof(stored) ||
      preferences.getBytes("party_v1", &stored, sizeof(stored)) !=
          sizeof(stored) || stored.magic != PARTY_REWARD_STATE_MAGIC ||
      stored.schema != 1U || stored.bytes != sizeof(stored) ||
      stored.crc32 != partyRewardStateCrc(stored) ||
      partyRewards.restore(stored.rewards) !=
          kitsu868::party::RewardStatus::Awarded) {
    // restore() uses Awarded as its generic successful state; any semantic
    // failure quarantines only Party Bond, never care or companion identity.
    partyRewards.reset();
    partyRewardStateReady = false;
    Serial.println("KITSU_WARN party_reward_state=invalid");
  }
}

bool clockReading(uint32_t now, kitsu868::timekeeping::ClockReading& output,
                  bool requireTrusted) {
  const kitsu868::timekeeping::ClockResult result = kitsuClock.read(now, output);
  return result == kitsu868::timekeeping::ClockResult::Ok &&
         (!requireTrusted || output.trusted());
}

bool applyClockToRuntime(uint32_t now) {
  kitsu868::timekeeping::ClockReading reading{};
  if (!clockReading(now, reading, true) || reading.unixSeconds > UINT32_MAX) {
    return false;
  }
  const timeval systemTime{static_cast<time_t>(reading.unixSeconds),
                           static_cast<suseconds_t>(reading.millisecond) * 1000};
  if (settimeofday(&systemTime, nullptr) != 0) return false;
  return meshTransport.setEpoch(static_cast<uint32_t>(reading.unixSeconds)) ==
      kitsu868::mesh::TransportStatus::Ok;
}

bool persistClockState() {
  if (!storageReady || !clockStateReady || !kitsuClock.set()) return false;
  kitsu868::timekeeping::ClockSnapshot snapshot{};
  uint32_t generation = clockSnapshotGeneration + 1U;
  if (generation == 0U) generation = 1U;
  if (kitsuClock.makeSnapshot(millis(), generation, snapshot) !=
      kitsu868::timekeeping::ClockResult::Ok) {
    return false;
  }
  const uint8_t nextSlot = static_cast<uint8_t>(clockSnapshotSlot ^ 1U);
  const char* key = nextSlot == 0U ? "clock_a" : "clock_b";
  if (!writePreferenceRecord(key, snapshot)) return false;
  clockSnapshotGeneration = generation;
  clockSnapshotSlot = nextSlot;
  return true;
}

bool commitClockMutation(
    const kitsu868::timekeeping::KitsuClock& before,
    uint32_t previousGeneration, uint8_t previousSlot, uint32_t now,
    bool applyRuntime, const char** error) {
  if (error) *error = nullptr;
  if (clockStateReady && kitsuClock.set() && !persistClockState()) {
    kitsuClock = before;
    clockSnapshotGeneration = previousGeneration;
    clockSnapshotSlot = previousSlot;
    if (error) *error = "clock_storage_failed";
    return false;
  }
  if (!applyRuntime || applyClockToRuntime(now)) return true;

  kitsuClock = before;
  clockSnapshotGeneration = previousGeneration;
  clockSnapshotSlot = previousSlot;
  if (clockStateReady) {
    if (before.set()) {
      if (!persistClockState()) {
        Serial.println("KITSU_WARN clock_state=rollback_flush_failed");
      }
    } else {
      (void)preferences.remove("clock_a");
      (void)preferences.remove("clock_b");
      clockSnapshotGeneration = 0U;
      clockSnapshotSlot = 0U;
    }
  }
  if (before.trusted() && !applyClockToRuntime(now)) {
    Serial.println("KITSU_WARN clock_runtime=rollback_failed");
  }
  if (error) *error = "clock_runtime_failed";
  return false;
}

void loadClockState() {
  kitsuClock.reset();
  clockStateReady = storageReady;
  clockSnapshotGeneration = 0U;
  clockSnapshotSlot = 0U;
  if (!storageReady) return;
  kitsu868::timekeeping::ClockSnapshot first{};
  kitsu868::timekeeping::ClockSnapshot second{};
  const bool firstValid = readPreferenceRecord("clock_a", first) &&
      kitsu868::timekeeping::KitsuClock::validateSnapshot(first) ==
          kitsu868::timekeeping::ClockResult::Ok;
  const bool secondValid = readPreferenceRecord("clock_b", second) &&
      kitsu868::timekeeping::KitsuClock::validateSnapshot(second) ==
          kitsu868::timekeeping::ClockResult::Ok;
  if (!firstValid && !secondValid) return;
  const bool useSecond = secondValid &&
      (!firstValid || kitsu868::timekeeping::KitsuClock::generationAfter(
                         second.generation, first.generation));
  const kitsu868::timekeeping::ClockSnapshot& selected =
      useSecond ? second : first;
  if (kitsuClock.restore(selected, millis()) !=
      kitsu868::timekeeping::ClockResult::Ok) {
    kitsuClock.reset();
    Serial.println("KITSU_WARN clock_state=invalid");
    return;
  }
  clockSnapshotGeneration = selected.generation;
  clockSnapshotSlot = useSecond ? 1U : 0U;
  Serial.printf("KITSU_CLOCK restored=stale source=%s generation=%lu\n",
                kitsu868::timekeeping::clockSourceName(kitsuClock.source()),
                static_cast<unsigned long>(clockSnapshotGeneration));
}

bool persistCompanionProgression() {
  if (!storageReady || !companionProgressionReady) return false;
  uint8_t snapshot[kitsu868::progression::kSnapshotCapacity]{};
  const size_t bytes = kitsu868::progression::CompanionProgression::snapshotSize();
  if (bytes > sizeof(snapshot) ||
      !companionProgression.writeSnapshot(snapshot, sizeof(snapshot))) {
    return false;
  }
  if (preferences.putBytes("progress_v1", snapshot, bytes) != bytes ||
      preferences.getBytesLength("progress_v1") != bytes) {
    return false;
  }
  uint8_t check[kitsu868::progression::kSnapshotCapacity]{};
  return preferences.getBytes("progress_v1", check, bytes) == bytes &&
         memcmp(snapshot, check, bytes) == 0;
}

bool captureCompanionProgression(
    uint8_t (&snapshot)[kitsu868::progression::kSnapshotCapacity]) {
  return companionProgressionReady &&
      companionProgression.writeSnapshot(snapshot, sizeof(snapshot));
}

void restoreCompanionProgression(
    const uint8_t (&snapshot)[kitsu868::progression::kSnapshotCapacity]) {
  (void)companionProgression.restoreSnapshot(
      snapshot,
      kitsu868::progression::CompanionProgression::snapshotSize(),
      companionBrain.deviceFingerprint());
}

void loadCompanionProgression() {
  companionProgressionReady = storageReady;
  if (!storageReady) return;
  const uint32_t fingerprint = companionBrain.deviceFingerprint();
  const size_t expected =
      kitsu868::progression::CompanionProgression::snapshotSize();
  uint8_t snapshot[kitsu868::progression::kSnapshotCapacity]{};
  const size_t bytes = preferences.getBytesLength("progress_v1");
  if (bytes == expected &&
      preferences.getBytes("progress_v1", snapshot, bytes) == bytes &&
      companionProgression.restoreSnapshot(snapshot, bytes, fingerprint)) {
    return;
  }
  uint32_t day = 1U;
  uint16_t minute = 0U;
  (void)currentLocalDayMinute(day, minute, true);
  companionProgression.initialize(fingerprint, day);
  if (bytes != 0U) Serial.println("KITSU_WARN progression_state=reset");
  if (!persistCompanionProgression()) {
    companionProgressionReady = false;
    Serial.println("KITSU_WARN progression_state=unavailable");
  }
}

bool persistAdventureProgression() {
  if (!storageReady || !adventureProgressionReady) return false;
  const kitsu868::adventure::ProgressState state =
      adventureProgression.snapshot();
  return kitsu868::adventure::validateProgressState(state) &&
         writePreferenceRecord("advent_v1", state);
}

void loadAdventureProgression() {
  adventureProgression.reset();
  adventureProgressionReady = storageReady;
  if (!storageReady) return;
  kitsu868::adventure::ProgressState state{};
  if (preferences.getBytesLength("advent_v1") == 0U) {
    adventureProgressionReady = persistAdventureProgression();
    if (!adventureProgressionReady) {
      Serial.println("KITSU_WARN adventure_state=unavailable");
    }
    return;
  }
  if (!readPreferenceRecord("advent_v1", state) ||
      adventureProgression.restore(state) !=
          kitsu868::adventure::RestoreStatus::Ok) {
    adventureProgression.reset();
    Serial.println("KITSU_WARN adventure_state=invalid_reset");
    adventureProgressionReady = persistAdventureProgression();
    if (!adventureProgressionReady) {
      Serial.println("KITSU_WARN adventure_state=unavailable");
    }
  }
}

bool persistSocialProgression() {
  if (!storageReady || !socialProgressionReady) return false;
  const kitsu868::social::SocialState state = socialProgression.snapshot();
  return kitsu868::social::validateSocialState(state) &&
         writePreferenceRecord("social_v1", state);
}

void loadSocialProgression() {
  socialProgression.reset();
  socialProgressionReady = storageReady;
  if (!storageReady) return;
  kitsu868::social::SocialState state{};
  if (preferences.getBytesLength("social_v1") == 0U) {
    socialProgressionReady = persistSocialProgression();
    if (!socialProgressionReady) {
      Serial.println("KITSU_WARN social_state=unavailable");
    }
    return;
  }
  if (!readPreferenceRecord("social_v1", state) ||
      socialProgression.restore(state) != kitsu868::social::SocialStatus::Ok) {
    socialProgression.reset();
    Serial.println("KITSU_WARN social_state=invalid_reset");
    socialProgressionReady = persistSocialProgression();
    if (!socialProgressionReady) {
      Serial.println("KITSU_WARN social_state=unavailable");
    }
  }
}

bool persistActivityState() {
  if (!storageReady || !activityStateReady) return false;
  const kitsu868::activities::ActivityState state = activitySuite.snapshot();
  return kitsu868::activities::validateActivityState(state) &&
         writePreferenceRecord("activity_v1", state);
}

bool persistActivityMutation(
    const kitsu868::activities::ActivityState& before,
    const char* operation) {
  if (persistActivityState()) return true;
  (void)activitySuite.restore(before);
  Serial.printf("KITSU_ERROR activity=%s_store_failed\n",
                operation ? operation : "state");
  return false;
}

void loadActivityState() {
  activitySuite.reset();
  activityStateReady = storageReady;
  activityResumePending = false;
  if (!storageReady) return;
  kitsu868::activities::ActivityState state{};
  if (preferences.getBytesLength("activity_v1") == 0U) {
    activityStateReady = persistActivityState();
    if (!activityStateReady) {
      Serial.println("KITSU_WARN activity_state=unavailable");
    }
    return;
  }
  if (!readPreferenceRecord("activity_v1", state) ||
      !activitySuite.restore(state)) {
    activitySuite.reset();
    Serial.println("KITSU_WARN activity_state=invalid_reset");
    activityStateReady = persistActivityState();
    if (!activityStateReady) {
      Serial.println("KITSU_WARN activity_state=unavailable");
    }
    return;
  }
  const kitsu868::activities::ActivityPhase phase =
      activitySuite.view(millis()).phase;
  if (phase == kitsu868::activities::ActivityPhase::Presenting ||
      phase == kitsu868::activities::ActivityPhase::Playing ||
      phase == kitsu868::activities::ActivityPhase::Result) {
    activityResumePending = activitySuite.resumeAfterCrash(millis());
    // A persisted result may already have paid its care reward immediately
    // before power loss. Resume the visible result, but never duplicate that
    // non-idempotent reward after reboot.
    activityRewarded =
        phase == kitsu868::activities::ActivityPhase::Result;
    if (activityResumePending && !persistActivityState()) {
      activitySuite.reset();
      activityStateReady = false;
      activityResumePending = false;
      Serial.println("KITSU_WARN activity_state=resume_store_failed");
    }
  }
}

kitsu868::focus::ClockSample focusClock(uint32_t now) {
  kitsu868::focus::ClockSample sample{};
  sample.monotonicMs = now;
  kitsu868::timekeeping::ClockReading reading{};
  if (clockReading(now, reading, false) && reading.trusted()) {
    sample.unixTrusted = true;
    sample.unixSeconds = reading.unixSeconds;
  }
  return sample;
}

bool persistFocusState() {
  if (!storageReady || !focusStateReady) return false;
  const kitsu868::focus::FocusState state = focusSession.snapshot();
  return kitsu868::focus::validateFocusState(state) &&
      writePreferenceRecord("focus_v1", state);
}

bool restoreFocusSnapshot(const kitsu868::focus::FocusState& state,
                          uint32_t now) {
  kitsu868::focus::Update ignored{};
  return focusSession.restore(state, focusClock(now), ignored) ==
      kitsu868::focus::RestoreStatus::Ok;
}

void loadFocusState() {
  focusSession.reset();
  focusStateReady = storageReady;
  focusPersistPending = false;
  focusNextTickAt = millis();
  focusPersistRetryAt = 0U;
  focusPersistedCheckpoint = 0U;
  if (!storageReady) return;

  kitsu868::focus::FocusState state{};
  if (preferences.getBytesLength("focus_v1") == 0U) {
    focusStateReady = persistFocusState();
    if (!focusStateReady) {
      Serial.println("KITSU_WARN focus_state=unavailable");
    }
    return;
  }

  kitsu868::focus::Update restored{};
  if (!readPreferenceRecord("focus_v1", state) ||
      focusSession.restore(state, focusClock(millis()), restored) !=
          kitsu868::focus::RestoreStatus::Ok) {
    focusSession.reset();
    Serial.println("KITSU_WARN focus_state=invalid_reset");
    focusStateReady = persistFocusState();
    if (!focusStateReady) {
      Serial.println("KITSU_WARN focus_state=unavailable");
    }
    return;
  }

  focusPersistedCheckpoint =
      focusSession.view().elapsedMs / FOCUS_CHECKPOINT_MS;
  if (restored.changed && !persistFocusState()) {
    focusPersistPending = true;
    focusPersistRetryAt = millis() + 5000UL;
    Serial.println("KITSU_WARN focus_state=resume_store_failed");
  }
}

struct LocalClockSample {
  uint32_t epochSeconds = 0U;
  uint32_t dayId = 0U;
  uint16_t minuteOfDay = 0U;
};

bool localClockSample(uint32_t now, bool requireTrusted,
                      LocalClockSample& output) {
  kitsu868::timekeeping::ClockReading reading{};
  if (!clockReading(now, reading, requireTrusted) ||
      reading.unixSeconds > UINT32_MAX) {
    return false;
  }
  const int64_t localSeconds = static_cast<int64_t>(reading.unixSeconds) +
      static_cast<int64_t>(reading.utcOffsetMinutes) * 60LL;
  if (localSeconds < 0) return false;
  const uint64_t localDay = static_cast<uint64_t>(localSeconds / 86400LL);
  if (localDay == 0U || localDay > UINT32_MAX) return false;
  LocalClockSample candidate{};
  candidate.epochSeconds = static_cast<uint32_t>(reading.unixSeconds);
  candidate.dayId = static_cast<uint32_t>(localDay);
  candidate.minuteOfDay =
      static_cast<uint16_t>((localSeconds % 86400LL) / 60LL);
  output = candidate;
  return true;
}

bool currentLocalDayMinute(uint32_t& dayId, uint16_t& minuteOfDay,
                           bool requireTrusted) {
  LocalClockSample sample{};
  if (!localClockSample(millis(), requireTrusted, sample)) return false;
  dayId = sample.dayId;
  minuteOfDay = sample.minuteOfDay;
  return true;
}

void serviceClockNetworkTime(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    if (clockNtpStarted) sntp_stop();
    clockNtpStarted = false;
    clockNtpCandidate = false;
    return;
  }
  if (clockNtpLastAcceptedAt != 0U &&
      static_cast<uint32_t>(now - clockNtpLastAcceptedAt) <
          CLOCK_NTP_RESYNC_MS) {
    return;
  }
  if (!clockNtpStarted) {
    configTime(0, 0, "time.cloudflare.com", "time.google.com",
               "pool.ntp.org");
    clockNtpStarted = true;
    clockNtpCandidate = false;
    Serial.println("KITSU_CLOCK ntp=started");
  }
  const int64_t epoch = static_cast<int64_t>(time(nullptr));
  if (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED ||
      epoch < static_cast<int64_t>(kitsu868::timekeeping::kMinimumClockUnixSeconds) ||
      epoch > static_cast<int64_t>(kitsu868::timekeeping::kMaximumClockUnixSeconds)) {
    clockNtpCandidate = false;
    return;
  }
  if (!clockNtpCandidate) {
    clockNtpCandidate = true;
    clockNtpCandidateAt = now;
    clockNtpCandidateEpoch = epoch;
    return;
  }
  if (static_cast<uint32_t>(now - clockNtpCandidateAt) <
      CLOCK_NTP_STABILITY_MS) {
    return;
  }
  if (epoch < clockNtpCandidateEpoch || epoch > clockNtpCandidateEpoch + 30) {
    clockNtpCandidate = false;
    return;
  }
  kitsu868::timekeeping::NetworkTimeSample sample{};
  sample.unixSeconds = static_cast<uint64_t>(epoch);
  sample.synchronized = true;
  const kitsu868::timekeeping::KitsuClock before = kitsuClock;
  const uint32_t previousGeneration = clockSnapshotGeneration;
  const uint8_t previousSlot = clockSnapshotSlot;
  if (kitsuClock.acceptNetworkTime(sample, now) !=
          kitsu868::timekeeping::ClockResult::Ok ||
      !commitClockMutation(before, previousGeneration, previousSlot, now,
                           true)) {
    clockNtpCandidate = false;
    return;
  }
  clockNtpLastAcceptedAt = now == 0U ? 1U : now;
  clockNtpCandidate = false;
  Serial.printf("KITSU_CLOCK ntp=accepted epoch=%lld\n",
                static_cast<long long>(epoch));
}

bool parseUtcOffset(const String& text, int16_t& minutes) {
  if (text.length() != 6U || (text[0] != '+' && text[0] != '-') ||
      text[3] != ':' || !isDigit(text[1]) || !isDigit(text[2]) ||
      !isDigit(text[4]) || !isDigit(text[5])) {
    return false;
  }
  const uint16_t hours = static_cast<uint16_t>(
      (text[1] - '0') * 10U + (text[2] - '0'));
  const uint16_t remainder = static_cast<uint16_t>(
      (text[4] - '0') * 10U + (text[5] - '0'));
  if (remainder > 59U || hours > 14U || (hours == 14U && remainder != 0U)) {
    return false;
  }
  int16_t value = static_cast<int16_t>(hours * 60U + remainder);
  if (text[0] == '-') value = static_cast<int16_t>(-value);
  minutes = value;
  return true;
}

String utcOffsetText(int16_t minutes) {
  const char sign = minutes < 0 ? '-' : '+';
  uint16_t magnitude = static_cast<uint16_t>(minutes < 0 ? -minutes : minutes);
  char buffer[8]{};
  snprintf(buffer, sizeof(buffer), "%c%02u:%02u", sign,
           magnitude / 60U, magnitude % 60U);
  return String(buffer);
}

void printClockStatus(const char* action, const char* error = nullptr) {
  kitsu868::timekeeping::ClockReading reading{};
  const kitsu868::timekeeping::ClockResult result =
      kitsuClock.read(millis(), reading);
  char iso[kitsu868::timekeeping::kClockIso8601BufferBytes]{};
  const bool formatted = result == kitsu868::timekeeping::ClockResult::Ok &&
      kitsu868::timekeeping::KitsuClock::formatIso8601(
          reading.unixSeconds, reading.millisecond,
          reading.utcOffsetMinutes, iso, sizeof(iso)) ==
          kitsu868::timekeeping::ClockResult::Ok;
  Serial.printf(
      "KITSU_CLOCK {\"action\":\"%s\",\"status\":\"%s\","
      "\"error\":",
      action, error ? "rejected" : "ok");
  if (error) Serial.printf("\"%s\"", error);
  else Serial.print("null");
  Serial.printf(
      ",\"set\":%s,\"trusted\":%s,\"source\":\"%s\","
      "\"epoch\":",
      reading.set() ? "true" : "false",
      reading.trusted() ? "true" : "false",
      kitsu868::timekeeping::clockSourceName(reading.source));
  if (reading.set()) Serial.print(static_cast<unsigned long long>(reading.unixSeconds));
  else Serial.print("null");
  Serial.print(",\"iso\":");
  if (formatted) Serial.printf("\"%s\"", iso);
  else Serial.print("null");
  Serial.printf(",\"offset\":\"%s\"}\n",
                utcOffsetText(reading.utcOffsetMinutes).c_str());
}

bool commitClockEditor() {
  uint64_t epoch = 0U;
  int16_t offset = 0;
  if (!clockEditor.value(epoch, offset)) return false;
  const uint32_t now = millis();
  const kitsu868::timekeeping::KitsuClock before = kitsuClock;
  const uint32_t previousGeneration = clockSnapshotGeneration;
  const uint8_t previousSlot = clockSnapshotSlot;
  if (kitsuClock.setFromUnixSeconds(
          epoch, 0U, kitsu868::timekeeping::ClockSource::ManualDevice,
          offset, now) != kitsu868::timekeeping::ClockResult::Ok ||
      !commitClockMutation(before, previousGeneration, previousSlot, now,
                           true)) {
    Serial.println("KITSU_ERROR clock=device_store_failed");
    return false;
  }
  clockEditor.cancel();
  momentView.active = true;
  momentView.line1 = "CLOCK SET";
  momentView.line2 = "TIME IS READY";
  momentView.until = now + MOMENT_DISPLAY_MS;
  printClockStatus("device");
  enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
  return true;
}

void beginClockEditor() {
  if (foregroundTransitionBlocked("clock")) return;
  kitsu868::timekeeping::ClockReading reading{};
  uint64_t seed = kitsu868::timekeeping::kMinimumClockUnixSeconds;
  int16_t offset = kitsuClock.utcOffsetMinutes();
  if (clockReading(millis(), reading, false)) {
    seed = reading.unixSeconds;
    offset = reading.utcOffsetMinutes;
  }
  if (clockEditor.begin(seed, offset) !=
      kitsu868::timekeeping::ClockResult::Ok) {
    Serial.println("KITSU_ERROR clock=editor_unavailable");
    return;
  }
  enterScreen(Screen::Clock);
}

bool executeClockCommand(const String& input) {
  String command = input;
  command.trim();
  String lowered = command;
  lowered.toLowerCase();
  if (lowered == "clock status") {
    printClockStatus("status");
    return true;
  }
  if (lowered == "clock edit") {
    beginClockEditor();
    return true;
  }
  if (lowered == "clock ntp") {
    if (WiFi.status() != WL_CONNECTED) {
      printClockStatus("ntp", "wifi_not_connected");
    } else {
      clockNtpLastAcceptedAt = 0U;
      clockNtpCandidate = false;
      serviceClockNetworkTime(millis());
      printClockStatus("ntp_start");
    }
    return true;
  }
  if (lowered.startsWith("clock offset ")) {
    int16_t offset = 0;
    if (!parseUtcOffset(command.substring(13), offset)) {
      printClockStatus("offset", "invalid_offset");
      return true;
    }
    const uint32_t now = millis();
    const kitsu868::timekeeping::KitsuClock before = kitsuClock;
    const uint32_t previousGeneration = clockSnapshotGeneration;
    const uint8_t previousSlot = clockSnapshotSlot;
    if (kitsuClock.setUtcOffsetMinutes(offset) !=
            kitsu868::timekeeping::ClockResult::Ok) {
      printClockStatus("offset", "invalid_offset");
    } else if (!commitClockMutation(before, previousGeneration, previousSlot,
                                    now, false)) {
      printClockStatus("offset", "storage_failed");
    } else {
      printClockStatus("offset");
    }
    return true;
  }
  kitsu868::timekeeping::ClockResult result =
      kitsu868::timekeeping::ClockResult::InvalidArgument;
  const uint32_t now = millis();
  const kitsu868::timekeeping::KitsuClock before = kitsuClock;
  const uint32_t previousGeneration = clockSnapshotGeneration;
  const uint8_t previousSlot = clockSnapshotSlot;
  if (lowered.startsWith("clock set ")) {
    result = kitsuClock.setFromIso8601(
        command.substring(10).c_str(),
        kitsu868::timekeeping::ClockSource::ManualSerial, now);
  } else if (lowered.startsWith("clock unix ")) {
    result = kitsuClock.setFromUnixText(
        command.substring(11).c_str(),
        kitsu868::timekeeping::ClockSource::ManualSerial,
        kitsuClock.utcOffsetMinutes(), now);
  } else {
    return false;
  }
  if (result != kitsu868::timekeeping::ClockResult::Ok) {
    printClockStatus("set", kitsu868::timekeeping::clockResultName(result));
    return true;
  }
  if (!commitClockMutation(before, previousGeneration, previousSlot, now,
                           true)) {
    printClockStatus("set", "runtime_or_storage_failed");
    return true;
  }
  printClockStatus("set");
  return true;
}

bool persistSignalEncounterState() {
  if (!storageReady || !signalEncounterStateReady) return false;
  const kitsu868::signal::CoordinatorState current =
      signalEncounterCoordinator.snapshot();
  const kitsu868::signal::SignalTrailState trail = signalTrail.snapshot();
  if (!kitsu868::signal::validateSignalTrailState(trail)) return false;
  SignalStateV2 stored{};
  stored.bytes = sizeof(stored);
  stored.lastOperationId = current.lastOperationId;
  stored.hasLastRecord = current.hasLastRecord;
  if (current.hasLastRecord) {
    const kitsu868::signal::EncounterRecord& record = current.lastRecord;
    stored.operationKind = record.operationKind;
    stored.encounterOccurred = record.encounterOccurred;
    stored.guaranteed = record.guaranteed;
    stored.rarity = record.rarity;
    stored.codeOutcome = record.codeOutcome;
    stored.encounterRoll = record.encounterRollBasisPoints;
    stored.rarityRoll = record.rarityRollBasisPoints;
    stored.codeRoll = record.codeRollBasisPoints;
    stored.recordOperationId = record.operationId;
    stored.entropy = record.entropy;
  }
  stored.trailSchema = trail.schemaVersion;
  stored.trailMissCount = trail.missCount;
  stored.trailHasLastOperation = trail.hasLastOperation;
  stored.trailReserved = trail.reserved;
  stored.trailLastOperationId = trail.lastOperationId;
  stored.crc32 = signalStateCrc(stored);
  return preferences.putBytes("signal_v2", &stored, sizeof(stored)) ==
      sizeof(stored);
}

void loadSignalEncounterState() {
  signalEncounterCoordinator.reset();
  signalTrail.reset();
  signalEncounterStateReady =
      signalEncounterCoordinator.configurationStatus() ==
          kitsu868::signal::ConfigurationStatus::Ok;
  if (!storageReady || !signalEncounterStateReady) return;
  const size_t v2Bytes = preferences.getBytesLength("signal_v2");
  kitsu868::signal::CoordinatorState restored{};
  kitsu868::signal::SignalTrailState restoredTrail{};
  bool migrateV1 = false;
  SignalStateV2 stored{};
  if (v2Bytes != 0U) {
    if (v2Bytes != sizeof(stored) ||
        preferences.getBytes("signal_v2", &stored, sizeof(stored)) !=
            sizeof(stored) ||
        stored.magic != SIGNAL_STATE_V2_MAGIC || stored.schema != 2U ||
        stored.bytes != sizeof(stored) || stored.hasLastRecord > 1U ||
        stored.crc32 != signalStateCrc(stored)) {
      signalEncounterStateReady = false;
      Serial.println("KITSU_WARN signal_state=invalid");
      return;
    }
    restoredTrail.schemaVersion = stored.trailSchema;
    restoredTrail.missCount = stored.trailMissCount;
    restoredTrail.hasLastOperation = stored.trailHasLastOperation;
    restoredTrail.reserved = stored.trailReserved;
    restoredTrail.lastOperationId = stored.trailLastOperationId;
  } else {
    const size_t v1Bytes = preferences.getBytesLength("signal_v1");
    if (v1Bytes == 0U) return;
    SignalStateV1 legacy{};
    if (v1Bytes != sizeof(legacy) ||
        preferences.getBytes("signal_v1", &legacy, sizeof(legacy)) !=
            sizeof(legacy) || legacy.magic != SIGNAL_STATE_MAGIC ||
        legacy.schema != 1U || legacy.bytes != sizeof(legacy) ||
        legacy.hasLastRecord > 1U ||
        legacy.crc32 != signalStateCrc(legacy)) {
      signalEncounterStateReady = false;
      Serial.println("KITSU_WARN signal_state=invalid");
      return;
    }
    stored.lastOperationId = legacy.lastOperationId;
    stored.hasLastRecord = legacy.hasLastRecord;
    stored.operationKind = legacy.operationKind;
    stored.encounterOccurred = legacy.encounterOccurred;
    stored.guaranteed = legacy.guaranteed;
    stored.rarity = legacy.rarity;
    stored.codeOutcome = legacy.codeOutcome;
    stored.encounterRoll = legacy.encounterRoll;
    stored.rarityRoll = legacy.rarityRoll;
    stored.codeRoll = legacy.codeRoll;
    stored.recordOperationId = legacy.recordOperationId;
    stored.entropy = legacy.entropy;
    restoredTrail.hasLastOperation = legacy.hasLastRecord;
    restoredTrail.lastOperationId =
        legacy.hasLastRecord ? legacy.lastOperationId : 0U;
    migrateV1 = true;
  }
  restored.lastOperationId = stored.lastOperationId;
  restored.hasLastRecord = stored.hasLastRecord;
  if (restored.hasLastRecord) {
    restored.lastRecord.schemaVersion =
        kitsu868::signal::kRecordSchemaVersion;
    restored.lastRecord.operationKind = stored.operationKind;
    restored.lastRecord.encounterOccurred = stored.encounterOccurred;
    restored.lastRecord.guaranteed = stored.guaranteed;
    restored.lastRecord.rarity = stored.rarity;
    restored.lastRecord.codeOutcome = stored.codeOutcome;
    restored.lastRecord.encounterRollBasisPoints = stored.encounterRoll;
    restored.lastRecord.rarityRollBasisPoints = stored.rarityRoll;
    restored.lastRecord.codeRollBasisPoints = stored.codeRoll;
    restored.lastRecord.operationId = stored.recordOperationId;
    restored.lastRecord.entropy = stored.entropy;
  }
  if (signalEncounterCoordinator.restore(restored) !=
          kitsu868::signal::RestoreStatus::Ok ||
      signalTrail.restore(restoredTrail) !=
          kitsu868::signal::SignalTrailRestoreStatus::Ok ||
      restored.lastOperationId != restoredTrail.lastOperationId) {
    signalEncounterCoordinator.reset();
    signalTrail.reset();
    signalEncounterStateReady = false;
    Serial.println("KITSU_WARN signal_state=invalid_semantics");
    return;
  }
  if (migrateV1 && !persistSignalEncounterState()) {
    Serial.println("KITSU_WARN signal_state=migration_flush_failed");
  }
}

bool persistFunState() {
  if (!storageReady || !funStateReady ||
      !kitsu868::fun::validateDiscoveryState(funDiscovery)) {
    return false;
  }
  FunStateV1 stored{};
  stored.bytes = sizeof(stored);
  stored.seenMask = funDiscovery.seenMask;
  memcpy(stored.encounterCounts, funDiscovery.encounterCounts,
         sizeof(stored.encounterCounts));
  memcpy(stored.lastSources, funDiscovery.lastSources,
         sizeof(stored.lastSources));
  stored.completedDreams = funDiscovery.completedDreams;
  stored.rareReactions = funDiscovery.rareReactions;
  memcpy(stored.dreamHistory, funDiscovery.dreamHistory,
         sizeof(stored.dreamHistory));
  stored.dreamHead = funDiscovery.dreamHead;
  stored.dreamHistoryCount = funDiscovery.dreamHistoryCount;
  stored.lastEncounterOperationId = lastFunEncounterOperationId;
  stored.crc32 = funStateCrc(stored);
  return preferences.putBytes("fun_v1", &stored, sizeof(stored)) ==
      sizeof(stored);
}

void loadFunState() {
  kitsu868::fun::resetDiscoveryState(funDiscovery);
  lastFunEncounterOperationId = 0U;
  funStateReady = storageReady;
  if (!storageReady) return;
  const size_t bytes = preferences.getBytesLength("fun_v1");
  if (bytes == 0U) return;
  FunStateV1 stored{};
  if (bytes != sizeof(stored) ||
      preferences.getBytes("fun_v1", &stored, sizeof(stored)) !=
          sizeof(stored) || stored.magic != FUN_STATE_MAGIC ||
      stored.schema != 1U || stored.bytes != sizeof(stored) ||
      stored.crc32 != funStateCrc(stored)) {
    funStateReady = false;
    Serial.println("KITSU_WARN fun_state=invalid");
    return;
  }
  funDiscovery.seenMask = stored.seenMask;
  memcpy(funDiscovery.encounterCounts, stored.encounterCounts,
         sizeof(funDiscovery.encounterCounts));
  memcpy(funDiscovery.lastSources, stored.lastSources,
         sizeof(funDiscovery.lastSources));
  funDiscovery.completedDreams = stored.completedDreams;
  funDiscovery.rareReactions = stored.rareReactions;
  memcpy(funDiscovery.dreamHistory, stored.dreamHistory,
         sizeof(funDiscovery.dreamHistory));
  funDiscovery.dreamHead = stored.dreamHead;
  funDiscovery.dreamHistoryCount = stored.dreamHistoryCount;
  lastFunEncounterOperationId = stored.lastEncounterOperationId;
  if (!kitsu868::fun::validateDiscoveryState(funDiscovery)) {
    funStateReady = false;
    kitsu868::fun::resetDiscoveryState(funDiscovery);
    lastFunEncounterOperationId = 0U;
    Serial.println("KITSU_WARN fun_state=invalid_semantics");
  }
}

bool persistPendingWildEncounter() {
  if (!storageReady || pendingWildOperationId == 0U ||
      !wildEncounterView.available || !wildEncounterView.creature.name ||
      !kitsu868::signal::validOperationKind(wildEncounterView.source) ||
      !kitsu868::signal::validRarity(wildEncounterView.creature.rarity) ||
      static_cast<uint8_t>(pendingWildCodeOutcome) >
          static_cast<uint8_t>(kitsu868::signal::CodeOutcome::Revealed)) {
    return false;
  }
  PendingWildStateV1 stored{};
  stored.bytes = sizeof(stored);
  stored.operationId = pendingWildOperationId;
  stored.entropy = pendingWildEntropy;
  stored.packId = wildEncounterView.creature.packId;
  stored.active = 1U;
  stored.source = static_cast<uint8_t>(wildEncounterView.source);
  stored.rarity = static_cast<uint8_t>(wildEncounterView.creature.rarity);
  stored.codeOutcome = static_cast<uint8_t>(pendingWildCodeOutcome);
  stored.guaranteed = wildEncounterView.guaranteed ? 1U : 0U;
  stored.crc32 = pendingWildStateCrc(stored);
  const bool written =
      preferences.putBytes("wild_v1", &stored, sizeof(stored)) ==
      sizeof(stored);
  PendingWildStateV1 verification{};
  return written &&
      preferences.getBytesLength("wild_v1") == sizeof(verification) &&
      preferences.getBytes("wild_v1", &verification,
                           sizeof(verification)) == sizeof(verification) &&
      memcmp(&verification, &stored, sizeof(stored)) == 0;
}

bool clearPendingWildEncounter() {
  if (!storageReady) return false;
  if (preferences.getBytesLength("wild_v1") == 0U) return true;
  return preferences.remove("wild_v1") &&
      preferences.getBytesLength("wild_v1") == 0U;
}

void loadPendingWildEncounter() {
  wildEncounterView = WildEncounterView{};
  pendingWildOperationId = 0U;
  pendingWildEntropy = 0U;
  pendingWildCodeOutcome = kitsu868::signal::CodeOutcome::NotApplicable;
  pendingWildMaterialized = false;
  if (!storageReady) return;
  const size_t bytes = preferences.getBytesLength("wild_v1");
  if (bytes == 0U) return;
  PendingWildStateV1 stored{};
  if (bytes != sizeof(stored) ||
      preferences.getBytes("wild_v1", &stored, sizeof(stored)) !=
          sizeof(stored) || stored.magic != PENDING_WILD_STATE_MAGIC ||
      stored.schema != 1U || stored.bytes != sizeof(stored) ||
      stored.active != 1U || stored.operationId == 0U ||
      stored.packId == 0U || stored.guaranteed > 1U ||
      stored.source >=
          static_cast<uint8_t>(kitsu868::signal::MeshOperationKind::Count) ||
      stored.rarity >=
          static_cast<uint8_t>(kitsu868::signal::Rarity::Count) ||
      stored.codeOutcome >
          static_cast<uint8_t>(kitsu868::signal::CodeOutcome::Revealed) ||
      stored.reserved[0] != 0U || stored.reserved[1] != 0U ||
      stored.reserved[2] != 0U ||
      stored.crc32 != pendingWildStateCrc(stored)) {
    Serial.println("KITSU_WARN pending_wild=invalid");
    return;
  }
  const kitsu868::signal::CoordinatorState signalState =
      signalEncounterCoordinator.snapshot();
  if (!signalEncounterStateReady) {
    // An unreadable coordinator cannot prove whether the prepared reward was
    // committed. Preserve the journal so recovery never destroys entitlement.
    Serial.println("KITSU_WARN pending_wild=signal_unavailable");
    return;
  }
  if (signalState.lastOperationId != stored.operationId) {
    // A prepared record whose signal operation never committed is not an
    // encounter. It is safe to discard and the operation may be retried.
    if (!clearPendingWildEncounter()) {
      Serial.println("KITSU_WARN pending_wild=stale_remove_failed");
    }
    return;
  }
  kitsu868::wild::Creature creature{};
  const kitsu868::signal::Rarity rarity =
      static_cast<kitsu868::signal::Rarity>(stored.rarity);
  if (!kitsu868::wild::creatureForRarity(rarity, stored.entropy, creature) ||
      creature.packId != stored.packId) {
    Serial.println("KITSU_WARN pending_wild=catalog_mismatch");
    return;
  }
  pendingWildOperationId = stored.operationId;
  pendingWildEntropy = stored.entropy;
  pendingWildCodeOutcome =
      static_cast<kitsu868::signal::CodeOutcome>(stored.codeOutcome);
  wildEncounterView.available = true;
  wildEncounterView.guaranteed = stored.guaranteed != 0U;
  wildEncounterView.creature = creature;
  wildEncounterView.source =
      static_cast<kitsu868::signal::MeshOperationKind>(stored.source);
  pendingWildMaterialized = materializePendingWildEncounter();
}

bool persistEncounterCodes() {
  if (!storageReady || !encounterCodesReady ||
      encounterCodes.serializedBytes() > sizeof(encounterCodeScratch)) {
    return false;
  }
  size_t bytes = 0U;
  memset(encounterCodeScratch, 0, sizeof(encounterCodeScratch));
  const bool encoded = encounterCodes.serialize(
      encounterCodeScratch, sizeof(encounterCodeScratch), bytes);
  const bool stored = encoded && bytes != 0U &&
      preferences.putBytes(UNLOCK_CODES_STORAGE_KEY, encounterCodeScratch,
                           bytes) == bytes;
  memset(encounterCodeScratch, 0, sizeof(encounterCodeScratch));
  return stored;
}

void loadEncounterCodes() {
  encounterCodes.reset();
  encounterCodesReady = storageReady &&
      encounterCodes.serializedBytes() <= sizeof(encounterCodeScratch);
  if (!encounterCodesReady) return;
  const char* storageKey = UNLOCK_CODES_STORAGE_KEY;
  size_t bytes = preferences.getBytesLength(storageKey);
  bool loadedLegacyKey = false;
  if (bytes == 0U) {
    storageKey = LEGACY_UNLOCK_CODES_STORAGE_KEY;
    bytes = preferences.getBytesLength(storageKey);
    loadedLegacyKey = bytes != 0U;
  }
  if (bytes == 0U) return;
  if (bytes > sizeof(encounterCodeScratch) ||
      preferences.getBytes(storageKey, encounterCodeScratch,
                           sizeof(encounterCodeScratch)) != bytes ||
      !encounterCodes.load(encounterCodeScratch, bytes)) {
    encounterCodesReady = false;
    Serial.println("KITSU_WARN unlock_codes=invalid");
  }
  memset(encounterCodeScratch, 0, sizeof(encounterCodeScratch));
  if (encounterCodesReady &&
      (loadedLegacyKey || encounterCodes.migrationRequired())) {
    if (persistEncounterCodes()) {
      Serial.println("KITSU_INFO unlock_codes=schema_2");
    } else {
      // The validated v1 ledger remains usable in RAM and untouched under its
      // old key; migration can be retried safely on the next boot.
      Serial.println("KITSU_WARN unlock_codes=migration_pending");
    }
  }
}

bool validCoreState(const CoreStateV2& state) {
  if (state.magic != CORE_STATE_MAGIC || state.bytes != sizeof(CoreStateV2) ||
      state.reserved != 0 || state.uid[0] != 'K' || state.uid[1] != 'T' ||
      state.uid[6] != '\0' || state.energy > 100 || state.curiosity > 100 ||
      state.affection > 100 || state.sleeping > 1 ||
      (state.collectedGifts & 0xf000U) != 0 ||
      (state.lastTrait != 0xff &&
       state.lastTrait >= kitsu868::encounter::kTraitCount) ||
      (state.lastGift != 0xff &&
       state.lastGift >= kitsu868::encounter::kGiftCount)) {
    return false;
  }
  for (uint8_t index = 2; index < 6; ++index) {
    const char value = state.uid[index];
    if (!((value >= '0' && value <= '9') || (value >= 'A' && value <= 'F'))) {
      return false;
    }
  }
  return state.crc32 == coreStateCrc(state);
}

void applyCoreState(const CoreStateV2& state) {
  wisp.uid = state.uid;
  wisp.energy = state.energy;
  wisp.curiosity = state.curiosity;
  wisp.affection = state.affection;
  wisp.sleeping = state.sleeping != 0;
  wisp.pets = state.pets;
  collectiblePackId = state.collectiblePackId;
  unlockedTraits = state.unlockedTraits;
  collectedGifts = state.collectedGifts;
  lastEncounterTrait = state.lastTrait;
  lastEncounterGift = state.lastGift;
}

bool saveState() {
  if (!storageReady) return false;
  CoreStateV2 state{};
  state.bytes = sizeof(CoreStateV2);
  memset(state.uid, 0, sizeof(state.uid));
  const size_t uidBytes = min<size_t>(6, wisp.uid.length());
  memcpy(state.uid, wisp.uid.c_str(), uidBytes);
  state.energy = wisp.energy;
  state.curiosity = wisp.curiosity;
  state.affection = wisp.affection;
  state.sleeping = wisp.sleeping ? 1 : 0;
  state.lastTrait = lastEncounterTrait;
  state.lastGift = lastEncounterGift;
  state.pets = wisp.pets;
  state.collectiblePackId = collectiblePackId;
  state.unlockedTraits = unlockedTraits;
  state.collectedGifts = collectedGifts;
  state.crc32 = coreStateCrc(state);
  const bool written =
      preferences.putBytes("core_v2", &state, sizeof(state)) == sizeof(state);
  CoreStateV2 verification{};
  const bool readBack = written &&
      preferences.getBytesLength("core_v2") == sizeof(verification) &&
      preferences.getBytes("core_v2", &verification, sizeof(verification)) ==
          sizeof(verification) &&
      validCoreState(verification) &&
      memcmp(&verification, &state, sizeof(state)) == 0;
  if (!readBack) {
    Serial.println("KITSU_WARN core_flush=false");
  }
  return readBack;
}

void loadNearbySequenceCursor() {
  nearbySequenceCursor = 1U;
  nearbySequenceReady = false;
  if (!storageReady) return;
  if (!preferences.isKey("near_seq")) {
    nearbySequenceReady =
        preferences.putUShort("near_seq", nearbySequenceCursor) ==
            sizeof(nearbySequenceCursor) &&
        preferences.getUShort("near_seq", 0U) == nearbySequenceCursor;
  } else if (preferences.getType("near_seq") == PT_U16) {
    nearbySequenceCursor = preferences.getUShort("near_seq", 0U);
    nearbySequenceReady = nearbySequenceCursor != 0U;
  }
  if (!nearbySequenceReady) {
    nearbySequenceCursor = 1U;
    Serial.println("KITSU_WARN nearby_sequence=unavailable");
  }
}

bool reserveNearbySequence(uint16_t sequence) {
  if (!nearbySequenceReady || sequence == 0U ||
      sequence != nearbySequenceCursor) {
    return false;
  }
  const uint16_t next =
      sequence == UINT16_MAX ? 1U : static_cast<uint16_t>(sequence + 1U);
  if (preferences.putUShort("near_seq", next) != sizeof(next) ||
      preferences.getUShort("near_seq", 0U) != next) {
    nearbySequenceReady = false;
    Serial.println("KITSU_WARN nearby_sequence=flush_failed");
    return false;
  }
  nearbySequenceCursor = next;
  for (NearbyNeighbor& neighbor : nearbyNeighbors) {
    if (neighbor.used) neighbor.nextSequence = next;
  }
  return true;
}

void persistProgress() {
  saveState();
  const uint32_t now = millis();
  const kitsu868::connectivity::StorageRetryStatus retry =
      brainStorageRetry.status();
  if (retry.dirty && !brainStorageRetry.attemptDue(now)) {
    brainMinutesSinceFlush = 15U;
    return;
  }
  brainStorageRetry.recordAttempt(now);
  if (companionBrain.flush()) {
    brainStorageRetry.recordSuccess();
    brainMinutesSinceFlush = 0U;
  } else {
    brainStorageRetry.markDirty(now, 0U);
    brainStorageRetry.recordFailure(
        now, kitsu868::connectivity::StorageRetryFailure::Transient);
    brainMinutesSinceFlush = 15U;
    Serial.printf("KITSU_WARN brain_flush=false retry=%u\n",
                  static_cast<unsigned>(
                      brainStorageRetry.status().failureCount));
  }
}

void loadState() {
  storageReady = preferences.begin("wisp868", false);
  wisp.uid = deviceUid();
  if (!storageReady) return;

  CoreStateV2 coreState{};
  const bool loadedCore =
      preferences.getBytesLength("core_v2") == sizeof(coreState) &&
      preferences.getBytes("core_v2", &coreState, sizeof(coreState)) ==
          sizeof(coreState) &&
      validCoreState(coreState);
  if (loadedCore) {
    firstHatch = false;
    applyCoreState(coreState);
  } else {
    // One-time migration from the v0.6 per-key layout.
    firstHatch = preferences.getUInt("magic", 0) != LEGACY_STATE_MAGIC;
  }
  if (!loadedCore && !firstHatch) {
    const String storedUid = preferences.getString("uid", "");
    if (storedUid.length() == 6 && storedUid.startsWith("KT")) wisp.uid = storedUid;
    wisp.energy = min<uint8_t>(100, preferences.getUChar("energy", 72));
    wisp.curiosity = min<uint8_t>(100, preferences.getUChar("curious", 14));
    wisp.affection = min<uint8_t>(100, preferences.getUChar("affection", 5));
    wisp.pets = preferences.getUInt("pets", 0);
    wisp.sleeping = preferences.getBool("sleeping", false);
    unlockedTraits = preferences.getUShort("traits", 0);
    collectedGifts = preferences.getUShort("gifts", 0);
    lastEncounterTrait = preferences.getUChar("last_tr", 0xff);
    lastEncounterGift = preferences.getUChar("last_gf", 0xff);
    collectiblePackId = preferences.getUInt("col_pid", 0);
    collectedGifts &= 0x0fffU;
    if (lastEncounterTrait >= kitsu868::encounter::kTraitCount) {
      lastEncounterTrait = 0xff;
    }
    if (lastEncounterGift >= kitsu868::encounter::kGiftCount) {
      lastEncounterGift = 0xff;
    }
  }

  if (!loadedCore) saveState();

  wisp.boots = preferences.getUInt("boots", 0) + 1;
  preferences.putUInt("boots", wisp.boots);
  companion_api::loadActionReplay();
  loadClockState();
  loadSignalEncounterState();
  loadEncounterCodes();
  loadFunState();
  loadDialogueState();
  loadExpeditionState();
  loadPartyRewardState();
  loadAdventureProgression();
  loadSocialProgression();
  loadActivityState();
  loadFocusState();
  loadPendingWildEncounter();
}

// The OLED remains a 128x64 framebuffer. Every UI pixel is mapped into a logical
// 64x128 canvas, upright when the board is turned counter-clockwise (USB-C right).
void uiPixel(int16_t x, int16_t y) {
  if (x < 0 || x >= UI_WIDTH || y < 0 || y >= UI_HEIGHT) return;
  display.setPixel(UI_HEIGHT - 1 - y, x);
}

void uiFillRect(int16_t x, int16_t y, int16_t width, int16_t height) {
  for (int16_t py = 0; py < height; ++py) {
    for (int16_t px = 0; px < width; ++px) uiPixel(x + px, y + py);
  }
}

void uiRect(int16_t x, int16_t y, int16_t width, int16_t height) {
  for (int16_t px = 0; px < width; ++px) {
    uiPixel(x + px, y);
    uiPixel(x + px, y + height - 1);
  }
  for (int16_t py = 1; py < height - 1; ++py) {
    uiPixel(x, y + py);
    uiPixel(x + width - 1, y + py);
  }
}

void uiGlyph(char character, int16_t x, int16_t y, uint8_t scale) {
  if (character < KITSU_FONT_FIRST || character > KITSU_FONT_LAST) character = '?';
  const uint8_t index = static_cast<uint8_t>(character) - KITSU_FONT_FIRST;
  for (uint8_t column = 0; column < 5; ++column) {
    const uint8_t pixels = pgm_read_byte(&KITSU_FONT_5X7[index][column]);
    for (uint8_t row = 0; row < 7; ++row) {
      if ((pixels & (1U << row)) == 0) continue;
      uiFillRect(x + column * scale, y + row * scale, scale, scale);
    }
  }
}

// All OLED labels flow through this function.  It sanitizes UTF-8 to the
// display's compact ASCII font, selects readable scale/tracking, and adds a
// visible ellipsis when even compact scale 1 cannot contain the full label.
// No caller can accidentally center a negative-width/off-screen string.
kitsu868::portrait::TextPlan uiTextCentered(
    const String& rawText, int16_t y, uint8_t preferredScale = 1,
    int16_t maximumWidth = kitsu868::portrait::kContentWidth) {
  String text = oledSafeText(rawText.c_str());
  text.toUpperCase();
  const kitsu868::portrait::TextPlan plan = kitsu868::portrait::planText(
      text.length(), maximumWidth, preferredScale);
  if (!plan.valid ||
      !kitsu868::portrait::centeredTextFitsVertically(y, plan)) {
    return plan;
  }

  if (plan.ellipsized) {
    text.remove(plan.sourceCharacters);
    while (text.length() < plan.renderedCharacters) text += '.';
  }
  int16_t x = (UI_WIDTH - plan.width) / 2;
  for (size_t index = 0; index < text.length(); ++index) {
    uiGlyph(text.charAt(index), x, y, plan.scale);
    x += plan.advance;
  }
  return plan;
}

kitsu868::portrait::TextPlan uiTextCenteredFit(
    const String& text, int16_t y, uint8_t preferredScale = 2) {
  return uiTextCentered(text, y, preferredScale);
}

void uiXbm(int16_t x, int16_t y, int16_t width, int16_t height,
           const uint8_t* bitmap) {
  const int16_t bytesPerRow = (width + 7) / 8;
  for (int16_t py = 0; py < height; ++py) {
    for (int16_t px = 0; px < width; ++px) {
      const uint8_t value = bitmap[py * bytesPerRow + px / 8];
      if (value & (1U << (px & 7))) uiPixel(x + px, y + py);
    }
  }
}

void uiEnergyBar(uint8_t value, int16_t y = 110) {
  constexpr int16_t innerWidth = 32;
  uiRect(15, y, innerWidth + 2, 5);
  uiFillRect(16, y + 1, static_cast<int16_t>(innerWidth * value / 100U), 3);
}

void uiProgressBar(uint8_t value, int16_t y, int16_t innerWidth = 42) {
  if (value > 100) value = 100;
  const int16_t x = (UI_WIDTH - innerWidth - 2) / 2;
  uiRect(x, y, innerWidth + 2, 5);
  uiFillRect(x + 1, y + 1,
             static_cast<int16_t>(innerWidth * value / 100U), 3);
}

void uiMenuDots(uint8_t selected, uint8_t count, int16_t y) {
  const kitsu868::portrait::DotPlan plan = kitsu868::portrait::planDots(
      count, kitsu868::portrait::kContentWidth);
  if (!plan.valid || !kitsu868::portrait::rectangleFits(
                         (UI_WIDTH - plan.width) / 2, y, plan.width,
                         plan.size)) {
    return;
  }
  int16_t x = (UI_WIDTH - plan.width) / 2;
  for (uint8_t index = 0; index < count; ++index) {
    if (index == selected) uiFillRect(x, y, plan.size, plan.size);
    else uiRect(x, y, plan.size, plan.size);
    x += plan.size + plan.gap;
  }
}

void uiMailBadge() {
  if (unreadChatMessages == 0U) return;
  uiRect(52, 3, 10, 7);
  uiPixel(53, 4);
  uiPixel(54, 5);
  uiPixel(55, 6);
  uiPixel(56, 7);
  uiPixel(57, 7);
  uiPixel(58, 6);
  uiPixel(59, 5);
  uiPixel(60, 4);
  uiFillRect(58, 12, 4, 4);
}

char bleIndicator(uint32_t now) {
  if (!companionBle.ready()) return '-';
  const kitsu868::connectivity::BleLinkStatus link =
      companionBle.linkStatus(now);
  if (link.connected) return '+';
  return '!';
}

void uiBluetoothIcon(int16_t x, int16_t y) {
  // A compact Bluetooth rune drawn directly into the portrait framebuffer.
  // Keeping this out of the 5x7 text font avoids unsupported-glyph fallbacks.
  static constexpr uint8_t ROWS[] = {
      0b0001000, 0b0001100, 0b0001010, 0b1001001,
      0b0101010, 0b0011100, 0b0101010, 0b1001001,
      0b0001010, 0b0001100, 0b0001000,
  };
  for (uint8_t row = 0; row < sizeof(ROWS); ++row) {
    for (uint8_t column = 0; column < 7U; ++column) {
      if ((ROWS[row] & (1U << (6U - column))) != 0U) {
        uiPixel(x + column, y + row);
      }
    }
  }
}

void uiConnectionIndicators(int16_t y = 2) {
  const uint32_t now = millis();
  uiBluetoothIcon(2, y);
  uiGlyph(bleIndicator(now), 11, y + 2, 1);
}

const char* bluetoothStatusLabel(uint32_t now) {
  if (!companionBle.ready()) return "UNAVAILABLE";
  const kitsu868::connectivity::BleSessionStatus session =
      companionBle.sessionStatus(now);
  const kitsu868::connectivity::BleLinkStatus link =
      companionBle.linkStatus(now);
  if (session.applicationAuthenticated) return "CONNECTED";
  if (link.connected) return "LINKING";
  if (link.pairingWindowOpen) return "PAIRING";
  return link.advertising ? "READY" : "OFF";
}

const char* connectionActionLabel(ConnectionAction action) {
  switch (action) {
    case ConnectionAction::Bluetooth: return "BLUETOOTH";
    case ConnectionAction::PairCaretaker: return "CARETAKER";
    case ConnectionAction::Controllers: return "CONTROLLERS";
    case ConnectionAction::Back: return "BACK";
  }
  return "BACK";
}

const char* connectionActionPrompt(ConnectionAction action, uint32_t now) {
  if (action == ConnectionAction::Bluetooth) {
    return companionBle.linkStatus(now).connected ? "HOLD VIEW" : "HOLD OPEN";
  }
  if (action == ConnectionAction::PairCaretaker) return "HOLD PAIR";
  if (action == ConnectionAction::Controllers) return "HOLD MANAGE";
  return "HOLD BACK";
}

bool controllerRecoveryDeadlineReached(uint32_t now, uint32_t deadline) {
  return deadline != 0U && static_cast<int32_t>(now - deadline) >= 0;
}

bool controllerRecoveryRequiresReboot() {
  return controllerRecoveryResult ==
             ControllerRecoveryResult::StorageNeedsReboot ||
      controllerRecoveryResult ==
             ControllerRecoveryResult::BleBondStoreError ||
      controllerRecoveryResult ==
             ControllerRecoveryResult::ControllerAuthorityChanged;
}

uint32_t controllerRecoverySecondsRemaining(uint32_t now,
                                            uint32_t deadline) {
  if (controllerRecoveryDeadlineReached(now, deadline)) return 0U;
  return (deadline - now + 999U) / 1000U;
}

bool controllerRecoveryBleDisconnected(uint32_t now) {
  return !companionBle.ready() ||
      (companionBle.localControllerRecoveryLocked() &&
       !companionBle.linkStatus(now).connected);
}

String controllerFingerprint(
    const uint8_t id[kitsu868::connectivity::kKitsuControllerIdBytes]) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  constexpr size_t fingerprintBytes[] = {0U, 1U, 14U, 15U};
  char output[9]{};
  size_t cursor = 0U;
  for (size_t index : fingerprintBytes) {
    output[cursor++] = HEX_DIGITS[id[index] >> 4U];
    output[cursor++] = HEX_DIGITS[id[index] & 0x0fU];
  }
  return String(output);
}

void clearControllerRecoveryTarget() {
  memset(controllerRecoveryTargetId, 0, sizeof(controllerRecoveryTargetId));
  kitsu868::connectivity::clearControllerAuthoritySnapshot(
      controllerRecoveryAuthoritySnapshot);
  controllerRecoveryAuthoritySnapshotValid = false;
  controllerRecoveryTargetSlot = 0U;
  controllerRecoveryOriginalCount = 0U;
}

void leaveControllerRecovery() {
  clearControllerRecoveryTarget();
  controllerRecoveryDeadline = 0U;
  controllerRecoveryResult = ControllerRecoveryResult::None;
  if (!companionBle.endLocalControllerRecovery()) {
    Serial.println("KITSU_WARN controller_recovery_ble_unlock=false");
  }
  enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
}

void showControllerManager(uint32_t now, bool resetSelection) {
  clearControllerRecoveryTarget();
  controllerRecoveryResult = ControllerRecoveryResult::None;
  if (resetSelection) controllerRecoverySelection = 0U;
  controllerRecoveryDeadline = now + CONTROLLER_RECOVERY_BROWSE_TIMEOUT_MS;
  enterScreen(Screen::ControllerManager);
}

void beginControllerRecovery(uint32_t now) {
  // Physical recovery and pairing never share a live authorization moment.
  companionBle.disconnectForLocalControllerRecovery();
  showControllerManager(now, true);
}

bool controllerIdPresent(
    const uint8_t id[kitsu868::connectivity::kKitsuControllerIdBytes]) {
  uint8_t current[kitsu868::connectivity::kKitsuControllerIdBytes]{};
  bool present = false;
  for (size_t slot = 0U;
       slot < kitsu868::connectivity::kKitsuControllerCapacity; ++slot) {
    if (deviceSecurity.controllerAtSlot(slot, current) &&
        memcmp(current, id, sizeof(current)) == 0) {
      present = true;
      break;
    }
  }
  memset(current, 0, sizeof(current));
  return present;
}

bool armControllerRecovery(uint32_t now, uint8_t selection) {
  if (!connectivitySecurityReady ||
      !controllerRecoveryBleDisconnected(now)) {
    return false;
  }
  clearControllerRecoveryTarget();
  controllerRecoveryOriginalCount = deviceSecurity.status().controllerCount;
  if (selection < kitsu868::connectivity::kKitsuControllerCapacity) {
    if (!deviceSecurity.controllerAtSlot(selection,
                                         controllerRecoveryTargetId)) {
      clearControllerRecoveryTarget();
      return false;
    }
    controllerRecoveryTargetSlot = selection;
  } else if (selection == CONTROLLER_RECOVERY_RESET_INDEX &&
             controllerRecoveryOriginalCount != 0U) {
    controllerRecoveryTargetSlot = CONTROLLER_RECOVERY_RESET_INDEX;
  } else if (selection == CONTROLLER_RECOVERY_BLE_BONDS_INDEX &&
             companionBle.bleBondCount() > 0) {
    controllerRecoveryTargetSlot = CONTROLLER_RECOVERY_BLE_BONDS_INDEX;
    controllerRecoveryAuthoritySnapshotValid =
        kitsu868::connectivity::captureControllerAuthorities(
            deviceSecurity, controllerRecoveryAuthoritySnapshot);
    if (!controllerRecoveryAuthoritySnapshotValid) {
      clearControllerRecoveryTarget();
      return false;
    }
  } else {
    clearControllerRecoveryTarget();
    return false;
  }
  controllerRecoveryResult = ControllerRecoveryResult::None;
  controllerRecoveryDeadline =
      now + CONTROLLER_RECOVERY_CONFIRM_TIMEOUT_MS;
  enterScreen(Screen::ControllerConfirm);
  return true;
}

void commitControllerRecovery(uint32_t now) {
  companionBle.disconnectForLocalControllerRecovery();
  if (!connectivitySecurityReady ||
      !controllerRecoveryBleDisconnected(now)) {
    showControllerManager(now, false);
    return;
  }

  const bool clearBleBonds =
      controllerRecoveryTargetSlot == CONTROLLER_RECOVERY_BLE_BONDS_INDEX;
  const bool resetAll =
      controllerRecoveryTargetSlot == CONTROLLER_RECOVERY_RESET_INDEX;
  if (clearBleBonds) {
    kitsu868::connectivity::ControllerAuthoritySnapshot beforeDelete{};
    const bool beforeStillValid = controllerRecoveryAuthoritySnapshotValid &&
        kitsu868::connectivity::captureControllerAuthorities(
            deviceSecurity, beforeDelete) &&
        kitsu868::connectivity::controllerAuthoritiesUnchanged(
            controllerRecoveryAuthoritySnapshot, beforeDelete);
    kitsu868::connectivity::clearControllerAuthoritySnapshot(beforeDelete);
    if (!beforeStillValid) {
      kitsu868::connectivity::clearControllerAuthoritySnapshot(
          controllerRecoveryAuthoritySnapshot);
      controllerRecoveryAuthoritySnapshotValid = false;
      controllerRecoveryResult =
          ControllerRecoveryResult::ControllerAuthorityChanged;
      controllerRecoveryDeadline = 0U;
      Serial.println(
          "KITSU_BLE_BOND_CLEAR physical_confirmed=true live_link=false "
          "attempted=false controllers_unchanged=false outcome=blocked");
      enterScreen(Screen::ControllerResult);
      return;
    }

    kitsu868::connectivity::BleBondClearStatus bondStatus{};
    companionBle.clearBleBondsForLocalRecovery(bondStatus);
    kitsu868::connectivity::ControllerAuthoritySnapshot afterDelete{};
    const bool afterCaptured =
        kitsu868::connectivity::captureControllerAuthorities(
            deviceSecurity, afterDelete);
    const kitsu868::connectivity::BleBondRecoveryOutcome outcome =
        afterCaptured
            ? kitsu868::connectivity::evaluateBleBondRecovery(
                  bondStatus.deleteSucceeded, bondStatus.bondsBefore,
                  bondStatus.bondsAfter,
                  controllerRecoveryAuthoritySnapshot, afterDelete)
            : kitsu868::connectivity::BleBondRecoveryOutcome::InvalidSnapshot;
    const bool controllersUnchanged = afterCaptured &&
        kitsu868::connectivity::controllerAuthoritiesUnchanged(
            controllerRecoveryAuthoritySnapshot, afterDelete);
    kitsu868::connectivity::clearControllerAuthoritySnapshot(afterDelete);
    kitsu868::connectivity::clearControllerAuthoritySnapshot(
        controllerRecoveryAuthoritySnapshot);
    controllerRecoveryAuthoritySnapshotValid = false;
    switch (outcome) {
      case kitsu868::connectivity::BleBondRecoveryOutcome::Cleared:
        controllerRecoveryResult = ControllerRecoveryResult::BleBondsCleared;
        break;
      case kitsu868::connectivity::BleBondRecoveryOutcome::ControllerAuthorityChanged:
      case kitsu868::connectivity::BleBondRecoveryOutcome::InvalidSnapshot:
        controllerRecoveryResult =
            ControllerRecoveryResult::ControllerAuthorityChanged;
        break;
      case kitsu868::connectivity::BleBondRecoveryOutcome::BondStoreError:
        controllerRecoveryResult =
            ControllerRecoveryResult::BleBondStoreError;
        break;
    }
    Serial.printf(
        "KITSU_BLE_BOND_CLEAR physical_confirmed=true live_link=false "
        "attempted=%s delete_all=%s bonds_before=%d bonds_after=%d "
        "controllers_before=%u controllers_after=%u "
        "controllers_unchanged=%s outcome=%u\n",
        bondStatus.attempted ? "true" : "false",
        bondStatus.deleteSucceeded ? "true" : "false",
        bondStatus.bondsBefore, bondStatus.bondsAfter,
        static_cast<unsigned>(controllerRecoveryOriginalCount),
        static_cast<unsigned>(deviceSecurity.status().controllerCount),
        controllersUnchanged ? "true" : "false",
        static_cast<unsigned>(outcome));
    controllerRecoveryDeadline =
        controllerRecoveryResult ==
                ControllerRecoveryResult::BleBondsCleared
            ? now + CONTROLLER_RECOVERY_RESULT_TIMEOUT_MS
            : 0U;
    enterScreen(Screen::ControllerResult);
    return;
  }

  if (!resetAll) {
    uint8_t current[kitsu868::connectivity::kKitsuControllerIdBytes]{};
    const bool targetUnchanged = deviceSecurity.controllerAtSlot(
        controllerRecoveryTargetSlot, current) &&
        memcmp(current, controllerRecoveryTargetId, sizeof(current)) == 0;
    memset(current, 0, sizeof(current));
    if (!targetUnchanged) {
      controllerRecoveryResult = ControllerRecoveryResult::Unchanged;
      controllerRecoveryDeadline =
          now + CONTROLLER_RECOVERY_RESULT_TIMEOUT_MS;
      enterScreen(Screen::ControllerResult);
      return;
    }
  }

  const kitsu868::connectivity::SecurityResult result = resetAll
      ? deviceSecurity.revokeAllControllersAfterPhysicalConfirmation(true)
      : deviceSecurity.revokeControllerAfterPhysicalConfirmation(
            controllerRecoveryTargetId, true);
  const bool authorityRemoved = resetAll
      ? deviceSecurity.status().controllerCount == 0U
      : !controllerIdPresent(controllerRecoveryTargetId);
  if (result == kitsu868::connectivity::SecurityResult::Ok &&
      authorityRemoved) {
    controllerRecoveryResult = resetAll
        ? ControllerRecoveryResult::RemovedAll
        : ControllerRecoveryResult::RemovedSlot;
  } else if (result ==
                 kitsu868::connectivity::SecurityResult::StorageReadFailed ||
             result ==
                 kitsu868::connectivity::SecurityResult::StorageWriteFailed ||
             result ==
                 kitsu868::connectivity::SecurityResult::ReadbackFailed ||
             authorityRemoved) {
    controllerRecoveryResult = ControllerRecoveryResult::StorageNeedsReboot;
  } else {
    controllerRecoveryResult = ControllerRecoveryResult::Unchanged;
  }
  Serial.printf(
      "KITSU_CONTROLLER_RECOVERY action=%s slot=%u count_before=%u "
      "count_after=%u security=%s outcome=%u\n",
      resetAll ? "all" : "slot",
      resetAll ? 0U : static_cast<unsigned>(controllerRecoveryTargetSlot + 1U),
      static_cast<unsigned>(controllerRecoveryOriginalCount),
      static_cast<unsigned>(deviceSecurity.status().controllerCount),
      kitsu868::connectivity::securityResultName(result),
      static_cast<unsigned>(controllerRecoveryResult));
  controllerRecoveryDeadline =
      controllerRecoveryResult == ControllerRecoveryResult::StorageNeedsReboot
          ? 0U
          : now + CONTROLLER_RECOVERY_RESULT_TIMEOUT_MS;
  enterScreen(Screen::ControllerResult);
}

void uiWrappedText(const char* rawText, int16_t y, uint8_t maxLines,
                   uint8_t lineSpacing = 10U) {
  String remaining = oledSafeText(rawText);
  remaining.trim();
  const size_t charactersPerLine = kitsu868::portrait::lineCapacity(
      kitsu868::portrait::kContentWidth, 1,
      kitsu868::portrait::compactAdvance(1));
  for (uint8_t lineIndex = 0;
       lineIndex < maxLines && remaining.length() > 0U; ++lineIndex) {
    String line;
    if (remaining.length() <= charactersPerLine) {
      line = remaining;
      remaining = "";
    } else {
      int breakAt = remaining.lastIndexOf(
          ' ', static_cast<unsigned>(charactersPerLine));
      if (breakAt <= 0) breakAt = static_cast<int>(charactersPerLine);
      line = remaining.substring(0, breakAt);
      remaining = remaining.substring(
          static_cast<unsigned>(breakAt) +
          (remaining.charAt(breakAt) == ' ' ? 1U : 0U));
      remaining.trim();
    }
    if (lineIndex + 1U == maxLines && remaining.length() > 0U) {
      if (line.length() > charactersPerLine - 2U) {
        line.remove(charactersPerLine - 2U);
      }
      line += "..";
    }
    uiTextCentered(line, y + lineIndex * lineSpacing);
  }
}

uint8_t bitCount16(uint16_t value) {
  uint8_t count = 0;
  while (value) {
    count += value & 1U;
    value >>= 1U;
  }
  return count;
}

uint8_t batteryPercentFromMillivolts(uint16_t millivolts) {
  struct Point { uint16_t millivolts; uint8_t percent; };
  static constexpr Point curve[] = {
      {3300, 0}, {3500, 5}, {3600, 12}, {3700, 25}, {3800, 48},
      {3900, 68}, {4000, 84}, {4100, 95}, {4200, 100}};
  if (millivolts <= curve[0].millivolts) return curve[0].percent;
  for (size_t index = 1; index < sizeof(curve) / sizeof(curve[0]); ++index) {
    if (millivolts > curve[index].millivolts) continue;
    const Point& low = curve[index - 1];
    const Point& high = curve[index];
    return static_cast<uint8_t>(low.percent +
        (static_cast<uint32_t>(millivolts - low.millivolts) *
         (high.percent - low.percent)) /
            (high.millivolts - low.millivolts));
  }
  return 100;
}

void sampleBattery(bool force = false) {
  const uint32_t now = millis();
  if (!force && now - lastBatterySampleAt < BATTERY_SAMPLE_MS) return;
  lastBatterySampleAt = now;

  digitalWrite(PIN_BATTERY_CTRL, LOW);
  delay(4);
  uint32_t adcMillivolts = 0;
  for (uint8_t sample = 0; sample < 8; ++sample) {
    adcMillivolts += analogReadMilliVolts(PIN_BATTERY_ADC);
  }
  digitalWrite(PIN_BATTERY_CTRL, HIGH);
  adcMillivolts /= 8U;

  // The V3.2 schematic uses a 390k/100k divider, exactly 4.9x.
  // analogReadMilliVolts() already applies the ESP32-S3 ADC calibration, so
  // no unrelated board-specific multiplier is layered on top.  Values outside
  // a plausible 1-cell LiPo range mean no battery is attached, common while
  // developing over USB.
  const uint32_t measured = (adcMillivolts * 4900U + 500U) / 1000U;
  battery.millivolts = measured > 0xffffU
                           ? 0xffffU
                           : static_cast<uint16_t>(measured);
  battery.present = battery.millivolts >= 2800U && battery.millivolts <= 4600U;
  battery.percent = battery.present
                        ? batteryPercentFromMillivolts(battery.millivolts)
                        : 0xff;
}

void wakeDisplay() {
  lastInteractionAt = millis();
  if (!oledDetected || !displaySleeping) return;
  display.displayOn();
  displaySleeping = false;
  lastRenderAt = 0;
}

void sleepDisplay() {
  if (!oledDetected || displaySleeping || radioListening ||
      screen == Screen::Menu || screen == Screen::Connect ||
      screen == Screen::GameMenu ||
      (screen == Screen::Game && !gameRewarded) || screen == Screen::Status ||
      screen == Screen::Clock ||
      (screen == Screen::Activity && activityRuntimeBusy()) ||
      screen == Screen::WildEncounter ||
      screen == Screen::PairPhone || screen == Screen::ControllerManager ||
      screen == Screen::ControllerConfirm ||
      screen == Screen::ControllerResult) {
    return;
  }
  display.displayOff();
  displaySleeping = true;
  Serial.println("KITSU_POWER display=off");
}

uint32_t nextAnimationToken() {
  ++animationToken;
  if (!animationToken) ++animationToken;
  return animationToken;
}

void scheduleNextAmbientAnimation() {
  nextAmbientAnimationAt = millis() + 5000UL + (esp_random() % 6000UL);
}

CompanionRole baseAnimationRole() {
  if (wisp.sleeping) return CompanionRole::Sleep;
  if (radioListening) return CompanionRole::Listen;
  if (wisp.energy < 25) return CompanionRole::Tired;
  return CompanionRole::Idle;
}

bool activateAnimation(CompanionRole role, bool finite, uint8_t playbackMask,
                       bool allowIdleFallback) {
  CompanionClipActivation clip{};
  const uint32_t token = nextAnimationToken();
  if (!companionPack.activateClip(role, token, clip,
                                  companionBrain.appearanceVariant(),
                                  playbackMask, allowIdleFallback)) {
    return false;
  }

  activeAnimation.active = true;
  activeAnimation.finite = finite;
  activeAnimation.role = role;
  activeAnimation.clipRole = clip.clipRole;
  activeAnimation.mode = clip.mode;
  activeAnimation.token = token;
  activeAnimation.startedAt = millis();
  activeAnimation.spanMs = clip.mode == PackPlayback::PingPong
                               ? clip.cycleDurationMs
                               : clip.forwardDurationMs;
  return true;
}

bool startBaseAnimation() {
  if (!companionPack.valid()) {
    activeAnimation.active = false;
    return false;
  }

  const CompanionRole role = baseAnimationRole();
  if (role == CompanionRole::Sleep) {
    // The rich pack uses one ONCE sleep transition. Keeping that activation
    // persistent makes the player hold its final sleeping frame afterward.
    if (activateAnimation(role, false, KITSU_PLAYBACK_ONCE, false)) return true;
  } else {
    constexpr uint8_t steadyModes = KITSU_PLAYBACK_HOLD |
        KITSU_PLAYBACK_LOOP | KITSU_PLAYBACK_PINGPONG;
    if (activateAnimation(role, false, steadyModes, false)) return true;
  }
  if (activateAnimation(role, false, KITSU_PLAYBACK_ANY, false)) return true;
  return activateAnimation(role, false, KITSU_PLAYBACK_ANY, true);
}

bool startTransientAnimation(CompanionRole role) {
  if (!companionPack.valid()) return false;
  scheduleNextAmbientAnimation();
  return activateAnimation(role, true, KITSU_PLAYBACK_ANY, false);
}

void tickAnimation() {
  if (!companionPack.valid()) {
    activeAnimation.active = false;
    return;
  }

  const uint32_t now = millis();
  if (activeAnimation.active && activeAnimation.finite &&
      now - activeAnimation.startedAt >= activeAnimation.spanMs) {
    const CompanionRole completedRole = activeAnimation.role;
    activeAnimation.active = false;
    startBaseAnimation();
    scheduleNextAmbientAnimation();
    if (completedRole == CompanionRole::Meet && wisp.sleeping &&
        screen == Screen::Pet) {
      screen = Screen::Sleep;
      screenEnteredAt = now;
    }
  }

  const CompanionRole baseRole = baseAnimationRole();
  if (!activeAnimation.active ||
      (!activeAnimation.finite && activeAnimation.role != baseRole)) {
    startBaseAnimation();
  }

  if (!activeAnimation.active || activeAnimation.finite ||
      baseRole != CompanionRole::Idle ||
      static_cast<int32_t>(now - nextAmbientAnimationAt) < 0) {
    return;
  }

  const CompanionRole ambientRole = (esp_random() % 5U) == 0
                                        ? CompanionRole::Play
                                        : CompanionRole::Blink;
  if (!startTransientAnimation(ambientRole)) scheduleNextAmbientAnimation();
}

bool drawCreatureSprite(int16_t legacyY = 24, int16_t highResolutionY = 16) {
  if (!activeAnimation.active) startBaseAnimation();
  if (!activeAnimation.active) {
    lastRenderedPetFrame = nullptr;
    lastRenderedPetFrameBytes = 0U;
    lastRenderedPetAnimationToken = 0U;
    return false;
  }
  const uint8_t* sprite = companionPack.activeFrame(
      millis() - activeAnimation.startedAt);
  if (!sprite) {
    lastRenderedPetFrame = nullptr;
    lastRenderedPetFrameBytes = 0U;
    lastRenderedPetAnimationToken = 0U;
    return false;
  }
  lastRenderedPetFrame = sprite;
  lastRenderedPetFrameBytes = companionPack.frameBytes();
  lastRenderedPetFrameAt = millis();
  lastRenderedPetFrameSurface = screen;
  lastRenderedPetAnimationToken = activeAnimation.token;
  const int16_t y = companionPack.formatVersion() == KITSU_PACK_V2
                        ? highResolutionY
                        : legacyY;
  uiXbm((UI_WIDTH - companionPack.frameWidth()) / 2, y,
        companionPack.frameWidth(), companionPack.frameHeight(), sprite);

  // Evolution remains pack-owned: appearance variants are selected above.
  // A few core sparkles make progression visible even when an older pack has
  // only variant zero, without introducing fallback creature artwork.
  const kitsu868::EvolutionStage stage = companionBrain.evolutionStage();
  if (stage >= kitsu868::EvolutionStage::Familiar) {
    const uint8_t phase = static_cast<uint8_t>((millis() / 350U) & 3U);
    uiPixel(static_cast<int16_t>(4 + phase * 3), y + 9 + phase * 8);
    uiPixel(static_cast<int16_t>(58 - phase * 2), y + 16 + phase * 7);
    if (stage >= kitsu868::EvolutionStage::Resonant) {
      uiPixel(3 + phase * 2, y + 48 - phase * 5);
      uiPixel(60 - phase * 3, y + 51 - phase * 6);
    }
  }
  if (lastEncounterTrait != 0xff) {
    const uint8_t orbit = static_cast<uint8_t>(
        ((millis() / 250U) + lastEncounterTrait) & 7U);
    const int16_t orbitX[8] = {7, 12, 24, 39, 52, 57, 45, 20};
    const int16_t orbitY[8] = {34, 16, 7, 7, 17, 35, 54, 55};
    uiPixel(orbitX[orbit], y + orbitY[orbit]);
    if (lastEncounterGift != 0xff &&
        (collectedGifts & (1U << lastEncounterGift))) {
      uiPixel(orbitX[orbit] + (orbitX[orbit] < 32 ? 1 : -1),
              y + orbitY[orbit]);
    }
  }
  if (sessionAuraActive ||
      (companionBrain.unlockMask() & kitsu868::UnlockAura) != 0U) {
    const uint8_t pulse = static_cast<uint8_t>((millis() / 180U) & 7U);
    uiPixel(2 + pulse * 2, y + 3);
    uiPixel(61 - pulse * 2, y + 28);
    uiPixel(5 + pulse * 3, y + 62);
  }
  return true;
}

void renderMissingPack() {
  uiTextCentered("NO", 32, 2);
  uiTextCentered("PACK", 52, 2);
  uiTextCentered("INSTALL", 82, 1);
  uiTextCentered("USB", 97, 1);
}

void renderPet() {
  uiConnectionIndicators();
  if (!drawCreatureSprite(20, 16)) {
    renderMissingPack();
    return;
  }
  uiMailBadge();
  const bool highResolution = companionPack.formatVersion() == KITSU_PACK_V2;
  if (momentView.active) {
    String momentText = momentView.line1 ? momentView.line1 : "";
    if (momentView.line2 && momentView.line2[0] != '\0') {
      if (momentText.length() > 0U) momentText += ' ';
      momentText += momentView.line2;
    }
    uiWrappedText(momentText.c_str(), highResolution ? 97 : 92, 4U, 8U);
  } else {
    uiTextCentered(moodText(), highResolution ? 99 : 93);
    uiEnergyBar(wisp.energy, highResolution ? 113 : 110);
  }
}

void renderMenu() {
  uiTextCentered("MENU", 8);
  uiTextCentered(MENU_ITEMS[menuIndex], 39, 2);
  uiMenuDots(menuIndex, sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]), 72);
  uiTextCentered("TAP NEXT", 91);
  uiTextCentered("HOLD SELECT", 108);
}

void renderConnect() {
  const uint32_t now = millis();
  uiConnectionIndicators(2);
  uiTextCentered("CONNECT", 14);
  if (connectionAction == ConnectionAction::PairCaretaker) {
    uiTextCentered("PAIR", 28);
    uiTextCentered("CARETAKER", 39);
  } else {
    uiTextCentered(connectionActionLabel(connectionAction), 31, 2);
  }
  switch (connectionAction) {
    case ConnectionAction::Bluetooth:
      uiTextCentered(bluetoothStatusLabel(now), 54);
      break;
    case ConnectionAction::PairCaretaker:
      uiTextCentered(bluetoothStatusLabel(now), 54);
      break;
    case ConnectionAction::Controllers: {
      if (!connectivitySecurityReady) {
        uiTextCentered("UNAVAILABLE", 54);
      } else {
        const uint8_t count = deviceSecurity.status().controllerCount;
        uiTextCentered(String(count) + " STORED", 54);
      }
      break;
    }
    case ConnectionAction::Back:
      uiTextCentered("RETURN", 54);
      break;
  }
  uiTextCentered(connectionActionPrompt(connectionAction, now), 76);
  uiMenuDots(static_cast<uint8_t>(connectionAction), 4, 94);
  uiTextCentered("TAP NEXT", 108);
}

void renderInbox() {
  uiTextCentered("INBOX", 4);
  ChatJournalEntry* entry = chatJournalNewest(inboxSelection);
  if (!entry) {
    uiTextCentered("QUIET", 39, 2);
    uiTextCentered("NO MESSAGES", 68);
    uiTextCentered("HOLD BACK", 108);
    return;
  }

  String origin;
  if (entry->kind == kitsu868::mesh::MessageKind::Direct) {
    origin = entry->senderName[0] != '\0'
                 ? oledSafeText(entry->senderName)
                 : "DM " + shortHex(entry->contactPublicKey, 6U);
  } else {
    origin = entry->senderName[0] != '\0'
                 ? oledSafeText(entry->senderName)
                 : "CHANNEL " + String(entry->channelSlot);
  }
  uiTextCentered(origin, 15);
  uiWrappedText(entry->text, 29, 6);

  uiTextCentered(String(inboxSelection + 1U) + "/" +
                     String(chatJournalCount), 91);
  const char* stateLabel = entry->state == ChatJournalState::Unconfirmed
                               ? "NO ACK"
                               : chatStateName(entry->state);
  uiTextCentered(entry->inbound ? "RECEIVED" : stateLabel, 103);
  uiTextCentered("HOLD BACK", 116);
}

void renderGameMenu() {
  uiTextCentered("GAMES", 8);
  uiTextCenteredFit(GAME_ITEMS[gameMenuIndex], 39, 2);
  uiMenuDots(gameMenuIndex,
             sizeof(GAME_ITEMS) / sizeof(GAME_ITEMS[0]), 72);
  uiTextCentered("TAP NEXT", 91);
  uiTextCentered("HOLD SELECT", 108);
}

void renderGameScore(uint8_t round, uint8_t rounds, uint16_t score,
                     kitsu868::MiniGamePhase phase,
                     kitsu868::MiniGameResult result) {
  uiTextCentered(String(round) + "/" + String(rounds), 18);
  if (phase == kitsu868::MiniGamePhase::Result) {
    uiTextCentered(kitsu868::miniGameResultLabel(result), 34);
  } else if (phase == kitsu868::MiniGamePhase::Finished) {
    uiTextCentered("DONE", 34, 2);
  }
  uiTextCentered("SCORE " + String(score), 96);
  if (phase == kitsu868::MiniGamePhase::Finished) {
    uiTextCentered("TAP HOME", 113);
  } else if (phase == kitsu868::MiniGamePhase::Result) {
    uiTextCentered("WAIT", 113);
  } else {
    uiTextCentered("TAP", 113);
  }
}

void renderSignalGame() {
  const kitsu868::SignalCatchView view = signalCatchGame.view(millis());
  uiTextCentered("SIGNAL", 4);
  renderGameScore(view.round, view.totalRounds, view.score,
                  view.phase, view.result);
  if (view.phase == kitsu868::MiniGamePhase::Finished) return;

  uiFillRect(view.targetLeft, view.targetY - 2,
             view.targetRight - view.targetLeft + 1, 5);
  uiFillRect(view.markerX > 0 ? view.markerX - 1 : 0,
             view.markerY - 8, 3, 7);
  uiPixel(view.markerX, view.markerY - 10);
  uiRect(3, view.targetY + 6, 58, 3);
}

void renderPounceGame() {
  const kitsu868::PounceFetchView view = pounceFetchGame.view(millis());
  uiTextCentered("POUNCE", 4);
  renderGameScore(view.round, view.totalRounds, view.score,
                  view.phase, view.result);
  if (view.phase == kitsu868::MiniGamePhase::Finished) return;

  uiRect(view.catchLeft, view.catchY - 4,
         view.catchRight - view.catchLeft + 1, 9);
  uiFillRect(view.objectX > 1 ? view.objectX - 2 : 0,
             view.objectY - 2, 5, 5);
  uiRect(3, view.catchY + 7, 58, 3);
}

void renderEchoGame() {
  const kitsu868::EchoBeatView view = echoBeatGame.view(millis());
  uiTextCentered("ECHO", 4);
  uiTextCentered(kitsu868::echoBeatStageLabel(view.stage), 20);
  uiTextCentered(String(view.stage == kitsu868::EchoBeatStage::Presenting
                            ? view.presentedBeats
                            : view.replayedBeats) +
                     "/" + String(view.totalBeats),
                 34);
  if (view.cueOn) {
    uiFillRect(23, 49, 18, 18);
    uiRect(19, 45, 26, 26);
  } else {
    uiRect(25, 51, 14, 14);
  }
  if (view.stage == kitsu868::EchoBeatStage::Replay) {
    uiTextCentered("TAP THE BEAT", 76);
  } else if (view.phase == kitsu868::MiniGamePhase::Result) {
    uiTextCentered(kitsu868::miniGameResultLabel(view.result), 76);
  }
  uiTextCentered("SCORE " + String(view.score), 96);
  uiTextCentered(view.phase == kitsu868::MiniGamePhase::Finished
                     ? "TAP HOME"
                     : "HOLD QUIT",
                 113);
}

void renderGame() {
  if (activeGame == ActiveGame::SignalCatch) renderSignalGame();
  else if (activeGame == ActiveGame::PounceFetch) renderPounceGame();
  else if (activeGame == ActiveGame::EchoBeat) renderEchoGame();
  else renderGameMenu();
}

void renderListen() {
  if (!companionPack.valid()) {
    renderMissingPack();
    return;
  }
  const uint32_t now = millis();
  const int32_t remainingMs = static_cast<int32_t>(listenUntil - now);
  const uint32_t remaining = remainingMs > 0
                                 ? (static_cast<uint32_t>(remainingMs) + 999UL) / 1000UL
                                 : 0;
  const kitsu868::presence::GroupSummary nearby =
      petPresenceTracker.summary();
  String signalLabel = "SEARCHING";
  String detailLabel;
  kitsu868::presence::PeerSnapshot strongest{};
  if (nearby.strongestSignalAvailable &&
      petPresenceTracker.peer(nearby.strongestSignalUid, strongest)) {
    if (strongest.trend == kitsu868::presence::SignalTrend::Approaching) {
      signalLabel = "SIGNAL STRONGER";
    } else if (strongest.trend == kitsu868::presence::SignalTrend::Leaving) {
      signalLabel = "SIGNAL FADING";
    } else if (strongest.band == kitsu868::presence::SignalBand::Strong) {
      signalLabel = "SIGNAL STRONG";
    } else if (strongest.band == kitsu868::presence::SignalBand::Medium) {
      signalLabel = "SIGNAL MEDIUM";
    } else if (strongest.band == kitsu868::presence::SignalBand::Weak) {
      signalLabel = "SIGNAL WEAK";
    }
    if (nearby.groupActive) {
      detailLabel = String("GROUP ") + nearby.presentCount;
      if (nearby.familiarCount != 0U) {
        detailLabel += String(" F") + nearby.familiarCount;
      }
    } else if (strongest.familiar) {
      detailLabel = "FAMILIAR";
    }
  }
  if (detailLabel.length() != 0U) detailLabel += "  ";
  detailLabel += String(remaining) + "S";
  uiTextCenteredFit(signalLabel, 7, 2);
  if (!drawCreatureSprite(25, 17)) {
    renderMissingPack();
    return;
  }
  uiTextCenteredFit(detailLabel, 101, 2);
}

void renderSleep() {
  uiConnectionIndicators();
  if (!drawCreatureSprite(22, 16)) {
    renderMissingPack();
    return;
  }
  uiTextCentered("DREAMING", 99);
}

void renderStatus() {
  if (statusPage == 0) {
    uiTextCentered(companionName(), 6);
    uiTextCenteredFit("BOND " + String(companionBrain.bondLevel()), 25, 2);
    uiProgressBar(companionBrain.bondProgressPercent(), 49);
    uiTextCentered(kitsu868::CompanionBrain::stageLabel(
                       companionBrain.evolutionStage()), 68);
    uiTextCentered(kitsu868::CompanionBrain::personalityLabel(
                       companionBrain.personality().kind), 87);
    uiTextCentered("T" + String(bitCount16(unlockedTraits)) + " G" +
                       String(bitCount16(collectedGifts)), 104);
  } else if (statusPage == 1) {
    uiTextCenteredFit("VITALS", 6, 2);
    uiTextCentered("ENERGY " + String(wisp.energy), 33);
    uiTextCentered("XP " + String(companionBrain.bondXp()), 50);
    if (battery.present) {
      uiTextCentered("BAT " + String(battery.percent) + "%", 67);
      uiTextCentered(String(battery.millivolts) + " MV", 84);
    } else {
      uiTextCentered("BAT USB", 67);
      uiTextCentered("NO CELL", 84);
    }
    const uint32_t minutes = companionBrain.lifetime().poweredMinutes;
    const uint32_t hours = minutes / 60U;
    // Keep the largest possible 32-bit lifetime meaningful in portrait mode;
    // multi-million-day values still fit without losing the unit.
    uiTextCentered(minutes < 60U
                       ? String(minutes) + " MIN"
                       : hours < 10000U
                             ? String(hours) + " HOURS"
                             : String(minutes / 1440U) + " DAYS", 101);
  } else if (statusPage == 2) {
    uiTextCenteredFit("MEMORY", 6, 2);
    kitsu868::MemoryEntry memory;
    if (companionBrain.recentMemory(0, memory)) {
      const kitsu868::MemoryText text =
          kitsu868::CompanionBrain::memoryText(memory);
      uiTextCenteredFit(text.line1, 42, 2);
      uiTextCenteredFit(text.line2, 65, 2);
      uiTextCentered("#" + String(memory.sequence), 96);
      if (memory.event == kitsu868::BrainEvent::Game ||
          memory.event == kitsu868::BrainEvent::PerfectGame) {
        uiTextCentered("SCORE " + String(memory.detail), 111);
      }
    } else {
      uiTextCentered("QUIET", 53, 2);
    }
  } else if (statusPage == 3) {
    uiTextCenteredFit("REWARDS", 6, 2);
    uiTextCentered("DREAMS " + String(funDiscovery.completedDreams), 25);
    uint8_t recentDreamIndex = 0U;
    if (kitsu868::fun::recentDream(funDiscovery, 0U, recentDreamIndex)) {
      const kitsu868::fun::Dream recent = kitsu868::fun::selectDream(
          companionBrain.personality().kind,
          companionBrain.deviceFingerprint(),
          static_cast<uint16_t>(funDiscovery.completedDreams - 1U));
      if (recent.index == recentDreamIndex) {
        uiTextCenteredFit(recent.line1, 39, 1);
        uiTextCenteredFit(recent.line2, 50, 1);
      } else {
        uiTextCentered("DREAM #" + String(recentDreamIndex + 1U), 45);
      }
    } else {
      uiTextCentered("NO DREAMS", 45);
    }
    uiTextCentered("R" + String(funDiscovery.rareReactions) + " C" +
                       String(kitsu868::fun::seenCreatureCount(funDiscovery)),
                   65);
    uiTextCentered(sessionAuraActive ? "SESSION AURA" : "AURA LOCKED", 82);
    uiTextCentered((companionBrain.unlockMask() & kitsu868::UnlockFinalForm)
                       ? "FINAL FORM"
                       : "KEEP BONDING",
                   99);
  } else if (statusPage == 4) {
    uiTextCenteredFit(companionProgressionReady &&
                          companionProgression.nickname()[0] != '\0'
                          ? companionProgression.nickname()
                          : "PROGRESSION",
                      5, 2);
    const kitsu868::progression::DailyGoal goal =
        companionProgression.dailyGoal();
    uiTextCentered("STREAK " +
                       String(companionProgression.currentStreak()), 28);
    uiTextCentered("GOAL " + String(goal.progress) + "/" +
                       String(goal.target), 45);
    uiTextCentered("LORE " +
                       String(bitCount16(companionProgression.loreMask())) +
                       "/10", 62);
    const kitsu868::social::SocialState social = socialProgression.snapshot();
    uiTextCentered("FRIENDS " + String(social.peerCount), 79);
    uiTextCentered("P " + String(social.completedParties), 96);
    uiTextCentered("PB" + String(static_cast<unsigned long>(
                       partyRewards.state().partyBond)), 111);
  } else {
    uiTextCentered("KITSU", 5, 2);
    uiTextCentered(FIRMWARE_VERSION, 27);
    uiTextCentered(wisp.uid, 43);
    uiTextCentered(oledDetected ? "OLED OK" : "OLED ERR", 59);
    uiTextCentered(!meshSettings.enabled
                       ? "MESH OFF"
                       : radioReady ? "MESH OK" : "MESH ERR", 75);
    uiTextCentered(storageReady ? "STORE OK" : "STORE ERR", 91);
    uiTextCentered(companionPack.valid() ? "PACK OK" : "NO PACK", 107);
  }
  uiMenuDots(statusPage, 6, 121);
}

bool catalogCreatureOwned(const kitsu868::wild::Creature& creature) {
  if (companionPack.valid() && companionPack.id() == creature.packId) {
    return true;
  }
  kitsu868::unlocks::CodeRecord record{};
  return encounterCodesReady &&
      encounterCodes.findByPackId(creature.packId, record);
}

void drawGuidePortrait(const kitsu868::wild::Creature& creature,
                       int16_t top) {
  const uint8_t* bitmap = nullptr;
  size_t bitmapBytes = 0U;
  if (!kitsu868::wild::guidePortraitBitmap(creature.portrait, bitmap,
                                           bitmapBytes) ||
      bitmapBytes != kitsu868::wild::kGuidePortraitBytes) {
    return;
  }
  uiXbm((UI_WIDTH - kitsu868::wild::kGuidePortraitWidth) / 2, top,
        kitsu868::wild::kGuidePortraitWidth,
        kitsu868::wild::kGuidePortraitHeight, bitmap);
}

void drawUnknownCreatureSilhouette() {
  uiFillRect(27, 30, 10, 10);
  uiFillRect(22, 40, 20, 20);
  uiFillRect(18, 48, 28, 8);
}

void renderFieldGuide() {
  kitsu868::wild::Creature creature{};
  if (!kitsu868::wild::creatureAt(fieldGuideIndex, creature)) {
    uiTextCentered("FIELD GUIDE", 4);
    uiTextCentered("UNAVAILABLE", 50, 2);
    return;
  }
  const bool seen = funStateReady &&
      kitsu868::fun::creatureSeen(funDiscovery, fieldGuideIndex);
  if (!seen) {
    uiTextCentered("FIELD GUIDE", 2);
    uiTextCenteredFit("???", 14, 1);
    drawUnknownCreatureSilhouette();
    uiTextCentered("UNDISCOVERED", 66);
    uiTextCentered("FOLLOW SIGNALS", 80);
    uiTextCentered(catalogCreatureOwned(creature) ? "OWNED" : "NOT OWNED",
                   94);
    uiTextCentered("ROSTER " + String(fieldGuideIndex + 1U) + "/" +
                       String(kitsu868::fun::kCatalogCreatureCount),
                   106);
    uiTextCentered("TRAIL " + String(signalTrail.missCount()) + "/" +
                       String(kitsu868::signal::kSignalTrailMaximumMisses),
                   118);
    return;
  }

  uiWrappedText(creature.name, 0, 2U, 8U);
  drawGuidePortrait(creature, 17);
  uiTextCentered(signalRarityName(creature.rarity), 99);
  uiTextCentered(catalogCreatureOwned(creature) ? "OWNED" : "NOT OWNED",
                 109);
  uiTextCentered(String(fieldGuideIndex + 1U) + "/" +
                     String(kitsu868::fun::kCatalogCreatureCount) + " S" +
                     String(kitsu868::fun::creatureEncounterCount(
                         funDiscovery, fieldGuideIndex)),
                 119);
}

void renderGoals() {
  uiTextCentered("SESSION GOALS", 5);
  uiTextCentered("CARE " + String(sessionChallenges.care) + "/" +
                     String(kitsu868::fun::challengeTarget(
                         kitsu868::fun::SessionActivity::Care)),
                 29);
  uiTextCentered("GAME " + String(sessionChallenges.games) + "/" +
                     String(kitsu868::fun::challengeTarget(
                         kitsu868::fun::SessionActivity::Game)),
                 47);
  uiTextCentered("SIGNAL " + String(sessionChallenges.signals) + "/" +
                     String(kitsu868::fun::challengeTarget(
                         kitsu868::fun::SessionActivity::Signal)),
                 65);
  uiTextCentered(sessionAuraActive ? "AURA ACTIVE" : "COMPLETE ALL", 87, 2);
  uiTextCentered("HOLD BACK", 113);
}

const char* clockFieldLabel(kitsu868::timekeeping::ClockEditorField field) {
  using kitsu868::timekeeping::ClockEditorField;
  switch (field) {
    case ClockEditorField::Year: return "YEAR";
    case ClockEditorField::Month: return "MONTH";
    case ClockEditorField::Day: return "DAY";
    case ClockEditorField::Hour: return "HOUR";
    case ClockEditorField::Minute: return "MINUTE";
    case ClockEditorField::UtcOffset: return "UTC OFFSET";
    case ClockEditorField::Review: return "REVIEW";
    case ClockEditorField::Inactive: return "CLOCK";
  }
  return "CLOCK";
}

void renderClock() {
  const kitsu868::timekeeping::ClockEditorView view = clockEditor.view();
  uiTextCentered("SET CLOCK", 5, 2);
  if (!view.active) {
    uiTextCentered("EDITOR OFF", 47, 2);
    uiTextCentered("HOLD BACK", 104);
    return;
  }
  uiTextCentered(clockFieldLabel(view.field), 27);
  char date[16]{};
  char timeText[10]{};
  snprintf(date, sizeof(date), "%04u-%02u-%02u", view.year, view.month,
           view.day);
  snprintf(timeText, sizeof(timeText), "%02u:%02u", view.hour, view.minute);
  uiTextCentered(date, 45, 1);
  uiTextCentered(timeText, 61, 2);
  uiTextCentered("UTC " + utcOffsetText(view.utcOffsetMinutes), 83);
  uiTextCentered(view.field == kitsu868::timekeeping::ClockEditorField::Review
                     ? "HOLD SAVE"
                     : "TAP CHANGE",
                 101);
  uiTextCentered(view.field == kitsu868::timekeeping::ClockEditorField::Review
                     ? "TAP REVISE"
                     : "HOLD NEXT",
                 114);
}

const char* terrainLabel(kitsu868::adventure::Terrain value) {
  using kitsu868::adventure::Terrain;
  switch (value) {
    case Terrain::Meadow: return "MEADOW";
    case Terrain::Forest: return "FOREST";
    case Terrain::Ridge: return "RIDGE";
    case Terrain::Waterfront: return "WATER";
    case Terrain::Town: return "TOWN";
    case Terrain::Count: break;
  }
  return "?";
}

const char* objectiveLabel(kitsu868::adventure::Objective value) {
  using kitsu868::adventure::Objective;
  switch (value) {
    case Objective::Explore: return "EXPLORE";
    case Objective::FollowSignal: return "SIGNAL";
    case Objective::MeetCreature: return "CREATURE";
    case Objective::Community: return "COMMUNITY";
    case Objective::ReturnHome: return "HOME";
    case Objective::Count: break;
  }
  return "?";
}

const char* riskLabel(kitsu868::adventure::Risk value) {
  using kitsu868::adventure::Risk;
  switch (value) {
    case Risk::Careful: return "CAREFUL";
    case Risk::Balanced: return "BALANCED";
    case Risk::Bold: return "BOLD";
    case Risk::Count: break;
  }
  return "?";
}

const char* weatherLabel(kitsu868::adventure::Weather value) {
  using kitsu868::adventure::Weather;
  switch (value) {
    case Weather::Unknown: return "UNKNOWN";
    case Weather::Clear: return "CLEAR";
    case Weather::Rain: return "RAIN";
    case Weather::Wind: return "WIND";
    case Weather::Snow: return "SNOW";
    case Weather::Count: break;
  }
  return "?";
}

const char* timeBandLabel(kitsu868::adventure::TimeBand value) {
  using kitsu868::adventure::TimeBand;
  switch (value) {
    case TimeBand::Unknown: return "unknown";
    case TimeBand::Dawn: return "dawn";
    case TimeBand::Day: return "day";
    case TimeBand::Dusk: return "dusk";
    case TimeBand::Night: return "night";
    case TimeBand::Count: break;
  }
  return "invalid";
}

const char* moonPhaseLabel(kitsu868::adventure::MoonPhase value) {
  using kitsu868::adventure::MoonPhase;
  switch (value) {
    case MoonPhase::Unknown: return "unknown";
    case MoonPhase::NewMoon: return "new";
    case MoonPhase::Waxing: return "waxing";
    case MoonPhase::FullMoon: return "full";
    case MoonPhase::Waning: return "waning";
    case MoonPhase::Count: break;
  }
  return "invalid";
}

void renderAdventure() {
  uiTextCentered("ADVENTURE", 3, 2);
  const kitsu868::expedition::ExpeditionView expedition = expeditionCore.view();
  if (expedition.phase == kitsu868::expedition::Phase::Traveling) {
    uiTextCentered(kitsu868::expedition::durationLabel(expedition.duration), 27);
    uiProgressBar(expedition.progressPercent, 48);
    uiTextCentered(String(expedition.progressPercent) + "%", 62, 2);
    uiTextCentered(String(expedition.remainingSeconds / 60U) + " MIN LEFT", 86);
    uiTextCentered("HOLD MENU", 110);
    return;
  }
  if (expedition.phase == kitsu868::expedition::Phase::Ready) {
    uiTextCentered("TRIP BACK", 31);
    uiTextCentered("REPORT READY", 52, 2);
    uiTextCentered("HOLD CLAIM", 91);
    uiTextCentered("TAP MENU", 108);
    return;
  }
  const kitsu868::adventure::RouteView route = adventureProgression.view();
  if (route.phase == kitsu868::adventure::RoutePhase::Active ||
      route.phase == kitsu868::adventure::RoutePhase::AwaitingRescue) {
    uiTextCentered(route.phase == kitsu868::adventure::RoutePhase::AwaitingRescue
                       ? "NEEDS PARTY"
                       : terrainLabel(route.terrain),
                   25);
    uiProgressBar(route.progressPercent, 46);
    uiTextCentered("STEPS " + String(route.progressPercent) + "%", 61);
    uiTextCentered("CHOICES " + String(route.decisionCount) + "/3", 79);
    uiTextCenteredFit(ADVENTURE_ITEMS[adventureMenuIndex], 98, 1);
    uiTextCentered("TAP/HOLD", 113);
    return;
  }
  if (route.phase == kitsu868::adventure::RoutePhase::Returned) {
    kitsu868::adventure::TextPostcard postcard{};
    uiTextCentered(route.outcome == kitsu868::adventure::Outcome::Complete
                       ? "ROUTE DONE"
                       : route.outcome == kitsu868::adventure::Outcome::Rescued
                             ? "RESCUED"
                             : "ROUTE BACK",
                   25);
    if (adventureProgression.currentPostcard(postcard)) {
      uiTextCenteredFit(postcard.title, 46, 1);
      uiWrappedText(postcard.line, 61, 3U, 10U);
    }
    uiTextCentered("TAP MENU", 98);
    uiTextCentered("HOLD ACK", 113);
    return;
  }
  uiTextCenteredFit(ADVENTURE_ITEMS[adventureMenuIndex], 33, 1);
  if (adventureMenuIndex == 9U) {
    uiTextCentered(terrainLabel(adventureTerrain), 56, 2);
  } else if (adventureMenuIndex == 10U) {
    uiTextCentered(objectiveLabel(adventureObjective), 56, 2);
  } else if (adventureMenuIndex == 11U) {
    uiTextCentered(riskLabel(adventureRisk), 56, 2);
  } else if (adventureMenuIndex == 12U) {
    uiTextCentered(weatherLabel(adventureWeather), 56, 2);
  } else {
    uiTextCentered(terrainLabel(adventureTerrain), 50);
    uiTextCentered(objectiveLabel(adventureObjective), 64);
    uiTextCentered(String(riskLabel(adventureRisk)[0]) + "/" +
                       weatherLabel(adventureWeather), 78);
  }
  uiMenuDots(adventureMenuIndex,
             sizeof(ADVENTURE_ITEMS) / sizeof(ADVENTURE_ITEMS[0]), 91);
  uiTextCentered("TAP NEXT", 104);
  uiTextCentered("HOLD SELECT", 116);
}

void renderActivity() {
  const uint32_t now = millis();
  const kitsu868::activities::ActivityView view = activitySuite.view(now);
  uiTextCenteredFit(kitsu868::activities::activityName(view.kind), 4, 2);
  using kitsu868::activities::ActivityKind;
  using kitsu868::activities::ActivityPhase;
  if (view.phase == ActivityPhase::Presenting) {
    uiTextCentered("WATCH", 31);
    uiTextCentered(view.cueOn ? "DASH" : "DOT", 51, 2);
    uiTextCentered(String(view.progress + 1U) + "/" + String(view.total), 76);
    uiTextCentered("THEN REPEAT", 102);
  } else if (view.phase == ActivityPhase::Playing) {
    if (view.kind == ActivityKind::MorseSignal) {
      uiTextCentered("REPEAT", 29);
      uiTextCentered(String(view.progress) + "/" + String(view.total), 49, 2);
      uiTextCentered("TAP DOT", 78);
      uiTextCentered("HOLD DASH", 96);
    } else if (view.kind == ActivityKind::StaticTuner) {
      uiTextCentered("TARGET " + String(view.target), 28);
      uiProgressBar(view.marker, 52);
      uiTextCentered("SIGNAL " + String(view.marker), 67);
      uiTextCentered("TAP TO LOCK", 97);
    } else if (view.kind == ActivityKind::ReactionFlash) {
      uiTextCentered(view.cueOn ? "NOW!" : "WAIT", 39, 2);
      uiTextCentered(view.cueOn ? "TAP" : "HANDS OFF", 79);
    } else if (view.kind == ActivityKind::HoldSteady) {
      uiTextCentered("HOLD PRG", 32, 2);
      uiProgressBar(view.progress, 63);
      uiTextCentered("RELEASE 100", 87);
    } else {
      uiTextCentered(view.marker < 50U ? "BREATHE IN" : "BREATHE OUT", 30);
      uiProgressBar(view.marker, 54);
      uiTextCentered("TAP AT PEAK", 76);
      uiTextCentered(String(view.progress) + "/" + String(view.total), 97, 2);
    }
  } else if (view.phase == ActivityPhase::Result) {
    uiTextCentered("SCORE", 31);
    uiTextCentered(String(view.score), 51, 2);
    if (view.ghostScore != 0U) {
      uiTextCentered("BEST " + String(view.ghostScore), 78);
    }
    uiTextCentered("NICE SIGNAL", 101);
  } else if (view.phase == ActivityPhase::Finished) {
    uiTextCentered("FINISHED", 35, 2);
    uiTextCentered(String(view.score) + "/" + String(view.maximumScore), 62);
    uiTextCentered("TAP BACK", 100);
  } else {
    uiTextCentered("NO ACTIVITY", 44, 2);
    uiTextCentered("TAP BACK", 100);
  }
}

void renderPairPhone() {
  const bool caretaker = pairingScreenRole ==
      kitsu868::connectivity::ControllerRole::Caretaker;
  if (caretaker) {
    uiTextCentered("PAIR", 2);
    uiTextCentered("CARETAKER", 11);
  } else {
    uiTextCentered("PAIR PHONE", 4);
  }
  if (!companionBle.ready()) {
    uiTextCentered("BLE OFF", 39, 2);
    uiTextCentered("STORAGE", 72);
    uiTextCentered("TAP BACK", 106);
    return;
  }
  const uint32_t now = millis();
  const kitsu868::connectivity::BleLinkStatus link =
      companionBle.linkStatus(now);
  const kitsu868::connectivity::BleSessionStatus session =
      companionBle.sessionStatus(now);
  if (!link.connected && companionBle.pairingStorageBlocked()) {
    uiTextCentered("STORAGE FULL", 24);
    uiTextCentered("PAIR BLOCKED", 49);
    uiTextCentered("HOLD RETRY", 78);
    uiTextCentered("TAP BACK", 105);
  } else if (link.numericComparisonPending) {
    char passkey[7]{};
    snprintf(passkey, sizeof(passkey), "%06lu",
             static_cast<unsigned long>(link.numericComparison));
    uiTextCentered("MATCH CODE", 22);
    uiTextCentered(passkey, 42, 2);
    uiTextCentered("HOLD IF SAME", 78);
    uiTextCentered("TAP CANCEL", 105);
  } else if (session.physicalConfirmationPending) {
    uiTextCentered(caretaker ? "CARETAKER" : "PHONE READY", 23);
    uiTextCentered("HOLD", 44, 2);
    uiTextCentered("PRG", 62, 2);
    uiTextCentered("TO GRANT", 85);
    uiTextCentered("TAP CANCEL", 105);
  } else if (session.pairingCompleted) {
    uiTextCentered(caretaker ? "CARETAKER" : "PHONE", 34);
    uiTextCentered("PAIRED", 58, 2);
    uiTextCentered("TAP CLOSE", 105);
  } else if (session.applicationAuthenticated) {
    uiTextCentered("CONNECTED", 38);
    uiTextCentered("APP VERIFIED", 70);
    uiTextCentered("TAP CLOSE", 105);
  } else if (link.connected) {
    uiTextCentered("SECURING", 38);
    uiTextCentered("WAIT", 64, 2);
    uiTextCentered("TAP CANCEL", 105);
  } else if (link.pairingWindowOpen) {
    const uint32_t seconds =
        (link.pairingWindowRemainingMs + 999U) / 1000U;
    uiTextCentered("OPEN", 27, 2);
    uiTextCentered(String(seconds) + "S", 49, 2);
    uiTextCentered(wisp.uid, 76);
    uiTextCentered("TAP CANCEL", 105);
  } else {
    uiTextCentered("WINDOW", 29, 2);
    uiTextCentered("CLOSED", 48, 2);
    uiTextCentered("HOLD REOPEN", 78);
    uiTextCentered("TAP BACK", 106);
  }
}

void renderControllerManager() {
  uiTextCentered("CONTROLLERS", 4);
  const uint32_t now = millis();
  if (!connectivitySecurityReady) {
    uiTextCentered("SECURITY", 31, 2);
    uiTextCentered("UNAVAILABLE", 54);
    uiTextCentered("NO CHANGES", 79);
    uiTextCentered("TAP BACK", 108);
    return;
  }
  if (!controllerRecoveryBleDisconnected(now)) {
    uiTextCentered("CLOSING BLE", 34);
    uiTextCentered("WAIT", 55, 2);
    uiTextCentered("NO CHANGES", 82);
    uiTextCentered("TAP BACK", 108);
    return;
  }

  const uint8_t count = deviceSecurity.status().controllerCount;
  if (controllerRecoverySelection <
      kitsu868::connectivity::kKitsuControllerCapacity) {
    uint8_t id[kitsu868::connectivity::kKitsuControllerIdBytes]{};
    const bool occupied = deviceSecurity.controllerAtSlot(
        controllerRecoverySelection, id);
    uiTextCentered("SLOT " + String(controllerRecoverySelection + 1U), 28,
                   2);
    if (occupied) {
      uiTextCentered("ID " + controllerFingerprint(id), 54);
      uiTextCentered("HOLD REMOVE", 78);
    } else {
      uiTextCentered("EMPTY", 54, 2);
      uiTextCentered("NO ACTION", 78);
    }
    memset(id, 0, sizeof(id));
  } else if (controllerRecoverySelection ==
             CONTROLLER_RECOVERY_RESET_INDEX) {
    uiTextCentered("RESET ALL", 31, 2);
    uiTextCentered(String(count) + " STORED", 56);
    uiTextCentered(count == 0U ? "NO ACTION" : "HOLD SELECT", 78);
  } else if (controllerRecoverySelection ==
             CONTROLLER_RECOVERY_BLE_BONDS_INDEX) {
    // Monochrome 64 px layout for: CLEAR BLE BONDS / CONTROLLERS KEPT.
    const int bonds = companionBle.bleBondCount();
    uiTextCentered("CLEAR", 19, 2);
    uiTextCentered("BLE BONDS", 40);
    uiTextCentered("CONTROLLERS", 57);
    uiTextCentered("KEPT", 70);
    uiTextCentered(bonds > 0 ? "HOLD SELECT" : "NO BONDS", 83);
  } else {
    uiTextCentered("BACK", 36, 2);
    uiTextCentered("HOLD RETURN", 69);
  }
  uiMenuDots(controllerRecoverySelection,
             CONTROLLER_RECOVERY_OPTION_COUNT, 94);
  uiTextCentered("TAP NEXT", 108);
}

void renderControllerConfirm() {
  const uint32_t now = millis();
  const bool clearBleBonds =
      controllerRecoveryTargetSlot == CONTROLLER_RECOVERY_BLE_BONDS_INDEX;
  const bool resetAll =
      controllerRecoveryTargetSlot == CONTROLLER_RECOVERY_RESET_INDEX;
  if (clearBleBonds) {
    uiTextCentered("CLEAR", 2, 2);
    uiTextCentered("BLE BONDS", 23);
    uiTextCentered(String(companionBle.bleBondCount()) + " STORED", 38);
    uiTextCentered("CONTROLLERS", 51);
    uiTextCentered("KEPT", 64);
    if (!controllerRecoveryBleDisconnected(now)) {
      uiTextCentered("BLE LINKED", 76);
      uiTextCentered("CANCELLED", 89);
    } else if (stableButton) {
      const uint32_t held = now - buttonPressedAt;
      const uint32_t holdRemaining = held >= CONTROLLER_RECOVERY_HOLD_MS
          ? 0U
          : (CONTROLLER_RECOVERY_HOLD_MS - held + 999U) / 1000U;
      uiTextCentered("KEEP HOLDING " + String(holdRemaining) + "S", 78);
    } else {
      uiTextCentered("HOLD PRG", 76);
      uiTextCentered("5S TO CLEAR", 89);
    }
    uiTextCentered("TAP CANCEL", 102);
    uiTextCentered("EXPIRES " +
                       String(controllerRecoverySecondsRemaining(
                           now, controllerRecoveryDeadline)) +
                       "S",
                   114);
    return;
  }

  uiTextCentered(resetAll ? "REMOVE ALL" : "REMOVE", 4,
                 resetAll ? 1 : 2);
  if (resetAll) {
    uiTextCentered(String(controllerRecoveryOriginalCount) + " STORED", 24);
  } else {
    uiTextCentered("SLOT " + String(controllerRecoveryTargetSlot + 1U), 23,
                   2);
    uiTextCentered("ID " + controllerFingerprint(controllerRecoveryTargetId),
                   42);
  }

  if (!controllerRecoveryBleDisconnected(now)) {
    uiTextCentered("BLE LINKED", (resetAll || clearBleBonds) ? 49 : 60);
    uiTextCentered("CANCELLED", (resetAll || clearBleBonds) ? 67 : 76, 2);
  } else if (stableButton) {
    const uint32_t held = now - buttonPressedAt;
    const uint32_t holdRemaining = held >= CONTROLLER_RECOVERY_HOLD_MS
        ? 0U
        : (CONTROLLER_RECOVERY_HOLD_MS - held + 999U) / 1000U;
    uiTextCentered("KEEP HOLDING", (resetAll || clearBleBonds) ? 50 : 60);
    uiTextCentered(String(holdRemaining) + "S",
                   (resetAll || clearBleBonds) ? 67 : 76, 2);
  } else {
    uiTextCentered("HOLD PRG", (resetAll || clearBleBonds) ? 50 : 58, 2);
    uiTextCentered(clearBleBonds ? "5S TO CLEAR" : "5S TO REMOVE",
                   (resetAll || clearBleBonds) ? 71 : 76);
  }
  uiTextCentered("TAP CANCEL", 95);
  uiTextCentered("EXPIRES " +
                     String(controllerRecoverySecondsRemaining(
                         now, controllerRecoveryDeadline)) +
                     "S",
                 109);
}

void renderControllerResult() {
  uiTextCentered("CONTROLLERS", 4);
  switch (controllerRecoveryResult) {
    case ControllerRecoveryResult::RemovedSlot:
      uiTextCentered("REMOVED", 29, 2);
      uiTextCentered("SLOT " + String(controllerRecoveryTargetSlot + 1U),
                     53);
      uiTextCentered("REOPEN PAIR", 79);
      break;
    case ControllerRecoveryResult::RemovedAll:
      uiTextCentered("ALL REMOVED", 29, 2);
      uiTextCentered(String(controllerRecoveryOriginalCount) + " CLEARED",
                     53);
      uiTextCentered("REOPEN PAIR", 79);
      break;
    case ControllerRecoveryResult::StorageNeedsReboot:
      uiTextCentered("UNCERTAIN", 23, 2);
      uiTextCentered("STORAGE ERR", 43);
      uiTextCentered("REBOOT KITSU", 64, 2);
      uiTextCentered("BEFORE PAIR", 88);
      break;
    case ControllerRecoveryResult::BleBondsCleared:
      uiTextCentered("BLE BONDS", 24, 2);
      uiTextCentered("CLEARED", 46, 2);
      uiTextCentered("CONTROLLERS", 72);
      uiTextCentered("KEPT", 86);
      break;
    case ControllerRecoveryResult::BleBondStoreError:
      uiTextCentered("BLE BONDS", 20, 2);
      uiTextCentered("NOT CLEARED", 43, 2);
      uiTextCentered("CONTROLLERS", 68);
      uiTextCentered("KEPT", 81);
      uiTextCentered("REBOOT RETRY", 94);
      break;
    case ControllerRecoveryResult::ControllerAuthorityChanged:
      uiTextCentered("SAFETY CHECK", 20, 2);
      uiTextCentered("FAILED", 43, 2);
      uiTextCentered("NO CLAIM", 69);
      uiTextCentered("REBOOT KITSU", 89);
      break;
    case ControllerRecoveryResult::Unchanged:
      uiTextCentered("NOT REMOVED", 29, 2);
      uiTextCentered("STORAGE ERROR", 57);
      uiTextCentered("TRY AGAIN", 82);
      break;
    case ControllerRecoveryResult::None:
      uiTextCentered("CANCELLED", 40, 2);
      uiTextCentered("NO CHANGES", 70);
      break;
  }
  uiTextCentered(
      controllerRecoveryRequiresReboot() ? "REBOOT NOW" : "TAP CONTINUE",
      108);
}

void renderWildEncounter() {
  if (!wildEncounterView.available || !wildEncounterView.creature.name) {
    uiTextCentered("WILD ENCOUNTER", 2);
    uiTextCentered("SIGNAL LOST", 48, 2);
    uiTextCentered("TAP OR HOLD", 108);
    return;
  }

  uiWrappedText(wildEncounterView.creature.name, 0, 2U, 8U);
  drawGuidePortrait(wildEncounterView.creature, 17);

  String rarity = signalRarityName(wildEncounterView.creature.rarity);
  rarity.replace("_", " ");
  rarity.toUpperCase();
  uiTextCenteredFit(rarity, 99, 1);
  uiTextCentered(wildEncounterView.codeRevealed ? "CODE FOUND" : "NO CODE",
                 109);
  uiTextCentered("TAP OR HOLD", 119);
}

void renderDisplay(bool force = false) {
  if (!oledDetected || displaySleeping) return;
  if (!force && millis() - lastRenderAt < 100) return;
  lastRenderAt = millis();

  display.clear();
  switch (screen) {
    case Screen::Pet: renderPet(); break;
    case Screen::Menu: renderMenu(); break;
    case Screen::Connect: renderConnect(); break;
    case Screen::Inbox: renderInbox(); break;
    case Screen::GameMenu: renderGameMenu(); break;
    case Screen::Game: renderGame(); break;
    case Screen::Listen: renderListen(); break;
    case Screen::Sleep: renderSleep(); break;
    case Screen::Status: renderStatus(); break;
    case Screen::WildEncounter: renderWildEncounter(); break;
    case Screen::FieldGuide: renderFieldGuide(); break;
    case Screen::Goals: renderGoals(); break;
    case Screen::Clock: renderClock(); break;
    case Screen::Adventure: renderAdventure(); break;
    case Screen::Activity: renderActivity(); break;
    case Screen::PairPhone: renderPairPhone(); break;
    case Screen::ControllerManager: renderControllerManager(); break;
    case Screen::ControllerConfirm: renderControllerConfirm(); break;
    case Screen::ControllerResult: renderControllerResult(); break;
  }
  display.display();
}

void enterScreen(Screen next) {
  wakeDisplay();
  screen = next;
  screenEnteredAt = millis();
  lastInteractionAt = screenEnteredAt;
  renderDisplay(true);
}

void dismissWildEncounter() {
  if (!pendingWildMaterialized && pendingWildOperationId != 0U) {
    pendingWildMaterialized = materializePendingWildEncounter();
  }
  if (pendingWildMaterialized && clearPendingWildEncounter()) {
    pendingWildOperationId = 0U;
    pendingWildEntropy = 0U;
    pendingWildCodeOutcome = kitsu868::signal::CodeOutcome::NotApplicable;
    pendingWildMaterialized = false;
  } else if (pendingWildMaterialized) {
    Serial.println("KITSU_WARN pending_wild=clear_failed");
  }
  wildEncounterView.available = false;
  enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
}

void showHatchSequence() {
  if (!oledDetected || !firstHatch) return;
  for (uint8_t frame = 0; frame < 3; ++frame) {
    display.clear();
    uiTextCentered("SIGNAL", 20);
    uiTextCentered(frame == 0 ? "(.)" : frame == 1 ? "(...)" : "*", 56, 2);
    if (frame == 2) uiTextCentered("FOUND", 101);
    display.display();
    delay(420);
  }
}

void cancelAmbientAnimation() {
  scheduleNextAmbientAnimation();
}

bool requireCompanion() {
  if (companionPack.valid()) return true;
  lastMemory = "No companion pack is installed.";
  Serial.printf("KITSU_ERROR no_pack reason=%s\n", companionPack.error());
  statusPage = 5;
  enterScreen(Screen::Status);
  return false;
}

void logBrainResult(const kitsu868::BrainEventResult& result) {
  Serial.printf(
      "KITSU_BRAIN xp=%u bond=%u->%u stage=%u->%u evolved=%s\n",
      result.xpAwarded, result.bondBefore, result.bondAfter,
      static_cast<unsigned>(result.stageBefore),
      static_cast<unsigned>(result.stageAfter),
      result.evolved() ? "true" : "false");
}

void startReaction(CompanionRole normalRole,
                   const kitsu868::BrainEventResult& result) {
  cancelAmbientAnimation();
  const bool evolved = result.evolved() || pendingEvolutionReaction;
  pendingEvolutionReaction = false;
  const CompanionRole role = evolved
                                 ? CompanionRole::Evolve
                                 : normalRole;
  if (!startTransientAnimation(role)) startBaseAnimation();
  logBrainResult(result);
}

CompanionRole momentReactionRole(kitsu868::fun::Reaction reaction) {
  using kitsu868::fun::Reaction;
  switch (reaction) {
    case Reaction::Blink: return CompanionRole::Blink;
    case Reaction::Pet: return CompanionRole::Pet;
    case Reaction::Surprise: return CompanionRole::Surprise;
    case Reaction::Play: return CompanionRole::Play;
    case Reaction::Tired: return CompanionRole::Tired;
    case Reaction::Feed: return CompanionRole::Feed;
    case Reaction::Wake: return CompanionRole::Wake;
    case Reaction::Meet: return CompanionRole::Meet;
    case Reaction::Evolve: return CompanionRole::Evolve;
  }
  return CompanionRole::Blink;
}

void showMoment(kitsu868::fun::MomentTrigger trigger,
                bool startAnimation = false) {
  const kitsu868::fun::Moment moment = kitsu868::fun::personalityMoment(
      companionBrain.personality().kind, trigger, esp_random());
  momentView.active = true;
  momentView.line1 = moment.line1;
  momentView.line2 = moment.line2;
  momentView.until = millis() + MOMENT_DISPLAY_MS;
  if (startAnimation) {
    cancelAmbientAnimation();
    if (!startTransientAnimation(momentReactionRole(moment.reaction))) {
      startBaseAnimation();
    }
  }
}

void showActionDialogue(kitsu868::dialogue::Action action,
                        kitsu868::dialogue::ActionOutcome outcome,
                        bool nearby, bool startPersonalityAnimation) {
  kitsu868::dialogue::ActionContext context{};
  context.personality = companionBrain.personality().kind;
  context.mood = companionBrain.mood(companionVitals());
  context.vitals = companionVitals();
  context.bondLevel = companionBrain.bondLevel();
  context.outcome = outcome;
  context.nearby = nearby;
  const kitsu868::dialogue::ActionState before = dialogueActions;
  const kitsu868::dialogue::ActionLine line =
      kitsu868::dialogue::selectActionLine(
          action, context, companionBrain.deviceFingerprint(),
          dialogueActions);
  momentView.active = true;
  momentView.line1 = line.line1;
  momentView.line2 = line.line2;
  momentView.until = millis() + MOMENT_DISPLAY_MS;
  if (activityStateReady && line.id != 0U) {
    activitySuite.rememberDialogue(line.id);
    if (!persistActivityState()) {
      Serial.println("KITSU_WARN dialogue_replay=flush_failed");
    }
  }
  if (dialogueStateReady && !persistDialogueState()) {
    dialogueActions = before;
    Serial.println("KITSU_WARN dialogue_state=flush_failed");
  }
  if (!nearby && outcome == kitsu868::dialogue::ActionOutcome::Success) {
    recordCompanionAction(action);
  }
  if (!startPersonalityAnimation) return;
  kitsu868::fun::MomentTrigger trigger =
      kitsu868::fun::MomentTrigger::Play;
  switch (action) {
    case kitsu868::dialogue::Action::Pet:
      trigger = kitsu868::fun::MomentTrigger::Pet;
      break;
    case kitsu868::dialogue::Action::Feed:
      trigger = kitsu868::fun::MomentTrigger::Feed;
      break;
    case kitsu868::dialogue::Action::Wake:
      trigger = kitsu868::fun::MomentTrigger::Wake;
      break;
    case kitsu868::dialogue::Action::Meet:
    case kitsu868::dialogue::Action::Gift:
      trigger = kitsu868::fun::MomentTrigger::Encounter;
      break;
    case kitsu868::dialogue::Action::Play:
    case kitsu868::dialogue::Action::Listen:
    case kitsu868::dialogue::Action::Sleep:
      break;
  }
  const kitsu868::fun::Moment personality =
      kitsu868::fun::personalityMoment(
          companionBrain.personality().kind, trigger, esp_random());
  cancelAmbientAnimation();
  if (!startTransientAnimation(momentReactionRole(personality.reaction))) {
    startBaseAnimation();
  }
}

kitsu868::progression::Season currentSeason(uint32_t now) {
  kitsu868::timekeeping::ClockReading reading{};
  if (!clockReading(now, reading, true)) {
    return kitsu868::progression::Season::Spring;
  }
  const time_t local = static_cast<time_t>(
      reading.unixSeconds + static_cast<int64_t>(reading.utcOffsetMinutes) *
                                60LL);
  tm broken{};
  gmtime_r(&local, &broken);
  const uint8_t month = static_cast<uint8_t>(broken.tm_mon + 1);
  if (month >= 3U && month <= 5U) return kitsu868::progression::Season::Spring;
  if (month >= 6U && month <= 8U) return kitsu868::progression::Season::Summer;
  if (month >= 9U && month <= 11U) return kitsu868::progression::Season::Autumn;
  return kitsu868::progression::Season::Winter;
}

void showProgressionLine(const kitsu868::progression::DisplayLine& line) {
  if (!line.line1 || line.line1[0] == '\0') return;
  momentView.active = true;
  momentView.line1 = line.line1;
  momentView.line2 = line.line2;
  momentView.until = millis() + MOMENT_DISPLAY_MS;
}

void queueProgressionLine(const kitsu868::progression::DisplayLine& line) {
  if (!line.line1 || line.line1[0] == '\0') return;
  Serial.printf("KITSU_PROGRESSION line1=\"%s\" line2=\"%s\"\n",
                line.line1, line.line2 ? line.line2 : "");
  if (progressionPendingLineCount >= PROGRESSION_LINE_CAPACITY) {
    progressionPendingLineHead = static_cast<uint8_t>(
        (progressionPendingLineHead + 1U) % PROGRESSION_LINE_CAPACITY);
    --progressionPendingLineCount;
    Serial.println("KITSU_WARN progression_lines=oldest_dropped");
  }
  const uint8_t slot = static_cast<uint8_t>(
      (progressionPendingLineHead + progressionPendingLineCount) %
      PROGRESSION_LINE_CAPACITY);
  progressionPendingLines[slot] = line;
  ++progressionPendingLineCount;
}

bool takeProgressionLine(kitsu868::progression::DisplayLine& line) {
  if (progressionPendingLineCount == 0U) return false;
  line = progressionPendingLines[progressionPendingLineHead];
  progressionPendingLines[progressionPendingLineHead] = {};
  progressionPendingLineHead = static_cast<uint8_t>(
      (progressionPendingLineHead + 1U) % PROGRESSION_LINE_CAPACITY);
  --progressionPendingLineCount;
  return true;
}

const char* progressionActionDisplayName(
    kitsu868::dialogue::Action action) {
  using kitsu868::dialogue::Action;
  switch (action) {
    case Action::Pet: return "PET";
    case Action::Feed: return "FEED";
    case Action::Play: return "PLAY";
    case Action::Listen: return "LISTEN";
    case Action::Sleep: return "SLEEP";
    case Action::Wake: return "WAKE";
    case Action::Meet: return "MEET";
    case Action::Gift: return "GIFT";
  }
  return "ACTION";
}

void recordCompanionAction(kitsu868::dialogue::Action action) {
  if (!companionProgressionReady) return;
  uint32_t day = 0U;
  uint16_t minute = 0U;
  if (!currentLocalDayMinute(day, minute, true)) return;
  uint8_t before[kitsu868::progression::kSnapshotCapacity]{};
  if (!captureCompanionProgression(before)) return;
  const uint32_t previousSessionDay = progressionLastSessionDay;
  const uint8_t previousSpeech = companionProgression.speechStage();
  const uint8_t previousBondBank =
      companionProgression.bondDialogueBank(companionBrain.bondLevel());
  kitsu868::progression::SessionResult sessionResult{};
  bool startedNewDay = false;
  if (progressionLastSessionDay != day) {
    sessionResult = companionProgression.startSession(
        day, minute, companionBrain.bondLevel(), companionVitals(),
        currentSeason(millis()));
    startedNewDay = sessionResult.valid;
    progressionLastSessionDay = day;
  }
  const kitsu868::progression::ActionResult result =
      companionProgression.recordAction(action, day, minute,
                                        companionBrain.bondLevel());
  if (!result.valid) {
    restoreCompanionProgression(before);
    progressionLastSessionDay = previousSessionDay;
    return;
  }
  (void)companionProgression.observeBond(companionBrain.bondLevel(), day);
  if (!persistCompanionProgression()) {
    restoreCompanionProgression(before);
    progressionLastSessionDay = previousSessionDay;
    Serial.println("KITSU_WARN progression_action=flush_failed");
    return;
  }
  if (startedNewDay) queueProgressionSession(sessionResult, day);
  if (result.requestCompleted) {
    queueProgressionLine({"YOU REMEMBERED", "THANK YOU"});
  }
  if (result.goalCompletedNow) {
    queueProgressionLine({"DAILY GOAL", "COMPLETE"});
  }
  if (result.ritualRecognized) {
    queueProgressionLine({"OUR RITUAL", "RIGHT ON TIME"});
  }
  if (result.favoriteChanged) {
    queueProgressionLine({"NEW FAVORITE", "I LIKE THIS"});
  }
  if (result.secretHabitUnlocked) {
    queueProgressionLine(
        kitsu868::progression::CompanionProgression::habitLine(
            companionProgression.secretHabit()));
  }
  if (result.comforted) {
    queueProgressionLine({"THAT HELPED", "THANK YOU"});
  }
  if (result.followedSuggestion) {
    queueProgressionLine({"GOOD IDEA", "TOGETHER"});
  }
  queueProgressionLine(
      {"TRY NEXT", progressionActionDisplayName(result.followUp)});
  if (result.repeatNoticed) {
    queueProgressionLine({"AGAIN?", "I NOTICE"});
  }
  if (result.speechStage > previousSpeech) {
    queueProgressionLine(
        kitsu868::progression::CompanionProgression::speechLine(
            result.speechStage));
  }
  if (result.bondDialogueBank > previousBondBank) {
    queueProgressionLine(
        kitsu868::progression::CompanionProgression::bondLine(
            result.bondDialogueBank));
  }
  companionBleRefreshDirty = true;
}

void replayLastDialogue() {
  if (foregroundTransitionBlocked("replay")) return;
  uint16_t id = 0U;
  kitsu868::dialogue::ActionLine line{};
  if (!activityStateReady || !activitySuite.replayDialogue(id) ||
      !kitsu868::dialogue::actionLineById(id, line)) {
    Serial.println("KITSU_ERROR replay=empty");
    return;
  }
  momentView.active = true;
  momentView.line1 = line.line1;
  momentView.line2 = line.line2;
  momentView.until = millis() + MOMENT_DISPLAY_MS;
  enterScreen(Screen::Pet);
  Serial.printf("KITSU_REPLAY dialogue_id=%u\n", id);
}

void queueProgressionSession(
    const kitsu868::progression::SessionResult& result, uint32_t day) {
  if (!result.valid) return;
  if (result.greeting != kitsu868::progression::GreetingKind::Normal) {
    queueProgressionLine(
        kitsu868::progression::CompanionProgression::greetingLine(
            result.greeting));
  }
  if (result.requestOffered) {
    queueProgressionLine(
        kitsu868::progression::CompanionProgression::requestLine(
            companionProgression.requestedAction()));
  }
  if (result.questionOffered) {
    kitsu868::progression::QuestionKind question{};
    if (companionProgression.pendingQuestion(question)) {
      queueProgressionLine(
          kitsu868::progression::CompanionProgression::questionLine(
              question));
    }
  }
  if (result.comfort != kitsu868::progression::ComfortKind::None) {
    queueProgressionLine(
        kitsu868::progression::CompanionProgression::comfortLine(
            result.comfort));
  }
  if (result.rareMoment) {
    queueProgressionLine(
        kitsu868::progression::CompanionProgression::rareMomentLine(
            companionProgression.dailySeed(day)));
  }
  queueProgressionLine(
      kitsu868::progression::CompanionProgression::weeklyChapterLine(
          result.weeklyChapter));
  if (result.seasonalMoment) {
    queueProgressionLine(
        kitsu868::progression::CompanionProgression::seasonalLine(
            result.season));
  }
  if (result.anniversary) {
    queueProgressionLine({"ANNIVERSARY", "STILL TOGETHER"});
  }
  if (result.previousDayPerfect) {
    queueProgressionLine({"PERFECT DAY", "I REMEMBER"});
  }
}

void tickCompanionProgression(uint32_t now) {
  if (!companionProgressionReady) return;
  uint32_t day = 0U;
  uint16_t minute = 0U;
  if (!currentLocalDayMinute(day, minute, true)) return;
  const bool quiet = activityStateReady && activitySuite.quietAt(minute);
  const bool canPresent = !quiet && screen == Screen::Pet &&
      !momentView.active;
  const bool havePending = progressionPendingLineCount != 0U;
  if (canPresent && havePending) {
    kitsu868::progression::DisplayLine pending{};
    if (takeProgressionLine(pending)) showProgressionLine(pending);
    return;
  }
  if (progressionLastSessionDay == day && progressionLastMinute == minute) {
    return;
  }
  uint8_t before[kitsu868::progression::kSnapshotCapacity]{};
  const size_t snapshotBytes =
      kitsu868::progression::CompanionProgression::snapshotSize();
  if (snapshotBytes > sizeof(before) ||
      !companionProgression.writeSnapshot(before, sizeof(before))) {
    Serial.println("KITSU_WARN progression_session=snapshot_failed");
    return;
  }
  const uint32_t previousDay = progressionLastSessionDay;
  const uint16_t previousMinute = progressionLastMinute;
  progressionLastMinute = minute;
  const bool newDay = progressionLastSessionDay != day;
  const kitsu868::progression::SessionResult result =
      companionProgression.startSession(
          day, minute, companionBrain.bondLevel(), companionVitals(),
          currentSeason(now));
  progressionLastSessionDay = day;
  kitsu868::progression::Callback callback{};
  const bool callbackReady = canPresent && !havePending &&
      companionProgression.takeCallback(day, callback);
  uint8_t after[kitsu868::progression::kSnapshotCapacity]{};
  if (!companionProgression.writeSnapshot(after, sizeof(after))) {
    (void)companionProgression.restoreSnapshot(
        before, snapshotBytes, companionBrain.deviceFingerprint());
    progressionLastSessionDay = previousDay;
    progressionLastMinute = previousMinute;
    Serial.println("KITSU_WARN progression_session=snapshot_failed");
    return;
  }
  const bool changed = memcmp(before, after, snapshotBytes) != 0;
  if (changed && !persistCompanionProgression()) {
    (void)companionProgression.restoreSnapshot(
        before, snapshotBytes, companionBrain.deviceFingerprint());
    progressionLastSessionDay = previousDay;
    progressionLastMinute = previousMinute;
    Serial.println("KITSU_WARN progression_session=flush_failed");
    return;
  }
  if (callbackReady) {
    queueProgressionLine(
        kitsu868::progression::CompanionProgression::callbackLine(callback));
  }
  if (newDay) queueProgressionSession(result, day);
  if (canPresent && progressionPendingLineCount != 0U) {
    kitsu868::progression::DisplayLine pending{};
    if (takeProgressionLine(pending)) showProgressionLine(pending);
  }
}

void recordSessionGoal(kitsu868::fun::SessionActivity activity) {
  const kitsu868::fun::ChallengeUpdate update =
      kitsu868::fun::recordSessionActivity(sessionChallenges, activity);
  if (update.allCompletedNow != 0U) {
    sessionAuraActive = true;
    Serial.println("KITSU_GOAL all_complete aura=true");
  } else if (update.newlyCompletedMask != 0U) {
    Serial.printf("KITSU_GOAL complete=%s\n",
                  kitsu868::fun::challengeName(activity));
  }
  companionBleRefreshDirty = true;
}

bool recordDreamAfterSleep(uint32_t now) {
  if (!funStateReady || sleepStartedAt == 0U ||
      now - sleepStartedAt < DREAM_MINIMUM_SLEEP_MS ||
      (companionBrain.unlockMask() & kitsu868::UnlockDream) == 0U) {
    return false;
  }
  const kitsu868::fun::Dream dream = kitsu868::fun::selectDream(
      companionBrain.personality().kind,
      companionBrain.deviceFingerprint(), funDiscovery.completedDreams);
  if (!kitsu868::fun::recordDream(funDiscovery, dream.index) ||
      !persistFunState()) {
    Serial.println("KITSU_WARN dream=store_failed");
    return false;
  }
  momentView.active = true;
  momentView.line1 = dream.line1;
  momentView.line2 = dream.line2;
  momentView.until = now + MOMENT_DISPLAY_MS;
  Serial.printf("KITSU_DREAM index=%u total=%u\n", dream.index,
                funDiscovery.completedDreams);
  if (companionProgressionReady) {
    uint32_t day = 0U;
    uint16_t minute = 0U;
    uint8_t before[kitsu868::progression::kSnapshotCapacity]{};
    if (captureCompanionProgression(before) &&
        currentLocalDayMinute(day, minute, true) &&
        companionProgression.rememberDream(dream.index, day) &&
        !persistCompanionProgression()) {
      restoreCompanionProgression(before);
      Serial.println("KITSU_WARN dream_memory=store_failed");
    }
    (void)minute;
  }
  companionBleRefreshDirty = true;
  return true;
}

bool ensureAwake() {
  if (!wisp.sleeping) return false;
  const uint32_t now = millis();
  wisp.sleeping = false;
  (void)recordDreamAfterSleep(now);
  sleepStartedAt = 0U;
  const kitsu868::BrainEventResult result = companionBrain.onWake();
  pendingEvolutionReaction = pendingEvolutionReaction || result.evolved();
  lastMemory = "The signal woke up.";
  logBrainResult(result);
  return true;
}

bool petWisp() {
  if (!requireCompanion()) return false;
  const bool preserveWakeMoment = ensureAwake() && momentView.active;
  wisp.energy = min<uint8_t>(100, wisp.energy + 4);
  wisp.curiosity = min<uint8_t>(100, wisp.curiosity + 1);
  wisp.affection = min<uint8_t>(100, wisp.affection + 3);
  ++wisp.pets;
  lastMemory = "You reached through the static.";
  const kitsu868::BrainEventResult result = companionBrain.onPet();
  const bool personalityAnimationSafe =
      !result.evolved() && !pendingEvolutionReaction;
  startReaction(CompanionRole::Pet, result);
  if (!preserveWakeMoment) {
    showActionDialogue(kitsu868::dialogue::Action::Pet,
                       kitsu868::dialogue::ActionOutcome::Success, false,
                       personalityAnimationSafe);
  }
  recordSessionGoal(kitsu868::fun::SessionActivity::Care);
  persistProgress();
  if (preserveWakeMoment && screen == Screen::Sleep) enterScreen(Screen::Pet);
  Serial.printf("KITSU_EVENT pet count=%lu energy=%u affection=%u\n",
                static_cast<unsigned long>(wisp.pets), wisp.energy, wisp.affection);
  return true;
}

bool feedKitsu() {
  if (!requireCompanion()) return false;
  const bool preserveWakeMoment = ensureAwake() && momentView.active;
  wisp.energy = min<uint8_t>(100, wisp.energy + 18);
  wisp.affection = min<uint8_t>(100, wisp.affection + 1);
  lastMemory = "A small meal crossed the static.";
  const kitsu868::BrainEventResult result = companionBrain.onFeed();
  const bool personalityAnimationSafe =
      !result.evolved() && !pendingEvolutionReaction;
  startReaction(CompanionRole::Feed, result);
  if (!preserveWakeMoment) {
    showActionDialogue(kitsu868::dialogue::Action::Feed,
                       kitsu868::dialogue::ActionOutcome::Success, false,
                       personalityAnimationSafe);
  }
  recordSessionGoal(kitsu868::fun::SessionActivity::Care);
  persistProgress();
  if (preserveWakeMoment && screen == Screen::Sleep) enterScreen(Screen::Pet);
  Serial.printf("KITSU_EVENT feed energy=%u affection=%u\n",
                wisp.energy, wisp.affection);
  return true;
}

bool playKitsu() {
  if (!requireCompanion()) return false;
  const bool preserveWakeMoment = ensureAwake() && momentView.active;
  // Active play may exhaust the companion below the passive-decay floor, but
  // it must never increase energy when the current value is already low.
  wisp.energy = wisp.energy > 6 ? wisp.energy - 6 : 1;
  wisp.curiosity = min<uint8_t>(100, wisp.curiosity + 10);
  wisp.affection = min<uint8_t>(100, wisp.affection + 4);
  lastMemory = "You played through the static.";
  const kitsu868::BrainEventResult result = companionBrain.onPlay();
  const bool personalityAnimationSafe =
      !result.evolved() && !pendingEvolutionReaction;
  startReaction(CompanionRole::Play, result);
  if (!preserveWakeMoment) {
    showActionDialogue(kitsu868::dialogue::Action::Play,
                       kitsu868::dialogue::ActionOutcome::Success, false,
                       personalityAnimationSafe);
  }
  persistProgress();
  if (preserveWakeMoment && screen == Screen::Sleep) enterScreen(Screen::Pet);
  Serial.printf("KITSU_EVENT play energy=%u curiosity=%u affection=%u\n",
                wisp.energy, wisp.curiosity, wisp.affection);
  return true;
}

bool activityRuntimeBusy() {
  const kitsu868::activities::ActivityPhase phase =
      activitySuite.view(millis()).phase;
  return phase == kitsu868::activities::ActivityPhase::Presenting ||
      phase == kitsu868::activities::ActivityPhase::Playing ||
      phase == kitsu868::activities::ActivityPhase::Result;
}

bool foregroundTransitionBlocked(const char* action) {
  const char* reason = nullptr;
  if (activeGame != ActiveGame::None) reason = "game";
  else if (radioListening) reason = "radio";
  else if (activityRuntimeBusy()) reason = "activity";
  if (!reason) return false;
  Serial.printf("KITSU_ERROR %s=busy_%s\n",
                action ? action : "action", reason);
  return true;
}

void cancelActivityForSleep() {
  if (!activityRuntimeBusy()) return;
  activitySuite.cancel();
  activityRewarded = false;
  activityPressAccepted = false;
  if (activityStateReady && !persistActivityState()) {
    Serial.println("KITSU_WARN activity_state=sleep_cancel_flush_failed");
  }
}

void setSleeping(bool sleeping) {
  if (sleeping && !requireCompanion()) return;
  if (sleeping) cancelActivityForSleep();
  wisp.sleeping = sleeping;
  const uint32_t now = millis();
  if (sleeping) {
    sleepStartedAt = now;
  } else {
    (void)recordDreamAfterSleep(now);
    sleepStartedAt = 0U;
  }
  cancelAmbientAnimation();
  lastMemory = sleeping ? "The signal curled into a dream." : "The signal woke up.";
  const kitsu868::BrainEventResult result = sleeping
                                                ? companionBrain.onSleep()
                                                : companionBrain.onWake();
  const bool evolved = result.evolved() || pendingEvolutionReaction;
  pendingEvolutionReaction = false;
  if (sleeping) {
    if (evolved) startTransientAnimation(CompanionRole::Evolve);
    else startBaseAnimation();
    showActionDialogue(kitsu868::dialogue::Action::Sleep,
                       kitsu868::dialogue::ActionOutcome::Success, false,
                       false);
  } else {
    if (!startTransientAnimation(evolved ? CompanionRole::Evolve
                                         : CompanionRole::Wake)) {
      startBaseAnimation();
    }
    if (!momentView.active) {
      showActionDialogue(kitsu868::dialogue::Action::Wake,
                         kitsu868::dialogue::ActionOutcome::Success, false,
                         !evolved);
    }
  }
  logBrainResult(result);
  persistProgress();
  Serial.printf("KITSU_EVENT sleeping=%s\n", sleeping ? "true" : "false");
}

void startGame(ActiveGame game) {
  if (!requireCompanion()) return;
  if (radioListening) {
    Serial.println("KITSU_ERROR busy=listening");
    return;
  }
  if (activeGame != ActiveGame::None) {
    Serial.println("KITSU_ERROR busy=game");
    return;
  }
  if (activityRuntimeBusy()) {
    Serial.println("KITSU_ERROR busy=activity");
    return;
  }
  if (ensureAwake()) persistProgress();
  const bool wakeEvolved = pendingEvolutionReaction;
  pendingEvolutionReaction = false;
  activeGame = game;
  gameRewarded = false;
  gameEvolved = wakeEvolved;
  gamePerfectRounds = 0;
  lastGamePhase = kitsu868::MiniGamePhase::Playing;
  const uint32_t now = millis();
  const uint32_t seed = esp_random() ^ companionBrain.deviceFingerprint() ^ now;
  if (game == ActiveGame::SignalCatch) signalCatchGame.start(now, seed);
  else if (game == ActiveGame::PounceFetch) pounceFetchGame.start(now, seed);
  else if (game == ActiveGame::EchoBeat) echoBeatGame.start(now, seed);
  enterScreen(Screen::Game);
  const char* const gameName = game == ActiveGame::SignalCatch
                                   ? "signal"
                                   : game == ActiveGame::PounceFetch
                                         ? "pounce"
                                         : "echo";
  Serial.printf("KITSU_GAME start=%s\n", gameName);
}

uint16_t activeGameScore() {
  if (activeGame == ActiveGame::SignalCatch) return signalCatchGame.score();
  if (activeGame == ActiveGame::PounceFetch) return pounceFetchGame.score();
  if (activeGame == ActiveGame::EchoBeat) return echoBeatGame.score();
  return 0;
}

uint16_t activeGameMaximumScore() {
  if (activeGame == ActiveGame::EchoBeat) {
    return echoBeatGame.view(millis()).maximumScore;
  }
  return 24U;
}

bool activeGamePerfect() {
  if (activeGame == ActiveGame::EchoBeat) {
    const kitsu868::EchoBeatView view = echoBeatGame.view(millis());
    return view.totalBeats != 0U && view.perfectBeats == view.totalBeats;
  }
  return gamePerfectRounds == 5U;
}

bool activeGameFinished() {
  if (activeGame == ActiveGame::SignalCatch) return signalCatchGame.finished();
  if (activeGame == ActiveGame::PounceFetch) return pounceFetchGame.finished();
  if (activeGame == ActiveGame::EchoBeat) return echoBeatGame.finished();
  return false;
}

void rewardFinishedGame() {
  if (gameRewarded || !activeGameFinished()) return;
  gameRewarded = true;
  const uint16_t rawScore = activeGameScore();
  const uint16_t maximumScore = max<uint16_t>(1U, activeGameMaximumScore());
  const uint8_t scorePercent = static_cast<uint8_t>(
      min<uint16_t>(100, static_cast<uint16_t>(
          (static_cast<uint32_t>(rawScore) * 100U + maximumScore / 2U) /
          maximumScore)));
  const bool perfect = activeGamePerfect();
  wisp.energy = wisp.energy > 4U ? wisp.energy - 4U : 1U;
  wisp.curiosity = min<uint8_t>(100, wisp.curiosity + 5U);
  wisp.affection = min<uint8_t>(100, wisp.affection + (perfect ? 5U : 2U));
  const kitsu868::BrainEventResult result =
      companionBrain.onGame(scorePercent, perfect);
  gameEvolved = gameEvolved || result.evolved();
  lastMemory = perfect ? "Perfect timing became a memory."
                       : "A little game became a memory.";
  logBrainResult(result);
  recordSessionGoal(kitsu868::fun::SessionActivity::Game);
  showMoment(perfect ? kitsu868::fun::MomentTrigger::PerfectGame
                     : kitsu868::fun::MomentTrigger::Play);
  persistProgress();
  Serial.printf("KITSU_GAME finish score=%u percent=%u perfect=%s\n",
                rawScore, scorePercent, perfect ? "true" : "false");
}

void tickGame() {
  if (screen != Screen::Game || activeGame == ActiveGame::None) return;
  // Timing actions are resolved at the debounced press timestamp on release.
  // Keep the game phase stable while PRG is down so a round timeout cannot
  // erase that historical tap before it is dispatched.
  if (rawButton || stableButton) return;
  const uint32_t now = millis();
  if (activeGame == ActiveGame::SignalCatch) signalCatchGame.tick(now);
  else if (activeGame == ActiveGame::PounceFetch) pounceFetchGame.tick(now);
  else echoBeatGame.tick(now);
  const kitsu868::MiniGamePhase phase = activeGame == ActiveGame::SignalCatch
      ? signalCatchGame.view(now).phase
      : activeGame == ActiveGame::PounceFetch
            ? pounceFetchGame.view(now).phase
            : echoBeatGame.view(now).phase;
  if (lastGamePhase != kitsu868::MiniGamePhase::Result &&
      phase == kitsu868::MiniGamePhase::Result) {
    const kitsu868::MiniGameResult result =
        activeGame == ActiveGame::SignalCatch
            ? signalCatchGame.view(now).result
            : activeGame == ActiveGame::PounceFetch
                  ? pounceFetchGame.view(now).result
                  : echoBeatGame.view(now).result;
    if (result == kitsu868::MiniGameResult::Perfect &&
        gamePerfectRounds != 0xffU) {
      ++gamePerfectRounds;
    }
  }
  lastGamePhase = phase;
  rewardFinishedGame();
}

kitsu868::activities::CompanionModifier currentActivityModifier() {
  return kitsu868::activities::companionModifier(
      companionBrain.personality().kind, companionBrain.mood(companionVitals()));
}

bool startActivity(kitsu868::activities::ActivityKind kind, bool daily,
                   bool ghost) {
  if (!requireCompanion() || !activityStateReady) {
    Serial.println("KITSU_ERROR activity=unavailable");
    return false;
  }
  if (radioListening || activeGame != ActiveGame::None) {
    Serial.println("KITSU_ERROR activity=busy");
    return false;
  }
  if (ensureAwake()) persistProgress();
  const uint32_t now = millis();
  bool started = false;
  if (daily || ghost) {
    uint32_t day = 0U;
    uint16_t minute = 0U;
    if (!currentLocalDayMinute(day, minute, true)) {
      Serial.println("KITSU_ERROR activity=clock_unavailable");
      beginClockEditor();
      return false;
    }
    const kitsu868::activities::DailyActivity challenge =
        kitsu868::activities::proceduralDailyActivity(
            day, companionBrain.deviceFingerprint());
    if (daily) {
      started = activitySuite.startDaily(
          day, companionBrain.deviceFingerprint(), now,
          currentActivityModifier());
    } else {
      started = activitySuite.startGhost(challenge.kind, now,
                                          currentActivityModifier());
      kind = challenge.kind;
    }
  } else {
    const uint8_t difficulty = static_cast<uint8_t>(
        1U + min<uint8_t>(4U, companionBrain.bondLevel() / 2U));
    uint32_t seed = esp_random() ^ companionBrain.deviceFingerprint() ^ now;
    if (seed == 0U) seed = 1U;
    started = activitySuite.start(kind, now, seed, difficulty,
                                  currentActivityModifier());
  }
  if (!started) {
    Serial.printf("KITSU_ERROR activity=%s_start_failed\n",
                  kitsu868::activities::activityName(kind));
    return false;
  }
  activityRewarded = false;
  activityPressAccepted = false;
  if (!persistActivityState()) {
    activitySuite.cancel();
    Serial.println("KITSU_ERROR activity=storage_failed");
    return false;
  }
  enterScreen(Screen::Activity);
  Serial.printf("KITSU_ACTIVITY start=%s mode=%s\n",
                kitsu868::activities::activityName(kind),
                daily ? "daily" : ghost ? "ghost" : "normal");
  return true;
}

void leaveActivity() {
  const kitsu868::activities::ActivityState before = activitySuite.snapshot();
  activitySuite.cancel();
  activityRewarded = false;
  activityPressAccepted = false;
  if (activityStateReady && !persistActivityMutation(before, "cancel")) {
    return;
  }
  enterScreen(Screen::GameMenu);
}

void rewardFinishedActivity(const kitsu868::activities::ActivityView& view) {
  if (activityRewarded ||
      view.phase != kitsu868::activities::ActivityPhase::Result) {
    return;
  }
  activityRewarded = true;
  const uint16_t maximum = max<uint16_t>(1U, view.maximumScore);
  const uint8_t percent = static_cast<uint8_t>(min<uint32_t>(
      100U, (static_cast<uint32_t>(view.score) * 100U + maximum / 2U) /
                maximum));
  const bool perfect = view.score >= maximum;
  wisp.energy = wisp.energy > 3U ? wisp.energy - 3U : 1U;
  wisp.curiosity = min<uint8_t>(100U, wisp.curiosity + 4U);
  wisp.affection = min<uint8_t>(100U,
      wisp.affection + static_cast<uint8_t>(perfect ? 4U : 2U));
  const kitsu868::BrainEventResult result =
      companionBrain.onGame(percent, perfect);
  logBrainResult(result);
  recordSessionGoal(kitsu868::fun::SessionActivity::Game);
  recordCompanionAction(kitsu868::dialogue::Action::Play);
  lastMemory = String(kitsu868::activities::activityName(view.kind)) +
      " scored " + String(view.score) + ".";
  if (!saveState()) Serial.println("KITSU_WARN activity_reward=store_failed");
  Serial.printf("KITSU_ACTIVITY finish=%s score=%u/%u\n",
                kitsu868::activities::activityName(view.kind), view.score,
                maximum);
}

void tickActivity(uint32_t now) {
  if (!activityStateReady) return;
  const kitsu868::activities::ActivityState before = activitySuite.snapshot();
  if (!rawButton && !stableButton) activitySuite.tick(now);
  const kitsu868::activities::ActivityView view = activitySuite.view(now);
  rewardFinishedActivity(view);
  const kitsu868::activities::ActivityState after = activitySuite.snapshot();
  if (memcmp(&before, &after, sizeof(after)) != 0 &&
      !persistActivityState()) {
    Serial.println("KITSU_WARN activity_state=flush_failed");
  }
}

void tickFocus(uint32_t now) {
  if (!focusStateReady ||
      static_cast<int32_t>(now - focusNextTickAt) < 0) {
    return;
  }
  focusNextTickAt = now + FOCUS_TICK_INTERVAL_MS;

  const kitsu868::focus::Phase before = focusSession.phase();
  kitsu868::focus::Update update{};
  const kitsu868::focus::Status status =
      focusSession.tick(focusClock(now), update);
  if (status != kitsu868::focus::Status::Ok) return;

  const kitsu868::focus::View view = focusSession.view();
  const uint32_t checkpoint = view.elapsedMs / FOCUS_CHECKPOINT_MS;
  const bool phaseChanged = before != view.phase;
  const bool retryDue = focusPersistPending &&
      static_cast<int32_t>(now - focusPersistRetryAt) >= 0;
  if ((phaseChanged || checkpoint != focusPersistedCheckpoint || retryDue) &&
      update.changed) {
    if (persistFocusState()) {
      focusPersistedCheckpoint = checkpoint;
      focusPersistPending = false;
    } else {
      focusPersistPending = true;
      focusPersistRetryAt = now + 5000UL;
      Serial.println("KITSU_WARN focus_state=flush_failed");
    }
  } else if (retryDue) {
    if (persistFocusState()) {
      focusPersistedCheckpoint = checkpoint;
      focusPersistPending = false;
    } else {
      focusPersistRetryAt = now + 5000UL;
    }
  }

  if ((update.focusCompleted || update.sessionCompleted) &&
      !momentView.active) {
    momentView.active = true;
    momentView.line1 = view.prompt.title;
    momentView.line2 = view.prompt.detail;
    momentView.until = now + MOMENT_DISPLAY_MS;
    wakeDisplay();
  }
  if (phaseChanged) companionBleRefreshDirty = true;
}

void scheduleRareReaction(uint32_t now) {
  nextRareReactionAt = now + 20UL * 60UL * 1000UL +
      (esp_random() % (20UL * 60UL * 1000UL));
}

void tickFun(uint32_t now) {
  if (momentView.active && static_cast<int32_t>(now - momentView.until) >= 0) {
    momentView = MomentView{};
  }
  if (nextRareReactionAt == 0U) scheduleRareReaction(now);
  if (static_cast<int32_t>(now - nextRareReactionAt) < 0) return;
  uint32_t quietDay = 0U;
  uint16_t quietMinute = 0U;
  const bool quiet = activityStateReady &&
      currentLocalDayMinute(quietDay, quietMinute, true) &&
      activitySuite.quietAt(quietMinute);
  if ((companionBrain.unlockMask() & kitsu868::UnlockRareReaction) == 0U ||
      screen != Screen::Pet || wisp.sleeping || radioListening ||
      activeGame != ActiveGame::None || momentView.active || quiet) {
    nextRareReactionAt = now + 5UL * 60UL * 1000UL;
    return;
  }
  kitsu868::fun::recordRareReaction(funDiscovery);
  if (funStateReady && !persistFunState()) {
    Serial.println("KITSU_WARN rare_reaction=store_failed");
  }
  showMoment(kitsu868::fun::MomentTrigger::RareAmbient, true);
  scheduleRareReaction(now);
  companionBleRefreshDirty = true;
}

uint8_t addClampedStat(uint8_t value, int8_t delta) {
  const int16_t next = static_cast<int16_t>(value) + delta;
  return static_cast<uint8_t>(next < 0 ? 0 : next > 100 ? 100 : next);
}

kitsu868::expedition::ClockSample expeditionClock(uint32_t now) {
  kitsu868::expedition::ClockSample sample{};
  sample.bootId = wisp.boots == 0U ? 1U : wisp.boots;
  sample.monotonicMillis = now;
  kitsu868::timekeeping::ClockReading reading{};
  if (clockReading(now, reading, true)) {
    sample.unixValid = 1U;
    sample.unixSeconds = reading.unixSeconds;
  }
  return sample;
}

kitsu868::adventure::ClockSample adventureClock(uint32_t now) {
  kitsu868::adventure::ClockSample sample{};
  kitsu868::timekeeping::ClockReading reading{};
  if (!clockReading(now, reading, true)) return sample;
  const int64_t localSeconds = static_cast<int64_t>(reading.unixSeconds) +
      static_cast<int64_t>(reading.utcOffsetMinutes) * 60LL;
  if (localSeconds < 0) return sample;
  sample.unixSeconds = reading.unixSeconds;
  sample.dayId = static_cast<uint32_t>(localSeconds / 86400LL);
  sample.minuteOfDay = static_cast<uint16_t>(
      (localSeconds % 86400LL) / 60LL);
  sample.trusted = 1U;
  return sample;
}

bool persistAdventureMutation(
    const kitsu868::adventure::ProgressState& before,
    const char* operation) {
  if (persistAdventureProgression()) return true;
  (void)adventureProgression.restore(before);
  Serial.printf("KITSU_ERROR adventure=%s_store_failed\n",
                operation ? operation : "state");
  return false;
}

bool beginAdventureRoute(bool commuteSafe = false) {
  if (!adventureProgressionReady || !requireCompanion()) return false;
  kitsu868::adventure::RouteRequest request{};
  request.terrain = adventureTerrain;
  request.objective = adventureObjective;
  request.risk = adventureRisk;
  request.personality = companionBrain.personality().kind;
  request.weather = adventureWeather;
  request.baseTargetSteps = 1000U;
  request.commuteSafe = commuteSafe ? 1U : 0U;
  const kitsu868::adventure::ProgressState before =
      adventureProgression.snapshot();
  const kitsu868::adventure::Status status = adventureProgression.begin(
      request, adventureClock(millis()),
      esp_random() ^ companionBrain.deviceFingerprint());
  if (status != kitsu868::adventure::Status::Ok) {
    Serial.printf("KITSU_ADVENTURE start=rejected error=%s\n",
                  kitsu868::adventure::statusName(status));
    if (status == kitsu868::adventure::Status::InvalidClock) {
      beginClockEditor();
    }
    return false;
  }
  if (!persistAdventureMutation(before, "start")) return false;
  momentView.active = true;
  momentView.line1 = "ROUTE STARTED";
  momentView.line2 = "TAKE IT WITH YOU";
  momentView.until = millis() + MOMENT_DISPLAY_MS;
  Serial.printf("KITSU_ADVENTURE start=ok route=%lu terrain=%s objective=%s risk=%s\n",
                static_cast<unsigned long>(adventureProgression.view().routeId),
                terrainLabel(adventureTerrain), objectiveLabel(adventureObjective),
                riskLabel(adventureRisk));
  adventureMenuIndex = 4U;
  return true;
}

bool applyAdventureDecision(kitsu868::adventure::MidDecision decision) {
  const kitsu868::adventure::ProgressState before =
      adventureProgression.snapshot();
  const kitsu868::adventure::Status status =
      adventureProgression.decide(decision);
  if (status != kitsu868::adventure::Status::Ok) {
    Serial.printf("KITSU_ADVENTURE decision=rejected error=%s\n",
                  kitsu868::adventure::statusName(status));
    return false;
  }
  return persistAdventureMutation(before, "decision");
}

void executeAdventureMenuItem() {
  const kitsu868::expedition::ExpeditionView expedition = expeditionCore.view();
  if (expedition.phase == kitsu868::expedition::Phase::Traveling) {
    enterScreen(Screen::Menu);
    return;
  }
  if (expedition.phase == kitsu868::expedition::Phase::Ready) {
    const char* error = nullptr;
    if (!claimExpedition(error)) {
      Serial.printf("KITSU_ERROR expedition=%s\n", error ? error : "claim");
    }
    return;
  }
  const kitsu868::adventure::RouteView route = adventureProgression.view();
  if (route.phase == kitsu868::adventure::RoutePhase::Returned) {
    const kitsu868::adventure::ProgressState before =
        adventureProgression.snapshot();
    const kitsu868::adventure::Status status =
        adventureProgression.acknowledge(route.routeId);
    if (status == kitsu868::adventure::Status::Ok &&
        persistAdventureMutation(before, "acknowledge")) {
      Serial.printf("KITSU_ADVENTURE acknowledged=%lu\n",
                    static_cast<unsigned long>(route.routeId));
    }
    return;
  }
  const char* error = nullptr;
  if (adventureMenuIndex <= 2U) {
    const kitsu868::expedition::Duration duration =
        static_cast<kitsu868::expedition::Duration>(adventureMenuIndex);
    if (!startExpedition(duration, error)) {
      Serial.printf("KITSU_ERROR expedition=%s\n", error ? error : "start");
      if (error && strcmp(error, "clock_unavailable") == 0) {
        beginClockEditor();
      }
    }
  } else if (adventureMenuIndex == 3U) {
    (void)beginAdventureRoute(false);
  } else if (adventureMenuIndex == 4U) {
    const kitsu868::adventure::ProgressState before =
        adventureProgression.snapshot();
    const kitsu868::adventure::Status status =
        adventureProgression.addSteps(250U);
    if (status == kitsu868::adventure::Status::Ok) {
      if (adventureProgression.view().progressPercent >= 100U) {
        (void)adventureProgression.finish(adventureClock(millis()));
      }
      (void)persistAdventureMutation(before, "steps");
    } else {
      Serial.printf("KITSU_ADVENTURE steps=rejected error=%s\n",
                    kitsu868::adventure::statusName(status));
    }
  } else if (adventureMenuIndex >= 5U && adventureMenuIndex <= 8U) {
    const kitsu868::adventure::MidDecision decision =
        static_cast<kitsu868::adventure::MidDecision>(
            adventureMenuIndex - 5U);
    if (applyAdventureDecision(decision) &&
        decision == kitsu868::adventure::MidDecision::ReturnEarly) {
      const kitsu868::adventure::ProgressState beforeFinish =
          adventureProgression.snapshot();
      const kitsu868::adventure::Status finishStatus =
          adventureProgression.finish(adventureClock(millis()));
      if (finishStatus == kitsu868::adventure::Status::Ok ||
          finishStatus == kitsu868::adventure::Status::RescueRequired) {
        (void)persistAdventureMutation(beforeFinish, "finish");
      }
    }
  } else if (adventureMenuIndex == 9U) {
    adventureTerrain = static_cast<kitsu868::adventure::Terrain>(
        (static_cast<uint8_t>(adventureTerrain) + 1U) %
        static_cast<uint8_t>(kitsu868::adventure::Terrain::Count));
  } else if (adventureMenuIndex == 10U) {
    adventureObjective = static_cast<kitsu868::adventure::Objective>(
        (static_cast<uint8_t>(adventureObjective) + 1U) %
        static_cast<uint8_t>(kitsu868::adventure::Objective::Count));
  } else if (adventureMenuIndex == 11U) {
    adventureRisk = static_cast<kitsu868::adventure::Risk>(
        (static_cast<uint8_t>(adventureRisk) + 1U) %
        static_cast<uint8_t>(kitsu868::adventure::Risk::Count));
  } else if (adventureMenuIndex == 12U) {
    adventureWeather = static_cast<kitsu868::adventure::Weather>(
        (static_cast<uint8_t>(adventureWeather) + 1U) %
        static_cast<uint8_t>(kitsu868::adventure::Weather::Count));
  } else if (adventureMenuIndex == 13U) {
    kitsu868::adventure::JournalEntry entry{};
    if (adventureProgression.journalNewest(0U, entry)) {
      kitsu868::adventure::TextPostcard postcard{};
      if (kitsu868::adventure::postcardForId(entry.postcardId, postcard)) {
        momentView.active = true;
        momentView.line1 = postcard.title;
        momentView.line2 = postcard.line;
        momentView.until = millis() + MOMENT_DISPLAY_MS;
      } else {
        momentView.active = true;
        momentView.line1 = "ROUTE REPORT";
        momentView.line2 = "NO POSTCARD";
        momentView.until = millis() + MOMENT_DISPLAY_MS;
      }
      enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
      Serial.printf("KITSU_ADVENTURE_JOURNAL route=%lu day=%lu outcome=%u terrain=%u objective=%u\n",
                    static_cast<unsigned long>(entry.routeId),
                    static_cast<unsigned long>(entry.dayId), entry.outcome,
                    entry.terrain, entry.objective);
    } else {
      Serial.println("KITSU_ADVENTURE_JOURNAL empty=true");
      momentView.active = true;
      momentView.line1 = "NO REPORTS";
      momentView.line2 = "TRY A ROUTE";
      momentView.until = millis() + MOMENT_DISPLAY_MS;
      enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
    }
  } else {
    enterScreen(Screen::Menu);
  }
  screenEnteredAt = millis();
}

bool startExpedition(kitsu868::expedition::Duration duration,
                     const char*& error) {
  error = nullptr;
  if (!companionPack.valid()) {
    error = "companion_unavailable";
    return false;
  }
  if (!expeditionStateReady || !dialogueStateReady) {
    error = "expedition_storage_unavailable";
    return false;
  }
  kitsu868::expedition::StartContext context{};
  context.personality = companionBrain.personality().kind;
  context.mood = companionBrain.mood(companionVitals());
  context.affection = wisp.affection;
  context.companionFingerprint = companionBrain.deviceFingerprint();
  // Expedition reports may revisit a creature already established in the
  // Field Guide. They do not bypass the normal rarity/code unlock path.
  context.eligibleEncounterMask = funStateReady ? funDiscovery.seenMask : 0U;
  const kitsu868::expedition::ExpeditionState before =
      expeditionCore.snapshot();
  const kitsu868::expedition::StartStatus status = expeditionCore.start(
      duration, context, expeditionClock(millis()), esp_random());
  if (status != kitsu868::expedition::StartStatus::Started) {
    error = status == kitsu868::expedition::StartStatus::Busy
                ? "expedition_busy"
                : status == kitsu868::expedition::StartStatus::InvalidClock
                      ? "clock_unavailable"
                      : "expedition_start_failed";
    return false;
  }
  if (!persistExpeditionState()) {
    (void)expeditionCore.restore(before);
    error = "expedition_storage_failed";
    return false;
  }
  lastMemory = String("Expedition started: ") +
      kitsu868::expedition::durationLabel(duration) + ".";
  showActionDialogue(kitsu868::dialogue::Action::Listen,
                     kitsu868::dialogue::ActionOutcome::Success, false,
                     false);
  companionBleRefreshDirty = true;
  Serial.printf("KITSU_EXPEDITION start=%s id=%lu\n",
                kitsu868::expedition::durationLabel(duration),
                static_cast<unsigned long>(expeditionCore.view().expeditionId));
  return true;
}

bool claimExpedition(const char*& error) {
  error = nullptr;
  kitsu868::expedition::CompletionHooks hooks{};
  if (!expeditionStateReady || !expeditionCore.completion(hooks)) {
    error = "expedition_not_ready";
    return false;
  }
  kitsu868::expedition::ExpeditionReport report{};
  const kitsu868::expedition::ExpeditionView readyView = expeditionCore.view();
  if (!kitsu868::expedition::reportForIndex(readyView.reportIndex, report)) {
    error = "expedition_report_invalid";
    return false;
  }

  if (adventureProgressionReady) {
    const kitsu868::adventure::ProgressState adventureBefore =
        adventureProgression.snapshot();
    const kitsu868::adventure::Status adventureStatus =
        adventureProgression.applyExpedition(hooks);
    if ((adventureStatus == kitsu868::adventure::Status::Ok ||
         adventureStatus == kitsu868::adventure::Status::Duplicate) &&
        !persistAdventureProgression()) {
      (void)adventureProgression.restore(adventureBefore);
      error = "adventure_progress_store_failed";
      return false;
    }
  }
  if (companionProgressionReady &&
      lastClaimedExpeditionId != hooks.expeditionId) {
    uint32_t day = 0U;
    uint16_t minute = 0U;
    uint8_t progressionBefore[kitsu868::progression::kSnapshotCapacity]{};
    const bool progressionCaptured =
        captureCompanionProgression(progressionBefore);
    const bool memoryClockReady =
        currentLocalDayMinute(day, minute, true);
    if (memoryClockReady && !progressionCaptured) {
      error = "expedition_memory_snapshot_failed";
      return false;
    }
    if (memoryClockReady &&
        companionProgression.rememberExpedition(
            readyView.reportIndex, day) &&
        !persistCompanionProgression()) {
      restoreCompanionProgression(progressionBefore);
      error = "expedition_memory_store_failed";
      return false;
    }
  }

  if (lastClaimedExpeditionId != hooks.expeditionId) {
    const uint8_t previousAffection = wisp.affection;
    const uint8_t previousCuriosity = wisp.curiosity;
    const kitsu868::fun::DiscoveryState previousDiscovery = funDiscovery;
    const uint32_t previousClaimedId = lastClaimedExpeditionId;
    bool discoveryChanged = false;
    wisp.affection = addClampedStat(wisp.affection, hooks.affectionDelta);
    if (hooks.personalityDelta > 0) {
      if (hooks.personalityAxis ==
              kitsu868::expedition::PersonalityAxis::Warmth) {
        wisp.affection = addClampedStat(wisp.affection,
                                        hooks.personalityDelta);
      } else {
        wisp.curiosity = addClampedStat(wisp.curiosity,
                                        hooks.personalityDelta);
      }
    }
    if (hooks.hasEncounter() && funStateReady &&
        !kitsu868::fun::recordCreatureEncounter(
            funDiscovery, hooks.encounterCatalogIndex, 9U)) {
      wisp.affection = previousAffection;
      wisp.curiosity = previousCuriosity;
      funDiscovery = previousDiscovery;
      error = "expedition_encounter_failed";
      return false;
    }
    discoveryChanged = hooks.hasEncounter() && funStateReady;
    const bool coreStored = saveState();
    const bool discoveryStored = coreStored &&
        (!discoveryChanged || persistFunState());
    if (!discoveryStored) {
      wisp.affection = previousAffection;
      wisp.curiosity = previousCuriosity;
      funDiscovery = previousDiscovery;
      if (coreStored && !saveState()) {
        Serial.println("KITSU_WARN expedition_core_rollback=failed");
      }
      if (discoveryChanged && funStateReady && !persistFunState()) {
        Serial.println("KITSU_WARN expedition_guide_rollback=failed");
      }
      error = "expedition_reward_store_failed";
      return false;
    }
    lastClaimedExpeditionId = hooks.expeditionId;
    if (!persistDialogueState()) {
      lastClaimedExpeditionId = previousClaimedId;
      wisp.affection = previousAffection;
      wisp.curiosity = previousCuriosity;
      funDiscovery = previousDiscovery;
      if (!saveState()) {
        Serial.println("KITSU_WARN expedition_core_rollback=failed");
      }
      if (discoveryChanged && funStateReady && !persistFunState()) {
        Serial.println("KITSU_WARN expedition_guide_rollback=failed");
      }
      if (!persistDialogueState()) {
        Serial.println("KITSU_WARN expedition_receipt_rollback=failed");
      }
      error = "expedition_receipt_store_failed";
      return false;
    }
  }

  const kitsu868::expedition::ExpeditionState beforeAcknowledge =
      expeditionCore.snapshot();
  if (expeditionCore.acknowledge(hooks.expeditionId) !=
          kitsu868::expedition::AcknowledgeStatus::Acknowledged) {
    error = "expedition_ack_failed";
    return false;
  }
  if (!persistExpeditionState()) {
    (void)expeditionCore.restore(beforeAcknowledge);
    error = "expedition_ack_failed";
    return false;
  }
  if (dialogueStories.activeStory == kitsu868::dialogue::kNoActiveStory) {
    const kitsu868::dialogue::StoryState beforeStory = dialogueStories;
    kitsu868::dialogue::StoryBeat ignored{};
    if (kitsu868::dialogue::startStory(
            kitsu868::dialogue::StoryTrigger::ExpeditionReturn,
            companionBrain.personality().kind,
            companionBrain.deviceFingerprint(), dialogueStories, ignored)) {
      if (persistDialogueState()) {
        storyResolutionAvailable = false;
      } else {
        dialogueStories = beforeStory;
        Serial.println("KITSU_WARN expedition_story=store_failed");
      }
    }
  }
  momentView.active = true;
  momentView.line1 = report.headline;
  momentView.line2 = report.detail;
  momentView.until = millis() + MOMENT_DISPLAY_MS;
  lastMemory = String(report.headline) + ": " + report.detail;
  companionBleRefreshDirty = true;
  Serial.printf("KITSU_EXPEDITION claim=%lu report=%u\n",
                static_cast<unsigned long>(hooks.expeditionId),
                static_cast<unsigned>(readyView.reportIndex));
  return true;
}

void tickExpedition(uint32_t now) {
  if (!expeditionStateReady ||
      expeditionCore.view().phase != kitsu868::expedition::Phase::Traveling) {
    return;
  }
  const kitsu868::expedition::ExpeditionState beforePoll =
      expeditionCore.snapshot();
  const kitsu868::expedition::PollStatus status =
      expeditionCore.poll(expeditionClock(now));
  static uint32_t lastCheckpointAt = 0U;
  if (status == kitsu868::expedition::PollStatus::BecameReady ||
      (status == kitsu868::expedition::PollStatus::Progressed &&
       (lastCheckpointAt == 0U || now - lastCheckpointAt >= 60000UL))) {
    if (!persistExpeditionState()) {
      (void)expeditionCore.restore(beforePoll);
      Serial.println("KITSU_WARN expedition_state=flush_failed");
      return;
    }
    lastCheckpointAt = now;
  }
  if (status == kitsu868::expedition::PollStatus::BecameReady) {
    kitsu868::expedition::ExpeditionReport report{};
    const kitsu868::expedition::ExpeditionView view = expeditionCore.view();
    if (kitsu868::expedition::reportForIndex(view.reportIndex, report)) {
      momentView.active = true;
      momentView.line1 = "EXPEDITION";
      momentView.line2 = "READY TO CLAIM";
      momentView.until = now + MOMENT_DISPLAY_MS;
    }
    companionBleRefreshDirty = true;
    Serial.printf("KITSU_EXPEDITION ready id=%lu\n",
                  static_cast<unsigned long>(view.expeditionId));
  }
}

void leaveGame(bool celebrate) {
  if (activeGame == ActiveGame::SignalCatch) signalCatchGame.cancel();
  else if (activeGame == ActiveGame::PounceFetch) pounceFetchGame.cancel();
  else if (activeGame == ActiveGame::EchoBeat) echoBeatGame.cancel();
  activeGame = ActiveGame::None;
  lastGamePhase = kitsu868::MiniGamePhase::Inactive;
  if (celebrate || gameEvolved) {
    cancelAmbientAnimation();
    if (!startTransientAnimation(gameEvolved ? CompanionRole::Evolve
                                             : CompanionRole::Play)) {
      startBaseAnimation();
    }
  }
  enterScreen(Screen::Pet);
}

void stopListening() {
  radioListening = false;
  listenUntil = 0;
  nextNearbyPresenceAt = 0U;
  if (radioProgressDirty) {
    persistProgress();
    radioProgressDirty = false;
  }
  if (!activeAnimation.active || !activeAnimation.finite) startBaseAnimation();
  if (screen == Screen::Listen) enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
  Serial.println("KITSU_RADIO listening=false");
}

bool startListening(uint32_t durationMs) {
  if (!requireCompanion()) return false;
  if (partyRuntimeBusy() || partyScanActive || partyJoinRequested) {
    Serial.println("KITSU_ERROR busy=party");
    return false;
  }
  if (activeGame != ActiveGame::None) {
    Serial.println("KITSU_ERROR busy=game");
    return false;
  }
  if (activityRuntimeBusy()) {
    Serial.println("KITSU_ERROR busy=activity");
    return false;
  }
  if (ensureAwake()) persistProgress();
  if (!radioReady) {
    lastMemory = "The nearby radio did not answer.";
    enterScreen(Screen::Status);
    return false;
  }

  radioListening = true;
  listenUntil = millis() + durationMs;
  nextNearbyPresenceAt = 0U;
  (void)sendNearbyPresence();
  if (pendingEvolutionReaction) {
    pendingEvolutionReaction = false;
    cancelAmbientAnimation();
    if (!startTransientAnimation(CompanionRole::Evolve)) startBaseAnimation();
  } else {
    startBaseAnimation();
  }
  enterScreen(Screen::Listen);
  showActionDialogue(kitsu868::dialogue::Action::Listen,
                     kitsu868::dialogue::ActionOutcome::Success, false,
                     false);
  Serial.println("KITSU_RADIO listening=true nearby=true meshcore=false");
  return true;
}

void executeMenuItem() {
  switch (menuIndex) {
    case 0:
      connectionAction = ConnectionAction::Bluetooth;
      enterScreen(Screen::Connect);
      break;
    case 1:
      if (!requireCompanion()) break;
      feedKitsu();
      enterScreen(Screen::Pet);
      break;
    case 2:
      if (!requireCompanion()) break;
      playKitsu();
      enterScreen(Screen::Pet);
      break;
    case 3:
      gameMenuIndex = 0;
      enterScreen(Screen::GameMenu);
      break;
    case 4:
      adventureMenuIndex =
          (adventureProgression.view().phase ==
               kitsu868::adventure::RoutePhase::Active ||
           adventureProgression.view().phase ==
               kitsu868::adventure::RoutePhase::AwaitingRescue)
              ? 4U
              : 0U;
      enterScreen(Screen::Adventure);
      break;
    case 5:
      fieldGuideIndex = 0U;
      enterScreen(Screen::FieldGuide);
      break;
    case 6:
      enterScreen(Screen::Goals);
      break;
    case 7:
      inboxSelection = 0;
      markChatJournalRead();
      enterScreen(Screen::Inbox);
      break;
    case 8: startListening(); break;
    case 9:
      beginClockEditor();
      break;
    case 10:
      if (!requireCompanion()) break;
      setSleeping(true);
      enterScreen(Screen::Sleep);
      break;
    case 11:
      statusPage = 0;
      sampleBattery(true);
      enterScreen(Screen::Status);
      break;
    default: enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet); break;
  }
}

bool openBluetoothControl(
    uint32_t now, kitsu868::connectivity::ControllerRole role) {
  if (!companionBle.ready()) return false;
  pairingScreenRole = role;
  const kitsu868::connectivity::BleLinkStatus link =
      companionBle.linkStatus(now);
  if (!link.connected && !companionBle.openPairing(now, role)) {
    if (companionBle.pairingStorageBlocked()) {
      enterScreen(Screen::PairPhone);
    }
    return false;
  }
  enterScreen(Screen::PairPhone);
  return true;
}

void executeConnectionAction() {
  const uint32_t now = millis();
  if (connectionAction == ConnectionAction::Back) {
    enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
    return;
  }
  if (connectionAction == ConnectionAction::Bluetooth) {
    const bool opened = openBluetoothControl(
        now, kitsu868::connectivity::ControllerRole::Owner);
    Serial.printf("KITSU_CONNECT_ACTION action=bluetooth result=%s\n",
                  opened ? "opened" : "unavailable");
  } else if (connectionAction == ConnectionAction::PairCaretaker) {
    const bool opened = openBluetoothControl(
        now, kitsu868::connectivity::ControllerRole::Caretaker);
    Serial.printf("KITSU_CONNECT_ACTION action=pair_caretaker result=%s\n",
                  opened ? "opened" : "unavailable");
  } else if (connectionAction == ConnectionAction::Controllers) {
    beginControllerRecovery(now);
    Serial.println("KITSU_CONNECT_ACTION action=controllers result=opened");
  }
}

void executeGameMenuItem() {
  if (gameMenuIndex == 0) startGame(ActiveGame::SignalCatch);
  else if (gameMenuIndex == 1) startGame(ActiveGame::PounceFetch);
  else if (gameMenuIndex == 2) startGame(ActiveGame::EchoBeat);
  else if (gameMenuIndex >= 3U && gameMenuIndex <= 7U) {
    startActivity(static_cast<kitsu868::activities::ActivityKind>(
                      gameMenuIndex - 2U), false, false);
  } else if (gameMenuIndex == 8U) {
    startActivity(kitsu868::activities::ActivityKind::MorseSignal, true,
                  false);
  } else if (gameMenuIndex == 9U) {
    startActivity(kitsu868::activities::ActivityKind::MorseSignal, false,
                  true);
  }
  else enterScreen(Screen::Menu);
}

void handleShortPress(uint32_t actionAt) {
  switch (screen) {
    case Screen::Pet: executeQuickAction(); break;
    case Screen::Menu:
      menuIndex = (menuIndex + 1) % (sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]));
      screenEnteredAt = millis();
      break;
    case Screen::Connect:
      connectionAction = static_cast<ConnectionAction>(
          (static_cast<uint8_t>(connectionAction) + 1U) % 4U);
      screenEnteredAt = millis();
      break;
    case Screen::Inbox:
      if (chatJournalCount > 1U) {
        inboxSelection = static_cast<uint8_t>(
            (inboxSelection + 1U) % chatJournalCount);
      }
      screenEnteredAt = millis();
      break;
    case Screen::GameMenu:
      gameMenuIndex =
          (gameMenuIndex + 1) % (sizeof(GAME_ITEMS) / sizeof(GAME_ITEMS[0]));
      screenEnteredAt = millis();
      break;
    case Screen::Game:
      if (activeGameFinished()) {
        leaveGame(gameRewarded);
      } else if (activeGame == ActiveGame::SignalCatch) {
        signalCatchGame.tap(actionAt);
      } else if (activeGame == ActiveGame::PounceFetch) {
        pounceFetchGame.tap(actionAt);
      } else if (activeGame == ActiveGame::EchoBeat) {
        echoBeatGame.tap(actionAt);
      }
      break;
    case Screen::Listen: stopListeningSafely(); break;
    case Screen::Sleep:
      setSleeping(false);
      enterScreen(Screen::Pet);
      break;
    case Screen::Status:
      statusPage = (statusPage + 1U) % 6U;
      screenEnteredAt = millis();
      break;
    case Screen::FieldGuide:
      fieldGuideIndex = static_cast<uint8_t>(
          (fieldGuideIndex + 1U) % kitsu868::fun::kCatalogCreatureCount);
      screenEnteredAt = millis();
      break;
    case Screen::Goals:
      screenEnteredAt = millis();
      break;
    case Screen::Clock:
      (void)clockEditor.shortPress();
      screenEnteredAt = millis();
      break;
    case Screen::Adventure:
      if (expeditionCore.view().phase ==
              kitsu868::expedition::Phase::Traveling ||
          expeditionCore.view().phase == kitsu868::expedition::Phase::Ready ||
          adventureProgression.view().phase ==
              kitsu868::adventure::RoutePhase::Returned) {
        enterScreen(Screen::Menu);
      } else {
        const kitsu868::adventure::RoutePhase routePhase =
            adventureProgression.view().phase;
        if (routePhase == kitsu868::adventure::RoutePhase::Active ||
            routePhase == kitsu868::adventure::RoutePhase::AwaitingRescue) {
          adventureMenuIndex = adventureMenuIndex < 4U ||
                                   adventureMenuIndex >= 8U
                               ? 4U
                               : static_cast<uint8_t>(adventureMenuIndex + 1U);
        } else {
          adventureMenuIndex = static_cast<uint8_t>(
              (adventureMenuIndex + 1U) %
              (sizeof(ADVENTURE_ITEMS) / sizeof(ADVENTURE_ITEMS[0])));
        }
      }
      screenEnteredAt = millis();
      break;
    case Screen::Activity: {
      const kitsu868::activities::ActivityView view =
          activitySuite.view(millis());
      if (view.phase == kitsu868::activities::ActivityPhase::Finished ||
          view.phase == kitsu868::activities::ActivityPhase::Idle) {
        leaveActivity();
      } else {
        const kitsu868::activities::ActivityState before =
            activitySuite.snapshot();
        const kitsu868::activities::InputResult result =
            activitySuite.tap(actionAt);
        if (result != kitsu868::activities::InputResult::Ignored &&
            result != kitsu868::activities::InputResult::Invalid) {
          (void)persistActivityMutation(before, "tap");
        }
      }
      break;
    }
    case Screen::WildEncounter: dismissWildEncounter(); break;
    case Screen::PairPhone:
      companionBle.closePairing();
      enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
      break;
    case Screen::ControllerManager:
      if (!connectivitySecurityReady ||
          !controllerRecoveryBleDisconnected(millis())) {
        leaveControllerRecovery();
      } else {
        controllerRecoverySelection = static_cast<uint8_t>(
            (controllerRecoverySelection + 1U) %
            CONTROLLER_RECOVERY_OPTION_COUNT);
        controllerRecoveryDeadline =
            millis() + CONTROLLER_RECOVERY_BROWSE_TIMEOUT_MS;
        screenEnteredAt = millis();
      }
      break;
    case Screen::ControllerConfirm:
      showControllerManager(millis(), false);
      Serial.println("KITSU_CONTROLLER_RECOVERY confirmation=cancelled");
      break;
    case Screen::ControllerResult:
      if (!controllerRecoveryRequiresReboot()) {
        showControllerManager(millis(), false);
      }
      break;
  }
}

void handleLongPress() {
  switch (screen) {
    case Screen::Pet:
      menuIndex = 0;
      enterScreen(Screen::Menu);
      break;
    case Screen::Menu: executeMenuItem(); break;
    case Screen::Connect: executeConnectionAction(); break;
    case Screen::Inbox:
      enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
      break;
    case Screen::GameMenu: executeGameMenuItem(); break;
    case Screen::Game: leaveGame(gameRewarded); break;
    case Screen::Listen: stopListeningSafely(); break;
    case Screen::Sleep:
      setSleeping(false);
      enterScreen(Screen::Pet);
      break;
    case Screen::Status: enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet); break;
    case Screen::FieldGuide:
    case Screen::Goals:
      enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
      break;
    case Screen::Clock: {
      const kitsu868::timekeeping::ClockEditorEvent event =
          clockEditor.longPress();
      if (event == kitsu868::timekeeping::ClockEditorEvent::CommitRequested) {
        (void)commitClockEditor();
      }
      screenEnteredAt = millis();
      break;
    }
    case Screen::Adventure: executeAdventureMenuItem(); break;
    case Screen::Activity: leaveActivity(); break;
    case Screen::WildEncounter: dismissWildEncounter(); break;
    case Screen::PairPhone: {
      const uint32_t now = millis();
      const kitsu868::connectivity::BleLinkStatus link =
          companionBle.linkStatus(now);
      const kitsu868::connectivity::BleSessionStatus session =
          companionBle.sessionStatus(now);
      bool accepted = false;
      if (link.numericComparisonPending) {
        accepted = companionBle.confirmNumeric();
      } else if (session.physicalConfirmationPending) {
        accepted = companionBle.confirmController(now);
      } else if (!link.pairingWindowOpen && !link.connected) {
        accepted = companionBle.openPairing(now, pairingScreenRole);
      }
      Serial.printf("KITSU_PAIR physical_action=%s\n",
                    accepted ? "accepted" : "ignored");
      screenEnteredAt = now;
      break;
    }
    case Screen::ControllerManager: {
      const uint32_t now = millis();
      if (!connectivitySecurityReady ||
          !controllerRecoveryBleDisconnected(now)) {
        break;
      }
      if (controllerRecoverySelection == CONTROLLER_RECOVERY_BACK_INDEX) {
        leaveControllerRecovery();
      } else {
        const bool armed = armControllerRecovery(
            now, controllerRecoverySelection);
        Serial.printf("KITSU_CONTROLLER_RECOVERY selection=%u armed=%s\n",
                      static_cast<unsigned>(controllerRecoverySelection),
                      armed ? "true" : "false");
        if (!armed) {
          controllerRecoveryDeadline =
              now + CONTROLLER_RECOVERY_BROWSE_TIMEOUT_MS;
        }
      }
      break;
    }
    case Screen::ControllerConfirm:
      // A partial hold is intentionally inert. Only the continuous five-second
      // service below may dispatch the destructive operation; tap cancels.
      break;
    case Screen::ControllerResult:
      if (!controllerRecoveryRequiresReboot()) {
        showControllerManager(millis(), false);
      }
      break;
  }
}

void pollButton() {
  const bool current = digitalRead(PIN_BUTTON) == LOW;
  if (current != rawButton) {
    rawButton = current;
    buttonChangedAt = millis();
    if (current) screenEnteredAt = buttonChangedAt;
  }
  if (millis() - buttonChangedAt < BUTTON_DEBOUNCE_MS || current == stableButton) return;

  stableButton = current;
  if (stableButton) {
    wakeDisplay();
    buttonHoldConsumed = false;
    buttonPressedAt = millis();
    if (screen == Screen::Activity) {
      const kitsu868::activities::ActivityState before =
          activitySuite.snapshot();
      const kitsu868::activities::InputResult result =
          activitySuite.press(buttonPressedAt);
      activityPressAccepted =
          result == kitsu868::activities::InputResult::Accepted;
      if (result != kitsu868::activities::InputResult::Ignored &&
          result != kitsu868::activities::InputResult::Invalid &&
          !persistActivityMutation(before, "press")) {
        activityPressAccepted = false;
      }
    }
    Serial.println("KITSU_BUTTON pressed");
  } else {
    const uint32_t duration = millis() - buttonPressedAt;
    Serial.printf("KITSU_BUTTON released duration_ms=%lu\n",
                  static_cast<unsigned long>(duration));
    if (screen == Screen::Activity && activityPressAccepted) {
      const kitsu868::activities::ActivityState before =
          activitySuite.snapshot();
      const kitsu868::activities::InputResult result =
          activitySuite.release(millis());
      activityPressAccepted = false;
      if (result != kitsu868::activities::InputResult::Ignored &&
          result != kitsu868::activities::InputResult::Invalid) {
        (void)persistActivityMutation(before, "release");
      }
    } else if (buttonHoldConsumed) {
      buttonHoldConsumed = false;
    } else if (duration >= BUTTON_HOLD_MS) {
      handleLongPress();
    } else {
      handleShortPress(buttonPressedAt);
    }
  }
}

void serviceControllerRecovery(uint32_t now) {
  if (screen != Screen::ControllerManager &&
      screen != Screen::ControllerConfirm &&
      screen != Screen::ControllerResult) {
    return;
  }

  if (!controllerRecoveryBleDisconnected(now)) {
    companionBle.disconnectForLocalControllerRecovery();
    if (screen == Screen::ControllerConfirm) {
      showControllerManager(now, false);
      Serial.println(
          "KITSU_CONTROLLER_RECOVERY confirmation=cancelled_ble_link");
    }
    return;
  }

  if (controllerRecoveryDeadlineReached(now, controllerRecoveryDeadline)) {
    if (screen == Screen::ControllerResult) {
      leaveControllerRecovery();
    } else if (screen == Screen::ControllerConfirm) {
      showControllerManager(now, false);
      Serial.println(
          "KITSU_CONTROLLER_RECOVERY confirmation=expired");
    } else {
      leaveControllerRecovery();
    }
    return;
  }

  if (screen == Screen::ControllerConfirm && stableButton &&
      !buttonHoldConsumed &&
      now - buttonPressedAt >= CONTROLLER_RECOVERY_HOLD_MS) {
    buttonHoldConsumed = true;
    commitControllerRecovery(now);
  }
}

uint16_t ownUidSuffix() {
  return static_cast<uint16_t>(strtoul(wisp.uid.c_str() + 2, nullptr, 16));
}

bool partyPhaseRunning(kitsu868::party::SessionPhase phase) {
  using kitsu868::party::SessionPhase;
  return phase == SessionPhase::Round1 || phase == SessionPhase::Round2 ||
      phase == SessionPhase::Round3;
}

bool partyPhaseInProgress(kitsu868::party::SessionPhase phase) {
  using kitsu868::party::SessionPhase;
  return phase == SessionPhase::Joining || phase == SessionPhase::Lobby ||
      partyPhaseRunning(phase);
}

bool modePartyHostInProgress(kitsu868::party_modes::HostPhase phase) {
  using kitsu868::party_modes::HostPhase;
  return phase == HostPhase::Lobby || phase == HostPhase::Active;
}

bool modePartyGuestInProgress(
    kitsu868::party_modes::ParticipantPhase phase) {
  using kitsu868::party_modes::ParticipantPhase;
  return phase == ParticipantPhase::Observed ||
      phase == ParticipantPhase::Joining || phase == ParticipantPhase::Lobby ||
      phase == ParticipantPhase::Active;
}

bool partyRuntimeBusy() {
  return partyScanActive || partyJoinRequested || partyWelcomeAccepted ||
      modePartyScanActive || modePartyJoinRequested ||
      modePartyAutoReadyPending ||
      partyPhaseInProgress(static_cast<kitsu868::party::SessionPhase>(
             partyHost.state().phase)) ||
      partyPhaseInProgress(static_cast<kitsu868::party::SessionPhase>(
             partyParticipant.state().phase)) ||
      modePartyHostInProgress(
          static_cast<kitsu868::party_modes::HostPhase>(
              modePartyHost.state().phase)) ||
      modePartyGuestInProgress(
          static_cast<kitsu868::party_modes::ParticipantPhase>(
              modePartyParticipant.state().phase));
}

void clearPartyReplay() {
  partyReplayPacket = kitsu868::party::Packet{};
  partyReplayRemaining = 0U;
  partyReplayAt = 0U;
}

void clearModePartyReplay() {
  modePartyReplayPacket = kitsu868::party_modes::Packet{};
  modePartyReplayRemaining = 0U;
  modePartyReplayAt = 0U;
}

void resetModePartyRuntimeView() {
  modePartyScanActive = false;
  modePartyJoinRequested = false;
  modePartyAutoReadyPending = false;
  lastModePartyBeaconTxAt = 0U;
  lastModePartyTxAt = 0U;
  clearModePartyReplay();
}

void resetPartyRuntimeView() {
  lastPartyBeaconAt = 0U;
  lastPartyBeaconTxAt = 0U;
  lastPartyTxAt = 0U;
  lastPartyRewardAttemptAt = 0U;
  lastPartyRewardSessionNonce = 0U;
  lastPartyCelebratedSessionNonce = 0U;
  partyScanActive = false;
  partyJoinRequested = false;
  partyWelcomeAccepted = false;
  partyRewardAvailable = false;
  lastPartyReward = kitsu868::party::RewardOutcome{};
  partyHostRssi = 0.0f;
  partyHostSnr = 0.0f;
  clearPartyReplay();
}

bool activatePartyListening() {
  if (!radioListening) return startListening(PARTY_LISTEN_TIME_MS);
  if (!radioReady) return false;
  const uint32_t now = millis();
  const uint32_t remaining =
      static_cast<int32_t>(listenUntil - now) > 0 ? listenUntil - now : 0U;
  if (remaining < PARTY_LISTEN_TIME_MS) {
    listenUntil = now + PARTY_LISTEN_TIME_MS;
  }
  return true;
}

bool sendPartyPacketNow(const kitsu868::party::Packet& packet) {
  uint8_t wire[kitsu868::party::kWireBytes]{};
  size_t wireBytes = 0U;
  const kitsu868::party::Status encoded = kitsu868::party::encode(
      packet, wire, sizeof(wire), &wireBytes);
  if (encoded != kitsu868::party::Status::Ok) {
    Serial.printf("KITSU_PARTY_TX encode=%s type=%u\n",
                  kitsu868::party::statusName(encoded),
                  static_cast<unsigned>(packet.type));
    return false;
  }
  lastPartyTxAt = millis();
  const kitsu868::mesh::TransportStatus status =
      meshTransport.sendNearbyRadioFrame(meshSettings, wire, wireBytes, true);
  memset(wire, 0, sizeof(wire));
  if (status != kitsu868::mesh::TransportStatus::Ok) {
    Serial.printf("KITSU_PARTY_TX result=%s type=%u\n",
                  kitsu868::mesh::transportStatusName(status),
                  static_cast<unsigned>(packet.type));
    return false;
  }
  Serial.printf("KITSU_PARTY_TX result=ok type=%u sequence=%u\n",
                static_cast<unsigned>(packet.type), packet.sequence);
  return true;
}

void armPartyReplay(const kitsu868::party::Packet& packet) {
  if (packet.type == kitsu868::party::PacketType::Beacon ||
      PARTY_PACKET_REPLAYS == 0U) {
    return;
  }
  partyReplayPacket = packet;
  partyReplayRemaining = PARTY_PACKET_REPLAYS;
  partyReplayAt = millis() + PARTY_REPLAY_INTERVAL_MS;
}

bool sendPartyPacket(const kitsu868::party::Packet& packet,
                     bool replay) {
  if (!sendPartyPacketNow(packet)) return false;
  if (replay) armPartyReplay(packet);
  return true;
}

uint32_t nonZeroPartyEntropy(uint32_t salt) {
  uint32_t value = esp_random() ^ salt ^ millis() ^
      companionBrain.deviceFingerprint();
  return value == 0U ? salt == 0U ? 1U : salt : value;
}

bool startPartyScan(const char*& error) {
  error = nullptr;
  if (partyRuntimeBusy()) {
    error = "party_busy";
    return false;
  }
  if (!activatePartyListening()) {
    error = "party_radio_unavailable";
    return false;
  }
  modePartyHost.reset();
  modePartyParticipant.reset();
  resetModePartyRuntimeView();
  partyHost.reset();
  partyParticipant.reset();
  resetPartyRuntimeView();
  partyScanActive = true;
  lastMemory = "Listening for a nearby party hotspot.";
  companionBleRefreshDirty = true;
  Serial.println("KITSU_PARTY scan=started");
  return true;
}

bool startPartyHost(const char*& error) {
  error = nullptr;
  if (partyRuntimeBusy()) {
    error = "party_busy";
    return false;
  }
  const uint16_t selfUid = ownUidSuffix();
  if (selfUid == 0U) {
    error = "party_identity_unavailable";
    return false;
  }
  if (!activatePartyListening()) {
    error = "party_radio_unavailable";
    return false;
  }
  modePartyHost.reset();
  modePartyParticipant.reset();
  resetModePartyRuntimeView();
  partyHost.reset();
  partyParticipant.reset();
  resetPartyRuntimeView();
  const uint32_t sessionNonce = nonZeroPartyEntropy(UINT32_C(0x5038534e));
  const uint32_t seed = nonZeroPartyEntropy(
      sessionNonce ^ UINT32_C(0x48554e54));
  const kitsu868::party::SessionStatus status = partyHost.start(
      selfUid, sessionNonce, seed, millis(), PARTY_LOBBY_WINDOW_SECONDS,
      PARTY_ROUND_WINDOW_SECONDS);
  if (status != kitsu868::party::SessionStatus::Ok) {
    partyHost.reset();
    error = "party_host_failed";
    Serial.printf("KITSU_PARTY host=%s\n",
                  kitsu868::party::sessionStatusName(status));
    return false;
  }

  // A normal listen start just sent a K8 presence frame, so the first P8
  // beacon may legitimately meet the shared discovery cooldown. Retain the
  // lobby and retry at the bounded beacon interval instead of reporting a
  // hotspot that never existed or spinning on the radio.
  const uint32_t now = millis();
  const kitsu868::party::HostState beforeBeacon = partyHost.snapshot();
  kitsu868::party::Packet beacon{};
  const kitsu868::party::SessionStatus beaconStatus =
      partyHost.makeBeacon(now, beacon);
  lastPartyBeaconTxAt = now;
  if (beaconStatus == kitsu868::party::SessionStatus::Ok &&
      !sendPartyPacket(beacon, false)) {
    (void)partyHost.restore(beforeBeacon);
  }
  lastMemory = "Party hotspot opened. Waiting for friends.";
  companionBleRefreshDirty = true;
  Serial.printf("KITSU_PARTY host=started nonce=%lu\n",
                static_cast<unsigned long>(sessionNonce));
  return true;
}

bool joinObservedParty(uint16_t hostUid, uint32_t sessionNonce,
                       const char*& error) {
  error = nullptr;
  const uint32_t now = millis();
  const kitsu868::party::ParticipantState current =
      partyParticipant.snapshot();
  if (static_cast<kitsu868::party::SessionPhase>(current.phase) !=
          kitsu868::party::SessionPhase::Joining ||
      current.hostUid != hostUid || current.sessionNonce != sessionNonce ||
      lastPartyBeaconAt == 0U ||
      static_cast<uint32_t>(now - lastPartyBeaconAt) >
          PARTY_DISCOVERY_TTL_MS) {
    error = "party_host_not_found";
    return false;
  }
  if (!activatePartyListening()) {
    error = "party_radio_unavailable";
    return false;
  }
  kitsu868::party::Packet request{};
  const kitsu868::party::SessionStatus status =
      partyParticipant.makeJoinRequest(request);
  if (status != kitsu868::party::SessionStatus::Ok) {
    error = "party_join_failed";
    return false;
  }
  if (!sendPartyPacket(request, true)) {
    (void)partyParticipant.restore(current);
    error = "party_send_failed";
    return false;
  }
  // Android uses a non-empty discovered_hosts list as the explicit Join
  // affordance. Clear it only after the JoinRequest is really on air; a
  // failed send leaves the observed hotspot selectable for a retry.
  lastPartyBeaconAt = 0U;
  partyScanActive = false;
  modePartyScanActive = false;
  partyJoinRequested = true;
  partyWelcomeAccepted = false;
  partyRewardAvailable = false;
  lastPartyReward = kitsu868::party::RewardOutcome{};
  lastPartyRewardAttemptAt = 0U;
  lastPartyRewardSessionNonce = 0U;
  lastPartyCelebratedSessionNonce = 0U;
  lastMemory = "Party join request sent.";
  companionBleRefreshDirty = true;
  Serial.printf("KITSU_PARTY join=requested host=%04X nonce=%lu\n",
                hostUid, static_cast<unsigned long>(sessionNonce));
  return true;
}

bool beginHostedParty(const char*& error) {
  error = nullptr;
  const kitsu868::party::HostState before = partyHost.snapshot();
  if (static_cast<kitsu868::party::SessionPhase>(before.phase) !=
      kitsu868::party::SessionPhase::Lobby) {
    error = "party_not_hosting";
    return false;
  }
  if (before.participantCount < kitsu868::party::kMinimumParticipants) {
    error = "party_needs_two";
    return false;
  }
  if (!activatePartyListening()) {
    error = "party_radio_unavailable";
    return false;
  }
  kitsu868::party::Packet roundOpen{};
  const kitsu868::party::SessionStatus status =
      partyHost.beginHunt(millis(), roundOpen);
  if (status != kitsu868::party::SessionStatus::Ok) {
    error = status == kitsu868::party::SessionStatus::NotReady
                ? "party_needs_two"
                : status == kitsu868::party::SessionStatus::TimedOut
                      ? "party_expired"
                      : "party_not_hosting";
    return false;
  }
  if (!sendPartyPacket(roundOpen, true)) {
    (void)partyHost.restore(before);
    error = "party_send_failed";
    return false;
  }
  lastMemory = "Signal hunt started. Choose together.";
  companionBleRefreshDirty = true;
  Serial.println("KITSU_PARTY hunt=started round=1");
  return true;
}

bool choosePartySignal(kitsu868::party::SignalChoice choice,
                       uint8_t round, const char*& error) {
  error = nullptr;
  if (!kitsu868::party::supportedChoice(choice)) {
    error = "party_choice_invalid";
    return false;
  }
  const kitsu868::party::HostState host = partyHost.snapshot();
  const kitsu868::party::SessionPhase hostPhase =
      static_cast<kitsu868::party::SessionPhase>(host.phase);
  if (partyPhaseRunning(hostPhase)) {
    if (host.currentRound != round) {
      error = "party_round_changed";
      return false;
    }
    const kitsu868::party::SessionStatus status =
        partyHost.submitHostChoice(round, choice);
    if (status != kitsu868::party::SessionStatus::Ok) {
      error = status == kitsu868::party::SessionStatus::ChoiceAlreadySubmitted
                  ? "party_choice_already_submitted"
                  : "party_choice_failed";
      return false;
    }
    lastMemory = "Signal choice locked in.";
    companionBleRefreshDirty = true;
    Serial.printf("KITSU_PARTY choice=host round=%u value=%u\n", round,
                  static_cast<unsigned>(choice));
    return true;
  }

  const kitsu868::party::ParticipantState guest =
      partyParticipant.snapshot();
  if (!partyPhaseRunning(
          static_cast<kitsu868::party::SessionPhase>(guest.phase))) {
    error = "party_not_running";
    return false;
  }
  if (guest.currentRound != round) {
    error = "party_round_changed";
    return false;
  }
  kitsu868::party::Packet choicePacket{};
  const kitsu868::party::SessionStatus status =
      partyParticipant.makeChoice(choice, choicePacket);
  if (status != kitsu868::party::SessionStatus::Ok) {
    error = status == kitsu868::party::SessionStatus::ChoiceAlreadySubmitted
                ? "party_choice_already_submitted"
                : "party_choice_failed";
    return false;
  }
  if (!sendPartyPacket(choicePacket, true)) {
    (void)partyParticipant.restore(guest);
    error = "party_send_failed";
    return false;
  }
  lastMemory = "Signal choice sent to the party.";
  companionBleRefreshDirty = true;
  Serial.printf("KITSU_PARTY choice=guest round=%u value=%u\n", round,
                static_cast<unsigned>(choice));
  return true;
}

bool parseModePartyName(const String& name,
                        kitsu868::party_modes::Mode& mode) {
  using kitsu868::party_modes::Mode;
  if (name == "signal" || name == "hunt") mode = Mode::SignalHunt;
  else if (name == "triangulation" || name == "tri") {
    mode = Mode::Triangulation;
  } else if (name == "hotcold" || name == "hot-cold") {
    mode = Mode::HotCold;
  } else if (name == "hide" || name == "hide-seek") {
    mode = Mode::HideAndSeek;
  } else if (name == "sync" || name == "rhythm") {
    mode = Mode::SyncRhythm;
  } else if (name == "echo") {
    mode = Mode::CoopEcho;
  } else if (name == "relay") {
    mode = Mode::AsyncRelay;
  } else if (name == "rare") {
    mode = Mode::RareEncounter;
  } else if (name == "trail") {
    mode = Mode::SharedTrail;
  } else if (name == "story" || name == "vote") {
    mode = Mode::StoryVote;
  } else {
    return false;
  }
  return true;
}

bool sendModePartyPacketNow(const kitsu868::party_modes::Packet& packet) {
  uint8_t wire[kitsu868::party_modes::kWireBytes]{};
  size_t wireBytes = 0U;
  const kitsu868::party_modes::Status encoded =
      kitsu868::party_modes::encode(packet, wire, sizeof(wire), &wireBytes);
  if (encoded != kitsu868::party_modes::Status::Ok) {
    Serial.printf("KITSU_MODE_PARTY_TX encode=%s type=%u\n",
                  kitsu868::party_modes::statusName(encoded),
                  static_cast<unsigned>(packet.type));
    return false;
  }
  lastModePartyTxAt = millis();
  const kitsu868::mesh::TransportStatus status =
      meshTransport.sendNearbyRadioFrame(meshSettings, wire, wireBytes, true);
  memset(wire, 0, sizeof(wire));
  if (status != kitsu868::mesh::TransportStatus::Ok) {
    Serial.printf("KITSU_MODE_PARTY_TX result=%s type=%u\n",
                  kitsu868::mesh::transportStatusName(status),
                  static_cast<unsigned>(packet.type));
    return false;
  }
  Serial.printf("KITSU_MODE_PARTY_TX result=ok type=%u sequence=%u\n",
                static_cast<unsigned>(packet.type), packet.sequence);
  return true;
}

void armModePartyReplay(const kitsu868::party_modes::Packet& packet) {
  if (packet.type == kitsu868::party_modes::PacketType::Beacon ||
      PARTY_PACKET_REPLAYS == 0U) {
    return;
  }
  modePartyReplayPacket = packet;
  modePartyReplayRemaining = PARTY_PACKET_REPLAYS;
  modePartyReplayAt = millis() + PARTY_REPLAY_INTERVAL_MS;
}

bool sendModePartyPacket(const kitsu868::party_modes::Packet& packet,
                         bool replay) {
  if (!sendModePartyPacketNow(packet)) return false;
  if (replay) armModePartyReplay(packet);
  return true;
}

void printModePartyPrompt(kitsu868::party_modes::Mode mode, uint32_t seed,
                          uint8_t round, bool privatePrompt) {
  const kitsu868::party_modes::RoundPrompt prompt =
      kitsu868::party_modes::promptFor(mode, seed, round);
  Serial.printf("KITSU_MODE_PARTY prompt=%s round=%u input=",
                kitsu868::party_modes::modeName(mode), round);
  using kitsu868::party_modes::Mode;
  switch (mode) {
    case Mode::Triangulation:
    case Mode::HotCold:
      Serial.println("rssi_-140_to_0");
      break;
    case Mode::HideAndSeek:
      if (privatePrompt) {
        Serial.printf("slot_0_to_7 hidden=%u\n", prompt.hideSlot);
      } else {
        Serial.println("slot_0_to_7");
      }
      break;
    case Mode::SyncRhythm:
      Serial.printf("tap_offset_ms_0_to_2000 perfect=%u\n",
                    prompt.syncPerfectMs);
      break;
    case Mode::CoopEcho:
      Serial.printf("bit_pattern beats=%u pattern=%u\n", prompt.echoBeats,
                    prompt.echoPattern);
      break;
    case Mode::AsyncRelay:
    case Mode::RareEncounter:
      Serial.println("score_0_to_1000");
      break;
    case Mode::SharedTrail:
      Serial.println("misses_0_to_20");
      break;
    case Mode::StoryVote:
      Serial.println("choice_0_to_2");
      break;
    case Mode::SignalHunt:
    case Mode::Count:
      Serial.println("unsupported");
      break;
  }
}

bool startModePartyScan(const char*& error) {
  error = nullptr;
  if (partyRuntimeBusy()) {
    error = "party_busy";
    return false;
  }
  if (!activatePartyListening()) {
    error = "party_radio_unavailable";
    return false;
  }
  partyHost.reset();
  partyParticipant.reset();
  resetPartyRuntimeView();
  modePartyHost.reset();
  modePartyParticipant.reset();
  resetModePartyRuntimeView();
  modePartyScanActive = true;
  lastMemory = "Scanning for multiplayer hotspots.";
  companionBleRefreshDirty = true;
  Serial.println("KITSU_MODE_PARTY scan=started");
  return true;
}

bool startModePartyHost(kitsu868::party_modes::Mode mode,
                        const char*& error) {
  error = nullptr;
  if (mode == kitsu868::party_modes::Mode::SignalHunt) {
    return startPartyHost(error);
  }
  if (partyRuntimeBusy()) {
    error = "party_busy";
    return false;
  }
  const uint16_t selfUid = ownUidSuffix();
  if (selfUid == 0U) {
    error = "party_identity_unavailable";
    return false;
  }
  if (!activatePartyListening()) {
    error = "party_radio_unavailable";
    return false;
  }
  partyHost.reset();
  partyParticipant.reset();
  resetPartyRuntimeView();
  modePartyHost.reset();
  modePartyParticipant.reset();
  resetModePartyRuntimeView();
  const uint32_t sessionNonce =
      nonZeroPartyEntropy(UINT32_C(0x4D38534E));
  const uint32_t seed =
      nonZeroPartyEntropy(sessionNonce ^ UINT32_C(0x4D4F4445));
  const kitsu868::party_modes::Status status = modePartyHost.start(
      selfUid, sessionNonce, mode, seed, millis(),
      PARTY_LOBBY_WINDOW_SECONDS, PARTY_ROUND_WINDOW_SECONDS);
  if (status != kitsu868::party_modes::Status::Ok ||
      modePartyHost.setHostReady(true) !=
          kitsu868::party_modes::Status::Ok) {
    modePartyHost.reset();
    error = "party_host_failed";
    Serial.printf("KITSU_MODE_PARTY host=%s\n",
                  kitsu868::party_modes::statusName(status));
    return false;
  }
  const uint32_t now = millis();
  const kitsu868::party_modes::HostState before = modePartyHost.snapshot();
  kitsu868::party_modes::Packet beacon{};
  const kitsu868::party_modes::Status beaconStatus =
      modePartyHost.makeBeacon(now, beacon);
  lastModePartyBeaconTxAt = now;
  if (beaconStatus == kitsu868::party_modes::Status::Ok &&
      !sendModePartyPacket(beacon, false)) {
    (void)modePartyHost.restore(before);
  }
  lastMemory = String("Party mode opened: ") +
      kitsu868::party_modes::modeName(mode);
  companionBleRefreshDirty = true;
  Serial.printf("KITSU_MODE_PARTY host=started mode=%s nonce=%lu ready=true\n",
                kitsu868::party_modes::modeName(mode),
                static_cast<unsigned long>(sessionNonce));
  return true;
}

bool startRotatingModePartyHost(const char*& error) {
  uint32_t day = 0U;
  uint16_t minute = 0U;
  if (!currentLocalDayMinute(day, minute, true)) {
    error = "clock_unavailable";
    return false;
  }
  (void)minute;
  const kitsu868::party_modes::Mode mode =
      kitsu868::party_modes::rotatingMode(
          day, companionBrain.deviceFingerprint());
  return startModePartyHost(mode, error);
}

bool startAllPartyScans(const char*& error) {
  if (!startModePartyScan(error)) return false;
  // A serial-only guest cannot know which protocol a nearby host selected.
  // One radio listen window therefore admits both P8 Signal Hunt and M8 mode
  // beacons; joining either one closes the other discovery path.
  partyScanActive = true;
  lastMemory = "Scanning for every party hotspot.";
  Serial.println("KITSU_PARTY scan=all_protocols");
  return true;
}

bool joinObservedModeParty(const char*& error) {
  error = nullptr;
  const uint32_t now = millis();
  const kitsu868::party_modes::ParticipantState before =
      modePartyParticipant.snapshot();
  if (static_cast<kitsu868::party_modes::ParticipantPhase>(before.phase) !=
          kitsu868::party_modes::ParticipantPhase::Observed ||
      before.deadlineMs == 0U ||
      static_cast<int32_t>(now - before.deadlineMs) >= 0) {
    error = "party_host_not_found";
    return false;
  }
  if (!activatePartyListening()) {
    error = "party_radio_unavailable";
    return false;
  }
  kitsu868::party_modes::Packet request{};
  const kitsu868::party_modes::Status status =
      modePartyParticipant.makeJoinRequest(request);
  if (status != kitsu868::party_modes::Status::Ok) {
    error = "party_join_failed";
    return false;
  }
  if (!sendModePartyPacket(request, true)) {
    (void)modePartyParticipant.restore(before);
    error = "party_send_failed";
    return false;
  }
  modePartyScanActive = false;
  partyScanActive = false;
  modePartyJoinRequested = true;
  lastMemory = "Multiplayer join request sent.";
  Serial.printf("KITSU_MODE_PARTY join=requested host=%04X nonce=%lu\n",
                before.hostUid,
                static_cast<unsigned long>(before.sessionNonce));
  return true;
}

bool setModePartyReady(bool ready, const char*& error) {
  error = nullptr;
  const kitsu868::party_modes::HostState host = modePartyHost.snapshot();
  if (static_cast<kitsu868::party_modes::HostPhase>(host.phase) ==
      kitsu868::party_modes::HostPhase::Lobby) {
    const kitsu868::party_modes::Status status =
        modePartyHost.setHostReady(ready);
    if (status != kitsu868::party_modes::Status::Ok) {
      error = "party_ready_failed";
      return false;
    }
    Serial.printf("KITSU_MODE_PARTY ready=host value=%s\n",
                  ready ? "true" : "false");
    return true;
  }
  const kitsu868::party_modes::ParticipantState guest =
      modePartyParticipant.snapshot();
  if (static_cast<kitsu868::party_modes::ParticipantPhase>(guest.phase) !=
      kitsu868::party_modes::ParticipantPhase::Lobby) {
    error = "party_not_in_lobby";
    return false;
  }
  kitsu868::party_modes::Packet packet{};
  const kitsu868::party_modes::Status status =
      modePartyParticipant.makeReady(ready, packet);
  if (status != kitsu868::party_modes::Status::Ok) {
    error = "party_ready_failed";
    return false;
  }
  if (!sendModePartyPacket(packet, true)) {
    (void)modePartyParticipant.restore(guest);
    error = "party_send_failed";
    return false;
  }
  modePartyAutoReadyPending = false;
  Serial.printf("KITSU_MODE_PARTY ready=guest value=%s\n",
                ready ? "true" : "false");
  return true;
}

bool beginModeParty(const char*& error) {
  error = nullptr;
  const kitsu868::party_modes::HostState before = modePartyHost.snapshot();
  if (static_cast<kitsu868::party_modes::HostPhase>(before.phase) !=
      kitsu868::party_modes::HostPhase::Lobby) {
    error = "party_not_hosting";
    return false;
  }
  if (!modePartyHost.canStart()) {
    error = "party_not_ready";
    return false;
  }
  kitsu868::party_modes::Packet roundOpen{};
  const kitsu868::party_modes::Status status =
      modePartyHost.begin(millis(), roundOpen);
  if (status != kitsu868::party_modes::Status::Ok) {
    error = "party_start_failed";
    return false;
  }
  if (!sendModePartyPacket(roundOpen, true)) {
    (void)modePartyHost.restore(before);
    error = "party_send_failed";
    return false;
  }
  const kitsu868::party_modes::HostState active = modePartyHost.snapshot();
  printModePartyPrompt(
      static_cast<kitsu868::party_modes::Mode>(active.mode), active.seed,
      active.currentRound, true);
  lastMemory = "Multiplayer round started.";
  return true;
}

bool parseModePartyContribution(long value,
                                kitsu868::party_modes::Mode mode,
                                kitsu868::party_modes::Contribution& out) {
  using kitsu868::party_modes::Mode;
  out = kitsu868::party_modes::Contribution{};
  if (mode == Mode::Triangulation || mode == Mode::HotCold) {
    if (value < -140L || value > 0L) return false;
    out.signalRssi = static_cast<int16_t>(value);
    return true;
  }
  long maximum = 1000L;
  if (mode == Mode::HideAndSeek) maximum = 7L;
  else if (mode == Mode::SyncRhythm) maximum = 2000L;
  else if (mode == Mode::CoopEcho) maximum = UINT16_MAX;
  else if (mode == Mode::SharedTrail) maximum = 20L;
  else if (mode == Mode::StoryVote) maximum = 2L;
  else if (mode == Mode::SignalHunt || mode == Mode::Count) return false;
  if (value < 0L || value > maximum) return false;
  out.value = static_cast<uint16_t>(value);
  return true;
}

bool submitModePartyContribution(long value, const char*& error) {
  error = nullptr;
  const kitsu868::party_modes::HostState host = modePartyHost.snapshot();
  if (static_cast<kitsu868::party_modes::HostPhase>(host.phase) ==
      kitsu868::party_modes::HostPhase::Active) {
    kitsu868::party_modes::Contribution contribution{};
    const kitsu868::party_modes::Mode mode =
        static_cast<kitsu868::party_modes::Mode>(host.mode);
    if (!parseModePartyContribution(value, mode, contribution)) {
      error = "party_contribution_invalid";
      return false;
    }
    const kitsu868::party_modes::Status status =
        modePartyHost.submitHostContribution(contribution);
    if (status != kitsu868::party_modes::Status::Ok) {
      error = kitsu868::party_modes::statusName(status);
      return false;
    }
    Serial.printf("KITSU_MODE_PARTY contribute=host mode=%s round=%u value=%ld\n",
                  kitsu868::party_modes::modeName(mode), host.currentRound,
                  value);
    return true;
  }
  const kitsu868::party_modes::ParticipantState guest =
      modePartyParticipant.snapshot();
  if (static_cast<kitsu868::party_modes::ParticipantPhase>(guest.phase) !=
      kitsu868::party_modes::ParticipantPhase::Active) {
    error = "party_not_running";
    return false;
  }
  kitsu868::party_modes::Contribution contribution{};
  const kitsu868::party_modes::Mode mode =
      static_cast<kitsu868::party_modes::Mode>(guest.mode);
  if (!parseModePartyContribution(value, mode, contribution)) {
    error = "party_contribution_invalid";
    return false;
  }
  kitsu868::party_modes::Packet packet{};
  const kitsu868::party_modes::Status status =
      modePartyParticipant.makeContribution(contribution, packet);
  if (status != kitsu868::party_modes::Status::Ok) {
    error = kitsu868::party_modes::statusName(status);
    return false;
  }
  if (!sendModePartyPacket(packet, true)) {
    (void)modePartyParticipant.restore(guest);
    error = "party_send_failed";
    return false;
  }
  Serial.printf("KITSU_MODE_PARTY contribute=guest mode=%s round=%u value=%ld\n",
                kitsu868::party_modes::modeName(mode), guest.currentRound,
                value);
  return true;
}

void leavePartyHotspot() {
  const kitsu868::party::SessionPhase hostPhase =
      static_cast<kitsu868::party::SessionPhase>(partyHost.state().phase);
  if (partyPhaseInProgress(hostPhase)) {
    kitsu868::party::Packet cancelled{};
    if (partyHost.cancel(kitsu868::party::CancelReason::HostEnded,
                         cancelled) ==
        kitsu868::party::SessionStatus::Ok) {
      (void)sendPartyPacket(cancelled, false);
    }
  }
  const kitsu868::party_modes::HostPhase modeHostPhase =
      static_cast<kitsu868::party_modes::HostPhase>(
          modePartyHost.state().phase);
  if (modePartyHostInProgress(modeHostPhase)) {
    kitsu868::party_modes::Packet cancelled{};
    if (modePartyHost.cancel(cancelled) ==
        kitsu868::party_modes::Status::Ok) {
      (void)sendModePartyPacket(cancelled, false);
    }
  }
  partyHost.reset();
  partyParticipant.reset();
  modePartyHost.reset();
  modePartyParticipant.reset();
  resetPartyRuntimeView();
  resetModePartyRuntimeView();
  if (radioListening) stopListening();
  lastMemory = "Party hotspot closed.";
  companionBleRefreshDirty = true;
  Serial.println("KITSU_PARTY leave=ok");
}

void stopListeningSafely() {
  if (partyRuntimeBusy()) {
    leavePartyHotspot();
  } else {
    stopListening();
  }
}

void showPartyCompletion(const kitsu868::party::HuntResult& result,
                         uint32_t sessionNonce) {
  if (sessionNonce == 0U ||
      lastPartyCelebratedSessionNonce == sessionNonce) {
    return;
  }
  const char* line1 = "SIGNAL FADED";
  const char* line2 = "YOU STAYED TOGETHER";
  switch (static_cast<kitsu868::party::ResultTier>(result.tier)) {
    case kitsu868::party::ResultTier::Trace:
      line1 = "TRACE FOUND";
      line2 = "THE PARTY HEARD IT";
      break;
    case kitsu868::party::ResultTier::Found:
      line1 = "SIGNAL FOUND";
      line2 = "TEAMWORK LOCKED ON";
      break;
    case kitsu868::party::ResultTier::Resonant:
      line1 = "RESONANT SIGNAL";
      line2 = "EVERYONE SYNCED";
      break;
    case kitsu868::party::ResultTier::Faded:
      break;
  }
  lastPartyCelebratedSessionNonce = sessionNonce;
  momentView.active = true;
  momentView.line1 = line1;
  momentView.line2 = line2;
  momentView.until = millis() + MOMENT_DISPLAY_MS;
  lastMemory = String(line1) + ": " + line2;
  recordSessionGoal(kitsu868::fun::SessionActivity::Signal);
  cancelAmbientAnimation();
  if (!startTransientAnimation(CompanionRole::Meet)) startBaseAnimation();

  if (dialogueStateReady &&
      dialogueStories.activeStory == kitsu868::dialogue::kNoActiveStory) {
    const kitsu868::dialogue::StoryState before = dialogueStories;
    kitsu868::dialogue::StoryBeat ignored{};
    if (kitsu868::dialogue::startStory(
            kitsu868::dialogue::StoryTrigger::NearbySignal,
            companionBrain.personality().kind,
            companionBrain.deviceFingerprint(), dialogueStories, ignored)) {
      if (persistDialogueState()) {
        storyResolutionAvailable = false;
      } else {
        dialogueStories = before;
        Serial.println("KITSU_WARN party_story=store_failed");
      }
    }
  }
  companionBleRefreshDirty = true;
  Serial.printf("KITSU_PARTY complete nonce=%lu tier=%u score=%u/%u\n",
                static_cast<unsigned long>(sessionNonce),
                static_cast<unsigned>(result.tier), result.score,
                result.maximumScore);
}

void recordPartyReward(const kitsu868::party::HuntResult& result,
                       uint32_t sessionNonce,
                       const uint16_t* participantUids,
                       uint8_t participantCount, uint32_t now) {
  if (sessionNonce == 0U ||
      lastPartyRewardSessionNonce == sessionNonce ||
      !partyRewardStateReady || !participantUids ||
      participantCount < kitsu868::party::kMinimumParticipants ||
      participantCount > kitsu868::party::kMaximumParticipants) {
    return;
  }
  if (lastPartyRewardAttemptAt != 0U &&
      static_cast<uint32_t>(now - lastPartyRewardAttemptAt) <
          PARTY_REWARD_RETRY_MS) {
    return;
  }
  lastPartyRewardAttemptAt = now;
  LocalClockSample rewardClock{};
  if (!localClockSample(now, true, rewardClock)) return;
  const uint32_t dayId = rewardClock.dayId;
  const uint32_t epoch = rewardClock.epochSeconds;

  kitsu868::party::CompletedHuntReward completed{};
  completed.selfUid = ownUidSuffix();
  completed.participantCount = participantCount;
  completed.tier = result.tier;
  completed.sessionNonce = sessionNonce;
  completed.resultProof = result.proof;
  completed.dayId = dayId;
  completed.nowEpochSeconds = epoch;
  for (uint8_t index = 0U; index < participantCount; ++index) {
    completed.participantUids[index] = participantUids[index];
  }

  const kitsu868::party::RewardState before = partyRewards.snapshot();
  kitsu868::party::RewardOutcome outcome{};
  const kitsu868::party::RewardStatus status =
      partyRewards.recordCompletedHunt(
          completed, kitsu868::party::kDefaultPeerRewardCooldownSeconds,
          outcome);
  if (status == kitsu868::party::RewardStatus::DuplicateSession) {
    outcome.partyBondAfter = partyRewards.state().partyBond;
    outcome.currentStreakDays = partyRewards.state().currentStreakDays;
    outcome.longestStreakDays = partyRewards.state().longestStreakDays;
  } else if (status == kitsu868::party::RewardStatus::Awarded ||
             status ==
                 kitsu868::party::RewardStatus::RecordedNoEligiblePeer) {
    if (!persistPartyRewardState()) {
      (void)partyRewards.restore(before);
      Serial.println("KITSU_WARN party_reward=store_failed");
      return;
    }
  } else {
    Serial.printf("KITSU_WARN party_reward=%s\n",
                  kitsu868::party::rewardStatusName(status));
    return;
  }
  lastPartyReward = outcome;
  partyRewardAvailable = true;
  lastPartyRewardSessionNonce = sessionNonce;

  if (socialProgressionReady &&
      status != kitsu868::party::RewardStatus::DuplicateSession) {
    const uint16_t normalizedScore = result.maximumScore == 0U
        ? 0U
        : static_cast<uint16_t>(min<uint32_t>(
              1000U, (static_cast<uint32_t>(result.score) * 1000U) /
                         result.maximumScore));
    const kitsu868::social::SocialState socialBefore =
        socialProgression.snapshot();
    kitsu868::social::PartyRewardRequest socialReward{};
    socialReward.sessionNonce = sessionNonce ^ UINT32_C(0x534F4349);
    if (socialReward.sessionNonce == 0U) {
      socialReward.sessionNonce = UINT32_C(0x534F4349);
    }
    socialReward.dayId = dayId;
    socialReward.epochSeconds = epoch;
    socialReward.score = normalizedScore;
    for (uint8_t index = 0U; index < participantCount; ++index) {
      const uint16_t peerUid = participantUids[index];
      if (peerUid == 0U || peerUid == ownUidSuffix()) continue;
      if (socialReward.peerCount >= kitsu868::social::kRewardPeerCapacity) {
        break;
      }
      socialReward.peerUids[socialReward.peerCount++] = peerUid;
    }
    kitsu868::social::PartyRewardBatchOutcome socialOutcome{};
    const kitsu868::social::SocialStatus socialStatus =
        socialReward.peerCount == 0U
            ? kitsu868::social::SocialStatus::InvalidInput
            : socialProgression.recordPartyRewards(socialReward,
                                                   socialOutcome);
    bool socialChanged =
        socialStatus == kitsu868::social::SocialStatus::Ok;
    uint8_t challengeCompleted = 0U;
    const kitsu868::social::SocialStatus challengeStatus =
        socialProgression.contributeDailyChallenge(
            dayId, 1U, 3U, challengeCompleted);
    socialChanged = socialChanged ||
        challengeStatus == kitsu868::social::SocialStatus::Ok;
    if (socialChanged && !persistSocialProgression()) {
      (void)socialProgression.restore(socialBefore);
      challengeCompleted = 0U;
      Serial.println("KITSU_WARN social_party=store_failed");
    }
    if (challengeCompleted != 0U) {
      momentView.active = true;
      momentView.line1 = "PARTY CHALLENGE";
      momentView.line2 = "COMPLETE";
      momentView.until = now + MOMENT_DISPLAY_MS;
    }
  }

  if (adventureProgressionReady) {
    const kitsu868::adventure::ProgressState adventureBefore =
        adventureProgression.snapshot();
    kitsu868::adventure::Status adventureStatus =
        adventureProgression.applyParty(result);
    if (adventureProgression.view().phase ==
        kitsu868::adventure::RoutePhase::AwaitingRescue) {
      adventureStatus =
          adventureProgression.rescue(result, adventureClock(now));
    }
    kitsu868::adventure::HotspotUpdate hotspot{};
    const kitsu868::adventure::Status hotspotStatus =
        adventureProgression.observeHotspot(
            sessionNonce, participantCount, adventureClock(now), hotspot);
    if ((adventureStatus == kitsu868::adventure::Status::Ok ||
         adventureStatus == kitsu868::adventure::Status::Duplicate ||
         hotspotStatus == kitsu868::adventure::Status::Ok ||
        hotspotStatus == kitsu868::adventure::Status::Duplicate) &&
        !persistAdventureProgression()) {
      (void)adventureProgression.restore(adventureBefore);
      Serial.println("KITSU_WARN adventure_party=store_failed");
    }
  }
  companionBleRefreshDirty = true;
  Serial.printf("KITSU_PARTY reward=%s bond=%u total=%lu peers=%u\n",
                kitsu868::party::rewardStatusName(status),
                outcome.bondAwarded,
                static_cast<unsigned long>(partyRewards.state().partyBond),
                outcome.eligibleUniquePeers);
}

void finishPartyIfComplete(uint32_t now) {
  const kitsu868::party::HostState host = partyHost.snapshot();
  if (static_cast<kitsu868::party::SessionPhase>(host.phase) ==
      kitsu868::party::SessionPhase::Complete) {
    uint16_t participants[kitsu868::party::kMaximumParticipants]{};
    uint8_t count = 0U;
    for (uint8_t index = 0U;
         index < kitsu868::party::kMaximumParticipants; ++index) {
      if (host.participants[index].active != 0U) {
        participants[count++] = host.participants[index].hunt.uid;
      }
    }
    showPartyCompletion(host.result, host.sessionNonce);
    recordPartyReward(host.result, host.sessionNonce, participants, count,
                      now);
    return;
  }

  const kitsu868::party::ParticipantState guest =
      partyParticipant.snapshot();
  if (static_cast<kitsu868::party::SessionPhase>(guest.phase) !=
      kitsu868::party::SessionPhase::Complete) {
    return;
  }
  // Guests do not receive the full host roster over the 30-byte packet.
  // Reward only the one peer we can truthfully identify instead of inventing
  // UIDs for unknown party members.
  const uint16_t participants[2] = {guest.localUid, guest.hostUid};
  showPartyCompletion(guest.result, guest.sessionNonce);
  recordPartyReward(guest.result, guest.sessionNonce, participants, 2U, now);
}

uint32_t modePartyRewardNonce(uint32_t rawNonce) {
  uint32_t scoped = rawNonce ^ UINT32_C(0x4D385231);  // "M8R1"
  scoped ^= scoped >> 16U;
  scoped *= UINT32_C(0x7FEB352D);
  scoped ^= scoped >> 15U;
  return scoped == 0U ? UINT32_C(0x4D385231) : scoped;
}

kitsu868::party::HuntResult modePartyHuntResult(
    const kitsu868::party_modes::ModeResult& result) {
  kitsu868::party::HuntResult adapted{};
  adapted.participantCount = result.participantCount;
  adapted.completedRounds = result.completedRounds;
  adapted.score = result.score;
  adapted.maximumScore = result.maximumScore;
  adapted.proof = result.proof;
  kitsu868::party::ResultTier tier = kitsu868::party::ResultTier::Faded;
  const uint16_t normalized = result.maximumScore == 0U
      ? 0U
      : static_cast<uint16_t>(min<uint32_t>(
            1000U, static_cast<uint32_t>(result.score) * 1000U /
                       result.maximumScore));
  if (normalized >= 850U) tier = kitsu868::party::ResultTier::Resonant;
  else if (normalized >= 600U) tier = kitsu868::party::ResultTier::Found;
  else if (normalized >= 300U) tier = kitsu868::party::ResultTier::Trace;
  adapted.tier = static_cast<uint8_t>(tier);
  return adapted;
}

bool applyModePartySocialOutcome(
    kitsu868::party_modes::Mode mode,
    const kitsu868::party_modes::ModeResult& result, uint32_t rawNonce) {
  const kitsu868::signal::SignalTrailState trailBefore =
      signalTrail.snapshot();
  if (mode == kitsu868::party_modes::Mode::SharedTrail) {
    if (result.outcomeValue > kitsu868::signal::kSignalTrailMaximumMisses) {
      return false;
    }
    const kitsu868::signal::SignalTrailMergeStatus mergeStatus =
        signalTrail.mergeSharedMissCount(
            static_cast<uint8_t>(result.outcomeValue));
    if (mergeStatus == kitsu868::signal::SignalTrailMergeStatus::Applied) {
      if (!persistSignalEncounterState()) {
        (void)signalTrail.restore(trailBefore);
        Serial.println("KITSU_WARN mode_party_live_trail=store_failed");
        return false;
      }
    } else if (mergeStatus !=
               kitsu868::signal::SignalTrailMergeStatus::Unchanged) {
      Serial.printf("KITSU_WARN mode_party_live_trail=%u\n",
                    static_cast<unsigned>(mergeStatus));
      return false;
    }
  }
  // The encounter meter is an independent durable reward. A damaged optional
  // social ledger must not erase a valid, already-verified shared result.
  if (!socialProgressionReady) return true;
  const kitsu868::social::SocialState before = socialProgression.snapshot();
  bool changed = false;
  if (result.rareEncounter != 0U) {
    const kitsu868::social::SocialStatus status =
        socialProgression.recordCooperativeRareEncounter(rawNonce);
    if (status == kitsu868::social::SocialStatus::Ok) {
      changed = true;
    } else if (status != kitsu868::social::SocialStatus::Duplicate) {
      (void)socialProgression.restore(before);
      Serial.printf("KITSU_WARN mode_party_rare=%u\n",
                    static_cast<unsigned>(status));
      return false;
    }
  }
  if (mode == kitsu868::party_modes::Mode::SharedTrail) {
    const kitsu868::social::SocialStatus status =
        socialProgression.recordSharedTrailResult(
            rawNonce, static_cast<uint8_t>(result.outcomeValue));
    if (status == kitsu868::social::SocialStatus::Ok) {
      changed = true;
    } else if (status != kitsu868::social::SocialStatus::Duplicate) {
      (void)socialProgression.restore(before);
      Serial.printf("KITSU_WARN mode_party_trail=%u\n",
                    static_cast<unsigned>(status));
      return false;
    }
  }
  if (changed && !persistSocialProgression()) {
    (void)socialProgression.restore(before);
    // The live trail is written first so a reboot cannot erase an earned
    // guarantee. This idempotent function retries the social counter later.
    Serial.println("KITSU_WARN mode_party_social=store_failed");
    return false;
  }
  if (changed && result.rareEncounter != 0U) {
    // The verified cooperative result earns a real encounter attempt through
    // the existing durable coordinator; rarity and unlock odds remain honest.
    recordSuccessfulEncounterTrigger(
        kitsu868::signal::MeshOperationKind::NearbyKitsuMet);
  }
  return true;
}

bool applyModePartyStoryVote(uint8_t choice) {
  if (!dialogueStateReady ||
      choice >= kitsu868::dialogue::kStoryChoiceCount) return false;
  kitsu868::dialogue::StoryBeat current{};
  if (!kitsu868::dialogue::currentStoryBeat(dialogueStories, current)) {
    Serial.println("KITSU_WARN mode_party_story=unavailable");
    return false;
  }
  if (!current.awaitsChoice) {
    const kitsu868::dialogue::StoryState before = dialogueStories;
    kitsu868::dialogue::StoryBeat next{};
    if (!kitsu868::dialogue::advanceStory(dialogueStories, next) ||
        !persistDialogueState()) {
      dialogueStories = before;
      Serial.println("KITSU_WARN mode_party_story=advance_failed");
      return false;
    }
    current = next;
  }
  String payload = "{\"story_id\":";
  payload += String(current.storyId);
  payload += ",\"choice\":";
  payload += String(choice);
  payload += '}';
  String response;
  const bool handled = companion_api::chooseFunStory(
      reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length(),
      response);
  if (!handled || response.indexOf("\"error_code\"") >= 0) {
    Serial.println("KITSU_WARN mode_party_story=resolve_failed");
    return false;
  }
  Serial.printf("KITSU_MODE_PARTY story_choice=%u story=%u\n", choice,
                current.storyId);
  return true;
}

void finishModePartyIfComplete(uint32_t now) {
  kitsu868::party_modes::Mode mode = kitsu868::party_modes::Mode::Count;
  kitsu868::party_modes::ModeResult result{};
  uint32_t rawNonce = 0U;
  uint16_t participants[kitsu868::party::kMaximumParticipants]{};
  uint8_t participantCount = 0U;

  const kitsu868::party_modes::HostState host = modePartyHost.snapshot();
  if (static_cast<kitsu868::party_modes::HostPhase>(host.phase) ==
      kitsu868::party_modes::HostPhase::Complete) {
    mode = static_cast<kitsu868::party_modes::Mode>(host.mode);
    result = host.result;
    rawNonce = host.sessionNonce;
    for (uint8_t index = 0U;
         index < kitsu868::party_modes::kMaximumParticipants; ++index) {
      if (host.members[index].active != 0U) {
        participants[participantCount++] = host.members[index].uid;
      }
    }
  } else {
    const kitsu868::party_modes::ParticipantState guest =
        modePartyParticipant.snapshot();
    if (static_cast<kitsu868::party_modes::ParticipantPhase>(guest.phase) !=
        kitsu868::party_modes::ParticipantPhase::Complete) {
      return;
    }
    mode = static_cast<kitsu868::party_modes::Mode>(guest.mode);
    result = guest.result;
    rawNonce = guest.sessionNonce;
    participants[0] = guest.localUid;
    participants[1] = guest.hostUid;
    participantCount = 2U;
  }
  if (rawNonce == 0U) return;

  const bool specialAlreadyHandled =
      lastModePartyHandledSessionNonce == rawNonce;
  const uint32_t rewardNonce = modePartyRewardNonce(rawNonce);
  const kitsu868::party::HuntResult adapted = modePartyHuntResult(result);
  showPartyCompletion(adapted, rewardNonce);
  recordPartyReward(adapted, rewardNonce, participants, participantCount,
                    now);
  if (!specialAlreadyHandled) {
    const bool socialApplied =
        applyModePartySocialOutcome(mode, result, rawNonce);
    const bool storyApplied =
        mode != kitsu868::party_modes::Mode::StoryVote ||
        applyModePartyStoryVote(result.storyChoice);
    if (socialApplied && storyApplied) {
      lastModePartyHandledSessionNonce = rawNonce;
    }
    Serial.printf(
        "KITSU_MODE_PARTY complete mode=%s score=%u/%u players=%u rare=%s "
        "outcome=%u proof=%08lX\n",
        kitsu868::party_modes::modeName(mode), result.score,
        result.maximumScore, result.participantCount,
        result.rareEncounter != 0U ? "true" : "false",
        result.outcomeValue, static_cast<unsigned long>(result.proof));
  }
}

void processModePartyRadioPacket(
    const kitsu868::party_modes::Packet& packet, float rssi, float snr) {
  if (!radioListening || packet.sourceUid == ownUidSuffix()) return;
  const uint32_t now = millis();
  const kitsu868::party_modes::HostState host = modePartyHost.snapshot();
  if (modePartyHostInProgress(
          static_cast<kitsu868::party_modes::HostPhase>(host.phase))) {
    if (packet.hostUid != host.hostUid ||
        packet.sessionNonce != host.sessionNonce ||
        packet.mode != static_cast<kitsu868::party_modes::Mode>(host.mode)) {
      return;
    }
    if (packet.type == kitsu868::party_modes::PacketType::JoinRequest) {
      const kitsu868::party_modes::HostState before = modePartyHost.snapshot();
      kitsu868::party_modes::Packet welcome{};
      const kitsu868::party_modes::Status status =
          modePartyHost.acceptJoin(packet, now, welcome);
      if (status == kitsu868::party_modes::Status::Ok) {
        if (!sendModePartyPacket(welcome, true)) {
          (void)modePartyHost.restore(before);
          return;
        }
        Serial.printf("KITSU_MODE_PARTY join=accepted uid=%04X count=%u\n",
                      packet.sourceUid,
                      modePartyHost.state().participantCount);
      } else if (status != kitsu868::party_modes::Status::Duplicate &&
                 status != kitsu868::party_modes::Status::StaleSequence) {
        Serial.printf("KITSU_MODE_PARTY join=rejected uid=%04X result=%s\n",
                      packet.sourceUid,
                      kitsu868::party_modes::statusName(status));
      }
      return;
    }
    if (packet.type == kitsu868::party_modes::PacketType::Ready) {
      const kitsu868::party_modes::Status status =
          modePartyHost.acceptReady(packet);
      if (status == kitsu868::party_modes::Status::Ok) {
        Serial.printf("KITSU_MODE_PARTY ready=received uid=%04X value=%d\n",
                      packet.sourceUid, packet.value);
      } else if (status != kitsu868::party_modes::Status::Duplicate &&
                 status != kitsu868::party_modes::Status::StaleSequence) {
        Serial.printf("KITSU_MODE_PARTY ready=rejected result=%s\n",
                      kitsu868::party_modes::statusName(status));
      }
      return;
    }
    if (packet.type == kitsu868::party_modes::PacketType::Contribution) {
      const kitsu868::party_modes::Status status =
          modePartyHost.acceptContribution(packet, now);
      if (status == kitsu868::party_modes::Status::Ok) {
        Serial.printf("KITSU_MODE_PARTY contribution=received uid=%04X round=%u\n",
                      packet.sourceUid, packet.round);
      } else if (status != kitsu868::party_modes::Status::Duplicate &&
                 status != kitsu868::party_modes::Status::StaleSequence &&
                 status != kitsu868::party_modes::Status::AlreadySubmitted) {
        Serial.printf("KITSU_MODE_PARTY contribution=rejected result=%s\n",
                      kitsu868::party_modes::statusName(status));
      }
    }
    return;
  }

  const kitsu868::party_modes::ParticipantState guest =
      modePartyParticipant.snapshot();
  const kitsu868::party_modes::ParticipantPhase guestPhase =
      static_cast<kitsu868::party_modes::ParticipantPhase>(guest.phase);
  if (packet.type == kitsu868::party_modes::PacketType::Beacon) {
    kitsu868::party_modes::Status status =
        kitsu868::party_modes::Status::WrongPhase;
    if ((guestPhase == kitsu868::party_modes::ParticipantPhase::Idle ||
         guestPhase == kitsu868::party_modes::ParticipantPhase::Observed) &&
        modePartyScanActive) {
      status = modePartyParticipant.observeBeacon(
          ownUidSuffix(), packet, now);
    } else if ((guestPhase ==
                    kitsu868::party_modes::ParticipantPhase::Lobby ||
                guestPhase ==
                    kitsu868::party_modes::ParticipantPhase::Active) &&
               guest.hostUid == packet.hostUid &&
               guest.sessionNonce == packet.sessionNonce) {
      status = modePartyParticipant.acceptHostPacket(packet, now);
    } else if (guestPhase ==
                   kitsu868::party_modes::ParticipantPhase::Joining &&
               modePartyJoinRequested && guest.hostUid == packet.hostUid &&
               guest.sessionNonce == packet.sessionNonce) {
      // Preserve the replay cursor for the targeted Welcome packet.
      partyHostRssi = rssi;
      partyHostSnr = snr;
      return;
    }
    if (status == kitsu868::party_modes::Status::Ok ||
        status == kitsu868::party_modes::Status::Duplicate) {
      partyHostRssi = rssi;
      partyHostSnr = snr;
      Serial.printf("KITSU_MODE_PARTY beacon=seen host=%04X mode=%s count=%u\n",
                    packet.hostUid,
                    kitsu868::party_modes::modeName(packet.mode),
                    packet.participantCount);
    }
    return;
  }

  if (!modePartyGuestInProgress(guestPhase) ||
      packet.hostUid != guest.hostUid ||
      packet.sessionNonce != guest.sessionNonce ||
      packet.sourceUid != packet.hostUid) {
    return;
  }
  if (packet.type != kitsu868::party_modes::PacketType::Welcome &&
      packet.type != kitsu868::party_modes::PacketType::RoundOpen &&
      packet.type != kitsu868::party_modes::PacketType::Result &&
      packet.type != kitsu868::party_modes::PacketType::Cancel) {
    return;
  }
  const kitsu868::party_modes::Status status =
      modePartyParticipant.acceptHostPacket(packet, now);
  if (status == kitsu868::party_modes::Status::Ok) {
    partyHostRssi = rssi;
    partyHostSnr = snr;
    clearModePartyReplay();
    if (packet.type == kitsu868::party_modes::PacketType::Welcome) {
      modePartyJoinRequested = false;
      modePartyScanActive = false;
      const kitsu868::party_modes::ParticipantPhase admitted =
          static_cast<kitsu868::party_modes::ParticipantPhase>(
              modePartyParticipant.state().phase);
      modePartyAutoReadyPending =
          admitted == kitsu868::party_modes::ParticipantPhase::Lobby;
    } else if (packet.type ==
               kitsu868::party_modes::PacketType::RoundOpen) {
      const kitsu868::party_modes::ParticipantState active =
          modePartyParticipant.snapshot();
      printModePartyPrompt(
          static_cast<kitsu868::party_modes::Mode>(active.mode), active.seed,
          active.currentRound, false);
    } else if (packet.type == kitsu868::party_modes::PacketType::Cancel) {
      modePartyJoinRequested = false;
      modePartyAutoReadyPending = false;
    }
    Serial.printf("KITSU_MODE_PARTY host_packet=%u phase=%u\n",
                  static_cast<unsigned>(packet.type),
                  static_cast<unsigned>(modePartyParticipant.state().phase));
  } else if (status != kitsu868::party_modes::Status::Duplicate &&
             status != kitsu868::party_modes::Status::StaleSequence) {
    Serial.printf("KITSU_MODE_PARTY host_packet=%u result=%s\n",
                  static_cast<unsigned>(packet.type),
                  kitsu868::party_modes::statusName(status));
  }
}

void processPartyRadioPacket(const kitsu868::party::Packet& packet,
                             float rssi, float snr) {
  if (!radioListening || packet.sourceUid == ownUidSuffix()) return;
  const uint32_t now = millis();
  const kitsu868::party::HostState host = partyHost.snapshot();
  const kitsu868::party::SessionPhase hostPhase =
      static_cast<kitsu868::party::SessionPhase>(host.phase);
  if (partyPhaseInProgress(hostPhase)) {
    if (packet.hostUid != host.hostUid ||
        packet.sessionNonce != host.sessionNonce) {
      return;
    }
    if (packet.type == kitsu868::party::PacketType::JoinRequest) {
      const kitsu868::party::HostState before = partyHost.snapshot();
      kitsu868::party::Packet welcome{};
      const kitsu868::party::SessionStatus status =
          partyHost.acceptJoin(packet, now, welcome);
      if (status == kitsu868::party::SessionStatus::Ok) {
        if (!sendPartyPacket(welcome, true)) {
          (void)partyHost.restore(before);
          Serial.println("KITSU_WARN party_join=welcome_send_failed");
          return;
        }
        lastMemory = "A friend joined the party hotspot.";
        companionBleRefreshDirty = true;
        Serial.printf("KITSU_PARTY join=accepted uid=%04X count=%u\n",
                      packet.sourceUid, partyHost.state().participantCount);
      } else if (status != kitsu868::party::SessionStatus::Duplicate &&
                 status != kitsu868::party::SessionStatus::StaleSequence) {
        Serial.printf("KITSU_PARTY join=rejected uid=%04X result=%s\n",
                      packet.sourceUid,
                      kitsu868::party::sessionStatusName(status));
      }
      return;
    }
    if (packet.type == kitsu868::party::PacketType::RoundChoice) {
      const kitsu868::party::SessionStatus status =
          partyHost.acceptChoice(packet, now);
      if (status == kitsu868::party::SessionStatus::Ok) {
        companionBleRefreshDirty = true;
        Serial.printf("KITSU_PARTY choice=received uid=%04X round=%u\n",
                      packet.sourceUid, packet.round);
      } else if (status != kitsu868::party::SessionStatus::Duplicate &&
                 status != kitsu868::party::SessionStatus::StaleSequence &&
                 status !=
                     kitsu868::party::SessionStatus::ChoiceAlreadySubmitted) {
        Serial.printf("KITSU_PARTY choice=rejected uid=%04X result=%s\n",
                      packet.sourceUid,
                      kitsu868::party::sessionStatusName(status));
      }
    }
    return;
  }

  const kitsu868::party::ParticipantState guest =
      partyParticipant.snapshot();
  const kitsu868::party::SessionPhase guestPhase =
      static_cast<kitsu868::party::SessionPhase>(guest.phase);
  if (packet.type == kitsu868::party::PacketType::Beacon) {
    kitsu868::party::SessionStatus status =
        kitsu868::party::SessionStatus::WrongPhase;
    if (!partyPhaseInProgress(guestPhase)) {
      if (!partyScanActive) return;
      status = partyParticipant.observeBeacon(ownUidSuffix(), packet, now);
      if (status == kitsu868::party::SessionStatus::Ok) {
        partyJoinRequested = false;
        partyWelcomeAccepted = false;
      }
    } else if (guestPhase == kitsu868::party::SessionPhase::Joining &&
               partyJoinRequested && guest.hostUid == packet.hostUid &&
               guest.sessionNonce == packet.sessionNonce) {
      // Do not advance the participant's single host replay cursor past an
      // in-flight Welcome. A later Beacon sequence would otherwise make that
      // targeted Welcome look stale. The original lobby deadline remains the
      // truthful retry boundary until admission is confirmed.
      partyHostRssi = rssi;
      partyHostSnr = snr;
      return;
    } else if (guest.hostUid == packet.hostUid &&
               guest.sessionNonce == packet.sessionNonce &&
               (guestPhase == kitsu868::party::SessionPhase::Joining ||
                guestPhase == kitsu868::party::SessionPhase::Lobby)) {
      status = partyParticipant.acceptHostPacket(packet, now);
    }
    if (status == kitsu868::party::SessionStatus::Ok ||
        status == kitsu868::party::SessionStatus::Duplicate) {
      partyHostRssi = rssi;
      partyHostSnr = snr;
      // Once the owner chose Join, continued beacons only keep the session
      // fresh; they must not recreate the Join affordance.
      if (!partyJoinRequested && !partyWelcomeAccepted) {
        lastPartyBeaconAt = now;
      }
      companionBleRefreshDirty = true;
      if (status == kitsu868::party::SessionStatus::Ok) {
        Serial.printf("KITSU_PARTY beacon=seen host=%04X count=%u\n",
                      packet.hostUid, packet.participantCount);
      }
    }
    return;
  }

  if ((!partyJoinRequested && !partyWelcomeAccepted) ||
      !partyPhaseInProgress(guestPhase) ||
      packet.hostUid != guest.hostUid ||
      packet.sessionNonce != guest.sessionNonce) {
    return;
  }
  if (packet.type != kitsu868::party::PacketType::Welcome &&
      packet.type != kitsu868::party::PacketType::RoundOpen &&
      packet.type != kitsu868::party::PacketType::Result &&
      packet.type != kitsu868::party::PacketType::Cancel) {
    return;
  }
  const kitsu868::party::SessionStatus status =
      partyParticipant.acceptHostPacket(packet, now);
  if (status == kitsu868::party::SessionStatus::Ok) {
    partyHostRssi = rssi;
    partyHostSnr = snr;
    lastPartyBeaconAt = 0U;
    clearPartyReplay();
    if (packet.type == kitsu868::party::PacketType::Welcome ||
        packet.type == kitsu868::party::PacketType::RoundOpen) {
      partyWelcomeAccepted = true;
      partyJoinRequested = false;
      partyScanActive = false;
    } else if (packet.type == kitsu868::party::PacketType::Cancel) {
      partyJoinRequested = false;
    }
    companionBleRefreshDirty = true;
    Serial.printf("KITSU_PARTY host_packet=%u phase=%u\n",
                  static_cast<unsigned>(packet.type),
                  static_cast<unsigned>(partyParticipant.state().phase));
  } else if (status != kitsu868::party::SessionStatus::Duplicate &&
             status != kitsu868::party::SessionStatus::StaleSequence) {
    Serial.printf("KITSU_PARTY host_packet=%u result=%s\n",
                  static_cast<unsigned>(packet.type),
                  kitsu868::party::sessionStatusName(status));
  }
}

void tickPartyHotspot(uint32_t now) {
  if (!radioListening) {
    partyScanActive = false;
    lastPartyBeaconAt = 0U;
    const kitsu868::party::SessionPhase hostPhase =
        static_cast<kitsu868::party::SessionPhase>(partyHost.state().phase);
    if (partyPhaseInProgress(hostPhase)) {
      kitsu868::party::Packet ignored{};
      (void)partyHost.cancel(kitsu868::party::CancelReason::HostEnded,
                             ignored);
      companionBleRefreshDirty = true;
    }
    const kitsu868::party::SessionPhase guestPhase =
        static_cast<kitsu868::party::SessionPhase>(
            partyParticipant.state().phase);
    if (partyPhaseInProgress(guestPhase)) {
      partyParticipant.reset();
      partyScanActive = false;
      partyJoinRequested = false;
      partyWelcomeAccepted = false;
      lastPartyBeaconAt = 0U;
      companionBleRefreshDirty = true;
    }
    clearPartyReplay();
    finishPartyIfComplete(now);
    return;
  }

  const kitsu868::party::SessionPhase initialHostPhase =
      static_cast<kitsu868::party::SessionPhase>(partyHost.state().phase);
  if (initialHostPhase == kitsu868::party::SessionPhase::Lobby) {
    const kitsu868::party::SessionStatus expiry =
        partyHost.expireIfNeeded(now);
    if (expiry == kitsu868::party::SessionStatus::TimedOut) {
      companionBleRefreshDirty = true;
      Serial.println("KITSU_PARTY host=expired");
    } else if (lastPartyBeaconTxAt == 0U ||
               static_cast<uint32_t>(now - lastPartyBeaconTxAt) >=
                   PARTY_BEACON_INTERVAL_MS) {
      const kitsu868::party::HostState before = partyHost.snapshot();
      kitsu868::party::Packet beacon{};
      const kitsu868::party::SessionStatus status =
          partyHost.makeBeacon(now, beacon);
      lastPartyBeaconTxAt = now;
      if (status == kitsu868::party::SessionStatus::Ok &&
          !sendPartyPacket(beacon, false)) {
        (void)partyHost.restore(before);
      }
    }
  } else if (partyPhaseRunning(initialHostPhase) &&
             (lastPartyTxAt == 0U ||
              static_cast<uint32_t>(now - lastPartyTxAt) >= 1000UL)) {
    const kitsu868::party::HostState before = partyHost.snapshot();
    kitsu868::party::Packet next{};
    const kitsu868::party::SessionStatus status =
        partyHost.advance(now, next);
    if (status == kitsu868::party::SessionStatus::Ok) {
      if (!sendPartyPacket(next, true)) {
        (void)partyHost.restore(before);
      } else {
        companionBleRefreshDirty = true;
        Serial.printf("KITSU_PARTY advance=ok packet=%u round=%u\n",
                      static_cast<unsigned>(next.type), next.round);
      }
    } else if (status != kitsu868::party::SessionStatus::NotReady) {
      Serial.printf("KITSU_PARTY advance=%s\n",
                    kitsu868::party::sessionStatusName(status));
    }
  }

  const kitsu868::party::ParticipantState beforeGuest =
      partyParticipant.snapshot();
  const kitsu868::party::SessionPhase guestPhase =
      static_cast<kitsu868::party::SessionPhase>(beforeGuest.phase);
  if (partyPhaseInProgress(guestPhase)) {
    const bool explicitSession =
        partyJoinRequested || partyWelcomeAccepted;
    const bool waitForHostPacket = explicitSession &&
        static_cast<int32_t>(now - beforeGuest.deadlineMs) >= 0 &&
        static_cast<uint32_t>(now - beforeGuest.deadlineMs) <
            PARTY_HOST_PACKET_GRACE_MS;
    const kitsu868::party::SessionStatus expiry =
        waitForHostPacket ? kitsu868::party::SessionStatus::Ok
                          : partyParticipant.expireIfNeeded(now);
    if (expiry == kitsu868::party::SessionStatus::TimedOut) {
      partyJoinRequested = false;
      companionBleRefreshDirty = true;
      Serial.println("KITSU_PARTY guest=expired");
    } else if (guestPhase == kitsu868::party::SessionPhase::Joining &&
               partyJoinRequested &&
               (lastPartyTxAt == 0U ||
                static_cast<uint32_t>(now - lastPartyTxAt) >=
                    PARTY_JOIN_RETRY_MS)) {
      kitsu868::party::Packet request{};
      const kitsu868::party::SessionStatus status =
          partyParticipant.makeJoinRequest(request);
      if (status == kitsu868::party::SessionStatus::Ok &&
          !sendPartyPacket(request, true)) {
        (void)partyParticipant.restore(beforeGuest);
      }
    }
  }

  finishPartyIfComplete(now);

  if (partyReplayRemaining != 0U && radioListening &&
      static_cast<int32_t>(now - partyReplayAt) >= 0) {
    const kitsu868::party::Packet replay = partyReplayPacket;
    --partyReplayRemaining;
    partyReplayAt = now + PARTY_REPLAY_INTERVAL_MS;
    (void)sendPartyPacketNow(replay);
    if (partyReplayRemaining == 0U) clearPartyReplay();
  } else if (partyReplayRemaining != 0U && !radioListening) {
    clearPartyReplay();
  }
}

void tickModeParty(uint32_t now) {
  finishModePartyIfComplete(now);
  if (!radioListening) {
    modePartyScanActive = false;
    modePartyJoinRequested = false;
    modePartyAutoReadyPending = false;
    const kitsu868::party_modes::HostPhase hostPhase =
        static_cast<kitsu868::party_modes::HostPhase>(
            modePartyHost.state().phase);
    if (modePartyHostInProgress(hostPhase)) modePartyHost.reset();
    const kitsu868::party_modes::ParticipantPhase guestPhase =
        static_cast<kitsu868::party_modes::ParticipantPhase>(
            modePartyParticipant.state().phase);
    if (modePartyGuestInProgress(guestPhase)) modePartyParticipant.reset();
    clearModePartyReplay();
    return;
  }

  kitsu868::party_modes::HostState host = modePartyHost.snapshot();
  kitsu868::party_modes::HostPhase hostPhase =
      static_cast<kitsu868::party_modes::HostPhase>(host.phase);
  if (hostPhase == kitsu868::party_modes::HostPhase::Lobby) {
    const kitsu868::party_modes::Status expiry =
        modePartyHost.expireIfNeeded(now);
    if (expiry == kitsu868::party_modes::Status::TimedOut) {
      Serial.println("KITSU_MODE_PARTY host=expired");
    } else if (modePartyHost.canStart() &&
               (lastModePartyTxAt == 0U ||
                static_cast<uint32_t>(now - lastModePartyTxAt) >= 1000UL)) {
      const char* error = nullptr;
      if (!beginModeParty(error) && error &&
          strcmp(error, "party_send_failed") != 0) {
        Serial.printf("KITSU_MODE_PARTY begin=%s\n", error);
      }
    } else if (lastModePartyBeaconTxAt == 0U ||
               static_cast<uint32_t>(now - lastModePartyBeaconTxAt) >=
                   PARTY_BEACON_INTERVAL_MS) {
      const kitsu868::party_modes::HostState before =
          modePartyHost.snapshot();
      kitsu868::party_modes::Packet beacon{};
      const kitsu868::party_modes::Status status =
          modePartyHost.makeBeacon(now, beacon);
      lastModePartyBeaconTxAt = now;
      if (status == kitsu868::party_modes::Status::Ok &&
          !sendModePartyPacket(beacon, false)) {
        (void)modePartyHost.restore(before);
      }
    }
  }

  host = modePartyHost.snapshot();
  hostPhase = static_cast<kitsu868::party_modes::HostPhase>(host.phase);
  if (hostPhase == kitsu868::party_modes::HostPhase::Active) {
    if (lastModePartyBeaconTxAt == 0U ||
        static_cast<uint32_t>(now - lastModePartyBeaconTxAt) >=
            PARTY_BEACON_INTERVAL_MS) {
      const kitsu868::party_modes::HostState before =
          modePartyHost.snapshot();
      kitsu868::party_modes::Packet beacon{};
      const kitsu868::party_modes::Status status =
          modePartyHost.makeBeacon(now, beacon);
      lastModePartyBeaconTxAt = now;
      if (status == kitsu868::party_modes::Status::Ok &&
          !sendModePartyPacket(beacon, false)) {
        (void)modePartyHost.restore(before);
      }
    }
    if (lastModePartyTxAt == 0U ||
        static_cast<uint32_t>(now - lastModePartyTxAt) >= 1000UL) {
      const kitsu868::party_modes::HostState before =
          modePartyHost.snapshot();
      kitsu868::party_modes::Packet next{};
      const kitsu868::party_modes::Status status =
          modePartyHost.advance(now, next);
      if (status == kitsu868::party_modes::Status::Ok) {
        if (!sendModePartyPacket(next, true)) {
          (void)modePartyHost.restore(before);
        } else if (next.type ==
                   kitsu868::party_modes::PacketType::RoundOpen) {
          const kitsu868::party_modes::HostState advanced =
              modePartyHost.snapshot();
          printModePartyPrompt(
              static_cast<kitsu868::party_modes::Mode>(advanced.mode),
              advanced.seed, advanced.currentRound, true);
        }
      } else if (status != kitsu868::party_modes::Status::NotReady) {
        Serial.printf("KITSU_MODE_PARTY advance=%s\n",
                      kitsu868::party_modes::statusName(status));
      }
    }
  }

  const kitsu868::party_modes::ParticipantState guest =
      modePartyParticipant.snapshot();
  const kitsu868::party_modes::ParticipantPhase guestPhase =
      static_cast<kitsu868::party_modes::ParticipantPhase>(guest.phase);
  if (modePartyGuestInProgress(guestPhase)) {
    const kitsu868::party_modes::Status expiry =
        modePartyParticipant.expireIfNeeded(now);
    if (expiry == kitsu868::party_modes::Status::TimedOut) {
      modePartyJoinRequested = false;
      modePartyAutoReadyPending = false;
      Serial.println("KITSU_MODE_PARTY guest=expired");
    } else if (modePartyAutoReadyPending &&
               guestPhase ==
                   kitsu868::party_modes::ParticipantPhase::Lobby &&
               (lastModePartyTxAt == 0U ||
                static_cast<uint32_t>(now - lastModePartyTxAt) >= 1000UL)) {
      const char* error = nullptr;
      if (!setModePartyReady(true, error) && error &&
          strcmp(error, "party_send_failed") != 0) {
        Serial.printf("KITSU_MODE_PARTY auto_ready=%s\n", error);
      }
    }
  }

  finishModePartyIfComplete(now);
  if (modePartyReplayRemaining != 0U && radioListening &&
      static_cast<int32_t>(now - modePartyReplayAt) >= 0) {
    const kitsu868::party_modes::Packet replay = modePartyReplayPacket;
    --modePartyReplayRemaining;
    modePartyReplayAt = now + PARTY_REPLAY_INTERVAL_MS;
    (void)sendModePartyPacketNow(replay);
    if (modePartyReplayRemaining == 0U) clearModePartyReplay();
  } else if (modePartyReplayRemaining != 0U && !radioListening) {
    clearModePartyReplay();
  }
}

NearbyNeighbor* nearbyNeighbor(uint16_t uid, bool allocate) {
  NearbyNeighbor* oldest = nullptr;
  uint32_t oldestAge = 0U;
  const uint32_t now = millis();
  for (NearbyNeighbor& neighbor : nearbyNeighbors) {
    if (neighbor.used && neighbor.uid == uid) return &neighbor;
    if (!neighbor.used) {
      if (allocate && !oldest) oldest = &neighbor;
      continue;
    }
    const uint32_t age = now - neighbor.lastSeenAt;
    if (allocate && (!oldest || age > oldestAge)) {
      oldest = &neighbor;
      oldestAge = age;
    }
  }
  if (!allocate || !oldest) return nullptr;
  *oldest = NearbyNeighbor{};
  oldest->used = true;
  oldest->uid = uid;
  oldest->nextSequence = nearbySequenceCursor;
  return oldest;
}

bool sendNearbyPacket(const kitsu868::nearby::Packet& packet) {
  uint8_t wire[kitsu868::nearby::kWireBytes]{};
  size_t wireBytes = 0U;
  if (kitsu868::nearby::encode(packet, wire, sizeof(wire), &wireBytes) !=
      kitsu868::nearby::Status::Ok) {
    return false;
  }
  const kitsu868::mesh::TransportStatus status =
      meshTransport.sendNearbyRadioFrame(meshSettings, wire, wireBytes, true);
  memset(wire, 0, sizeof(wire));
  if (status != kitsu868::mesh::TransportStatus::Ok) {
    Serial.printf("KITSU_NEARBY_TX result=%s\n",
                  kitsu868::mesh::transportStatusName(status));
    return false;
  }
  return true;
}

bool sendNearbyPresence() {
  if (!radioListening || nearbySessionNonce == 0U ||
      !companionPack.valid()) {
    return false;
  }
  kitsu868::nearby::Packet presence{};
  presence.type = kitsu868::nearby::PacketType::Presence;
  presence.sourceUid = ownUidSuffix();
  presence.sessionNonce = nearbySessionNonce;
  presence.packId = companionPack.id();
  presence.appearance = min<uint8_t>(
      kitsu868::nearby::kMaxAppearance,
      companionBrain.appearanceVariant());
  presence.evolutionStage = min<uint8_t>(
      kitsu868::nearby::kMaxEvolutionStage,
      static_cast<uint8_t>(companionBrain.evolutionStage()));
  presence.bond = min<uint8_t>(
      kitsu868::nearby::kMaxBond,
      static_cast<uint8_t>(min<uint16_t>(
          UINT8_MAX, static_cast<uint16_t>(companionBrain.bondLevel()) * 10U)));
  presence.mood = min<uint8_t>(
      kitsu868::nearby::kMaxMood,
      static_cast<uint8_t>(companionBrain.mood(companionVitals())));
  const uint32_t now = millis();
  const bool sent = sendNearbyPacket(presence);
  if (sent) {
    const uint32_t jitterRange =
        NEARBY_PRESENCE_HEARTBEAT_MAX_MS -
        NEARBY_PRESENCE_HEARTBEAT_MIN_MS + 1UL;
    nextNearbyPresenceAt = now + NEARBY_PRESENCE_HEARTBEAT_MIN_MS +
        (esp_random() % jitterRange);
  } else {
    nextNearbyPresenceAt = now + NEARBY_PRESENCE_RETRY_MS;
  }
  return sent;
}

kitsu868::presence::ObserveResult observePetPresence(
    const kitsu868::nearby::Packet& packet, float rssi, float snr,
    uint32_t now, bool familiarHint) {
  kitsu868::presence::Observation observation{};
  observation.uid = packet.sourceUid;
  observation.rssiDbm = rssi;
  observation.snrDb = snr;
  observation.observedAtMs = now;
  // The hint comes from the existing remembered-peer filter. Nearby-v2 has
  // CRC validation but no peer authentication, so this remains familiarity,
  // never ownership or identity proof.
  observation.familiarHint = familiarHint;
  const kitsu868::presence::ObserveResult result =
      petPresenceTracker.observe(observation);
  if (result.status == kitsu868::presence::ObserveStatus::Ok) {
    Serial.printf(
        "KITSU_PET_PRESENCE uid=%04X band=%s trend=%s familiar=%s "
        "group=%u rssi=%.1f snr=%.1f\n",
        result.peer.uid, kitsu868::presence::signalBandName(result.peer.band),
        kitsu868::presence::signalTrendName(result.peer.trend),
        result.peer.familiar ? "true" : "false", result.group.presentCount,
        result.peer.smoothedRssiDbm, result.peer.latestSnrDb);
  } else {
    Serial.printf("KITSU_PET_PRESENCE uid=%04X result=%s\n",
                  packet.sourceUid,
                  kitsu868::presence::observeStatusName(result.status));
  }
  return result;
}

void presentPetPresenceEvents(uint16_t events) {
  if (!radioListening ||
      (activeAnimation.active && activeAnimation.finite)) {
    return;
  }
  CompanionRole role = CompanionRole::Listen;
  if (kitsu868::presence::hasEvent(
          events, kitsu868::presence::EventGroupStarted)) {
    role = CompanionRole::Play;
  } else if (kitsu868::presence::hasEvent(
                 events, kitsu868::presence::EventAppeared)) {
    role = kitsu868::presence::hasEvent(
               events, kitsu868::presence::EventFamiliar)
               ? CompanionRole::Surprise
               : CompanionRole::Meet;
  } else if (kitsu868::presence::hasEvent(
                 events, kitsu868::presence::EventApproaching)) {
    role = CompanionRole::Surprise;
  } else if (kitsu868::presence::hasEvent(
                 events, kitsu868::presence::EventLeaving) ||
             kitsu868::presence::hasEvent(
                 events, kitsu868::presence::EventGone)) {
    role = CompanionRole::Blink;
  } else {
    return;
  }
  cancelAmbientAnimation();
  if (!startTransientAnimation(role)) startBaseAnimation();
}

void tickPetPresence(uint32_t now) {
  const kitsu868::presence::ExpireResult expired =
      petPresenceTracker.expire(now);
  if (expired.events != kitsu868::presence::EventNone) {
    presentPetPresenceEvents(expired.events);
  }

  // The long party hotspot listen is a different feature and must never turn
  // into a periodic pet beacon. Heartbeats belong only to normal Listen.
  if (!radioListening || partyRuntimeBusy()) return;
  if (nextNearbyPresenceAt != 0U &&
      static_cast<int32_t>(now - nextNearbyPresenceAt) < 0) {
    return;
  }
  (void)sendNearbyPresence();
}

kitsu868::mesh::TransportStatus queueNearbyAction(
    uint16_t targetUid, uint32_t targetSessionNonce, uint16_t sequence,
    kitsu868::nearby::PositiveAction action) {
  NearbyNeighbor* neighbor = nearbyNeighbor(targetUid, false);
  if (pendingNearbyAction.active &&
      static_cast<uint32_t>(millis() - pendingNearbyAction.sentAt) <=
          30000UL) {
    return kitsu868::mesh::TransportStatus::SendBusy;
  }
  pendingNearbyAction = PendingNearbyAction{};
  if (!neighbor || targetUid == ownUidSuffix() ||
      neighbor->sessionNonce != targetSessionNonce || sequence == 0U ||
      static_cast<uint32_t>(millis() - neighbor->lastSeenAt) >
          NEARBY_NEIGHBOR_TTL_MS) {
    return kitsu868::mesh::TransportStatus::ContactNotFound;
  }
  kitsu868::nearby::Packet request{};
  request.type = kitsu868::nearby::PacketType::ActionRequest;
  request.sourceUid = ownUidSuffix();
  request.targetUid = targetUid;
  request.sessionNonce = targetSessionNonce;
  request.requestSequence = sequence;
  request.action = action;
  uint8_t wire[kitsu868::nearby::kWireBytes]{};
  size_t wireBytes = 0U;
  if (kitsu868::nearby::encode(request, wire, sizeof(wire), &wireBytes) !=
      kitsu868::nearby::Status::Ok) {
    return kitsu868::mesh::TransportStatus::InvalidArgument;
  }
  // Reserve the sequence durably before transmission. A reboot can create a
  // gap after a failed radio send, but can never reuse an already-sent token.
  if (!reserveNearbySequence(sequence)) {
    memset(wire, 0, sizeof(wire));
    return kitsu868::mesh::TransportStatus::MessagingStorageFailed;
  }
  const kitsu868::mesh::TransportStatus status =
      meshTransport.sendNearbyRadioFrame(meshSettings, wire, wireBytes, true);
  memset(wire, 0, sizeof(wire));
  if (status == kitsu868::mesh::TransportStatus::Ok) {
    pendingNearbyAction.active = true;
    pendingNearbyAction.request = request;
    pendingNearbyAction.sentAt = millis();
  }
  return status;
}

void processNearbyPresence(const kitsu868::nearby::Packet& packet,
                           float rssi, float snr) {
  if (!radioListening || packet.sourceUid == ownUidSuffix() ||
      (packet.targetUid != 0U && packet.targetUid != ownUidSuffix())) {
    return;
  }
  const uint32_t now = millis();
  NearbyNeighbor* neighbor = nearbyNeighbor(packet.sourceUid, true);
  if (!neighbor) return;
  const bool newMeeting = neighbor->sessionNonce != packet.sessionNonce;
  neighbor->sessionNonce = packet.sessionNonce;
  neighbor->packId = packet.packId;
  neighbor->appearance = packet.appearance;
  neighbor->evolutionStage = packet.evolutionStage;
  neighbor->bond = packet.bond;
  neighbor->mood = packet.mood;
  neighbor->emote = packet.emote;
  neighbor->rssi = rssi;
  neighbor->snr = snr;
  neighbor->lastSeenAt = now;
  companionBleRefreshDirty = true;

  kitsu868::social::FriendOutcome socialOutcome{};
  bool socialGreeting = false;
  bool socialObservationRecorded = false;
  uint32_t socialDay = 0U;
  LocalClockSample socialClock{};
  if (socialProgressionReady && localClockSample(now, true, socialClock)) {
    socialDay = socialClock.dayId;
    const kitsu868::social::SocialState before = socialProgression.snapshot();
    bool observedToday = false;
    for (uint8_t index = 0U; index < before.peerCount; ++index) {
      if (before.peers[index].uid == packet.sourceUid &&
          before.peers[index].lastSeenDay == socialDay) {
        observedToday = true;
        break;
      }
    }
    if (!observedToday) {
      kitsu868::social::FriendObservation observation{};
      observation.uid = packet.sourceUid;
      observation.dayId = socialDay;
      observation.epochSeconds = socialClock.epochSeconds;
      observation.peerIsNewcomer = packet.bond <= 1U ? 1U : 0U;
      observation.localBondLevel = companionBrain.bondLevel();
      const kitsu868::social::SocialStatus status =
          socialProgression.observeFriend(observation, socialOutcome);
      if (status == kitsu868::social::SocialStatus::Ok) {
        if (persistSocialProgression()) {
          socialGreeting = true;
          socialObservationRecorded = true;
        } else {
          (void)socialProgression.restore(before);
          Serial.println("KITSU_WARN social_friend=store_failed");
        }
      }
    }
  }

  if (socialObservationRecorded && companionProgressionReady) {
    uint8_t before[kitsu868::progression::kSnapshotCapacity]{};
    if (captureCompanionProgression(before) &&
        companionProgression.rememberFriend(
            static_cast<uint8_t>(packet.sourceUid & 0xffU), socialDay) &&
        !persistCompanionProgression()) {
      restoreCompanionProgression(before);
      Serial.println("KITSU_WARN friend_memory=store_failed");
    }
  }

  if (!newMeeting) {
    const kitsu868::presence::ObserveResult observed =
        observePetPresence(packet, rssi, snr, now, false);
    presentPetPresenceEvents(observed.events);
    if (socialGreeting) {
      lastMemory = "A familiar Kitsu returned.";
      momentView.active = true;
      momentView.line1 = kitsu868::social::greetingLine1(
          socialOutcome.greeting);
      momentView.line2 = kitsu868::social::greetingLine2(
          socialOutcome.greeting);
      momentView.until = now + MOMENT_DISPLAY_MS;
      radioProgressDirty = true;
      Serial.printf(
          "KITSU_NEARBY_PRESENCE uid=%04X pack=%08lX new=false "
          "rssi=%.1f snr=%.1f\n",
          packet.sourceUid, static_cast<unsigned long>(packet.packId), rssi,
          snr);
    }
    return;
  }

  char peerIdentity[7]{};
  snprintf(peerIdentity, sizeof(peerIdentity), "KT%04X", packet.sourceUid);
  const kitsu868::BrainEventResult result = companionBrain.onEncounter(
      kitsu868::CompanionBrain::fingerprint(peerIdentity));
  (void)observePetPresence(packet, rssi, snr, now, !result.newEncounter);
  wisp.curiosity = min<uint8_t>(100U, wisp.curiosity + 8U);
  wisp.affection = min<uint8_t>(
      100U, wisp.affection + static_cast<uint8_t>(
          result.newEncounter ? 2U : 1U));
  lastMemory = result.newEncounter ? "A nearby Kitsu came to meet."
                                   : "A familiar Kitsu returned.";
  startReaction(result.newEncounter ? CompanionRole::Meet
                                    : CompanionRole::Surprise,
                result);
  if (socialGreeting) {
    momentView.active = true;
    momentView.line1 = kitsu868::social::greetingLine1(socialOutcome.greeting);
    momentView.line2 = kitsu868::social::greetingLine2(socialOutcome.greeting);
    momentView.until = millis() + MOMENT_DISPLAY_MS;
  }
  if (result.newEncounter) {
    recordSuccessfulEncounterTrigger(
        kitsu868::signal::MeshOperationKind::NearbyKitsuMet);
  }
  radioProgressDirty = true;
  Serial.printf(
      "KITSU_NEARBY_PRESENCE uid=%04X pack=%08lX new=%s rssi=%.1f snr=%.1f\n",
      packet.sourceUid, static_cast<unsigned long>(packet.packId),
      result.newEncounter ? "true" : "false", rssi, snr);
}

const char* nearbyActionName(kitsu868::nearby::PositiveAction action) {
  using kitsu868::nearby::PositiveAction;
  switch (action) {
    case PositiveAction::None: return "none";
    case PositiveAction::Pet: return "pet";
    case PositiveAction::Greet: return "greet";
    case PositiveAction::Play: return "play";
    case PositiveAction::Gift: return "gift";
  }
  return "unknown";
}

bool nearbySequenceAfter(uint16_t candidate, uint16_t reference) {
  const uint16_t delta = static_cast<uint16_t>(candidate - reference);
  return delta != 0U && delta < 0x8000U;
}

void sendNearbyActionResult(const kitsu868::nearby::Packet& request,
                            kitsu868::nearby::ActionResult outcome) {
  kitsu868::nearby::Packet response{};
  if (kitsu868::nearby::makeActionResult(request, outcome, response) &&
      !sendNearbyPacket(response)) {
    Serial.printf("KITSU_WARN nearby_ack=send_failed sequence=%u\n",
                  request.requestSequence);
  }
}

void processNearbyActionRequest(const kitsu868::nearby::Packet& packet) {
  if (packet.targetUid != ownUidSuffix() ||
      packet.sessionNonce != nearbySessionNonce) {
    return;
  }
  NearbyNeighbor* neighbor = nearbyNeighbor(packet.sourceUid, false);
  if (!neighbor) return;
  if (kitsu868::nearby::isDuplicate(neighbor->lastInbound, packet)) {
    if (neighbor->hasInboundResult) {
      sendNearbyActionResult(packet, neighbor->lastInboundResult);
    }
    return;
  }
  if (neighbor->lastInbound.valid != 0U &&
      !nearbySequenceAfter(packet.requestSequence,
                           neighbor->lastInbound.requestSequence)) {
    sendNearbyActionResult(packet, kitsu868::nearby::ActionResult::Disabled);
    Serial.printf("KITSU_NEARBY_ACTION uid=%04X sequence=%u result=stale\n",
                  packet.sourceUid, packet.requestSequence);
    return;
  }
  kitsu868::nearby::ActionResult outcome =
      kitsu868::nearby::ActionResult::Disabled;
  const uint32_t now = millis();
  if (companionPack.valid() &&
      (neighbor->lastAcceptedActionAt == 0U ||
       static_cast<uint32_t>(now - neighbor->lastAcceptedActionAt) >=
           30000UL)) {
    // Nearby actions are tiny social acknowledgements, never owner-care XP.
    CompanionRole role = CompanionRole::Meet;
    const char* memory = nullptr;
    const uint8_t previousEnergy = wisp.energy;
    const uint8_t previousCuriosity = wisp.curiosity;
    const uint8_t previousAffection = wisp.affection;
    const uint16_t previousGifts = collectedGifts;
    const uint8_t previousLastGift = lastEncounterGift;
    if (packet.action == kitsu868::nearby::PositiveAction::Pet) {
      wisp.affection = min<uint8_t>(100U, wisp.affection + 1U);
      memory = "A nearby friend petted your companion.";
      role = CompanionRole::Pet;
    } else if (packet.action == kitsu868::nearby::PositiveAction::Greet) {
      wisp.curiosity = min<uint8_t>(100U, wisp.curiosity + 1U);
      memory = "A nearby Kitsu waved hello.";
      role = CompanionRole::Meet;
    } else if (packet.action == kitsu868::nearby::PositiveAction::Play) {
      wisp.curiosity = min<uint8_t>(100U, wisp.curiosity + 2U);
      wisp.energy = wisp.energy > 1U ? wisp.energy - 1U : 1U;
      memory = "A nearby Kitsu invited a quick game.";
      role = CompanionRole::Play;
    } else if (packet.action == kitsu868::nearby::PositiveAction::Gift) {
      const uint8_t gift = static_cast<uint8_t>(
          (static_cast<uint32_t>(packet.sourceUid) ^
           static_cast<uint32_t>(packet.targetUid) ^ packet.sessionNonce ^
           packet.requestSequence) % kitsu868::encounter::kGiftCount);
      collectedGifts |= static_cast<uint16_t>(1U << gift);
      lastEncounterGift = gift;
      wisp.affection = min<uint8_t>(100U, wisp.affection + 1U);
      memory = "A nearby Kitsu shared a tiny gift.";
      role = CompanionRole::Surprise;
    }
    if (memory && saveState()) {
      neighbor->lastAcceptedActionAt = now;
      lastMemory = memory;
      cancelAmbientAnimation();
      if (!startTransientAnimation(role)) startBaseAnimation();
      kitsu868::dialogue::Action dialogueAction =
          kitsu868::dialogue::Action::Meet;
      if (packet.action == kitsu868::nearby::PositiveAction::Pet) {
        dialogueAction = kitsu868::dialogue::Action::Pet;
      } else if (packet.action == kitsu868::nearby::PositiveAction::Play) {
        dialogueAction = kitsu868::dialogue::Action::Play;
      } else if (packet.action == kitsu868::nearby::PositiveAction::Gift) {
        dialogueAction = kitsu868::dialogue::Action::Gift;
      }
      showActionDialogue(dialogueAction,
                         kitsu868::dialogue::ActionOutcome::Success, true,
                         false);
      outcome = kitsu868::nearby::ActionResult::Accepted;
    } else {
      wisp.energy = previousEnergy;
      wisp.curiosity = previousCuriosity;
      wisp.affection = previousAffection;
      collectedGifts = previousGifts;
      lastEncounterGift = previousLastGift;
    }
  } else if (companionPack.valid()) {
    outcome = kitsu868::nearby::ActionResult::Busy;
  }
  neighbor->lastInbound = kitsu868::nearby::makeDuplicateToken(packet);
  neighbor->lastInboundResult = outcome;
  neighbor->hasInboundResult = true;
  sendNearbyActionResult(packet, outcome);
  companionBleRefreshDirty = true;
  Serial.printf("KITSU_NEARBY_ACTION uid=%04X action=%s result=%s\n",
                packet.sourceUid, nearbyActionName(packet.action),
                outcome == kitsu868::nearby::ActionResult::Accepted
                    ? "accepted"
                    : outcome == kitsu868::nearby::ActionResult::Busy
                          ? "busy" : "disabled");
}

void processNearbyRadio() {
  kitsu868::mesh::NearbyRadioFrame frame{};
  while (meshTransport.takeNearbyRadioFrame(frame)) {
    if (frame.length == kitsu868::party_modes::kWireBytes &&
        frame.bytes[0] == kitsu868::party_modes::kMagic0 &&
        frame.bytes[1] == kitsu868::party_modes::kMagic1) {
      kitsu868::party_modes::Packet modePacket{};
      const kitsu868::party_modes::Status modeStatus =
          kitsu868::party_modes::decode(frame.bytes, frame.length,
                                        modePacket);
      if (modeStatus == kitsu868::party_modes::Status::Ok) {
        processModePartyRadioPacket(modePacket, frame.rssi, frame.snr);
      } else {
        Serial.printf("KITSU_MODE_PARTY_RX result=%s\n",
                      kitsu868::party_modes::statusName(modeStatus));
      }
      continue;
    }
    if (frame.length == kitsu868::party::kWireBytes &&
        frame.bytes[0] == kitsu868::party::kMagic0 &&
        frame.bytes[1] == kitsu868::party::kMagic1) {
      kitsu868::party::Packet partyPacket{};
      const kitsu868::party::Status partyStatus = kitsu868::party::decode(
          frame.bytes, frame.length, partyPacket);
      if (partyStatus == kitsu868::party::Status::Ok) {
        processPartyRadioPacket(partyPacket, frame.rssi, frame.snr);
      } else {
        Serial.printf("KITSU_PARTY_RX result=%s\n",
                      kitsu868::party::statusName(partyStatus));
      }
      continue;
    }
    kitsu868::nearby::Packet packet{};
    if (kitsu868::nearby::decode(frame.bytes, frame.length, packet) !=
        kitsu868::nearby::Status::Ok ||
        packet.sourceUid == ownUidSuffix()) {
      continue;
    }
    if (packet.type == kitsu868::nearby::PacketType::Presence) {
      processNearbyPresence(packet, frame.rssi, frame.snr);
    } else if (packet.type == kitsu868::nearby::PacketType::ActionRequest) {
      processNearbyActionRequest(packet);
    } else if (packet.type == kitsu868::nearby::PacketType::ActionResult &&
               packet.targetUid == ownUidSuffix() &&
               pendingNearbyAction.active &&
               kitsu868::nearby::actionResultAcknowledges(
                   pendingNearbyAction.request, packet)) {
      const kitsu868::nearby::PositiveAction acceptedAction =
          pendingNearbyAction.request.action;
      pendingNearbyAction = PendingNearbyAction{};
      const char* const action = nearbyActionName(acceptedAction);
      if (packet.result == kitsu868::nearby::ActionResult::Accepted) {
        lastMemory = String("Nearby action accepted: ") + action + ".";
      } else if (packet.result == kitsu868::nearby::ActionResult::Busy) {
        lastMemory = String("Nearby friend was busy: ") + action + ".";
      } else {
        lastMemory = String("Nearby action declined: ") + action + ".";
      }
      kitsu868::dialogue::Action dialogueAction =
          kitsu868::dialogue::Action::Meet;
      if (acceptedAction == kitsu868::nearby::PositiveAction::Pet) {
        dialogueAction = kitsu868::dialogue::Action::Pet;
      } else if (acceptedAction == kitsu868::nearby::PositiveAction::Play) {
        dialogueAction = kitsu868::dialogue::Action::Play;
      } else if (acceptedAction == kitsu868::nearby::PositiveAction::Gift) {
        dialogueAction = kitsu868::dialogue::Action::Gift;
      }
      kitsu868::dialogue::ActionOutcome dialogueOutcome =
          kitsu868::dialogue::ActionOutcome::NoReply;
      if (packet.result == kitsu868::nearby::ActionResult::Accepted) {
        dialogueOutcome = kitsu868::dialogue::ActionOutcome::Success;
      } else if (packet.result == kitsu868::nearby::ActionResult::Busy) {
        dialogueOutcome = kitsu868::dialogue::ActionOutcome::Busy;
      } else if (packet.result == kitsu868::nearby::ActionResult::Declined ||
                 packet.result == kitsu868::nearby::ActionResult::Disabled) {
        dialogueOutcome = kitsu868::dialogue::ActionOutcome::Failed;
      }
      showActionDialogue(dialogueAction, dialogueOutcome, true, false);
      companionBleRefreshDirty = true;
      Serial.printf("KITSU_NEARBY_ACK uid=%04X sequence=%u result=%u\n",
                    packet.sourceUid, packet.requestSequence,
                    static_cast<unsigned>(packet.result));
    }
  }
}

void handleEncounter(const kitsu868::encounter::Packet& peer) {
  if (peer.uid == ownUidSuffix()) {
    Serial.println("KITSU_ENCOUNTER ignored=self");
    return;
  }

  if (hasEncounterDuplicate &&
      kitsu868::encounter::isDuplicate(lastEncounterDuplicate, peer)) {
    Serial.printf("KITSU_ENCOUNTER ignored=duplicate uid=%04X\n", peer.uid);
    return;
  }
  lastEncounterDuplicate = kitsu868::encounter::makeDuplicateToken(peer);
  hasEncounterDuplicate = true;

  kitsu868::encounter::Packet local{};
  local.type = kitsu868::encounter::PacketType::Reply;
  local.uid = ownUidSuffix();
  local.packId = companionPack.id();
  local.appearance = companionBrain.appearanceVariant();
  local.evolutionStage = static_cast<uint8_t>(companionBrain.evolutionStage());
  local.bond = static_cast<uint8_t>(min<uint16_t>(
      100, static_cast<uint16_t>(companionBrain.bondLevel() * 10U)));
  local.mood = static_cast<uint8_t>(companionBrain.mood(companionVitals()));
  local.emote = 0;
  // RX-only v0.7 has no Reply exchange.  Derive a stable local half from the
  // device and received Offer so replaying the same valid packet after reboot
  // yields the same local cosmetic result.  A future bidirectional handshake
  // will instead use the actual Reply nonce already supported by the codec.
  local.nonce = companionBrain.deviceFingerprint() ^ peer.nonce ^
      companionPack.id() ^ (static_cast<uint32_t>(peer.uid) << 16U) ^
      0x4b383638UL;
  local.nonce ^= local.nonce << 13U;
  local.nonce ^= local.nonce >> 17U;
  local.nonce ^= local.nonce << 5U;
  if (!local.nonce) local.nonce = 1;

  const kitsu868::encounter::SharedResult shared =
      kitsu868::encounter::deriveSharedResult(local, peer);
  lastPeerUid = peer.uid;
  lastEncounterTrait = shared.trait;
  lastEncounterGift = shared.gift;
  const bool newTrait = (unlockedTraits & (1U << shared.trait)) == 0;
  const bool newGift = (collectedGifts & (1U << shared.gift)) == 0;
  unlockedTraits |= 1U << shared.trait;
  collectedGifts |= 1U << shared.gift;

  char peerIdentity[7];
  snprintf(peerIdentity, sizeof(peerIdentity), "KT%04X", peer.uid);
  const uint32_t fingerprint = kitsu868::CompanionBrain::fingerprint(peerIdentity);
  const kitsu868::BrainEventResult result =
      companionBrain.onEncounter(fingerprint);
  wisp.curiosity = min<uint8_t>(100, wisp.curiosity + 8U);
  wisp.affection = min<uint8_t>(100, wisp.affection +
      static_cast<uint8_t>(result.newEncounter ? 2U : 1U));
  lastMemory = result.newEncounter ? "A new companion shared a tiny gift."
                                   : "A familiar signal returned.";
  startReaction(result.newEncounter ? CompanionRole::Meet
                                    : CompanionRole::Surprise,
                result);
  if (radioListening) radioProgressDirty = true;
  else persistProgress();
  Serial.printf(
      "KITSU_ENCOUNTER uid=%04X pack=%08lX new=%s trait=%u gift=%u "
      "new_trait=%s new_gift=%s rssi=%.1f snr=%.1f tx_enabled=%s\n",
      peer.uid, static_cast<unsigned long>(peer.packId),
      result.newEncounter ? "true" : "false", shared.trait, shared.gift,
      newTrait ? "true" : "false", newGift ? "true" : "false",
      lastRssi, lastSnr, ENCOUNTER_TX_ENABLED ? "true" : "false");
}

const char* signalOperationName(kitsu868::signal::MeshOperationKind kind) {
  using kitsu868::signal::MeshOperationKind;
  switch (kind) {
    case MeshOperationKind::RepeaterDiscovered: return "mesh_repeater";
    case MeshOperationKind::PeerDiscovered: return "mesh_peer";
    case MeshOperationKind::MessageSent: return "mesh_message_tx";
    case MeshOperationKind::MessageReceived: return "mesh_message_rx";
    case MeshOperationKind::AdvertSent: return "mesh_advert_tx";
    case MeshOperationKind::AdvertReceived: return "mesh_advert_rx";
    case MeshOperationKind::OtherCompleted: return "mesh_other";
    case MeshOperationKind::NearbyKitsuMet: return "kitsu_neighbor";
    case MeshOperationKind::Count: break;
  }
  return "mesh_other";
}

const char* signalRarityName(kitsu868::signal::Rarity rarity) {
  using kitsu868::signal::Rarity;
  switch (rarity) {
    case Rarity::Common: return "common";
    case Rarity::Uncommon: return "uncommon";
    case Rarity::Rare: return "rare";
    case Rarity::VeryRare: return "very_rare";
    case Rarity::Epic: return "epic";
    case Rarity::Legendary: return "legendary";
    case Rarity::Mythical: return "mythical";
    case Rarity::Count: break;
  }
  return "common";
}

kitsu868::unlocks::Rarity unlockRarity(kitsu868::signal::Rarity rarity) {
  const uint8_t raw = static_cast<uint8_t>(rarity);
  return raw <= static_cast<uint8_t>(kitsu868::unlocks::Rarity::Mythical)
             ? static_cast<kitsu868::unlocks::Rarity>(raw)
             : kitsu868::unlocks::Rarity::Common;
}

bool addEncounterCode(const kitsu868::wild::Creature& creature,
                       kitsu868::signal::MeshOperationKind source) {
  if (!creature.packPublished || !encounterCodesReady ||
      encounterCodes.serializedBytes() > sizeof(encounterCodeRollback)) {
    return false;
  }
  kitsu868::unlocks::CodeRecord existing{};
  if (encounterCodes.findByPackId(creature.packId, existing)) {
    existing = kitsu868::unlocks::CodeRecord{};
    return true;
  }
  size_t rollbackBytes = 0U;
  memset(encounterCodeRollback, 0, sizeof(encounterCodeRollback));
  if (!encounterCodes.serialize(encounterCodeRollback,
                                sizeof(encounterCodeRollback),
                                rollbackBytes)) {
    return false;
  }
  for (uint8_t attempt = 0U; attempt < 4U; ++attempt) {
    uint8_t entropy[10]{};
    for (size_t offset = 0U; offset < sizeof(entropy); offset += 4U) {
      const uint32_t random = esp_random();
      const size_t available = min<size_t>(4U, sizeof(entropy) - offset);
      memcpy(entropy + offset, &random, available);
    }
    kitsu868::unlocks::CodeRecord record{};
    if (!kitsu868::unlocks::CodeStore::generateCode(entropy, record.code)) {
      continue;
    }
    record.codeId = kitsu868::unlocks::CodeStore::codeId(record.code);
    record.packId = creature.packId;
    const size_t nameBytes = min<size_t>(
        strlen(creature.name), sizeof(record.creatureName) - 1U);
    memcpy(record.creatureName, creature.name, nameBytes);
    record.creatureName[nameBytes] = '\0';
    record.rarity = unlockRarity(creature.rarity);
    const char* const sourceName = signalOperationName(source);
    const size_t sourceBytes = min<size_t>(
        strlen(sourceName), sizeof(record.source) - 1U);
    memcpy(record.source, sourceName, sourceBytes);
    record.source[sourceBytes] = '\0';
    record.acquiredAtEpoch = meshTransport.timeValid()
                                 ? meshTransport.currentEpoch()
                                 : 0U;
    const kitsu868::unlocks::AddResult added = encounterCodes.add(record);
    memset(entropy, 0, sizeof(entropy));
    if (added == kitsu868::unlocks::AddResult::Duplicate) continue;
    if (added != kitsu868::unlocks::AddResult::Added) break;
    if (persistEncounterCodes()) {
      memset(encounterCodeRollback, 0, sizeof(encounterCodeRollback));
      return true;
    }
    (void)encounterCodes.load(encounterCodeRollback, rollbackBytes);
    memset(encounterCodeRollback, 0, sizeof(encounterCodeRollback));
    Serial.println("KITSU_WARN unlock_code=flush_failed");
    return false;
  }
  memset(encounterCodeRollback, 0, sizeof(encounterCodeRollback));
  return false;
}

bool materializePendingWildEncounter() {
  if (pendingWildOperationId == 0U || !wildEncounterView.available ||
      !wildEncounterView.creature.name) {
    return false;
  }
  const bool wantsCode =
      pendingWildCodeOutcome == kitsu868::signal::CodeOutcome::Revealed;
  const bool codeReady = !wantsCode ||
      addEncounterCode(wildEncounterView.creature, wildEncounterView.source);
  wildEncounterView.codeRevealed = wantsCode && codeReady;

  bool guideReady = false;
  if (funStateReady) {
    if (lastFunEncounterOperationId == pendingWildOperationId) {
      guideReady = true;
    } else if (lastFunEncounterOperationId < pendingWildOperationId) {
      for (uint8_t index = 0U;
           index < kitsu868::fun::kCatalogCreatureCount; ++index) {
        kitsu868::wild::Creature catalog{};
        if (!kitsu868::wild::creatureAt(index, catalog) ||
            catalog.packId != wildEncounterView.creature.packId) {
          continue;
        }
        const kitsu868::fun::DiscoveryState previous = funDiscovery;
        const uint64_t previousOperation = lastFunEncounterOperationId;
        if (kitsu868::fun::recordCreatureEncounter(
                funDiscovery, index,
                static_cast<uint8_t>(wildEncounterView.source))) {
          lastFunEncounterOperationId = pendingWildOperationId;
          guideReady = persistFunState();
        }
        if (!guideReady) {
          funDiscovery = previous;
          lastFunEncounterOperationId = previousOperation;
        }
        break;
      }
    }
  }
  if (!codeReady) Serial.println("KITSU_WARN pending_wild=code_pending");
  if (!guideReady) Serial.println("KITSU_WARN pending_wild=guide_pending");
  return codeReady && guideReady;
}

bool wildEncounterMayInterrupt() {
  return activeGame == ActiveGame::None && !radioListening &&
      screen != Screen::Activity && screen != Screen::Clock &&
      screen != Screen::PairPhone && screen != Screen::ControllerManager &&
      screen != Screen::ControllerConfirm &&
      screen != Screen::ControllerResult;
}

void presentPendingWildEncounter() {
  if (!wildEncounterView.available || screen == Screen::WildEncounter ||
      !wildEncounterMayInterrupt()) {
    return;
  }
  enterScreen(Screen::WildEncounter);
}

void recordSuccessfulEncounterTrigger(
    kitsu868::signal::MeshOperationKind kind) {
  if (!signalEncounterStateReady ||
      !kitsu868::signal::validOperationKind(kind) ||
      pendingWildOperationId != 0U || wildEncounterView.available) {
    return;
  }
  const kitsu868::signal::CoordinatorState previous =
      signalEncounterCoordinator.snapshot();
  const kitsu868::signal::SignalTrailState previousTrail =
      signalTrail.snapshot();
  if (previous.lastOperationId == UINT64_MAX) return;
  kitsu868::signal::LogicalOperationEvent event{};
  event.operationId = previous.lastOperationId + 1U;
  event.kind = kind;
  event.successful = 1U;
  kitsu868::signal::EncounterRecord record{};
  const kitsu868::signal::ProcessStatus status =
      signalEncounterCoordinator.process(event, esp_random(), record);
  if (status != kitsu868::signal::ProcessStatus::RecordedEncounter &&
      status != kitsu868::signal::ProcessStatus::RecordedNoEncounter) {
    return;
  }
  kitsu868::signal::SignalTrailResult trailResult{};
  const kitsu868::signal::SignalTrailProcessStatus trailStatus =
      signalTrail.process(event, record.encounterOccurred, trailResult);
  if (trailStatus != kitsu868::signal::SignalTrailProcessStatus::RecordedMiss &&
      trailStatus !=
          kitsu868::signal::SignalTrailProcessStatus::RecordedEncounter) {
    (void)signalEncounterCoordinator.restore(previous);
    (void)signalTrail.restore(previousTrail);
    Serial.printf("KITSU_WARN signal_trail=%s\n",
                  kitsu868::signal::signalTrailProcessStatusName(trailStatus));
    return;
  }
  if (trailResult.encounterOccurred == 0U) {
    if (!persistSignalEncounterState()) {
      (void)signalEncounterCoordinator.restore(previous);
      (void)signalTrail.restore(previousTrail);
      Serial.println("KITSU_WARN signal_state=flush_failed");
      return;
    }
    recordSessionGoal(kitsu868::fun::SessionActivity::Signal);
    companionBleRefreshDirty = true;
    return;
  }

  kitsu868::signal::Rarity rarity = kitsu868::signal::Rarity::Common;
  kitsu868::signal::CodeOutcome codeOutcome =
      kitsu868::signal::CodeOutcome::NotApplicable;
  if (record.encounterOccurred != 0U) {
    rarity = static_cast<kitsu868::signal::Rarity>(record.rarity);
    codeOutcome =
        static_cast<kitsu868::signal::CodeOutcome>(record.codeOutcome);
  } else {
    if (!kitsu868::signal::rarityForRoll(
            SIGNAL_ENCOUNTER_CONFIGURATION,
            record.rarityRollBasisPoints, rarity)) {
      (void)signalEncounterCoordinator.restore(previous);
      (void)signalTrail.restore(previousTrail);
      Serial.println("KITSU_WARN signal_trail=rarity_resolution_failed");
      return;
    }
    codeOutcome = kitsu868::signal::codeOutcomeForRoll(
        SIGNAL_ENCOUNTER_CONFIGURATION, rarity,
        record.codeRollBasisPoints);
  }
  kitsu868::wild::Creature creature{};
  if (!kitsu868::wild::creatureForRarity(rarity, record.entropy, creature)) {
    (void)signalEncounterCoordinator.restore(previous);
    (void)signalTrail.restore(previousTrail);
    Serial.println("KITSU_WARN wild_encounter=catalog_missing");
    return;
  }

  pendingWildOperationId = event.operationId;
  pendingWildEntropy = record.entropy;
  pendingWildCodeOutcome = codeOutcome;
  pendingWildMaterialized = false;
  wildEncounterView = WildEncounterView{};
  wildEncounterView.available = true;
  wildEncounterView.guaranteed = trailResult.guaranteed != 0U;
  wildEncounterView.creature = creature;
  wildEncounterView.source = kind;

  // Prepare the final outcome first, then commit the signal operation. On a
  // reset between these writes, boot compares operation IDs and discards an
  // uncommitted prepared record. Once both exist, the reward is recoverable.
  if (!persistPendingWildEncounter()) {
    (void)signalEncounterCoordinator.restore(previous);
    (void)signalTrail.restore(previousTrail);
    wildEncounterView = WildEncounterView{};
    pendingWildOperationId = 0U;
    pendingWildEntropy = 0U;
    pendingWildCodeOutcome = kitsu868::signal::CodeOutcome::NotApplicable;
    Serial.println("KITSU_WARN pending_wild=prepare_failed");
    return;
  }
  if (!persistSignalEncounterState()) {
    (void)signalEncounterCoordinator.restore(previous);
    (void)signalTrail.restore(previousTrail);
    if (!clearPendingWildEncounter()) {
      Serial.println("KITSU_WARN pending_wild=rollback_remove_failed");
    }
    wildEncounterView = WildEncounterView{};
    pendingWildOperationId = 0U;
    pendingWildEntropy = 0U;
    pendingWildCodeOutcome = kitsu868::signal::CodeOutcome::NotApplicable;
    Serial.println("KITSU_WARN signal_state=flush_failed");
    return;
  }
  if (adventureProgressionReady) {
    const kitsu868::adventure::ProgressState adventureBefore =
        adventureProgression.snapshot();
    kitsu868::signal::EncounterRecord effectiveRecord = record;
    effectiveRecord.encounterOccurred = 1U;
    effectiveRecord.guaranteed = trailResult.guaranteed;
    effectiveRecord.rarity = static_cast<uint8_t>(rarity);
    effectiveRecord.codeOutcome = static_cast<uint8_t>(codeOutcome);
    const kitsu868::adventure::Status adventureStatus =
        adventureProgression.applyEncounter(effectiveRecord);
    if ((adventureStatus == kitsu868::adventure::Status::Ok ||
        adventureStatus == kitsu868::adventure::Status::Duplicate) &&
        !persistAdventureProgression()) {
      (void)adventureProgression.restore(adventureBefore);
      Serial.println("KITSU_WARN adventure_encounter=store_failed");
    }
  }
  if (companionProgressionReady) {
    uint32_t day = 0U;
    uint16_t minute = 0U;
    uint8_t before[kitsu868::progression::kSnapshotCapacity]{};
    if (captureCompanionProgression(before) &&
        currentLocalDayMinute(day, minute, true) &&
        companionProgression.rememberEvent(
            static_cast<uint8_t>(rarity), day) &&
        !persistCompanionProgression()) {
      restoreCompanionProgression(before);
      Serial.println("KITSU_WARN encounter_memory=store_failed");
    }
  }
  recordSessionGoal(kitsu868::fun::SessionActivity::Signal);
  pendingWildMaterialized = materializePendingWildEncounter();
  lastMemory = "Something is approaching.";
  showMoment(kitsu868::fun::MomentTrigger::Encounter, true);
  companionBleRefreshDirty = true;
  Serial.printf(
      "KITSU_WILD_ENCOUNTER {\"protocol\":1,\"source\":\"%s\","
      "\"pack_id\":\"%08lX\",\"creature\":\"%s\","
      "\"rarity\":\"%s\",\"guaranteed\":%s,\"code_revealed\":%s}\n",
      signalOperationName(kind), static_cast<unsigned long>(creature.packId),
      creature.name, signalRarityName(rarity),
      wildEncounterView.guaranteed ? "true" : "false",
      wildEncounterView.codeRevealed ? "true" : "false");
}

const char* meshAdvertTypeName(uint8_t type) {
  switch (type) {
    case 1: return "client";
    case 2: return "repeater";
    case 3: return "room";
    case 4: return "sensor";
    default: return "unknown";
  }
}

void processMeshAdvert() {
  kitsu868::mesh::ReceivedAdvert advert{};
  while (meshTransport.takeAdvert(advert)) {
    lastRssi = advert.rssi;
    lastSnr = advert.snr;
    const String escapedName = jsonEscaped(String(advert.name));
    char publicPrefix[9];
    snprintf(publicPrefix, sizeof(publicPrefix), "%02X%02X%02X%02X",
             advert.publicKeyPrefix[0], advert.publicKeyPrefix[1],
             advert.publicKeyPrefix[2], advert.publicKeyPrefix[3]);
    Serial.printf(
        "KITSU_MESH_ADVERT {\"type\":\"%s\",\"name\":\"%s\"," 
        "\"kitsu_named\":%s,\"public_prefix\":\"%s\"," 
        "\"timestamp\":%lu,\"has_location\":%s,\"lat_e6\":%ld," 
        "\"lon_e6\":%ld,\"rssi\":%.1f,\"snr\":%.1f}\n",
        meshAdvertTypeName(advert.type), escapedName.c_str(),
        advert.kitsuNamed ? "true" : "false", publicPrefix,
        static_cast<unsigned long>(advert.timestamp),
        advert.hasLocation ? "true" : "false",
        static_cast<long>(advert.hasLocation ? advert.location.latitudeE6 : 0),
        static_cast<long>(advert.hasLocation ? advert.location.longitudeE6 : 0),
        advert.rssi, advert.snr);

    // The verified full public key, sender timestamp and local observation
    // evidence are committed independently of the creature's RADIO activity.
    // This is the device-authoritative history queried by a direct companion.
    bool discoveredPeer = false;
    if (discoveryJournalReady) {
      kitsu868::discovery::AdvertObservation observation{};
      memcpy(observation.publicKey, advert.publicKey,
             sizeof(observation.publicKey));
      const size_t nameBytes = strnlen(advert.name,
                                       sizeof(observation.name) - 1U);
      memcpy(observation.name, advert.name, nameBytes);
      observation.name[nameBytes] = '\0';
      observation.type = advert.type;
      observation.kitsuNamed = advert.kitsuNamed;
      observation.hasLocation = advert.hasLocation;
      observation.latitudeE6 = advert.location.latitudeE6;
      observation.longitudeE6 = advert.location.longitudeE6;
      observation.senderAdvertTimestamp = advert.timestamp;
      observation.observed.epochValid = meshTransport.timeValid();
      observation.observed.epoch = observation.observed.epochValid
                                       ? meshTransport.currentEpoch()
                                       : 0U;
      observation.observed.bootId = discoveryBootId;
      observation.observed.millis = millis();
      observation.lastHop.valid = true;
      observation.lastHop.rssi = advert.rssi;
      observation.lastHop.snr = advert.snr;
      const kitsu868::discovery::RecordResult recorded =
          discoveryJournal.record(observation);
      if (recorded.result != kitsu868::discovery::JournalResult::Ok) {
        Serial.printf("KITSU_WARN discovery_record=%s\n",
                      kitsu868::discovery::journalResultName(recorded.result));
      } else {
        discoveredPeer = recorded.newPeer;
        const uint32_t journalNow = millis();
        if (!discoveryStorageRetry.status().dirty) {
          discoveryJournalDirtyAt = journalNow;
        }
        discoveryStorageRetry.markDirty(
            journalNow, recorded.urgent ? DISCOVERY_URGENT_DEFER_MS
                                        : DISCOVERY_JOURNAL_DEBOUNCE_MS);
        if (recorded.urgent &&
            companionBle.pairingStorageReserved(journalNow)) {
          Serial.println(
              "KITSU_DISCOVERY_FLUSH deferred=pairing urgent=true");
        }
      }
    }
    const kitsu868::signal::MeshOperationKind operation = discoveredPeer
        ? advert.type == 2U
              ? kitsu868::signal::MeshOperationKind::RepeaterDiscovered
              : kitsu868::signal::MeshOperationKind::PeerDiscovered
        : kitsu868::signal::MeshOperationKind::AdvertReceived;
    recordSuccessfulEncounterTrigger(operation);
  }
}

void processFloodAdvertStatus() {
  const bool floodChanged = meshTransport.takeFloodAdvertStatusChanged();
  const bool nearbyChanged = meshTransport.takeNearbyAdvertStatusChanged();
  if (floodChanged) {
    kitsu868::mesh::FloodAdvertStatus status{};
    if (meshTransport.lastFloodAdvertStatus(status) &&
        status.state == kitsu868::mesh::AdvertTransmitState::Sent &&
        status.emittedAt != lastSignaledFloodAdvert) {
      lastSignaledFloodAdvert = status.emittedAt;
      recordSuccessfulEncounterTrigger(
          kitsu868::signal::MeshOperationKind::AdvertSent);
    }
  }
  if (nearbyChanged) {
    kitsu868::mesh::NearbyAdvertStatus status{};
    if (meshTransport.lastNearbyAdvertStatus(status) &&
        status.state == kitsu868::mesh::AdvertTransmitState::Sent &&
        status.emittedAt != lastSignaledNearbyAdvert) {
      lastSignaledNearbyAdvert = status.emittedAt;
      recordSuccessfulEncounterTrigger(
          kitsu868::signal::MeshOperationKind::AdvertSent);
    }
  }
  if (floodChanged || nearbyChanged) {
    companionBleRefreshDirty = true;
  }
}

bool backgroundStorageWindowReady(uint32_t now) {
  const kitsu868::connectivity::BleOtaState otaState =
      bleOta.status().state;
  return !backgroundStorageTransactionUsed &&
      !companionBle.pairingStorageReserved(now) &&
      companionBle.bleTransmitIdle() && !meshTransport.sendInProgress() &&
      otaState != kitsu868::connectivity::BleOtaState::Receiving &&
      otaState != kitsu868::connectivity::BleOtaState::ReadyToReboot;
}

bool discoveryWriteHeadroomAvailable(
    kitsu868::connectivity::NvsHeadroomStats& stats) {
  if (!pairingNvsHeadroomPlatform.readStats(stats)) return false;
  constexpr size_t required =
      kitsu868::connectivity::kNvsEntriesPerPage +
      kitsu868::connectivity::nvsBlobEntries(
          kitsu868::discovery::kDiscoverySnapshotCapacity);
  return stats.freeEntries >= required;
}

void tickDiscoveryJournal(uint32_t now) {
  if (!discoveryJournalReady) return;
  const kitsu868::discovery::JournalStatus status = discoveryJournal.status();
  if (!status.dirty) {
    if (discoveryStorageRetry.status().dirty) {
      discoveryStorageRetry.recordSuccess();
    }
    return;
  }
  if (!discoveryStorageRetry.status().dirty) {
    discoveryJournalDirtyAt = now;
    discoveryStorageRetry.markDirty(now, DISCOVERY_JOURNAL_DEBOUNCE_MS);
  }

  kitsu868::connectivity::StorageRetryStatus retry =
      discoveryStorageRetry.status();
  if (retry.blockedNoSpace) {
    if (discoveryHeadroomRecheckAt == 0U ||
        static_cast<int32_t>(now - discoveryHeadroomRecheckAt) >= 0) {
      discoveryHeadroomRecheckAt = now + STORAGE_HEADROOM_RECHECK_MS;
      kitsu868::connectivity::NvsHeadroomStats stats{};
      if (discoveryWriteHeadroomAvailable(stats)) {
        discoveryStorageRetry.rearmAfterHeadroom(
            now, DISCOVERY_JOURNAL_DEBOUNCE_MS);
      }
    }
    return;
  }
  if (!discoveryStorageRetry.attemptDue(now) ||
      !backgroundStorageWindowReady(now)) {
    return;
  }

  backgroundStorageTransactionUsed = true;
  discoveryStorageRetry.recordAttempt(now);
  const kitsu868::discovery::JournalResult flushed = discoveryJournal.flush();
  if (flushed == kitsu868::discovery::JournalResult::Ok) {
    discoveryStorageRetry.recordSuccess();
    discoveryHeadroomRecheckAt = 0U;
    return;
  }

  kitsu868::connectivity::StorageRetryFailure failure =
      kitsu868::connectivity::StorageRetryFailure::Transient;
  kitsu868::connectivity::NvsHeadroomStats stats{};
  if (flushed == kitsu868::discovery::JournalResult::StorageWriteFailed &&
      pairingNvsHeadroomPlatform.readStats(stats)) {
    constexpr size_t required =
        kitsu868::connectivity::kNvsEntriesPerPage +
        kitsu868::connectivity::nvsBlobEntries(
            kitsu868::discovery::kDiscoverySnapshotCapacity);
    if (stats.freeEntries < required) {
      failure = kitsu868::connectivity::StorageRetryFailure::NoSpace;
      discoveryHeadroomRecheckAt = now + STORAGE_HEADROOM_RECHECK_MS;
    }
  }
  discoveryStorageRetry.recordFailure(now, failure);
  retry = discoveryStorageRetry.status();
  Serial.printf(
      "KITSU_WARN discovery_flush=%s blocked_no_space=%s retry=%u "
      "free_entries=%lu dirty_age_ms=%lu\n",
      kitsu868::discovery::journalResultName(flushed),
      retry.blockedNoSpace ? "true" : "false",
      static_cast<unsigned>(retry.failureCount),
      static_cast<unsigned long>(stats.freeEntries),
      static_cast<unsigned long>(now - discoveryJournalDirtyAt));
}

ChatJournalEntry* findChannelJournalByDelivery(
    const kitsu868::mesh::DeliveryEvent& delivery) {
  // getCurrentTimeUnique() gives every locally queued MeshCore message a
  // distinct timestamp. Match both it and the slot so overlapping recent
  // sends can never transfer a repeat observation to the wrong chat row.
  for (uint8_t offset = 0U; offset < chatJournalCount; ++offset) {
    ChatJournalEntry* entry = chatJournalNewest(offset);
    if (entry && !entry->inbound &&
        entry->kind == kitsu868::mesh::MessageKind::Channel &&
        entry->channelSlot == delivery.channelSlot &&
        entry->timestamp == delivery.messageTimestamp &&
        (entry->state == ChatJournalState::Queued ||
         entry->state == ChatJournalState::Sent)) {
      return entry;
    }
  }
  return nullptr;
}

void emitChatEvent(const char* event, const ChatJournalEntry& entry,
                   const char* state = nullptr, int32_t roundTripMs = -1) {
  companionBleRefreshDirty = true;
  Serial.printf(
      "KITSU_CHAT_EVENT {\"protocol\":1,\"event\":\"%s\"," 
      "\"message_id\":%lu,\"state\":",
      event, static_cast<unsigned long>(entry.id));
  if (state) Serial.printf("\"%s\"", state);
  else Serial.print("null");
  Serial.print(",\"round_trip_ms\":");
  if (roundTripMs >= 0) Serial.print(roundTripMs);
  else Serial.print("null");
  Serial.println("}");
}

void serviceCompanionBleRefresh(uint32_t now) {
  const kitsu868::connectivity::BleOtaState otaState = bleOta.status().state;
  if (otaState == kitsu868::connectivity::BleOtaState::Receiving ||
      otaState == kitsu868::connectivity::BleOtaState::ReadyToReboot) {
    return;
  }
  const kitsu868::connectivity::BleSessionStatus status =
      companionBle.sessionStatus(now);
  if (!status.applicationAuthenticated ||
      !status.authenticatedRequestBarrier) return;
  const bool periodicDue = companionBleRefreshAt == 0U ||
      static_cast<int32_t>(now - companionBleRefreshAt) >= 0;
  if ((!companionBleRefreshDirty && !periodicDue) ||
      !companionBle.bleTransmitIdle()) {
    return;
  }

  uint32_t sequence = ++companionBleRefreshSequence;
  if (sequence == 0U) sequence = ++companionBleRefreshSequence;
  char payload[112]{};
  const int written = snprintf(
      payload, sizeof(payload),
      "{\"v\":1,\"cursor\":\"ble:%lu\",\"kind\":\"refresh\",\"body\":{}}",
      static_cast<unsigned long>(sequence));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(payload) ||
      !companionBle.sendEvent(
          "companion.refresh", reinterpret_cast<const uint8_t*>(payload),
          static_cast<size_t>(written))) {
    companionBleRefreshAt = now + BLE_REFRESH_RETRY_MS;
    return;
  }
  companionBleRefreshDirty = false;
  companionBleRefreshAt = now + BLE_REFRESH_INTERVAL_MS;
}

void processMeshMessages() {
  kitsu868::mesh::ReceivedMessage received{};
  while (meshTransport.takeMessage(received)) {
    char normalizedText[kitsu868::mesh::kMeshTextCapacity]{};
    const size_t textBytes = strnlen(received.text,
                                     sizeof(normalizedText) - 1U);
    memcpy(normalizedText, received.text, textBytes);
    normalizedText[textBytes] = '\0';

    ChatJournalEntry& entry = appendChatJournal();
    entry.inbound = true;
    entry.timestamp = received.timestamp;
    entry.kind = received.kind;
    entry.route = received.route;
    entry.state = ChatJournalState::Received;
    entry.authenticated = received.senderAuthenticated;
    memcpy(entry.contactPublicKey, received.publicKey,
           sizeof(entry.contactPublicKey));
    entry.channelSlot = received.channelSlot;
    memcpy(entry.senderName, received.senderName, sizeof(entry.senderName));
    entry.senderName[sizeof(entry.senderName) - 1U] = '\0';
    memcpy(entry.text, normalizedText,
           strnlen(normalizedText, sizeof(entry.text) - 1U) + 1U);
    entry.rssi = received.rssi;
    entry.snr = received.snr;
    // Flood path hashes are the repeaters that actually forwarded this copy
    // before Kitsu received it. Direct routing consumes path hashes hop by
    // hop, so a zero count at the destination cannot distinguish zero-hop
    // delivery from a longer consumed route and must remain unknown.
    entry.repeaterCountKnown =
        received.route == kitsu868::mesh::MessageRoute::Flood;
    entry.repeaterCount = entry.repeaterCountKnown
        ? received.hopCount
        : 0U;

    entry.unread = screen != Screen::Inbox;
    if (!entry.unread) {
      inboxSelection = 0;
    } else if (unreadChatMessages < kitsu868::chat::kInboxCapacity) {
      ++unreadChatMessages;
    }
    pendingChatReaction = true;
    lastMemory = received.kind == kitsu868::mesh::MessageKind::Direct
                     ? "A private message reached your companion."
                     : "A channel message crossed the mesh.";
    emitChatEvent("message", entry);
    recordSuccessfulEncounterTrigger(
        kitsu868::signal::MeshOperationKind::MessageReceived);
  }

  kitsu868::mesh::DeliveryEvent delivery{};
  while (meshTransport.takeDelivery(delivery)) {
    ChatJournalEntry* entry = delivery.kind == kitsu868::mesh::MessageKind::Direct
                                  ? findChatByDelivery(delivery)
                                  : findChannelJournalByDelivery(delivery);
    if (!entry) {
      Serial.println("KITSU_WARN chat_delivery_unmatched=true");
      continue;
    }
    const uint32_t elapsed = millis() -
        (entry->sentAt != 0U ? entry->sentAt : entry->queuedAt);
    switch (delivery.state) {
      case kitsu868::mesh::DeliveryState::Sent: {
        const bool transitionedToSent =
            entry->state == ChatJournalState::Queued;
        entry->state = ChatJournalState::Sent;
        entry->sentAt = millis();
        if (entry->kind == kitsu868::mesh::MessageKind::Channel) {
          entry->repeatCountKnown = delivery.repeatCountKnown;
          entry->repeatCount = 0U;
          entry->repeatObservationOpen = delivery.repeatObservationOpen;
          entry->repeatSourceCount = 0U;
          memset(entry->repeatSources, 0, sizeof(entry->repeatSources));
          entry->repeatSourcesTruncated = false;
        }
        touchChatJournal(*entry);
        emitChatEvent("tx", *entry, "sent");
        if (transitionedToSent) {
          recordSuccessfulEncounterTrigger(
              kitsu868::signal::MeshOperationKind::MessageSent);
        }
        break;
      }
      case kitsu868::mesh::DeliveryState::RepeatObserved: {
        // The pre-dedup hook can only emit this after logTx installed a
        // successful-sent tracker. Preserve Sent: a returned flood copy is
        // useful reception evidence, but it is neither a channel ACK nor a
        // proof of delivery to any intended human recipient.
        if (entry->kind != kitsu868::mesh::MessageKind::Channel ||
            !delivery.repeatCountKnown) {
          Serial.println("KITSU_WARN chat_repeat_invalid=true");
          break;
        }
        const bool recoveredSentTransition =
            entry->state == ChatJournalState::Queued;
        if (recoveredSentTransition) {
          // Recover honestly if a bounded delivery queue dropped the earlier
          // Sent transition; RepeatObserved itself is only armed by logTx.
          entry->state = ChatJournalState::Sent;
          entry->sentAt = millis();
        }
        entry->repeatCountKnown = true;
        entry->repeatCount = delivery.repeatCount;
        entry->repeatObservationOpen = delivery.repeatObservationOpen;
        entry->repeatSourceCount = delivery.repeatSourceCount;
        memcpy(entry->repeatSources, delivery.repeatSources,
               sizeof(entry->repeatSources));
        entry->repeatSourcesTruncated = delivery.repeatSourcesTruncated;
        touchChatJournal(*entry);
        emitChatEvent("repeat", *entry,
                      delivery.repeatObservationOpen ? "observed" : "closed");
        if (recoveredSentTransition) {
          recordSuccessfulEncounterTrigger(
              kitsu868::signal::MeshOperationKind::MessageSent);
        }
        break;
      }
      case kitsu868::mesh::DeliveryState::Delivered:
        entry->state = ChatJournalState::Delivered;
        entry->repeaterCountKnown = delivery.repeaterCountKnown;
        entry->repeaterCount = delivery.repeaterCountKnown
            ? delivery.repeaterCount
            : 0U;
        touchChatJournal(*entry);
        emitChatEvent("delivery", *entry, "delivered",
                      elapsed > INT32_MAX ? INT32_MAX
                                          : static_cast<int32_t>(elapsed));
        break;
      case kitsu868::mesh::DeliveryState::TimedOut:
        entry->state = ChatJournalState::Unconfirmed;
        entry->repeaterCountKnown = false;
        entry->repeaterCount = 0U;
        touchChatJournal(*entry);
        emitChatEvent("delivery", *entry, "unconfirmed");
        break;
      case kitsu868::mesh::DeliveryState::Cancelled:
        entry->state = ChatJournalState::Cancelled;
        entry->repeaterCountKnown = false;
        entry->repeaterCount = 0U;
        if (entry->kind == kitsu868::mesh::MessageKind::Channel) {
          entry->repeatCountKnown = false;
          entry->repeatCount = 0U;
          entry->repeatObservationOpen = false;
          entry->repeatSourceCount = 0U;
          memset(entry->repeatSources, 0, sizeof(entry->repeatSources));
          entry->repeatSourcesTruncated = false;
        }
        touchChatJournal(*entry);
        emitChatEvent("tx", *entry, "cancelled");
        break;
      case kitsu868::mesh::DeliveryState::TxFailed:
        entry->state = ChatJournalState::Failed;
        entry->repeaterCountKnown = false;
        entry->repeaterCount = 0U;
        if (entry->kind == kitsu868::mesh::MessageKind::Channel) {
          entry->repeatCountKnown = false;
          entry->repeatCount = 0U;
          entry->repeatObservationOpen = false;
          entry->repeatSourceCount = 0U;
          memset(entry->repeatSources, 0, sizeof(entry->repeatSources));
          entry->repeatSourcesTruncated = false;
        }
        touchChatJournal(*entry);
        emitChatEvent("tx", *entry, "failed");
        break;
    }
  }

  if (pendingChatReaction && !wisp.sleeping && !radioListening &&
      activeGame == ActiveGame::None && screen == Screen::Pet &&
      (!activeAnimation.active || !activeAnimation.finite)) {
    pendingChatReaction = false;
    cancelAmbientAnimation();
    if (!startTransientAnimation(CompanionRole::Surprise)) {
      startBaseAnimation();
    }
  }
}

void tickCreature() {
  if (wisp.sleeping || millis() - lastEnergyTickAt < ENERGY_TICK_MS) return;
  lastEnergyTickAt = millis();
  if (wisp.energy > 5) --wisp.energy;
  if (wisp.curiosity < 100) ++wisp.curiosity;
  saveState();
}

void tickProgression() {
  const uint32_t now = millis();
  const uint32_t elapsed = now - lastBrainMinuteAt;
  if (elapsed < BRAIN_MINUTE_MS) return;
  const uint32_t wholeMinutes = elapsed / BRAIN_MINUTE_MS;
  const uint16_t minutes = static_cast<uint16_t>(
      min<uint32_t>(wholeMinutes, 0xffffU));
  companionBrain.advanceMinutes(minutes);
  lastBrainMinuteAt += static_cast<uint32_t>(minutes) * BRAIN_MINUTE_MS;
  brainMinutesSinceFlush = static_cast<uint8_t>(min<uint16_t>(
      15, static_cast<uint16_t>(brainMinutesSinceFlush + minutes)));
  if (brainMinutesSinceFlush >= 15U && companionBrain.dirty()) {
    if (!brainStorageRetry.status().dirty) {
      brainStorageRetry.markDirty(now, 0U);
    }
    if (brainStorageRetry.attemptDue(now) &&
        backgroundStorageWindowReady(now)) {
      backgroundStorageTransactionUsed = true;
      brainStorageRetry.recordAttempt(now);
      if (companionBrain.flush()) {
        brainStorageRetry.recordSuccess();
        brainMinutesSinceFlush = 0U;
      } else {
        brainStorageRetry.recordFailure(
            now, kitsu868::connectivity::StorageRetryFailure::Transient);
        Serial.printf("KITSU_WARN brain_flush=false retry=%u\n",
                      static_cast<unsigned>(
                          brainStorageRetry.status().failureCount));
      }
    }
  } else if (!companionBrain.dirty()) {
    brainStorageRetry.recordSuccess();
    brainMinutesSinceFlush = 0U;
  }
}

void tickDisplayPower() {
  if (displaySleeping || !oledDetected) return;
  const uint32_t timeout = wisp.sleeping
                               ? DREAM_DISPLAY_SLEEP_MS
                               : AWAKE_DISPLAY_SLEEP_MS;
  if (millis() - lastInteractionAt >= timeout) sleepDisplay();
}

bool parseAnimationRole(const String& name, CompanionRole& role) {
  if (name == "idle") role = CompanionRole::Idle;
  else if (name == "blink") role = CompanionRole::Blink;
  else if (name == "pet") role = CompanionRole::Pet;
  else if (name == "sleep") role = CompanionRole::Sleep;
  else if (name == "listen") role = CompanionRole::Listen;
  else if (name == "surprise") role = CompanionRole::Surprise;
  else if (name == "play") role = CompanionRole::Play;
  else if (name == "tired") role = CompanionRole::Tired;
  else if (name == "feed") role = CompanionRole::Feed;
  else if (name == "wake") role = CompanionRole::Wake;
  else if (name == "meet") role = CompanionRole::Meet;
  else if (name == "evolve" || name == "special") role = CompanionRole::Evolve;
  else return false;
  return true;
}

bool startMeetForNewPack() {
  if (!companionPack.valid()) return false;
  const uint32_t packId = companionPack.id();
  const uint32_t lastMetPackId = storageReady
                                     ? preferences.getUInt("last_met", 0)
                                     : 0;
  if (lastMetPackId == packId) return false;

  if (storageReady) preferences.putUInt("last_met", packId);
  cancelAmbientAnimation();
  const bool started = startTransientAnimation(CompanionRole::Meet);
  Serial.printf("KITSU_EVENT meet pack_id=%08lX animation=%s\n",
                static_cast<unsigned long>(packId), started ? "true" : "missing");
  return started;
}

void printSelfTest() {
  sampleBattery(false);
  kitsu868::connectivity::NvsHeadroomStats nvsStats{};
  const bool nvsStatsReady = pairingNvsHeadroomPlatform.readStats(nvsStats);
  const kitsu868::connectivity::StorageRetryStatus discoveryRetry =
      discoveryStorageRetry.status();
  const kitsu868::connectivity::StorageRetryStatus brainRetry =
      brainStorageRetry.status();
  Serial.printf(
      "KITSU_STORAGE {\"stats\":%s,\"used_entries\":%lu,"
      "\"free_entries\":%lu,\"total_entries\":%lu,"
      "\"discovery_dirty\":%s,\"discovery_blocked_no_space\":%s,"
      "\"discovery_retry\":%u,\"brain_dirty\":%s,"
      "\"brain_retry\":%u}\n",
      nvsStatsReady ? "true" : "false",
      static_cast<unsigned long>(nvsStats.usedEntries),
      static_cast<unsigned long>(nvsStats.freeEntries),
      static_cast<unsigned long>(nvsStats.totalEntries),
      discoveryRetry.dirty ? "true" : "false",
      discoveryRetry.blockedNoSpace ? "true" : "false",
      static_cast<unsigned>(discoveryRetry.failureCount),
      companionBrain.dirty() ? "true" : "false",
      static_cast<unsigned>(brainRetry.failureCount));
  const kitsu868::LifetimeCounters& life = companionBrain.lifetime();
  const kitsu868::CompanionMood mood = companionBrain.mood(companionVitals());
  const int bleBondCount = companionBle.bleBondCount();
  const uint8_t controllerCount = deviceSecurity.status().controllerCount;
  const String escapedCompanion = jsonEscaped(companionName());
  kitsu868::timekeeping::ClockReading selftestClock{};
  (void)kitsuClock.read(millis(), selftestClock);
  const kitsu868::social::SocialState selftestSocial =
      socialProgression.snapshot();
  const kitsu868::adventure::RouteView selftestAdventure =
      adventureProgression.view();
  const kitsu868::activities::ActivityView selftestActivity =
      activitySuite.view(millis());
  const kitsu868::connectivity::BleLinkStatus selftestBle =
      companionBle.linkStatus(millis());
  const char* gameName = activeGame == ActiveGame::SignalCatch
                             ? "signal"
                             : activeGame == ActiveGame::PounceFetch
                                   ? "pounce"
                                   : activeGame == ActiveGame::EchoBeat
                                         ? "echo"
                                         : "none";
  Serial.printf(
      "KITSU_SELFTEST {\"firmware\":\"%s\",\"version\":\"%s\","
      "\"board\":\"heltec-v3.2\",\"oled\":%s,\"radio\":%s,"
      "\"radio_code\":%d,\"storage\":%s,\"pairing_storage\":%s,"
      "\"button_released\":%s,"
      "\"tx_enabled\":false,\"boot\":%lu,\"uid\":\"%s\","
      "\"companion\":\"%s\",\"orientation\":\"portrait\",",
      FIRMWARE_NAME, FIRMWARE_VERSION,
      oledDetected ? "true" : "false", radioReady ? "true" : "false",
      radioInitCode, storageReady ? "true" : "false",
      companionBle.pairingStorageBlocked() ? "false" : "true",
      digitalRead(PIN_BUTTON) == HIGH ? "true" : "false",
      static_cast<unsigned long>(wisp.boots), wisp.uid.c_str(),
      escapedCompanion.c_str());
  Serial.printf(
      "\"energy\":%u,\"curiosity\":%u,\"affection\":%u,"
      "\"ui_width\":64,\"ui_height\":128,\"pack_present\":%s,"
      "\"pack_valid\":%s,\"pack_error\":\"%s\",\"pack_id\":\"%08lX\","
      "\"pack_revision\":%lu,\"pack_frames\":%u,\"pack_bytes\":%lu,"
      "\"pack_capacity\":%u,\"active_source\":\"%s\",",
      wisp.energy, wisp.curiosity, wisp.affection,
      companionPack.present() ? "true" : "false",
      companionPack.valid() ? "true" : "false", companionPack.error(),
      static_cast<unsigned long>(companionPack.id()),
      static_cast<unsigned long>(companionPack.revision()),
      companionPack.frameCount(), static_cast<unsigned long>(companionPack.bytes()),
      static_cast<unsigned>(companionPack.capacity()),
      companionPack.valid() ? "pack" : "none");
  Serial.printf(
      "\"animation_pack\":true,\"animation_role\":\"%s\","
      "\"brain_storage\":%s,\"brain_loaded\":%s,"
      "\"personality\":\"%s\",\"mood\":\"%s\","
      "\"bond_level\":%u,\"bond_xp\":%u,\"bond_progress\":%u,"
      "\"evolution_stage\":%u,\"evolution_name\":\"%s\","
      "\"appearance_variant\":%u,\"unlocks\":%u,\"memories\":%u,",
      activeAnimation.active ? animationRoleName(activeAnimation.role) : "none",
      companionBrain.storageAvailable() ? "true" : "false",
      companionBrain.loadedFromStorage() ? "true" : "false",
      kitsu868::CompanionBrain::personalityLabel(
          companionBrain.personality().kind),
      kitsu868::CompanionBrain::moodLabel(mood), companionBrain.bondLevel(),
      companionBrain.bondXp(), companionBrain.bondProgressPercent(),
      static_cast<unsigned>(companionBrain.evolutionStage()),
      kitsu868::CompanionBrain::stageLabel(companionBrain.evolutionStage()),
      companionBrain.appearanceVariant(), companionBrain.unlockMask(),
      companionBrain.memoryCount());
  Serial.printf(
      "\"lifetime_minutes\":%lu,\"lifetime_pets\":%lu,"
      "\"lifetime_feeds\":%lu,\"lifetime_plays\":%lu,"
      "\"games_played\":%lu,\"perfect_games\":%lu,"
      "\"encounters\":%lu,\"unique_encounters\":%lu,"
      "\"battery_present\":%s,\"battery_mv\":%u,\"battery_pct\":%d,"
      "\"display_sleeping\":%s,\"game\":\"%s\",",
      static_cast<unsigned long>(life.poweredMinutes),
      static_cast<unsigned long>(life.pets),
      static_cast<unsigned long>(life.feeds),
      static_cast<unsigned long>(life.plays),
      static_cast<unsigned long>(life.gamesPlayed),
      static_cast<unsigned long>(life.perfectGames),
      static_cast<unsigned long>(life.encounters),
      static_cast<unsigned long>(life.uniqueEncounters),
      battery.present ? "true" : "false", battery.millivolts,
      battery.present ? static_cast<int>(battery.percent) : -1,
      displaySleeping ? "true" : "false", gameName);
  Serial.printf(
      "\"encounter_protocol\":1,\"last_peer\":\"%04X\","
      "\"last_trait\":%d,\"last_gift\":%d,"
      "\"trait_mask\":%u,\"gift_mask\":%u,"
      "\"trait_count\":%u,\"gift_count\":%u,"
      "\"sync_protocol\":1,\"sync_transport\":\"serial\","
      "\"mesh_protocol\":1,"
      "\"meshcore_version\":\"1.17.1\",\"mesh_enabled\":%s,"
      "\"mesh_rx_ready\":%s,\"mesh_time_valid\":%s,"
      "\"mesh_tx_unlocked\":%s,\"mesh_profile\":\"%s\","
      "\"mesh_adverts\":%lu,\"chat_protocol\":1,"
      "\"chat_storage\":%s,\"chat_contacts\":%u,"
      "\"chat_channels\":%u,\"chat_messages\":%u,"
      "\"chat_unread\":%u,\"clock_set\":%s,\"clock_trusted\":%s,"
      "\"clock_source\":\"%s\",\"progression_ready\":%s,"
      "\"progression_actions\":%lu,\"social_ready\":%s,\"friends\":%u,"
      "\"adventure_ready\":%s,\"adventure_phase\":%u,"
      "\"journal_entries\":%u,\"activity_ready\":%s,"
      "\"activity\":\"%s\",\"ble_connected\":%s,"
      "\"ble_application_authenticated\":%s,"
      "\"ble_last_close_available\":%s,"
      "\"ble_last_close_cause\":\"%s\","
      "\"ble_last_close_local\":%s,"
      "\"ble_last_disconnect_reason_available\":%s,"
      "\"ble_last_disconnect_reason\":%ld,"
      "\"ble_last_disconnect_at_ms\":%lu,"
      "\"ble_last_notify_status_available\":%s,"
      "\"ble_last_notify_status\":%ld,"
      "\"ble_bonds\":%d,\"controllers\":%u}\n",
      lastPeerUid,
      lastEncounterTrait == 0xff ? -1 : static_cast<int>(lastEncounterTrait),
      lastEncounterGift == 0xff ? -1 : static_cast<int>(lastEncounterGift),
      unlockedTraits, collectedGifts, bitCount16(unlockedTraits),
      bitCount16(collectedGifts), meshSettings.enabled ? "true" : "false",
      meshTransport.active() ? "true" : "false",
      meshTransport.timeValid() ? "true" : "false",
      meshTransport.transmitAllowed(meshSettings) ? "true" : "false",
      kitsu868::mesh::kUkEuNarrowProfileName,
      static_cast<unsigned long>(meshTransport.receivedAdvertCount()),
      meshTransport.messagingStorageReady() ? "true" : "false",
      static_cast<unsigned>(meshTransport.contactCount()),
      static_cast<unsigned>(configuredChannelCount()), chatJournalCount,
      unreadChatMessages, selftestClock.set() ? "true" : "false",
      selftestClock.trusted() ? "true" : "false",
      kitsu868::timekeeping::clockSourceName(selftestClock.source),
      companionProgressionReady ? "true" : "false",
      static_cast<unsigned long>(companionProgression.totalActions()),
      socialProgressionReady ? "true" : "false", selftestSocial.peerCount,
      adventureProgressionReady ? "true" : "false",
      static_cast<unsigned>(selftestAdventure.phase),
      adventureProgression.journalCount(),
      activityStateReady ? "true" : "false",
      kitsu868::activities::activityName(selftestActivity.kind),
      selftestBle.connected ? "true" : "false",
      selftestBle.applicationAuthenticated ? "true" : "false",
      selftestBle.closeTelemetryAvailable ? "true" : "false",
      kitsu868::connectivity::bleCloseCauseName(
          selftestBle.lastCloseCause),
      selftestBle.lastCloseWasLocal ? "true" : "false",
      selftestBle.disconnectReasonAvailable ? "true" : "false",
      static_cast<long>(selftestBle.lastDisconnectReason),
      static_cast<unsigned long>(selftestBle.lastDisconnectAtMillis),
      selftestBle.notifyStatusAvailable ? "true" : "false",
      static_cast<long>(selftestBle.lastNotifyStatus), bleBondCount,
      static_cast<unsigned>(controllerCount));
}

void printSync() {
  sampleBattery(false);
  kitsu868::MemoryEntry memory{};
  const bool hasMemory = companionBrain.recentMemory(0, memory);
  const String escapedCompanion = jsonEscaped(companionName());
  Serial.printf(
      "KITSU_SYNC {\"protocol\":1,\"uid\":\"%s\",\"pack_id\":\"%08lX\","
      "\"companion\":\"%s\",\"energy\":%u,\"curiosity\":%u,"
      "\"affection\":%u,\"sleeping\":%s,\"listening\":%s,"
      "\"mood\":\"%s\",\"bond\":%u,\"bond_xp\":%u,"
      "\"stage\":%u,\"battery_pct\":%d,\"memory_seq\":%u,"
      "\"memory_event\":%u,"
      "\"mesh_protocol\":1,\"mesh_enabled\":%s,"
      "\"mesh_rx_ready\":%s,\"chat_protocol\":1,"
      "\"chat_messages\":%u,\"chat_unread\":%u}\n",
      wisp.uid.c_str(), static_cast<unsigned long>(companionPack.id()),
      escapedCompanion.c_str(), wisp.energy, wisp.curiosity, wisp.affection,
      wisp.sleeping ? "true" : "false",
      radioListening ? "true" : "false",
      kitsu868::CompanionBrain::moodLabel(
          companionBrain.mood(companionVitals())),
      companionBrain.bondLevel(), companionBrain.bondXp(),
      static_cast<unsigned>(companionBrain.evolutionStage()),
      battery.present ? static_cast<int>(battery.percent) : -1,
      hasMemory ? memory.sequence : 0,
      hasMemory ? static_cast<unsigned>(memory.event) : 0,
      meshSettings.enabled ? "true" : "false",
      meshTransport.active() ? "true" : "false", chatJournalCount,
      unreadChatMessages);
}

void printJournal() {
  for (uint8_t index = 0; index < companionBrain.memoryCount(); ++index) {
    kitsu868::MemoryEntry memory;
    if (!companionBrain.recentMemory(index, memory)) break;
    const kitsu868::MemoryText text =
        kitsu868::CompanionBrain::memoryText(memory);
    Serial.printf(
        "KITSU_MEMORY {\"index\":%u,\"sequence\":%u,\"event\":%u,"
        "\"detail\":%u,\"value\":%lu,\"line1\":\"%s\","
        "\"line2\":\"%s\"}\n",
        index, memory.sequence, static_cast<unsigned>(memory.event),
        memory.detail, static_cast<unsigned long>(memory.value),
        text.line1, text.line2);
  }
  Serial.printf("KITSU_MEMORY_END {\"count\":%u}\n",
                companionBrain.memoryCount());
}

void printGuideList() {
  static const uint8_t emptyObject[] = {'{', '}'};
  String output;
  if (!companion_api::buildEncounterDiscovery(
          emptyObject, sizeof(emptyObject), output)) {
    Serial.println("KITSU_ERROR guide_list=unavailable");
    return;
  }
  Serial.print("KITSU_GUIDE_LIST ");
  Serial.println(output);
}

int8_t hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

void injectEncounterHex(const String& text) {
  if (text.length() != kitsu868::encounter::kWireBytes * 2U) {
    Serial.println("KITSU_ERROR inject_length");
    return;
  }
  uint8_t wire[kitsu868::encounter::kWireBytes]{};
  for (size_t index = 0; index < sizeof(wire); ++index) {
    const int8_t high = hexNibble(text.charAt(index * 2U));
    const int8_t low = hexNibble(text.charAt(index * 2U + 1U));
    if (high < 0 || low < 0) {
      Serial.println("KITSU_ERROR inject_hex");
      return;
    }
    wire[index] = static_cast<uint8_t>((high << 4) | low);
  }
  kitsu868::encounter::Packet packet{};
  const kitsu868::encounter::Status status =
      kitsu868::encounter::decode(wire, sizeof(wire), packet);
  if (status != kitsu868::encounter::Status::Ok) {
    Serial.printf("KITSU_ERROR inject_packet=%s\n",
                  kitsu868::encounter::statusName(status));
    return;
  }
  lastRssi = 0;
  lastSnr = 0;
  handleEncounter(packet);
}

const char* meshLocationModeName(kitsu868::mesh::LocationMode mode) {
  switch (mode) {
    case kitsu868::mesh::LocationMode::Hidden: return "hidden";
    case kitsu868::mesh::LocationMode::Fixed: return "fixed";
    case kitsu868::mesh::LocationMode::CurrentOnce: return "current_once";
  }
  return "hidden";
}

uint32_t derivedMeshProfileId(const kitsu868::mesh::RadioProfile& profile) {
  // Stable FNV-1a over the actual selected PHY.  This is a configuration key,
  // not a credential or an over-the-air capability value.
  uint32_t hash = 2166136261UL;
  const auto mix = [&hash](uint32_t value, uint8_t bytes) {
    for (uint8_t index = 0; index < bytes; ++index) {
      hash ^= static_cast<uint8_t>(value >> (index * 8U));
      hash *= 16777619UL;
    }
  };
  mix(profile.frequencyHz, 4);
  mix(profile.bandwidthHz, 4);
  mix(profile.spreadingFactor, 1);
  mix(profile.codingRate, 1);
  mix(profile.syncWord, 1);
  mix(static_cast<uint8_t>(profile.txPowerDbm), 1);
  mix(profile.preambleSymbols, 2);
  return hash ? hash : 1UL;
}

void refreshMeshRuntimeStatus(kitsu868::mesh::TransportStatus status) {
  meshInitStatus = status;
  radioReady = meshTransport.active();
  radioInitCode = meshTransport.radioCode();
}

bool commitMeshRadioSettings(const kitsu868::mesh::Settings& candidate,
                             const char*& error) {
  error = nullptr;
  if (kitsu868::mesh::validateSettings(candidate) !=
      kitsu868::mesh::Status::Ok) {
    error = "unsupported_profile";
    return false;
  }

  const kitsu868::mesh::Settings previous = meshSettings;
  meshTransport.lockTransmit();
  const bool exactNoOp = kitsu868::mesh::sameSettings(candidate, previous) &&
      (candidate.enabled ? meshTransport.active() : !meshTransport.active());
  if (exactNoOp) {
    refreshMeshRuntimeStatus(candidate.enabled
        ? kitsu868::mesh::TransportStatus::Ok
        : kitsu868::mesh::TransportStatus::Disabled);
    return true;
  }
  const kitsu868::mesh::TransportStatus applied =
      meshTransport.applySettings(candidate);
  if (applied != kitsu868::mesh::TransportStatus::Ok &&
      applied != kitsu868::mesh::TransportStatus::Disabled) {
    refreshMeshRuntimeStatus(meshTransport.applySettings(previous));
    error = kitsu868::mesh::transportStatusName(applied);
    return false;
  }

  const kitsu868::mesh::StoreStatus saved =
      meshSettingsStore.save(candidate);
  if (saved != kitsu868::mesh::StoreStatus::Saved) {
    refreshMeshRuntimeStatus(meshTransport.applySettings(previous));
    error = "storage";
    return false;
  }
  meshSettings = candidate;
  refreshMeshRuntimeStatus(applied);
  return true;
}

bool persistMeshPrivacySettings(const kitsu868::mesh::Settings& candidate,
                                const char*& error) {
  error = nullptr;
  if (kitsu868::mesh::validateSettings(candidate) !=
      kitsu868::mesh::Status::Ok) {
    error = "invalid_location";
    return false;
  }
  const kitsu868::mesh::StoreStatus saved =
      meshSettingsStore.save(candidate);
  if (saved != kitsu868::mesh::StoreStatus::Saved) {
    error = "storage";
    return false;
  }
  meshSettings = candidate;
  return true;
}

void printMeshResult(const char* action, const char* status,
                     const char* error = nullptr) {
  Serial.printf(
      "KITSU_MESH_RESULT {\"protocol\":1,\"action\":\"%s\"," 
      "\"status\":\"%s\",\"error\":",
      action, status);
  if (error) Serial.printf("\"%s\"", error);
  else Serial.print("null");
  Serial.println("}");
}

void printMeshStatus() {
  char publicKey[65]{};
  const bool hasPublicKey =
      meshTransport.publicKeyHex(publicKey, sizeof(publicKey));
  const bool configured = meshSettings.radio.selected() &&
      kitsu868::mesh::validateRadioProfile(meshSettings.radio) ==
          kitsu868::mesh::Status::Ok;
  const bool txUnlocked = meshTransport.transmitAllowed(meshSettings);
  const bool txReady = txUnlocked && meshTransport.timeValid();
  const String advertisedName = jsonEscaped(String(meshIdentity.advertisedName));

  kitsu868::mesh::AdvertLocation location{};
  const bool hasLocation = kitsu868::mesh::selectAdvertLocation(
      meshSettings, meshCurrentLocation, location);
  Serial.printf(
      "KITSU_MESH {\"protocol\":1,\"meshcore_version\":\"1.17.1\"," 
      "\"available\":true,\"configured\":%s,\"enabled\":%s," 
      "\"role\":\"client\",\"kitsu\":true,\"uid\":\"%s\"," 
      "\"marker\":\"fox\",\"advert_name\":\"%s\",",
      configured ? "true" : "false",
      meshSettings.enabled ? "true" : "false", wisp.uid.c_str(),
      advertisedName.c_str());
  if (hasPublicKey) Serial.printf("\"public_key\":\"%s\",", publicKey);
  else Serial.print("\"public_key\":null,");
  if (configured) {
    Serial.printf(
        "\"profile\":{\"id\":\"%08lX\",\"name\":\"%s\"," 
        "\"frequency_hz\":%lu,\"bandwidth_hz\":%lu," 
        "\"spreading_factor\":%u,\"coding_rate\":%u," 
        "\"sync_word\":%u,\"preamble_symbols\":%u," 
        "\"tx_power_dbm\":%d},",
        static_cast<unsigned long>(meshSettings.radio.profileId),
        meshSettings.radio.profileId == kitsu868::mesh::kUkEuNarrowProfileId
            ? kitsu868::mesh::kUkEuNarrowProfileName
            : "Custom",
        static_cast<unsigned long>(meshSettings.radio.frequencyHz),
        static_cast<unsigned long>(meshSettings.radio.bandwidthHz),
        meshSettings.radio.spreadingFactor, meshSettings.radio.codingRate,
        meshSettings.radio.syncWord, meshSettings.radio.preambleSymbols,
        meshSettings.radio.txPowerDbm);
  } else {
    Serial.print("\"profile\":null,");
  }

  Serial.printf("\"location\":{\"mode\":\"%s\",",
                meshLocationModeName(meshSettings.locationMode));
  if (hasLocation) {
    Serial.printf("\"lat_e6\":%ld,\"lon_e6\":%ld},",
                  static_cast<long>(location.coordinates.latitudeE6),
                  static_cast<long>(location.coordinates.longitudeE6));
  } else {
    Serial.print("\"lat_e6\":null,\"lon_e6\":null},");
  }
  Serial.printf(
      "\"time_valid\":%s,\"epoch\":%lu," 
      "\"tx_policy\":\"%s\",\"tx_unlocked\":%s," 
      "\"tx_ready\":%s,\"rx_ready\":%s," 
      "\"received_adverts\":%lu,\"dropped_adverts\":%lu," 
      "\"queued_adverts\":%lu," 
      "\"map_upload\":\"phone_only\"}\n",
      meshTransport.timeValid() ? "true" : "false",
      static_cast<unsigned long>(meshTransport.currentEpoch()),
      meshSettings.txPolicy == kitsu868::mesh::TxPolicy::ExplicitSession
          ? "explicit_session"
          : "locked",
      txUnlocked ? "true" : "false", txReady ? "true" : "false",
      meshTransport.active() ? "true" : "false",
      static_cast<unsigned long>(meshTransport.receivedAdvertCount()),
      static_cast<unsigned long>(meshTransport.droppedAdvertCount()),
      static_cast<unsigned long>(meshTransport.queuedAdvertCount()));
}

void printMeshRepeatStatus() {
  kitsu868::mesh::RepeatDiagnostics diagnostics{};
  if (!meshTransport.repeatDiagnostics(diagnostics)) {
    Serial.println("KITSU_MESH_REPEAT {\"protocol\":1,\"error\":\"unavailable\"}");
    return;
  }
  const auto resultName = [](kitsu868::mesh::RepeatDiagnosticResult value) {
    switch (value) {
      case kitsu868::mesh::RepeatDiagnosticResult::None: return "none";
      case kitsu868::mesh::RepeatDiagnosticResult::NoActiveHash:
        return "no_active_hash";
      case kitsu868::mesh::RepeatDiagnosticResult::WireMismatch:
        return "wire_mismatch";
      case kitsu868::mesh::RepeatDiagnosticResult::DigestMismatch:
        return "digest_mismatch";
      case kitsu868::mesh::RepeatDiagnosticResult::Recorded:
        return "recorded";
      case kitsu868::mesh::RepeatDiagnosticResult::Saturated:
        return "saturated";
    }
    return "unknown";
  };
  String output;
  output.reserve(3600U);
  output = "KITSU_MESH_REPEAT {\"protocol\":1,\"journal_revision\":\"";
  output += String(chatJournalRevision);
  output += "\",\"tx_done\":";
  output += String(diagnostics.txDoneFrames);
  output += ",\"tx_failed\":";
  output += String(diagnostics.txFailedFrames);
  output += ",\"rx_rearmed\":";
  output += String(diagnostics.rxReadyAfterTx);
  output += ",\"physical_rx_confirmed_after_tx\":";
  output += String(diagnostics.physicalRxConfirmedAfterTx);
  output += ",\"sync_turnaround_completed\":";
  output += String(diagnostics.syncTurnaroundCompleted);
  output += ",\"sync_turnaround_start_failures\":";
  output += String(diagnostics.syncTurnaroundStartFailures);
  output += ",\"sync_turnaround_timeouts\":";
  output += String(diagnostics.syncTurnaroundTimeouts);
  output += ",\"rx_rearm_attempts\":";
  output += String(diagnostics.rxRearmAttempts);
  output += ",\"rx_rearm_retries\":";
  output += String(diagnostics.rxRearmRetries);
  output += ",\"rx_rearm_failures\":";
  output += String(diagnostics.rxRearmFailures);
  output += ",\"last_rx_start_attempts\":";
  output += String(diagnostics.lastRxStartAttempts);
  output += ",\"last_rx_start_code\":";
  if (!diagnostics.lastRxStartCodeAvailable) {
    output += "null";
  } else {
    output += String(diagnostics.lastRxStartCode);
  }
  output += ",\"last_rx_start_software_state\":";
  if (!diagnostics.lastRxStartCodeAvailable) {
    output += "null";
  } else {
    output += diagnostics.lastRxStartSoftwareState ? "true" : "false";
  }
  output += ",\"last_rx_chip_status_available\":";
  output += diagnostics.lastRxChipStatusAvailable ? "true" : "false";
  output += ",\"last_rx_chip_status\":";
  if (!diagnostics.lastRxChipStatusAvailable) {
    output += "null";
  } else {
    const uint8_t status = diagnostics.lastRxChipStatus;
    output += "\"";
    output += shortHex(&status, 1U);
    output += "\"";
  }
  output += ",\"last_rx_chip_mode\":";
  if (!diagnostics.lastRxChipStatusAvailable) {
    output += "null";
  } else {
    output += String(kitsu868::mesh::sx126xChipMode(
        diagnostics.lastRxChipStatus));
  }
  output += ",\"last_tx_done_to_start_receive_us\":";
  if (!diagnostics.lastTxDoneToStartReceiveMicrosAvailable) {
    output += "null";
  } else {
    output += String(diagnostics.lastTxDoneToStartReceiveMicros);
  }
  output += ",\"last_tx_done_to_rx_confirmed_us\":";
  if (!diagnostics.lastTxDoneToRxConfirmedMicrosAvailable) {
    output += "null";
  } else {
    output += String(diagnostics.lastTxDoneToRxConfirmedMicros);
  }
  output += ",\"current_rx_software_state\":";
  output += diagnostics.currentRxSoftwareState ? "true" : "false";
  output += ",\"current_rx_chip_status_available\":";
  output += diagnostics.currentRxChipStatusAvailable ? "true" : "false";
  output += ",\"current_rx_chip_status\":";
  if (!diagnostics.currentRxChipStatusAvailable) {
    output += "null";
  } else {
    const uint8_t status = diagnostics.currentRxChipStatus;
    output += "\"";
    output += shortHex(&status, 1U);
    output += "\"";
  }
  output += ",\"current_rx_chip_mode\":";
  if (!diagnostics.currentRxChipStatusAvailable) {
    output += "null";
  } else {
    output += String(kitsu868::mesh::sx126xChipMode(
        diagnostics.currentRxChipStatus));
  }
  companion_api::appendReceiveObservability(output, diagnostics);
  output += ",\"scoped_flood_tx_done\":";
  output += String(diagnostics.scopedFloodTxDoneFrames);
  output += ",\"unscoped_flood_tx_done\":";
  output += String(diagnostics.unscopedFloodTxDoneFrames);
  output += ",\"raw_frames\":";
  output += String(diagnostics.rawFrames);
  output += ",\"parsed_frames\":";
  output += String(diagnostics.parsedFrames);
  output += ",\"raw_rejected\":";
  output += String(diagnostics.rawRejected);
  output += ",\"channel_candidates\":";
  output += String(diagnostics.channelForwardCandidates);
  output += ",\"channel_hash_matches\":";
  output += String(diagnostics.channelHashMatches);
  output += ",\"channel_wire_mismatches\":";
  output += String(diagnostics.channelWireMismatches);
  output += ",\"channel_exact_matches\":";
  output += String(diagnostics.channelExactMatches);
  output += ",\"channel_recorded\":";
  output += String(diagnostics.channelRecorded);
  output += ",\"channel_digest_mismatches\":";
  output += String(diagnostics.channelDigestMismatches);
  output += ",\"channel_saturated\":";
  output += String(diagnostics.channelSaturated);
  output += ",\"advert_candidates\":";
  output += String(diagnostics.advertForwardCandidates);
  output += ",\"advert_hash_matches\":";
  output += String(diagnostics.advertHashMatches);
  output += ",\"advert_wire_mismatches\":";
  output += String(diagnostics.advertWireMismatches);
  output += ",\"advert_exact_matches\":";
  output += String(diagnostics.advertExactMatches);
  output += ",\"advert_recorded\":";
  output += String(diagnostics.advertRecorded);
  output += ",\"advert_digest_mismatches\":";
  output += String(diagnostics.advertDigestMismatches);
  output += ",\"advert_saturated\":";
  output += String(diagnostics.advertSaturated);
  output += ",\"last_flood_tx\":";
  if (!diagnostics.lastFloodTxAvailable) {
    output += "null";
  } else {
    output += "{\"payload_type\":";
    output += String(diagnostics.lastFloodTxPayloadType);
    output += ",\"scoped\":";
    output += diagnostics.lastFloodTxScoped ? "true" : "false";
    output += ",\"scope\":";
    if (diagnostics.lastFloodTxScoped) {
      output += "\"";
      output += kitsu868::mesh::kDefaultTransportScopeName;
      output += "\"";
    } else {
      output += "null";
    }
    output += ",\"transport_code\":";
    if (diagnostics.lastFloodTxScoped) {
      const uint8_t codeBytes[2] = {
          static_cast<uint8_t>(diagnostics.lastFloodTxTransportCode),
          static_cast<uint8_t>(diagnostics.lastFloodTxTransportCode >> 8U)};
      output += "\"";
      output += shortHex(codeBytes, sizeof(codeBytes));
      output += "\"";
    } else {
      output += "null";
    }
    output += ",\"scope_tag\":";
    if (diagnostics.lastFloodTxScoped) {
      output += "\"";
      output += kitsu868::mesh::kDefaultTransportScopeTag;
      output += "\"";
    } else {
      output += "null";
    }
    output += ",\"scope_key_fingerprint\":";
    if (diagnostics.lastFloodTxScoped) {
      output += "\"";
      output += kitsu868::mesh::kDefaultTransportScopeKeyFingerprint;
      output += "\"";
    } else {
      output += "null";
    }
    output += '}';
  }
  output += ",\"last_channel\":";
  if (!diagnostics.lastChannelAvailable ||
      diagnostics.lastPathHashSize == 0U ||
      diagnostics.lastPathBytes < diagnostics.lastPathHashSize) {
    output += "null";
  } else {
    const String token = shortHex(
        diagnostics.lastPath + diagnostics.lastPathBytes -
            diagnostics.lastPathHashSize,
        diagnostics.lastPathHashSize);
    output += "{\"last_hop_token\":\"";
    output += token;
    output += "\",\"path_hash_bytes\":";
    output += String(diagnostics.lastPathHashSize);
    output += ",\"path_count\":";
    output += String(diagnostics.lastPathCount);
    output += ",\"rssi_dbm\":";
    output += String(diagnostics.lastRssi, 1);
    output += ",\"snr_db\":";
    output += String(diagnostics.lastSnr, 1);
    output += ",\"result\":\"";
    output += resultName(diagnostics.lastResult);
    output += "\"}";
  }
  output += ",\"last_advert\":";
  if (!diagnostics.lastAdvertAvailable ||
      diagnostics.lastAdvertPathHashSize == 0U ||
      diagnostics.lastAdvertPathBytes < diagnostics.lastAdvertPathHashSize) {
    output += "null";
  } else {
    const String token = shortHex(
        diagnostics.lastAdvertPath + diagnostics.lastAdvertPathBytes -
            diagnostics.lastAdvertPathHashSize,
        diagnostics.lastAdvertPathHashSize);
    output += "{\"last_hop_token\":\"";
    output += token;
    output += "\",\"path_hash_bytes\":";
    output += String(diagnostics.lastAdvertPathHashSize);
    output += ",\"path_count\":";
    output += String(diagnostics.lastAdvertPathCount);
    output += ",\"rssi_dbm\":";
    output += String(diagnostics.lastAdvertRssi, 1);
    output += ",\"snr_db\":";
    output += String(diagnostics.lastAdvertSnr, 1);
    output += ",\"result\":\"";
    output += resultName(diagnostics.lastAdvertResult);
    output += "\"}";
  }
  output += '}';
  Serial.println(output);
}

bool consumeCurrentLocationOnce() {
  if (meshSettings.locationMode !=
      kitsu868::mesh::LocationMode::CurrentOnce) {
    return true;
  }
  kitsu868::mesh::clearCurrentLocationOnce(meshCurrentLocation);
  kitsu868::mesh::Settings hidden = meshSettings;
  kitsu868::mesh::hideLocation(hidden);
  const char* error = nullptr;
  return persistMeshPrivacySettings(hidden, error);
}

void configureMeshFromCommand(const String& command) {
  kitsu868::mesh::Settings candidate = meshSettings;
  bool disabling = false;
  if (command == "mesh config on") {
    candidate.enabled = true;
    candidate.radio = kitsu868::mesh::ukEuNarrowProfile();
    candidate.txPolicy = kitsu868::mesh::TxPolicy::ExplicitSession;
  } else if (command == "mesh config off") {
    candidate.enabled = false;
    candidate.txPolicy = kitsu868::mesh::TxPolicy::Locked;
    kitsu868::mesh::hideLocation(candidate);
    disabling = true;
  } else {
    unsigned long long frequency = 0;
    unsigned long long bandwidth = 0;
    unsigned spreadingFactor = 0;
    unsigned codingRate = 0;
    int txPower = 0;
    char trailing = 0;
    if (sscanf(command.c_str(), "mesh config %llu %llu %u %u %d %c",
               &frequency, &bandwidth, &spreadingFactor, &codingRate,
               &txPower, &trailing) != 5) {
      printMeshResult("config", "rejected", "invalid_arguments");
      return;
    }
    if (frequency > UINT32_MAX || bandwidth > UINT32_MAX ||
        spreadingFactor < 5U || spreadingFactor > 12U ||
        codingRate < 5U || codingRate > 8U ||
        txPower < -9 || txPower > 22) {
      printMeshResult("config", "rejected", "unsupported_profile");
      return;
    }
    kitsu868::mesh::RadioProfile profile{};
    profile.frequencyHz = static_cast<uint32_t>(frequency);
    profile.bandwidthHz = static_cast<uint32_t>(bandwidth);
    profile.spreadingFactor = static_cast<uint8_t>(spreadingFactor);
    profile.codingRate = static_cast<uint8_t>(codingRate);
    profile.syncWord = 0x12;
    profile.txPowerDbm = static_cast<int8_t>(txPower);
    profile.preambleSymbols = profile.spreadingFactor <= 8U ? 32U : 16U;
    profile.profileId = derivedMeshProfileId(profile);
    if (kitsu868::mesh::validateRadioProfile(profile) !=
        kitsu868::mesh::Status::Ok) {
      printMeshResult("config", "rejected", "unsupported_profile");
      return;
    }
    candidate.enabled = true;
    candidate.radio = profile;
    candidate.txPolicy = kitsu868::mesh::TxPolicy::ExplicitSession;
  }

  const char* error = nullptr;
  if (!commitMeshRadioSettings(candidate, error)) {
    // Disabling is a privacy revocation even if flash storage is unhealthy.
    // Current-once coordinates never survive the rejected operation in RAM.
    if (disabling) {
      kitsu868::mesh::clearCurrentLocationOnce(meshCurrentLocation);
    }
    printMeshResult("config", "rejected", error);
    return;
  }
  if (disabling) {
    kitsu868::mesh::clearCurrentLocationOnce(meshCurrentLocation);
  }
  printMeshResult("config", "ok");
}

void configureMeshLocation(const String& command) {
  kitsu868::mesh::Settings candidate = meshSettings;
  kitsu868::mesh::CurrentLocationOnce candidateCurrent = meshCurrentLocation;
  const char* action = "location";
  if (command == "mesh location hidden") {
    kitsu868::mesh::hideLocation(candidate);
    kitsu868::mesh::clearCurrentLocationOnce(candidateCurrent);
    action = "location_hidden";
  } else {
    long long latitude = 0;
    long long longitude = 0;
    char trailing = 0;
    bool currentOnce = false;
    int parsed = sscanf(command.c_str(),
                        "mesh location fixed %lld %lld %c",
                        &latitude, &longitude, &trailing);
    if (parsed != 2) {
      parsed = sscanf(command.c_str(),
                      "mesh location current-once %lld %lld %c",
                      &latitude, &longitude, &trailing);
      currentOnce = parsed == 2;
    }
    if (parsed != 2) {
      printMeshResult(action, "rejected", "invalid_arguments");
      return;
    }
    if (latitude < INT32_MIN || latitude > INT32_MAX ||
        longitude < INT32_MIN || longitude > INT32_MAX) {
      printMeshResult(action, "rejected", "invalid_coordinates");
      return;
    }
    kitsu868::mesh::Coordinates coordinates{};
    coordinates.latitudeE6 = static_cast<int32_t>(latitude);
    coordinates.longitudeE6 = static_cast<int32_t>(longitude);
    if (kitsu868::mesh::validateCoordinates(coordinates) !=
        kitsu868::mesh::Status::Ok) {
      printMeshResult(action, "rejected", "invalid_coordinates");
      return;
    }
    if (currentOnce) {
      kitsu868::mesh::requestCurrentLocationOnce(candidate);
      if (kitsu868::mesh::stageCurrentLocationOnce(candidateCurrent,
                                                   coordinates) !=
          kitsu868::mesh::Status::Ok) {
        printMeshResult(action, "rejected", "invalid_coordinates");
        return;
      }
      action = "location_current_once";
    } else {
      if (kitsu868::mesh::setFixedLocation(candidate, coordinates) !=
          kitsu868::mesh::Status::Ok) {
        printMeshResult(action, "rejected", "invalid_coordinates");
        return;
      }
      kitsu868::mesh::clearCurrentLocationOnce(candidateCurrent);
      action = "location_fixed";
    }
  }

  // An explicit privacy mutation revokes any signed advert that has not yet
  // gone on air.  Stage CurrentOnce in a copy so a storage error cannot make
  // rejected coordinates live in RAM.
  meshTransport.lockTransmit();
  const char* error = nullptr;
  if (!persistMeshPrivacySettings(candidate, error)) {
    printMeshResult(action, "rejected", error);
    return;
  }
  meshCurrentLocation = candidateCurrent;
  printMeshResult(action, "ok");
}

void introduceKitsu(const String& command) {
  const char* const action = command == "mesh introduce nearby"
                                 ? "introduce_nearby"
                                 : "introduce_mesh";
  const uint32_t now = millis();
  if (hasMeshIntroduced &&
      static_cast<uint32_t>(now - lastMeshIntroduceAt) <
          MESH_INTRODUCE_MIN_INTERVAL_MS) {
    printMeshResult(action, "rejected", "rate_limited");
    return;
  }
  const kitsu868::mesh::AdvertScope scope = command == "mesh introduce nearby"
      ? kitsu868::mesh::AdvertScope::Nearby
      : kitsu868::mesh::AdvertScope::Flood;
  const kitsu868::mesh::TransportStatus status =
      meshTransport.introduce(scope, meshSettings, meshCurrentLocation);
  if (status != kitsu868::mesh::TransportStatus::Ok) {
    printMeshResult(action, "rejected",
                    kitsu868::mesh::transportStatusName(status));
    return;
  }
  lastMeshIntroduceAt = now;
  hasMeshIntroduced = true;
  consumeCurrentLocationOnce();
  printMeshResult(action, "queued");
}

void publishKitsuMapCard() {
  kitsu868::mesh::AdvertLocation location{};
  if (!kitsu868::mesh::selectAdvertLocation(
          meshSettings, meshCurrentLocation, location)) {
    Serial.println(
        "KITSU_MAP_PUBLISH {\"protocol\":1,\"status\":\"rejected\"," 
        "\"error\":\"location_hidden\",\"uploader\":\"phone\"," 
        "\"firmware_upload\":false,\"advert_hex\":null," 
        "\"location\":null}");
    return;
  }

  char advertHex[kitsu868::mesh::kAdvertHexCapacity]{};
  size_t advertLength = 0;
  const kitsu868::mesh::TransportStatus status =
      meshTransport.exportSignedAdvert(meshSettings, meshCurrentLocation,
                                       advertHex, sizeof(advertHex),
                                       advertLength);
  if (status != kitsu868::mesh::TransportStatus::Ok || advertLength == 0) {
    Serial.printf(
        "KITSU_MAP_PUBLISH {\"protocol\":1,\"status\":\"rejected\"," 
        "\"error\":\"%s\",\"uploader\":\"phone\"," 
        "\"firmware_upload\":false,\"advert_hex\":null," 
        "\"location\":null}\n",
        kitsu868::mesh::transportStatusName(status));
    return;
  }

  const char* mode = location.source == kitsu868::mesh::LocationSource::Fixed
                         ? "fixed"
                         : "current_once";
  Serial.printf(
      "KITSU_MAP_PUBLISH {\"protocol\":1,\"status\":\"ready\"," 
      "\"error\":null,\"uploader\":\"phone\"," 
      "\"firmware_upload\":false,\"advert_hex\":\"%s\"," 
      "\"location\":{\"mode\":\"%s\",\"lat_e6\":%ld," 
      "\"lon_e6\":%ld}}\n",
      advertHex, mode, static_cast<long>(location.coordinates.latitudeE6),
      static_cast<long>(location.coordinates.longitudeE6));
  consumeCurrentLocationOnce();
}

bool executeMeshCommand(const String& command) {
  if (command == "mesh status") {
    printMeshStatus();
  } else if (command == "mesh repeat-status") {
    printMeshRepeatStatus();
  } else if (command == "mesh config on" ||
             command == "mesh config off" ||
             command.startsWith("mesh config ")) {
    configureMeshFromCommand(command);
  } else if (command.startsWith("mesh time ")) {
    unsigned long long epoch = 0;
    char trailing = 0;
    if (sscanf(command.c_str(), "mesh time %llu %c", &epoch, &trailing) != 1 ||
        epoch > UINT32_MAX ||
        epoch < kitsu868::timekeeping::kMinimumClockUnixSeconds) {
      printMeshResult("time", "rejected", "invalid_time");
    } else {
      const uint32_t now = millis();
      const kitsu868::timekeeping::KitsuClock before = kitsuClock;
      const uint32_t previousGeneration = clockSnapshotGeneration;
      const uint8_t previousSlot = clockSnapshotSlot;
      const kitsu868::timekeeping::ClockResult clockResult =
          kitsuClock.setFromUnixSeconds(
              static_cast<uint64_t>(epoch), 0U,
              kitsu868::timekeeping::ClockSource::ManualSerial,
              kitsuClock.utcOffsetMinutes(), now);
      const bool runtimeReady =
          clockResult == kitsu868::timekeeping::ClockResult::Ok &&
          commitClockMutation(before, previousGeneration, previousSlot, now,
                              true);
      const kitsu868::mesh::TransportStatus status = runtimeReady
          ? kitsu868::mesh::TransportStatus::Ok
          : kitsu868::mesh::TransportStatus::TimeUnset;
      printMeshResult("time",
                      status == kitsu868::mesh::TransportStatus::Ok
                          ? "ok"
                          : "rejected",
                      status == kitsu868::mesh::TransportStatus::Ok
                          ? nullptr
                          : kitsu868::mesh::transportStatusName(status));
    }
  } else if (command == "mesh tx unlock") {
    const bool unlocked = meshTransport.unlockTransmit(meshSettings, true);
    printMeshResult("tx_unlock", unlocked ? "ok" : "rejected",
                    unlocked ? nullptr : "not_ready");
  } else if (command == "mesh tx lock") {
    meshTransport.lockTransmit();
    printMeshResult("tx_lock", "ok");
  } else if (command.startsWith("mesh location ")) {
    configureMeshLocation(command);
  } else if (command == "mesh introduce nearby" ||
             command == "mesh introduce mesh") {
    introduceKitsu(command);
  } else if (command == "mesh publish-map") {
    publishKitsuMapCard();
  } else {
    return false;
  }
  return true;
}

const char* chatErrorName(kitsu868::mesh::TransportStatus status) {
  switch (status) {
    case kitsu868::mesh::TransportStatus::Disabled: return "mesh_disabled";
    case kitsu868::mesh::TransportStatus::TimeUnset: return "time_unset";
    case kitsu868::mesh::TransportStatus::TxLocked: return "tx_locked";
    case kitsu868::mesh::TransportStatus::PacketPoolFull: return "queue_full";
    case kitsu868::mesh::TransportStatus::ContactNotFound:
      return "unknown_contact";
    case kitsu868::mesh::TransportStatus::ContactNotClient:
      return "contact_not_client";
    case kitsu868::mesh::TransportStatus::ContactTableFull:
      return "contact_full";
    case kitsu868::mesh::TransportStatus::ChannelNotFound:
      return "unknown_channel";
    case kitsu868::mesh::TransportStatus::TextTooLong: return "text_too_long";
    case kitsu868::mesh::TransportStatus::SendBusy: return "send_busy";
    case kitsu868::mesh::TransportStatus::MessagingStorageFailed:
      return "storage_failed";
    case kitsu868::mesh::TransportStatus::InvalidArgument:
      return "invalid_argument";
    default: return kitsu868::mesh::transportStatusName(status);
  }
}

void printChatResult(const char* action, const char* status,
                     const char* error = nullptr,
                     const ChatJournalEntry* entry = nullptr) {
  Serial.printf(
      "KITSU_CHAT_RESULT {\"protocol\":1,\"action\":\"%s\"," 
      "\"status\":\"%s\",\"error\":",
      action, status);
  if (error) Serial.printf("\"%s\"", error);
  else Serial.print("null");
  Serial.print(",\"message_id\":");
  if (entry) Serial.print(entry->id);
  else Serial.print("null");
  Serial.print(",\"route\":");
  if (entry) Serial.printf("\"%s\"", chatRouteName(entry->route));
  else Serial.print("null");
  Serial.println("}");
}

const char* meshContactRoleName(uint8_t type) {
  switch (type) {
    case 1: return "client";
    case 2: return "repeater";
    case 3: return "room";
    case 4: return "sensor";
    default: return "unknown";
  }
}

size_t configuredChannelCount() {
  size_t count = 0;
  for (uint8_t slot = 0; slot < kitsu868::mesh::kMeshChannelCapacity;
       ++slot) {
    kitsu868::mesh::ChannelRecord channel{};
    if (meshTransport.channelAt(slot, channel) && channel.configured) ++count;
  }
  return count;
}

void printChatStorageStatus() {
  kitsu868::mesh::MessagingStorageStatus status{};
  (void)meshTransport.messagingStorageStatus(status);
  Serial.print(
      "KITSU_CHAT_STORAGE {\"protocol\":1,\"usable\":");
  Serial.print(status.usable ? "true" : "false");
  Serial.print(",\"persisted_schema\":");
  if (status.persistedSchema == 0U) Serial.print("null");
  else Serial.print(status.persistedSchema);
  Serial.print(",\"migration_pending\":");
  Serial.print(status.migrationPending ? "true" : "false");
  Serial.print(",\"cleanup_pending\":");
  Serial.print(status.cleanupPending ? "true" : "false");
  Serial.print(",\"generation\":");
  if (status.generation == 0U) Serial.print("null");
  else Serial.print(static_cast<unsigned long>(status.generation));
  Serial.print(",\"writable_last_result\":");
  if (status.lastWriteResult ==
      kitsu868::mesh::MessagingStorageWriteResult::NotAttempted) {
    Serial.print("null");
  } else {
    Serial.print('"');
    Serial.print(kitsu868::mesh::messagingStorageWriteResultName(
        status.lastWriteResult));
    Serial.print('"');
  }
  Serial.print(",\"reason\":\"");
  Serial.print(kitsu868::mesh::messagingStorageReasonName(status.reason));
  Serial.println("\"}");
}

void printChatStatus() {
  const bool txUnlocked = meshTransport.transmitAllowed(meshSettings);
  Serial.printf(
      "KITSU_CHAT {\"protocol\":1,\"available\":true," 
      "\"meshcore_version\":\"1.17.1\",\"session\":\"%08lX\"," 
      "\"contacts\":%u,\"contact_capacity\":%u," 
      "\"channels\":%u,\"channel_capacity\":%u," 
      "\"messages\":%u,\"inbox_capacity\":%u," 
      "\"dropped_messages\":%lu,\"time_valid\":%s," 
      "\"tx_unlocked\":%s,\"tx_ready\":%s," 
      "\"direct_text_max_bytes\":%u," 
      "\"channel_text_max_bytes\":%u}\n",
      static_cast<unsigned long>(chatSession),
      static_cast<unsigned>(meshTransport.contactCount()),
      static_cast<unsigned>(kitsu868::mesh::kMeshContactCapacity),
      static_cast<unsigned>(configuredChannelCount()),
      static_cast<unsigned>(kitsu868::mesh::kMeshChannelCapacity),
      chatJournalCount, static_cast<unsigned>(kitsu868::chat::kInboxCapacity),
      static_cast<unsigned long>(
          chatJournalDropped + meshTransport.droppedMessageCount() +
          meshTransport.droppedDeliveryCount()),
      meshTransport.timeValid() ? "true" : "false",
      txUnlocked ? "true" : "false",
      txUnlocked && meshTransport.timeValid() ? "true" : "false",
      static_cast<unsigned>(kitsu868::chat::kDirectTextMaxBytes),
      static_cast<unsigned>(kitsu868::chat::kKitsuChannelTextMaxBytes));
  printChatStorageStatus();
}

void printChatContacts() {
  const size_t count = meshTransport.contactCount();
  size_t emitted = 0;
  for (size_t index = 0; index < count; ++index) {
    kitsu868::mesh::ContactRecord contact{};
    if (!meshTransport.contactAt(index, contact)) continue;
    const String id = shortHex(contact.publicKey, 6U);
    const String publicKey = shortHex(contact.publicKey,
                                      sizeof(contact.publicKey));
    const String escapedName = jsonEscaped(String(contact.name));
    uint32_t lastHeard = 0;
    for (uint8_t offset = 0; offset < chatJournalCount; ++offset) {
      const ChatJournalEntry* message = chatJournalNewest(offset);
      if (message && message->inbound &&
          message->kind == kitsu868::mesh::MessageKind::Direct &&
          memcmp(message->contactPublicKey, contact.publicKey,
                 sizeof(contact.publicKey)) == 0) {
        lastHeard = message->timestamp;
        break;
      }
    }
    Serial.printf(
        "KITSU_CONTACT {\"protocol\":1,\"index\":%u," 
        "\"id\":\"%s\",\"public_key\":\"%s\"," 
        "\"name\":\"%s\",\"role\":\"%s\"," 
        "\"favorite\":%s,\"last_advert\":%lu," 
        "\"last_heard\":%lu,\"route_hint\":\"%s\"," 
        "\"dm_capable\":%s}\n",
        static_cast<unsigned>(emitted), id.c_str(), publicKey.c_str(),
        escapedName.c_str(), meshContactRoleName(contact.type),
        contact.pinned ? "true" : "false",
        static_cast<unsigned long>(contact.lastAdvertTimestamp),
        static_cast<unsigned long>(lastHeard),
        contact.pathKnown ? "direct" : "flood",
        contact.type == 1U ? "true" : "false");
    ++emitted;
  }
  Serial.printf("KITSU_CONTACT_END {\"protocol\":1,\"count\":%u}\n",
                static_cast<unsigned>(emitted));
}

void printChatChannels() {
  for (uint8_t slot = 0; slot < kitsu868::mesh::kMeshChannelCapacity;
       ++slot) {
    kitsu868::mesh::ChannelRecord channel{};
    meshTransport.channelAt(slot, channel);
    const String escapedName = jsonEscaped(String(channel.name));
    Serial.printf(
        "KITSU_CHANNEL {\"protocol\":1,\"index\":%u," 
        "\"name\":\"%s\",\"configured\":%s,\"public\":%s," 
        "\"hash\":",
        slot, escapedName.c_str(), channel.configured ? "true" : "false",
        slot == 0U ? "true" : "false");
    if (channel.configured) Serial.printf("\"%02X\"", channel.hash);
    else Serial.print("null");
    Serial.print(",\"region_scope\":");
    if (channel.configured &&
        channel.regionScope == kitsu868::mesh::ChannelRegionScope::Eu) {
      Serial.print("\"EU\"");
    } else {
      Serial.print("null");
    }
    Serial.println("}");
  }
  Serial.printf("KITSU_CHANNEL_END {\"protocol\":1,\"count\":%u}\n",
                static_cast<unsigned>(kitsu868::mesh::kMeshChannelCapacity));
}

void printChatInbox(uint32_t afterMessageId) {
  uint8_t emitted = 0;
  uint32_t newestId = 0;
  for (uint8_t ordinal = 0; ordinal < chatJournalCount; ++ordinal) {
    const uint8_t index = static_cast<uint8_t>(
        (chatJournalStart + ordinal) % kitsu868::chat::kInboxCapacity);
    const ChatJournalEntry& entry = chatJournal[index];
    newestId = entry.id;
    if (entry.id <= afterMessageId) continue;
    const String contact = shortHex(entry.contactPublicKey, 6U);
    const String escapedText = jsonEscaped(String(entry.text));
    const String escapedSender = jsonEscaped(String(entry.senderName));
    Serial.printf(
        "KITSU_MESSAGE {\"protocol\":1,\"id\":%lu," 
        "\"direction\":\"%s\",\"kind\":\"%s\",\"contact\":",
        static_cast<unsigned long>(entry.id), entry.inbound ? "in" : "out",
        chatKindName(entry.kind));
    if (entry.kind == kitsu868::mesh::MessageKind::Direct) {
      Serial.printf("\"%s\"", contact.c_str());
    } else {
      Serial.print("null");
    }
    Serial.print(",\"channel\":");
    if (entry.kind == kitsu868::mesh::MessageKind::Channel) {
      Serial.print(entry.channelSlot);
    } else {
      Serial.print("null");
    }
    Serial.print(",\"sender\":");
    if (entry.senderName[0] != '\0') {
      Serial.printf("\"%s\"", escapedSender.c_str());
    } else {
      Serial.print("null");
    }
    Serial.printf(
        ",\"timestamp\":%lu,\"text\":\"%s\",\"state\":\"%s\"," 
        "\"route\":\"%s\",\"snr_db\":",
        static_cast<unsigned long>(entry.timestamp), escapedText.c_str(),
        chatStateName(entry.state), chatRouteName(entry.route));
    if (entry.inbound) Serial.print(entry.snr, 2);
    else Serial.print("null");
    Serial.printf(",\"authenticated\":%s}\n",
                  entry.authenticated ? "true" : "false");
    ++emitted;
  }
  Serial.printf(
      "KITSU_MESSAGE_END {\"protocol\":1,\"count\":%u," 
      "\"newest_id\":%lu,\"dropped\":%lu,\"session\":\"%08lX\"}\n",
      emitted, static_cast<unsigned long>(newestId),
      static_cast<unsigned long>(
          chatJournalDropped + meshTransport.droppedMessageCount() +
          meshTransport.droppedDeliveryCount()),
      static_cast<unsigned long>(chatSession));
}

bool findUniqueContact(const uint8_t reference[6],
                       kitsu868::mesh::ContactRecord& output,
                       const char*& error) {
  size_t matches = 0;
  const size_t count = meshTransport.contactCount();
  for (size_t index = 0; index < count; ++index) {
    kitsu868::mesh::ContactRecord candidate{};
    if (meshTransport.contactAt(index, candidate) &&
        memcmp(candidate.publicKey, reference, 6U) == 0) {
      output = candidate;
      ++matches;
    }
  }
  if (matches == 1U) return true;
  error = matches == 0U ? "unknown_contact" : "ambiguous_contact";
  return false;
}

void executeChatCommand(const kitsu868::chat::Command& command) {
  using kitsu868::chat::CommandKind;
  switch (command.kind) {
    case CommandKind::Status:
      printChatStatus();
      return;
    case CommandKind::ListContacts:
      printChatContacts();
      return;
    case CommandKind::ListChannels:
      printChatChannels();
      return;
    case CommandKind::ListInbox:
      printChatInbox(command.afterMessageId);
      return;
    case CommandKind::Reset: {
      // A frame already handed to the SX1262 cannot be recalled. Lock first,
      // surface cancellations for anything still queued, and require a retry
      // after the on-air frame reports its honest completion.
      meshTransport.lockTransmit();
      processMeshMessages();
      if (meshTransport.sendInProgress()) {
        printChatResult("reset", "rejected", "send_busy");
        return;
      }
      // Messaging reset invalidates the contact/channel context needed by
      // every pending send.  Surface those cancellations before the
      // transport deliberately clears its transient delivery ring.
      for (uint8_t offset = 0; offset < chatJournalCount; ++offset) {
        ChatJournalEntry* entry = chatJournalNewest(offset);
        if (!entry || entry->inbound) continue;
        if (entry->kind == kitsu868::mesh::MessageKind::Channel &&
            entry->state == ChatJournalState::Sent &&
            entry->repeatCountKnown && entry->repeatObservationOpen) {
          // Messaging reset discards the RAM correlation fingerprints. Close
          // the app-visible observation explicitly before that irreversible
          // context change so the row cannot remain "listening" forever.
          entry->repeatObservationOpen = false;
          touchChatJournal(*entry);
          emitChatEvent("repeat", *entry, "closed");
        }
        const bool pendingDirect =
            entry->kind == kitsu868::mesh::MessageKind::Direct &&
            (entry->state == ChatJournalState::Queued ||
             entry->state == ChatJournalState::Sent);
        const bool pendingChannel =
            entry->kind == kitsu868::mesh::MessageKind::Channel &&
            entry->state == ChatJournalState::Queued;
        if (!pendingDirect && !pendingChannel) continue;
        if (entry->kind == kitsu868::mesh::MessageKind::Direct &&
            entry->state == ChatJournalState::Sent) {
          // The packet was already physically transmitted; only its ACK
          // tracker is being discarded, so "unconfirmed" is honest.
          entry->state = ChatJournalState::Unconfirmed;
          entry->repeaterCountKnown = false;
          entry->repeaterCount = 0U;
          touchChatJournal(*entry);
          emitChatEvent("delivery", *entry, "unconfirmed");
        } else {
          entry->state = ChatJournalState::Cancelled;
          entry->repeaterCountKnown = false;
          entry->repeaterCount = 0U;
          if (entry->kind == kitsu868::mesh::MessageKind::Channel) {
            entry->repeatCountKnown = false;
            entry->repeatCount = 0U;
            entry->repeatObservationOpen = false;
            entry->repeatSourceCount = 0U;
            memset(entry->repeatSources, 0, sizeof(entry->repeatSources));
            entry->repeatSourcesTruncated = false;
          }
          touchChatJournal(*entry);
          emitChatEvent("tx", *entry, "cancelled");
        }
      }
      const kitsu868::mesh::TransportStatus status =
          meshTransport.resetMessagingState();
      printChatResult("reset", status == kitsu868::mesh::TransportStatus::Ok
                                     ? "ok"
                                     : "rejected",
                      status == kitsu868::mesh::TransportStatus::Ok
                          ? nullptr
                          : chatErrorName(status));
      return;
    }
    case CommandKind::SetContact: {
      const kitsu868::mesh::TransportStatus status =
          meshTransport.upsertContact(
              command.publicKey, command.name,
              static_cast<uint8_t>(command.contactRole));
      printChatResult("contact_set", status == kitsu868::mesh::TransportStatus::Ok
                                         ? "ok"
                                         : "rejected",
                      status == kitsu868::mesh::TransportStatus::Ok
                          ? nullptr
                          : chatErrorName(status));
      return;
    }
    case CommandKind::DropContact: {
      const kitsu868::mesh::TransportStatus status =
          meshTransport.removeContact(command.publicKey);
      printChatResult("contact_drop",
                      status == kitsu868::mesh::TransportStatus::Ok
                          ? "ok"
                          : "rejected",
                      status == kitsu868::mesh::TransportStatus::Ok
                          ? nullptr
                          : chatErrorName(status));
      return;
    }
    case CommandKind::SetChannel: {
      uint8_t secret[32]{};
      memcpy(secret, command.channelSecret, sizeof(command.channelSecret));
      const kitsu868::mesh::ChannelRegionScope regionScope =
          command.channelRegionScope ==
                  kitsu868::chat::ChannelRegionScope::Eu
              ? kitsu868::mesh::ChannelRegionScope::Eu
              : kitsu868::mesh::ChannelRegionScope::Legacy;
      const kitsu868::mesh::TransportStatus status =
          meshTransport.setChannel(command.channelIndex, command.name, secret,
                                   regionScope);
      memset(secret, 0, sizeof(secret));
      printChatResult("channel_set",
                      status == kitsu868::mesh::TransportStatus::Ok
                          ? "ok"
                          : "rejected",
                      status == kitsu868::mesh::TransportStatus::Ok
                          ? nullptr
                          : chatErrorName(status));
      return;
    }
    case CommandKind::ClearChannel: {
      const kitsu868::mesh::TransportStatus status =
          meshTransport.clearChannel(command.channelIndex);
      printChatResult("channel_clear",
                      status == kitsu868::mesh::TransportStatus::Ok
                          ? "ok"
                          : "rejected",
                      status == kitsu868::mesh::TransportStatus::Ok
                          ? nullptr
                          : chatErrorName(status));
      return;
    }
    case CommandKind::SendDirect: {
      kitsu868::mesh::ContactRecord contact{};
      const char* lookupError = nullptr;
      if (!findUniqueContact(command.contactReference, contact, lookupError)) {
        printChatResult("send", "rejected", lookupError);
        return;
      }
      if (contact.type != 1U) {
        printChatResult("send", "rejected", "contact_not_client");
        return;
      }
      uint32_t queuedTimestamp = 0;
      uint32_t expectedAck = 0;
      kitsu868::mesh::MessageRoute route = kitsu868::mesh::MessageRoute::Flood;
      const kitsu868::mesh::TransportStatus status =
          meshTransport.sendDirectText(meshSettings, contact.publicKey,
                                       command.text, 0, queuedTimestamp,
                                       expectedAck, route);
      if (status != kitsu868::mesh::TransportStatus::Ok) {
        printChatResult("send", "rejected", chatErrorName(status));
        return;
      }
      ChatJournalEntry& entry = appendChatJournal();
      entry.inbound = false;
      entry.kind = kitsu868::mesh::MessageKind::Direct;
      entry.route = route;
      entry.state = ChatJournalState::Queued;
      entry.authenticated = true;
      entry.timestamp = queuedTimestamp;
      entry.expectedAck = expectedAck;
      entry.queuedAt = millis();
      memcpy(entry.contactPublicKey, contact.publicKey,
             sizeof(entry.contactPublicKey));
      memcpy(entry.text, command.text, command.textLength + 1U);
      printChatResult("send", "queued", nullptr, &entry);
      return;
    }
    case CommandKind::SendChannel: {
      uint32_t queuedTimestamp = 0;
      const kitsu868::mesh::TransportStatus status =
          meshTransport.sendChannelText(meshSettings, command.channelIndex,
                                        command.text, queuedTimestamp);
      if (status != kitsu868::mesh::TransportStatus::Ok) {
        printChatResult("send", "rejected", chatErrorName(status));
        return;
      }
      ChatJournalEntry& entry = appendChatJournal();
      entry.inbound = false;
      entry.kind = kitsu868::mesh::MessageKind::Channel;
      entry.route = kitsu868::mesh::MessageRoute::Flood;
      entry.state = ChatJournalState::Queued;
      entry.authenticated = false;
      entry.timestamp = queuedTimestamp;
      entry.channelSlot = command.channelIndex;
      entry.queuedAt = millis();
      memcpy(entry.text, command.text, command.textLength + 1U);
      printChatResult("send", "queued", nullptr, &entry);
      return;
    }
    case CommandKind::None:
      break;
  }
  printChatResult("command", "rejected", "bad_syntax");
}

bool validCodeVerificationRequestId(const String& requestId) {
  if (requestId.length() != 32U) return false;
  for (size_t index = 0U; index < requestId.length(); ++index) {
    const char value = requestId[index];
    if (!((value >= '0' && value <= '9') ||
          (value >= 'a' && value <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool executeEncounterCodeSerial(const String& command) {
  const int firstSpace = command.indexOf(' ');
  String verb = firstSpace < 0 ? command : command.substring(0, firstSpace);
  verb.toLowerCase();
  if (verb != "codes") return false;

  constexpr const char* prefix = "codes verify ";
  if (!command.startsWith(prefix)) {
    Serial.println("KITSU_ERROR codes_verify=bad_syntax");
    return true;
  }
  const int codeStart = static_cast<int>(strlen(prefix));
  const int separator = command.indexOf(' ', codeStart);
  if (separator <= codeStart ||
      command.indexOf(' ', separator + 1) >= 0) {
    Serial.println("KITSU_ERROR codes_verify=bad_syntax");
    return true;
  }
  const String code = command.substring(codeStart, separator);
  const String requestId = command.substring(separator + 1);
  if (!validCodeVerificationRequestId(requestId)) {
    Serial.println("KITSU_ERROR codes_verify=bad_request");
    return true;
  }

  kitsu868::unlocks::CodeRecord record{};
  const bool found = encounterCodesReady &&
      encounterCodes.find(code.c_str(), record);
  Serial.print("KITSU_CODE_VERIFY_V1 {\"schema\":"
               "\"kitsu.code-verification.v1\",\"requestId\":\"");
  Serial.print(requestId);
  Serial.print("\",\"status\":\"");
  Serial.print(found ? "valid" : "invalid");
  Serial.print("\",\"deviceId\":\"");
  Serial.print(wisp.uid);
  if (found) {
    char codeId[9]{};
    char packId[9]{};
    snprintf(codeId, sizeof(codeId), "%08lX",
             static_cast<unsigned long>(record.codeId));
    snprintf(packId, sizeof(packId), "%08lX",
             static_cast<unsigned long>(record.packId));
    Serial.print("\",\"boundDeviceId\":\"");
    Serial.print(wisp.uid);
    Serial.print("\",\"codeId\":\"");
    Serial.print(codeId);
    Serial.print("\",\"packId\":\"");
    Serial.print(packId);
    Serial.print("\",\"rarity\":\"");
    Serial.print(kitsu868::unlocks::rarityName(record.rarity));
    memset(codeId, 0, sizeof(codeId));
    memset(packId, 0, sizeof(packId));
  }
  Serial.println("\"}");
  return true;
}

const char* quickActionName(kitsu868::activities::QuickAction action) {
  using kitsu868::activities::QuickAction;
  switch (action) {
    case QuickAction::Pet: return "pet";
    case QuickAction::Feed: return "feed";
    case QuickAction::Play: return "play";
    case QuickAction::Listen: return "listen";
    case QuickAction::DailyGame: return "daily";
    case QuickAction::Expedition: return "expedition";
  }
  return "pet";
}

bool parseQuickAction(const String& name,
                      kitsu868::activities::QuickAction& action) {
  using kitsu868::activities::QuickAction;
  if (name == "pet") action = QuickAction::Pet;
  else if (name == "feed") action = QuickAction::Feed;
  else if (name == "play") action = QuickAction::Play;
  else if (name == "listen") action = QuickAction::Listen;
  else if (name == "daily") action = QuickAction::DailyGame;
  else if (name == "expedition") action = QuickAction::Expedition;
  else return false;
  return true;
}

void executeQuickAction() {
  using kitsu868::activities::QuickAction;
  const QuickAction action = activityStateReady
                                 ? activitySuite.quickAction()
                                 : QuickAction::Pet;
  if (action == QuickAction::Pet) petWisp();
  else if (action == QuickAction::Feed) feedKitsu();
  else if (action == QuickAction::Play) playKitsu();
  else if (action == QuickAction::Listen) startListening();
  else if (action == QuickAction::DailyGame) {
    startActivity(kitsu868::activities::ActivityKind::MorseSignal, true,
                  false);
  } else {
    const char* error = nullptr;
    if (!startExpedition(kitsu868::expedition::Duration::Short, error)) {
      Serial.printf("KITSU_ERROR quick_expedition=%s\n",
                    error ? error : "start_failed");
    } else {
      enterScreen(Screen::Adventure);
    }
  }
}

bool parseMinuteText(const String& text, uint16_t& minute) {
  if (text.length() != 5U || text[2] != ':' || !isDigit(text[0]) ||
      !isDigit(text[1]) || !isDigit(text[3]) || !isDigit(text[4])) {
    return false;
  }
  const uint8_t hour = static_cast<uint8_t>(
      (text[0] - '0') * 10U + text[1] - '0');
  const uint8_t minutes = static_cast<uint8_t>(
      (text[3] - '0') * 10U + text[4] - '0');
  if (hour > 23U || minutes > 59U) return false;
  minute = static_cast<uint16_t>(hour * 60U + minutes);
  return true;
}

const char* progressionActionName(kitsu868::dialogue::Action action) {
  using kitsu868::dialogue::Action;
  switch (action) {
    case Action::Pet: return "pet";
    case Action::Feed: return "feed";
    case Action::Play: return "play";
    case Action::Listen: return "listen";
    case Action::Sleep: return "sleep";
    case Action::Wake: return "wake";
    case Action::Meet: return "meet";
    case Action::Gift: return "gift";
  }
  return "unknown";
}

const char* progressionTimeName(kitsu868::progression::TimeBucket bucket) {
  using kitsu868::progression::TimeBucket;
  switch (bucket) {
    case TimeBucket::Morning: return "morning";
    case TimeBucket::Day: return "day";
    case TimeBucket::Evening: return "evening";
    case TimeBucket::Night: return "night";
  }
  return "unknown";
}

void printProgressionCatalogue() {
  static const char* const achievementNames[] = {
      "first_callback", "favorite_learned", "ritual", "secret_habit",
      "perfect_day", "streak_seven", "comeback", "dreamer",
      "explorer", "friendly", "anniversary", "rare_moment",
      "variety_five", "hundred_actions"};
  const uint32_t achievements = companionProgression.achievementMask();
  for (uint8_t index = 0U;
       index < sizeof(achievementNames) / sizeof(achievementNames[0]);
       ++index) {
    if ((achievements & (UINT32_C(1) << index)) != 0U) {
      Serial.printf("KITSU_ACHIEVEMENT id=%u name=%s\n", index,
                    achievementNames[index]);
    }
  }
  const uint16_t lore = companionProgression.loreMask();
  for (uint8_t index = 0U; index < 10U; ++index) {
    if ((lore & (UINT16_C(1) << index)) == 0U) continue;
    const kitsu868::progression::DisplayLine line =
        kitsu868::progression::CompanionProgression::loreLine(
            static_cast<kitsu868::progression::LoreUnlock>(
                UINT16_C(1) << index));
    Serial.printf("KITSU_LORE id=%u line1=\"%s\" line2=\"%s\"\n",
                  index, line.line1, line.line2);
  }
}

bool executeProfileCommand(const String& raw) {
  String command = raw;
  command.trim();
  String lowered = command;
  lowered.toLowerCase();
  if (lowered == "profile status" || lowered == "profile inspect") {
    if (!companionProgressionReady) {
      Serial.println("KITSU_PROFILE {\"ready\":false}");
      return true;
    }
    const kitsu868::progression::PersonalBests bests =
        companionProgression.personalBests();
    kitsu868::progression::QuestionKind question{};
    const bool hasQuestion = companionProgression.pendingQuestion(question);
    uint8_t preferredChoice = 0U;
    const bool hasPreferredChoice = hasQuestion &&
        companionProgression.preferredQuestionChoice(question,
                                                       preferredChoice);
    kitsu868::progression::TimeBucket favoriteTime{};
    const bool hasFavorite = companionProgression.hasFavorite();
    const bool hasFavoriteTime = hasFavorite &&
        companionProgression.preferredTime(
            companionProgression.favoriteAction(), favoriteTime);
    uint32_t day = 0U;
    uint16_t minute = 0U;
    const bool hasLocalTime = currentLocalDayMinute(day, minute, true);
    const kitsu868::progression::TimeBucket currentBucket =
        hasLocalTime
            ? kitsu868::progression::CompanionProgression::timeBucket(minute)
            : kitsu868::progression::TimeBucket::Day;
    kitsu868::dialogue::Action routineAction{};
    const bool hasRoutine = hasLocalTime &&
        companionProgression.recognizedRoutine(currentBucket, routineAction);
    const bool hasRitual = companionProgression.hasRitual();
    const bool hasHabit = companionProgression.hasSecretHabit();
    const kitsu868::progression::ComfortKind comfort =
        companionProgression.comfortNeed();
    const kitsu868::progression::DisplayLine comfortText =
        kitsu868::progression::CompanionProgression::comfortLine(comfort);
    String output;
    output.reserve(1024U);
    output = "KITSU_PROFILE {\"ready\":true,\"nickname\":\"";
    output += jsonEscaped(String(companionProgression.nickname()));
    output += "\",\"streak\":" +
        String(companionProgression.currentStreak());
    output += ",\"perfect_days\":" + String(companionProgression.perfectDays());
    output += ",\"actions\":" + String(companionProgression.totalActions());
    output += ",\"achievements\":" +
        String(companionProgression.achievementMask());
    output += ",\"lore\":" + String(companionProgression.loreMask());
    output += ",\"favorite\":";
    output += hasFavorite
                  ? String("\"") + progressionActionName(
                        companionProgression.favoriteAction()) + "\""
                  : "null";
    output += ",\"favorite_time\":";
    output += hasFavoriteTime
                  ? String("\"") + progressionTimeName(favoriteTime) + "\""
                  : "null";
    output += ",\"routine\":";
    output += hasRoutine
                  ? String("\"") + progressionActionName(routineAction) +
                        "@" + progressionTimeName(currentBucket) + "\""
                  : "null";
    output += ",\"ritual\":";
    if (hasRitual) {
      output += "{\"action\":\"";
      output += progressionActionName(companionProgression.ritualAction());
      output += "\",\"time\":\"";
      output += progressionTimeName(companionProgression.ritualTime());
      output += "\",\"streak\":" +
          String(companionProgression.ritualStreak()) + "}";
    } else {
      output += "null";
    }
    output += ",\"habit\":";
    if (hasHabit) {
      const kitsu868::progression::DisplayLine habit =
          kitsu868::progression::CompanionProgression::habitLine(
              companionProgression.secretHabit());
      output += "{\"id\":" +
          String(static_cast<unsigned>(companionProgression.secretHabit())) +
          ",\"line1\":\"" + jsonEscaped(String(habit.line1)) +
          "\",\"line2\":\"" + jsonEscaped(String(habit.line2)) + "\"}";
    } else {
      output += "null";
    }
    output += ",\"question\":";
    if (hasQuestion) {
      output += "{\"id\":" + String(static_cast<unsigned>(question)) +
          ",\"option0\":\"" +
          jsonEscaped(String(kitsu868::progression::CompanionProgression::
                                 questionOption(question, 0U))) +
          "\",\"option1\":\"" +
          jsonEscaped(String(kitsu868::progression::CompanionProgression::
                                 questionOption(question, 1U))) +
          "\",\"preferred\":";
      output += hasPreferredChoice ? String(preferredChoice) : "null";
      output += "}";
    } else {
      output += "null";
    }
    output += ",\"comfort\":{\"id\":" +
        String(static_cast<unsigned>(comfort)) + ",\"line1\":\"" +
        jsonEscaped(String(comfortText.line1)) + "\",\"line2\":\"" +
        jsonEscaped(String(comfortText.line2)) + "\"}";
    output += ",\"speech\":" + String(companionProgression.speechStage());
    output += ",\"bond_bank\":" +
        String(companionProgression.bondDialogueBank(
            companionBrain.bondLevel()));
    output += ",\"momentum\":" + String(companionProgression.moodMomentum());
    output += ",\"bests\":{\"daily\":" + String(bests.dailyActions) +
        ",\"variety\":" + String(bests.dailyVariety) +
        ",\"chain\":" + String(bests.varietyChain) +
        ",\"rhythm\":" + String(bests.careRhythm) +
        ",\"streak\":" + String(bests.streak) + "}";
    output += ",\"requests\":{\"accepted\":" +
        String(companionProgression.acceptedRequests()) +
        ",\"declined\":" +
        String(companionProgression.declinedRequests()) + "}";
    output += ",\"comeback\":";
    output += companionProgression.lastDayWasComeback() ? "true" : "false";
    output += ",\"quick\":\"" + String(quickActionName(
        activitySuite.quickAction())) + "\",\"quiet\":";
    output += activitySuite.snapshot().quietHoursEnabled ? "true" : "false";
    output += "}";
    Serial.println(output);
    if (lowered == "profile inspect") printProgressionCatalogue();
    return true;
  }
  if (lowered.startsWith("profile nickname ")) {
    const String nickname = command.substring(17);
    uint8_t before[kitsu868::progression::kSnapshotCapacity]{};
    if (!captureCompanionProgression(before) ||
        !companionProgression.setNickname(nickname.c_str())) {
      Serial.println("KITSU_ERROR profile=invalid_nickname");
    } else if (!persistCompanionProgression()) {
      restoreCompanionProgression(before);
      Serial.println("KITSU_ERROR profile=nickname_store_failed");
    } else {
      Serial.print("KITSU_PROFILE nickname=\"");
      Serial.print(jsonEscaped(String(companionProgression.nickname())));
      Serial.println("\"");
    }
    return true;
  }
  if (lowered.startsWith("profile quick ")) {
    kitsu868::activities::QuickAction action{};
    const kitsu868::activities::ActivityState before = activitySuite.snapshot();
    if (!parseQuickAction(lowered.substring(14), action) ||
        !activitySuite.setQuickAction(action)) {
      Serial.println("KITSU_ERROR profile=invalid_quick_action");
    } else if (!persistActivityState()) {
      (void)activitySuite.restore(before);
      Serial.println("KITSU_ERROR profile=quick_store_failed");
    } else {
      Serial.printf("KITSU_PROFILE quick=%s\n", quickActionName(action));
    }
    return true;
  }
  if (lowered == "profile quiet off") {
    const kitsu868::activities::ActivityState before = activitySuite.snapshot();
    if (!activitySuite.setQuietHours(false, 0U, 1U)) {
      Serial.println("KITSU_ERROR profile=invalid_quiet_hours");
    } else if (!persistActivityState()) {
      (void)activitySuite.restore(before);
      Serial.println("KITSU_ERROR profile=quiet_store_failed");
    } else {
      Serial.println("KITSU_PROFILE quiet=off");
    }
    return true;
  }
  if (lowered.startsWith("profile quiet ")) {
    const String range = lowered.substring(14);
    const int separator = range.indexOf('-');
    uint16_t start = 0U;
    uint16_t end = 0U;
    const kitsu868::activities::ActivityState before = activitySuite.snapshot();
    if (separator != 5 || !parseMinuteText(range.substring(0, 5), start) ||
        !parseMinuteText(range.substring(6), end) ||
        !activitySuite.setQuietHours(true, start, end)) {
      Serial.println("KITSU_ERROR profile=invalid_quiet_hours");
    } else if (!persistActivityState()) {
      (void)activitySuite.restore(before);
      Serial.println("KITSU_ERROR profile=quiet_store_failed");
    } else {
      Serial.printf("KITSU_PROFILE quiet=%s\n", range.c_str());
    }
    return true;
  }
  if (lowered == "profile replay") {
    replayLastDialogue();
    return true;
  }
  if (lowered == "profile request accept" ||
      lowered == "profile request decline") {
    const bool accept = lowered.endsWith("accept");
    uint8_t before[kitsu868::progression::kSnapshotCapacity]{};
    if (!captureCompanionProgression(before)) {
      Serial.println("KITSU_ERROR profile=request_unavailable");
      return true;
    }
    const kitsu868::progression::RequestResult result =
        companionProgression.answerRequest(accept);
    if (!result.valid) {
      Serial.println("KITSU_ERROR profile=request_unavailable");
    } else if (!persistCompanionProgression()) {
      restoreCompanionProgression(before);
      Serial.println("KITSU_ERROR profile=request_store_failed");
    } else {
      Serial.printf("KITSU_PROFILE request=%s action=%u\n",
                    accept ? "accepted" : "declined",
                    static_cast<unsigned>(result.requestedAction));
    }
    return true;
  }
  if (lowered.startsWith("profile answer ")) {
    const String value = lowered.substring(15);
    if (value.length() != 1U || value[0] < '0' || value[0] > '1') {
      Serial.println("KITSU_ERROR profile=invalid_answer");
    } else {
      uint8_t before[kitsu868::progression::kSnapshotCapacity]{};
      if (!captureCompanionProgression(before)) {
        Serial.println("KITSU_ERROR profile=question_unavailable");
        return true;
      }
      const kitsu868::progression::QuestionResult result =
          companionProgression.answerQuestion(
              static_cast<uint8_t>(value[0] - '0'));
      if (!result.valid) {
        Serial.println("KITSU_ERROR profile=question_unavailable");
      } else if (!persistCompanionProgression()) {
        restoreCompanionProgression(before);
        Serial.println("KITSU_ERROR profile=question_store_failed");
      } else {
        Serial.printf("KITSU_PROFILE answer=%u question=%u\n", result.choice,
                      static_cast<unsigned>(result.question));
      }
    }
    return true;
  }
  return false;
}

const char* modePartyHostPhaseName(kitsu868::party_modes::HostPhase phase) {
  using kitsu868::party_modes::HostPhase;
  switch (phase) {
    case HostPhase::Idle: return "idle";
    case HostPhase::Lobby: return "lobby";
    case HostPhase::Active: return "active";
    case HostPhase::Complete: return "complete";
    case HostPhase::Cancelled: return "cancelled";
    case HostPhase::Expired: return "expired";
    case HostPhase::Unavailable: return "unavailable";
  }
  return "unavailable";
}

const char* modePartyGuestPhaseName(
    kitsu868::party_modes::ParticipantPhase phase) {
  using kitsu868::party_modes::ParticipantPhase;
  switch (phase) {
    case ParticipantPhase::Idle: return "idle";
    case ParticipantPhase::Observed: return "observed";
    case ParticipantPhase::Joining: return "joining";
    case ParticipantPhase::Lobby: return "lobby";
    case ParticipantPhase::Active: return "active";
    case ParticipantPhase::Complete: return "complete";
    case ParticipantPhase::Cancelled: return "cancelled";
    case ParticipantPhase::Expired: return "expired";
    case ParticipantPhase::Unavailable: return "unavailable";
  }
  return "unavailable";
}

void printSocialCommandError(const char* error) {
  Serial.printf("KITSU_ERROR social=%s\n", error ? error : "failed");
}

bool executeSocialCommand(const String& raw) {
  String command = raw;
  command.trim();
  command.toLowerCase();
  if (command == "social status") {
    const kitsu868::social::SocialState state = socialProgression.snapshot();
    uint32_t day = 0U;
    uint16_t minute = 0U;
    const bool trusted = currentLocalDayMinute(day, minute, true);
    const kitsu868::party_modes::Mode dailyMode =
        kitsu868::party_modes::rotatingMode(
            trusted ? day : 1U, companionBrain.deviceFingerprint());
    const kitsu868::party::HostState signalHost = partyHost.snapshot();
    const kitsu868::party::ParticipantState signalGuest =
        partyParticipant.snapshot();
    const kitsu868::party::SessionPhase signalHostPhase =
        static_cast<kitsu868::party::SessionPhase>(signalHost.phase);
    const kitsu868::party::SessionPhase signalGuestPhase =
        static_cast<kitsu868::party::SessionPhase>(signalGuest.phase);
    const bool hasSignalHost =
        signalHostPhase != kitsu868::party::SessionPhase::Idle &&
        signalHostPhase != kitsu868::party::SessionPhase::Unavailable;
    const bool hasSignalGuest = !hasSignalHost &&
        signalGuestPhase != kitsu868::party::SessionPhase::Idle &&
        signalGuestPhase != kitsu868::party::SessionPhase::Unavailable;
    if (hasSignalHost || hasSignalGuest) {
      const kitsu868::party::SessionPhase signalPhase =
          hasSignalHost ? signalHostPhase : signalGuestPhase;
      const kitsu868::party::HuntResult signalResult =
          hasSignalHost ? signalHost.result : signalGuest.result;
      const uint16_t signalHostUid =
          hasSignalHost ? signalHost.hostUid : signalGuest.hostUid;
      const uint32_t signalNonce = hasSignalHost
          ? signalHost.sessionNonce : signalGuest.sessionNonce;
      const uint8_t signalParticipants = hasSignalHost
          ? signalHost.participantCount : signalGuest.participantCount;
      const uint8_t signalRound = hasSignalHost
          ? signalHost.currentRound : signalGuest.currentRound;
      Serial.printf(
          "KITSU_SOCIAL {\"ready\":%s,\"friends\":%u,"
          "\"parties\":%lu,\"daily_unique\":%u,\"challenge\":%u,"
          "\"challenge_target\":%u,\"shared_trail\":%lu,"
          "\"cooperative_rare\":%lu,\"party_bond\":%lu,"
          "\"protocol\":\"p8\",\"ready_required\":false,"
          "\"daily_mode\":\"%s\",\"role\":\"%s\","
          "\"phase\":\"%s\",\"mode\":\"signal_hunt\","
          "\"host_uid\":%u,\"session_nonce\":%lu,"
          "\"participants\":%u,\"ready_count\":0,"
          "\"round\":%u,\"rounds\":%u,\"score\":%u,"
          "\"maximum_score\":%u,\"clock_trusted\":%s}\n",
          socialProgressionReady ? "true" : "false", state.peerCount,
          static_cast<unsigned long>(state.completedParties),
          state.dailyUniquePeers, state.dailyChallengeProgress,
          state.dailyChallengeTarget,
          static_cast<unsigned long>(state.sharedTrailProgress),
          static_cast<unsigned long>(state.cooperativeRareEncounters),
          static_cast<unsigned long>(partyRewards.state().partyBond),
          kitsu868::party_modes::modeName(dailyMode),
          hasSignalHost ? "host" : "guest",
          companion_api::partyPhaseWire(signalPhase),
          signalHostUid, static_cast<unsigned long>(signalNonce),
          signalParticipants, signalRound, kitsu868::party::kHuntRounds,
          signalResult.score, signalResult.maximumScore,
          trusted ? "true" : "false");
      (void)minute;
      return true;
    }
    const kitsu868::party_modes::HostState host = modePartyHost.snapshot();
    const kitsu868::party_modes::ParticipantState guest =
        modePartyParticipant.snapshot();
    const kitsu868::party_modes::HostPhase hostPhase =
        static_cast<kitsu868::party_modes::HostPhase>(host.phase);
    const kitsu868::party_modes::ParticipantPhase guestPhase =
        static_cast<kitsu868::party_modes::ParticipantPhase>(guest.phase);
    const bool hasHost = hostPhase != kitsu868::party_modes::HostPhase::Idle &&
        hostPhase != kitsu868::party_modes::HostPhase::Unavailable;
    const bool hasGuest = !hasHost &&
        guestPhase != kitsu868::party_modes::ParticipantPhase::Idle &&
        guestPhase != kitsu868::party_modes::ParticipantPhase::Unavailable;
    const kitsu868::party_modes::Mode selectedMode = hasHost
        ? static_cast<kitsu868::party_modes::Mode>(host.mode)
        : hasGuest ? static_cast<kitsu868::party_modes::Mode>(guest.mode)
                   : dailyMode;
    uint8_t readyCount = 0U;
    if (hasHost) {
      for (uint8_t index = 0U;
           index < kitsu868::party_modes::kMaximumParticipants; ++index) {
        if (host.members[index].active != 0U &&
            host.members[index].ready != 0U) {
          ++readyCount;
        }
      }
    } else if (hasGuest) {
      readyCount = guest.readyCount;
    }
    const uint32_t sessionNonce =
        hasHost ? host.sessionNonce : hasGuest ? guest.sessionNonce : 0U;
    const uint16_t hostUid =
        hasHost ? host.hostUid : hasGuest ? guest.hostUid : 0U;
    const uint8_t participants = hasHost
        ? host.participantCount : hasGuest ? guest.participantCount : 0U;
    const uint8_t round =
        hasHost ? host.currentRound : hasGuest ? guest.currentRound : 0U;
    const uint8_t rounds =
        hasHost ? host.totalRounds : hasGuest ? guest.totalRounds : 0U;
    const kitsu868::party_modes::ModeResult result =
        hasHost ? host.result : guest.result;
    const bool idleSignalHunt = !hasHost && !hasGuest &&
        selectedMode == kitsu868::party_modes::Mode::SignalHunt;
    Serial.printf(
        "KITSU_SOCIAL {\"ready\":%s,\"friends\":%u,"
        "\"parties\":%lu,\"daily_unique\":%u,\"challenge\":%u,"
        "\"challenge_target\":%u,\"shared_trail\":%lu,"
        "\"cooperative_rare\":%lu,\"party_bond\":%lu,"
        "\"protocol\":\"%s\",\"ready_required\":%s,"
        "\"daily_mode\":\"%s\",\"role\":\"%s\","
        "\"phase\":\"%s\",\"mode\":\"%s\","
        "\"host_uid\":%u,\"session_nonce\":%lu,"
        "\"participants\":%u,\"ready_count\":%u,"
        "\"round\":%u,\"rounds\":%u,\"score\":%u,"
        "\"maximum_score\":%u,\"clock_trusted\":%s}\n",
        socialProgressionReady ? "true" : "false", state.peerCount,
        static_cast<unsigned long>(state.completedParties),
        state.dailyUniquePeers, state.dailyChallengeProgress,
        state.dailyChallengeTarget,
        static_cast<unsigned long>(state.sharedTrailProgress),
        static_cast<unsigned long>(state.cooperativeRareEncounters),
        static_cast<unsigned long>(partyRewards.state().partyBond),
        idleSignalHunt ? "p8" : "m8",
        idleSignalHunt ? "false" : "true",
        kitsu868::party_modes::modeName(dailyMode),
        hasHost ? "host" : hasGuest ? "guest" : "none",
        hasHost ? modePartyHostPhaseName(hostPhase)
                : modePartyGuestPhaseName(guestPhase),
        kitsu868::party_modes::modeName(selectedMode), hostUid,
        static_cast<unsigned long>(sessionNonce), participants, readyCount,
        round, rounds, result.score, result.maximumScore,
        trusted ? "true" : "false");
    (void)minute;
    return true;
  }
  if (command == "social leaderboard") {
    kitsu868::social::LeaderboardEntry entries[
        kitsu868::social::kPeerCapacity]{};
    const uint8_t count = socialProgression.leaderboard(
        entries, kitsu868::social::kPeerCapacity);
    for (uint8_t index = 0U; index < count; ++index) {
      Serial.printf("KITSU_FRIEND rank=%u uid=KT%04X points=%u parties=%u best=%u\n",
                    index + 1U, entries[index].uid,
                    entries[index].friendshipPoints,
                    entries[index].successfulParties,
                    entries[index].bestPartyScore);
    }
    if (count == 0U) Serial.println("KITSU_FRIEND empty=true");
    return true;
  }
  if (command == "social scan") {
    const char* error = nullptr;
    if (!startAllPartyScans(error)) printSocialCommandError(error);
    return true;
  }
  if (command == "social host") {
    const char* error = nullptr;
    if (!startRotatingModePartyHost(error)) printSocialCommandError(error);
    return true;
  }
  if (command.startsWith("social host ")) {
    kitsu868::party_modes::Mode mode{};
    if (!parseModePartyName(command.substring(12), mode)) {
      printSocialCommandError("unknown_mode");
      return true;
    }
    const char* error = nullptr;
    if (!startModePartyHost(mode, error)) printSocialCommandError(error);
    return true;
  }
  if (command == "social join") {
    const char* error = nullptr;
    const uint32_t now = millis();
    const kitsu868::party::ParticipantState signal =
        partyParticipant.snapshot();
    const bool signalObserved =
        static_cast<kitsu868::party::SessionPhase>(signal.phase) ==
            kitsu868::party::SessionPhase::Joining &&
        signal.deadlineMs != 0U &&
        static_cast<int32_t>(now - signal.deadlineMs) < 0 &&
        lastPartyBeaconAt != 0U &&
        static_cast<uint32_t>(now - lastPartyBeaconAt) <=
            PARTY_DISCOVERY_TTL_MS;
    const bool joined = signalObserved
        ? joinObservedParty(signal.hostUid, signal.sessionNonce, error)
        : joinObservedModeParty(error);
    if (joined) {
      if (signalObserved) {
        modePartyParticipant.reset();
        resetModePartyRuntimeView();
      } else {
        partyParticipant.reset();
        resetPartyRuntimeView();
      }
      partyScanActive = false;
      modePartyScanActive = false;
    } else {
      printSocialCommandError(error);
    }
    return true;
  }
  if (command == "social ready" || command == "social ready 1" ||
      command == "social ready 0") {
    const kitsu868::party::SessionPhase signalHostPhase =
        static_cast<kitsu868::party::SessionPhase>(partyHost.state().phase);
    const kitsu868::party::SessionPhase signalGuestPhase =
        static_cast<kitsu868::party::SessionPhase>(
            partyParticipant.state().phase);
    if ((signalHostPhase != kitsu868::party::SessionPhase::Idle &&
         signalHostPhase != kitsu868::party::SessionPhase::Unavailable) ||
        (signalGuestPhase != kitsu868::party::SessionPhase::Idle &&
         signalGuestPhase != kitsu868::party::SessionPhase::Unavailable)) {
      printSocialCommandError("ready_not_required");
      return true;
    }
    const bool ready = !command.endsWith(" 0");
    const char* error = nullptr;
    if (!setModePartyReady(ready, error)) printSocialCommandError(error);
    return true;
  }
  if (command == "social begin") {
    const char* error = nullptr;
    const kitsu868::party::SessionPhase signalHostPhase =
        static_cast<kitsu868::party::SessionPhase>(partyHost.state().phase);
    const kitsu868::party::SessionPhase signalGuestPhase =
        static_cast<kitsu868::party::SessionPhase>(
            partyParticipant.state().phase);
    const bool signalSelected =
        (signalHostPhase != kitsu868::party::SessionPhase::Idle &&
         signalHostPhase != kitsu868::party::SessionPhase::Unavailable) ||
        (signalGuestPhase != kitsu868::party::SessionPhase::Idle &&
         signalGuestPhase != kitsu868::party::SessionPhase::Unavailable);
    const bool started = signalSelected ? beginHostedParty(error)
                                        : beginModeParty(error);
    if (!started) printSocialCommandError(error);
    return true;
  }
  if (command.startsWith("social contribute ")) {
    const String valueText = command.substring(18);
    char* end = nullptr;
    const long value = strtol(valueText.c_str(), &end, 10);
    if (valueText.length() == 0U || !end || *end != '\0') {
      printSocialCommandError("invalid_contribution");
      return true;
    }
    const char* error = nullptr;
    const kitsu868::party::HostState signalHost = partyHost.snapshot();
    const kitsu868::party::ParticipantState signalGuest =
        partyParticipant.snapshot();
    const kitsu868::party::SessionPhase signalHostPhase =
        static_cast<kitsu868::party::SessionPhase>(signalHost.phase);
    const kitsu868::party::SessionPhase signalGuestPhase =
        static_cast<kitsu868::party::SessionPhase>(signalGuest.phase);
    const bool signalSelected =
        (signalHostPhase != kitsu868::party::SessionPhase::Idle &&
         signalHostPhase != kitsu868::party::SessionPhase::Unavailable) ||
        (signalGuestPhase != kitsu868::party::SessionPhase::Idle &&
         signalGuestPhase != kitsu868::party::SessionPhase::Unavailable);
    bool submitted = false;
    if (signalSelected) {
      if (value < 0L || value > 2L) {
        printSocialCommandError("invalid_contribution");
        return true;
      }
      const uint8_t round = partyPhaseRunning(signalHostPhase)
          ? signalHost.currentRound : signalGuest.currentRound;
      submitted = choosePartySignal(
          static_cast<kitsu868::party::SignalChoice>(value + 1L), round,
          error);
    } else {
      submitted = submitModePartyContribution(value, error);
    }
    if (!submitted) {
      printSocialCommandError(error);
    }
    return true;
  }
  if (command == "social leave") {
    if (!partyRuntimeBusy()) {
      printSocialCommandError("party_inactive");
    } else {
      leavePartyHotspot();
    }
    return true;
  }
  return false;
}

bool parseActivityKind(const String& name,
                       kitsu868::activities::ActivityKind& kind) {
  using kitsu868::activities::ActivityKind;
  if (name == "morse") kind = ActivityKind::MorseSignal;
  else if (name == "tuner") kind = ActivityKind::StaticTuner;
  else if (name == "flash") kind = ActivityKind::ReactionFlash;
  else if (name == "steady") kind = ActivityKind::HoldSteady;
  else if (name == "breathe") kind = ActivityKind::PulseBreathing;
  else return false;
  return true;
}

bool executeActivityCommand(const String& raw) {
  String command = raw;
  command.trim();
  command.toLowerCase();
  if (command == "activity status") {
    const kitsu868::activities::ActivityView view =
        activitySuite.view(millis());
    Serial.printf("KITSU_ACTIVITY status=%u kind=%s score=%u/%u progress=%u/%u\n",
                  static_cast<unsigned>(view.phase),
                  kitsu868::activities::activityName(view.kind), view.score,
                  view.maximumScore, view.progress, view.total);
  } else if (command == "activity daily") {
    (void)startActivity(kitsu868::activities::ActivityKind::MorseSignal,
                        true, false);
  } else if (command == "activity ghost") {
    (void)startActivity(kitsu868::activities::ActivityKind::MorseSignal,
                        false, true);
  } else if (command.startsWith("activity start ")) {
    kitsu868::activities::ActivityKind kind{};
    if (!parseActivityKind(command.substring(15), kind)) {
      Serial.println("KITSU_ERROR activity=unknown_kind");
    } else {
      (void)startActivity(kind, false, false);
    }
  } else if (command == "activity tap") {
    const kitsu868::activities::ActivityState before = activitySuite.snapshot();
    const kitsu868::activities::InputResult result =
        activitySuite.tap(millis());
    if (result == kitsu868::activities::InputResult::Ignored ||
        result == kitsu868::activities::InputResult::Invalid) {
      Serial.println("KITSU_ERROR activity=tap_ignored");
    } else {
      (void)persistActivityMutation(before, "tap");
    }
  } else if (command == "activity press") {
    const kitsu868::activities::ActivityState before = activitySuite.snapshot();
    const kitsu868::activities::InputResult result =
        activitySuite.press(millis());
    if (result == kitsu868::activities::InputResult::Ignored ||
        result == kitsu868::activities::InputResult::Invalid) {
      Serial.println("KITSU_ERROR activity=press_ignored");
    } else {
      (void)persistActivityMutation(before, "press");
    }
  } else if (command == "activity release") {
    const kitsu868::activities::ActivityState before = activitySuite.snapshot();
    const kitsu868::activities::InputResult result =
        activitySuite.release(millis());
    if (result == kitsu868::activities::InputResult::Ignored ||
        result == kitsu868::activities::InputResult::Invalid) {
      Serial.println("KITSU_ERROR activity=release_ignored");
    } else {
      (void)persistActivityMutation(before, "release");
    }
  } else if (command == "activity cancel") {
    if (screen != Screen::Activity && !activityRuntimeBusy()) {
      Serial.println("KITSU_ERROR activity=inactive");
    } else {
      leaveActivity();
    }
  } else {
    return false;
  }
  return true;
}

void printAdventureStatus() {
  const kitsu868::adventure::RouteView route = adventureProgression.view();
  const kitsu868::adventure::ProgressState progress =
      adventureProgression.snapshot();
  const kitsu868::expedition::ExpeditionView trip = expeditionCore.view();
  Serial.printf(
      "KITSU_ADVENTURE {\"trip_phase\":%u,\"trip_progress\":%u,"
      "\"route_phase\":%u,\"route_id\":%lu,\"steps\":%lu,"
      "\"target\":%lu,\"progress\":%u,\"terrain\":\"%s\","
      "\"objective\":\"%s\",\"risk\":\"%s\",\"weather\":\"%s\","
      "\"time_band\":\"%s\",\"moon\":\"%s\",\"chain\":%u,"
      "\"variant\":%u,\"party_size\":%u,\"distance_m\":%lu,"
      "\"home_comfort\":%u,\"community_day\":%s,"
      "\"community_visits\":%u,\"community_bonuses\":%u,"
      "\"privacy\":%u,\"total_distance_m\":%lu,\"journal\":%u}\n",
      static_cast<unsigned>(trip.phase), trip.progressPercent,
      static_cast<unsigned>(route.phase),
      static_cast<unsigned long>(route.routeId),
      static_cast<unsigned long>(route.steps),
      static_cast<unsigned long>(route.targetSteps), route.progressPercent,
      terrainLabel(route.terrain), objectiveLabel(route.objective),
      riskLabel(route.risk), weatherLabel(route.weather),
      timeBandLabel(route.timeBand), moonPhaseLabel(route.moonPhase),
      route.chainDepth, route.routeVariant, route.partySize,
      static_cast<unsigned long>(route.routeDistanceMeters), route.homeComfort,
      route.communityDayActive ? "true" : "false",
      progress.communityHotspotVisits, progress.communityDayBonuses,
      progress.privacyMode,
      static_cast<unsigned long>(progress.totalDistanceMeters),
      adventureProgression.journalCount());
}

bool executeAdventureCommand(const String& raw) {
  String command = raw;
  command.trim();
  command.toLowerCase();
  if (command == "adventure status") {
    printAdventureStatus();
  } else if (command == "adventure start short" ||
             command == "adventure start medium" ||
             command == "adventure start long") {
    const kitsu868::expedition::Duration duration =
        command.endsWith("short")
            ? kitsu868::expedition::Duration::Short
            : command.endsWith("medium")
                  ? kitsu868::expedition::Duration::Medium
                  : kitsu868::expedition::Duration::Long;
    const char* error = nullptr;
    if (!startExpedition(duration, error)) {
      Serial.printf("KITSU_ERROR expedition=%s\n", error ? error : "start");
      if (error && strcmp(error, "clock_unavailable") == 0) {
        beginClockEditor();
      }
    }
  } else if (command == "adventure route" ||
             command == "adventure route commute") {
    (void)beginAdventureRoute(command.endsWith("commute"));
  } else if (command.startsWith("adventure steps ")) {
    char* end = nullptr;
    const unsigned long steps =
        strtoul(command.substring(16).c_str(), &end, 10);
    if (!end || *end != '\0' || steps == 0U || steps > 100000UL) {
      Serial.println("KITSU_ERROR adventure=invalid_steps");
    } else {
      const kitsu868::adventure::ProgressState before =
          adventureProgression.snapshot();
      const kitsu868::adventure::Status status =
          adventureProgression.addSteps(static_cast<uint32_t>(steps));
      if (status == kitsu868::adventure::Status::Ok) {
        (void)persistAdventureMutation(before, "steps");
      } else {
        Serial.printf("KITSU_ERROR adventure=%s\n",
                      kitsu868::adventure::statusName(status));
      }
    }
  } else if (command.startsWith("adventure decide ")) {
    const String choice = command.substring(17);
    kitsu868::adventure::MidDecision decision{};
    if (choice == "continue") decision = kitsu868::adventure::MidDecision::Continue;
    else if (choice == "detour") decision = kitsu868::adventure::MidDecision::Detour;
    else if (choice == "help") decision = kitsu868::adventure::MidDecision::Help;
    else if (choice == "return") decision = kitsu868::adventure::MidDecision::ReturnEarly;
    else {
      Serial.println("KITSU_ERROR adventure=invalid_decision");
      return true;
    }
    (void)applyAdventureDecision(decision);
  } else if (command == "adventure finish") {
    const kitsu868::adventure::ProgressState before =
        adventureProgression.snapshot();
    const kitsu868::adventure::Status status =
        adventureProgression.finish(adventureClock(millis()));
    if (status == kitsu868::adventure::Status::Ok ||
        status == kitsu868::adventure::Status::RescueRequired) {
      if (!persistAdventureMutation(before, "finish")) return true;
    }
    Serial.printf("KITSU_ADVENTURE finish=%s\n",
                  kitsu868::adventure::statusName(status));
  } else if (command == "adventure acknowledge") {
    const uint32_t id = adventureProgression.view().routeId;
    const kitsu868::adventure::ProgressState before =
        adventureProgression.snapshot();
    const kitsu868::adventure::Status status =
        adventureProgression.acknowledge(id);
    if (status == kitsu868::adventure::Status::Ok) {
      if (!persistAdventureMutation(before, "acknowledge")) return true;
    }
    Serial.printf("KITSU_ADVENTURE acknowledge=%s\n",
                  kitsu868::adventure::statusName(status));
  } else if (command.startsWith("adventure privacy ")) {
    const String mode = command.substring(18);
    const kitsu868::adventure::PrivacyMode privacy =
        mode == "off" ? kitsu868::adventure::PrivacyMode::Off
        : mode == "coarse" ? kitsu868::adventure::PrivacyMode::Coarse
        : mode == "precise" ? kitsu868::adventure::PrivacyMode::PreciseTransient
        : kitsu868::adventure::PrivacyMode::Count;
    const kitsu868::adventure::ProgressState before =
        adventureProgression.snapshot();
    const kitsu868::adventure::Status status =
        adventureProgression.setPrivacyMode(
            privacy, companionBrain.deviceFingerprint() ^ UINT32_C(0xA7D04319));
    if (status == kitsu868::adventure::Status::Ok) {
      if (!persistAdventureMutation(before, "privacy")) return true;
    }
    Serial.printf("KITSU_ADVENTURE privacy=%s result=%s\n", mode.c_str(),
                  kitsu868::adventure::statusName(status));
  } else if (command.startsWith("adventure home ")) {
    unsigned long token = 0U;
    char trailing = 0;
    if (sscanf(command.c_str(), "adventure home %lx %c", &token,
               &trailing) != 1 || token == 0U) {
      Serial.println("KITSU_ERROR adventure=invalid_home_zone");
    } else {
      const kitsu868::adventure::ProgressState before =
          adventureProgression.snapshot();
      const kitsu868::adventure::Status status =
          adventureProgression.setHomeZone(static_cast<uint32_t>(token));
      if (status == kitsu868::adventure::Status::Ok) {
        if (!persistAdventureMutation(before, "home")) return true;
      }
      Serial.printf("KITSU_ADVENTURE home=%08lX result=%s\n", token,
                    kitsu868::adventure::statusName(status));
    }
  } else if (command.startsWith("adventure zone ")) {
    unsigned long token = 0U;
    unsigned long steps = 0U;
    unsigned long meters = 0U;
    char trailing = 0;
    if (sscanf(command.c_str(), "adventure zone %lx %lu %lu %c", &token,
               &steps, &meters, &trailing) != 3 || token == 0U) {
      Serial.println("KITSU_ERROR adventure=invalid_zone");
    } else {
      const kitsu868::adventure::ProgressState before =
          adventureProgression.snapshot();
      kitsu868::adventure::LocationUpdate update{};
      const kitsu868::adventure::Status status =
          adventureProgression.observeCoarseZone(
              static_cast<uint32_t>(token), static_cast<uint32_t>(steps),
              static_cast<uint32_t>(meters), update);
      if (status == kitsu868::adventure::Status::Ok) {
        if (!persistAdventureMutation(before, "zone")) return true;
      }
      Serial.printf("KITSU_ADVENTURE zone=%08lX result=%s new=%u familiarity=%u distance=%lu\n",
                    token, kitsu868::adventure::statusName(status),
                    update.newZone, static_cast<unsigned>(update.familiarity),
                    static_cast<unsigned long>(update.totalDistanceMeters));
    }
  } else if (command.startsWith("adventure precise ")) {
    long latitude = 0;
    long longitude = 0;
    unsigned accuracy = 0U;
    unsigned long steps = 0U;
    unsigned long meters = 0U;
    char trailing = 0;
    if (sscanf(command.c_str(), "adventure precise %ld %ld %u %lu %lu %c",
               &latitude, &longitude, &accuracy, &steps, &meters,
               &trailing) != 5 || accuracy > UINT16_MAX) {
      Serial.println("KITSU_ERROR adventure=invalid_precise_sample");
    } else {
      const kitsu868::adventure::ProgressState before =
          adventureProgression.snapshot();
      kitsu868::adventure::PreciseLocationSample sample{};
      sample.latitudeE7 = static_cast<int32_t>(latitude);
      sample.longitudeE7 = static_cast<int32_t>(longitude);
      sample.accuracyMeters = static_cast<uint16_t>(accuracy);
      kitsu868::adventure::LocationUpdate update{};
      const kitsu868::adventure::Status status =
          adventureProgression.observePreciseTransient(
              sample, static_cast<uint32_t>(steps),
              static_cast<uint32_t>(meters), update);
      sample = kitsu868::adventure::PreciseLocationSample{};
      if (status == kitsu868::adventure::Status::Ok) {
        if (!persistAdventureMutation(before, "precise")) return true;
      }
      Serial.printf("KITSU_ADVENTURE precise=result_%s zone=%08lX raw_persisted=false\n",
                    kitsu868::adventure::statusName(status),
                    static_cast<unsigned long>(update.zoneToken));
    }
  } else if (command.startsWith("adventure hotspot ")) {
    unsigned long token = 0U;
    unsigned participants = 0U;
    char trailing = 0;
    if (sscanf(command.c_str(), "adventure hotspot %lx %u %c", &token,
               &participants, &trailing) != 2 || token == 0U ||
        participants < 2U || participants > 4U) {
      Serial.println("KITSU_ERROR adventure=invalid_hotspot");
    } else {
      const kitsu868::adventure::ProgressState before =
          adventureProgression.snapshot();
      kitsu868::adventure::HotspotUpdate update{};
      const kitsu868::adventure::Status status =
          adventureProgression.observeHotspot(
              static_cast<uint32_t>(token),
              static_cast<uint8_t>(participants), adventureClock(millis()),
              update);
      if (status == kitsu868::adventure::Status::Ok) {
        if (!persistAdventureMutation(before, "hotspot")) return true;
      }
      Serial.printf("KITSU_ADVENTURE hotspot=%08lX result=%s visits=%u community_bonus=%u\n",
                    token, kitsu868::adventure::statusName(status),
                    update.hotspotVisits, update.bonusAwarded);
    }
  } else if (command.startsWith("adventure terrain ")) {
    const String value = command.substring(18);
    if (value == "meadow") adventureTerrain = kitsu868::adventure::Terrain::Meadow;
    else if (value == "forest") adventureTerrain = kitsu868::adventure::Terrain::Forest;
    else if (value == "ridge") adventureTerrain = kitsu868::adventure::Terrain::Ridge;
    else if (value == "water") adventureTerrain = kitsu868::adventure::Terrain::Waterfront;
    else if (value == "town") adventureTerrain = kitsu868::adventure::Terrain::Town;
    else {
      Serial.println("KITSU_ERROR adventure=invalid_terrain");
      return true;
    }
    Serial.printf("KITSU_ADVENTURE terrain=%s\n", terrainLabel(adventureTerrain));
  } else if (command.startsWith("adventure objective ")) {
    const String value = command.substring(20);
    if (value == "explore") adventureObjective = kitsu868::adventure::Objective::Explore;
    else if (value == "signal") adventureObjective = kitsu868::adventure::Objective::FollowSignal;
    else if (value == "creature") adventureObjective = kitsu868::adventure::Objective::MeetCreature;
    else if (value == "community") adventureObjective = kitsu868::adventure::Objective::Community;
    else if (value == "home") adventureObjective = kitsu868::adventure::Objective::ReturnHome;
    else {
      Serial.println("KITSU_ERROR adventure=invalid_objective");
      return true;
    }
    Serial.printf("KITSU_ADVENTURE objective=%s\n", objectiveLabel(adventureObjective));
  } else if (command.startsWith("adventure risk ")) {
    const String value = command.substring(15);
    if (value == "careful") adventureRisk = kitsu868::adventure::Risk::Careful;
    else if (value == "balanced") adventureRisk = kitsu868::adventure::Risk::Balanced;
    else if (value == "bold") adventureRisk = kitsu868::adventure::Risk::Bold;
    else {
      Serial.println("KITSU_ERROR adventure=invalid_risk");
      return true;
    }
    Serial.printf("KITSU_ADVENTURE risk=%s\n", riskLabel(adventureRisk));
  } else if (command.startsWith("adventure weather ")) {
    const String value = command.substring(18);
    if (value == "unknown") {
      adventureWeather = kitsu868::adventure::Weather::Unknown;
    } else if (value == "clear") {
      adventureWeather = kitsu868::adventure::Weather::Clear;
    } else if (value == "rain") {
      adventureWeather = kitsu868::adventure::Weather::Rain;
    } else if (value == "wind") {
      adventureWeather = kitsu868::adventure::Weather::Wind;
    } else if (value == "snow") {
      adventureWeather = kitsu868::adventure::Weather::Snow;
    } else {
      Serial.println("KITSU_ERROR adventure=invalid_weather");
      return true;
    }
    Serial.printf("KITSU_ADVENTURE weather=%s\n",
                  weatherLabel(adventureWeather));
  } else if (command == "adventure journal") {
    kitsu868::adventure::JournalEntry entry{};
    for (uint8_t index = 0U;
         adventureProgression.journalNewest(index, entry); ++index) {
      Serial.printf("KITSU_ADVENTURE_JOURNAL index=%u route=%lu day=%lu outcome=%u postcard=%u\n",
                    index, static_cast<unsigned long>(entry.routeId),
                    static_cast<unsigned long>(entry.dayId), entry.outcome,
                    entry.postcardId);
    }
  } else {
    return false;
  }
  return true;
}

void executeSerialCommand(String command) {
  kitsu868::chat::Command chatCommand{};
  const kitsu868::chat::ParseStatus chatStatus =
      kitsu868::chat::parseCommand(command.c_str(), command.length(),
                                   chatCommand);
  if (chatStatus == kitsu868::chat::ParseStatus::Ok) {
    executeChatCommand(chatCommand);
    return;
  }
  if (chatStatus != kitsu868::chat::ParseStatus::NotChat) {
    printChatResult("command", "rejected",
                    kitsu868::chat::parseStatusName(chatStatus));
    return;
  }

  command.trim();
  if (executeClockCommand(command)) return;
  if (executeProfileCommand(command)) return;
  if (executeSocialCommand(command)) return;
  if (executeActivityCommand(command)) return;
  if (executeAdventureCommand(command)) return;
  if (executeEncounterCodeSerial(command)) return;
  command.toLowerCase();
  if (executeMeshCommand(command)) return;
  if (command == "status" || command == "selftest") printSelfTest();
  else if (command == "sync" || command == "brain") printSync();
  else if (command == "journal") printJournal();
  else if (command == "guide list") printGuideList();
  else if (command == "pet") petWisp();
  else if (command == "feed") feedKitsu();
  else if (command == "play") playKitsu();
  else if (command == "game signal") startGame(ActiveGame::SignalCatch);
  else if (command == "game pounce") startGame(ActiveGame::PounceFetch);
  else if (command == "game echo") startGame(ActiveGame::EchoBeat);
  else if (command == "game tap") {
    if (screen == Screen::Game) handleShortPress(millis());
    else Serial.println("KITSU_ERROR game_inactive");
  } else if (command == "game cancel") {
    if (screen == Screen::Game) leaveGame(false);
    else Serial.println("KITSU_ERROR game_inactive");
  } else if (command.startsWith("inject ")) {
    injectEncounterHex(command.substring(7));
  }
  else if (command == "listen") startListening();
  else if (command == "sleep") {
    if (activeGame != ActiveGame::None) {
      Serial.println("KITSU_ERROR busy=game");
    } else if (requireCompanion()) {
      if (radioListening) stopListeningSafely();
      setSleeping(true);
      enterScreen(Screen::Sleep);
    }
  } else if (command == "wake") {
    if (!foregroundTransitionBlocked("wake")) {
      setSleeping(false);
      enterScreen(Screen::Pet);
    }
  } else if (command == "stop") {
    if (radioListening) stopListeningSafely();
  } else if (command == "pack") {
    printSelfTest();
  } else if (command.startsWith("anim ")) {
    if (foregroundTransitionBlocked("animation")) {
      return;
    }
    CompanionRole role = CompanionRole::Idle;
    const String roleText = command.substring(5);
    if (!parseAnimationRole(roleText, role)) {
      Serial.printf("KITSU_ERROR unknown_animation=%s\n", roleText.c_str());
    } else if (requireCompanion()) {
      cancelAmbientAnimation();
      if (startTransientAnimation(role)) {
        enterScreen(Screen::Pet);
        Serial.printf("KITSU_ANIM role=%s duration_ms=%lu mode=%u\n",
                      animationRoleName(role),
                      static_cast<unsigned long>(activeAnimation.spanMs),
                      static_cast<unsigned>(activeAnimation.mode));
      } else {
        Serial.printf("KITSU_ERROR missing_animation=%s\n",
                      animationRoleName(role));
        startBaseAnimation();
      }
    }
  } else if (command == "help") {
    Serial.println(
        "KITSU_HELP selftest|sync|journal|guide list|pet|feed|play|game <signal|pounce|echo|tap|cancel>|activity <status|start|daily|ghost|tap|press|release|cancel>|adventure <status|start|route|steps|decide|finish|acknowledge|privacy|home|zone|precise|hotspot|terrain|objective|risk|weather|journal>|profile <status|inspect|nickname|quick|quiet|replay|request|answer>|social <status|leaderboard|scan|host [mode]|join|ready [0|1]|begin|contribute value|leave>|clock <status|set|unix|offset|edit|ntp>|listen|sleep|wake|stop|inject <38hex>|anim <role>|mesh <status|config|time|tx|location|introduce|publish-map>|chat <status|contacts|channels|inbox|contact|channel|send>|help");
  } else if (command.length()) {
    Serial.printf("KITSU_ERROR unknown_command=%s\n", command.c_str());
  }
}

void pollSerial() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      if (serialControlRejected) {
        printChatResult("command", "rejected", "control_character");
        serialLine = "";
        serialControlRejected = false;
        serialOverflow = false;
      } else if (serialOverflow) {
        printChatResult("command", "rejected", "line_too_long");
        serialLine = "";
        serialOverflow = false;
      } else if (serialLine.length()) {
        executeSerialCommand(serialLine);
        // A command can synchronously cancel a queued MeshCore packet. Drain
        // it before accepting another line from the same USB burst so the
        // small transport ring remains lossless and journal order is stable.
        processMeshMessages();
        serialLine = "";
      }
    } else if (serialOverflow || serialControlRejected) {
      continue;
    } else if (static_cast<uint8_t>(c) < 0x20U ||
               static_cast<uint8_t>(c) == 0x7fU) {
      serialLine = "";
      serialControlRejected = true;
    } else if (serialLine.length() < kitsu868::chat::kInputLineMaxBytes) {
      serialLine += c;
    } else {
      serialLine = "";
      serialOverflow = true;
    }
  }
}

void initDisplay() {
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);
  delay(20);

  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(1);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(20);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(20);

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.beginTransmission(0x3c);
  oledDetected = Wire.endTransmission() == 0;
  if (!oledDetected) return;

  display.init();
  display.flipScreenVertically();
  display.clear();
  uiTextCentered("KITSU", 36, 2);
  uiTextCentered("868", 65, 2);
  display.display();
}

void initLocalSecurityStorage() {
  connectivitySecurityReady = false;
  discoveryJournalReady = false;
  discoveryBootId = esp_random();
  if (discoveryBootId == 0U) discoveryBootId = 1U;

  uint8_t hardwareId[8]{};
  const uint64_t factoryMac = ESP.getEfuseMac();
  memcpy(hardwareId, &factoryMac, sizeof(hardwareId));
  if (!connectivityStorage.begin()) {
    Serial.println("KITSU_WARN connectivity_storage=unavailable");
    return;
  }
  const kitsu868::connectivity::SecurityResult security =
      deviceSecurity.begin(connectivityStorage, connectivityPlatform,
                           hardwareId);
  memset(hardwareId, 0, sizeof(hardwareId));
  connectivitySecurityReady = deviceSecurity.ready();
  const kitsu868::connectivity::DeviceSecurityStatus securityStatus =
      deviceSecurity.status();
  Serial.printf(
      "KITSU_LOCAL_SECURITY status=%s security_mode=%s "
      "application_encrypted=%s hardware_root_protected=%s "
      "controllers=%u\n",
      kitsu868::connectivity::securityResultName(security),
      kitsu868::connectivity::kSelectedSecurityModeName,
      securityStatus.applicationEncrypted ? "true" : "false",
      securityStatus.hardwareRootProtected ? "true" : "false",
      securityStatus.controllerCount);
  if (!connectivitySecurityReady) return;
  Serial.println(
      "KITSU_REFLASHABLE secure_boot=false flash_encryption=false "
      "nvs_encryption=false application_encrypted=true "
      "owner_repurpose_allowed=true");

  uint8_t journalKey[kitsu868::connectivity::kKitsuSecretBytes]{};
  const kitsu868::connectivity::SecurityResult derived =
      deviceSecurity.deriveJournalKey(journalKey);
  const bool storageBegun = discoveryStorage.begin();
  const bool cryptoReady =
      derived == kitsu868::connectivity::SecurityResult::Ok &&
      discoveryCrypto.setKey(journalKey);
  memset(journalKey, 0, sizeof(journalKey));
  if (!storageBegun || !cryptoReady) {
    Serial.printf("KITSU_WARN discovery_init=dependency storage=%s crypto=%s\n",
                  storageBegun ? "true" : "false",
                  cryptoReady ? "true" : "false");
    return;
  }
  const kitsu868::discovery::JournalResult begun =
      discoveryJournal.begin(discoveryStorage, discoveryCrypto);
  discoveryJournalReady =
      begun == kitsu868::discovery::JournalResult::Ok;
  Serial.printf("KITSU_DISCOVERY_JOURNAL status=%s ready=%s\n",
                kitsu868::discovery::journalResultName(begun),
                discoveryJournalReady ? "true" : "false");
}


void initMesh() {
  meshSettings = kitsu868::mesh::defaultSettings();
  const kitsu868::mesh::StoreStatus loaded =
      meshSettingsStore.load(meshSettings);
  if (loaded == kitsu868::mesh::StoreStatus::Missing) {
    const kitsu868::mesh::StoreStatus saved =
        meshSettingsStore.save(meshSettings);
    if (saved != kitsu868::mesh::StoreStatus::Saved) {
      Serial.printf("KITSU_WARN mesh_settings=%s\n",
                    kitsu868::mesh::storeStatusName(saved));
    }
  } else if (loaded != kitsu868::mesh::StoreStatus::Loaded) {
    // Never overwrite a malformed record automatically.  Run with the safe,
    // disabled UK/EU Narrow defaults and let the owner explicitly save it.
    meshSettings = kitsu868::mesh::defaultSettings();
    Serial.printf("KITSU_WARN mesh_settings=%s\n",
                  kitsu868::mesh::storeStatusName(loaded));
  }

  const kitsu868::mesh::Status identityStatus =
      kitsu868::mesh::makeClientIdentity(wisp.uid.c_str(), meshIdentity);
  if (identityStatus != kitsu868::mesh::Status::Ok) {
    meshInitStatus = kitsu868::mesh::TransportStatus::InvalidIdentity;
    radioReady = false;
    radioInitCode = -1;
    return;
  }
  meshInitStatus = meshTransport.begin(meshSettings, meshIdentity);
  if (meshInitStatus == kitsu868::mesh::TransportStatus::Ok &&
      discoveryJournalReady) {
    for (size_t ordinal = 0U;
         ordinal < kitsu868::discovery::kDiscoveryPeerCapacity; ++ordinal) {
      kitsu868::discovery::DiscoveryPeer peer{};
      if (!discoveryJournal.peerAt(ordinal, peer)) break;
      if (peer.type != 1U) continue;
      const char* const name = peer.name[0] != '\0'
                                   ? peer.name
                                   : "MeshCore peer";
      (void)meshTransport.stageObservedContact(
          peer.publicKey, name, peer.type, peer.senderAdvertTimestamp);
    }
  }
  radioReady = meshTransport.active();
  radioInitCode = meshTransport.radioCode();
  Serial.printf(
      "KITSU_MESH_INIT status=%s enabled=%s profile=%s identity=%s\n",
      kitsu868::mesh::transportStatusName(meshInitStatus),
      meshSettings.enabled ? "true" : "false",
      kitsu868::mesh::kUkEuNarrowProfileName,
      meshTransport.identityReady() ? "true" : "false");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  if (kitsu868::connectivity::destructiveNvsEraseBlocked()) {
    Serial.println("KITSU_BLOCKED reason=nvs-destructive-format-prevented");
    while (true) delay(1000U);
  }
  delay(300);
  Serial.printf(
      "\nKITSU_BOOT firmware=%s version=%s board=heltec-v3.2 "
      "tx_enabled=false identity=%s\n",
      FIRMWARE_NAME, FIRMWARE_VERSION, FIRMWARE_IDENTITY);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  rawButton = stableButton = digitalRead(PIN_BUTTON) == LOW;
  pinMode(PIN_BATTERY_CTRL, OUTPUT);
  digitalWrite(PIN_BATTERY_CTRL, HIGH);
  pinMode(PIN_BATTERY_ADC, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_2_5db);

  initDisplay();
  const kitsu868::connectivity::FlashLayoutResult flashLayout =
      kitsu868::connectivity::validateKitsuFlashLayout(flashLayoutPlatform);
  Serial.printf("KITSU_FLASH_LAYOUT status=%s\n",
                kitsu868::connectivity::flashLayoutResultName(flashLayout));
  if (flashLayout != kitsu868::connectivity::FlashLayoutResult::Ready) {
    // Never let a 0.20.3 application mutate the legacy 20 KiB NVS or an
    // incomplete serial migration. Arduino has already mounted NVS before
    // setup(), so the migration workflow separately proves that the exact
    // expanded image initializes without entering Arduino's erase fallback.
    Serial.println("KITSU_BLOCKED reason=flash-layout-mismatch");
    while (true) delay(1000U);
  }
  // Inspect the web flasher's transaction sectors before mandatory legacy
  // retirement. A canonical PREPARED record makes retirement preserve those
  // two fixed sectors while still erasing and verifying the rest of
  // kitsu_conn. No companion state has been loaded or mutated yet.
  kitsu868::CompanionReplacementTransaction replacementTransaction{};
  kitsu868::CompanionReplacementTransactionStorage replacementTransactionStorage;
  const bool replacementTransactionReadable =
      replacementTransactionStorage.beginAndRead(replacementTransaction);
  const bool replacementPreparedValid = replacementTransactionReadable &&
      replacementTransactionStorage.preparedSectorCanonical() &&
      kitsu868::companionReplacementIntentValid(
          replacementTransaction.prepared);
  const bool replacementCommittedValid = replacementPreparedValid &&
      replacementTransactionStorage.committedSectorCanonical() &&
      kitsu868::companionReplacementTransactionValid(replacementTransaction);
  const kitsu868::connectivity::LegacyConnectivityPreservation
      replacementPreservation = replacementCommittedValid
          ? kitsu868::connectivity::
              LegacyConnectivityPreservation::Transaction
          : replacementPreparedValid
              ? kitsu868::connectivity::
                  LegacyConnectivityPreservation::Prepared
              : kitsu868::connectivity::
                  LegacyConnectivityPreservation::None;
  const kitsu868::connectivity::LegacyConnectivityRetirementResult
      legacyRetirement =
          kitsu868::connectivity::KitsuLegacyConnectivityRetirement::run(
              legacyConnectivityRetirementPlatform,
              replacementPreservation);
  legacyConnectivityRetirementReady =
      kitsu868::connectivity::legacyConnectivityRetirementSucceeded(
          legacyRetirement);
  Serial.printf("KITSU_LEGACY_CONNECTIVITY_RETIREMENT status=%s ready=%s\n",
                kitsu868::connectivity::
                    legacyConnectivityRetirementResultName(legacyRetirement),
                legacyConnectivityRetirementReady ? "true" : "false");
  (void)companionBle.preparePairingStorage();
  loadState();
  loadNearbySequenceCursor();
  nearbySessionNonce = esp_random();
  if (nearbySessionNonce == 0U) nearbySessionNonce = 1U;
  initLocalSecurityStorage();
  bleOtaReady = bleOta.begin(bleOtaPlatform, FIRMWARE_VERSION);
  const kitsu868::connectivity::BleOtaStatus otaBootStatus = bleOta.status();
  Serial.printf("KITSU_BLE_OTA ready=%s state=%s error=%s\n",
                bleOtaReady ? "true" : "false",
                kitsu868::connectivity::KitsuBleOta::stateName(
                    otaBootStatus.state),
                otaBootStatus.error ? otaBootStatus.error : "none");
  companionPack.begin();
  bool replacementAttemptedThisBoot = false;
  if (companionPack.valid() && collectiblePackId != companionPack.id()) {
    const uint32_t installedPackId = companionPack.id();
    const bool firstPackAssignment = collectiblePackId == 0U;
    const bool replacementTransactionMatches = !firstPackAssignment &&
        legacyConnectivityRetirementReady &&
        replacementCommittedValid &&
        kitsu868::companionReplacementTransactionAuthorizes(
            replacementTransaction, collectiblePackId, installedPackId,
            companionPack.revision(), companionPack.bytes(),
            companionPack.payloadCrc32(), companionPack.headerCrc32());
    // A valid PREPARED record made the retirement pass preserve both fixed
    // transaction sectors. COMMITTED is separate and exact, so the source ID
    // survives every retry window while only the readback-complete target can
    // authorize destructive state changes.
    const bool replacementAuthorized = replacementTransactionMatches;
    if (replacementAuthorized) {
      replacementAttemptedThisBoot = true;
      // A different species is destructive only when the flasher's one-shot
      // transaction record binds the stored ID to this exact validated pack.
      // Interrupted writes and arbitrary pack mismatches never enter here.
      wisp.energy = 72;
      wisp.curiosity = 14;
      wisp.affection = 5;
      wisp.pets = 0;
      wisp.sleeping = false;
      if (!kitsu868::CompanionBrain::clearStoredState()) {
        Serial.println("KITSU_WARN brain_reset=false");
      }
      collectiblePackId = installedPackId;
      unlockedTraits = 0;
      collectedGifts = 0;
      lastEncounterTrait = 0xff;
      lastEncounterGift = 0xff;
      const bool packIdentityStateSaved = saveState();
      if (packIdentityStateSaved && !replacementTransactionStorage.consume()) {
        // The saved target identity makes a leftover record harmless.  The
        // next mandatory retirement pass will consume it before normal boot.
        Serial.println("KITSU_WARN pack_replacement_intent_consumed=false");
      } else if (!packIdentityStateSaved) {
        // The marker remains durable, so a reboot retries instead of exposing
        // the newly written pack as an unauthorized intermediate starter.
        Serial.println("KITSU_WARN pack_replacement_pending=true");
      }
      Serial.printf(
          "KITSU_PACK_REPLACED source=%08lX target=%08lX authorized=true\n",
          static_cast<unsigned long>(
              replacementTransaction.prepared.sourcePackId),
          static_cast<unsigned long>(installedPackId));
    } else if (firstPackAssignment) {
      // Assigning the first pack ID preserves the legacy care vitals, but it
      // establishes a new pack-scoped brain identity and clears pack-specific
      // traits/gifts. It is not an authorized replacement of a stored species.
      collectiblePackId = installedPackId;
      unlockedTraits = 0;
      collectedGifts = 0;
      lastEncounterTrait = 0xff;
      lastEncounterGift = 0xff;
      saveState();
    } else {
      Serial.printf(
          "KITSU_PACK_BLOCKED stored=%08lX installed=%08lX "
          "reason=replacement-not-authorized\n",
          static_cast<unsigned long>(collectiblePackId),
          static_cast<unsigned long>(installedPackId));
      companionPack.quarantineUnapprovedReplacement();
    }
  }
  if (!replacementAttemptedThisBoot && legacyConnectivityRetirementReady &&
      replacementPreparedValid &&
      replacementTransaction.prepared.sourcePackId != collectiblePackId &&
      !replacementTransactionStorage.consume()) {
    // A completed or stale transaction is no longer a retry source. The
    // authorized path already consumes only after its core-ID readback; on a
    // subsequent boot this branch cleans up a marker left across reset.
    Serial.println("KITSU_WARN pack_replacement_intent_consumed=false");
  }
  const uint32_t brainPackId = collectiblePackId != 0U
      ? collectiblePackId
      : (companionPack.valid() ? companionPack.id() : 0U);
  companionBrain.begin(wisp.uid.c_str(), brainPackId);
  companionBrain.syncSleeping(wisp.sleeping);
  loadCompanionProgression();
  initMesh();
  printChatStorageStatus();
  // Establish the boot-scoped journal identity before BLE starts accepting
  // requests; every v2 page, including an empty first page, requires it.
  chatSession = esp_random();
  if (chatSession == 0U) chatSession = 1U;
  const bool bleReady = legacyConnectivityRetirementReady &&
      companionBle.begin();
  Serial.printf("KITSU_BLE_COMPANION ready=%s service=%s bonds=%d "
                "controllers=%u\n",
                bleReady ? "true" : "false",
                kitsu868::connectivity::kKitsuGattServiceUuid,
                companionBle.bleBondCount(),
                static_cast<unsigned>(deviceSecurity.status().controllerCount));
  sampleBattery(true);
  showHatchSequence();

  delay(300);
  lastEnergyTickAt = lastBrainMinuteAt = millis();
  lastInteractionAt = millis();
  kitsu868::fun::resetSessionChallenges(sessionChallenges);
  sessionAuraActive = false;
  sleepStartedAt = wisp.sleeping ? millis() : 0U;
  scheduleRareReaction(millis());
  cancelAmbientAnimation();
  const bool meetingNewPack = startMeetForNewPack();
  if (!meetingNewPack) startBaseAnimation();
  enterScreen(activityResumePending
                  ? Screen::Activity
                  : companionPack.valid() && wisp.sleeping && !meetingNewPack
                        ? Screen::Sleep
                        : Screen::Pet);
  printSelfTest();
  const bool criticalHealth = legacyConnectivityRetirementReady &&
      connectivitySecurityReady && bleReady;
  const bool otaInitializationAccepted = bleOtaReady &&
      bleOta.finishCriticalInitialization(criticalHealth, millis());
  Serial.printf("KITSU_BLE_OTA_HEALTH critical=%s accepted=%s\n",
                criticalHealth ? "true" : "false",
                otaInitializationAccepted ? "true" : "false");
  if (criticalHealth && otaInitializationAccepted) {
    Serial.printf("KITSU_READY uid=%s companion=\"%s\" pack_valid=%s "
                  "meshcore=1.17.1 profile=UK_EU_NARROW rx_ready=%s "
                  "tx_unlocked=false\n",
                  wisp.uid.c_str(), companionName().c_str(),
                  companionPack.valid() ? "true" : "false",
                  meshTransport.active() ? "true" : "false");
  } else {
    Serial.printf("KITSU_BLOCKED retirement=%s security=%s ble=%s ota=%s\n",
                  legacyConnectivityRetirementReady ? "true" : "false",
                  connectivitySecurityReady ? "true" : "false",
                  bleReady ? "true" : "false",
                  otaInitializationAccepted ? "true" : "false");
  }
}

void loop() {
  backgroundStorageTransactionUsed = false;
  pollButton();
  pollSerial();
  const uint32_t now = millis();
  serviceClockNetworkTime(now);
  companionBle.loop(now);
  serviceControllerRecovery(now);
  bleOta.loop(now, legacyConnectivityRetirementReady &&
                       connectivitySecurityReady && companionBle.ready(),
              companionBle.bleTransmitIdle());
  meshTransport.loop();
  processNearbyRadio();
  tickPetPresence(now);
  processFloodAdvertStatus();
  processMeshAdvert();
  processMeshMessages();
  presentPendingWildEncounter();
  serviceCompanionBleRefresh(now);
  tickCreature();
  tickProgression();
  tickGame();
  tickActivity(now);
  tickFocus(now);
  tickFun(now);
  tickCompanionProgression(now);
  tickExpedition(now);
  tickPartyHotspot(now);
  tickModeParty(now);
  tickAnimation();
  sampleBattery();

  tickDiscoveryJournal(now);
  if (radioListening && static_cast<int32_t>(now - listenUntil) >= 0) {
    stopListeningSafely();
  }
  if ((screen == Screen::Menu || screen == Screen::Inbox ||
       screen == Screen::GameMenu || screen == Screen::Status ||
       screen == Screen::FieldGuide || screen == Screen::Goals ||
       screen == Screen::Clock || screen == Screen::Adventure ||
       (screen == Screen::Activity && !activityRuntimeBusy())) &&
      !rawButton && !stableButton &&
      now - screenEnteredAt >= SCREEN_TIMEOUT_MS) {
    if (screen == Screen::Clock) clockEditor.cancel();
    enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
  }
  tickDisplayPower();
  renderDisplay();
  delay(2);
}
