#include "../src/portrait_ui_layout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using kitsu868::portrait::DotPlan;
using kitsu868::portrait::TextPlan;

namespace {

struct Label {
  const char* text;
  int16_t y;
  uint8_t preferredScale;
};

void assertLabel(const Label& label) {
  const TextPlan plan = kitsu868::portrait::planText(
      strlen(label.text), kitsu868::portrait::kContentWidth,
      label.preferredScale);
  assert(plan.valid);
  assert(plan.width <= kitsu868::portrait::kContentWidth);
  assert(kitsu868::portrait::centeredTextFitsVertically(label.y, plan));
  assert(plan.renderedCharacters ==
         plan.sourceCharacters +
             (plan.ellipsized ? kitsu868::portrait::kEllipsisCharacters : 0U));
}

template <size_t Count>
void assertScreen(const Label (&labels)[Count]) {
  for (size_t index = 0; index < Count; ++index) assertLabel(labels[index]);
}

void testTextPlanner() {
  TextPlan plan = kitsu868::portrait::planText(10U, 60, 1);
  assert(plan.valid && !plan.compact && !plan.ellipsized && plan.width == 59);

  plan = kitsu868::portrait::planText(12U, 60, 1);
  assert(plan.valid && plan.compact && !plan.ellipsized && plan.width == 60);

  // Six 5x7 digits use compact scale 2: prominent, complete, and exactly
  // contained in the 60 px portrait content area.
  plan = kitsu868::portrait::planText(6U, 60, 2);
  assert(plan.valid && plan.scale == 2U && plan.compact);
  assert(plan.width == 60 && !plan.ellipsized);

  plan = kitsu868::portrait::planText(26U, 60, 2);
  assert(plan.valid && plan.scale == 1U && plan.compact);
  assert(plan.ellipsized && plan.sourceCharacters == 10U);
  assert(plan.renderedCharacters == 12U && plan.width == 60);

  assert(kitsu868::portrait::lineCapacity(
             60, 1, kitsu868::portrait::regularAdvance(1)) == 10U);
  assert(kitsu868::portrait::lineCapacity(
             60, 1, kitsu868::portrait::compactAdvance(1)) == 12U);
  assert(!kitsu868::portrait::planText(1U, 4, 1).valid);
}

void testPetMissingPackAndHatch() {
  const Label pet[] = {{"LISTENING", 93, 1}};
  const Label missing[] = {
      {"NO", 32, 2}, {"PACK", 52, 2}, {"INSTALL", 82, 1}, {"USB", 97, 1}};
  const Label hatch[] = {
      {"SIGNAL", 20, 1}, {"(...)" , 56, 2}, {"FOUND", 101, 1}};
  const Label boot[] = {{"KITSU", 36, 2}, {"868", 65, 2}};
  assertScreen(pet);
  assertScreen(missing);
  assertScreen(hatch);
  assertScreen(boot);
  assert(kitsu868::portrait::rectangleFits(0, 20, 64, 64));
  assert(kitsu868::portrait::rectangleFits(15, 110, 34, 5));
  assert(kitsu868::portrait::rectangleFits(52, 3, 10, 7));
  assert(kitsu868::portrait::rectangleFits(58, 12, 4, 4));
}

void testMainAndGameMenus() {
  static const char* const mainItems[] = {
      "CONNECT", "FEED", "PLAY", "GAMES", "INBOX", "RADIO", "SLEEP",
      "INFO", "BACK"};
  for (const char* item : mainItems) assertLabel({item, 39, 2});
  assertLabel({"MENU", 8, 1});
  assertLabel({"TAP NEXT", 91, 1});
  assertLabel({"HOLD SELECT", 108, 1});

  const DotPlan mainDots = kitsu868::portrait::planDots(9, 60);
  assert(mainDots.valid && mainDots.width == 59);
  assert(kitsu868::portrait::rectangleFits(
      (64 - mainDots.width) / 2, 72, mainDots.width, mainDots.size));

  static const char* const gameItems[] = {"SIGNAL", "POUNCE", "BACK"};
  for (const char* item : gameItems) assertLabel({item, 39, 2});
  assertLabel({"GAMES", 8, 1});
  const DotPlan gameDots = kitsu868::portrait::planDots(3, 60);
  assert(gameDots.valid);
  assert(kitsu868::portrait::rectangleFits(
      (64 - gameDots.width) / 2, 72, gameDots.width, gameDots.size));
}

void testConnectVariants() {
  const Label labels[] = {
      {"CONNECT", 14, 1},      {"BLUETOOTH", 31, 2},
      {"CONTROLLERS", 31, 2}, {"BACK", 31, 2},
      {"UNAVAILABLE", 54, 1}, {"4 STORED", 54, 1},
      {"CONNECTED", 54, 1},   {"PAIRING", 54, 1},
      {"HOLD VIEW", 76, 1},   {"HOLD OPEN", 76, 1},
      {"HOLD MANAGE", 76, 1}, {"HOLD BACK", 76, 1},
      {"TAP NEXT", 108, 1},
  };
  assertScreen(labels);
  const DotPlan dots = kitsu868::portrait::planDots(3, 60);
  assert(dots.valid);
  assert(kitsu868::portrait::rectangleFits(
      (64 - dots.width) / 2, 94, dots.width, dots.size));
}

void testControllerRecoveryVariants() {
  const Label unavailable[] = {
      {"CONTROLLERS", 4, 1}, {"SECURITY", 31, 2},
      {"UNAVAILABLE", 54, 1}, {"NO CHANGES", 79, 1},
      {"TAP BACK", 108, 1}};
  const Label closing[] = {
      {"CONTROLLERS", 4, 1}, {"CLOSING BLE", 34, 1},
      {"WAIT", 55, 2}, {"NO CHANGES", 82, 1},
      {"TAP BACK", 108, 1}};
  const Label occupied[] = {
      {"CONTROLLERS", 4, 1}, {"SLOT 4", 28, 2},
      {"ID 01234567", 54, 1}, {"HOLD REMOVE", 78, 1},
      {"TAP NEXT", 108, 1}};
  const Label empty[] = {
      {"CONTROLLERS", 4, 1}, {"SLOT 4", 28, 2},
      {"EMPTY", 54, 2}, {"NO ACTION", 78, 1},
      {"TAP NEXT", 108, 1}};
  const Label reset[] = {
      {"CONTROLLERS", 4, 1}, {"RESET ALL", 31, 2},
      {"4 STORED", 56, 1}, {"HOLD SELECT", 78, 1},
      {"TAP NEXT", 108, 1}};
  const Label confirmSlot[] = {
      {"REMOVE", 4, 2}, {"SLOT 4", 23, 2},
      {"ID 01234567", 42, 1}, {"HOLD PRG", 58, 2},
      {"5S TO REMOVE", 76, 1}, {"TAP CANCEL", 95, 1},
      {"EXPIRES 15S", 109, 1}};
  const Label confirmAll[] = {
      {"REMOVE ALL", 4, 1}, {"4 STORED", 24, 1},
      {"KEEP HOLDING", 48, 1}, {"5S", 65, 2},
      {"TAP CANCEL", 95, 1}, {"EXPIRES 15S", 109, 1}};
  const Label success[] = {
      {"CONTROLLERS", 4, 1}, {"ALL REMOVED", 29, 2},
      {"4 CLEARED", 53, 1}, {"REOPEN PAIR", 79, 1},
      {"TAP CONTINUE", 108, 1}};
  const Label uncertain[] = {
      {"CONTROLLERS", 4, 1}, {"UNCERTAIN", 23, 2},
      {"STORAGE ERR", 43, 1}, {"REBOOT KITSU", 64, 2},
      {"BEFORE PAIR", 88, 1}, {"REBOOT NOW", 108, 1}};
  assertScreen(unavailable);
  assertScreen(closing);
  assertScreen(occupied);
  assertScreen(empty);
  assertScreen(reset);
  assertScreen(confirmSlot);
  assertScreen(confirmAll);
  assertScreen(success);
  assertScreen(uncertain);
  const DotPlan dots = kitsu868::portrait::planDots(6, 60);
  assert(dots.valid);
  assert(kitsu868::portrait::rectangleFits(
      (64 - dots.width) / 2, 94, dots.width, dots.size));
}

void testInboxVariants() {
  const Label empty[] = {
      {"INBOX", 4, 1}, {"QUIET", 39, 2}, {"NO MESSAGES", 68, 1},
      {"HOLD BACK", 108, 1}};
  const Label populated[] = {
      {"INBOX", 4, 1},
      {"A 32-BYTE UTF-8 NAME BECOMES SAFE", 15, 1},
      {"1234567890", 29, 1},
      {"1234567890", 39, 1},
      {"1234567890", 49, 1},
      {"1234567890", 59, 1},
      {"1234567890", 69, 1},
      {"12345678..", 79, 1},
      {"24/24", 91, 1},
      {"UNCONFIRMED", 103, 1},
      {"HOLD BACK", 116, 1},
  };
  assertScreen(empty);
  assertScreen(populated);
}

void testActiveGames() {
  const Label labels[] = {
      {"SIGNAL", 4, 1},       {"POUNCE", 4, 1},
      {"12/12", 18, 1},       {"PERFECT", 34, 1},
      {"DONE", 34, 2},        {"SCORE 65535", 96, 1},
      {"TAP", 113, 1},        {"WAIT", 113, 1},
      {"TAP HOME", 113, 1},
  };
  assertScreen(labels);
  assert(kitsu868::portrait::rectangleFits(5, 64, 11, 5));
  assert(kitsu868::portrait::rectangleFits(4, 56, 3, 7));
  assert(kitsu868::portrait::rectangleFits(3, 72, 58, 3));
  assert(kitsu868::portrait::rectangleFits(25, 64, 14, 9));
  assert(kitsu868::portrait::rectangleFits(3, 75, 58, 3));
}

void testListenAndSleep() {
  const Label listen[] = {
      {"LISTEN", 7, 1}, {"4294968S", 101, 2}};
  const Label sleep[] = {{"DREAMING", 99, 1}};
  assertScreen(listen);
  assertScreen(sleep);
  assert(kitsu868::portrait::rectangleFits(0, 25, 64, 64));
  assert(kitsu868::portrait::rectangleFits(0, 22, 64, 64));
}

void testStatusPages() {
  const Label identity[] = {
      {"A COMPANION PACK NAME MAY BE LONG", 6, 1},
      {"BOND 255", 25, 2},
      {"ASCENDED", 68, 1},
      {"ADVENTUROUS", 87, 1},
      {"T16 G12", 104, 1},
  };
  const Label vitals[] = {
      {"VITALS", 6, 2},       {"ENERGY 100", 33, 1},
      {"XP 4294967295", 50, 1}, {"BAT 100%", 67, 1},
      {"65535 MV", 84, 1},    {"2982616 DAYS", 101, 1},
  };
  const Label memory[] = {
      {"MEMORY", 6, 2}, {"A LONG MEMORY HEADING", 42, 2},
      {"A LONG MEMORY DETAIL", 65, 2}, {"#4294967295", 96, 1},
      {"SCORE 65535", 111, 1},
  };
  const Label diagnostics[] = {
      {"KITSU", 5, 2},   {"0.16.5", 27, 1}, {"KTFFFF", 43, 1},
      {"OLED ERR", 59, 1}, {"MESH ERR", 75, 1},
      {"STORE ERR", 91, 1}, {"NO PACK", 107, 1},
  };
  assertScreen(identity);
  assertScreen(vitals);
  assertScreen(memory);
  assertScreen(diagnostics);
  assert(kitsu868::portrait::rectangleFits(10, 49, 44, 5));
  const DotPlan dots = kitsu868::portrait::planDots(4, 60);
  assert(dots.valid);
  assert(kitsu868::portrait::rectangleFits(
      (64 - dots.width) / 2, 121, dots.width, dots.size));
}

void testPairPhoneVariants() {
  const Label unavailable[] = {
      {"PAIR PHONE", 4, 1}, {"BLE OFF", 39, 2},
      {"STORAGE", 72, 1}, {"TAP BACK", 106, 1}};
  const Label comparison[] = {
      {"PAIR PHONE", 4, 1}, {"MATCH CODE", 22, 1},
      {"999999", 42, 2}, {"HOLD IF SAME", 78, 1},
      {"TAP CANCEL", 105, 1}};
  const Label grant[] = {
      {"PAIR PHONE", 4, 1}, {"PHONE READY", 23, 1},
      {"HOLD", 44, 2}, {"PRG", 62, 2}, {"TO GRANT", 85, 1},
      {"TAP CANCEL", 105, 1}};
  const Label authenticated[] = {
      {"PAIR PHONE", 4, 1}, {"CONNECTED", 38, 1},
      {"APP VERIFIED", 70, 1}, {"TAP CLOSE", 105, 1}};
  const Label securing[] = {
      {"PAIR PHONE", 4, 1}, {"SECURING", 38, 1},
      {"WAIT", 64, 2}, {"TAP CANCEL", 105, 1}};
  const Label open[] = {
      {"PAIR PHONE", 4, 1}, {"OPEN", 27, 2},
      {"4294968S", 49, 2}, {"KTFFFF", 76, 1},
      {"TAP CANCEL", 105, 1}};
  const Label closed[] = {
      {"PAIR PHONE", 4, 1}, {"WINDOW", 29, 2},
      {"CLOSED", 48, 2}, {"HOLD REOPEN", 78, 1},
      {"TAP BACK", 106, 1}};
  assertScreen(unavailable);
  assertScreen(comparison);
  assertScreen(grant);
  assertScreen(authenticated);
  assertScreen(securing);
  assertScreen(open);
  assertScreen(closed);
}

}  // namespace

int main() {
  static_assert(kitsu868::portrait::kCanvasWidth == 64,
                "portrait width changed");
  static_assert(kitsu868::portrait::kCanvasHeight == 128,
                "portrait height changed");
  testTextPlanner();
  testPetMissingPackAndHatch();
  testMainAndGameMenus();
  testConnectVariants();
  testInboxVariants();
  testActiveGames();
  testListenAndSleep();
  testStatusPages();
  testPairPhoneVariants();
  testControllerRecoveryVariants();
  puts("PASS portrait_ui_layout_host");
  puts("  canvas: 64x128; content: 60px");
  puts("  screens: pet menu connect inbox game-menu game listen sleep status phone controller-recovery");
  puts("  phone states: unavailable compare grant authenticated securing open closed");
  return 0;
}
