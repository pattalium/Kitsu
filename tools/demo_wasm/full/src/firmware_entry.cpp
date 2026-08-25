// Browser entry translation unit for the unmodified production firmware.
// Including main.cpp keeps the browser-only ABI in this translation unit, so
// it can expose read-only diagnostics without adding host hooks to the
// shipping ESP32 source. All mutable browser input still crosses a production
// interface: raw GPIO, serial, BLE frames, flash, or raw radio packets.
#include "main.cpp"

namespace {

struct EmulatorDebugV1 {
  uint32_t abiVersion;
  uint32_t bytes;
  uint32_t firmwareMillis;
  uint32_t screen;
  uint32_t menuIndex;
  uint32_t gameMenuIndex;
  uint32_t statusPage;
  uint32_t connectionAction;
  uint32_t activeGame;
  uint32_t gamePhase;
  uint32_t gameResult;
  uint32_t gameRound;
  uint32_t gameTotalRounds;
  uint32_t gameScore;
  uint32_t gameRewarded;
  uint32_t radioListening;
  uint32_t listenRemainingMs;
  uint32_t sleeping;
  uint32_t inboxSelection;
  uint32_t inboxCount;
  uint32_t unreadCount;
  uint32_t controllerSelection;
  uint32_t controllerTargetSlot;
  uint32_t controllerResult;
  uint32_t rawButton;
  uint32_t stableButton;
  uint32_t buttonHoldConsumed;
  uint32_t energy;
  uint32_t curiosity;
  uint32_t affection;
  uint32_t bondLevel;
  uint32_t bondXp;
  uint32_t packValid;
  uint32_t packId;
  uint32_t packRevision;
  uint32_t displaySleeping;
  uint32_t meshEnabled;
  uint32_t meshActive;
  uint32_t radioReady;
  uint32_t bootCount;
};

static_assert(sizeof(EmulatorDebugV1) == 40U * sizeof(uint32_t),
              "browser debug ABI layout changed");

EmulatorDebugV1 emulatorDebug{};

void updateGameDebug(EmulatorDebugV1& output, uint32_t nowMillis) {
  if (activeGame == ActiveGame::SignalCatch) {
    const kitsu868::SignalCatchView view = signalCatchGame.view(nowMillis);
    output.gamePhase = static_cast<uint32_t>(view.phase);
    output.gameResult = static_cast<uint32_t>(view.result);
    output.gameRound = view.round;
    output.gameTotalRounds = view.totalRounds;
    output.gameScore = view.score;
  } else if (activeGame == ActiveGame::PounceFetch) {
    const kitsu868::PounceFetchView view = pounceFetchGame.view(nowMillis);
    output.gamePhase = static_cast<uint32_t>(view.phase);
    output.gameResult = static_cast<uint32_t>(view.result);
    output.gameRound = view.round;
    output.gameTotalRounds = view.totalRounds;
    output.gameScore = view.score;
  }
}

}  // namespace

extern "C" const uint32_t* kitsu_emulator_debug_view() {
  const uint32_t nowMillis = millis();
  emulatorDebug = EmulatorDebugV1{};
  emulatorDebug.abiVersion = 1U;
  emulatorDebug.bytes = sizeof(EmulatorDebugV1);
  emulatorDebug.firmwareMillis = nowMillis;
  emulatorDebug.screen = static_cast<uint32_t>(screen);
  emulatorDebug.menuIndex = menuIndex;
  emulatorDebug.gameMenuIndex = gameMenuIndex;
  emulatorDebug.statusPage = statusPage;
  emulatorDebug.connectionAction = static_cast<uint32_t>(connectionAction);
  emulatorDebug.activeGame = static_cast<uint32_t>(activeGame);
  emulatorDebug.gameRewarded = gameRewarded ? 1U : 0U;
  emulatorDebug.radioListening = radioListening ? 1U : 0U;
  emulatorDebug.listenRemainingMs =
      radioListening && static_cast<int32_t>(listenUntil - nowMillis) > 0
          ? listenUntil - nowMillis
          : 0U;
  emulatorDebug.sleeping = wisp.sleeping ? 1U : 0U;
  emulatorDebug.inboxSelection = inboxSelection;
  emulatorDebug.inboxCount = chatJournalCount;
  emulatorDebug.unreadCount = unreadChatMessages;
  emulatorDebug.controllerSelection = controllerRecoverySelection;
  emulatorDebug.controllerTargetSlot = controllerRecoveryTargetSlot;
  emulatorDebug.controllerResult =
      static_cast<uint32_t>(controllerRecoveryResult);
  emulatorDebug.rawButton = rawButton ? 1U : 0U;
  emulatorDebug.stableButton = stableButton ? 1U : 0U;
  emulatorDebug.buttonHoldConsumed = buttonHoldConsumed ? 1U : 0U;
  emulatorDebug.energy = wisp.energy;
  emulatorDebug.curiosity = wisp.curiosity;
  emulatorDebug.affection = wisp.affection;
  emulatorDebug.bondLevel = companionBrain.bondLevel();
  emulatorDebug.bondXp = companionBrain.bondXp();
  emulatorDebug.packValid = companionPack.valid() ? 1U : 0U;
  emulatorDebug.packId = companionPack.id();
  emulatorDebug.packRevision = companionPack.revision();
  emulatorDebug.displaySleeping = displaySleeping ? 1U : 0U;
  emulatorDebug.meshEnabled = meshSettings.enabled ? 1U : 0U;
  emulatorDebug.meshActive = meshTransport.active() ? 1U : 0U;
  emulatorDebug.radioReady = radioReady ? 1U : 0U;
  emulatorDebug.bootCount = wisp.boots;
  updateGameDebug(emulatorDebug, nowMillis);
  return reinterpret_cast<const uint32_t*>(&emulatorDebug);
}

extern "C" uint32_t kitsu_emulator_debug_view_bytes() {
  return sizeof(EmulatorDebugV1);
}
