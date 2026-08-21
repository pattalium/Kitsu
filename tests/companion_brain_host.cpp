#include "companion_brain.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace kitsu868;

namespace {

void assertPortraitLabel(const char* text) {
  assert(text != nullptr);
  assert(strlen(text) <= 10U);  // Ten 5x7 glyph cells fit the 64 px canvas.
}

void testDeterministicPersonality() {
  CompanionBrain first;
  CompanionBrain again;
  CompanionBrain otherPack;
  first.initialize("KTDEAD", 0x13579BDFUL);
  again.initialize("KTDEAD", 0x13579BDFUL);
  otherPack.initialize("KTDEAD", 0x13579BE0UL);

  const PersonalityTraits& a = first.personality();
  const PersonalityTraits& b = again.personality();
  const PersonalityTraits& c = otherPack.personality();
  assert(a.kind == b.kind);
  assert(a.warmth == b.warmth);
  assert(a.playfulness == b.playfulness);
  assert(a.boldness == b.boldness);
  assert(a.curiosity == b.curiosity);
  assert(a.kind != c.kind || a.warmth != c.warmth ||
         a.playfulness != c.playfulness || a.boldness != c.boldness ||
         a.curiosity != c.curiosity);
}

void testMoodAndDiminishingReturns() {
  CompanionBrain brain;
  brain.initialize("KTDEAD", 0x13579BDFUL);
  CompanionVitals vitals;
  assert(brain.mood(vitals) == CompanionMood::Content);

  vitals.sleeping = true;
  assert(brain.mood(vitals) == CompanionMood::Dreaming);
  vitals.sleeping = false;
  vitals.listening = true;
  assert(brain.mood(vitals) == CompanionMood::Listening);
  vitals.listening = false;
  vitals.energy = 12;
  assert(brain.mood(vitals) == CompanionMood::Drowsy);

  vitals.energy = 72;
  brain.advanceMinutes(180);
  assert(brain.mood(vitals) == CompanionMood::Lonely);

  BrainEventResult event = brain.onPet();
  assert(event.xpAwarded == 2);
  assert(brain.mood(vitals) == CompanionMood::Loved);
  event = brain.onPet();
  assert(event.xpAwarded == 1);
  event = brain.onPet();
  assert(event.xpAwarded == 1);
  brain.advanceMinutes(10);
  event = brain.onPet();
  assert(event.xpAwarded == 2);
}

void testSleepGuardsAndJournalRing() {
  CompanionBrain brain;
  brain.initialize("KTDEAD", 0x13579BDFUL);
  assert(brain.onSleep().xpAwarded == 2);
  assert(brain.onSleep().xpAwarded == 0);
  assert(brain.lifetime().sleepSessions == 1);
  assert(brain.onWake().xpAwarded == 1);
  assert(brain.onWake().xpAwarded == 0);
  assert(brain.lifetime().wakes == 1);

  for (uint8_t index = 0; index < 40; ++index) {
    brain.advanceMinutes(10);
    brain.onPlay();
  }
  assert(brain.memoryCount() == KITSU_BRAIN_MEMORY_CAPACITY);
  MemoryEntry newest{};
  MemoryEntry oldest{};
  assert(brain.recentMemory(0, newest));
  assert(brain.recentMemory(KITSU_BRAIN_MEMORY_CAPACITY - 1U, oldest));
  assert(newest.sequence != oldest.sequence);
  assert(!brain.recentMemory(KITSU_BRAIN_MEMORY_CAPACITY, oldest));
}

void testEncountersBondAndEvolution() {
  CompanionBrain brain;
  brain.initialize("KTDEAD", 0x13579BDFUL);
  const uint32_t firstPeer = CompanionBrain::fingerprint("KT1111");
  BrainEventResult encounter = brain.onEncounter(firstPeer);
  assert(encounter.newEncounter);
  assert(encounter.xpAwarded == 12);
  encounter = brain.onEncounter(firstPeer);
  assert(!encounter.newEncounter);
  assert(brain.lifetime().encounters == 2);
  assert(brain.lifetime().uniqueEncounters == 1);

  const char* peers[] = {
      "KT2222", "KT3333", "KT4444", "KT5555",
      "KT6666", "KT7777", "KT8888"};
  for (const char* peer : peers) {
    brain.advanceMinutes(10);
    encounter = brain.onEncounter(CompanionBrain::fingerprint(peer));
    assert(encounter.newEncounter);
  }
  assert(brain.lifetime().uniqueEncounters == 8);

  for (uint8_t game = 0; game < 60 && brain.bondLevel() < 10U; ++game) {
    brain.advanceMinutes(10);
    brain.onGame(100, true);
  }
  assert(brain.bondXp() == 630);
  assert(brain.bondLevel() == 10);
  assert(brain.evolutionStage() == EvolutionStage::Ascended);
  assert(brain.appearanceVariant() == 4);
  assert((brain.unlockMask() & UnlockFinalForm) != 0);
  assert(brain.xpUntilNextBond() == 0);
  assert(brain.bondProgressPercent() == 100);
}

void testPortraitLabels() {
  for (uint8_t value = 0;
       value <= static_cast<uint8_t>(PersonalityKind::Impish); ++value) {
    assertPortraitLabel(CompanionBrain::personalityLabel(
        static_cast<PersonalityKind>(value)));
  }
  for (uint8_t value = 0;
       value <= static_cast<uint8_t>(CompanionMood::Awake); ++value) {
    assertPortraitLabel(CompanionBrain::moodLabel(
        static_cast<CompanionMood>(value)));
  }
  for (uint8_t value = 0;
       value <= static_cast<uint8_t>(EvolutionStage::Ascended); ++value) {
    assertPortraitLabel(CompanionBrain::stageLabel(
        static_cast<EvolutionStage>(value)));
  }
  for (uint8_t value = 0;
       value <= static_cast<uint8_t>(BrainEvent::Evolve); ++value) {
    MemoryEntry memory{};
    memory.event = static_cast<BrainEvent>(value);
    const MemoryText text = CompanionBrain::memoryText(memory);
    assertPortraitLabel(text.line1);
    assertPortraitLabel(text.line2);
  }
}

}  // namespace

int main() {
  static_assert(KITSU_BRAIN_PERSISTED_BYTES == 300,
                "Persistence budget changed");
  static_assert(KITSU_BRAIN_STORED_BYTES == 600,
                "Double-buffer budget changed");
  testDeterministicPersonality();
  testMoodAndDiminishingReturns();
  testSleepGuardsAndJournalRing();
  testEncountersBondAndEvolution();
  testPortraitLabels();
  puts("PASS companion_brain_host");
  puts("  persistence: 2 x 300-byte CRC slots");
  puts("  journal: 24 x 8-byte entries");
  puts("  bond: levels 0..10; evolution stages 0..4");
  return 0;
}
