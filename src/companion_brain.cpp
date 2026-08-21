#include "companion_brain.h"

#include <string.h>

#ifdef ARDUINO
#include <Preferences.h>
#endif

namespace kitsu868 {
namespace {

constexpr char kPreferencesNamespace[] = "kitsu_brain";
constexpr char kSlotKeys[2][8] = {"brain_a", "brain_b"};

uint8_t axisFromSeed(uint32_t seed, uint8_t minimum, uint8_t span) {
  return static_cast<uint8_t>(minimum + (seed % span));
}

uint16_t saturatingAdd16(uint16_t value, uint16_t add) {
  const uint32_t total = static_cast<uint32_t>(value) + add;
  return total > 0xffffU ? 0xffffU : static_cast<uint16_t>(total);
}

}  // namespace

bool CompanionBrain::begin(const char* deviceUid, uint32_t packId) {
  initialize(deviceUid, packId);

#ifdef ARDUINO
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;

  storageAvailable_ = true;
  PersistedState candidates[2]{};
  bool valid[2] = {false, false};

  for (uint8_t slot = 0; slot < 2; ++slot) {
    if (preferences.getBytesLength(kSlotKeys[slot]) != sizeof(PersistedState)) {
      continue;
    }
    if (preferences.getBytes(kSlotKeys[slot], &candidates[slot],
                             sizeof(PersistedState)) !=
        sizeof(PersistedState)) {
      continue;
    }
    valid[slot] = validState(candidates[slot], state_.deviceFingerprint,
                             state_.packId);
  }
  preferences.end();

  if (valid[0] || valid[1]) {
    if (valid[0] && valid[1]) {
      activeSlot_ = generationNewer(candidates[1].generation,
                                    candidates[0].generation)
                        ? 1
                        : 0;
    } else {
      activeSlot_ = valid[1] ? 1 : 0;
    }
    state_ = candidates[activeSlot_];
    loadedFromStorage_ = true;
    dirty_ = false;
    derivePersonality();
    return true;
  }

  // initialize() already produced a complete fresh state.  Persist it to one
  // slot now, while still leaving the in-RAM companion usable if the write is
  // interrupted or NVS has no free page.
  flush();
  return true;
#else
  // Host builds intentionally exercise the exact same pure logic while
  // compiling the storage calls out.
  return false;
#endif
}

bool CompanionBrain::clearStoredState() {
#ifdef ARDUINO
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;
  const bool cleared = preferences.clear();
  preferences.end();
  return cleared;
#else
  return false;
#endif
}

void CompanionBrain::initialize(const char* deviceUid, uint32_t packId) {
  state_ = PersistedState{};
  state_.deviceFingerprint = fingerprint(deviceUid);
  state_.packId = packId;
  storageAvailable_ = false;
  loadedFromStorage_ = false;
  activeSlot_ = 0xff;
  dirty_ = true;
  derivePersonality();
  addMemory(BrainEvent::Hatch, 0, state_.packId);
}

bool CompanionBrain::flush() {
  if (!dirty_) return true;
  if (!storageAvailable_) return false;

#ifdef ARDUINO
  const uint8_t destination = activeSlot_ == 0 ? 1 : 0;
  PersistedState candidate = state_;
  candidate.magic = kMagic;
  candidate.schema = KITSU_BRAIN_SCHEMA_VERSION;
  candidate.bytes = sizeof(PersistedState);
  candidate.generation = state_.generation + 1U;
  candidate.crc32 = calculateCrc(candidate);

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;
  const size_t written = preferences.putBytes(
      kSlotKeys[destination], &candidate, sizeof(PersistedState));

  PersistedState verification{};
  const bool readBack = written == sizeof(PersistedState) &&
      preferences.getBytesLength(kSlotKeys[destination]) ==
          sizeof(PersistedState) &&
      preferences.getBytes(kSlotKeys[destination], &verification,
                           sizeof(PersistedState)) == sizeof(PersistedState);
  preferences.end();

  if (!readBack ||
      !validState(verification, candidate.deviceFingerprint,
                  candidate.packId) ||
      verification.generation != candidate.generation) {
    return false;
  }

  state_ = candidate;
  activeSlot_ = destination;
  dirty_ = false;
  return true;
#else
  return false;
#endif
}

CompanionMood CompanionBrain::mood(const CompanionVitals& vitals) const {
  if (vitals.sleeping) return CompanionMood::Dreaming;
  if (vitals.listening) return CompanionMood::Listening;

  if (state_.recentEventAgeMinutes <= 3U) {
    switch (static_cast<BrainEvent>(state_.recentEvent)) {
      case BrainEvent::Pet: return CompanionMood::Loved;
      case BrainEvent::Feed: return CompanionMood::Satisfied;
      case BrainEvent::Play:
      case BrainEvent::Game: return CompanionMood::Playful;
      case BrainEvent::PerfectGame:
      case BrainEvent::Evolve: return CompanionMood::Proud;
      case BrainEvent::Encounter: return CompanionMood::Startled;
      case BrainEvent::NewFriend: return CompanionMood::Excited;
      case BrainEvent::Wake: return CompanionMood::Awake;
      case BrainEvent::Sleep: return CompanionMood::Dreaming;
      default: break;
    }
  }

  if (vitals.energy <= 20U) return CompanionMood::Drowsy;
  if (vitals.affection <= 15U && state_.recentEventAgeMinutes >= 180U) {
    return CompanionMood::Lonely;
  }
  if (vitals.curiosity >= 80U && vitals.energy >= 45U) {
    return personality_.playfulness >= 70U ? CompanionMood::Excited
                                           : CompanionMood::Curious;
  }
  if (vitals.affection >= 75U) return CompanionMood::Devoted;
  if (personality_.kind == PersonalityKind::Impish &&
      vitals.curiosity >= 55U && vitals.energy >= 40U) {
    return CompanionMood::Impish;
  }
  if (vitals.curiosity >= 55U) return CompanionMood::Curious;
  return CompanionMood::Content;
}

uint8_t CompanionBrain::bondLevel() const {
  for (int8_t level = 10; level >= 0; --level) {
    if (state_.bondXp >= bondThreshold(static_cast<uint8_t>(level))) {
      return static_cast<uint8_t>(level);
    }
  }
  return 0;
}

uint16_t CompanionBrain::xpUntilNextBond() const {
  const uint8_t level = bondLevel();
  if (level >= 10U) return 0;
  return static_cast<uint16_t>(bondThreshold(level + 1U) - state_.bondXp);
}

uint8_t CompanionBrain::bondProgressPercent() const {
  const uint8_t level = bondLevel();
  if (level >= 10U) return 100;
  const uint16_t floor = bondThreshold(level);
  const uint16_t ceiling = bondThreshold(level + 1U);
  const uint16_t span = static_cast<uint16_t>(ceiling - floor);
  return static_cast<uint8_t>(
      (static_cast<uint32_t>(state_.bondXp - floor) * 100U) / span);
}

EvolutionStage CompanionBrain::evolutionStage() const {
  const uint8_t level = bondLevel();
  const uint32_t peers = state_.lifetime.uniqueEncounters;
  if (level >= 10U && peers >= 8U) return EvolutionStage::Ascended;
  if (level >= 8U && peers >= 3U) return EvolutionStage::Resonant;
  if (level >= 5U && peers >= 1U) return EvolutionStage::Trusted;
  if (level >= 2U) return EvolutionStage::Familiar;
  return EvolutionStage::NewSignal;
}

uint16_t CompanionBrain::unlockMask() const {
  const uint8_t level = bondLevel();
  const EvolutionStage stage = evolutionStage();
  uint16_t mask = 0;
  if (level >= 1U) mask |= UnlockGreeting;
  if (level >= 2U) mask |= UnlockJournal;
  if (level >= 3U) mask |= UnlockMiniGame;
  if (stage >= EvolutionStage::Familiar) {
    mask |= UnlockAccessory | UnlockVariant1;
  }
  if (level >= 5U) mask |= UnlockDream;
  if (state_.lifetime.uniqueEncounters >= 1U) mask |= UnlockGift;
  if (stage >= EvolutionStage::Trusted) mask |= UnlockVariant2;
  if (level >= 7U) mask |= UnlockRareReaction;
  if (stage >= EvolutionStage::Resonant) mask |= UnlockAura;
  if (stage >= EvolutionStage::Ascended) mask |= UnlockFinalForm;
  return mask;
}

void CompanionBrain::advanceMinutes(uint16_t minutes) {
  if (minutes == 0U) return;
  state_.lifetime.poweredMinutes =
      saturatingAdd(state_.lifetime.poweredMinutes, minutes);
  state_.recentEventAgeMinutes =
      saturatingAdd16(state_.recentEventAgeMinutes, minutes);
  dirty_ = true;
}

void CompanionBrain::syncSleeping(bool sleeping) {
  const bool wasSleeping = (state_.flags & kFlagSleeping) != 0;
  if (wasSleeping == sleeping) return;
  if (sleeping) state_.flags |= kFlagSleeping;
  else state_.flags &= static_cast<uint8_t>(~kFlagSleeping);
  dirty_ = true;
}

BrainEventResult CompanionBrain::onPet() {
  const uint8_t before = bondLevel();
  const EvolutionStage stage = evolutionStage();
  state_.lifetime.pets = saturatingAdd(state_.lifetime.pets, 1);
  return recordEvent(BrainEvent::Pet, 2, 0, state_.lifetime.pets,
                     before, stage);
}

BrainEventResult CompanionBrain::onFeed() {
  const uint8_t before = bondLevel();
  const EvolutionStage stage = evolutionStage();
  state_.lifetime.feeds = saturatingAdd(state_.lifetime.feeds, 1);
  return recordEvent(BrainEvent::Feed, 4, 0, state_.lifetime.feeds,
                     before, stage);
}

BrainEventResult CompanionBrain::onPlay() {
  const uint8_t before = bondLevel();
  const EvolutionStage stage = evolutionStage();
  state_.lifetime.plays = saturatingAdd(state_.lifetime.plays, 1);
  return recordEvent(BrainEvent::Play, 5, 0, state_.lifetime.plays,
                     before, stage);
}

BrainEventResult CompanionBrain::onSleep() {
  const uint8_t before = bondLevel();
  const EvolutionStage stage = evolutionStage();
  if ((state_.flags & kFlagSleeping) != 0) {
    BrainEventResult result{};
    result.bondBefore = result.bondAfter = before;
    result.stageBefore = result.stageAfter = stage;
    return result;
  }
  state_.flags |= kFlagSleeping;
  state_.lifetime.sleepSessions =
      saturatingAdd(state_.lifetime.sleepSessions, 1);
  return recordEvent(BrainEvent::Sleep, 2, 0,
                     state_.lifetime.sleepSessions, before, stage);
}

BrainEventResult CompanionBrain::onWake() {
  const uint8_t before = bondLevel();
  const EvolutionStage stage = evolutionStage();
  if ((state_.flags & kFlagSleeping) == 0) {
    BrainEventResult result{};
    result.bondBefore = result.bondAfter = before;
    result.stageBefore = result.stageAfter = stage;
    return result;
  }
  state_.flags &= static_cast<uint8_t>(~kFlagSleeping);
  state_.lifetime.wakes = saturatingAdd(state_.lifetime.wakes, 1);
  return recordEvent(BrainEvent::Wake, 1, 0, state_.lifetime.wakes,
                     before, stage);
}

BrainEventResult CompanionBrain::onGame(uint8_t score, bool perfect) {
  if (score > 100U) score = 100U;
  perfect = perfect || score == 100U;
  const uint8_t before = bondLevel();
  const EvolutionStage stage = evolutionStage();
  state_.lifetime.gamesPlayed =
      saturatingAdd(state_.lifetime.gamesPlayed, 1);
  if (perfect) {
    state_.lifetime.perfectGames =
        saturatingAdd(state_.lifetime.perfectGames, 1);
  }
  const uint16_t reward = static_cast<uint16_t>(
      2U + score / 20U + (perfect ? 3U : 0U));
  return recordEvent(perfect ? BrainEvent::PerfectGame : BrainEvent::Game,
                     reward, score, state_.lifetime.gamesPlayed,
                     before, stage);
}

BrainEventResult CompanionBrain::onEncounter(uint32_t peerFingerprint) {
  const uint8_t before = bondLevel();
  const EvolutionStage stage = evolutionStage();
  const bool isNew = !peerSeen(peerFingerprint);
  rememberPeer(peerFingerprint);
  state_.lifetime.encounters =
      saturatingAdd(state_.lifetime.encounters, 1);
  if (isNew) {
    state_.lifetime.uniqueEncounters =
        saturatingAdd(state_.lifetime.uniqueEncounters, 1);
  }
  return recordEvent(isNew ? BrainEvent::NewFriend : BrainEvent::Encounter,
                     isNew ? 12U : 2U,
                     static_cast<uint8_t>(
                         state_.lifetime.uniqueEncounters > 255U
                             ? 255U
                             : state_.lifetime.uniqueEncounters),
                     peerFingerprint, before, stage, isNew);
}

bool CompanionBrain::recentMemory(uint8_t newestIndex,
                                  MemoryEntry& entry) const {
  if (newestIndex >= state_.memoryCount) return false;
  const uint8_t index = static_cast<uint8_t>(
      (state_.memoryHead + KITSU_BRAIN_MEMORY_CAPACITY - 1U - newestIndex) %
      KITSU_BRAIN_MEMORY_CAPACITY);
  entry = state_.memories[index];
  return true;
}

uint32_t CompanionBrain::fingerprint(const char* text) {
  uint32_t hash = 2166136261UL;
  if (!text) return hash;
  while (*text) {
    hash ^= static_cast<uint8_t>(*text++);
    hash *= 16777619UL;
  }
  return hash;
}

const char* CompanionBrain::personalityLabel(PersonalityKind kind) {
  switch (kind) {
    case PersonalityKind::Gentle: return "GENTLE";
    case PersonalityKind::Bold: return "BOLD";
    case PersonalityKind::Curious: return "CURIOUS";
    case PersonalityKind::Playful: return "PLAYFUL";
    case PersonalityKind::Shy: return "SHY";
    case PersonalityKind::Impish: return "IMPISH";
  }
  return "GENTLE";
}

const char* CompanionBrain::moodLabel(CompanionMood mood) {
  switch (mood) {
    case CompanionMood::Content: return "CONTENT";
    case CompanionMood::Dreaming: return "DREAMING";
    case CompanionMood::Listening: return "LISTENING";
    case CompanionMood::Drowsy: return "DROWSY";
    case CompanionMood::Lonely: return "LONELY";
    case CompanionMood::Curious: return "CURIOUS";
    case CompanionMood::Excited: return "EXCITED";
    case CompanionMood::Devoted: return "DEVOTED";
    case CompanionMood::Impish: return "IMPISH";
    case CompanionMood::Loved: return "LOVED";
    case CompanionMood::Satisfied: return "SATISFIED";
    case CompanionMood::Playful: return "PLAYFUL";
    case CompanionMood::Proud: return "PROUD";
    case CompanionMood::Startled: return "STARTLED";
    case CompanionMood::Awake: return "AWAKE";
  }
  return "CONTENT";
}

const char* CompanionBrain::stageLabel(EvolutionStage stage) {
  switch (stage) {
    case EvolutionStage::NewSignal: return "NEW";
    case EvolutionStage::Familiar: return "FAMILIAR";
    case EvolutionStage::Trusted: return "TRUSTED";
    case EvolutionStage::Resonant: return "RESONANT";
    case EvolutionStage::Ascended: return "ASCENDED";
  }
  return "NEW";
}

MemoryText CompanionBrain::memoryText(const MemoryEntry& memory) {
  switch (memory.event) {
    case BrainEvent::Hatch: return {"FIRST", "SIGNAL"};
    case BrainEvent::Pet: return {"GENTLE", "PETS"};
    case BrainEvent::Feed: return {"GOOD", "MEAL"};
    case BrainEvent::Play: return {"PLAYED", "TOGETHER"};
    case BrainEvent::Sleep: return {"DREAM", "TIME"};
    case BrainEvent::Wake: return {"AWAKE", "AGAIN"};
    case BrainEvent::Game: return {"GAME", "CLEARED"};
    case BrainEvent::PerfectGame: return {"PERFECT", "TIMING"};
    case BrainEvent::Encounter: return {"SIGNAL", "RETURNED"};
    case BrainEvent::NewFriend: return {"NEW", "SIGNAL"};
    case BrainEvent::BondUp: return {"BOND", "LEVEL UP"};
    case BrainEvent::Evolve: return {"NEW", "FORM"};
  }
  return {"QUIET", "MEMORY"};
}

uint32_t CompanionBrain::mix32(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352dUL;
  value ^= value >> 15;
  value *= 0x846ca68bUL;
  value ^= value >> 16;
  return value;
}

uint32_t CompanionBrain::calculateCrc(const PersistedState& state) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&state);
  uint32_t crc = 0xffffffffUL;
  for (size_t index = 0; index < offsetof(PersistedState, crc32); ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320UL &
          static_cast<uint32_t>(-static_cast<int32_t>(crc & 1U)));
    }
  }
  return ~crc;
}

bool CompanionBrain::generationNewer(uint32_t left, uint32_t right) {
  return static_cast<int32_t>(left - right) > 0;
}

uint32_t CompanionBrain::saturatingAdd(uint32_t value, uint32_t add) {
  return value > 0xffffffffUL - add ? 0xffffffffUL : value + add;
}

uint16_t CompanionBrain::bondThreshold(uint8_t level) {
  static constexpr uint16_t kThresholds[11] = {
      0, 15, 35, 65, 105, 155, 220, 300, 395, 505, 630};
  return kThresholds[level > 10U ? 10U : level];
}

bool CompanionBrain::validState(const PersistedState& candidate,
                                uint32_t expectedFingerprint,
                                uint32_t expectedPackId) const {
  if (candidate.magic != kMagic ||
      candidate.schema != KITSU_BRAIN_SCHEMA_VERSION ||
      candidate.bytes != sizeof(PersistedState) ||
      candidate.deviceFingerprint != expectedFingerprint ||
      candidate.packId != expectedPackId ||
      candidate.memoryHead >= KITSU_BRAIN_MEMORY_CAPACITY ||
      candidate.memoryCount > KITSU_BRAIN_MEMORY_CAPACITY ||
      candidate.recentEvent > static_cast<uint8_t>(BrainEvent::Evolve) ||
      (candidate.flags & static_cast<uint8_t>(~kFlagSleeping)) != 0 ||
      candidate.bondXp > kMaximumBondXp) {
    return false;
  }
  return candidate.crc32 == calculateCrc(candidate);
}

void CompanionBrain::derivePersonality() {
  const uint32_t seed = mix32(state_.deviceFingerprint ^
      mix32(state_.packId ^ 0x4b697473UL));
  const uint32_t second = mix32(seed + 0x9e3779b9UL);
  const uint32_t third = mix32(second + 0x9e3779b9UL);
  const uint32_t fourth = mix32(third + 0x9e3779b9UL);

  personality_.warmth = axisFromSeed(seed, 30, 64);       // 30..93
  personality_.playfulness = axisFromSeed(second, 25, 71);  // 25..95
  personality_.boldness = axisFromSeed(third, 20, 72);    // 20..91
  personality_.curiosity = axisFromSeed(fourth, 30, 66);  // 30..95

  if (personality_.boldness < 35U && personality_.warmth < 65U) {
    personality_.kind = PersonalityKind::Shy;
  } else if (personality_.playfulness >= 80U &&
             personality_.boldness >= 58U) {
    personality_.kind = PersonalityKind::Impish;
  } else if (personality_.warmth >= personality_.boldness &&
             personality_.warmth >= personality_.curiosity &&
             personality_.warmth >= personality_.playfulness) {
    personality_.kind = PersonalityKind::Gentle;
  } else if (personality_.boldness >= personality_.curiosity &&
             personality_.boldness >= personality_.playfulness) {
    personality_.kind = PersonalityKind::Bold;
  } else if (personality_.curiosity >= personality_.playfulness) {
    personality_.kind = PersonalityKind::Curious;
  } else {
    personality_.kind = PersonalityKind::Playful;
  }
}

bool CompanionBrain::peerSeen(uint32_t peerFingerprint) const {
  const uint32_t first = mix32(peerFingerprint ^ state_.deviceFingerprint);
  const uint32_t stride =
      mix32(peerFingerprint ^ state_.packId ^ 0xa5b35705UL) | 1U;
  for (uint8_t probe = 0; probe < 3; ++probe) {
    const uint8_t bit = static_cast<uint8_t>(
        first + static_cast<uint32_t>(probe) * stride +
        static_cast<uint32_t>(probe) * probe * 17U);
    if ((state_.peerBloom[bit >> 5] & (1UL << (bit & 31U))) == 0) {
      return false;
    }
  }
  return true;
}

void CompanionBrain::rememberPeer(uint32_t peerFingerprint) {
  const uint32_t first = mix32(peerFingerprint ^ state_.deviceFingerprint);
  const uint32_t stride =
      mix32(peerFingerprint ^ state_.packId ^ 0xa5b35705UL) | 1U;
  for (uint8_t probe = 0; probe < 3; ++probe) {
    const uint8_t bit = static_cast<uint8_t>(
        first + static_cast<uint32_t>(probe) * stride +
        static_cast<uint32_t>(probe) * probe * 17U);
    state_.peerBloom[bit >> 5] |= 1UL << (bit & 31U);
  }
  dirty_ = true;
}

void CompanionBrain::addMemory(BrainEvent event, uint8_t detail,
                               uint32_t value) {
  MemoryEntry& memory = state_.memories[state_.memoryHead];
  memory.event = event;
  memory.detail = detail;
  memory.sequence = ++state_.memorySequence;
  memory.value = value;
  state_.memoryHead = static_cast<uint8_t>(
      (state_.memoryHead + 1U) % KITSU_BRAIN_MEMORY_CAPACITY);
  if (state_.memoryCount < KITSU_BRAIN_MEMORY_CAPACITY) {
    ++state_.memoryCount;
  }
  dirty_ = true;
}

BrainEventResult CompanionBrain::recordEvent(
    BrainEvent event, uint16_t baseXp, uint8_t detail, uint32_t value,
    uint8_t bondBefore, EvolutionStage stageBefore, bool newEncounter) {
  uint16_t award = baseXp;
  if (state_.recentEvent == static_cast<uint8_t>(event) &&
      state_.recentEventAgeMinutes < 10U) {
    if (state_.repeatCount < 0xffU) ++state_.repeatCount;
    award = state_.repeatCount == 1U
                ? static_cast<uint16_t>((baseXp + 1U) / 2U)
                : static_cast<uint16_t>(baseXp == 0U ? 0U : 1U);
  } else {
    state_.repeatCount = 0;
  }

  state_.recentEvent = static_cast<uint8_t>(event);
  state_.recentEventAgeMinutes = 0;
  const uint32_t increased = static_cast<uint32_t>(state_.bondXp) + award;
  state_.bondXp = increased > kMaximumBondXp
                      ? kMaximumBondXp
                      : static_cast<uint16_t>(increased);
  addMemory(event, detail, value);

  const uint8_t bondAfter = bondLevel();
  const EvolutionStage stageAfter = evolutionStage();
  if (bondAfter != bondBefore) addMemory(BrainEvent::BondUp, bondAfter, 0);
  if (stageAfter != stageBefore) {
    addMemory(BrainEvent::Evolve, static_cast<uint8_t>(stageAfter), 0);
  }
  dirty_ = true;

  BrainEventResult result{};
  result.xpAwarded = award;
  result.bondBefore = bondBefore;
  result.bondAfter = bondAfter;
  result.stageBefore = stageBefore;
  result.stageAfter = stageAfter;
  result.newEncounter = newEncounter;
  return result;
}

}  // namespace kitsu868
