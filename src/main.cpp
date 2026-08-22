#include <Arduino.h>
#include <cstring>
#include <new>
#include <Preferences.h>
#include <SSD1306Wire.h>
#include <sys/time.h>
#include <time.h>
#include <Wire.h>

#include "companion_brain.h"
#include "companion_pack.h"
#include "encounter_protocol.h"
#include "kitsu_chat_contract.h"
#include "kitsu_ble_action.h"
#include "kitsu_ble_gatt.h"
#include "kitsu_ble_session.h"
#include "kitsu_connectivity_config.h"
#include "kitsu_connectivity_runtime.h"
#include "kitsu_device_security.h"
#include "kitsu_esp32_connectivity.h"
#include "kitsu_esp32_gateway_action.h"
#include "kitsu_esp32_gateway_tls.h"
#include "kitsu_esp32_security.h"
#include "kitsu_gateway_action_runtime.h"
#include "kitsu_gateway_bootstrap.h"
#include "kitsu_gateway_enrollment_flow.h"
#include "kitsu_mobile_relay.h"
#include "kitsu_mesh_config.h"
#include "kitsu_mesh_transport.h"
#include "mesh_discovery_journal.h"
#include "mini_games.h"
#include "portrait_font.h"
#include "portrait_ui_layout.h"

namespace {

constexpr char FIRMWARE_NAME[] = "Kitsu868";
constexpr char FIRMWARE_VERSION[] = "0.11.4";
constexpr uint32_t LEGACY_STATE_MAGIC = 0x57535031;
constexpr uint32_t CORE_STATE_MAGIC = 0x4b433732;  // "KC72"

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
constexpr uint32_t SCREEN_TIMEOUT_MS = 9000;
constexpr uint32_t LISTEN_TIME_MS = 60UL * 1000UL;
constexpr uint32_t ENERGY_TICK_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t BRAIN_MINUTE_MS = 60UL * 1000UL;
constexpr uint32_t BATTERY_SAMPLE_MS = 60UL * 1000UL;
constexpr uint32_t AWAKE_DISPLAY_SLEEP_MS = 2UL * 60UL * 1000UL;
constexpr uint32_t DREAM_DISPLAY_SLEEP_MS = 20UL * 1000UL;
constexpr uint32_t MESH_INTRODUCE_MIN_INTERVAL_MS = 1000UL;
constexpr uint32_t DISCOVERY_JOURNAL_DEBOUNCE_MS = 5000UL;
constexpr uint32_t GATEWAY_SNAPSHOT_INTERVAL_MS = 60UL * 1000UL;
static_assert(kitsu868::mesh::kMeshChannelCapacity == 4U,
              "backend companion snapshot contract requires four slots");

SSD1306Wire display(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);
Preferences preferences;
CompanionPack companionPack;
kitsu868::CompanionBrain companionBrain;
kitsu868::SignalCatchGame signalCatchGame;
kitsu868::PounceFetchGame pounceFetchGame;
kitsu868::mesh::KitsuMeshTransport meshTransport;
kitsu868::mesh::SettingsStore meshSettingsStore;
kitsu868::mesh::Settings meshSettings = kitsu868::mesh::defaultSettings();
kitsu868::mesh::ClientIdentity meshIdentity{};
kitsu868::mesh::CurrentLocationOnce meshCurrentLocation{};
kitsu868::mesh::TransportStatus meshInitStatus =
    kitsu868::mesh::TransportStatus::Disabled;
kitsu868::connectivity::Esp32DeviceSecurityStorage connectivityStorage;
kitsu868::connectivity::Esp32DeviceSecurityPlatform connectivityPlatform;
kitsu868::connectivity::KitsuDeviceSecurity deviceSecurity;
kitsu868::connectivity::Esp32ConnectionSlotStorage connectionSlotStorage;
kitsu868::connectivity::Esp32ConnectionStoreCrypto connectionStoreCrypto;
kitsu868::connectivity::Esp32GatewayTrustValidator gatewayTrustValidator;
kitsu868::connectivity::ConnectionConfigStore connectionConfigStore;
kitsu868::connectivity::Esp32WifiRuntime wifiRuntime;
kitsu868::connectivity::Esp32GatewayLanReplayStorage
    gatewayLanReplayStorage;
kitsu868::connectivity::DurableGatewayLanActionReplayStore
    gatewayLanReplayStore;
kitsu868::connectivity::Esp32GatewayLanTlsTransport gatewayLanTls;
kitsu868::connectivity::KitsuDeviceSecurityLanSequenceStore
    gatewayLanSequences(deviceSecurity);
kitsu868::connectivity::Esp32CompanionCrypto gatewayLanCrypto;
kitsu868::connectivity::KitsuGatewayLanRuntime gatewayLanRuntime;
kitsu868::connectivity::GatewayLanActionDispatcher gatewayLanActions;
kitsu868::connectivity::KitsuMobileRelay mobileRelay;
kitsu868::connectivity::Esp32EnrollmentPlatformCrypto enrollmentCrypto;
kitsu868::connectivity::KitsuEnrollmentRecipient enrollmentRecipient;
kitsu868::connectivity::KitsuGatewayEnrollmentFlow gatewayEnrollmentFlow;
kitsu868::connectivity::Esp32GatewayBootstrapTransport
    gatewayBootstrapTransport;
kitsu868::connectivity::KitsuGatewayBootstrap gatewayBootstrap;
kitsu868::connectivity::GatewayBootstrapWorkspace* gatewayBootstrapWorkspace =
    nullptr;
kitsu868::connectivity::GatewayBootstrapResult gatewayBootstrapLastResult =
    kitsu868::connectivity::GatewayBootstrapResult::NotActive;
kitsu868::connectivity::BleActionReplayCache bleActionReplayCache;
alignas(4) uint8_t bleActionReplayScratch[
    kitsu868::connectivity::kBleActionReplaySerializedBytes]{};
kitsu868::connectivity::Esp32JournalStorage discoveryStorage;
kitsu868::connectivity::Esp32JournalCrypto discoveryCrypto;
kitsu868::discovery::MeshDiscoveryJournal discoveryJournal;
bool connectivitySecurityReady = false;
bool bleActionReplayReady = false;
bool discoveryJournalReady = false;
bool gatewayLanReplayReady = false;
bool gatewayLanActionsReady = false;
bool mobileRelayReady = false;
bool gatewayLanBegun = false;
bool gatewayLanEverConnected = false;
uint32_t gatewayLanConfigGeneration = 0U;
uint32_t gatewayLanNextSnapshotAt = 0U;
uint32_t gatewayLanLastSnapshotHash = 0U;
uint32_t gatewayLanLastObservedSnapshotHash = 0U;
bool gatewaySnapshotDirty = true;
kitsu868::connectivity::GatewayLanRuntimeResult gatewayLanLastResult =
    kitsu868::connectivity::GatewayLanRuntimeResult::NotBegun;

enum class GatewayLanServiceState : uint8_t {
  ConfigUnavailable = 0,
  ConnectivityUnavailable,
  Unconfigured,
  BlePriority,
  WifiPending,
  EnrollmentPending,
  TimePending,
  ReplayUnavailable,
  Reconnecting,
  Connected,
};

GatewayLanServiceState gatewayLanServiceState =
    GatewayLanServiceState::ConfigUnavailable;
GatewayLanServiceState gatewayLanReportedState =
    GatewayLanServiceState::Connected;
kitsu868::connectivity::GatewayLanRuntimeResult gatewayLanReportedResult =
    kitsu868::connectivity::GatewayLanRuntimeResult::Ok;
bool gatewayLanReportInitialized = false;
uint32_t discoveryBootId = 0U;
uint32_t discoveryJournalDirtyAt = 0U;

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
};

enum class ConnectionAction : uint8_t {
  Bluetooth = 0,
  Wifi,
  Gateway,
  Back,
};

enum class ActiveGame : uint8_t { None, SignalCatch, PounceFetch };

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
#pragma pack(pop)

static_assert(sizeof(CoreStateV2) == 37, "core persistence layout changed");

WispState wisp;
Screen screen = Screen::Pet;
const char* const MENU_ITEMS[] = {
    "CONNECT", "FEED", "PLAY", "GAMES", "INBOX", "RADIO", "SLEEP",
    "INFO", "BACK"};
const char* const GAME_ITEMS[] = {"SIGNAL", "POUNCE", "BACK"};
uint8_t menuIndex = 0;
uint8_t gameMenuIndex = 0;
uint8_t statusPage = 0;
ConnectionAction connectionAction = ConnectionAction::Bluetooth;
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

uint32_t screenEnteredAt = 0;
uint32_t listenUntil = 0;
uint32_t nextAmbientAnimationAt = 0;
uint32_t animationToken = 0;
ActiveAnimation activeAnimation;
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
  uint8_t contactPublicKey[32]{};
  uint8_t channelSlot = 0xff;
  char senderName[33]{};
  char text[kitsu868::mesh::kMeshTextCapacity]{};
  float snr = 0.0f;
};

ChatJournalEntry chatJournal[kitsu868::chat::kInboxCapacity]{};
uint8_t chatJournalStart = 0;
uint8_t chatJournalCount = 0;
uint32_t chatJournalDropped = 0;
uint32_t nextChatMessageId = 1;
uint32_t chatSession = 0;
uint8_t unreadChatMessages = 0;
uint8_t inboxSelection = 0;
bool pendingChatReaction = false;

bool rawButton = false;
bool stableButton = false;
uint32_t buttonChangedAt = 0;
uint32_t buttonPressedAt = 0;
String serialLine;
bool serialOverflow = false;
bool serialControlRejected = false;

bool handleCompanionBleRequest(
    const kitsu868::companion::DecodedEnvelope& request,
    const uint8_t* payload, size_t payloadBytes, uint8_t* responsePayload,
    size_t responseCapacity, size_t& responseBytes);
void wipeSensitive(void* memory, size_t bytes);
bool petWisp();
bool feedKitsu();
bool playKitsu();
bool startListening(uint32_t durationMs = LISTEN_TIME_MS);
ChatJournalEntry& appendChatJournal();
bool commitMeshRadioSettings(const kitsu868::mesh::Settings& candidate,
                             const char*& error);
void enterScreen(Screen next);
bool trustedGatewayWallClock(int64_t& epoch);
void stopGatewayLanRuntime();

class FirmwareBleBridge final
    : public kitsu868::connectivity::BleFrameDelegate,
      public kitsu868::connectivity::BleSessionTransport,
      public kitsu868::connectivity::BleOperationDelegate {
 public:
  bool begin() {
    begun_ = false;
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

  bool openPairing(uint32_t now) {
    if (!begun_ || !link_.openPairingWindow(
                       now, kitsu868::connectivity::kBlePairingWindowMaximumMs)) {
      return false;
    }
    session_.setPairingWindow(
        true, kitsu868::connectivity::kBlePairingWindowMaximumMs, now);
    return true;
  }

  void closePairing() {
    if (!begun_) return;
    link_.closePairingWindow();
    session_.setPairingWindow(false, 0U, millis());
    session_.cancelPendingPairing();
  }

  bool confirmNumeric() { return begun_ && link_.confirmNumericComparison(true); }
  bool confirmController(uint32_t now) {
    return begun_ && session_.confirmPendingPairing(now);
  }
  bool sendEnrollmentEvent(
      const kitsu868::connectivity::GatewayEnrollmentReceipt& receipt) {
    uint8_t payload[512]{};
    size_t payloadBytes = 0U;
    const bool encoded =
        kitsu868::connectivity::encodeGatewayEnrollmentEvent(
            receipt, payload, sizeof(payload), payloadBytes);
    const bool sent = encoded && session_.sendEvent(
        "gateway.enroll.event", payload, payloadBytes);
    wipeSensitive(payload, sizeof(payload));
    return sent;
  }
  bool ready() const { return begun_; }
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
      case kitsu868::connectivity::BleLinkEvent::LinkAuthenticated:
        session_.onSecureLinkEstablished(
            status.secureConnections, status.encrypted,
            status.authenticated, status.bonded, millis());
        break;
      case kitsu868::connectivity::BleLinkEvent::Disconnected:
      case kitsu868::connectivity::BleLinkEvent::LinkRejected:
        session_.onLinkClosed(millis());
        gatewayEnrollmentFlow.onBleDisconnected();
        mobileRelay.onBleDisconnected();
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
  void disconnectBle() override { link_.disconnect(); }
  bool handleBleRequest(
      const kitsu868::companion::DecodedEnvelope& request,
      const uint8_t* payload, size_t payloadBytes, uint8_t* responsePayload,
      size_t responseCapacity, size_t& responseBytes) override {
    return handleCompanionBleRequest(request, payload, payloadBytes,
                                     responsePayload, responseCapacity,
                                     responseBytes);
  }

 private:
  kitsu868::connectivity::KitsuBleGattLink link_{};
  kitsu868::connectivity::KitsuBleSession session_{};
  kitsu868::connectivity::Esp32CompanionCrypto crypto_{};
  bool begun_ = false;
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

void wipeSensitive(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

const char* gatewayLanServiceStateName(GatewayLanServiceState state) {
  switch (state) {
    case GatewayLanServiceState::ConfigUnavailable:
      return "config_unavailable";
    case GatewayLanServiceState::ConnectivityUnavailable:
      return "connectivity_unavailable";
    case GatewayLanServiceState::Unconfigured: return "unconfigured";
    case GatewayLanServiceState::BlePriority: return "ble_priority";
    case GatewayLanServiceState::WifiPending: return "wifi_pending";
    case GatewayLanServiceState::EnrollmentPending:
      return "enrollment_pending";
    case GatewayLanServiceState::TimePending: return "time_pending";
    case GatewayLanServiceState::ReplayUnavailable:
      return "replay_unavailable";
    case GatewayLanServiceState::Reconnecting: return "reconnecting";
    case GatewayLanServiceState::Connected: return "connected";
  }
  return "config_unavailable";
}

const char* chatStateName(ChatJournalState state);

namespace companion_api {

struct CursorQuery {
  bool hasAfter = false;
  uint32_t after = 0U;
  uint8_t limit = 0U;
};

// Optional structured result for the LAN adapter. BLE callers continue to
// consume the frozen JSON receipt; both paths still enter applyAction below.
struct ActionExecutionOutcome {
  bool decided = false;
  bool accepted = false;
  const char* state = nullptr;
  const char* errorCode = nullptr;
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

bool parseCanonicalUuid(const uint8_t* input, size_t bytes,
                        uint8_t output[
                            kitsu868::connectivity::kEnrollmentUuidBytes]) {
  if (!output) return false;
  memset(output, 0,
         kitsu868::connectivity::kEnrollmentUuidBytes);
  if (!input || bytes != 36U) return false;
  size_t written = 0U;
  int high = -1;
  for (size_t i = 0U; i < bytes; ++i) {
    const bool separator = i == 8U || i == 13U || i == 18U || i == 23U;
    if (separator) {
      if (input[i] != '-') return false;
      continue;
    }
    const uint8_t c = input[i];
    int nibble = -1;
    if (c >= '0' && c <= '9') {
      nibble = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      nibble = c - 'a' + 10;
    }
    if (nibble < 0) return false;
    if (high < 0) {
      high = nibble;
    } else {
      if (written >= kitsu868::connectivity::kEnrollmentUuidBytes) {
        return false;
      }
      output[written++] = static_cast<uint8_t>((high << 4U) | nibble);
      high = -1;
    }
  }
  uint8_t aggregate = 0U;
  for (size_t i = 0U;
       i < kitsu868::connectivity::kEnrollmentUuidBytes; ++i) {
    aggregate |= output[i];
  }
  return written == kitsu868::connectivity::kEnrollmentUuidBytes &&
      high < 0 && aggregate != 0U;
}

bool parseGatewayForget(
    const uint8_t* input, size_t bytes,
    uint8_t gatewayId[kitsu868::connectivity::kEnrollmentUuidBytes]) {
  if (!gatewayId) return false;
  memset(gatewayId, 0,
         kitsu868::connectivity::kEnrollmentUuidBytes);
  if (!input || !kitsu868::companion::validUtf8(input, bytes)) return false;
  size_t cursor = 0U;
  const uint8_t* key = nullptr;
  size_t keyBytes = 0U;
  const uint8_t* value = nullptr;
  size_t valueBytes = 0U;
  if (!consume(input, bytes, cursor, '{') ||
      !parseAsciiString(input, bytes, cursor, key, keyBytes) ||
      !sameToken(key, keyBytes, "gateway_id") ||
      !consume(input, bytes, cursor, ':') ||
      !parseAsciiString(input, bytes, cursor, value, valueBytes) ||
      !consume(input, bytes, cursor, '}')) {
    return false;
  }
  skipWhitespace(input, bytes, cursor);
  return cursor == bytes &&
      parseCanonicalUuid(value, valueBytes, gatewayId);
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

const char* bleMeshErrorName(kitsu868::mesh::TransportStatus status) {
  using kitsu868::mesh::TransportStatus;
  switch (status) {
    case TransportStatus::Disabled: return "mesh_disabled";
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

bool rejectAction(const kitsu868::connectivity::BleActionCommand& command,
                  const char* errorCode, uint8_t* output, size_t capacity,
                  size_t& outputBytes) {
  return kitsu868::connectivity::encodeBleActionReceipt(
      command, false, "rejected", errorCode, output, capacity, outputBytes);
}

bool applyAction(const uint8_t* payload, size_t payloadBytes,
                 uint8_t* output, size_t capacity, size_t& outputBytes,
                 ActionExecutionOutcome* execution = nullptr) {
  using kitsu868::connectivity::BleActionCommand;
  using kitsu868::connectivity::BleActionDecodeResult;
  using kitsu868::connectivity::BleActionKind;
  using kitsu868::connectivity::BleActionReplayDecision;

  BleActionCommand command{};
  if (execution) *execution = ActionExecutionOutcome{};
  const auto rejected = [&](const char* errorCode) -> bool {
    if (execution) {
      execution->decided = true;
      execution->accepted = false;
      execution->state = "rejected";
      execution->errorCode = errorCode;
    }
    return rejectAction(command, errorCode, output, capacity, outputBytes);
  };
  const auto accepted = [&](const char* state) -> bool {
    if (execution) {
      execution->decided = true;
      execution->accepted = true;
      execution->state = state;
      execution->errorCode = nullptr;
    }
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
    if (execution) {
      execution->decided = true;
      execution->accepted = false;
      execution->state = "rejected";
      execution->errorCode = error;
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
    return accepted(command.kind == BleActionKind::SendMessage
                        ? "queued" : "applied");
  }
  if (replay == BleActionReplayDecision::DuplicateIndeterminate) {
    return rejected("action_result_unknown");
  }

  // Persist the idempotency reservation before invoking a side effect. Keep
  // the previous blob in global scratch rather than placing multi-kilobyte
  // cache copies on the 8 KiB Arduino loop task stack.
  if (!snapshotActionReplay()) {
    return rejected("idempotency_unavailable");
  }
  if (!bleActionReplayCache.remember(command, actionNow) ||
      !persistActionReplay()) {
    if (!restoreActionReplaySnapshot()) bleActionReplayReady = false;
    Serial.println("KITSU_WARN ble_action_replay=flush_failed");
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
    case BleActionKind::AdvertiseOnce:
    case BleActionKind::ShareLocationOnce:
      break;
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
  return accepted(command.kind == BleActionKind::SendMessage
                      ? "queued" : "applied");
}

bool buildState(const uint8_t* payload, size_t payloadBytes, String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  const kitsu868::discovery::JournalStatus journal = discoveryJournal.status();
  const kitsu868::connectivity::DeviceSecurityStatus security =
      deviceSecurity.status();
  const kitsu868::connectivity::ConnectionConfigStatus connectivity =
      connectionConfigStore.status();
  const kitsu868::connectivity::ConnectivityRuntimeStatus lan =
      wifiRuntime.status(millis());
  const kitsu868::connectivity::GatewayLanRuntimeStatus gateway =
      gatewayLanRuntime.status();
  const kitsu868::connectivity::GatewayLanActionReplayStatus replay =
      gatewayLanReplayStore.status();
  const kitsu868::connectivity::GatewayEnrollmentFlowStatus enrollment =
      gatewayEnrollmentFlow.status(millis());
  const bool durablyEnrolled = connectivity.gatewayEnrolled;
  output.reserve(1120U);
  output = "{\"schema\":\"kitsu.state.v1\",\"device_uid\":\"";
  output += wisp.uid;
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
  output += ",\"mesh_rx_ready\":";
  output += meshTransport.active() ? "true" : "false";
  output += ",\"mesh_enabled\":";
  output += meshSettings.enabled ? "true" : "false";
  output += ",\"mesh_time_valid\":";
  output += meshTransport.timeValid() ? "true" : "false";
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
  output += ",\"remote_connectivity_allowed\":";
  output += deviceSecurity.remoteConnectivityAllowed() ? "true" : "false";
  output += ",\"wifi_configured\":";
  output += connectivity.wifiConfigured ? "true" : "false";
  output += ",\"wifi_state\":\"";
  output += kitsu868::connectivity::wifiRuntimeStateName(lan.wifiState);
  output += "\",\"gateway_configured\":";
  output += connectivity.gatewayConfigured ? "true" : "false";
  output += ",\"gateway_enrolled\":";
  output += connectivity.gatewayEnrolled ? "true" : "false";
  output += ",\"gateway_enrollment_state\":\"";
  output += durablyEnrolled
      ? "enrolled"
      : kitsu868::connectivity::gatewayEnrollmentFlowStateName(
            enrollment.state);
  output += "\",\"gateway_enrollment_error\":";
  if (durablyEnrolled ||
      enrollment.lastError ==
          kitsu868::connectivity::GatewayEnrollmentError::None) {
    output += "null";
  } else {
    output += '"';
    output += kitsu868::connectivity::gatewayEnrollmentErrorName(
        enrollment.lastError);
    output += '"';
  }
  output += ",\"gateway_enrollment_expires_in_ms\":";
  output += String(enrollment.expiresInMs);
  output += ",\"lan_state\":\"";
  // This legacy field is consumed by the native clients, so expose the
  // authoritative steady gateway service instead of the Wi-Fi policy
  // machine's pre-TLS placeholder. Keep gateway_lan_state as an explicit
  // alias for newer clients and diagnostics.
  output += gatewayLanServiceStateName(gatewayLanServiceState);
  output += "\",\"gateway_lan_state\":\"";
  output += gatewayLanServiceStateName(gatewayLanServiceState);
  output += "\",\"gateway_lan_last_result\":\"";
  output += kitsu868::connectivity::gatewayLanRuntimeResultName(
      gatewayLanLastResult);
  output += "\",\"gateway_lan_connected\":";
  output += gateway.connected ? "true" : "false";
  output += ",\"gateway_lan_ever_connected\":";
  output += gatewayLanEverConnected ? "true" : "false";
  output += ",\"gateway_lan_queue_depth\":";
  output += String(gateway.queuedFrames);
  output += ",\"gateway_lan_failures\":";
  output += String(gateway.consecutiveFailures);
  output += ",\"gateway_lan_replay_ready\":";
  output += gatewayLanReplayReady ? "true" : "false";
  output += ",\"gateway_lan_replay_records\":";
  output += String(replay.records);
  output += '}';
  return true;
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

bool syncClock(const uint8_t* payload, size_t payloadBytes, String& output) {
  uint32_t epoch = 0U;
  if (!parseClockSync(payload, payloadBytes, epoch)) return false;
  const kitsu868::mesh::TransportStatus status =
      meshTransport.setEpoch(epoch);
  if (status != kitsu868::mesh::TransportStatus::Ok) {
    output = "{\"ok\":false,\"error\":\"";
    output += kitsu868::mesh::transportStatusName(status);
    output += "\"}";
    return true;
  }
  const timeval wallClock{static_cast<time_t>(epoch), 0};
  if (settimeofday(&wallClock, nullptr) != 0) {
    output = "{\"ok\":false,\"error\":\"system_clock_failed\"}";
    return true;
  }
  if (!wifiRuntime.noteAuthenticatedTime(epoch)) {
    output = "{\"ok\":false,\"error\":\"clock_provenance_failed\"}";
    return true;
  }
  output = "{\"schema\":\"kitsu.clock.v1\",\"epoch\":";
  output += String(meshTransport.currentEpoch());
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

void appendConfigurationReceipt(
    String& output, const char* schema,
    kitsu868::connectivity::ConfigResult result) {
  output = "{\"schema\":\"";
  output += schema;
  output += "\",\"accepted\":";
  output += result == kitsu868::connectivity::ConfigResult::Ok
                ? "true"
                : "false";
  output += ",\"state\":\"";
  output += result == kitsu868::connectivity::ConfigResult::Ok
                ? "stored"
                : "rejected";
  output += "\",\"error_code\":";
  if (result == kitsu868::connectivity::ConfigResult::Ok) {
    output += "null";
  } else {
    output += '"';
    output += kitsu868::connectivity::configResultName(result);
    output += '"';
  }
  output += '}';
}

bool configureWifi(const uint8_t* payload, size_t payloadBytes,
                   String& output) {
  kitsu868::connectivity::WifiConfig config{};
  kitsu868::connectivity::ConfigResult result =
      kitsu868::connectivity::decodeWifiConfig(payload, payloadBytes,
                                                config);
  if (result == kitsu868::connectivity::ConfigResult::Ok) {
    result = connectionConfigStore.commitWifi(config);
    if (result == kitsu868::connectivity::ConfigResult::Ok) {
      wifiRuntime.requestCredentialReload();
      gatewaySnapshotDirty = true;
    }
  }
  // The signed receipt intentionally contains no SSID or passphrase.
  wipeSensitive(&config, sizeof(config));
  appendConfigurationReceipt(output, "kitsu.wifi-config.v1", result);
  return true;
}

bool retryWifi(const uint8_t* payload, size_t payloadBytes,
               String& output) {
  if (!emptyObject(payload, payloadBytes)) return false;
  const kitsu868::connectivity::ConnectionConfigStatus config =
      connectionConfigStore.status();
  const char* error = nullptr;
  if (!config.begun) {
    error = "storage_unavailable";
  } else if (!config.wifiConfigured) {
    error = "wifi_unconfigured";
  } else if (!deviceSecurity.remoteConnectivityAllowed()) {
    error = "connectivity_unavailable";
  } else {
    stopGatewayLanRuntime();
    wifiRuntime.requestCredentialReload();
    gatewaySnapshotDirty = true;
  }
  output = "{\"schema\":\"kitsu.wifi-retry.v1\",\"accepted\":";
  output += error ? "false" : "true";
  output += ",\"state\":\"";
  output += error ? "rejected" : "retrying";
  output += "\",\"error_code\":";
  if (error) {
    output += '"';
    output += error;
    output += '"';
  } else {
    output += "null";
  }
  output += '}';
  return true;
}

bool configureGateway(const uint8_t* payload, size_t payloadBytes,
                       String& output) {
  auto* config = new (std::nothrow) kitsu868::connectivity::GatewayConfig{};
  kitsu868::connectivity::ConfigResult result =
      kitsu868::connectivity::ConfigResult::StorageAllocationFailed;
  if (config) {
    result = kitsu868::connectivity::decodeGatewayConfig(
        payload, payloadBytes, gatewayTrustValidator, *config);
    if (result == kitsu868::connectivity::ConfigResult::Ok) {
      result = connectionConfigStore.commitGateway(*config);
      if (result == kitsu868::connectivity::ConfigResult::Ok) {
        gatewayEnrollmentFlow.abort();
        gatewaySnapshotDirty = true;
      }
    }
    wipeSensitive(config, sizeof(*config));
    delete config;
  }
  // Trust material, endpoint details, and pins are never echoed or logged.
  appendConfigurationReceipt(output, "kitsu.gateway-config.v2", result);
  return true;
}

void appendGatewayForgetReceipt(String& output, bool accepted,
                                const char* error) {
  output =
      "{\"schema\":\"kitsu.gateway-forget.v1\",\"accepted\":";
  output += accepted ? "true" : "false";
  output += ",\"state\":\"";
  output += accepted ? "forgotten" : "rejected";
  output += "\",\"error_code\":";
  if (accepted) {
    output += "null";
  } else {
    output += '"';
    output += error ? error : "storage_unavailable";
    output += '"';
  }
  output += '}';
}

bool forgetGateway(const uint8_t* payload, size_t payloadBytes,
                   String& output) {
  uint8_t expectedGatewayId[
      kitsu868::connectivity::kEnrollmentUuidBytes]{};
  if (!parseGatewayForget(payload, payloadBytes, expectedGatewayId)) {
    appendGatewayForgetReceipt(output, false, "invalid_request");
    wipeSensitive(expectedGatewayId, sizeof(expectedGatewayId));
    return true;
  }

  const kitsu868::connectivity::ConfigResult result =
      connectionConfigStore.forgetMobileRelayGateway(expectedGatewayId);
  wipeSensitive(expectedGatewayId, sizeof(expectedGatewayId));
  if (result != kitsu868::connectivity::ConfigResult::Ok) {
    const char* error =
        result == kitsu868::connectivity::ConfigResult::InvalidArgument
            ? "gateway_mode_unsupported"
            : result ==
                      kitsu868::connectivity::ConfigResult::InvalidGatewayId
                  ? "gateway_mismatch"
                  : kitsu868::connectivity::configResultName(result);
    appendGatewayForgetReceipt(output, false, error);
    return true;
  }

  // Commit the durable clear before resetting volatile coordinators. No pack,
  // companion brain, controller root, BLE bond, or Wi-Fi field is touched.
  gatewayEnrollmentFlow.abort();
  gatewayBootstrap.cancel();
  if (gatewayBootstrapWorkspace) {
    wipeSensitive(gatewayBootstrapWorkspace,
                  sizeof(*gatewayBootstrapWorkspace));
    delete gatewayBootstrapWorkspace;
    gatewayBootstrapWorkspace = nullptr;
  }
  gatewayBootstrapTransport.close();
  gatewayBootstrapLastResult =
      kitsu868::connectivity::GatewayBootstrapResult::NotActive;
  stopGatewayLanRuntime();
  mobileRelay.clearGatewayState();
  gatewayLanNextSnapshotAt = 0U;
  gatewayLanLastSnapshotHash = 0U;
  gatewayLanLastObservedSnapshotHash = 0U;
  gatewaySnapshotDirty = true;
  appendGatewayForgetReceipt(output, true, nullptr);
  return true;
}

kitsu868::connectivity::GatewayEnrollmentGuards gatewayEnrollmentGuards(
    uint32_t now) {
  const kitsu868::connectivity::ConnectionConfigStatus config =
      connectionConfigStore.status();
  int64_t epoch = 0;
  kitsu868::connectivity::GatewayEnrollmentGuards guards{};
  guards.authenticatedController =
      companionBle.sessionStatus(now).applicationAuthenticated;
  guards.storageReady = connectionConfigStore.ready();
  guards.gatewayConfigured = config.gatewayConfigured;
  guards.alreadyEnrolled = config.gatewayEnrolled;
  guards.trustedClock = trustedGatewayWallClock(epoch);
  guards.remoteConnectivityAllowed =
      deviceSecurity.remoteConnectivityAllowed();
  return guards;
}

bool beginGatewayEnrollment(const uint8_t* payload, size_t payloadBytes,
                            uint8_t* output, size_t outputCapacity,
                            size_t& outputBytes) {
  const uint32_t now = millis();
  kitsu868::connectivity::GatewayEnrollmentReceipt receipt{};
  gatewayEnrollmentFlow.beginOperation(
      payload, payloadBytes, gatewayEnrollmentGuards(now), now, receipt);
  if (receipt.accepted &&
      receipt.state == kitsu868::connectivity::
          GatewayEnrollmentFlowState::PhysicalConfirmationRequired) {
    enterScreen(Screen::PairPhone);
  }
  return kitsu868::connectivity::encodeGatewayEnrollmentReceipt(
      receipt, output, outputCapacity, outputBytes);
}

bool finishGatewayEnrollment(const uint8_t* payload, size_t payloadBytes,
                             uint8_t* output, size_t outputCapacity,
                             size_t& outputBytes) {
  const uint32_t now = millis();
  kitsu868::connectivity::GatewayEnrollmentGuards guards =
      gatewayEnrollmentGuards(now);
  uint8_t gatewayId[kitsu868::connectivity::kEnrollmentUuidBytes]{};
  const bool copiedGateway = connectionConfigStore.copyGatewayId(gatewayId);
  guards.storageReady = guards.storageReady && copiedGateway;
  kitsu868::connectivity::GatewayEnrollmentReceipt receipt{};
  gatewayEnrollmentFlow.finishOperation(
      payload, payloadBytes, guards, gatewayId, wisp.uid.c_str(),
      wisp.uid.length(), now, gatewayLanCrypto, enrollmentCrypto,
      enrollmentRecipient, receipt);
  wipeSensitive(gatewayId, sizeof(gatewayId));
  return kitsu868::connectivity::encodeGatewayEnrollmentReceipt(
      receipt, output, outputCapacity, outputBytes);
}

bool buildMessages(const uint8_t* payload, size_t payloadBytes,
                    String& output) {
  CursorQuery query{};
  if (!parseCursorQuery(payload, payloadBytes, query)) return false;
  output.reserve(6000U);
  output = "{\"schema\":\"kitsu.messages.v1\",\"items\":[";
  uint32_t cursor = query.after;
  bool hasCursor = query.hasAfter;
  uint8_t count = 0U;
  bool hasMore = false;
  for (uint8_t ordinal = 0U; ordinal < chatJournalCount; ++ordinal) {
    const uint8_t index = static_cast<uint8_t>(
        (chatJournalStart + ordinal) % kitsu868::chat::kInboxCapacity);
    const ChatJournalEntry& entry = chatJournal[index];
    if (query.hasAfter && !sequenceAfter(entry.id, query.after)) continue;
    if (count >= query.limit) {
      hasMore = true;
      break;
    }
    if (count != 0U) output += ',';
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
  output += chatJournalDropped != 0U ? "true" : "false";
  output += '}';
  return true;
}

}  // namespace companion_api

class FirmwareMobileRelayEnrollment final
    : public kitsu868::connectivity::MobileRelayEnrollmentDelegate {
 public:
  bool buildMobileRelayEnrollmentRequest(
      uint8_t* output, size_t outputCapacity,
      size_t& outputBytes) override {
    return enrollmentRecipient.buildRequestJson(
               output, outputCapacity, outputBytes) ==
        kitsu868::connectivity::EnrollmentResult::Ok;
  }

  kitsu868::connectivity::MobileRelayResult
  installMobileRelayEnrollmentResponse(
      const uint8_t* response, size_t responseBytes) override {
    using kitsu868::connectivity::ConfigResult;
    using kitsu868::connectivity::EnrollmentResult;
    using kitsu868::connectivity::GatewayBootstrapResult;
    using kitsu868::connectivity::MobileRelayResult;

    const uint32_t now = millis();
    const kitsu868::connectivity::GatewayEnrollmentFlowStatus attempt =
        gatewayEnrollmentFlow.status(now);
    if (!response || responseBytes == 0U || !attempt.hasEnrollmentId ||
        attempt.state != kitsu868::connectivity::
                             GatewayEnrollmentFlowState::ReadyForWifi ||
        !enrollmentRecipient.active() ||
        !gatewayEnrollmentFlow.markBootstrapping(now)) {
      return MobileRelayResult::EnrollmentUnavailable;
    }

    uint8_t gatewayId[kitsu868::connectivity::kEnrollmentUuidBytes]{};
    auto* workspace = new (std::nothrow)
        kitsu868::connectivity::GatewayBootstrapWorkspace{};
    MobileRelayResult result = workspace
        ? MobileRelayResult::EnrollmentUnavailable
        : MobileRelayResult::OutOfMemory;
    if (workspace && connectionConfigStore.copyGatewayId(gatewayId)) {
      kitsu868::connectivity::EnrollmentResponse decoded{};
      const GatewayBootstrapResult decodedResult =
          kitsu868::connectivity::decodeBackendEnrollmentResponse(
              response, responseBytes, attempt.enrollmentId, gatewayId,
              *workspace, decoded);
      if (decodedResult != GatewayBootstrapResult::ReconnectSteady) {
        result = decodedResult == GatewayBootstrapResult::BackendMalformed
            ? MobileRelayResult::EnrollmentBackendMalformed
            : MobileRelayResult::EnrollmentUnavailable;
      } else {
        const size_t maximumCompactBytes = sizeof(*workspace);
        size_t compactBytes = decoded.certificateBytes;
        bool compactValid = decoded.certificateDer && compactBytes != 0U &&
            compactBytes <=
                kitsu868::connectivity::kEnrollmentMaximumCertificateBytes &&
            decoded.certificateChainCount != 0U &&
            decoded.certificateChainCount <= kitsu868::connectivity::
                kEnrollmentMaximumChainCertificates;
        for (size_t i = 0U;
             compactValid && i < decoded.certificateChainCount; ++i) {
          const size_t chainBytes = decoded.certificateChainBytes[i];
          if (!decoded.certificateChainDer[i] || chainBytes == 0U ||
              chainBytes > kitsu868::connectivity::
                  kEnrollmentMaximumCertificateBytes ||
              compactBytes > maximumCompactBytes ||
              chainBytes > maximumCompactBytes - compactBytes) {
            compactValid = false;
          } else {
            compactBytes += chainBytes;
          }
        }
        uint8_t* compact = compactValid
            ? new (std::nothrow) uint8_t[compactBytes]
            : nullptr;
        if (!compactValid) {
          result = MobileRelayResult::EnrollmentBackendMalformed;
        } else if (!compact) {
          result = MobileRelayResult::OutOfMemory;
        } else {
          uint8_t* cursor = compact;
          memcpy(cursor, decoded.certificateDer, decoded.certificateBytes);
          decoded.certificateDer = cursor;
          cursor += decoded.certificateBytes;
          for (size_t i = 0U; i < decoded.certificateChainCount; ++i) {
            memcpy(cursor, decoded.certificateChainDer[i],
                   decoded.certificateChainBytes[i]);
            decoded.certificateChainDer[i] = cursor;
            cursor += decoded.certificateChainBytes[i];
          }

          // Only the exact certificate material must survive through commit.
          // Drop the fixed 20 KiB decoder workspace before the store allocates
          // its authenticated snapshot buffers on this no-PSRAM target.
          wipeSensitive(workspace, sizeof(*workspace));
          delete workspace;
          workspace = nullptr;

          const EnrollmentResult finished =
              enrollmentRecipient.finish(decoded, connectionConfigStore);
          if (finished == EnrollmentResult::Ok) {
            result = connectionConfigStore.status().gatewayEnrolled
                ? MobileRelayResult::Ok
                : MobileRelayResult::EnrollmentCommitFailed;
          } else if (finished == EnrollmentResult::ResponseMismatch) {
            result = MobileRelayResult::EnrollmentResponseMismatch;
          } else if (finished == EnrollmentResult::InvalidCertificate) {
            result = MobileRelayResult::EnrollmentInvalidCertificate;
          } else if (finished == EnrollmentResult::DecryptionFailed) {
            result = MobileRelayResult::EnrollmentDecryptionFailed;
          } else if (finished == EnrollmentResult::CommitFailed) {
            switch (connectionConfigStore.status().lastResult) {
              case ConfigResult::EnrollmentInvalid:
                result = MobileRelayResult::EnrollmentInvalid;
                break;
              case ConfigResult::EnrollmentTrustFailed:
                result = MobileRelayResult::EnrollmentTrustFailed;
                break;
              case ConfigResult::StorageAllocationFailed:
                result = MobileRelayResult::StorageAllocationFailed;
                break;
              case ConfigResult::StorageReadFailed:
                result = MobileRelayResult::StorageReadFailed;
                break;
              case ConfigResult::StorageWriteFailed:
                result = MobileRelayResult::StorageWriteFailed;
                break;
              case ConfigResult::StorageReadbackFailed:
                result = MobileRelayResult::StorageReadbackFailed;
                break;
              case ConfigResult::StorageCorrupt:
                result = MobileRelayResult::StorageCorrupt;
                break;
              case ConfigResult::CryptoFailed:
                result = MobileRelayResult::CryptoFailed;
                break;
              default:
                result = MobileRelayResult::EnrollmentCommitFailed;
                break;
            }
          } else {
            result = finished == EnrollmentResult::CryptoFailed
                ? MobileRelayResult::CryptoFailed
                : MobileRelayResult::EnrollmentUnavailable;
          }
          wipeSensitive(compact, compactBytes);
          delete[] compact;
        }
      }
      wipeSensitive(&decoded, sizeof(decoded));
    }
    const bool installed = result == MobileRelayResult::Ok;
    gatewayEnrollmentFlow.completeBootstrap(
        installed,
        installed ? kitsu868::connectivity::GatewayEnrollmentError::None
                  : kitsu868::connectivity::
                        GatewayEnrollmentError::BootstrapFailed);
    wipeSensitive(gatewayId, sizeof(gatewayId));
    if (workspace) {
      wipeSensitive(workspace, sizeof(*workspace));
      delete workspace;
    }
    if (installed) gatewaySnapshotDirty = true;
    return result;
  }
};

FirmwareMobileRelayEnrollment mobileRelayEnrollment;

bool handleMobileRelayExchange(
    const uint8_t* payload, size_t payloadBytes, uint8_t* responsePayload,
    size_t responseCapacity, size_t& responseBytes) {
  const uint32_t now = millis();
  const kitsu868::connectivity::GatewayEnrollmentFlowStatus enrollment =
      gatewayEnrollmentFlow.status(now);
  kitsu868::connectivity::MobileRelayGuards guards{};
  guards.authenticatedController =
      companionBle.sessionStatus(now).applicationAuthenticated;
  guards.enrollmentPrgConfirmed =
      enrollment.state == kitsu868::connectivity::
                              GatewayEnrollmentFlowState::ReadyForWifi;
  guards.enrollmentActive = enrollmentRecipient.active();
  int64_t epoch = 0;
  const bool clockValid = trustedGatewayWallClock(epoch);
  kitsu868::connectivity::MobileRelayExchangeOutcome outcome{};
  const bool encoded = mobileRelay.handleExchange(
      payload, payloadBytes, guards, epoch, clockValid, responsePayload,
      responseCapacity, responseBytes, &outcome);
  if (outcome.gatewayConfigurationChanged) {
    gatewayEnrollmentFlow.abort();
    gatewayBootstrap.cancel();
    stopGatewayLanRuntime();
    gatewaySnapshotDirty = true;
  }
  // A gateway ACK only retires the exact pending uplink. Treating every
  // successful downlink as new snapshot truth creates an ACK -> snapshot
  // feedback loop that bypasses GATEWAY_SNAPSHOT_INTERVAL_MS.
  if (outcome.enrollmentCompleted) {
    gatewaySnapshotDirty = true;
  }
  return encoded;
}

class FirmwareGatewayLanPayloadQueue final
    : public kitsu868::connectivity::GatewayLanDevicePayloadQueue {
 public:
  bool canEnqueue(size_t payloadCount,
                  size_t worstCasePayloadBytes) const override {
    const uint32_t now = millis();
    const kitsu868::connectivity::ConnectionConfigStatus configured =
        connectionConfigStore.status();
    const bool relayActive = mobileRelayReady &&
        companionBle.sessionStatus(now).applicationAuthenticated &&
        configured.mobileRelayConfigured && configured.gatewayEnrolled;
    if (relayActive) {
      return mobileRelay.canEnqueueDevicePayloads(
          payloadCount, worstCasePayloadBytes);
    }
    if (!gatewayLanBegun || payloadCount == 0U ||
        payloadCount > kitsu868::connectivity::kGatewayLanQueueDepth) {
      return false;
    }
    const kitsu868::connectivity::GatewayLanRuntimeStatus status =
        gatewayLanRuntime.status();
    return status.begun &&
        status.queuedFrames + payloadCount <=
            kitsu868::connectivity::kGatewayLanQueueDepth &&
        status.queuedBytes <=
            kitsu868::connectivity::kGatewayLanMaximumQueuedBytes &&
        worstCasePayloadBytes <=
            kitsu868::connectivity::kGatewayLanMaximumQueuedBytes -
                status.queuedBytes;
  }

  bool enqueue(const char* payloadType, const uint8_t* payload,
               size_t payloadBytes, int64_t issuedEpoch) override {
    const uint32_t now = millis();
    const kitsu868::connectivity::ConnectionConfigStatus configured =
        connectionConfigStore.status();
    const bool relayActive = mobileRelayReady &&
        companionBle.sessionStatus(now).applicationAuthenticated &&
        configured.mobileRelayConfigured && configured.gatewayEnrolled;
    if (relayActive) {
      return mobileRelay.enqueueDevicePayload(
                 payloadType, payload, payloadBytes, issuedEpoch) ==
          kitsu868::connectivity::MobileRelayResult::Ok;
    }
    if (!gatewayLanBegun) return false;
    const kitsu868::connectivity::GatewayLanRuntimeResult result =
        gatewayLanRuntime.enqueueDevicePayload(
            payloadType, payload, payloadBytes, issuedEpoch);
    gatewayLanLastResult = result;
    return result == kitsu868::connectivity::GatewayLanRuntimeResult::Ok;
  }
};

class FirmwareGatewayLanActionExecutor final
    : public kitsu868::connectivity::GatewayLanDirectActionExecutor {
 public:
  bool executeDirectAction(
      const uint8_t* actionRequest, size_t actionRequestBytes,
      kitsu868::connectivity::GatewayLanDirectActionOutcome& outcome)
      override {
    outcome = kitsu868::connectivity::GatewayLanDirectActionOutcome{};
    uint8_t receipt[512]{};
    size_t receiptBytes = 0U;
    companion_api::ActionExecutionOutcome execution{};
    const bool encoded = companion_api::applyAction(
        actionRequest, actionRequestBytes, receipt, sizeof(receipt),
        receiptBytes, &execution);
    wipeSensitive(receipt, sizeof(receipt));
    if (!encoded || receiptBytes == 0U || !execution.decided ||
        !execution.state) {
      return false;
    }
    const char* code = execution.accepted
                           ? execution.state
                           : execution.errorCode;
    if (!code) return false;
    const size_t codeBytes = strlen(code);
    if (codeBytes == 0U ||
        codeBytes >
            kitsu868::connectivity::kGatewayLanActionResultCodeBytes) {
      return false;
    }
    outcome.status = execution.accepted
        ? kitsu868::connectivity::GatewayLanActionOutcomeStatus::Succeeded
        : kitsu868::connectivity::GatewayLanActionOutcomeStatus::Rejected;
    memcpy(outcome.code, code, codeBytes);
    outcome.code[codeBytes] = '\0';
    return true;
  }
};

FirmwareGatewayLanPayloadQueue gatewayLanPayloadQueue;
FirmwareGatewayLanActionExecutor gatewayLanActionExecutor;

bool handleCompanionBleRequest(
    const kitsu868::companion::DecodedEnvelope& request,
    const uint8_t* payload, size_t payloadBytes, uint8_t* responsePayload,
    size_t responseCapacity, size_t& responseBytes) {
  if (strcmp(request.operation, "action.apply") == 0) {
    return companion_api::applyAction(payload, payloadBytes, responsePayload,
                                      responseCapacity, responseBytes);
  }
  if (strcmp(request.operation, "gateway.enroll.begin") == 0) {
    return companion_api::beginGatewayEnrollment(
        payload, payloadBytes, responsePayload, responseCapacity,
        responseBytes);
  }
  if (strcmp(request.operation, "gateway.enroll.finish") == 0) {
    return companion_api::finishGatewayEnrollment(
        payload, payloadBytes, responsePayload, responseCapacity,
        responseBytes);
  }
  if (strcmp(request.operation, "mobile.relay.exchange") == 0) {
    return handleMobileRelayExchange(
        payload, payloadBytes, responsePayload, responseCapacity,
        responseBytes);
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
    handled = companion_api::buildMessages(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "channels.get") == 0) {
    handled = companion_api::buildChannels(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "clock.sync") == 0) {
    handled = companion_api::syncClock(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "mesh.configure") == 0) {
    handled = companion_api::configureMesh(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "wifi.configure") == 0) {
    handled = companion_api::configureWifi(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "wifi.retry") == 0) {
    handled = companion_api::retryWifi(payload, payloadBytes, response);
  } else if (strcmp(request.operation, "gateway.configure") == 0) {
    handled = companion_api::configureGateway(payload, payloadBytes,
                                                response);
  } else if (strcmp(request.operation, "gateway.forget") == 0) {
    handled = companion_api::forgetGateway(payload, payloadBytes, response);
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

uint32_t allocateChatMessageId() {
  uint32_t id = nextChatMessageId++;
  if (id == 0U) {
    id = 1U;
    nextChatMessageId = 2U;
  }
  return id;
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
  return chatJournal[writeIndex];
}

void markChatJournalRead() {
  for (uint8_t ordinal = 0; ordinal < chatJournalCount; ++ordinal) {
    const uint8_t index = static_cast<uint8_t>(
        (chatJournalStart + ordinal) % kitsu868::chat::kInboxCapacity);
    chatJournal[index].unread = false;
  }
  unreadChatMessages = 0;
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

uint32_t coreStateCrc(const CoreStateV2& state) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
  uint32_t crc = 0xffffffffUL;
  for (size_t index = 0; index < offsetof(CoreStateV2, crc32); ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^
          (0xedb88320UL & (0U - static_cast<uint32_t>(crc & 1U)));
    }
  }
  return ~crc;
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

void saveState() {
  if (!storageReady) return;
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
  if (preferences.putBytes("core_v2", &state, sizeof(state)) != sizeof(state)) {
    Serial.println("KITSU_WARN core_flush=false");
  }
}

void persistProgress() {
  saveState();
  if (!companionBrain.flush()) {
    Serial.println("KITSU_WARN brain_flush=false");
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

void uiEnergyBar(uint8_t value) {
  constexpr int16_t innerWidth = 32;
  uiRect(15, 110, innerWidth + 2, 5);
  uiFillRect(16, 111, static_cast<int16_t>(innerWidth * value / 100U), 3);
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
  if (!companionBle.ready()) return '!';
  const kitsu868::connectivity::BleLinkStatus link =
      companionBle.linkStatus(now);
  if (link.connected) return '+';
  return link.advertising ? '~' : '-';
}

char wifiIndicator(uint32_t now) {
  switch (wifiRuntime.status(now).wifiState) {
    case kitsu868::connectivity::WifiRuntimeState::Connected: return '+';
    case kitsu868::connectivity::WifiRuntimeState::Grace:
    case kitsu868::connectivity::WifiRuntimeState::Connecting:
    case kitsu868::connectivity::WifiRuntimeState::Backoff:
    case kitsu868::connectivity::WifiRuntimeState::BleActive: return '~';
    case kitsu868::connectivity::WifiRuntimeState::Unconfigured: return '-';
    case kitsu868::connectivity::WifiRuntimeState::StorageUnavailable:
    case kitsu868::connectivity::WifiRuntimeState::ConnectivityUnavailable:
      return '!';
  }
  return '!';
}

char gatewayIndicator() {
  if (gatewayBootstrap.active()) return '~';
  switch (gatewayLanServiceState) {
    case GatewayLanServiceState::Connected: return '+';
    case GatewayLanServiceState::Unconfigured: return '-';
    case GatewayLanServiceState::BlePriority:
    case GatewayLanServiceState::WifiPending:
    case GatewayLanServiceState::EnrollmentPending:
    case GatewayLanServiceState::TimePending:
    case GatewayLanServiceState::Reconnecting: return '~';
    case GatewayLanServiceState::ConfigUnavailable:
    case GatewayLanServiceState::ConnectivityUnavailable:
    case GatewayLanServiceState::ReplayUnavailable: return '!';
  }
  return '!';
}

void uiConnectionIndicators(int16_t y = 3) {
  const uint32_t now = millis();
  const char labels[] = {'B', 'W', 'G'};
  const char states[] = {
      bleIndicator(now), wifiIndicator(now), gatewayIndicator()};
  for (uint8_t index = 0U; index < 3U; ++index) {
    const int16_t x = 2 + static_cast<int16_t>(index) * 16;
    uiGlyph(labels[index], x, y, 1);
    uiGlyph(states[index], x + 6, y, 1);
  }
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

String wifiStatusLabel(uint32_t now) {
  const kitsu868::connectivity::ConnectivityRuntimeStatus status =
      wifiRuntime.status(now);
  switch (status.wifiState) {
    case kitsu868::connectivity::WifiRuntimeState::Unconfigured:
      return "SETUP NEEDED";
    case kitsu868::connectivity::WifiRuntimeState::StorageUnavailable:
      return "STORE ERROR";
    case kitsu868::connectivity::WifiRuntimeState::ConnectivityUnavailable:
      return "UNAVAILABLE";
    case kitsu868::connectivity::WifiRuntimeState::BleActive:
      return "BLE ACTIVE";
    case kitsu868::connectivity::WifiRuntimeState::Grace: return "STARTING";
    case kitsu868::connectivity::WifiRuntimeState::Connecting:
      return "CONNECTING";
    case kitsu868::connectivity::WifiRuntimeState::Connected:
      return "CONNECTED";
    case kitsu868::connectivity::WifiRuntimeState::Backoff:
      return status.retryRemainingMs == 0U
                 ? "RETRYING"
                 : "RETRY " + String((status.retryRemainingMs + 999U) / 1000U) +
                       "S";
  }
  return "UNAVAILABLE";
}

const char* gatewayStatusLabel(uint32_t now) {
  if (gatewayBootstrap.active()) return "BOOTSTRAP";
  const kitsu868::connectivity::GatewayEnrollmentFlowStatus enrollment =
      gatewayEnrollmentFlow.status(now);
  switch (enrollment.state) {
    case kitsu868::connectivity::
        GatewayEnrollmentFlowState::PhysicalConfirmationRequired:
      return "CONFIRM";
    case kitsu868::connectivity::GatewayEnrollmentFlowState::PhysicalConfirmed:
      return "APP FINISH";
    case kitsu868::connectivity::GatewayEnrollmentFlowState::ReadyForWifi:
    case kitsu868::connectivity::GatewayEnrollmentFlowState::Bootstrapping:
      return "BOOTSTRAP";
    default: break;
  }
  switch (gatewayLanServiceState) {
    case GatewayLanServiceState::ConfigUnavailable: return "STORE ERROR";
    case GatewayLanServiceState::ConnectivityUnavailable:
      return "UNAVAILABLE";
    case GatewayLanServiceState::Unconfigured: return "SETUP NEEDED";
    case GatewayLanServiceState::BlePriority: return "BLE ACTIVE";
    case GatewayLanServiceState::WifiPending: return "WIFI WAIT";
    case GatewayLanServiceState::EnrollmentPending: return "ENROLL NEEDED";
    case GatewayLanServiceState::TimePending: return "TIME SYNC";
    case GatewayLanServiceState::ReplayUnavailable: return "STORE ERROR";
    case GatewayLanServiceState::Reconnecting: return "CONNECTING";
    case GatewayLanServiceState::Connected: return "CONNECTED";
  }
  return "UNAVAILABLE";
}

const char* connectionActionLabel(ConnectionAction action) {
  switch (action) {
    case ConnectionAction::Bluetooth: return "BLUETOOTH";
    case ConnectionAction::Wifi: return "WIFI";
    case ConnectionAction::Gateway: return "GATEWAY";
    case ConnectionAction::Back: return "BACK";
  }
  return "BACK";
}

const char* connectionActionPrompt(ConnectionAction action, uint32_t now) {
  const kitsu868::connectivity::ConnectionConfigStatus config =
      connectionConfigStore.status();
  if (action == ConnectionAction::Bluetooth) {
    return companionBle.linkStatus(now).connected ? "HOLD VIEW" : "HOLD OPEN";
  }
  if (action == ConnectionAction::Wifi) {
    return config.begun && config.wifiConfigured
               ? "HOLD RETRY"
               : "HOLD SETUP";
  }
  if (action == ConnectionAction::Gateway) {
    return config.begun && config.gatewayConfigured && config.gatewayEnrolled
               ? "HOLD RETRY"
               : "HOLD SETUP";
  }
  return "HOLD BACK";
}

void uiWrappedText(const char* rawText, int16_t y, uint8_t maxLines) {
  String remaining = oledSafeText(rawText);
  remaining.trim();
  const size_t charactersPerLine = kitsu868::portrait::lineCapacity(
      kitsu868::portrait::kContentWidth, 1,
      kitsu868::portrait::regularAdvance(1));
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
    uiTextCentered(line, y + lineIndex * 10);
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
      screen == Screen::PairPhone) {
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

bool drawCreatureSprite(int16_t y = 24) {
  if (!activeAnimation.active) startBaseAnimation();
  if (!activeAnimation.active) return false;
  const uint8_t* sprite = companionPack.activeFrame(
      millis() - activeAnimation.startedAt);
  if (!sprite) return false;
  uiXbm((UI_WIDTH - KITSU_FRAME_WIDTH) / 2, y,
        KITSU_FRAME_WIDTH, KITSU_FRAME_HEIGHT, sprite);

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
  if (!drawCreatureSprite(20)) {
    renderMissingPack();
    return;
  }
  uiMailBadge();
  uiTextCentered(moodText(), 93);
  uiEnergyBar(wisp.energy);
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
  uiTextCentered(connectionActionLabel(connectionAction), 31, 2);
  switch (connectionAction) {
    case ConnectionAction::Bluetooth:
      uiTextCentered(bluetoothStatusLabel(now), 54);
      break;
    case ConnectionAction::Wifi:
      uiTextCentered(wifiStatusLabel(now), 54);
      break;
    case ConnectionAction::Gateway:
      uiTextCentered(gatewayStatusLabel(now), 54);
      break;
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

void renderGame() {
  if (activeGame == ActiveGame::SignalCatch) renderSignalGame();
  else if (activeGame == ActiveGame::PounceFetch) renderPounceGame();
  else renderGameMenu();
}

void renderListen() {
  if (!companionPack.valid()) {
    renderMissingPack();
    return;
  }
  uiTextCentered("LISTEN", 7);
  const uint32_t now = millis();
  const int32_t remainingMs = static_cast<int32_t>(listenUntil - now);
  const uint32_t remaining = remainingMs > 0
                                 ? (static_cast<uint32_t>(remainingMs) + 999UL) / 1000UL
                                 : 0;
  if (!drawCreatureSprite(25)) {
    renderMissingPack();
    return;
  }
  uiTextCentered(String(remaining) + "S", 101, 2);
}

void renderSleep() {
  uiConnectionIndicators();
  if (!drawCreatureSprite(22)) {
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
  uiMenuDots(statusPage, 4, 121);
}

void renderPairPhone() {
  uiTextCentered("PAIR PHONE", 4);
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
  const kitsu868::connectivity::GatewayEnrollmentFlowStatus enrollment =
      gatewayEnrollmentFlow.status(now);
  if (enrollment.state == kitsu868::connectivity::
          GatewayEnrollmentFlowState::PhysicalConfirmationRequired) {
    const uint32_t seconds = (enrollment.expiresInMs + 999U) / 1000U;
    uiTextCentered("GATEWAY", 21);
    uiTextCentered("HOLD", 42, 2);
    uiTextCentered("PRG", 60, 2);
    uiTextCentered("TO ENROLL", 83);
    uiTextCentered(String(seconds) + "S", 105);
  } else if (enrollment.state == kitsu868::connectivity::
                 GatewayEnrollmentFlowState::PhysicalConfirmed) {
    uiTextCentered("CONFIRMED", 28);
    uiTextCentered("APP", 50, 2);
    uiTextCentered("FINISH", 69, 2);
    uiTextCentered("TAP CANCEL", 105);
  } else if (enrollment.state == kitsu868::connectivity::
                 GatewayEnrollmentFlowState::ReadyForWifi) {
    uiTextCentered("ENROLL READY", 27);
    uiTextCentered("DISCONNECT", 52);
    uiTextCentered("WIFI NEXT", 76);
    uiTextCentered("APP CONTROLS", 105);
  } else if (link.numericComparisonPending) {
    char passkey[7]{};
    snprintf(passkey, sizeof(passkey), "%06lu",
             static_cast<unsigned long>(link.numericComparison));
    uiTextCentered("MATCH CODE", 22);
    uiTextCentered(passkey, 42, 2);
    uiTextCentered("HOLD IF SAME", 78);
    uiTextCentered("TAP CANCEL", 105);
  } else if (session.physicalConfirmationPending) {
    uiTextCentered("PHONE READY", 23);
    uiTextCentered("HOLD", 44, 2);
    uiTextCentered("PRG", 62, 2);
    uiTextCentered("TO GRANT", 85);
    uiTextCentered("TAP CANCEL", 105);
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
    case Screen::PairPhone: renderPairPhone(); break;
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
  statusPage = 3;
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

bool ensureAwake() {
  if (!wisp.sleeping) return false;
  wisp.sleeping = false;
  const kitsu868::BrainEventResult result = companionBrain.onWake();
  pendingEvolutionReaction = pendingEvolutionReaction || result.evolved();
  lastMemory = "The signal woke up.";
  logBrainResult(result);
  return true;
}

bool petWisp() {
  if (!requireCompanion()) return false;
  ensureAwake();
  wisp.energy = min<uint8_t>(100, wisp.energy + 4);
  wisp.curiosity = min<uint8_t>(100, wisp.curiosity + 1);
  wisp.affection = min<uint8_t>(100, wisp.affection + 3);
  ++wisp.pets;
  lastMemory = "You reached through the static.";
  const kitsu868::BrainEventResult result = companionBrain.onPet();
  startReaction(CompanionRole::Pet, result);
  persistProgress();
  Serial.printf("KITSU_EVENT pet count=%lu energy=%u affection=%u\n",
                static_cast<unsigned long>(wisp.pets), wisp.energy, wisp.affection);
  return true;
}

bool feedKitsu() {
  if (!requireCompanion()) return false;
  ensureAwake();
  wisp.energy = min<uint8_t>(100, wisp.energy + 18);
  wisp.affection = min<uint8_t>(100, wisp.affection + 1);
  lastMemory = "A small meal crossed the static.";
  const kitsu868::BrainEventResult result = companionBrain.onFeed();
  startReaction(CompanionRole::Feed, result);
  persistProgress();
  Serial.printf("KITSU_EVENT feed energy=%u affection=%u\n",
                wisp.energy, wisp.affection);
  return true;
}

bool playKitsu() {
  if (!requireCompanion()) return false;
  ensureAwake();
  // Active play may exhaust the companion below the passive-decay floor, but
  // it must never increase energy when the current value is already low.
  wisp.energy = wisp.energy > 6 ? wisp.energy - 6 : 1;
  wisp.curiosity = min<uint8_t>(100, wisp.curiosity + 10);
  wisp.affection = min<uint8_t>(100, wisp.affection + 4);
  lastMemory = "You played through the static.";
  const kitsu868::BrainEventResult result = companionBrain.onPlay();
  startReaction(CompanionRole::Play, result);
  persistProgress();
  Serial.printf("KITSU_EVENT play energy=%u curiosity=%u affection=%u\n",
                wisp.energy, wisp.curiosity, wisp.affection);
  return true;
}

void setSleeping(bool sleeping) {
  if (sleeping && !requireCompanion()) return;
  wisp.sleeping = sleeping;
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
  } else {
    if (!startTransientAnimation(evolved ? CompanionRole::Evolve
                                         : CompanionRole::Wake)) {
      startBaseAnimation();
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
  enterScreen(Screen::Game);
  Serial.printf("KITSU_GAME start=%s\n",
                game == ActiveGame::SignalCatch ? "signal" : "pounce");
}

uint16_t activeGameScore() {
  if (activeGame == ActiveGame::SignalCatch) return signalCatchGame.score();
  if (activeGame == ActiveGame::PounceFetch) return pounceFetchGame.score();
  return 0;
}

bool activeGameFinished() {
  if (activeGame == ActiveGame::SignalCatch) return signalCatchGame.finished();
  if (activeGame == ActiveGame::PounceFetch) return pounceFetchGame.finished();
  return false;
}

void rewardFinishedGame() {
  if (gameRewarded || !activeGameFinished()) return;
  gameRewarded = true;
  const uint16_t rawScore = activeGameScore();
  const uint8_t scorePercent = static_cast<uint8_t>(
      min<uint16_t>(100, static_cast<uint16_t>(
          (static_cast<uint32_t>(rawScore) * 100U + 12U) / 24U)));
  const bool perfect = gamePerfectRounds == 5U;
  wisp.energy = wisp.energy > 4U ? wisp.energy - 4U : 1U;
  wisp.curiosity = min<uint8_t>(100, wisp.curiosity + 5U);
  wisp.affection = min<uint8_t>(100, wisp.affection + (perfect ? 5U : 2U));
  const kitsu868::BrainEventResult result =
      companionBrain.onGame(scorePercent, perfect);
  gameEvolved = gameEvolved || result.evolved();
  lastMemory = perfect ? "Perfect timing became a memory."
                       : "A little game became a memory.";
  logBrainResult(result);
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
  else pounceFetchGame.tick(now);
  const kitsu868::MiniGamePhase phase = activeGame == ActiveGame::SignalCatch
      ? signalCatchGame.view(now).phase
      : pounceFetchGame.view(now).phase;
  if (lastGamePhase != kitsu868::MiniGamePhase::Result &&
      phase == kitsu868::MiniGamePhase::Result) {
    const kitsu868::MiniGameResult result =
        activeGame == ActiveGame::SignalCatch
            ? signalCatchGame.view(now).result
            : pounceFetchGame.view(now).result;
    if (result == kitsu868::MiniGameResult::Perfect &&
        gamePerfectRounds != 0xffU) {
      ++gamePerfectRounds;
    }
  }
  lastGamePhase = phase;
  rewardFinishedGame();
}

void leaveGame(bool celebrate) {
  if (activeGame == ActiveGame::SignalCatch) signalCatchGame.cancel();
  else if (activeGame == ActiveGame::PounceFetch) pounceFetchGame.cancel();
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
  if (activeGame != ActiveGame::None) {
    Serial.println("KITSU_ERROR busy=game");
    return false;
  }
  if (ensureAwake()) persistProgress();
  if (!radioReady) {
    lastMemory = meshSettings.enabled
                     ? "The MeshCore radio did not answer."
                     : "MeshCore is turned off.";
    enterScreen(Screen::Status);
    return false;
  }

  radioListening = true;
  listenUntil = millis() + durationMs;
  if (pendingEvolutionReaction) {
    pendingEvolutionReaction = false;
    cancelAmbientAnimation();
    if (!startTransientAnimation(CompanionRole::Evolve)) startBaseAnimation();
  } else {
    startBaseAnimation();
  }
  enterScreen(Screen::Listen);
  Serial.printf("KITSU_RADIO listening=true meshcore=true tx_unlocked=%s\n",
                meshTransport.transmitAllowed(meshSettings) ? "true" : "false");
  return true;
}

void executeMenuItem() {
  switch (menuIndex) {
    case 0:
      connectionAction = ConnectionAction::Bluetooth;
      if (connectionConfigStore.status().begun) {
        const kitsu868::connectivity::ConnectionConfigStatus config =
            connectionConfigStore.status();
        const kitsu868::connectivity::ConnectivityRuntimeStatus wifi =
            wifiRuntime.status(millis());
        if (!config.wifiConfigured ||
            wifi.wifiState !=
                kitsu868::connectivity::WifiRuntimeState::Connected) {
          connectionAction = ConnectionAction::Wifi;
        } else if (!config.gatewayConfigured || !config.gatewayEnrolled ||
                   gatewayLanServiceState !=
                       GatewayLanServiceState::Connected) {
          connectionAction = ConnectionAction::Gateway;
        }
      }
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
      inboxSelection = 0;
      markChatJournalRead();
      enterScreen(Screen::Inbox);
      break;
    case 5: startListening(); break;
    case 6:
      if (!requireCompanion()) break;
      setSleeping(true);
      enterScreen(Screen::Sleep);
      break;
    case 7:
      statusPage = 0;
      sampleBattery(true);
      enterScreen(Screen::Status);
      break;
    default: enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet); break;
  }
}

bool openBluetoothControl(uint32_t now) {
  if (!companionBle.ready()) return false;
  const kitsu868::connectivity::BleLinkStatus link =
      companionBle.linkStatus(now);
  if (!link.connected && !companionBle.openPairing(now)) return false;
  enterScreen(Screen::PairPhone);
  return true;
}

void executeConnectionAction() {
  const uint32_t now = millis();
  const kitsu868::connectivity::ConnectionConfigStatus config =
      connectionConfigStore.status();
  if (connectionAction == ConnectionAction::Back) {
    enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
    return;
  }
  if (connectionAction == ConnectionAction::Bluetooth) {
    const bool opened = openBluetoothControl(now);
    Serial.printf("KITSU_CONNECT_ACTION action=bluetooth result=%s\n",
                  opened ? "opened" : "unavailable");
    return;
  }
  const bool needsSetup = !config.begun ||
      (connectionAction == ConnectionAction::Wifi
           ? !config.wifiConfigured
           : !config.gatewayConfigured || !config.gatewayEnrolled);
  if (needsSetup) {
    const bool opened = openBluetoothControl(now);
    Serial.printf("KITSU_CONNECT_ACTION action=%s result=%s\n",
                  connectionAction == ConnectionAction::Wifi
                      ? "wifi_setup"
                      : "gateway_setup",
                  opened ? "opened" : "unavailable");
    return;
  }
  if (!deviceSecurity.remoteConnectivityAllowed()) {
    Serial.printf("KITSU_CONNECT_ACTION action=%s result=unavailable\n",
                  connectionAction == ConnectionAction::Wifi
                      ? "wifi_retry"
                      : "gateway_retry");
    lastRenderAt = 0U;
    return;
  }
  stopGatewayLanRuntime();
  if (connectionAction == ConnectionAction::Wifi ||
      wifiRuntime.status(now).wifiState !=
          kitsu868::connectivity::WifiRuntimeState::Connected) {
    wifiRuntime.requestCredentialReload();
  }
  gatewaySnapshotDirty = true;
  Serial.printf("KITSU_CONNECT_ACTION action=%s result=retrying\n",
                connectionAction == ConnectionAction::Wifi
                    ? "wifi_retry"
                    : "gateway_retry");
  screenEnteredAt = now;
  lastRenderAt = 0U;
}

void executeGameMenuItem() {
  if (gameMenuIndex == 0) startGame(ActiveGame::SignalCatch);
  else if (gameMenuIndex == 1) startGame(ActiveGame::PounceFetch);
  else enterScreen(Screen::Menu);
}

void handleShortPress(uint32_t actionAt) {
  switch (screen) {
    case Screen::Pet: petWisp(); break;
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
      }
      break;
    case Screen::Listen: stopListening(); break;
    case Screen::Sleep:
      setSleeping(false);
      enterScreen(Screen::Pet);
      break;
    case Screen::Status:
      statusPage = (statusPage + 1U) % 4U;
      screenEnteredAt = millis();
      break;
    case Screen::PairPhone:
      gatewayEnrollmentFlow.abort();
      companionBle.closePairing();
      enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
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
    case Screen::Listen: stopListening(); break;
    case Screen::Sleep:
      setSleeping(false);
      enterScreen(Screen::Pet);
      break;
    case Screen::Status: enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet); break;
    case Screen::PairPhone: {
      const uint32_t now = millis();
      const kitsu868::connectivity::BleLinkStatus link =
          companionBle.linkStatus(now);
      const kitsu868::connectivity::BleSessionStatus session =
          companionBle.sessionStatus(now);
      bool accepted = false;
      const kitsu868::connectivity::GatewayEnrollmentFlowStatus enrollment =
          gatewayEnrollmentFlow.status(now);
      if (enrollment.state == kitsu868::connectivity::
              GatewayEnrollmentFlowState::PhysicalConfirmationRequired &&
          session.applicationAuthenticated) {
        kitsu868::connectivity::GatewayEnrollmentReceipt event{};
        accepted = gatewayEnrollmentFlow.confirmPhysical(now, event);
        if (accepted && !companionBle.sendEnrollmentEvent(event)) {
          Serial.println("KITSU_ENROLL event=send_failed state=confirmed");
        }
      } else if (link.numericComparisonPending) {
        accepted = companionBle.confirmNumeric();
      } else if (session.physicalConfirmationPending) {
        accepted = companionBle.confirmController(now);
      } else if (!link.pairingWindowOpen && !link.connected) {
        accepted = companionBle.openPairing(now);
      }
      Serial.printf("KITSU_PAIR physical_action=%s\n",
                    accepted ? "accepted" : "ignored");
      screenEnteredAt = now;
      break;
    }
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
    buttonPressedAt = millis();
    Serial.println("KITSU_BUTTON pressed");
  } else {
    const uint32_t duration = millis() - buttonPressedAt;
    Serial.printf("KITSU_BUTTON released duration_ms=%lu\n",
                  static_cast<unsigned long>(duration));
    if (duration >= BUTTON_HOLD_MS) handleLongPress();
    else handleShortPress(buttonPressedAt);
  }
}

uint16_t ownUidSuffix() {
  return static_cast<uint16_t>(strtoul(wisp.uid.c_str() + 2, nullptr, 16));
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
      } else if (recorded.urgent) {
        const kitsu868::discovery::JournalResult flushed =
            discoveryJournal.flush();
        if (flushed != kitsu868::discovery::JournalResult::Ok) {
          discoveryJournalDirtyAt = millis();
          Serial.printf("KITSU_WARN discovery_flush=%s urgent=true\n",
                        kitsu868::discovery::journalResultName(flushed));
        }
      } else {
        discoveryJournalDirtyAt = millis();
      }
    }

    // Mesh reception remains active in the background, but creature progress
    // changes only during the explicit 60-second RADIO activity.  This avoids
    // surprise NVS writes and off-screen reactions on a busy public mesh.
    if (!radioListening) continue;

    char peerIdentity[11];
    snprintf(peerIdentity, sizeof(peerIdentity), "MC%s", publicPrefix);
    const uint32_t fingerprint =
        kitsu868::CompanionBrain::fingerprint(peerIdentity);
    const kitsu868::BrainEventResult result =
        companionBrain.onEncounter(fingerprint);
    wisp.curiosity = min<uint8_t>(100, wisp.curiosity + 6U);
    wisp.affection = min<uint8_t>(
        100, wisp.affection + static_cast<uint8_t>(result.newEncounter ? 2U : 1U));
    lastMemory = advert.kitsuNamed
                     ? "Another Kitsu client crossed the mesh."
                     : "A MeshCore signal crossed the dark.";
    startReaction(advert.kitsuNamed || result.newEncounter
                      ? CompanionRole::Meet
                      : CompanionRole::Surprise,
                  result);
    radioProgressDirty = true;
  }
}

void tickDiscoveryJournal(uint32_t now) {
  if (!discoveryJournalReady) return;
  const kitsu868::discovery::JournalStatus status = discoveryJournal.status();
  if (!status.dirty ||
      static_cast<uint32_t>(now - discoveryJournalDirtyAt) <
          DISCOVERY_JOURNAL_DEBOUNCE_MS) {
    return;
  }
  const kitsu868::discovery::JournalResult flushed = discoveryJournal.flush();
  if (flushed != kitsu868::discovery::JournalResult::Ok) {
    // Keep retry cadence bounded; flush() intentionally leaves the in-RAM
    // journal dirty and the previous slot recoverable.
    discoveryJournalDirtyAt = now;
    Serial.printf("KITSU_WARN discovery_flush=%s urgent=false\n",
                  kitsu868::discovery::journalResultName(flushed));
  }
}

ChatJournalEntry* findPendingChannelJournal(uint8_t channelSlot) {
  // Transport lifecycle events are emitted in queue order. Match the oldest
  // queued send first so a cancel/requeue burst cannot swap two same-channel
  // journal entries.
  for (uint8_t ordinal = 0; ordinal < chatJournalCount; ++ordinal) {
    const uint8_t index = static_cast<uint8_t>(
        (chatJournalStart + ordinal) % kitsu868::chat::kInboxCapacity);
    ChatJournalEntry* entry = &chatJournal[index];
    if (entry && !entry->inbound &&
        entry->kind == kitsu868::mesh::MessageKind::Channel &&
        entry->channelSlot == channelSlot &&
        entry->state == ChatJournalState::Queued) {
      return entry;
    }
  }
  return nullptr;
}

void emitChatEvent(const char* event, const ChatJournalEntry& entry,
                   const char* state = nullptr, int32_t roundTripMs = -1) {
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
    entry.snr = received.snr;

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
  }

  kitsu868::mesh::DeliveryEvent delivery{};
  while (meshTransport.takeDelivery(delivery)) {
    ChatJournalEntry* entry = delivery.kind == kitsu868::mesh::MessageKind::Direct
                                  ? findChatByDelivery(delivery)
                                  : findPendingChannelJournal(
                                        delivery.channelSlot);
    if (!entry) {
      Serial.println("KITSU_WARN chat_delivery_unmatched=true");
      continue;
    }
    const uint32_t elapsed = millis() -
        (entry->sentAt != 0U ? entry->sentAt : entry->queuedAt);
    switch (delivery.state) {
      case kitsu868::mesh::DeliveryState::Sent:
        entry->state = ChatJournalState::Sent;
        entry->sentAt = millis();
        emitChatEvent("tx", *entry, "sent");
        break;
      case kitsu868::mesh::DeliveryState::Delivered:
        entry->state = ChatJournalState::Delivered;
        emitChatEvent("delivery", *entry, "delivered",
                      elapsed > INT32_MAX ? INT32_MAX
                                          : static_cast<int32_t>(elapsed));
        break;
      case kitsu868::mesh::DeliveryState::TimedOut:
        entry->state = ChatJournalState::Unconfirmed;
        emitChatEvent("delivery", *entry, "unconfirmed");
        break;
      case kitsu868::mesh::DeliveryState::Cancelled:
        entry->state = ChatJournalState::Cancelled;
        emitChatEvent("tx", *entry, "cancelled");
        break;
      case kitsu868::mesh::DeliveryState::TxFailed:
        entry->state = ChatJournalState::Failed;
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
  if (brainMinutesSinceFlush >= 15U) {
    companionBrain.flush();
    brainMinutesSinceFlush = 0;
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
  const kitsu868::LifetimeCounters& life = companionBrain.lifetime();
  const kitsu868::CompanionMood mood = companionBrain.mood(companionVitals());
  const String escapedCompanion = jsonEscaped(companionName());
  const char* gameName = activeGame == ActiveGame::SignalCatch
                             ? "signal"
                             : activeGame == ActiveGame::PounceFetch
                                   ? "pounce"
                                   : "none";
  Serial.printf(
      "KITSU_SELFTEST {\"firmware\":\"%s\",\"version\":\"%s\","
      "\"board\":\"heltec-v3.2\",\"oled\":%s,\"radio\":%s,"
      "\"radio_code\":%d,\"storage\":%s,\"button_released\":%s,"
      "\"tx_enabled\":false,\"boot\":%lu,\"uid\":\"%s\","
      "\"companion\":\"%s\",\"orientation\":\"portrait\",",
      FIRMWARE_NAME, FIRMWARE_VERSION,
      oledDetected ? "true" : "false", radioReady ? "true" : "false",
      radioInitCode, storageReady ? "true" : "false",
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
      "\"remote_available\":false,\"mesh_protocol\":1,"
      "\"meshcore_version\":\"1.17.1\",\"mesh_enabled\":%s,"
      "\"mesh_rx_ready\":%s,\"mesh_time_valid\":%s,"
      "\"mesh_tx_unlocked\":%s,\"mesh_profile\":\"%s\","
      "\"mesh_adverts\":%lu,\"chat_protocol\":1,"
      "\"chat_storage\":%s,\"chat_contacts\":%u,"
      "\"chat_channels\":%u,\"chat_messages\":%u,"
      "\"chat_unread\":%u}\n",
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
      unreadChatMessages);
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
      "\"memory_event\":%u,\"remote\":false,"
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
  } else if (command == "mesh config on" ||
             command == "mesh config off" ||
             command.startsWith("mesh config ")) {
    configureMeshFromCommand(command);
  } else if (command.startsWith("mesh time ")) {
    unsigned long long epoch = 0;
    char trailing = 0;
    if (sscanf(command.c_str(), "mesh time %llu %c", &epoch, &trailing) != 1 ||
        epoch > UINT32_MAX) {
      printMeshResult("time", "rejected", "invalid_time");
    } else {
      const kitsu868::mesh::TransportStatus status =
          meshTransport.setEpoch(static_cast<uint32_t>(epoch));
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
          emitChatEvent("delivery", *entry, "unconfirmed");
        } else {
          entry->state = ChatJournalState::Cancelled;
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
      const kitsu868::mesh::TransportStatus status =
          meshTransport.setChannel(command.channelIndex, command.name, secret);
      memset(secret, 0, sizeof(secret));
      if (status == kitsu868::mesh::TransportStatus::Ok) {
        gatewaySnapshotDirty = true;
      }
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
      if (status == kitsu868::mesh::TransportStatus::Ok) {
        gatewaySnapshotDirty = true;
      }
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
  command.toLowerCase();
  if (executeMeshCommand(command)) return;
  if (command == "status" || command == "selftest") printSelfTest();
  else if (command == "sync" || command == "brain") printSync();
  else if (command == "journal") printJournal();
  else if (command == "pet") petWisp();
  else if (command == "feed") feedKitsu();
  else if (command == "play") playKitsu();
  else if (command == "game signal") startGame(ActiveGame::SignalCatch);
  else if (command == "game pounce") startGame(ActiveGame::PounceFetch);
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
      if (radioListening) stopListening();
      setSleeping(true);
      enterScreen(Screen::Sleep);
    }
  } else if (command == "wake") {
    if (activeGame != ActiveGame::None) {
      Serial.println("KITSU_ERROR busy=game");
    } else {
      setSleeping(false);
      enterScreen(Screen::Pet);
    }
  } else if (command == "stop") {
    if (radioListening) stopListening();
  } else if (command == "pack") {
    printSelfTest();
  } else if (command.startsWith("anim ")) {
    if (activeGame != ActiveGame::None) {
      Serial.println("KITSU_ERROR busy=game");
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
        "KITSU_HELP selftest|sync|journal|pet|feed|play|game <signal|pounce|tap|cancel>|listen|sleep|wake|stop|inject <38hex>|anim <role>|mesh <status|config|time|tx|location|introduce|publish-map>|chat <status|contacts|channels|inbox|contact|channel|send>|help");
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

void initConnectivityStorage() {
  gatewayEnrollmentFlow.abort();
  gatewayBootstrap.cancel();
  if (gatewayBootstrapWorkspace) {
    wipeSensitive(gatewayBootstrapWorkspace,
                  sizeof(*gatewayBootstrapWorkspace));
    delete gatewayBootstrapWorkspace;
    gatewayBootstrapWorkspace = nullptr;
  }
  gatewayBootstrapTransport.close();
  gatewayBootstrapLastResult =
      kitsu868::connectivity::GatewayBootstrapResult::NotActive;
  gatewayLanRuntime.stop();
  mobileRelay.stop();
  gatewayLanActions.stop();
  gatewayLanReplayStore.stop();
  gatewayLanReplayStorage.end();
  connectivitySecurityReady = false;
  discoveryJournalReady = false;
  gatewayLanReplayReady = false;
  gatewayLanActionsReady = false;
  mobileRelayReady = false;
  gatewayLanBegun = false;
  gatewayLanEverConnected = false;
  gatewayLanNextSnapshotAt = 0U;
  gatewayLanLastSnapshotHash = 0U;
  gatewayLanLastObservedSnapshotHash = 0U;
  gatewaySnapshotDirty = true;
  gatewayLanLastResult =
      kitsu868::connectivity::GatewayLanRuntimeResult::NotBegun;
  gatewayLanServiceState = GatewayLanServiceState::ConfigUnavailable;
  gatewayLanReportInitialized = false;
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
      "KITSU_CONNECTIVITY_SECURITY status=%s security_mode=%s "
      "application_encrypted=%s hardware_root_protected=%s "
      "remote_allowed=%s controllers=%u\n",
      kitsu868::connectivity::securityResultName(security),
      kitsu868::connectivity::kSelectedSecurityModeName,
      securityStatus.applicationEncrypted ? "true" : "false",
      securityStatus.hardwareRootProtected ? "true" : "false",
      deviceSecurity.remoteConnectivityAllowed() ? "true" : "false",
      securityStatus.controllerCount);
  if (!connectivitySecurityReady) return;
  Serial.println(
      "KITSU_REFLASHABLE secure_boot=false flash_encryption=false "
      "nvs_encryption=false application_encrypted=true "
      "owner_repurpose_allowed=true");

  uint8_t connectionKey[kitsu868::connectivity::kKitsuSecretBytes]{};
  const kitsu868::connectivity::SecurityResult connectionKeyResult =
      deviceSecurity.deriveConnectionStoreKey(connectionKey);
  const bool connectionPartitionReady = connectionSlotStorage.begin();
  const bool connectionCryptoReady =
      connectionKeyResult == kitsu868::connectivity::SecurityResult::Ok &&
      connectionStoreCrypto.setKey(connectionKey);
  wipeSensitive(connectionKey, sizeof(connectionKey));
  kitsu868::connectivity::ConfigResult configStoreResult =
      kitsu868::connectivity::ConfigResult::StorageUnavailable;
  if (connectionPartitionReady && connectionCryptoReady) {
    configStoreResult = connectionConfigStore.begin(
        connectionSlotStorage, connectionStoreCrypto, gatewayTrustValidator);
  } else if (!connectionCryptoReady) {
    configStoreResult =
        kitsu868::connectivity::ConfigResult::SecurityUnavailable;
  }
  connectionConfigStore.setRemoteConnectivityAllowed(
      deviceSecurity.remoteConnectivityAllowed());
  const bool lanReplayStorageReady = gatewayLanReplayStorage.begin();
  gatewayLanReplayReady = lanReplayStorageReady &&
      gatewayLanReplayStore.begin(gatewayLanReplayStorage);
  gatewayLanActionsReady = gatewayLanReplayReady &&
      gatewayLanActions.begin(gatewayLanReplayStore,
                              gatewayLanActionExecutor,
                              gatewayLanPayloadQueue);
  mobileRelayReady = connectionConfigStore.ready() &&
      gatewayLanActionsReady &&
      mobileRelay.begin(connectionConfigStore, gatewayLanSequences,
                        gatewayLanCrypto, gatewayLanReplayStore,
                        gatewayLanActions, connectionConfigStore,
                        mobileRelayEnrollment);
  const kitsu868::connectivity::GatewayLanActionReplayStatus replayStatus =
      gatewayLanReplayStore.status();
  Serial.printf(
      "KITSU_GATEWAY_REPLAY storage=%s ledger=%s dispatcher=%s "
      "relay=%s records=%u generation=%lu\n",
      lanReplayStorageReady ? "ready" : "blocked",
      gatewayLanReplayReady ? "ready" : "blocked",
      gatewayLanActionsReady ? "ready" : "blocked",
      mobileRelayReady ? "ready" : "blocked",
      static_cast<unsigned>(replayStatus.records),
      static_cast<unsigned long>(replayStatus.generation));
  char wifiHostname[33]{};
  snprintf(wifiHostname, sizeof(wifiHostname), "kitsu-%s",
           wisp.uid.c_str());
  const bool wifiManagerReady = wifiRuntime.begin(
      connectionConfigStore, deviceSecurity.remoteConnectivityAllowed(),
      wifiHostname);
  memset(wifiHostname, 0, sizeof(wifiHostname));
  const kitsu868::connectivity::ConnectionConfigStatus configStatus =
      connectionConfigStore.status();
  Serial.printf(
      "KITSU_CONNECTIVITY_CONFIG status=%s partition=%s store=%s "
      "wifi=%s gateway=%s enrolled=%s runtime=%s\n",
      kitsu868::connectivity::configResultName(configStoreResult),
      connectionPartitionReady ? "ready" : "missing",
      configStatus.begun ? "ready" : "blocked",
      configStatus.wifiConfigured ? "configured" : "unconfigured",
      configStatus.gatewayConfigured ? "configured" : "unconfigured",
      configStatus.gatewayEnrolled ? "true" : "false",
      wifiManagerReady ? "ready" : "blocked");

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

bool trustedGatewayWallClock(int64_t& epoch) {
  return kitsu868::connectivity::trustedWallClock(epoch);
}

void serviceGatewayEnrollment(uint32_t now, bool authenticatedBleSession) {
  if (gatewayBootstrap.active()) {
    const kitsu868::connectivity::ConnectivityRuntimeStatus wifi =
        wifiRuntime.status(now);
    int64_t epoch = 0;
    if (authenticatedBleSession ||
        wifi.wifiState !=
            kitsu868::connectivity::WifiRuntimeState::Connected ||
        !trustedGatewayWallClock(epoch) ||
        !deviceSecurity.remoteConnectivityAllowed()) {
      gatewayBootstrap.cancel();
      gatewayBootstrapLastResult = authenticatedBleSession
          ? kitsu868::connectivity::GatewayBootstrapResult::NotActive
          : !deviceSecurity.remoteConnectivityAllowed()
                ? kitsu868::connectivity::GatewayBootstrapResult::
                      RemoteConnectivityUnavailable
                : !trustedGatewayWallClock(epoch)
                      ? kitsu868::connectivity::GatewayBootstrapResult::
                            TimeUnavailable
                      : kitsu868::connectivity::GatewayBootstrapResult::
                            TransportFailed;
      gatewayEnrollmentFlow.completeBootstrap(false);
    } else {
      gatewayBootstrapLastResult =
          gatewayBootstrap.pollExchangeAndInstall();
      if (gatewayBootstrapLastResult ==
          kitsu868::connectivity::GatewayBootstrapResult::InProgress) {
        return;
      }
      const bool installed = gatewayBootstrapLastResult ==
              kitsu868::connectivity::GatewayBootstrapResult::
                  ReconnectSteady &&
          connectionConfigStore.status().gatewayEnrolled;
      gatewayEnrollmentFlow.completeBootstrap(installed);
      Serial.printf(
          "KITSU_ENROLL state=%s result=%s secrets_logged=false\n",
          installed ? "enrolled" : "failed",
          kitsu868::connectivity::gatewayBootstrapResultName(
              gatewayBootstrapLastResult));
    }
    if (gatewayBootstrapWorkspace) {
      wipeSensitive(gatewayBootstrapWorkspace,
                    sizeof(*gatewayBootstrapWorkspace));
      delete gatewayBootstrapWorkspace;
      gatewayBootstrapWorkspace = nullptr;
    }
    return;
  }

  const kitsu868::connectivity::GatewayEnrollmentFlowStatus attempt =
      gatewayEnrollmentFlow.status(now);
  if (attempt.state !=
          kitsu868::connectivity::GatewayEnrollmentFlowState::ReadyForWifi ||
      authenticatedBleSession) {
    return;
  }
  // A logical mobile relay deliberately has no LAN bootstrap endpoint. Its
  // exact issuer response is installed only through mobile.relay.exchange.
  if (connectionConfigStore.status().mobileRelayConfigured) return;
  const kitsu868::connectivity::ConnectivityRuntimeStatus wifi =
      wifiRuntime.status(now);
  int64_t epoch = 0;
  if (wifi.wifiState !=
          kitsu868::connectivity::WifiRuntimeState::Connected ||
      !trustedGatewayWallClock(epoch) ||
      !deviceSecurity.remoteConnectivityAllowed()) {
    return;
  }

  auto* config =
      new (std::nothrow) kitsu868::connectivity::GatewayConfig{};
  gatewayBootstrapWorkspace = new (std::nothrow)
      kitsu868::connectivity::GatewayBootstrapWorkspace{};
  if (!config || !gatewayBootstrapWorkspace ||
      !connectionConfigStore.copyGateway(*config)) {
    if (gatewayBootstrapWorkspace) {
      wipeSensitive(gatewayBootstrapWorkspace,
                    sizeof(*gatewayBootstrapWorkspace));
      delete gatewayBootstrapWorkspace;
      gatewayBootstrapWorkspace = nullptr;
    }
    if (config) {
      wipeSensitive(config, sizeof(*config));
      delete config;
    }
    gatewayEnrollmentFlow.completeBootstrap(
        false, kitsu868::connectivity::GatewayEnrollmentError::StorageFailed);
    gatewayBootstrapLastResult =
        kitsu868::connectivity::GatewayBootstrapResult::TransportFailed;
    Serial.println(
        "KITSU_ENROLL state=failed error=storage_failed secrets_logged=false");
    return;
  }
  if (!gatewayEnrollmentFlow.markBootstrapping(now)) {
    wipeSensitive(gatewayBootstrapWorkspace,
                  sizeof(*gatewayBootstrapWorkspace));
    wipeSensitive(config, sizeof(*config));
    delete gatewayBootstrapWorkspace;
    gatewayBootstrapWorkspace = nullptr;
    delete config;
    return;
  }

  kitsu868::connectivity::GatewayBootstrapTrust trust{};
  trust.host = config->host;
  trust.serverName = config->serverName;
  trust.port = config->bootstrapPort;
  trust.caCertificateDer = config->caCertificateDer;
  trust.caCertificateBytes = config->caCertificateBytes;
  memcpy(trust.spkiSha256, config->spkiSha256,
         sizeof(trust.spkiSha256));
  memcpy(trust.gatewayUuid, config->gatewayId,
         sizeof(trust.gatewayUuid));
  gatewayBootstrapLastResult = gatewayBootstrap.beginExchangeAndInstall(
      attempt.enrollmentId, deviceSecurity.remoteConnectivityAllowed(),
      trust, enrollmentRecipient, gatewayBootstrapTransport,
      connectionConfigStore, *gatewayBootstrapWorkspace);
  wipeSensitive(&trust, sizeof(trust));
  wipeSensitive(config, sizeof(*config));
  delete config;
  if (gatewayBootstrapLastResult ==
      kitsu868::connectivity::GatewayBootstrapResult::InProgress) {
    return;
  }
  const bool installed = gatewayBootstrapLastResult ==
          kitsu868::connectivity::GatewayBootstrapResult::ReconnectSteady &&
      connectionConfigStore.status().gatewayEnrolled;
  gatewayEnrollmentFlow.completeBootstrap(installed);
  wipeSensitive(gatewayBootstrapWorkspace,
                sizeof(*gatewayBootstrapWorkspace));
  delete gatewayBootstrapWorkspace;
  gatewayBootstrapWorkspace = nullptr;
  Serial.printf(
      "KITSU_ENROLL state=%s result=%s secrets_logged=false\n",
      installed ? "enrolled" : "failed",
      kitsu868::connectivity::gatewayBootstrapResultName(
          gatewayBootstrapLastResult));
}

void stopGatewayLanRuntime() {
  if (!gatewayLanBegun) return;
  gatewayLanRuntime.stop();
  gatewayLanBegun = false;
  gatewayLanNextSnapshotAt = 0U;
  gatewaySnapshotDirty = true;
}

uint32_t gatewaySnapshotHashUpdate(uint32_t hash, const uint8_t* input,
                                   size_t bytes) {
  for (size_t i = 0U; i < bytes; ++i) {
    hash ^= input[i];
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t gatewaySnapshotHash(const uint8_t* input, size_t bytes) {
  uint32_t hash = gatewaySnapshotHashUpdate(
      2166136261UL, input, bytes);
  return hash == 0U ? 1U : hash;
}

uint32_t gatewaySnapshotTruthHash(uint32_t now) {
  const kitsu868::connectivity::ConnectionConfigStatus config =
      connectionConfigStore.status();
  const kitsu868::connectivity::ConnectivityRuntimeStatus wifi =
      wifiRuntime.status(now);
  const uint8_t facts[] = {
      static_cast<uint8_t>(deviceSecurity.remoteConnectivityAllowed()),
      static_cast<uint8_t>(config.wifiConfigured),
      static_cast<uint8_t>(wifi.wifiState),
      static_cast<uint8_t>(config.gatewayConfigured),
      static_cast<uint8_t>(config.gatewayEnrolled),
      static_cast<uint8_t>(gatewayLanServiceState),
  };
  uint32_t hash = gatewaySnapshotHashUpdate(
      2166136261UL, facts, sizeof(facts));
  for (uint8_t slot = 0U;
       slot < kitsu868::mesh::kMeshChannelCapacity; ++slot) {
    kitsu868::mesh::ChannelRecord channel{};
    const bool configured =
        meshTransport.channelAt(slot, channel) && channel.configured;
    const uint8_t record[] = {
        slot, static_cast<uint8_t>(configured),
    };
    hash = gatewaySnapshotHashUpdate(hash, record, sizeof(record));
    if (configured) {
      const size_t nameBytes = strnlen(channel.name, sizeof(channel.name));
      hash = gatewaySnapshotHashUpdate(
          hash, reinterpret_cast<const uint8_t*>(channel.name), nameBytes);
    }
  }
  return hash == 0U ? 1U : hash;
}

bool buildGatewaySnapshot(String& output, uint32_t now) {
  const kitsu868::connectivity::ConnectionConfigStatus config =
      connectionConfigStore.status();
  const kitsu868::connectivity::ConnectivityRuntimeStatus wifi =
      wifiRuntime.status(now);
  output.reserve(768U);
  output = "{\"schema\":\"kitsu.companion-snapshot.v1\",";
  output += "\"firmware_version\":\"";
  output += FIRMWARE_VERSION;
  output += "\",\"remote_connectivity_allowed\":";
  output += deviceSecurity.remoteConnectivityAllowed() ? "true" : "false";
  output += ",\"wifi\":{\"configured\":";
  output += config.wifiConfigured ? "true" : "false";
  output += ",\"state\":\"";
  output += kitsu868::connectivity::wifiRuntimeStateName(wifi.wifiState);
  output += "\"},\"gateway\":{\"configured\":";
  output += config.gatewayConfigured ? "true" : "false";
  output += ",\"enrolled\":";
  output += config.gatewayEnrolled ? "true" : "false";
  output += ",\"lan_state\":\"";
  output += gatewayLanServiceStateName(gatewayLanServiceState);
  output += "\"},\"channels\":[";
  for (uint8_t slot = 0U;
       slot < kitsu868::mesh::kMeshChannelCapacity; ++slot) {
    if (slot != 0U) output += ',';
    kitsu868::mesh::ChannelRecord channel{};
    const bool available = meshTransport.channelAt(slot, channel);
    const bool configured = available && channel.configured;
    output += "{\"slot\":";
    output += String(slot);
    output += ",\"configured\":";
    output += configured ? "true" : "false";
    if (configured) {
      output += ",\"name\":\"";
      output += jsonEscaped(String(channel.name));
      output += '"';
    }
    output += ",\"max_utf8_bytes\":128}";
  }
  output += "]}";
  return output.length() != 0U &&
      output.length() <= kitsu868::connectivity::kLanMaximumDevicePayloadBytes;
}

void reportGatewayLanStatus() {
  if (gatewayLanReportInitialized &&
      gatewayLanReportedState == gatewayLanServiceState &&
      gatewayLanReportedResult == gatewayLanLastResult) {
    return;
  }
  gatewayLanReportInitialized = true;
  gatewayLanReportedState = gatewayLanServiceState;
  gatewayLanReportedResult = gatewayLanLastResult;
  gatewaySnapshotDirty = true;
  const kitsu868::connectivity::GatewayLanRuntimeStatus status =
      gatewayLanRuntime.status();
  Serial.printf(
      "KITSU_GATEWAY_LAN state=%s result=%s connected=%s queued=%u "
      "failures=%lu generation=%lu\n",
      gatewayLanServiceStateName(gatewayLanServiceState),
      kitsu868::connectivity::gatewayLanRuntimeResultName(
          gatewayLanLastResult),
      status.connected ? "true" : "false",
      static_cast<unsigned>(status.queuedFrames),
      static_cast<unsigned long>(status.consecutiveFailures),
      static_cast<unsigned long>(gatewayLanConfigGeneration));
}

void serviceGatewayLan(uint32_t now, bool authenticatedBleSession) {
  const kitsu868::connectivity::ConnectionConfigStatus configured =
      connectionConfigStore.status();
  const kitsu868::connectivity::ConnectivityRuntimeStatus wifi =
      wifiRuntime.status(now);
  int64_t epoch = 0;
  const bool timeValid = trustedGatewayWallClock(epoch);

  if (configured.generation != gatewayLanConfigGeneration) {
    const bool changed = gatewayLanConfigGeneration != 0U;
    stopGatewayLanRuntime();
    gatewayLanConfigGeneration = configured.generation;
    if (changed) {
      gatewayLanLastResult =
          kitsu868::connectivity::GatewayLanRuntimeResult::CredentialChanged;
    }
  }

  if (!configured.begun) {
    stopGatewayLanRuntime();
    gatewayLanServiceState = GatewayLanServiceState::ConfigUnavailable;
    gatewayLanLastResult =
        kitsu868::connectivity::GatewayLanRuntimeResult::NotBegun;
  } else if (!configured.gatewayConfigured) {
    stopGatewayLanRuntime();
    gatewayLanServiceState = GatewayLanServiceState::Unconfigured;
    gatewayLanLastResult =
        kitsu868::connectivity::GatewayLanRuntimeResult::NotBegun;
  } else if (!configured.gatewayEnrolled) {
    // No claim token or enrollment material is synthesized here. The owner
    // flow must complete authenticated+PRG enrollment before steady mTLS.
    stopGatewayLanRuntime();
    gatewayLanServiceState = GatewayLanServiceState::EnrollmentPending;
    gatewayLanLastResult =
        kitsu868::connectivity::GatewayLanRuntimeResult::NotBegun;
  } else if (!deviceSecurity.remoteConnectivityAllowed()) {
    stopGatewayLanRuntime();
    gatewayLanServiceState =
        GatewayLanServiceState::ConnectivityUnavailable;
    gatewayLanLastResult = kitsu868::connectivity::
        GatewayLanRuntimeResult::RemoteConnectivityUnavailable;
  } else if (!gatewayLanReplayReady || !gatewayLanActionsReady) {
    stopGatewayLanRuntime();
    gatewayLanServiceState = GatewayLanServiceState::ReplayUnavailable;
    gatewayLanLastResult =
        kitsu868::connectivity::GatewayLanRuntimeResult::ActionStoreFailed;
  } else if (authenticatedBleSession) {
    stopGatewayLanRuntime();
    gatewayLanServiceState = GatewayLanServiceState::BlePriority;
  } else if (!configured.gatewayLanConfigured) {
    // Mobile relay credentials are valid without inventing a LAN endpoint.
    // They become active only inside an authenticated BLE owner session.
    stopGatewayLanRuntime();
    gatewayLanServiceState = GatewayLanServiceState::Unconfigured;
    gatewayLanLastResult =
        kitsu868::connectivity::GatewayLanRuntimeResult::NotBegun;
  } else if (wifi.wifiState !=
             kitsu868::connectivity::WifiRuntimeState::Connected) {
    stopGatewayLanRuntime();
    gatewayLanServiceState = GatewayLanServiceState::WifiPending;
  } else if (!timeValid) {
    stopGatewayLanRuntime();
    gatewayLanServiceState = GatewayLanServiceState::TimePending;
    gatewayLanLastResult =
        kitsu868::connectivity::GatewayLanRuntimeResult::TimeUnavailable;
  } else {
    if (!gatewayLanBegun) {
      gatewayLanLastResult = gatewayLanRuntime.begin(
          connectionConfigStore, gatewayLanSequences, gatewayLanCrypto,
          gatewayLanReplayStore, gatewayLanActions, gatewayLanTls);
      gatewayLanBegun = gatewayLanLastResult ==
          kitsu868::connectivity::GatewayLanRuntimeResult::Ok;
      gatewayLanNextSnapshotAt = now;
    }
    if (gatewayLanBegun) {
      gatewayLanLastResult = gatewayLanRuntime.poll(now, epoch, true);
      const kitsu868::connectivity::GatewayLanRuntimeStatus status =
          gatewayLanRuntime.status();
      gatewayLanEverConnected = gatewayLanEverConnected || status.connected;
      gatewayLanServiceState = status.connected
          ? GatewayLanServiceState::Connected
          : GatewayLanServiceState::Reconnecting;

    } else {
      gatewayLanServiceState = GatewayLanServiceState::Reconnecting;
    }
  }

  // Detect every projected truth change without allocating/building JSON on
  // every loop. This includes Wi-Fi state changes that can occur while the
  // coarser gateway service state remains wifi_pending.
  const uint32_t observedSnapshotHash = gatewaySnapshotTruthHash(now);
  if (observedSnapshotHash != gatewayLanLastObservedSnapshotHash) {
    gatewayLanLastObservedSnapshotHash = observedSnapshotHash;
    gatewaySnapshotDirty = true;
  }
  const kitsu868::connectivity::GatewayLanRuntimeStatus lanStatus =
      gatewayLanRuntime.status();
  const bool relayActive = mobileRelayReady && authenticatedBleSession &&
      configured.mobileRelayConfigured && configured.gatewayEnrolled;
  const bool uplinkTransportReady = relayActive ||
      (gatewayLanBegun && lanStatus.connected && timeValid);
  const bool snapshotDue = gatewayLanNextSnapshotAt == 0U ||
      static_cast<int32_t>(now - gatewayLanNextSnapshotAt) >= 0;
  if (uplinkTransportReady && (snapshotDue || gatewaySnapshotDirty)) {
    String snapshot;
    if (buildGatewaySnapshot(snapshot, now)) {
      const uint8_t* bytes =
          reinterpret_cast<const uint8_t*>(snapshot.c_str());
      const size_t byteCount = snapshot.length();
      const uint32_t hash = gatewaySnapshotHash(bytes, byteCount);
      const bool changed = hash != gatewayLanLastSnapshotHash;
      if ((snapshotDue || changed || gatewaySnapshotDirty) &&
          gatewayLanPayloadQueue.canEnqueue(1U, byteCount) &&
          gatewayLanPayloadQueue.enqueue(
              "companion.snapshot", bytes, byteCount,
              timeValid ? epoch : 0)) {
        gatewayLanLastSnapshotHash = hash;
        gatewayLanNextSnapshotAt = now + GATEWAY_SNAPSHOT_INTERVAL_MS;
        gatewaySnapshotDirty = false;
      }
    }
  }
  reportGatewayLanStatus();
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
  delay(300);
  Serial.printf("\nKITSU_BOOT firmware=%s version=%s board=heltec-v3.2 tx_enabled=false\n",
                FIRMWARE_NAME, FIRMWARE_VERSION);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  rawButton = stableButton = digitalRead(PIN_BUTTON) == LOW;
  pinMode(PIN_BATTERY_CTRL, OUTPUT);
  digitalWrite(PIN_BATTERY_CTRL, HIGH);
  pinMode(PIN_BATTERY_ADC, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_2_5db);

  initDisplay();
  loadState();
  initConnectivityStorage();
  companionPack.begin();
  if (companionPack.valid() && collectiblePackId != companionPack.id()) {
    const bool replacingCompanion = collectiblePackId != 0;
    if (replacingCompanion) {
      // The device intentionally keeps one companion state. A real pack
      // replacement starts fresh; assigning the first v0.7 pack ID during
      // migration preserves the v0.6 Fox stats already on this board.
      wisp.energy = 72;
      wisp.curiosity = 14;
      wisp.affection = 5;
      wisp.pets = 0;
      wisp.sleeping = false;
      if (!kitsu868::CompanionBrain::clearStoredState()) {
        Serial.println("KITSU_WARN brain_reset=false");
      }
    }
    collectiblePackId = companionPack.id();
    unlockedTraits = 0;
    collectedGifts = 0;
    lastEncounterTrait = 0xff;
    lastEncounterGift = 0xff;
    saveState();
  }
  companionBrain.begin(wisp.uid.c_str(),
                       companionPack.valid() ? companionPack.id() : 0);
  companionBrain.syncSleeping(wisp.sleeping);
  initMesh();
  const bool bleReady = companionBle.begin();
  Serial.printf("KITSU_BLE_COMPANION ready=%s service=%s\n",
                bleReady ? "true" : "false",
                kitsu868::connectivity::kKitsuGattServiceUuid);
  chatSession = esp_random();
  if (chatSession == 0U) chatSession = 1U;
  sampleBattery(true);
  showHatchSequence();

  delay(300);
  lastEnergyTickAt = lastBrainMinuteAt = millis();
  lastInteractionAt = millis();
  cancelAmbientAnimation();
  const bool meetingNewPack = startMeetForNewPack();
  if (!meetingNewPack) startBaseAnimation();
  enterScreen(companionPack.valid() && wisp.sleeping && !meetingNewPack
                  ? Screen::Sleep
                  : Screen::Pet);
  printSelfTest();
  Serial.printf("KITSU_READY uid=%s companion=\"%s\" pack_valid=%s "
                "meshcore=1.17.1 profile=UK_EU_NARROW rx_ready=%s "
                "tx_unlocked=false\n",
                wisp.uid.c_str(), companionName().c_str(),
                companionPack.valid() ? "true" : "false",
                meshTransport.active() ? "true" : "false");
}

void loop() {
  pollButton();
  pollSerial();
  const uint32_t now = millis();
  companionBle.loop(now);
  kitsu868::connectivity::GatewayEnrollmentReceipt enrollmentTransition{};
  if (gatewayEnrollmentFlow.poll(now, &enrollmentTransition)) {
    (void)companionBle.sendEnrollmentEvent(enrollmentTransition);
  }
  const kitsu868::connectivity::BleSessionStatus bleSession =
      companionBle.sessionStatus(now);
  // An authenticated owner session has strict command/LAN priority, while the
  // ESP32-S3 keeps its STA association warm for observable provisioning and a
  // prompt BLE -> gateway handoff. MeshCore remains independent below.
  wifiRuntime.loop(now, bleSession.applicationAuthenticated);
  int64_t trustedEpoch = 0;
  if (!meshTransport.timeValid() &&
      trustedGatewayWallClock(trustedEpoch) && trustedEpoch > 0 &&
      trustedEpoch <= UINT32_MAX) {
    (void)meshTransport.setEpoch(static_cast<uint32_t>(trustedEpoch));
  }
  serviceGatewayEnrollment(now, bleSession.applicationAuthenticated);
  serviceGatewayLan(now, bleSession.applicationAuthenticated);
  meshTransport.loop();
  processMeshAdvert();
  processMeshMessages();
  tickCreature();
  tickProgression();
  tickGame();
  tickAnimation();
  sampleBattery();

  tickDiscoveryJournal(now);
  if (radioListening && static_cast<int32_t>(now - listenUntil) >= 0) {
    stopListening();
  }
  if ((screen == Screen::Menu || screen == Screen::Inbox ||
       screen == Screen::GameMenu ||
       screen == Screen::Status) &&
      !rawButton && !stableButton &&
      now - screenEnteredAt >= SCREEN_TIMEOUT_MS) {
    enterScreen(wisp.sleeping ? Screen::Sleep : Screen::Pet);
  }
  tickDisplayPower();
  renderDisplay();
  delay(2);
}
