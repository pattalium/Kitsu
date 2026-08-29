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

void assertCompleteLabel(const Label& label) {
  assertLabel(label);
  const size_t characters = strlen(label.text);
  const TextPlan plan = kitsu868::portrait::planText(
      characters, kitsu868::portrait::kContentWidth, label.preferredScale);
  assert(!plan.ellipsized);
  assert(plan.sourceCharacters == characters);
  assert(plan.renderedCharacters == characters);
}

template <size_t Count>
void assertScreen(const Label (&labels)[Count]) {
  for (size_t index = 0; index < Count; ++index) assertLabel(labels[index]);
}

template <size_t Count>
void assertCompleteScreen(const Label (&labels)[Count]) {
  for (size_t index = 0; index < Count; ++index) {
    assertCompleteLabel(labels[index]);
  }
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
  const Label wrappedMoment[] = {
      {"RESONANT", 97, 1}, {"SIGNAL", 105, 1},
      {"EVERYONE", 113, 1}, {"SYNCED", 121, 1}};
  const Label missing[] = {
      {"NO", 32, 2}, {"PACK", 52, 2}, {"INSTALL", 82, 1}, {"USB", 97, 1}};
  const Label hatch[] = {
      {"SIGNAL", 20, 1}, {"(...)" , 56, 2}, {"FOUND", 101, 1}};
  const Label boot[] = {{"KITSU", 36, 2}, {"868", 65, 2}};
  assertScreen(pet);
  assertScreen(wrappedMoment);
  assertScreen(missing);
  assertScreen(hatch);
  assertScreen(boot);
  assert(kitsu868::portrait::rectangleFits(0, 20, 64, 64));
  assert(kitsu868::portrait::rectangleFits(15, 110, 34, 5));
  assert(kitsu868::portrait::rectangleFits(52, 3, 10, 7));
  assert(kitsu868::portrait::rectangleFits(58, 12, 4, 4));
  assert(kitsu868::portrait::lineCapacity(
             kitsu868::portrait::kContentWidth, 1,
             kitsu868::portrait::compactAdvance(1)) == 12U);
  assert(kitsu868::portrait::rectangleFits(2, 97, 60, 31));
}

void testMainAndGameMenus() {
  static const char* const mainItems[] = {
      "CONNECT", "FEED", "PLAY", "GAMES", "ADVENTURE", "CREATURES",
      "GOALS", "INBOX", "RADIO", "CLOCK", "SLEEP", "INFO", "BACK"};
  for (const char* item : mainItems) assertCompleteLabel({item, 39, 2});
  assertCompleteLabel({"MENU", 8, 1});
  assertCompleteLabel({"TAP NEXT", 91, 1});
  assertCompleteLabel({"HOLD SELECT", 108, 1});

  const DotPlan mainDots = kitsu868::portrait::planDots(13, 60);
  assert(mainDots.valid);
  assert(kitsu868::portrait::rectangleFits(
      (64 - mainDots.width) / 2, 72, mainDots.width, mainDots.size));

  static const char* const gameItems[] = {
      "SIGNAL", "POUNCE", "ECHO", "MORSE", "TUNER", "FLASH",
      "STEADY", "BREATHE", "DAILY", "GHOST", "BACK"};
  for (const char* item : gameItems) assertCompleteLabel({item, 39, 2});
  assertCompleteLabel({"GAMES", 8, 1});
  const DotPlan gameDots = kitsu868::portrait::planDots(11, 60);
  assert(gameDots.valid);
  assert(kitsu868::portrait::rectangleFits(
      (64 - gameDots.width) / 2, 72, gameDots.width, gameDots.size));
}

void testClockEditorVariants() {
  const Label inactive[] = {
      {"SET CLOCK", 5, 2}, {"EDITOR OFF", 47, 2}, {"HOLD BACK", 104, 1}};
  const Label fields[] = {
      {"YEAR", 27, 1},       {"MONTH", 27, 1},
      {"DAY", 27, 1},        {"HOUR", 27, 1},
      {"MINUTE", 27, 1},     {"UTC OFFSET", 27, 1},
      {"REVIEW", 27, 1},     {"2100-12-31", 45, 1},
      {"23:59", 61, 2},      {"UTC -14:00", 83, 1},
      {"TAP CHANGE", 101, 1}, {"HOLD SAVE", 101, 1},
      {"HOLD NEXT", 114, 1}, {"TAP REVISE", 114, 1},
  };
  assertCompleteScreen(inactive);
  assertCompleteScreen(fields);
}

void testAdventureVariants() {
  static const char* const adventureItems[] = {
      "SHORT TRIP", "MEDIUM TRIP", "LONG TRIP", "START ROUTE",
      "+250 STEPS", "CONTINUE", "DETOUR", "HELP", "RETURN EARLY",
      "TERRAIN", "OBJECTIVE", "RISK", "WEATHER", "REPORT", "BACK"};
  for (const char* item : adventureItems) {
    assertCompleteLabel({item, 33, 1});
    assertCompleteLabel({item, 98, 1});
  }

  const Label traveling[] = {
      {"ADVENTURE", 3, 2}, {"LONG", 27, 1}, {"100%", 62, 2},
      {"480 MIN LEFT", 86, 1}, {"HOLD MENU", 110, 1}};
  const Label ready[] = {
      {"ADVENTURE", 3, 2}, {"TRIP BACK", 31, 1},
      {"REPORT READY", 52, 2}, {"HOLD CLAIM", 91, 1},
      {"TAP MENU", 108, 1}};
  const Label active[] = {
      {"NEEDS PARTY", 25, 1}, {"WATER", 25, 1},
      {"STEPS 100%", 61, 1}, {"CHOICES 3/3", 79, 1},
      {"TAP/HOLD", 113, 1}};
  const Label returned[] = {
      {"ROUTE DONE", 25, 1}, {"RESCUED", 25, 1},
      {"ROUTE BACK", 25, 1}, {"TAP MENU", 98, 1},
      {"HOLD ACK", 113, 1}};
  const Label settings[] = {
      {"MEADOW", 50, 1}, {"COMMUNITY", 64, 1}, {"BALANCED", 78, 1},
      {"WATER", 56, 2}, {"CREATURE", 56, 2}, {"CAREFUL", 56, 2},
      {"UNKNOWN", 56, 2}, {"B/UNKNOWN", 78, 1},
      {"TAP NEXT", 104, 1}, {"HOLD SELECT", 116, 1}};
  const Label postcardTitles[] = {
      {"MEADOW NOTE", 46, 1}, {"FOREST NOTE", 46, 1},
      {"HIGH NOTE", 46, 1}, {"TOWN NOTE", 46, 1},
      {"HALF A TRAIL", 46, 1}, {"SMALL DETOUR", 46, 1},
      {"EARLY POST", 46, 1}, {"SAFE RETURN", 46, 1},
      {"RESCUE ECHO", 46, 1}, {"PARTY RESCUE", 46, 1}};
  assertCompleteScreen(traveling);
  assertCompleteScreen(ready);
  assertCompleteScreen(active);
  assertCompleteScreen(returned);
  assertCompleteScreen(settings);
  assertCompleteScreen(postcardTitles);
  assert(kitsu868::portrait::rectangleFits(10, 48, 44, 5));
  assert(kitsu868::portrait::rectangleFits(10, 46, 44, 5));
  assert(kitsu868::portrait::rectangleFits(2, 61, 60, 30));
  const DotPlan dots = kitsu868::portrait::planDots(15, 60);
  assert(dots.valid);
  assert(kitsu868::portrait::rectangleFits(
      (64 - dots.width) / 2, 91, dots.width, dots.size));
}

void testActivityVariants() {
  static const char* const activityNames[] = {
      "MORSE", "TUNER", "FLASH", "STEADY", "BREATHE"};
  for (const char* name : activityNames) assertCompleteLabel({name, 4, 2});
  const Label presenting[] = {
      {"WATCH", 31, 1}, {"DASH", 51, 2}, {"DOT", 51, 2},
      {"8/8", 76, 1}, {"THEN REPEAT", 102, 1}};
  const Label playing[] = {
      {"REPEAT", 29, 1}, {"8/8", 49, 2}, {"TAP DOT", 78, 1},
      {"HOLD DASH", 96, 1}, {"TARGET 100", 28, 1},
      {"SIGNAL 100", 67, 1}, {"TAP TO LOCK", 97, 1},
      {"NOW!", 39, 2}, {"WAIT", 39, 2}, {"TAP", 79, 1},
      {"HANDS OFF", 79, 1}, {"HOLD PRG", 32, 2},
      {"RELEASE 100", 87, 1}, {"BREATHE IN", 30, 1},
      {"BREATHE OUT", 30, 1}, {"TAP AT PEAK", 76, 1},
      {"3/3", 97, 2}};
  const Label result[] = {
      {"SCORE", 31, 1}, {"1000", 51, 2}, {"BEST 1000", 78, 1},
      {"NICE SIGNAL", 101, 1}, {"FINISHED", 35, 2},
      {"1000/1000", 62, 1}, {"TAP BACK", 100, 1},
      {"NO ACTIVITY", 44, 2}};
  assertCompleteScreen(presenting);
  assertCompleteScreen(playing);
  assertCompleteScreen(result);
  const int16_t progressBars[] = {46, 52, 54, 63};
  for (int16_t y : progressBars) {
    assert(kitsu868::portrait::rectangleFits(10, y, 44, 5));
  }
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
      {"SIGNAL", 4, 1},       {"POUNCE", 4, 1}, {"ECHO", 4, 1},
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
  assert(kitsu868::portrait::rectangleFits(19, 45, 26, 26));
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

void testWildEncounterVariants() {
  const Label unavailable[] = {
      {"WILD ENCOUNTER", 2, 1},
      {"SIGNAL LOST", 48, 2},
      {"TAP OR HOLD", 108, 1},
  };
  const Label revealed[] = {
      {"TASMANIAN", 0, 1},
      {"DEVIL", 8, 1},
      {"VERY RARE", 99, 1},
      {"CODE FOUND", 109, 1},
      {"TAP OR HOLD", 119, 1},
  };
  const Label hidden[] = {
      {"NO CODE", 109, 1},
  };
  assertScreen(unavailable);
  assertScreen(revealed);
  assertScreen(hidden);
  assert(kitsu868::portrait::rectangleFits(0, 17, 64, 80));
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
      {"KITSU", 5, 2},   {"0.20.0", 27, 1}, {"KTFFFF", 43, 1},
      {"OLED ERR", 59, 1}, {"MESH ERR", 75, 1},
      {"STORE ERR", 91, 1}, {"NO PACK", 107, 1},
  };
  const Label rewards[] = {
      {"REWARDS", 6, 2}, {"DREAMS 65535", 25, 1},
      {"WOKE INSIDE", 39, 1}, {"A GOOD DREAM", 50, 1},
      {"DREAM #12", 45, 1}, {"NO DREAMS", 45, 1},
      {"R65535 C21", 65, 1}, {"SESSION AURA", 82, 1},
      {"FINAL FORM", 99, 1},
  };
  const Label progressionHeader[] = {
      {"A NICKNAME MAY BE LONG", 5, 2}, {"PROGRESSION", 5, 2}};
  const Label progressionValues[] = {
      {"STREAK 65535", 28, 1}, {"GOAL 255/255", 45, 1},
      {"LORE 10/10", 62, 1}, {"FRIENDS 12", 79, 1},
      {"P 4294967295", 96, 1}, {"PB4294967295", 111, 1}};
  assertScreen(identity);
  assertScreen(vitals);
  assertScreen(memory);
  assertScreen(rewards);
  assertScreen(progressionHeader);
  assertCompleteScreen(progressionValues);
  assertScreen(diagnostics);
  assert(kitsu868::portrait::rectangleFits(10, 49, 44, 5));
  const DotPlan dots = kitsu868::portrait::planDots(6, 60);
  assert(dots.valid);
  assert(kitsu868::portrait::rectangleFits(
      (64 - dots.width) / 2, 121, dots.width, dots.size));
}

void testFieldGuideAndGoals() {
  const Label unseen[] = {
      {"FIELD GUIDE", 2, 1}, {"???", 14, 1},
      {"UNDISCOVERED", 66, 1}, {"FOLLOW SIGNALS", 80, 1},
      {"NOT OWNED", 94, 1}, {"ROSTER 21/21", 106, 1},
      {"TRAIL 20/20", 118, 1},
  };
  const Label seen[] = {
      {"TASMANIAN", 0, 1}, {"DEVIL", 8, 1},
      {"VERY RARE", 99, 1},
      {"NOT OWNED", 109, 1}, {"21/21 S65535", 119, 1},
  };
  const Label goals[] = {
      {"SESSION GOALS", 5, 1}, {"CARE 2/2", 29, 1},
      {"GAME 1/1", 47, 1}, {"SIGNAL 1/1", 65, 1},
      {"AURA ACTIVE", 87, 2}, {"HOLD BACK", 113, 1},
  };
  assertScreen(unseen);
  assertScreen(seen);
  assertScreen(goals);
  assert(kitsu868::portrait::rectangleFits(0, 17, 64, 80));
  assert(kitsu868::portrait::rectangleFits(27, 30, 10, 10));
  assert(kitsu868::portrait::rectangleFits(22, 40, 20, 20));
  assert(kitsu868::portrait::rectangleFits(18, 48, 28, 8));
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
  testClockEditorVariants();
  testAdventureVariants();
  testActivityVariants();
  testConnectVariants();
  testInboxVariants();
  testActiveGames();
  testListenAndSleep();
  testWildEncounterVariants();
  testStatusPages();
  testPairPhoneVariants();
  testControllerRecoveryVariants();
  testFieldGuideAndGoals();
  puts("PASS portrait_ui_layout_host");
  puts("  canvas: 64x128; content: 60px");
  puts("  screens: pet menu connect inbox game-menu game listen sleep wild-encounter field-guide goals clock adventure activity status phone controller-recovery");
  puts("  phone states: unavailable compare grant authenticated securing open closed");
  return 0;
}
