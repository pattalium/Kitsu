#include "../src/mesh_discovery_journal.h"

#include <assert.h>
#include <math.h>
#include <string.h>

using kitsu868::discovery::AdvertObservation;
using kitsu868::discovery::DiscoveryEvent;
using kitsu868::discovery::DiscoveryPeer;
using kitsu868::discovery::JournalCrypto;
using kitsu868::discovery::JournalResult;
using kitsu868::discovery::JournalStorage;
using kitsu868::discovery::MeshDiscoveryJournal;
using kitsu868::discovery::kDiscoveryJournalSlots;
using kitsu868::discovery::kDiscoveryNonceBytes;
using kitsu868::discovery::kDiscoverySnapshotCapacity;
using kitsu868::discovery::kDiscoveryTagBytes;

namespace {

uint32_t mix(uint32_t state, uint8_t value) {
  state ^= value;
  state *= 16777619UL;
  state ^= state >> 13U;
  return state;
}

class TestCrypto final : public JournalCrypto {
 public:
  bool randomNonce(uint8_t output[kDiscoveryNonceBytes]) override {
    if (failRandom) return false;
    ++nonceCounter;
    for (size_t i = 0; i < kDiscoveryNonceBytes; ++i) {
      output[i] = static_cast<uint8_t>(nonceCounter +
                                       static_cast<uint32_t>(i * 17U));
    }
    return true;
  }

  bool seal(uint32_t generation,
            const uint8_t nonce[kDiscoveryNonceBytes],
            const uint8_t* plaintext, size_t plaintextBytes,
            uint8_t* ciphertext,
            uint8_t tag[kDiscoveryTagBytes]) override {
    if (failSeal) return false;
    for (size_t i = 0; i < plaintextBytes; ++i) {
      ciphertext[i] = static_cast<uint8_t>(
          plaintext[i] ^ streamByte(generation, nonce, i));
    }
    makeTag(generation, nonce, ciphertext, plaintextBytes, tag);
    return true;
  }

  bool open(uint32_t generation,
            const uint8_t nonce[kDiscoveryNonceBytes],
            const uint8_t* ciphertext, size_t ciphertextBytes,
            const uint8_t tag[kDiscoveryTagBytes],
            uint8_t* plaintext) override {
    uint8_t expected[kDiscoveryTagBytes]{};
    makeTag(generation, nonce, ciphertext, ciphertextBytes, expected);
    if (memcmp(expected, tag, sizeof(expected)) != 0) return false;
    for (size_t i = 0; i < ciphertextBytes; ++i) {
      plaintext[i] = static_cast<uint8_t>(
          ciphertext[i] ^ streamByte(generation, nonce, i));
    }
    return true;
  }

  bool failRandom = false;
  bool failSeal = false;
  uint32_t nonceCounter = 0;

 private:
  static uint8_t streamByte(
      uint32_t generation,
      const uint8_t nonce[kDiscoveryNonceBytes], size_t index) {
    uint32_t state = generation ^ static_cast<uint32_t>(index * 2654435761UL);
    state = mix(state, nonce[index % kDiscoveryNonceBytes]);
    return static_cast<uint8_t>(state ^ (state >> 8U) ^ (state >> 16U));
  }

  static void makeTag(uint32_t generation,
                      const uint8_t nonce[kDiscoveryNonceBytes],
                      const uint8_t* ciphertext, size_t ciphertextBytes,
                      uint8_t tag[kDiscoveryTagBytes]) {
    uint32_t state = 2166136261UL ^ generation;
    for (size_t i = 0; i < kDiscoveryNonceBytes; ++i) {
      state = mix(state, nonce[i]);
    }
    for (size_t i = 0; i < ciphertextBytes; ++i) {
      state = mix(state, ciphertext[i]);
    }
    for (size_t i = 0; i < kDiscoveryTagBytes; ++i) {
      state = mix(state, static_cast<uint8_t>(i + ciphertextBytes));
      tag[i] = static_cast<uint8_t>(state >> ((i & 3U) * 8U));
    }
  }
};

class MemoryStorage final : public JournalStorage {
 public:
  bool readSlot(uint8_t slot, uint8_t* output, size_t capacity,
                size_t& outputBytes) override {
    if (slot >= kDiscoveryJournalSlots || failRead) return false;
    if (sizes[slot] > capacity) return false;
    memcpy(output, data[slot], sizes[slot]);
    outputBytes = sizes[slot];
    return true;
  }

  bool writeSlot(uint8_t slot, const uint8_t* input,
                 size_t inputBytes) override {
    if (slot >= kDiscoveryJournalSlots ||
        inputBytes > kDiscoverySnapshotCapacity) {
      return false;
    }
    if (tearNextWrite) {
      const size_t copied = tearBytes < inputBytes ? tearBytes : inputBytes;
      memcpy(data[slot], input, copied);
      sizes[slot] = copied;
      tearNextWrite = false;
      return false;
    }
    memcpy(data[slot], input, inputBytes);
    sizes[slot] = inputBytes;
    if (corruptNextWrite && inputBytes > 50U) {
      data[slot][50U] ^= 0x80U;
      corruptNextWrite = false;
    }
    return true;
  }

  void flip(uint8_t slot, size_t offset) {
    assert(slot < kDiscoveryJournalSlots);
    assert(offset < sizes[slot]);
    data[slot][offset] ^= 0x40U;
  }

  uint8_t data[kDiscoveryJournalSlots][kDiscoverySnapshotCapacity]{};
  size_t sizes[kDiscoveryJournalSlots]{};
  bool failRead = false;
  bool tearNextWrite = false;
  size_t tearBytes = 0U;
  bool corruptNextWrite = false;
};

AdvertObservation observation(uint8_t identity, uint32_t boot,
                              uint32_t atMillis) {
  AdvertObservation value{};
  for (size_t i = 0; i < sizeof(value.publicKey); ++i) {
    value.publicKey[i] = static_cast<uint8_t>(identity + i * 3U + 1U);
  }
  value.name[0] = 'K';
  value.name[1] = 'i';
  value.name[2] = 't';
  value.name[3] = 's';
  value.name[4] = 'u';
  value.name[5] = '-';
  value.name[6] = static_cast<char>('A' + (identity % 26U));
  value.type = 1U;
  value.kitsuNamed = true;
  value.hasLocation = true;
  value.latitudeE6 = 44500000L + identity;
  value.longitudeE6 = 26100000L + identity;
  value.senderAdvertTimestamp = 1800000000UL + atMillis;
  value.observed.epochValid = true;
  value.observed.epoch = 1800000000UL + atMillis;
  value.observed.bootId = boot;
  value.observed.millis = atMillis;
  value.lastHop.valid = true;
  value.lastHop.rssi = -91.25f + static_cast<float>(identity);
  value.lastHop.snr = 7.75f;
  return value;
}

bool hasPeer(const MeshDiscoveryJournal& journal, uint8_t identity) {
  const AdvertObservation wanted = observation(identity, 0U, 0U);
  DiscoveryPeer peer{};
  for (size_t i = 0; journal.peerAt(i, peer); ++i) {
    if (memcmp(peer.publicKey, wanted.publicKey,
               sizeof(wanted.publicKey)) == 0) {
      return true;
    }
  }
  return false;
}

void testUrgencyDedupAndReadback() {
  MemoryStorage storage;
  TestCrypto crypto;
  MeshDiscoveryJournal journal;
  assert(journal.begin(storage, crypto) == JournalResult::Ok);

  AdvertObservation first = observation(1U, 0x11111111UL, 100U);
  auto recorded = journal.record(first);
  assert(recorded.result == JournalResult::Ok);
  assert(recorded.urgent && recorded.newPeer && !recorded.evictedPeer);
  assert(journal.status().dirty);

  first.observed.millis = 200U;
  first.lastHop.rssi = -70.5f;
  recorded = journal.record(first);
  assert(!recorded.urgent && !recorded.newPeer);
  assert(journal.status().duplicateSightings == 1U);

  first.observed.bootId = 0x22222222UL;
  recorded = journal.record(first);
  assert(recorded.urgent && !recorded.newPeer);
  assert(journal.flush() == JournalResult::Ok);
  assert(!journal.status().dirty);
  assert(journal.status().activeSlot == 0);
  assert(journal.status().generation == 1U);
  assert(journal.status().committedBytes <= kDiscoverySnapshotCapacity);

  MeshDiscoveryJournal restored;
  assert(restored.begin(storage, crypto) == JournalResult::Ok);
  assert(restored.status().peerCount == 1U);
  assert(restored.status().eventCount == 3U);
  assert(restored.status().duplicateSightings == 2U);
  DiscoveryPeer peer{};
  assert(restored.peerAt(0U, peer));
  assert(peer.sightingCount == 3U);
  assert(peer.lastObserved.bootId == 0x22222222UL);
  assert(fabsf(peer.lastHop.rssi - (-70.5f)) < 0.011f);
  assert(strcmp(peer.name, "Kitsu-B") == 0);

  DiscoveryEvent event{};
  assert(restored.eventAfter(0U, event));
  assert(event.sequence == 1U);
  assert(restored.flush() == JournalResult::Ok);  // clean no-op
  assert(restored.status().generation == 1U);
}

void testFullKeysAndRingRollover() {
  MemoryStorage storage;
  TestCrypto crypto;
  MeshDiscoveryJournal journal;
  assert(journal.begin(storage, crypto) == JournalResult::Ok);

  AdvertObservation a = observation(4U, 77U, 1U);
  AdvertObservation b = a;
  b.publicKey[31] ^= 1U;
  assert(journal.record(a).urgent);
  assert(journal.record(b).urgent);
  assert(journal.status().peerCount == 2U);

  for (uint32_t i = 3U; i <= 20U; ++i) {
    AdvertObservation value = observation(4U, 77U, i);
    value.publicKey[31] = a.publicKey[31];
    assert(journal.record(value).result == JournalResult::Ok);
  }
  assert(journal.status().eventCount == 16U);
  assert(journal.status().newestSequence == 20U);
  DiscoveryEvent event{};
  uint32_t cursor = 0U;
  uint32_t count = 0U;
  while (journal.eventAfter(cursor, event)) {
    if (count == 0U) assert(event.sequence == 5U);
    cursor = event.sequence;
    ++count;
  }
  assert(count == 16U);
  assert(cursor == 20U);

  assert(journal.flush() == JournalResult::Ok);
  assert(journal.status().committedBytes < kDiscoverySnapshotCapacity);
  MeshDiscoveryJournal restored;
  assert(restored.begin(storage, crypto) == JournalResult::Ok);
  assert(restored.status().eventCount == 16U);
  assert(restored.eventAfter(0U, event) && event.sequence == 5U);
}

void testLruEviction() {
  MemoryStorage storage;
  TestCrypto crypto;
  MeshDiscoveryJournal journal;
  assert(journal.begin(storage, crypto) == JournalResult::Ok);
  for (uint8_t i = 1U; i <= 12U; ++i) {
    assert(journal.record(observation(i, 9U, i)).urgent);
  }
  assert(journal.status().peerCount == 12U);
  assert(journal.record(observation(1U, 9U, 100U)).urgent == false);
  const auto result = journal.record(observation(13U, 9U, 101U));
  assert(result.urgent && result.newPeer && result.evictedPeer);
  assert(journal.status().peerCount == 12U);
  assert(journal.status().evictions == 1U);
  assert(hasPeer(journal, 1U));
  assert(!hasPeer(journal, 2U));
  assert(hasPeer(journal, 13U));
}

void testPowerLossAndCorruptNewestRecovery() {
  MemoryStorage storage;
  TestCrypto crypto;
  MeshDiscoveryJournal journal;
  assert(journal.begin(storage, crypto) == JournalResult::Ok);
  assert(journal.record(observation(1U, 1U, 1U)).result ==
         JournalResult::Ok);
  assert(journal.flush() == JournalResult::Ok);

  assert(journal.record(observation(2U, 1U, 2U)).result ==
         JournalResult::Ok);
  storage.tearNextWrite = true;
  storage.tearBytes = 80U;
  assert(journal.flush() == JournalResult::StorageWriteFailed);
  assert(journal.status().dirty);
  assert(journal.status().activeSlot == 0);

  MeshDiscoveryJournal afterPowerLoss;
  assert(afterPowerLoss.begin(storage, crypto) == JournalResult::Ok);
  assert(afterPowerLoss.status().generation == 1U);
  assert(afterPowerLoss.status().peerCount == 1U);
  assert(hasPeer(afterPowerLoss, 1U));
  assert(!hasPeer(afterPowerLoss, 2U));

  assert(afterPowerLoss.record(observation(3U, 2U, 3U)).result ==
         JournalResult::Ok);
  assert(afterPowerLoss.flush() == JournalResult::Ok);
  assert(afterPowerLoss.status().activeSlot == 1);
  storage.flip(1U, 60U);
  MeshDiscoveryJournal afterCorruption;
  assert(afterCorruption.begin(storage, crypto) == JournalResult::Ok);
  assert(afterCorruption.status().activeSlot == 0);
  assert(afterCorruption.status().generation == 1U);
  assert(hasPeer(afterCorruption, 1U));
  assert(!hasPeer(afterCorruption, 3U));
}

void testReadbackFailureDoesNotCommit() {
  MemoryStorage storage;
  TestCrypto crypto;
  MeshDiscoveryJournal journal;
  assert(journal.begin(storage, crypto) == JournalResult::Ok);
  assert(journal.record(observation(1U, 1U, 1U)).result ==
         JournalResult::Ok);
  assert(journal.flush() == JournalResult::Ok);
  assert(journal.record(observation(2U, 1U, 2U)).result ==
         JournalResult::Ok);
  storage.corruptNextWrite = true;
  assert(journal.flush() == JournalResult::ReadbackFailed);
  assert(journal.status().dirty);
  assert(journal.status().activeSlot == 0);
  assert(journal.status().generation == 1U);

  MeshDiscoveryJournal restored;
  assert(restored.begin(storage, crypto) == JournalResult::Ok);
  assert(restored.status().generation == 1U);
  assert(restored.status().peerCount == 1U);
}

void testAlternatingSlotsAndMaximumSnapshot() {
  MemoryStorage storage;
  TestCrypto crypto;
  MeshDiscoveryJournal journal;
  assert(journal.begin(storage, crypto) == JournalResult::Ok);
  for (uint8_t i = 1U; i <= 12U; ++i) {
    AdvertObservation value = observation(i, 5U, i);
    memset(value.name, 'N', 32U);
    value.name[32] = '\0';
    assert(journal.record(value).result == JournalResult::Ok);
  }
  for (uint32_t i = 13U; i <= 16U; ++i) {
    assert(journal.record(observation(12U, 5U, i)).result ==
           JournalResult::Ok);
  }
  assert(journal.status().peerCount == 12U);
  assert(journal.status().eventCount == 16U);
  assert(journal.flush() == JournalResult::Ok);
  const uint16_t fullBytes = journal.status().committedBytes;
  assert(fullBytes > 2000U && fullBytes <= kDiscoverySnapshotCapacity);
  assert(journal.status().activeSlot == 0);
  assert(journal.record(observation(12U, 5U, 17U)).result ==
         JournalResult::Ok);
  assert(journal.flush() == JournalResult::Ok);
  assert(journal.status().activeSlot == 1);
  assert(journal.status().generation == 2U);
  assert(storage.sizes[0] <= kDiscoverySnapshotCapacity);
  assert(storage.sizes[1] <= kDiscoverySnapshotCapacity);
}

void testArgumentAndCryptoFailures() {
  MemoryStorage storage;
  TestCrypto crypto;
  MeshDiscoveryJournal journal;
  AdvertObservation value = observation(1U, 1U, 1U);
  assert(journal.record(value).result == JournalResult::NotBegun);
  assert(journal.begin(storage, crypto) == JournalResult::Ok);
  memset(value.publicKey, 0, sizeof(value.publicKey));
  assert(journal.record(value).result == JournalResult::InvalidArgument);
  value = observation(1U, 1U, 1U);
  value.latitudeE6 = 91000000L;
  assert(journal.record(value).result == JournalResult::InvalidArgument);
  value = observation(1U, 1U, 1U);
  assert(journal.record(value).result == JournalResult::Ok);
  crypto.failRandom = true;
  assert(journal.flush() == JournalResult::CryptoFailed);
  assert(journal.status().dirty);
}

}  // namespace

int main() {
  testUrgencyDedupAndReadback();
  testFullKeysAndRingRollover();
  testLruEviction();
  testPowerLossAndCorruptNewestRecovery();
  testReadbackFailureDoesNotCommit();
  testAlternatingSlotsAndMaximumSnapshot();
  testArgumentAndCryptoFailures();
  return 0;
}

