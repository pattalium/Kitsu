#include "companion_progression.h"

#include <stddef.h>
#include <string.h>

namespace kitsu868 {
namespace progression {
namespace {

constexpr uint8_t kSeasonalShown = 1U << 0;

uint32_t addDaysSaturated(uint32_t day, uint8_t delay) {
  if (UINT32_MAX - day < static_cast<uint32_t>(delay)) return UINT32_MAX;
  return day + static_cast<uint32_t>(delay);
}

uint8_t actionIndex(dialogue::Action action) {
  return static_cast<uint8_t>(action);
}

dialogue::Action actionFromIndex(uint8_t action) {
  return static_cast<dialogue::Action>(action);
}

uint8_t bucketIndex(TimeBucket bucket) {
  return static_cast<uint8_t>(bucket);
}

}  // namespace

void CompanionProgression::initialize(uint32_t companionFingerprint,
                                      uint32_t firstDay) {
  state_ = PersistedState{};
  state_.magic = kMagic;
  state_.schema = kSchema;
  state_.bytes = static_cast<uint16_t>(sizeof(PersistedState));
  state_.companionFingerprint = companionFingerprint;
  state_.firstDay = firstDay;
  state_.currentDay = firstDay;
  state_.lastAction = kNoAction;
  state_.lastSuggestedAction = kNoAction;
  state_.favoriteAction = kNoAction;
  state_.pendingQuestion = kNoQuestion;
  state_.ritualAction = kNoAction;
  state_.secretHabit = kNoHabit;
}

SessionResult CompanionProgression::startSession(
    uint32_t day, uint16_t minuteOfDay, uint8_t bondLevel,
    const CompanionVitals& vitals, Season season) {
  SessionResult result{};
  if (minuteOfDay >= 1440U || static_cast<uint8_t>(season) >
                                  static_cast<uint8_t>(Season::Winter)) {
    return result;
  }

  const bool hadPriorSession = state_.activeDays != 0U;
  const uint32_t priorSessionDay = state_.lastSessionDay;
  bool previousPerfect = false;
  if (!moveToDay(day, previousPerfect)) return result;

  uint32_t absent = 0U;
  if (hadPriorSession && day > priorSessionDay) {
    absent = day - priorSessionDay - 1U;
  }
  result.absentDays = absent > UINT16_MAX ? UINT16_MAX
                                          : static_cast<uint16_t>(absent);
  if (absent >= 7U) {
    result.greeting = GreetingKind::LongAbsence;
  } else if (absent >= 3U) {
    result.greeting = GreetingKind::Comeback;
  } else if (absent != 0U) {
    result.greeting = GreetingKind::MissedYou;
  }

  result.comebackDay = absent >= 3U;
  if (result.comebackDay) {
    state_.dailyComeback = 1U;
    state_.achievements |= AchievementComeback;
  }

  const uint32_t seed = dailySeed(day);
  const bool requestWasEmpty =
      state_.requestState == static_cast<uint8_t>(RequestState::None);
  if (requestWasEmpty) {
    state_.requestAction = actionIndex(chooseRequest(vitals, seed));
    state_.requestState = static_cast<uint8_t>(RequestState::Pending);
  }
  result.requestOffered = requestWasEmpty;
  result.questionOffered = state_.pendingQuestion != kNoQuestion;

  if (vitals.sleeping || vitals.energy < 20U) {
    state_.comfort = static_cast<uint8_t>(ComfortKind::Tired);
  } else if (vitals.affection < 15U && absent != 0U) {
    state_.comfort = static_cast<uint8_t>(ComfortKind::Lonely);
  } else if (vitals.curiosity > 85U) {
    state_.comfort = static_cast<uint8_t>(ComfortKind::Restless);
  } else {
    state_.comfort = static_cast<uint8_t>(ComfortKind::None);
  }

  const uint32_t ageDays = day >= state_.firstDay ? day - state_.firstDay : 0U;
  if (ageDays >= 365U && (ageDays % 365U) == 0U &&
      state_.anniversaryShownToday == 0U) {
    state_.anniversaryShownToday = 1U;
    state_.achievements |= AchievementAnniversary;
    state_.lore |= LoreAnniversary;
    enqueueCallback(CallbackKind::Anniversary,
                    static_cast<uint8_t>((ageDays / 365U) & 0xffU), day);
    result.anniversary = true;
  }

  if ((state_.reservedFlags & kSeasonalShown) == 0U &&
      ((seed >> 5U) & 3U) == static_cast<uint8_t>(season)) {
    state_.reservedFlags |= kSeasonalShown;
    result.seasonalMoment = true;
  }

  if (state_.rareShownToday == 0U && (seed % 17U) == 0U) {
    state_.rareShownToday = 1U;
    state_.lastRareDay = day;
    state_.achievements |= AchievementRareMoment;
    result.rareMoment = true;
  }

  observeBond(bondLevel, day);
  state_.lastSessionDay = day;
  result.callbackReady = callbackReady(day);
  result.previousDayPerfect = previousPerfect;
  result.weeklyChapter = static_cast<uint8_t>(ageDays % 7U);
  state_.weeklySeenMask |= static_cast<uint8_t>(
      1U << static_cast<uint8_t>(result.weeklyChapter));
  result.season = season;
  result.comfort = static_cast<ComfortKind>(state_.comfort);
  result.goal = goalFromState();
  result.valid = true;
  updateLoreAndAchievements();
  return result;
}

ActionResult CompanionProgression::recordAction(dialogue::Action action,
                                                uint32_t day,
                                                uint16_t minuteOfDay,
                                                uint8_t bondLevel) {
  ActionResult result{};
  const uint8_t index = actionIndex(action);
  if (!validAction(index) || minuteOfDay >= 1440U) return result;

  bool ignoredPerfect = false;
  if (!moveToDay(day, ignoredPerfect)) return result;

  const uint8_t previousAction = state_.lastAction;
  const bool sameDay =
      validAction(previousAction) && state_.lastActionDay == day;
  result.followedSuggestion = state_.lastSuggestedAction == index;
  if (sameDay && previousAction == index) {
    state_.repeatCount = saturatingIncrement8(state_.repeatCount);
  } else {
    state_.repeatCount = 1U;
  }
  result.repeatCount = state_.repeatCount;
  result.repeatNoticed = state_.repeatCount >= 3U;

  static const dialogue::Action kFollowUps[kActionCount] = {
      dialogue::Action::Play,   dialogue::Action::Listen,
      dialogue::Action::Pet,    dialogue::Action::Gift,
      dialogue::Action::Wake,   dialogue::Action::Feed,
      dialogue::Action::Listen, dialogue::Action::Pet,
  };
  result.followUp = kFollowUps[index];
  state_.lastSuggestedAction = actionIndex(result.followUp);

  if (sameDay && previousAction != index) {
    state_.varietyChain = saturatingIncrement8(state_.varietyChain);
  } else if (!sameDay || previousAction == index) {
    state_.varietyChain = 1U;
  }
  result.varietyChain = state_.varietyChain;

  const TimeBucket bucket = timeBucket(minuteOfDay);
  state_.actionCounts[index] = saturatingIncrement16(state_.actionCounts[index]);
  uint8_t& bucketCount = state_.actionBucketCounts[index][bucketIndex(bucket)];
  bucketCount = saturatingIncrement8(bucketCount);
  state_.totalActions = saturatingIncrement32(state_.totalActions);
  state_.dailyActions = saturatingIncrement16(state_.dailyActions);
  state_.dailyActionMask |= static_cast<uint8_t>(1U << index);
  result.dailyVariety = popcount8(state_.dailyActionMask);

  if (sameDay && minuteOfDay >= state_.lastActionMinute) {
    const uint16_t spacing =
        static_cast<uint16_t>(minuteOfDay - state_.lastActionMinute);
    if (spacing >= 45U && spacing <= 360U) {
      state_.dailyRhythm = saturatingIncrement8(state_.dailyRhythm);
    }
  }
  result.rhythmBonus = state_.dailyRhythm;

  const bool goalWasComplete = state_.goalProgress >= state_.goalTarget;
  const GoalKind goalKind = static_cast<GoalKind>(state_.goalKind);
  if (goalKind == GoalKind::AnyCare) {
    state_.goalProgress = saturatingIncrement8(state_.goalProgress);
  } else if (goalKind == GoalKind::Variety) {
    state_.goalProgress = popcount8(state_.dailyActionMask);
  } else if (index == state_.goalAction) {
    state_.goalProgress = saturatingIncrement8(state_.goalProgress);
  }
  result.goalCompletedNow =
      !goalWasComplete && state_.goalProgress >= state_.goalTarget;

  updateFavorite(result.favoriteChanged);
  updateRitual(action, bucket, day, result.ritualRecognized);
  updateSecretHabit(result.secretHabitUnlocked);

  if (state_.requestState == static_cast<uint8_t>(RequestState::Accepted) &&
      state_.requestAction == index) {
    state_.requestState = static_cast<uint8_t>(RequestState::Completed);
    result.requestCompleted = true;
  }

  updateComfortForAction(action, result.comforted);

  static const int8_t kMomentumDelta[kActionCount] = {2, 1, 2, 1,
                                                      1, 1, 2, 2};
  int16_t momentum =
      static_cast<int16_t>(state_.moodMomentum) + kMomentumDelta[index];
  if (result.repeatNoticed) momentum -= 2;
  state_.moodMomentum = clampMomentum(momentum);

  state_.lastAction = index;
  state_.lastActionDay = day;
  state_.lastActionMinute = minuteOfDay;
  updateSpeech(bondLevel);
  observeBond(bondLevel, day);

  if (state_.dailyActions > state_.bests.dailyActions) {
    state_.bests.dailyActions = state_.dailyActions;
  }
  if (result.dailyVariety > state_.bests.dailyVariety) {
    state_.bests.dailyVariety = result.dailyVariety;
  }
  if (state_.varietyChain > state_.bests.varietyChain) {
    state_.bests.varietyChain = state_.varietyChain;
  }
  if (state_.dailyRhythm > state_.bests.careRhythm) {
    state_.bests.careRhythm = state_.dailyRhythm;
  }

  updateLoreAndAchievements();
  result.moodMomentum = state_.moodMomentum;
  result.speechStage = state_.speechStage;
  result.bondDialogueBank = bondDialogueBank(bondLevel);
  result.valid = true;
  return result;
}

bool CompanionProgression::rememberEvent(uint8_t eventId, uint32_t day,
                                         uint8_t delayDays) {
  return enqueueCallback(CallbackKind::Event, eventId,
                         addDaysSaturated(day, delayDays));
}

bool CompanionProgression::rememberDream(uint8_t dreamId, uint32_t day) {
  state_.lore |= LoreDream;
  state_.achievements |= AchievementDreamer;
  return enqueueCallback(CallbackKind::Dream, dreamId,
                         addDaysSaturated(day, 1U));
}

bool CompanionProgression::rememberExpedition(uint8_t outcomeId,
                                              uint32_t day) {
  state_.lore |= LoreExpedition;
  state_.achievements |= AchievementExplorer;
  return enqueueCallback(CallbackKind::Expedition, outcomeId,
                         addDaysSaturated(day, 1U));
}

bool CompanionProgression::rememberFriend(uint8_t friendDetail,
                                          uint32_t day) {
  state_.lore |= LoreFriend;
  state_.achievements |= AchievementFriendly;
  return enqueueCallback(CallbackKind::Friend, friendDetail,
                         addDaysSaturated(day, 1U));
}

bool CompanionProgression::takeCallback(uint32_t day, Callback& callback) {
  if (!callbackReady(day)) return false;
  const CallbackRecord& record = state_.callbacks[state_.callbackHead];
  callback.kind = static_cast<CallbackKind>(record.kind);
  callback.detail = record.detail;
  callback.dueDay = record.dueDay;
  state_.callbackHead = static_cast<uint8_t>(
      (state_.callbackHead + 1U) % kCallbackCapacity);
  --state_.callbackCount;
  if (state_.callbackCount == 0U) state_.callbackHead = 0U;
  state_.achievements |= AchievementFirstCallback;
  state_.lore |= LoreFirstMemory;
  return true;
}

bool CompanionProgression::callbackReady(uint32_t day) const {
  if (state_.callbackCount == 0U) return false;
  return state_.callbacks[state_.callbackHead].dueDay <= day;
}

RequestResult CompanionProgression::answerRequest(bool accept) {
  RequestResult result{};
  if (state_.requestState != static_cast<uint8_t>(RequestState::Pending)) {
    return result;
  }
  result.requestedAction = actionFromIndex(state_.requestAction);
  result.accepted = accept;
  result.valid = true;
  if (accept) {
    state_.requestState = static_cast<uint8_t>(RequestState::Accepted);
    state_.acceptedRequests = saturatingIncrement16(state_.acceptedRequests);
  } else {
    state_.requestState = static_cast<uint8_t>(RequestState::Declined);
    state_.declinedRequests = saturatingIncrement16(state_.declinedRequests);
  }
  return result;
}

QuestionResult CompanionProgression::answerQuestion(uint8_t choice) {
  QuestionResult result{};
  if (state_.pendingQuestion == kNoQuestion || choice > 1U) return result;
  const uint8_t question = state_.pendingQuestion;
  const uint8_t choiceMask = static_cast<uint8_t>(1U << question);
  const uint8_t answeredMask = static_cast<uint8_t>(1U << (question + 4U));
  if (choice != 0U) {
    state_.questionPreferences |= choiceMask;
  } else {
    state_.questionPreferences &= static_cast<uint8_t>(~choiceMask);
  }
  state_.questionPreferences |= answeredMask;
  state_.pendingQuestion = kNoQuestion;
  result.question = static_cast<QuestionKind>(question);
  result.choice = choice;
  result.valid = true;
  return result;
}

bool CompanionProgression::observeBond(uint8_t bondLevel, uint32_t day) {
  const uint8_t bank = bondDialogueBank(bondLevel);
  if (bank <= state_.highestBondBank) return false;
  state_.highestBondBank = bank;
  state_.lore |= LoreBond;
  enqueueCallback(CallbackKind::BondMilestone, bank, day);
  return true;
}

bool CompanionProgression::setNickname(const char* nicknameValue) {
  if (nicknameValue == nullptr) return false;
  size_t length = 0U;
  while (nicknameValue[length] != '\0' && length <= kNicknameCapacity) {
    const unsigned char value =
        static_cast<unsigned char>(nicknameValue[length]);
    if (value < 32U || value > 126U) return false;
    ++length;
  }
  if (length > kNicknameCapacity) return false;
  memset(state_.nickname, 0, sizeof(state_.nickname));
  if (length != 0U) memcpy(state_.nickname, nicknameValue, length);
  return true;
}

const char* CompanionProgression::nickname() const { return state_.nickname; }

DailyGoal CompanionProgression::dailyGoal() const { return goalFromState(); }

RequestState CompanionProgression::requestState() const {
  return static_cast<RequestState>(state_.requestState);
}

dialogue::Action CompanionProgression::requestedAction() const {
  return actionFromIndex(state_.requestAction);
}

bool CompanionProgression::pendingQuestion(QuestionKind& question) const {
  if (state_.pendingQuestion == kNoQuestion) return false;
  question = static_cast<QuestionKind>(state_.pendingQuestion);
  return true;
}

bool CompanionProgression::preferredQuestionChoice(QuestionKind question,
                                                    uint8_t& choice) const {
  const uint8_t index = static_cast<uint8_t>(question);
  if (index > static_cast<uint8_t>(QuestionKind::HomeOrExplore)) return false;
  if ((state_.questionPreferences & static_cast<uint8_t>(1U << (index + 4U))) ==
      0U) {
    return false;
  }
  choice = (state_.questionPreferences & static_cast<uint8_t>(1U << index)) !=
                   0U
               ? 1U
               : 0U;
  return true;
}

ComfortKind CompanionProgression::comfortNeed() const {
  return static_cast<ComfortKind>(state_.comfort);
}

bool CompanionProgression::hasFavorite() const {
  return validAction(state_.favoriteAction);
}

dialogue::Action CompanionProgression::favoriteAction() const {
  return hasFavorite() ? actionFromIndex(state_.favoriteAction)
                       : dialogue::Action::Pet;
}

bool CompanionProgression::preferredTime(dialogue::Action action,
                                         TimeBucket& bucket) const {
  const uint8_t index = actionIndex(action);
  if (!validAction(index)) return false;
  uint8_t best = 0U;
  uint8_t runner = 0U;
  uint8_t bestBucket = 0U;
  for (uint8_t current = 0U; current < kTimeBucketCount; ++current) {
    const uint8_t count = state_.actionBucketCounts[index][current];
    if (count > best) {
      runner = best;
      best = count;
      bestBucket = current;
    } else if (count > runner) {
      runner = count;
    }
  }
  if (best < 2U || best <= runner) return false;
  bucket = static_cast<TimeBucket>(bestBucket);
  return true;
}

bool CompanionProgression::recognizedRoutine(TimeBucket bucket,
                                              dialogue::Action& action) const {
  const uint8_t time = bucketIndex(bucket);
  if (time >= kTimeBucketCount) return false;
  uint8_t best = 0U;
  uint8_t runner = 0U;
  uint8_t bestAction = 0U;
  for (uint8_t current = 0U; current < kActionCount; ++current) {
    const uint8_t count = state_.actionBucketCounts[current][time];
    if (count > best) {
      runner = best;
      best = count;
      bestAction = current;
    } else if (count > runner) {
      runner = count;
    }
  }
  if (best < 4U || static_cast<uint16_t>(best) <
                       static_cast<uint16_t>(runner) + 2U) {
    return false;
  }
  action = actionFromIndex(bestAction);
  return true;
}

bool CompanionProgression::hasRitual() const {
  return validAction(state_.ritualAction) && state_.ritualStreak >= 3U;
}

dialogue::Action CompanionProgression::ritualAction() const {
  return hasRitual() ? actionFromIndex(state_.ritualAction)
                     : dialogue::Action::Pet;
}

TimeBucket CompanionProgression::ritualTime() const {
  return static_cast<TimeBucket>(state_.ritualBucket);
}

uint8_t CompanionProgression::ritualStreak() const {
  return state_.ritualStreak;
}

bool CompanionProgression::hasSecretHabit() const {
  return state_.secretHabit < 6U;
}

HabitKind CompanionProgression::secretHabit() const {
  return hasSecretHabit() ? static_cast<HabitKind>(state_.secretHabit)
                          : HabitKind::DawnPets;
}

uint8_t CompanionProgression::speechStage() const { return state_.speechStage; }

uint8_t CompanionProgression::bondDialogueBank(uint8_t bondLevel) const {
  if (bondLevel >= 5U) return 3U;
  if (bondLevel >= 3U) return 2U;
  return bondLevel == 0U ? 0U : 1U;
}

int8_t CompanionProgression::moodMomentum() const {
  return state_.moodMomentum;
}

uint16_t CompanionProgression::currentStreak() const {
  return state_.currentStreak;
}

uint16_t CompanionProgression::perfectDays() const {
  return state_.perfectDays;
}

uint32_t CompanionProgression::achievementMask() const {
  return state_.achievements;
}

uint16_t CompanionProgression::loreMask() const { return state_.lore; }

PersonalBests CompanionProgression::personalBests() const {
  return state_.bests;
}

uint32_t CompanionProgression::totalActions() const {
  return state_.totalActions;
}

uint16_t CompanionProgression::acceptedRequests() const {
  return state_.acceptedRequests;
}

uint16_t CompanionProgression::declinedRequests() const {
  return state_.declinedRequests;
}

bool CompanionProgression::lastDayWasComeback() const {
  return state_.dailyComeback != 0U;
}

uint32_t CompanionProgression::dailySeed(uint32_t day) const {
  return mix32(state_.companionFingerprint ^
               (day * UINT32_C(0x9E3779B9)) ^ UINT32_C(0x44535931));
}

size_t CompanionProgression::snapshotSize() {
  static_assert(sizeof(PersistedState) <= kSnapshotCapacity,
                "Progression snapshot exceeded its storage budget");
  return sizeof(PersistedState);
}

bool CompanionProgression::writeSnapshot(void* destination,
                                         size_t capacity) const {
  if (destination == nullptr || capacity < sizeof(PersistedState)) return false;
  PersistedState copy = state_;
  copy.bytes = static_cast<uint16_t>(sizeof(PersistedState));
  copy.crc32 = calculateCrc(copy);
  memcpy(destination, &copy, sizeof(copy));
  return true;
}

bool CompanionProgression::restoreSnapshot(
    const void* source, size_t length,
    uint32_t expectedCompanionFingerprint) {
  if (source == nullptr || length != sizeof(PersistedState)) return false;
  PersistedState candidate{};
  memcpy(&candidate, source, sizeof(candidate));
  if (!validState(candidate, expectedCompanionFingerprint)) return false;
  state_ = candidate;
  return true;
}

TimeBucket CompanionProgression::timeBucket(uint16_t minuteOfDay) {
  if (minuteOfDay >= 300U && minuteOfDay < 720U) return TimeBucket::Morning;
  if (minuteOfDay >= 720U && minuteOfDay < 1020U) return TimeBucket::Day;
  if (minuteOfDay >= 1020U && minuteOfDay < 1320U) return TimeBucket::Evening;
  return TimeBucket::Night;
}

DisplayLine CompanionProgression::greetingLine(GreetingKind greeting) {
  switch (greeting) {
    case GreetingKind::MissedYou:
      return {"MISSED YOU", "WELCOME BACK"};
    case GreetingKind::LongAbsence:
      return {"LONG TIME", "STILL YOURS"};
    case GreetingKind::Comeback:
      return {"YOU CAME BACK", "LET'S BEGIN"};
    case GreetingKind::Normal:
    default:
      return {"GOOD TO SEE YOU", "READY?"};
  }
}

DisplayLine CompanionProgression::callbackLine(const Callback& callback) {
  switch (callback.kind) {
    case CallbackKind::Dream:
      return {"THAT DREAM...", "I REMEMBER"};
    case CallbackKind::Expedition:
      return {"OUR LAST TRIP", "STAYS WITH ME"};
    case CallbackKind::Friend:
      return {"OUR NEW FRIEND", "WAS NICE"};
    case CallbackKind::PerfectDay:
      return {"YESTERDAY", "WAS PERFECT"};
    case CallbackKind::BondMilestone:
      return {"WE FEEL CLOSER", "DON'T WE?"};
    case CallbackKind::Ritual:
      return {"OUR LITTLE", "RITUAL AGAIN"};
    case CallbackKind::Anniversary:
      return {"OUR DAY AGAIN", "THANK YOU"};
    case CallbackKind::Event:
    default:
      return {"I REMEMBER", "THAT MOMENT"};
  }
}

DisplayLine CompanionProgression::requestLine(dialogue::Action action) {
  switch (action) {
    case dialogue::Action::Feed:
      return {"MAY I HAVE", "A SNACK?"};
    case dialogue::Action::Play:
      return {"CAN WE PLAY", "FOR A BIT?"};
    case dialogue::Action::Listen:
      return {"LET'S LISTEN", "TOGETHER"};
    case dialogue::Action::Sleep:
      return {"CAN WE REST", "FOR A WHILE?"};
    case dialogue::Action::Wake:
      return {"WAKE ME", "WHEN READY"};
    case dialogue::Action::Meet:
      return {"LET'S FIND", "A FRIEND"};
    case dialogue::Action::Gift:
      return {"A SURPRISE", "MAYBE?"};
    case dialogue::Action::Pet:
    default:
      return {"A LITTLE PET?", "YES OR NO"};
  }
}

DisplayLine CompanionProgression::questionLine(QuestionKind question) {
  switch (question) {
    case QuestionKind::DawnOrNight:
      return {"WHICH FEELS", "MORE LIKE US?"};
    case QuestionKind::HomeOrExplore:
      return {"STAY HOME OR", "GO EXPLORE?"};
    case QuestionKind::QuietOrPlay:
    default:
      return {"QUIET OR", "PLAYFUL?"};
  }
}

const char* CompanionProgression::questionOption(QuestionKind question,
                                                 uint8_t choice) {
  if (choice > 1U) return "";
  switch (question) {
    case QuestionKind::DawnOrNight:
      return choice == 0U ? "DAWN" : "NIGHT";
    case QuestionKind::HomeOrExplore:
      return choice == 0U ? "HOME" : "EXPLORE";
    case QuestionKind::QuietOrPlay:
    default:
      return choice == 0U ? "QUIET" : "PLAY";
  }
}

DisplayLine CompanionProgression::seasonalLine(Season season) {
  switch (season) {
    case Season::Summer:
      return {"SUMMER SIGNAL", "FEELS BRIGHT"};
    case Season::Autumn:
      return {"AUTUMN STATIC", "DRIFTS SOFTLY"};
    case Season::Winter:
      return {"WINTER SIGNAL", "STAY CLOSE"};
    case Season::Spring:
    default:
      return {"SPRING SIGNAL", "FEELS NEW"};
  }
}

DisplayLine CompanionProgression::rareMomentLine(uint32_t seed) {
  switch (seed % 3U) {
    case 0U:
      return {"DID TIME", "JUST SHIMMER?"};
    case 1U:
      return {"A TINY SIGNAL", "SAID HELLO"};
    default:
      return {"KEEP THIS", "QUIET MOMENT"};
  }
}

DisplayLine CompanionProgression::bondLine(uint8_t dialogueBank) {
  switch (dialogueBank) {
    case 1U:
      return {"I KNOW YOU", "A LITTLE NOW"};
    case 2U:
      return {"I TRUST YOU", "WITH MY SIGNAL"};
    case 3U:
      return {"ALWAYS WITH YOU", "ALWAYS"};
    default:
      return {"HELLO THERE", "NEW FRIEND"};
  }
}

DisplayLine CompanionProgression::comfortLine(ComfortKind comfort) {
  switch (comfort) {
    case ComfortKind::Tired:
      return {"A BIT TIRED", "STAY NEAR"};
    case ComfortKind::Lonely:
      return {"I MISSED YOU", "HOLD CLOSE"};
    case ComfortKind::Restless:
      return {"SO MUCH STATIC", "HELP ME SETTLE"};
    case ComfortKind::None:
    default:
      return {"I'M ALL RIGHT", "RIGHT HERE"};
  }
}

DisplayLine CompanionProgression::speechLine(uint8_t speechStageValue) {
  switch (speechStageValue) {
    case 1U:
      return {"I'M LEARNING", "YOUR RHYTHM"};
    case 2U:
      return {"WE HAVE OUR", "OWN LANGUAGE"};
    case 3U:
      return {"I KNOW WHAT", "YOU MEAN NOW"};
    default:
      return {"HELLO?", "CAN YOU HEAR?"};
  }
}

DisplayLine CompanionProgression::habitLine(HabitKind habit) {
  switch (habit) {
    case HabitKind::LunchSnacks:
      return {"I COUNT SNACKS", "AT MIDDAY"};
    case HabitKind::EveningGames:
      return {"EVENING GAMES", "ARE MY SECRET"};
    case HabitKind::NightListening:
      return {"NIGHT STATIC", "HELPS ME THINK"};
    case HabitKind::WakeUpCalls:
      return {"I WAIT FOR", "YOUR WAKE CALL"};
    case HabitKind::GiftWatcher:
      return {"I PRETEND NOT", "TO SEE GIFTS"};
    case HabitKind::DawnPets:
    default:
      return {"DAWN PETS ARE", "MY FAVORITE"};
  }
}

DisplayLine CompanionProgression::weeklyChapterLine(uint8_t chapter) {
  static const DisplayLine kChapters[7] = {
      {"A SIGNAL BEGAN", "UNDER THE HILL"},
      {"IT FOUND A PATH", "MADE OF STATIC"},
      {"A SMALL ECHO", "FOLLOWED CLOSE"},
      {"THE PATH SPLIT", "WE CHOSE ONE"},
      {"THE ECHO SANG", "OUR NAMES BACK"},
      {"DAWN FOUND US", "STILL WALKING"},
      {"WE CAME HOME", "WITH A STORY"},
  };
  return kChapters[chapter < 7U ? chapter : 0U];
}

DisplayLine CompanionProgression::loreLine(LoreUnlock lore) {
  switch (lore) {
    case LoreFavorite:
      return {"FAMILIAR CARE", "SHAPES A SIGNAL"};
    case LoreRitual:
      return {"RITUALS LEAVE", "WARM ECHOES"};
    case LoreSecretHabit:
      return {"EVERY SIGNAL", "HAS A QUIRK"};
    case LoreDream:
      return {"DREAMS TRAVEL", "QUIET BANDS"};
    case LoreExpedition:
      return {"DISTANT PATHS", "CHANGE US"};
    case LoreFriend:
      return {"KNOWN SIGNALS", "GROW BRIGHTER"};
    case LorePerfectDay:
      return {"BALANCED DAYS", "RESONATE"};
    case LoreBond:
      return {"TRUST TUNES TWO", "SIGNALS AS ONE"};
    case LoreAnniversary:
      return {"A YEARLY ECHO", "NEVER FADES"};
    case LoreFirstMemory:
    default:
      return {"MEMORIES LIVE", "BETWEEN PULSES"};
  }
}

uint32_t CompanionProgression::mix32(uint32_t value) {
  value ^= value >> 16U;
  value *= UINT32_C(0x7FEB352D);
  value ^= value >> 15U;
  value *= UINT32_C(0x846CA68B);
  return value ^ (value >> 16U);
}

uint32_t CompanionProgression::calculateCrc(const PersistedState& state) {
  PersistedState copy = state;
  copy.crc32 = 0U;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&copy);
  uint32_t crc = UINT32_C(0xFFFFFFFF);
  for (size_t index = 0U; index < sizeof(copy); ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(
          -static_cast<int32_t>(crc & UINT32_C(1)));
      crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
    }
  }
  return ~crc;
}

uint8_t CompanionProgression::popcount8(uint8_t value) {
  uint8_t count = 0U;
  while (value != 0U) {
    count = static_cast<uint8_t>(count + (value & 1U));
    value >>= 1U;
  }
  return count;
}

uint8_t CompanionProgression::saturatingIncrement8(uint8_t value) {
  return value == UINT8_MAX ? value : static_cast<uint8_t>(value + 1U);
}

uint16_t CompanionProgression::saturatingIncrement16(uint16_t value) {
  return value == UINT16_MAX ? value : static_cast<uint16_t>(value + 1U);
}

uint32_t CompanionProgression::saturatingIncrement32(uint32_t value) {
  return value == UINT32_MAX ? value : value + 1U;
}

int8_t CompanionProgression::clampMomentum(int16_t value) {
  if (value > 12) return 12;
  if (value < -12) return -12;
  return static_cast<int8_t>(value);
}

bool CompanionProgression::validAction(uint8_t action) {
  return action < kActionCount;
}

bool CompanionProgression::validState(
    const PersistedState& candidate, uint32_t expectedFingerprint) const {
  if (candidate.magic != kMagic || candidate.schema != kSchema ||
      candidate.bytes != sizeof(PersistedState) ||
      candidate.companionFingerprint != expectedFingerprint ||
      candidate.crc32 != calculateCrc(candidate)) {
    return false;
  }
  if (candidate.currentDay < candidate.firstDay ||
      (candidate.activeDays != 0U &&
       candidate.lastSessionDay > candidate.currentDay) ||
      candidate.lastActionMinute >= 1440U ||
      (candidate.lastAction != kNoAction &&
       !validAction(candidate.lastAction)) ||
      (candidate.lastSuggestedAction != kNoAction &&
       !validAction(candidate.lastSuggestedAction)) ||
      (candidate.favoriteAction != kNoAction &&
       !validAction(candidate.favoriteAction)) ||
      candidate.speechStage > 3U || candidate.highestBondBank > 3U ||
      candidate.goalKind > static_cast<uint8_t>(GoalKind::Favorite) ||
      !validAction(candidate.goalAction) || candidate.goalTarget == 0U ||
      candidate.requestState > static_cast<uint8_t>(RequestState::Completed) ||
      !validAction(candidate.requestAction) ||
      (candidate.pendingQuestion != kNoQuestion &&
       candidate.pendingQuestion >
           static_cast<uint8_t>(QuestionKind::HomeOrExplore)) ||
      candidate.comfort > static_cast<uint8_t>(ComfortKind::Restless) ||
      (candidate.ritualAction != kNoAction &&
       !validAction(candidate.ritualAction)) ||
      candidate.ritualBucket >= kTimeBucketCount ||
      (candidate.secretHabit != kNoHabit && candidate.secretHabit >= 6U) ||
      candidate.callbackHead >= kCallbackCapacity ||
      candidate.callbackCount > kCallbackCapacity ||
      candidate.moodMomentum < -12 || candidate.moodMomentum > 12 ||
      candidate.dailyComeback > 1U || candidate.rareShownToday > 1U ||
      candidate.anniversaryShownToday > 1U || candidate.graceUsed > 1U) {
    return false;
  }
  bool terminated = false;
  for (size_t index = 0U; index < sizeof(candidate.nickname); ++index) {
    const unsigned char value =
        static_cast<unsigned char>(candidate.nickname[index]);
    if (value == 0U) {
      terminated = true;
      break;
    }
    if (value < 32U || value > 126U) return false;
  }
  if (!terminated) return false;
  for (uint8_t offset = 0U; offset < candidate.callbackCount; ++offset) {
    const uint8_t index = static_cast<uint8_t>(
        (candidate.callbackHead + offset) % kCallbackCapacity);
    if (candidate.callbacks[index].kind >
        static_cast<uint8_t>(CallbackKind::Anniversary)) {
      return false;
    }
  }
  return true;
}

bool CompanionProgression::moveToDay(uint32_t day, bool& previousPerfect) {
  previousPerfect = false;
  if (day < state_.firstDay) return false;
  if (state_.activeDays == 0U) {
    state_.currentDay = day;
    beginFreshDay(day);
    return true;
  }
  if (day < state_.currentDay) return false;
  if (day == state_.currentDay) return true;

  const uint32_t oldDay = state_.currentDay;
  previousPerfect = finalizeActiveDay();
  const uint32_t gap = day - oldDay;
  if (gap > 1U) missDay();
  if (gap > 2U && state_.currentStreak != 0U) missDay();

  const uint8_t decay = gap > 12U ? 12U : static_cast<uint8_t>(gap);
  if (state_.moodMomentum > 0) {
    const int16_t next = static_cast<int16_t>(state_.moodMomentum) - decay;
    state_.moodMomentum = next < 0 ? 0 : static_cast<int8_t>(next);
  } else if (state_.moodMomentum < 0) {
    const int16_t next = static_cast<int16_t>(state_.moodMomentum) + decay;
    state_.moodMomentum = next > 0 ? 0 : static_cast<int8_t>(next);
  }

  state_.currentDay = day;
  beginFreshDay(day);
  return true;
}

bool CompanionProgression::finalizeActiveDay() {
  const bool completed = state_.goalProgress >= state_.goalTarget;
  if (completed) {
    state_.currentStreak = saturatingIncrement16(state_.currentStreak);
    if (state_.currentStreak != 0U &&
        (state_.currentStreak % 7U) == 0U) {
      state_.graceUsed = 0U;
    }
  } else {
    missDay();
  }
  if (state_.currentStreak > state_.bests.streak) {
    state_.bests.streak = state_.currentStreak;
  }

  const bool perfect = completed && popcount8(state_.dailyActionMask) >= 3U &&
                       state_.dailyRhythm != 0U;
  if (perfect) {
    state_.perfectDays = saturatingIncrement16(state_.perfectDays);
    state_.lastPerfectDay = state_.currentDay;
    state_.achievements |= AchievementPerfectDay;
    state_.lore |= LorePerfectDay;
    enqueueCallback(CallbackKind::PerfectDay, 0U,
                    addDaysSaturated(state_.currentDay, 1U));
  }
  updateLoreAndAchievements();
  return perfect;
}

void CompanionProgression::beginFreshDay(uint32_t day) {
  state_.dailyActions = 0U;
  state_.dailyActionMask = 0U;
  state_.dailyRhythm = 0U;
  state_.varietyChain = 0U;
  state_.repeatCount = 0U;
  state_.lastSuggestedAction = kNoAction;
  state_.requestState = static_cast<uint8_t>(RequestState::None);
  state_.requestAction = 0U;
  state_.comfort = static_cast<uint8_t>(ComfortKind::None);
  state_.dailyComeback = 0U;
  state_.rareShownToday = 0U;
  state_.anniversaryShownToday = 0U;
  state_.reservedFlags &= static_cast<uint8_t>(~kSeasonalShown);

  const uint32_t seed = dailySeed(day);
  const uint32_t average =
      state_.activeDays == 0U ? 0U : state_.totalActions / state_.activeDays;
  if (hasFavorite() && (seed % 3U) == 0U) {
    state_.goalKind = static_cast<uint8_t>(GoalKind::Favorite);
    state_.goalAction = state_.favoriteAction;
    state_.goalTarget = average >= 5U ? 3U : 2U;
  } else if (average >= 4U) {
    state_.goalKind = static_cast<uint8_t>(GoalKind::Variety);
    state_.goalAction = 0U;
    state_.goalTarget = average >= 8U ? 4U : 3U;
  } else {
    state_.goalKind = static_cast<uint8_t>(GoalKind::AnyCare);
    state_.goalAction = 0U;
    state_.goalTarget = average == 0U ? 2U : 3U;
  }
  state_.goalProgress = 0U;
  state_.pendingQuestion =
      (seed % 3U) == 0U ? static_cast<uint8_t>((seed >> 8U) % 3U)
                        : kNoQuestion;
  state_.activeDays = saturatingIncrement16(state_.activeDays);
}

void CompanionProgression::missDay() {
  if (state_.currentStreak == 0U) return;
  if (state_.graceUsed == 0U) {
    state_.graceUsed = 1U;
  } else {
    state_.currentStreak = 0U;
  }
}

void CompanionProgression::updateFavorite(bool& changed) {
  changed = false;
  uint16_t best = 0U;
  uint16_t runner = 0U;
  uint8_t bestAction = 0U;
  for (uint8_t action = 0U; action < kActionCount; ++action) {
    const uint16_t count = state_.actionCounts[action];
    if (count > best) {
      runner = best;
      best = count;
      bestAction = action;
    } else if (count > runner) {
      runner = count;
    }
  }
  if (best >= 4U && static_cast<uint32_t>(best) >=
                        static_cast<uint32_t>(runner) + 2U &&
      state_.favoriteAction != bestAction) {
    state_.favoriteAction = bestAction;
    changed = true;
    state_.achievements |= AchievementFavoriteLearned;
    state_.lore |= LoreFavorite;
  }
}

void CompanionProgression::updateSpeech(uint8_t bondLevel) {
  uint8_t actionStage = 0U;
  if (state_.totalActions >= 75U) {
    actionStage = 3U;
  } else if (state_.totalActions >= 30U) {
    actionStage = 2U;
  } else if (state_.totalActions >= 10U) {
    actionStage = 1U;
  }
  const uint8_t bondStage = bondDialogueBank(bondLevel);
  state_.speechStage = actionStage > bondStage ? actionStage : bondStage;
}

void CompanionProgression::updateLoreAndAchievements() {
  if (state_.bests.dailyVariety >= 5U) {
    state_.achievements |= AchievementVarietyFive;
  }
  if (state_.totalActions >= 100U) {
    state_.achievements |= AchievementHundredActions;
  }
  if (state_.currentStreak >= 7U) {
    state_.achievements |= AchievementStreakSeven;
  }
}

bool CompanionProgression::enqueueCallback(CallbackKind kind, uint8_t detail,
                                           uint32_t dueDay) {
  for (uint8_t offset = 0U; offset < state_.callbackCount; ++offset) {
    const uint8_t index = static_cast<uint8_t>(
        (state_.callbackHead + offset) % kCallbackCapacity);
    const CallbackRecord& existing = state_.callbacks[index];
    if (existing.kind == static_cast<uint8_t>(kind) &&
        existing.detail == detail) {
      return true;
    }
  }
  if (state_.callbackCount >= kCallbackCapacity) return false;
  const uint8_t tail = static_cast<uint8_t>(
      (state_.callbackHead + state_.callbackCount) % kCallbackCapacity);
  state_.callbacks[tail].dueDay = dueDay;
  state_.callbacks[tail].kind = static_cast<uint8_t>(kind);
  state_.callbacks[tail].detail = detail;
  ++state_.callbackCount;
  return true;
}

void CompanionProgression::updateRitual(dialogue::Action action,
                                        TimeBucket bucket, uint32_t day,
                                        bool& recognizedNow) {
  recognizedNow = false;
  if (state_.ritualLastDay == day && validAction(state_.ritualAction)) return;
  const uint8_t actionValue = actionIndex(action);
  const uint8_t bucketValue = bucketIndex(bucket);
  const bool consecutive = validAction(state_.ritualAction) &&
                           day > state_.ritualLastDay &&
                           day - state_.ritualLastDay == 1U;
  if (consecutive && state_.ritualAction == actionValue &&
      state_.ritualBucket == bucketValue) {
    state_.ritualStreak = saturatingIncrement8(state_.ritualStreak);
  } else {
    state_.ritualAction = actionValue;
    state_.ritualBucket = bucketValue;
    state_.ritualStreak = 1U;
  }
  state_.ritualLastDay = day;
  if (state_.ritualStreak == 3U) {
    recognizedNow = true;
    state_.achievements |= AchievementRitual;
    state_.lore |= LoreRitual;
    enqueueCallback(CallbackKind::Ritual, actionValue, day);
  }
}

void CompanionProgression::updateSecretHabit(bool& unlockedNow) {
  unlockedNow = false;
  if (hasSecretHabit()) return;
  const uint8_t habit =
      static_cast<uint8_t>(state_.companionFingerprint % 6U);
  static const uint8_t kHabitActions[6] = {
      static_cast<uint8_t>(dialogue::Action::Pet),
      static_cast<uint8_t>(dialogue::Action::Feed),
      static_cast<uint8_t>(dialogue::Action::Play),
      static_cast<uint8_t>(dialogue::Action::Listen),
      static_cast<uint8_t>(dialogue::Action::Wake),
      static_cast<uint8_t>(dialogue::Action::Gift),
  };
  static const uint8_t kHabitBuckets[6] = {
      static_cast<uint8_t>(TimeBucket::Morning),
      static_cast<uint8_t>(TimeBucket::Day),
      static_cast<uint8_t>(TimeBucket::Evening),
      static_cast<uint8_t>(TimeBucket::Night),
      static_cast<uint8_t>(TimeBucket::Morning),
      static_cast<uint8_t>(TimeBucket::Evening),
  };
  if (state_.actionBucketCounts[kHabitActions[habit]][kHabitBuckets[habit]] <
      3U) {
    return;
  }
  state_.secretHabit = habit;
  state_.achievements |= AchievementSecretHabit;
  state_.lore |= LoreSecretHabit;
  unlockedNow = true;
}

void CompanionProgression::updateComfortForAction(dialogue::Action action,
                                                  bool& comforted) {
  comforted = false;
  const ComfortKind need = static_cast<ComfortKind>(state_.comfort);
  if ((need == ComfortKind::Tired &&
       (action == dialogue::Action::Feed || action == dialogue::Action::Sleep)) ||
      (need == ComfortKind::Lonely &&
       (action == dialogue::Action::Pet || action == dialogue::Action::Meet ||
        action == dialogue::Action::Gift)) ||
      (need == ComfortKind::Restless &&
       (action == dialogue::Action::Play ||
        action == dialogue::Action::Listen))) {
    state_.comfort = static_cast<uint8_t>(ComfortKind::None);
    comforted = true;
  }
}

dialogue::Action CompanionProgression::chooseRequest(
    const CompanionVitals& vitals, uint32_t seed) const {
  if (vitals.sleeping || vitals.energy < 20U) return dialogue::Action::Sleep;
  if (vitals.curiosity > 80U) return dialogue::Action::Play;
  if (vitals.affection < 20U) return dialogue::Action::Pet;
  if (hasFavorite() && (seed & 1U) != 0U) return favoriteAction();
  static const dialogue::Action kRequests[5] = {
      dialogue::Action::Pet, dialogue::Action::Feed, dialogue::Action::Play,
      dialogue::Action::Listen, dialogue::Action::Meet,
  };
  return kRequests[seed % 5U];
}

DailyGoal CompanionProgression::goalFromState() const {
  DailyGoal goal{};
  goal.kind = static_cast<GoalKind>(state_.goalKind);
  goal.action = actionFromIndex(state_.goalAction);
  goal.target = state_.goalTarget;
  goal.progress = state_.goalProgress;
  return goal;
}

}  // namespace progression
}  // namespace kitsu868
