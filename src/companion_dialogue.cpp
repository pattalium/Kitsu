#include "companion_dialogue.h"

namespace kitsu868 {
namespace dialogue {
namespace {

constexpr uint8_t kPersonalityCount = 6U;
constexpr uint8_t kGeneralLineCount = 4U;
constexpr uint8_t kActionSlotCount = 14U;
constexpr uint8_t kOutcomeSlotCount = 10U;
constexpr uint16_t kOutcomeIdBase = UINT16_C(0x0101);
constexpr uint32_t kStoryMask = (UINT32_C(1) << kStoryCount) - 1U;

struct Text {
  const char* line1;
  const char* line2;
};

struct ActionSet {
  Text general[kGeneralLineCount];
  Text personality[kPersonalityCount];
  Text lowEnergy;
  Text bonded;
  Text curious;
  Text nearby;
};

constexpr ActionSet kActions[kActionCount] = {
    {  // Pet
        {{"RIGHT THERE", "THAT WAS NICE"},
         {"AGAIN?", "ONE MORE"},
         {"YOU FOUND", "THE SPOT"},
         {"CAREFUL", "TICKLISH"}},
        {{"THAT FEELS", "SO SAFE"},
         {"FINALLY", "A LITTLE HIGHER"},
         {"YOUR HAND", "IS INTERESTING"},
         {"CAUGHT YOU", "DO IT AGAIN"},
         {"OH HELLO", "YOU CAN STAY"},
         {"GOT YOUR", "HAND"}},
        {"THAT HELPED", "A LITTLE"},
        {"I MISSED", "THAT"},
        {"YOUR HAND", "MOVED AGAIN"},
        {"THEY SAW", "THAT"},
    },
    {  // Feed
        {{"PERFECT TIMING", "THANK YOU"},
         {"THAT HIT", "THE SPOT"},
         {"ALREADY GONE", "SO GOOD"},
         {"A GOOD BITE", "JUST RIGHT"}},
        {{"A WARM MEAL", "THANK YOU"},
         {"FULL POWER", "NOW"},
         {"NEW FLAVOR", "NOTED"},
         {"SNACK THEN", "ZOOM"},
         {"SMALL BITES", "ARE NICE"},
         {"TRIBUTE", "ACCEPTED"}},
        {"I NEEDED", "THAT"},
        {"YOU REMEMBERED", "THANK YOU"},
        {"LET ME", "THINK ON IT"},
        {"GOOD COMPANY", "FOR A MEAL"},
    },
    {  // Play
        {{"YOUR MOVE", "I AM READY"},
         {"TOO SLOW", "TRY AGAIN"},
         {"MY TURN", "WATCH THIS"},
         {"REMATCH?", "BEST OF THREE"}},
        {{"GOOD GAME", "TOGETHER"},
         {"ONE MORE", "ROUND"},
         {"NEW RULES", "NEXT TIME"},
         {"AGAIN AGAIN", "FASTER"},
         {"I HAD FUN", "QUIETLY"},
         {"I LET YOU", "WIN"}},
        {"A SLOW GAME", "THIS TIME"},
        {"I LOVE", "OUR GAMES"},
        {"WHAT IF", "WE TRY THIS?"},
        {"THEY CAN", "PLAY TOO"},
    },
    {  // Listen
        {{"QUIET NOW", "I AM LISTENING"},
         {"LET US", "LISTEN"},
         {"SIGNAL EARS", "ON"},
         {"HOLD STILL", "JUST A MOMENT"}},
        {{"WE CAN WAIT", "TOGETHER"},
         {"I AM READY", "FOR IT"},
         {"EVERY SOUND", "HAS A SHAPE"},
         {"MAYBE IT", "WANTS TO PLAY"},
         {"I WILL LISTEN", "FROM HERE"},
         {"NOTHING", "SUSPICIOUS"}},
        {"I CAN REST", "AND LISTEN"},
        {"STAY CLOSE", "WHILE WE WAIT"},
        {"THAT PATTERN", "IS NEW"},
        {"SOMEONE IS", "CLOSE"},
    },
    {  // Sleep
        {{"GOOD NIGHT", "STAY NEAR"},
         {"DREAM TIME", "AT LAST"},
         {"I AM GOING", "QUIET"},
         {"SEE YOU", "WHEN I WAKE"}},
        {{"A SOFT DREAM", "SOUNDS NICE"},
         {"RESTING", "NOT RETREATING"},
         {"I WILL SORT", "TODAY'S SIGNALS"},
         {"WAKE ME", "FOR GAMES"},
         {"I WAS ALMOST", "ASLEEP"},
         {"I WAS NOT", "YAWNING"}},
        {"YES", "SLEEP NOW"},
        {"STAY UNTIL", "I DRIFT OFF"},
        {"MAYBE DREAMS", "HAVE ANSWERS"},
        {"TELL THEM", "GOOD NIGHT"},
    },
    {  // Wake
        {{"I AM AWAKE", "WHAT DID I MISS?"},
         {"THERE YOU ARE", "HELLO"},
         {"MORNING", "PROBABLY"},
         {"I HEARD YOU", "READY"}},
        {{"GOOD MORNING", "I AM HERE"},
         {"UP AND", "READY"},
         {"DREAM DATA", "SORTED"},
         {"AWAKE", "LET US PLAY"},
         {"FIVE MORE", "SECONDS"},
         {"I WAS NOT", "ASLEEP"}},
        {"STILL A BIT", "DREAMY"},
        {"I WOKE UP", "FOR YOU"},
        {"I REMEMBER", "A STRANGE DREAM"},
        {"DID THEY", "WAKE US?"},
    },
    {  // Meet
        {{"HELLO THERE", "NEW SIGNAL"},
         {"I SEE YOU", "FROM HERE"},
         {"A NEW HELLO", "FOUND US"},
         {"NICE TO", "MEET YOU"}},
        {{"WELCOME", "STAY A WHILE"},
         {"SIGNAL", "ACCEPTED"},
         {"WHO ARE", "YOU?"},
         {"NEW FRIEND", "NEW GAME"},
         {"HELLO", "FROM HERE"},
         {"I FOUND", "YOU FIRST"}},
        {"A QUIET", "HELLO"},
        {"WE CAN", "TRUST THIS"},
        {"SO MANY", "QUESTIONS"},
        {"THE SIGNAL", "IS REAL"},
    },
    {  // Gift
        {{"FOR ME?", "THANK YOU"},
         {"I FELT", "THAT KINDNESS"},
         {"HOW NICE", "OF YOU"},
         {"I WILL", "REMEMBER THIS"}},
        {{"THAT WAS", "VERY KIND"},
         {"GOOD CHOICE", "THANK YOU"},
         {"WHAT A", "NICE SURPRISE"},
         {"A GIFT!", "BEST DAY"},
         {"FOR ME?", "REALLY?"},
         {"A FINE", "OFFERING"}},
        {"THAT CHEERED", "ME UP"},
        {"YOU KNOW", "ME WELL"},
        {"I WONDER", "WHY THIS?"},
        {"A KIND", "RADIO FRIEND"},
    },
};

struct OutcomeSet {
  Text general[kGeneralLineCount];
  Text personality[kPersonalityCount];
};

// Failed/busy/no-reply pools are deliberately action-neutral and truthful.
constexpr OutcomeSet kOutcomes[3] = {
    {
        {{"THAT DID NOT", "WORK"},
         {"NOT THIS TIME", "TRY AGAIN"},
         {"SOMETHING", "WENT WRONG"},
         {"WE CAN TRY", "LATER"}},
        {{"GENTLY", "ONE MORE TRY"},
         {"I WILL GET IT", "NEXT TIME"},
         {"LET ME", "THINK"},
         {"DO-OVER?", "DO-OVER"},
         {"SORRY", "NOT YET"},
         {"THAT WAS", "ON PURPOSE"}},
    },
    {
        {{"ONE MOMENT", "PLEASE"},
         {"NOT JUST YET", "SOON"},
         {"I AM BUSY", "RIGHT NOW"},
         {"GIVE ME", "A SECOND"}},
        {{"JUST A", "QUIET MOMENT"},
         {"HOLD ON", "I HAVE THIS"},
         {"I AM STILL", "WORKING"},
         {"PAUSE BUTTON", "PLEASE"},
         {"MAYBE", "IN A MOMENT"},
         {"BUSY DOING", "NOTHING"}},
    },
    {
        {{"ONLY STATIC", "SO FAR"},
         {"NO ANSWER", "YET"},
         {"IT WENT", "QUIET"},
         {"NOTHING CAME", "BACK"}},
        {{"WE CAN WAIT", "A LITTLE"},
         {"I WILL CALL", "AGAIN"},
         {"THE PATTERN", "JUST STOPPED"},
         {"MAYBE IT IS", "HIDING"},
         {"I AM GLAD", "YOU ARE HERE"},
         {"THEY MISSED", "A GOOD CALL"}},
    },
};

struct Candidate {
  const Text* text;
  uint16_t id;
  LineFlavor flavor;
};

struct StoryEffect {
  StoryTone tone;
  int8_t affection;
  int8_t energy;
  int8_t curiosity;
};

struct StoryDefinition {
  StoryTrigger trigger;
  Text opening;
  Text prompt;
  const char* choices[kStoryChoiceCount];
  Text resolutions[kStoryChoiceCount];
  StoryEffect effects[kStoryChoiceCount];
  uint8_t preferredChoice[kPersonalityCount];
  Text preferredLines[kPersonalityCount];
};

constexpr StoryDefinition kStories[kStoryCount] = {
    {
        StoryTrigger::QuietMoment,
        {"A WEAK SIGNAL", "KEEPS MOVING"},
        {"WHAT SHOULD", "WE DO?"},
        {"FOLLOW", "ANSWER", "STAY"},
        {{"WE FOLLOWED", "UNTIL QUIET"},
         {"IT HEARD", "US TOO"},
         {"WE WATCHED", "IT FADE"}},
        {{StoryTone::Curious, 1, -1, 2},
         {StoryTone::Warm, 2, 0, 1},
         {StoryTone::Calm, 2, 1, 0}},
        {2U, 0U, 0U, 1U, 2U, 1U},
        {{"STAYING CLOSE", "FELT RIGHT"},
         {"I ALMOST", "CAUGHT IT"},
         {"I FOUND", "ITS TURN"},
         {"IT COPIED", "OUR ECHO"},
         {"THANKS FOR", "STAYING"},
         {"I SENT A", "WEIRD ECHO"}},
    },
    {
        StoryTrigger::QuietMoment,
        {"THREE NOTES", "REPEAT OUTSIDE"},
        {"DO WE ANSWER", "THE PATTERN?"},
        {"COPY", "WAIT", "HUM"},
        {{"THE NOTES", "COPIED US"},
         {"A FOURTH NOTE", "ARRIVED"},
         {"WE MADE", "A NEW SONG"}},
        {{StoryTone::Brave, 1, 0, 1},
         {StoryTone::Curious, 1, 1, 2},
         {StoryTone::Playful, 2, -1, 1}},
        {1U, 0U, 1U, 2U, 1U, 0U},
        {{"WE WAITED", "TOGETHER"},
         {"I ANSWERED", "AT ONCE"},
         {"THE PAUSE", "WAS THE CLUE"},
         {"OUR SONG", "WAS BETTER"},
         {"I LIKED", "THE QUIET PART"},
         {"I CHANGED", "THE LAST NOTE"}},
    },
    {
        StoryTrigger::ExpeditionReturn,
        {"I TOOK THE", "LONG WAY HOME"},
        {"AT THE FORK", "I CHOSE..."},
        {"LIGHTS", "RIVER", "STATIC"},
        {{"THE LIGHTS", "LED ME BACK"},
         {"THE RIVER", "KNEW THE WAY"},
         {"THE STATIC", "FELT FAMILIAR"}},
        {{StoryTone::Warm, 2, -1, 1},
         {StoryTone::Calm, 1, 1, 1},
         {StoryTone::Curious, 1, -1, 2}},
        {1U, 2U, 2U, 0U, 1U, 0U},
        {{"THE RIVER", "FELT KIND"},
         {"I CHASED", "EVERY LIGHT"},
         {"I MAPPED", "THE STATIC"},
         {"THE LIGHTS", "RACED ME"},
         {"QUIET WATER", "FELT SAFE"},
         {"I BEAT THE", "LIGHTS HOME"}},
    },
    {
        StoryTrigger::ExpeditionReturn,
        {"RAIN HID", "THE TRAIL"},
        {"SO I...", ""},
        {"RAN", "LISTENED", "SHELTERED"},
        {{"I OUTRAN", "THE RAIN"},
         {"DROPS MARKED", "THE PATH"},
         {"I WAITED", "THEN CAME HOME"}},
        {{StoryTone::Brave, 1, -2, 1},
         {StoryTone::Curious, 1, 0, 2},
         {StoryTone::Calm, 2, 1, 0}},
        {2U, 0U, 1U, 0U, 2U, 1U},
        {{"WAITING WAS", "THE WARM WAY"},
         {"RAIN COULD NOT", "CATCH ME"},
         {"I HEARD", "A DRY PATH"},
         {"WE SHOULD", "RACE THE RAIN"},
         {"I FOUND", "A QUIET ROOF"},
         {"I FOLLOWED", "RAIN'S SECRET"}},
    },
    {
        StoryTrigger::NearbySignal,
        {"A NEARBY PULSE", "SAID HELLO"},
        {"HOW SHOULD", "WE ANSWER?"},
        {"GREET", "ECHO", "WAIT"},
        {{"OUR HELLO", "CAME BACK"},
         {"TWO ECHOES", "BECAME THREE"},
         {"THEY WAITED", "WITH US"}},
        {{StoryTone::Warm, 2, 0, 1},
         {StoryTone::Playful, 1, -1, 2},
         {StoryTone::Calm, 1, 1, 1}},
        {0U, 0U, 1U, 1U, 2U, 1U},
        {{"A SOFT HELLO", "WAS ENOUGH"},
         {"I CALLED", "FIRST"},
         {"I COUNTED", "EVERY ECHO"},
         {"THE THIRD ECHO", "WAS MINE"},
         {"THEY STAYED", "NOT TOO CLOSE"},
         {"I ADDED", "AN EXTRA ECHO"}},
    },
    {
        StoryTrigger::NearbySignal,
        {"THREE SIGNALS", "FORMED A RING"},
        {"I STEPPED...", ""},
        {"INSIDE", "BESIDE", "BACK"},
        {{"THE RING", "MADE ROOM"},
         {"WE KEPT", "EQUAL DISTANCE"},
         {"THEY SENT", "A GOODBYE"}},
        {{StoryTone::Brave, 2, -1, 1},
         {StoryTone::Warm, 2, 0, 1},
         {StoryTone::Calm, 1, 1, 0}},
        {1U, 0U, 1U, 0U, 2U, 0U},
        {{"BESIDE THEM", "FELT RIGHT"},
         {"I WALKED", "RIGHT IN"},
         {"I FOUND", "THE OPEN SIDE"},
         {"THE RING", "STARTED A GAME"},
         {"I WAVED", "FROM HERE"},
         {"I CUT", "THROUGH IT"}},
    },
};

uint8_t personalityIndex(PersonalityKind personality) {
  const uint8_t value = static_cast<uint8_t>(personality);
  return value < kPersonalityCount ? value : 0U;
}

bool validAction(Action action) {
  return static_cast<uint8_t>(action) < kActionCount;
}

bool validOutcome(ActionOutcome outcome) {
  return static_cast<uint8_t>(outcome) <=
      static_cast<uint8_t>(ActionOutcome::NoReply);
}

bool validTrigger(StoryTrigger trigger) {
  return static_cast<uint8_t>(trigger) <=
      static_cast<uint8_t>(StoryTrigger::NearbySignal);
}

bool validChoice(StoryChoice choice) {
  return static_cast<uint8_t>(choice) < kStoryChoiceCount;
}

uint32_t mix32(uint32_t value) {
  value ^= value >> 16U;
  value *= UINT32_C(0x7FEB352D);
  value ^= value >> 15U;
  value *= UINT32_C(0x846CA68B);
  value ^= value >> 16U;
  return value;
}

bool validActionLineId(uint16_t id) {
  if (id >= 1U && id <=
      static_cast<uint16_t>((kActionCount - 1U) * 16U + kActionSlotCount)) {
    const uint16_t slot = static_cast<uint16_t>((id - 1U) & 15U);
    return slot < kActionSlotCount;
  }
  if (id >= kOutcomeIdBase) {
    const uint16_t offset = static_cast<uint16_t>(id - kOutcomeIdBase);
    const uint16_t outcome = static_cast<uint16_t>(offset >> 4U);
    const uint16_t slot = static_cast<uint16_t>(offset & 15U);
    return outcome < 3U && slot < kOutcomeSlotCount;
  }
  return false;
}

bool recentActionLine(const ActionState& state, uint16_t id) {
  for (uint8_t index = 0U; index < state.recentCount; ++index) {
    if (state.recent[index] == id) return true;
  }
  return false;
}

uint16_t newestActionLine(const ActionState& state) {
  if (state.recentCount == 0U) return 0U;
  const uint8_t index = static_cast<uint8_t>(
      (state.recentHead + kActionRecentCapacity - 1U) %
      kActionRecentCapacity);
  return state.recent[index];
}

void rememberActionLine(ActionState& state, uint16_t id) {
  state.recent[state.recentHead] = id;
  state.recentHead = static_cast<uint8_t>(
      (state.recentHead + 1U) % kActionRecentCapacity);
  if (state.recentCount < kActionRecentCapacity) ++state.recentCount;
}

void addCandidate(Candidate* candidates, uint8_t& count, const Text& text,
                  uint16_t id, LineFlavor flavor) {
  if (text.line1 == nullptr || text.line2 == nullptr) return;
  candidates[count].text = &text;
  candidates[count].id = id;
  candidates[count].flavor = flavor;
  ++count;
}

uint8_t chooseCandidate(const Candidate* candidates, uint8_t count,
                        uint32_t key, const ActionState& state) {
  const uint8_t start = static_cast<uint8_t>(key % count);
  for (uint8_t offset = 0U; offset < count; ++offset) {
    const uint8_t index = static_cast<uint8_t>((start + offset) % count);
    if (!recentActionLine(state, candidates[index].id)) return index;
  }
  const uint16_t newest = newestActionLine(state);
  if (count > 1U) {
    for (uint8_t offset = 0U; offset < count; ++offset) {
      const uint8_t index = static_cast<uint8_t>((start + offset) % count);
      if (candidates[index].id != newest) return index;
    }
  }
  return start;
}

bool validStoryId(uint8_t id) {
  return id >= 1U && id <= kStoryCount;
}

bool recentStory(const StoryState& state, uint8_t id) {
  for (uint8_t index = 0U; index < state.recentCount; ++index) {
    if (state.recent[index] == id) return true;
  }
  return false;
}

uint8_t newestStory(const StoryState& state) {
  if (state.recentCount == 0U) return 0U;
  const uint8_t index = static_cast<uint8_t>(
      (state.recentHead + kStoryRecentCapacity - 1U) %
      kStoryRecentCapacity);
  return state.recent[index];
}

void rememberStory(StoryState& state, uint8_t id) {
  state.recent[state.recentHead] = id;
  state.recentHead = static_cast<uint8_t>(
      (state.recentHead + 1U) % kStoryRecentCapacity);
  if (state.recentCount < kStoryRecentCapacity) ++state.recentCount;
}

}  // namespace

void resetActionState(ActionState& state) {
  state = ActionState{};
}

bool validateActionState(const ActionState& state) {
  if (state.recentHead >= kActionRecentCapacity ||
      state.recentCount > kActionRecentCapacity ||
      (state.recentCount < kActionRecentCapacity &&
       state.recentHead != state.recentCount)) {
    return false;
  }
  for (uint8_t index = 0U; index < kActionRecentCapacity; ++index) {
    if (index < state.recentCount) {
      if (!validActionLineId(state.recent[index])) return false;
    } else if (state.recent[index] != 0U) {
      return false;
    }
  }
  return true;
}

ActionLine selectActionLine(Action action, const ActionContext& context,
                            uint32_t companionFingerprint,
                            ActionState& state) {
  if (!validAction(action)) return ActionLine{};
  if (!validateActionState(state)) resetActionState(state);

  const uint8_t person = personalityIndex(context.personality);
  const ActionOutcome outcome = validOutcome(context.outcome)
      ? context.outcome
      : ActionOutcome::Failed;
  Candidate candidates[9]{};
  uint8_t count = 0U;

  if (outcome == ActionOutcome::Success) {
    const uint8_t actionIndex = static_cast<uint8_t>(action);
    const ActionSet& set = kActions[actionIndex];
    const uint16_t base = static_cast<uint16_t>(1U + actionIndex * 16U);
    for (uint8_t index = 0U; index < kGeneralLineCount; ++index) {
      addCandidate(candidates, count, set.general[index],
                   static_cast<uint16_t>(base + index),
                   LineFlavor::General);
    }
    addCandidate(candidates, count, set.personality[person],
                 static_cast<uint16_t>(base + 4U + person),
                 LineFlavor::Personality);

    const bool lowEnergy = context.vitals.energy <= 30U ||
        context.mood == CompanionMood::Drowsy ||
        context.mood == CompanionMood::Dreaming;
    const bool bonded = context.vitals.affection >= 70U ||
        context.bondLevel >= 4U || context.mood == CompanionMood::Loved ||
        context.mood == CompanionMood::Devoted;
    const bool curious = context.vitals.curiosity >= 65U ||
        context.mood == CompanionMood::Curious;
    if (lowEnergy) {
      addCandidate(candidates, count, set.lowEnergy,
                   static_cast<uint16_t>(base + 10U),
                   LineFlavor::LowEnergy);
    }
    if (bonded) {
      addCandidate(candidates, count, set.bonded,
                   static_cast<uint16_t>(base + 11U), LineFlavor::Bonded);
    }
    if (curious) {
      addCandidate(candidates, count, set.curious,
                   static_cast<uint16_t>(base + 12U), LineFlavor::Curious);
    }
    if (context.nearby) {
      addCandidate(candidates, count, set.nearby,
                   static_cast<uint16_t>(base + 13U), LineFlavor::Nearby);
    }
  } else {
    const uint8_t outcomeIndex = static_cast<uint8_t>(outcome) - 1U;
    const OutcomeSet& set = kOutcomes[outcomeIndex];
    const uint16_t base = static_cast<uint16_t>(
        kOutcomeIdBase + outcomeIndex * 16U);
    for (uint8_t index = 0U; index < kGeneralLineCount; ++index) {
      addCandidate(candidates, count, set.general[index],
                   static_cast<uint16_t>(base + index), LineFlavor::Outcome);
    }
    addCandidate(candidates, count, set.personality[person],
                 static_cast<uint16_t>(base + 4U + person),
                 LineFlavor::Outcome);
  }

  if (count == 0U) return ActionLine{};
  const uint32_t contextKey =
      (static_cast<uint32_t>(action) << 24U) ^
      (static_cast<uint32_t>(person) << 20U) ^
      (static_cast<uint32_t>(outcome) << 16U) ^
      (static_cast<uint32_t>(context.mood) << 8U) ^
      (context.nearby ? UINT32_C(0xA5A5) : 0U);
  const uint32_t key = mix32(companionFingerprint ^ contextKey) +
      state.selections;
  const uint8_t selected = chooseCandidate(candidates, count, key, state);
  ActionLine line;
  line.line1 = candidates[selected].text->line1;
  line.line2 = candidates[selected].text->line2;
  line.id = candidates[selected].id;
  line.flavor = candidates[selected].flavor;
  ++state.selections;
  rememberActionLine(state, line.id);
  return line;
}

void resetStoryState(StoryState& state) {
  state = StoryState{};
}

bool validateStoryState(const StoryState& state) {
  if ((state.completedMask & ~kStoryMask) != 0U ||
      state.recentHead >= kStoryRecentCapacity ||
      state.recentCount > kStoryRecentCapacity ||
      (state.recentCount < kStoryRecentCapacity &&
       state.recentHead != state.recentCount)) {
    return false;
  }
  if (state.activeStory == kNoActiveStory) {
    if (state.scene != 0U) return false;
  } else if (state.activeStory >= kStoryCount || state.scene > 1U) {
    return false;
  }
  for (uint8_t index = 0U; index < kStoryRecentCapacity; ++index) {
    if (index < state.recentCount) {
      if (!validStoryId(state.recent[index])) return false;
    } else if (state.recent[index] != 0U) {
      return false;
    }
  }
  return true;
}

bool currentStoryBeat(const StoryState& state, StoryBeat& beat) {
  beat = StoryBeat{};
  if (!validateStoryState(state) || state.activeStory == kNoActiveStory) {
    return false;
  }
  const StoryDefinition& story = kStories[state.activeStory];
  const Text& text = state.scene == 0U ? story.opening : story.prompt;
  beat.line1 = text.line1;
  beat.line2 = text.line2;
  beat.storyId = static_cast<uint8_t>(state.activeStory + 1U);
  beat.scene = state.scene;
  beat.awaitsChoice = state.scene == 1U;
  if (beat.awaitsChoice) {
    for (uint8_t index = 0U; index < kStoryChoiceCount; ++index) {
      beat.choices[index] = story.choices[index];
    }
  }
  return true;
}

bool startStory(StoryTrigger trigger, PersonalityKind personality,
                uint32_t companionFingerprint, StoryState& state,
                StoryBeat& beat) {
  beat = StoryBeat{};
  if (!validTrigger(trigger)) return false;
  if (!validateStoryState(state)) resetStoryState(state);
  if (state.activeStory != kNoActiveStory) return false;

  uint8_t candidates[kStoryCount]{};
  uint8_t count = 0U;
  for (uint8_t index = 0U; index < kStoryCount; ++index) {
    if (kStories[index].trigger == trigger) candidates[count++] = index;
  }
  if (count == 0U) return false;

  const uint8_t person = personalityIndex(personality);
  const uint32_t key = mix32(
      companionFingerprint ^ (static_cast<uint32_t>(trigger) << 24U) ^
      (static_cast<uint32_t>(person) << 16U)) + state.starts;
  const uint8_t start = static_cast<uint8_t>(key % count);
  uint8_t selected = candidates[start];
  for (uint8_t offset = 0U; offset < count; ++offset) {
    const uint8_t candidate = candidates[(start + offset) % count];
    if (!recentStory(state, static_cast<uint8_t>(candidate + 1U))) {
      selected = candidate;
      break;
    }
  }
  if (recentStory(state, static_cast<uint8_t>(selected + 1U)) && count > 1U) {
    const uint8_t newest = newestStory(state);
    for (uint8_t offset = 0U; offset < count; ++offset) {
      const uint8_t candidate = candidates[(start + offset) % count];
      if (static_cast<uint8_t>(candidate + 1U) != newest) {
        selected = candidate;
        break;
      }
    }
  }

  state.activeStory = selected;
  state.scene = 0U;
  ++state.starts;
  rememberStory(state, static_cast<uint8_t>(selected + 1U));
  return currentStoryBeat(state, beat);
}

bool advanceStory(StoryState& state, StoryBeat& beat) {
  beat = StoryBeat{};
  if (!validateStoryState(state) || state.activeStory == kNoActiveStory ||
      state.scene != 0U) {
    return false;
  }
  state.scene = 1U;
  return currentStoryBeat(state, beat);
}

bool resolveStory(StoryChoice choice, PersonalityKind personality,
                  StoryState& state, StoryResolution& resolution) {
  resolution = StoryResolution{};
  if (!validChoice(choice) || !validateStoryState(state) ||
      state.activeStory == kNoActiveStory || state.scene != 1U) {
    return false;
  }
  const uint8_t storyIndex = state.activeStory;
  const uint8_t choiceIndex = static_cast<uint8_t>(choice);
  const uint8_t person = personalityIndex(personality);
  const StoryDefinition& story = kStories[storyIndex];
  const bool personalityMatch = story.preferredChoice[person] == choiceIndex;
  const Text& text = personalityMatch
      ? story.preferredLines[person]
      : story.resolutions[choiceIndex];
  const StoryEffect& effect = story.effects[choiceIndex];

  resolution.line1 = text.line1;
  resolution.line2 = text.line2;
  resolution.storyId = static_cast<uint8_t>(storyIndex + 1U);
  resolution.tone = effect.tone;
  resolution.affectionDelta = effect.affection;
  resolution.energyDelta = effect.energy;
  resolution.curiosityDelta = effect.curiosity;
  resolution.personalityMatch = personalityMatch;

  state.completedMask |= UINT32_C(1) << storyIndex;
  if (state.completions != UINT16_MAX) ++state.completions;
  state.activeStory = kNoActiveStory;
  state.scene = 0U;
  return true;
}

void cancelStory(StoryState& state) {
  if (!validateStoryState(state)) {
    resetStoryState(state);
    return;
  }
  state.activeStory = kNoActiveStory;
  state.scene = 0U;
}

bool storyCompleted(const StoryState& state, uint8_t storyId) {
  return validateStoryState(state) && validStoryId(storyId) &&
      (state.completedMask &
       (UINT32_C(1) << static_cast<uint8_t>(storyId - 1U))) != 0U;
}

}  // namespace dialogue
}  // namespace kitsu868
