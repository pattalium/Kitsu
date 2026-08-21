#pragma once

#include <stddef.h>
#include <stdint.h>

// CompanionBrain is intentionally independent of the display, radio, animation
// pack, and Arduino String classes.  The same progression rules can therefore
// be exercised by a normal host C++ test.  On ESP32, begin()/flush() add a
// double-buffered Preferences backend.
namespace kitsu868 {

constexpr uint8_t KITSU_BRAIN_MEMORY_CAPACITY = 24;
constexpr uint8_t KITSU_BRAIN_SCHEMA_VERSION = 1;
constexpr size_t KITSU_BRAIN_PERSISTED_BYTES = 300;
constexpr size_t KITSU_BRAIN_STORED_BYTES = KITSU_BRAIN_PERSISTED_BYTES * 2;

enum class PersonalityKind : uint8_t {
  Gentle = 0,
  Bold,
  Curious,
  Playful,
  Shy,
  Impish,
};

struct PersonalityTraits {
  PersonalityKind kind = PersonalityKind::Gentle;
  uint8_t warmth = 50;
  uint8_t playfulness = 50;
  uint8_t boldness = 50;
  uint8_t curiosity = 50;
};

enum class CompanionMood : uint8_t {
  Content = 0,
  Dreaming,
  Listening,
  Drowsy,
  Lonely,
  Curious,
  Excited,
  Devoted,
  Impish,
  Loved,
  Satisfied,
  Playful,
  Proud,
  Startled,
  Awake,
};

// These are the existing Kitsu868 stats.  CompanionBrain reads them but never
// mutates them, so adopting the module does not alter current stat semantics.
struct CompanionVitals {
  uint8_t energy = 72;
  uint8_t curiosity = 14;
  uint8_t affection = 5;
  bool sleeping = false;
  bool listening = false;
};

enum class EvolutionStage : uint8_t {
  NewSignal = 0,
  Familiar,
  Trusted,
  Resonant,
  Ascended,
};

enum CompanionUnlock : uint16_t {
  UnlockGreeting = 1U << 0,
  UnlockJournal = 1U << 1,
  UnlockMiniGame = 1U << 2,
  UnlockAccessory = 1U << 3,
  UnlockDream = 1U << 4,
  UnlockGift = 1U << 5,
  UnlockVariant1 = 1U << 6,
  UnlockVariant2 = 1U << 7,
  UnlockRareReaction = 1U << 8,
  UnlockAura = 1U << 9,
  UnlockFinalForm = 1U << 10,
};

enum class BrainEvent : uint8_t {
  Hatch = 0,
  Pet,
  Feed,
  Play,
  Sleep,
  Wake,
  Game,
  PerfectGame,
  Encounter,
  NewFriend,
  BondUp,
  Evolve,
};

#pragma pack(push, 1)
struct MemoryEntry {
  BrainEvent event = BrainEvent::Hatch;
  uint8_t detail = 0;
  uint16_t sequence = 0;
  uint32_t value = 0;
};

struct LifetimeCounters {
  uint32_t poweredMinutes = 0;
  uint32_t pets = 0;
  uint32_t feeds = 0;
  uint32_t plays = 0;
  uint32_t sleepSessions = 0;
  uint32_t wakes = 0;
  uint32_t gamesPlayed = 0;
  uint32_t perfectGames = 0;
  uint32_t encounters = 0;
  uint32_t uniqueEncounters = 0;
};
#pragma pack(pop)

static_assert(sizeof(MemoryEntry) == 8, "A journal entry must remain compact");
static_assert(sizeof(LifetimeCounters) == 40,
              "Lifetime counter layout changed unexpectedly");

struct MemoryText {
  const char* line1;
  const char* line2;
};

struct BrainEventResult {
  uint16_t xpAwarded = 0;
  uint8_t bondBefore = 0;
  uint8_t bondAfter = 0;
  EvolutionStage stageBefore = EvolutionStage::NewSignal;
  EvolutionStage stageAfter = EvolutionStage::NewSignal;
  bool newEncounter = false;

  bool bondChanged() const { return bondAfter != bondBefore; }
  bool evolved() const { return stageAfter != stageBefore; }
};

class CompanionBrain {
 public:
  // Loads the matching UID + pack state from the two Preferences slots.  A
  // fresh state is created when no valid slot exists.  The return value says
  // whether Preferences was available; the brain remains usable when false.
  bool begin(const char* deviceUid, uint32_t packId);

  // Pure, storage-free initializer used by host tests and callers that want an
  // explicitly fresh companion.
  void initialize(const char* deviceUid, uint32_t packId);

  // Removes both redundant Preferences slots.  Use this before beginning a
  // genuinely different one-slot companion so an older pack cannot revive a
  // dormant bond/journal if it is installed again later.
  static bool clearStoredState();

  // Writes the dirty state to the inactive CRC-protected slot, then makes that
  // slot current.  Repeated calls with no changes do not write flash.
  bool flush();

  bool storageAvailable() const { return storageAvailable_; }
  bool loadedFromStorage() const { return loadedFromStorage_; }
  bool dirty() const { return dirty_; }

  uint32_t deviceFingerprint() const { return state_.deviceFingerprint; }
  uint32_t packId() const { return state_.packId; }
  const PersonalityTraits& personality() const { return personality_; }
  CompanionMood mood(const CompanionVitals& vitals) const;

  uint16_t bondXp() const { return state_.bondXp; }
  uint8_t bondLevel() const;
  uint16_t xpUntilNextBond() const;
  uint8_t bondProgressPercent() const;
  EvolutionStage evolutionStage() const;
  uint8_t appearanceVariant() const {
    return static_cast<uint8_t>(evolutionStage());
  }
  uint16_t unlockMask() const;

  const LifetimeCounters& lifetime() const { return state_.lifetime; }

  // Call from the existing slow stat/age tick.  Values saturate rather than
  // wrapping, including across millis() rollover handled by the caller.
  void advanceMinutes(uint16_t minutes);

  // Keeps the internal transition guard aligned with a legacy saved sleep
  // state without awarding XP or adding a memory.
  void syncSleeping(bool sleeping);

  BrainEventResult onPet();
  BrainEventResult onFeed();
  BrainEventResult onPlay();
  BrainEventResult onSleep();
  BrainEventResult onWake();
  BrainEventResult onGame(uint8_t score, bool perfect = false);

  // peerFingerprint should be derived from the peer's public KT UID with
  // fingerprint().  The returned newEncounter flag is false for repeats.  A
  // 256-bit, three-probe persistent filter prevents replay farming without
  // storing raw peer identifiers.
  BrainEventResult onEncounter(uint32_t peerFingerprint);

  uint8_t memoryCount() const { return state_.memoryCount; }
  bool recentMemory(uint8_t newestIndex, MemoryEntry& entry) const;

  static uint32_t fingerprint(const char* text);
  static const char* personalityLabel(PersonalityKind kind);
  static const char* moodLabel(CompanionMood mood);
  static const char* stageLabel(EvolutionStage stage);
  static MemoryText memoryText(const MemoryEntry& memory);

 private:
  static constexpr uint32_t kMagic = 0x4B425231UL;  // "KBR1"
  static constexpr uint16_t kMaximumBondXp = 630;
  static constexpr uint8_t kFlagSleeping = 1U << 0;

#pragma pack(push, 1)
  struct PersistedState {
    uint32_t magic = kMagic;
    uint16_t schema = KITSU_BRAIN_SCHEMA_VERSION;
    uint16_t bytes = KITSU_BRAIN_PERSISTED_BYTES;
    uint32_t generation = 0;
    uint32_t deviceFingerprint = 0;
    uint32_t packId = 0;
    uint16_t bondXp = 0;
    uint8_t recentEvent = static_cast<uint8_t>(BrainEvent::Hatch);
    uint8_t repeatCount = 0;
    uint16_t recentEventAgeMinutes = 0;
    uint16_t memorySequence = 0;
    uint8_t memoryHead = 0;
    uint8_t memoryCount = 0;
    uint8_t flags = 0;
    uint8_t reserved = 0;
    LifetimeCounters lifetime{};
    uint32_t peerBloom[8]{};  // 256 bits, no raw peer IDs.
    MemoryEntry memories[KITSU_BRAIN_MEMORY_CAPACITY]{};
    uint32_t crc32 = 0;
  };
#pragma pack(pop)

  static_assert(sizeof(PersistedState) == KITSU_BRAIN_PERSISTED_BYTES,
                "Update schema when the persistent brain layout changes");

  static uint32_t mix32(uint32_t value);
  static uint32_t calculateCrc(const PersistedState& state);
  static bool generationNewer(uint32_t left, uint32_t right);
  static uint32_t saturatingAdd(uint32_t value, uint32_t add);
  static uint16_t bondThreshold(uint8_t level);

  bool validState(const PersistedState& candidate,
                  uint32_t expectedFingerprint,
                  uint32_t expectedPackId) const;
  void derivePersonality();
  bool peerSeen(uint32_t peerFingerprint) const;
  void rememberPeer(uint32_t peerFingerprint);
  void addMemory(BrainEvent event, uint8_t detail, uint32_t value);
  BrainEventResult recordEvent(BrainEvent event, uint16_t baseXp,
                               uint8_t detail, uint32_t value,
                               uint8_t bondBefore,
                               EvolutionStage stageBefore,
                               bool newEncounter = false);

  PersistedState state_{};
  PersonalityTraits personality_{};
  bool storageAvailable_ = false;
  bool loadedFromStorage_ = false;
  bool dirty_ = false;
  uint8_t activeSlot_ = 0xff;
};

}  // namespace kitsu868
