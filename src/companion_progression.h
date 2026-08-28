#pragma once

#include <stddef.h>
#include <stdint.h>

#include "companion_dialogue.h"

// Persistent, deterministic companion progression. This module deliberately
// has no Arduino, storage, rendering, radio, or heap dependency. The caller
// supplies a local calendar-day serial and minute-of-day, then stores the
// versioned CRC snapshot through its existing Preferences backend.
namespace kitsu868 {
namespace progression {

constexpr uint8_t kActionCount = dialogue::kActionCount;
constexpr uint8_t kTimeBucketCount = 4U;
constexpr uint8_t kCallbackCapacity = 6U;
constexpr uint8_t kNicknameCapacity = 12U;
constexpr size_t kSnapshotCapacity = 384U;
constexpr uint8_t kNoAction = UINT8_MAX;
constexpr uint8_t kNoQuestion = UINT8_MAX;
constexpr uint8_t kNoHabit = UINT8_MAX;

enum class TimeBucket : uint8_t {
  Morning = 0,
  Day,
  Evening,
  Night,
};

enum class Season : uint8_t {
  Spring = 0,
  Summer,
  Autumn,
  Winter,
};

enum class GreetingKind : uint8_t {
  Normal = 0,
  MissedYou,
  LongAbsence,
  Comeback,
};

enum class CallbackKind : uint8_t {
  Event = 0,
  Dream,
  Expedition,
  Friend,
  PerfectDay,
  BondMilestone,
  Ritual,
  Anniversary,
};

enum class GoalKind : uint8_t {
  AnyCare = 0,
  Variety,
  Favorite,
};

enum class RequestState : uint8_t {
  None = 0,
  Pending,
  Accepted,
  Declined,
  Completed,
};

enum class QuestionKind : uint8_t {
  QuietOrPlay = 0,
  DawnOrNight,
  HomeOrExplore,
};

enum class ComfortKind : uint8_t {
  None = 0,
  Tired,
  Lonely,
  Restless,
};

enum class HabitKind : uint8_t {
  DawnPets = 0,
  LunchSnacks,
  EveningGames,
  NightListening,
  WakeUpCalls,
  GiftWatcher,
};

enum Achievement : uint32_t {
  AchievementFirstCallback = UINT32_C(1) << 0,
  AchievementFavoriteLearned = UINT32_C(1) << 1,
  AchievementRitual = UINT32_C(1) << 2,
  AchievementSecretHabit = UINT32_C(1) << 3,
  AchievementPerfectDay = UINT32_C(1) << 4,
  AchievementStreakSeven = UINT32_C(1) << 5,
  AchievementComeback = UINT32_C(1) << 6,
  AchievementDreamer = UINT32_C(1) << 7,
  AchievementExplorer = UINT32_C(1) << 8,
  AchievementFriendly = UINT32_C(1) << 9,
  AchievementAnniversary = UINT32_C(1) << 10,
  AchievementRareMoment = UINT32_C(1) << 11,
  AchievementVarietyFive = UINT32_C(1) << 12,
  AchievementHundredActions = UINT32_C(1) << 13,
};

enum LoreUnlock : uint16_t {
  LoreFirstMemory = UINT16_C(1) << 0,
  LoreFavorite = UINT16_C(1) << 1,
  LoreRitual = UINT16_C(1) << 2,
  LoreSecretHabit = UINT16_C(1) << 3,
  LoreDream = UINT16_C(1) << 4,
  LoreExpedition = UINT16_C(1) << 5,
  LoreFriend = UINT16_C(1) << 6,
  LorePerfectDay = UINT16_C(1) << 7,
  LoreBond = UINT16_C(1) << 8,
  LoreAnniversary = UINT16_C(1) << 9,
};

struct DisplayLine {
  const char* line1;
  const char* line2;

  constexpr DisplayLine(const char* first = "", const char* second = "")
      : line1(first), line2(second) {}
};

struct Callback {
  CallbackKind kind = CallbackKind::Event;
  uint8_t detail = 0U;
  uint32_t dueDay = 0U;
};

struct DailyGoal {
  GoalKind kind = GoalKind::AnyCare;
  dialogue::Action action = dialogue::Action::Pet;
  uint8_t target = 2U;
  uint8_t progress = 0U;

  bool complete() const { return progress >= target; }
};

struct PersonalBests {
  uint16_t dailyActions = 0U;
  uint8_t dailyVariety = 0U;
  uint8_t varietyChain = 0U;
  uint8_t careRhythm = 0U;
  uint16_t streak = 0U;
};

struct SessionResult {
  bool valid = false;
  GreetingKind greeting = GreetingKind::Normal;
  uint16_t absentDays = 0U;
  bool callbackReady = false;
  bool requestOffered = false;
  bool questionOffered = false;
  bool comebackDay = false;
  bool anniversary = false;
  bool seasonalMoment = false;
  bool rareMoment = false;
  bool previousDayPerfect = false;
  uint8_t weeklyChapter = 0U;
  Season season = Season::Spring;
  ComfortKind comfort = ComfortKind::None;
  DailyGoal goal{};
};

struct ActionResult {
  bool valid = false;
  uint8_t repeatCount = 0U;
  bool repeatNoticed = false;
  dialogue::Action followUp = dialogue::Action::Pet;
  bool followedSuggestion = false;
  bool favoriteChanged = false;
  bool ritualRecognized = false;
  bool secretHabitUnlocked = false;
  bool requestCompleted = false;
  bool goalCompletedNow = false;
  bool comforted = false;
  uint8_t varietyChain = 0U;
  uint8_t dailyVariety = 0U;
  uint8_t rhythmBonus = 0U;
  int8_t moodMomentum = 0;
  uint8_t speechStage = 0U;
  uint8_t bondDialogueBank = 0U;
};

struct RequestResult {
  bool valid = false;
  bool accepted = false;
  dialogue::Action requestedAction = dialogue::Action::Pet;
};

struct QuestionResult {
  bool valid = false;
  QuestionKind question = QuestionKind::QuietOrPlay;
  uint8_t choice = 0U;
};

class CompanionProgression {
 public:
  // firstDay is an arbitrary, monotonically increasing local calendar serial.
  void initialize(uint32_t companionFingerprint, uint32_t firstDay);

  // Starts/continues a local day and performs bounded rollover finalization.
  SessionResult startSession(uint32_t day, uint16_t minuteOfDay,
                             uint8_t bondLevel,
                             const CompanionVitals& vitals, Season season);

  // Records an authored companion action. Invalid enum/time/day input is
  // rejected without mutating state.
  ActionResult recordAction(dialogue::Action action, uint32_t day,
                            uint16_t minuteOfDay, uint8_t bondLevel);

  // Feature 1 and context-specific wrappers for features 16-18. Callbacks are
  // retained in a bounded FIFO and become readable on/after dueDay.
  bool rememberEvent(uint8_t eventId, uint32_t day, uint8_t delayDays = 1U);
  bool rememberDream(uint8_t dreamId, uint32_t day);
  bool rememberExpedition(uint8_t outcomeId, uint32_t day);
  bool rememberFriend(uint8_t friendDetail, uint32_t day);
  bool takeCallback(uint32_t day, Callback& callback);
  bool callbackReady(uint32_t day) const;

  RequestResult answerRequest(bool accept);
  QuestionResult answerQuestion(uint8_t choice);
  bool observeBond(uint8_t bondLevel, uint32_t day);

  bool setNickname(const char* nickname);
  const char* nickname() const;

  DailyGoal dailyGoal() const;
  RequestState requestState() const;
  dialogue::Action requestedAction() const;
  bool pendingQuestion(QuestionKind& question) const;
  bool preferredQuestionChoice(QuestionKind question, uint8_t& choice) const;
  ComfortKind comfortNeed() const;

  bool hasFavorite() const;
  dialogue::Action favoriteAction() const;
  bool preferredTime(dialogue::Action action, TimeBucket& bucket) const;
  bool recognizedRoutine(TimeBucket bucket, dialogue::Action& action) const;
  bool hasRitual() const;
  dialogue::Action ritualAction() const;
  TimeBucket ritualTime() const;
  uint8_t ritualStreak() const;
  bool hasSecretHabit() const;
  HabitKind secretHabit() const;

  uint8_t speechStage() const;
  uint8_t bondDialogueBank(uint8_t bondLevel) const;
  int8_t moodMomentum() const;
  uint16_t currentStreak() const;
  uint16_t perfectDays() const;
  uint32_t achievementMask() const;
  uint16_t loreMask() const;
  PersonalBests personalBests() const;
  uint32_t totalActions() const;
  uint16_t acceptedRequests() const;
  uint16_t declinedRequests() const;
  bool lastDayWasComeback() const;

  uint32_t dailySeed(uint32_t day) const;

  // Stable copy-out/copy-in contract. Restore rejects corruption, unknown
  // schemas, wrong companion fingerprints, and malformed enum/ring fields.
  static size_t snapshotSize();
  bool writeSnapshot(void* destination, size_t capacity) const;
  bool restoreSnapshot(const void* source, size_t length,
                       uint32_t expectedCompanionFingerprint);

  static TimeBucket timeBucket(uint16_t minuteOfDay);
  static DisplayLine greetingLine(GreetingKind greeting);
  static DisplayLine callbackLine(const Callback& callback);
  static DisplayLine requestLine(dialogue::Action action);
  static DisplayLine questionLine(QuestionKind question);
  static const char* questionOption(QuestionKind question, uint8_t choice);
  static DisplayLine seasonalLine(Season season);
  static DisplayLine rareMomentLine(uint32_t seed);
  static DisplayLine bondLine(uint8_t dialogueBank);
  static DisplayLine comfortLine(ComfortKind comfort);
  static DisplayLine speechLine(uint8_t speechStage);
  static DisplayLine habitLine(HabitKind habit);
  static DisplayLine weeklyChapterLine(uint8_t chapter);
  static DisplayLine loreLine(LoreUnlock lore);

 private:
  static constexpr uint32_t kMagic = UINT32_C(0x4B505231);  // "KPR1"
  static constexpr uint16_t kSchema = 1U;

#pragma pack(push, 1)
  struct CallbackRecord {
    uint32_t dueDay = 0U;
    uint8_t kind = 0U;
    uint8_t detail = 0U;
  };

  struct PersistedState {
    uint32_t magic = kMagic;
    uint16_t schema = kSchema;
    uint16_t bytes = 0U;
    uint32_t companionFingerprint = 0U;
    uint32_t firstDay = 0U;
    uint32_t currentDay = 0U;
    uint32_t lastSessionDay = 0U;
    uint32_t lastActionDay = 0U;
    uint32_t lastPerfectDay = 0U;
    uint32_t lastRareDay = 0U;
    uint32_t totalActions = 0U;
    uint32_t achievements = 0U;
    uint16_t lore = 0U;
    uint16_t activeDays = 0U;
    uint16_t currentStreak = 0U;
    uint16_t perfectDays = 0U;
    uint16_t acceptedRequests = 0U;
    uint16_t declinedRequests = 0U;
    uint16_t lastActionMinute = 0U;
    uint16_t dailyActions = 0U;
    uint16_t actionCounts[kActionCount]{};
    uint8_t actionBucketCounts[kActionCount][kTimeBucketCount]{};
    uint8_t dailyActionMask = 0U;
    uint8_t lastAction = kNoAction;
    uint8_t lastSuggestedAction = kNoAction;
    uint8_t repeatCount = 0U;
    uint8_t favoriteAction = kNoAction;
    int8_t moodMomentum = 0;
    uint8_t speechStage = 0U;
    uint8_t highestBondBank = 0U;
    uint8_t varietyChain = 0U;
    uint8_t dailyRhythm = 0U;
    uint8_t goalKind = static_cast<uint8_t>(GoalKind::AnyCare);
    uint8_t goalAction = 0U;
    uint8_t goalTarget = 2U;
    uint8_t goalProgress = 0U;
    uint8_t requestState = static_cast<uint8_t>(RequestState::None);
    uint8_t requestAction = 0U;
    uint8_t pendingQuestion = kNoQuestion;
    uint8_t questionPreferences = 0U;
    uint8_t comfort = static_cast<uint8_t>(ComfortKind::None);
    uint8_t ritualAction = kNoAction;
    uint8_t ritualBucket = 0U;
    uint8_t ritualStreak = 0U;
    uint32_t ritualLastDay = 0U;
    uint8_t secretHabit = kNoHabit;
    uint8_t callbackHead = 0U;
    uint8_t callbackCount = 0U;
    CallbackRecord callbacks[kCallbackCapacity]{};
    char nickname[kNicknameCapacity + 1U]{};
    uint8_t graceUsed = 0U;
    uint8_t dailyComeback = 0U;
    uint8_t rareShownToday = 0U;
    uint8_t anniversaryShownToday = 0U;
    uint8_t weeklySeenMask = 0U;
    uint8_t reservedFlags = 0U;
    PersonalBests bests{};
    uint32_t crc32 = 0U;
  };
#pragma pack(pop)

  static uint32_t mix32(uint32_t value);
  static uint32_t calculateCrc(const PersistedState& state);
  static uint8_t popcount8(uint8_t value);
  static uint8_t saturatingIncrement8(uint8_t value);
  static uint16_t saturatingIncrement16(uint16_t value);
  static uint32_t saturatingIncrement32(uint32_t value);
  static int8_t clampMomentum(int16_t value);
  static bool validAction(uint8_t action);

  bool validState(const PersistedState& state,
                  uint32_t expectedFingerprint) const;
  bool moveToDay(uint32_t day, bool& previousPerfect);
  bool finalizeActiveDay();
  void beginFreshDay(uint32_t day);
  void missDay();
  void updateFavorite(bool& changed);
  void updateSpeech(uint8_t bondLevel);
  void updateLoreAndAchievements();
  bool enqueueCallback(CallbackKind kind, uint8_t detail, uint32_t dueDay);
  void updateRitual(dialogue::Action action, TimeBucket bucket, uint32_t day,
                    bool& recognizedNow);
  void updateSecretHabit(bool& unlockedNow);
  void updateComfortForAction(dialogue::Action action, bool& comforted);
  dialogue::Action chooseRequest(const CompanionVitals& vitals,
                                 uint32_t seed) const;
  DailyGoal goalFromState() const;

  PersistedState state_{};
};

static_assert(sizeof(PersonalBests) <= 8U,
              "Personal-best state unexpectedly grew");

}  // namespace progression
}  // namespace kitsu868
