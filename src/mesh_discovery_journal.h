#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kitsu868 {
namespace discovery {

constexpr size_t kDiscoveryPublicKeyBytes = 32U;
constexpr size_t kDiscoveryNameBytes = 32U;
constexpr size_t kDiscoveryPeerCapacity = 12U;
constexpr size_t kDiscoveryEventCapacity = 16U;
constexpr size_t kDiscoveryJournalSlots = 2U;
constexpr size_t kDiscoverySnapshotCapacity = 2500U;
// Reflashable owner-image partition: data subtype
// 0x40, label "kitsu_journal", 0x2000 bytes.  Each alternating snapshot owns
// one independently erased 0x1000-byte sector; the current <=2500-byte sealed
// snapshot leaves room for a future sector header.  Flashing/repartitioning is
// intentionally deferred for owner review.
constexpr size_t kDiscoveryProposedPartitionBytes = 0x2000U;
constexpr size_t kDiscoveryProposedSlotBytes = 0x1000U;
static_assert(kDiscoverySnapshotCapacity < kDiscoveryProposedSlotBytes,
              "sealed discovery snapshot must fit one erase sector");
constexpr size_t kDiscoveryNonceBytes = 12U;
constexpr size_t kDiscoveryTagBytes = 16U;

// JournalStorage owns exactly two independently replaceable blobs.  Firmware
// adapters may map them to two encrypted-NVS blobs or, preferably, to the two
// erase regions in the proposed Kitsu journal partition.  A failed write must
// leave the previously committed slot readable.
class JournalStorage {
 public:
  virtual ~JournalStorage() = default;
  virtual bool readSlot(uint8_t slot, uint8_t* output, size_t capacity,
                        size_t& outputBytes) = 0;
  virtual bool writeSlot(uint8_t slot, const uint8_t* input,
                         size_t inputBytes) = 0;
};

// Authenticated encryption is mandatory at the journal boundary.  The
// firmware implementation uses AES-256-GCM with a journal-only key derived
// from the Kitsu device secret.  The generation is authenticated as AAD and
// also selects a unique deterministic nonce domain after begin() recovers the
// newest committed slot.
class JournalCrypto {
 public:
  virtual ~JournalCrypto() = default;
  virtual bool randomNonce(uint8_t output[kDiscoveryNonceBytes]) = 0;
  virtual bool seal(uint32_t generation,
                    const uint8_t nonce[kDiscoveryNonceBytes],
                    const uint8_t* plaintext, size_t plaintextBytes,
                    uint8_t* ciphertext,
                    uint8_t tag[kDiscoveryTagBytes]) = 0;
  virtual bool open(uint32_t generation,
                    const uint8_t nonce[kDiscoveryNonceBytes],
                    const uint8_t* ciphertext, size_t ciphertextBytes,
                    const uint8_t tag[kDiscoveryTagBytes],
                    uint8_t* plaintext) = 0;
};

enum class JournalResult : uint8_t {
  Ok = 0,
  NotBegun,
  InvalidArgument,
  StorageReadFailed,
  StorageWriteFailed,
  ReadbackFailed,
  CryptoFailed,
  CorruptSnapshot,
};

const char* journalResultName(JournalResult result);

struct ObservationTime {
  bool epochValid = false;
  uint32_t epoch = 0;
  uint32_t bootId = 0;
  uint32_t millis = 0;
};

struct LastHopSignal {
  bool valid = false;
  float rssi = 0.0f;
  float snr = 0.0f;
};

// A verified MeshCore advert as seen by the local radio.  The complete public
// key is the identity; prefixes are never accepted for deduplication.
struct AdvertObservation {
  uint8_t publicKey[kDiscoveryPublicKeyBytes]{};
  char name[kDiscoveryNameBytes + 1U]{};
  uint8_t type = 0;
  bool kitsuNamed = false;
  bool hasLocation = false;
  int32_t latitudeE6 = 0;
  int32_t longitudeE6 = 0;
  uint32_t senderAdvertTimestamp = 0;
  ObservationTime observed{};
  LastHopSignal lastHop{};
};

struct DiscoveryPeer {
  uint8_t publicKey[kDiscoveryPublicKeyBytes]{};
  char name[kDiscoveryNameBytes + 1U]{};
  uint8_t type = 0;
  bool kitsuNamed = false;
  bool hasLocation = false;
  int32_t latitudeE6 = 0;
  int32_t longitudeE6 = 0;
  uint32_t senderAdvertTimestamp = 0;
  ObservationTime lastObserved{};
  LastHopSignal lastHop{};
  uint32_t lastSequence = 0;
  uint32_t sightingCount = 0;
  uint32_t lru = 0;
};

// Events intentionally repeat only identity, timing and RF evidence.  Advert
// metadata lives in the 12-peer dedup table, keeping the complete encrypted
// snapshot below 2500 bytes while preserving every recent reception.
struct DiscoveryEvent {
  uint32_t sequence = 0;
  uint8_t publicKey[kDiscoveryPublicKeyBytes]{};
  uint32_t senderAdvertTimestamp = 0;
  ObservationTime observed{};
  LastHopSignal lastHop{};
};

struct RecordResult {
  JournalResult result = JournalResult::NotBegun;
  uint32_t sequence = 0;
  bool urgent = false;
  bool newPeer = false;
  bool evictedPeer = false;
};

struct JournalStatus {
  JournalResult lastResult = JournalResult::NotBegun;
  bool begun = false;
  bool dirty = false;
  int8_t activeSlot = -1;
  uint32_t generation = 0;
  uint32_t newestSequence = 0;
  uint32_t totalSightings = 0;
  uint32_t duplicateSightings = 0;
  uint32_t evictions = 0;
  uint32_t lruClock = 0;
  uint8_t peerCount = 0;
  uint8_t eventCount = 0;
  uint16_t committedBytes = 0;
};

class MeshDiscoveryJournal {
 public:
  MeshDiscoveryJournal();

  // Recovers the newest authenticated slot.  Two empty/corrupt slots produce
  // a clean empty journal; an I/O failure is reported instead of silently
  // treating persisted state as absent.
  JournalResult begin(JournalStorage& storage, JournalCrypto& crypto);

  // Every valid reception gets a monotonic serial-number event.  The first
  // sighting of a peer in each boot is urgent so callers can flush immediately;
  // later same-boot receptions remain dirty and may be debounce-flushed.
  RecordResult record(const AdvertObservation& observation);

  // Commits to the inactive slot, then reads it back and authenticates it
  // before changing the active generation.  A failed commit retains dirty
  // in-memory state and the old slot remains the recovery point.
  JournalResult flush();

  bool peerAt(size_t ordinal, DiscoveryPeer& output) const;

  // Returns the oldest retained event whose serial number is strictly after
  // afterSequence.  RFC-1982-style signed comparison makes uint32 rollover
  // safe for windows much smaller than 2^31 (this ring contains only 16).
  bool eventAfter(uint32_t afterSequence, DiscoveryEvent& output) const;

  JournalStatus status() const;

  static constexpr size_t maximumSnapshotBytes() {
    return kDiscoverySnapshotCapacity;
  }

 private:
  struct PeerSlot {
    bool valid = false;
    DiscoveryPeer value{};
  };

  JournalResult setResult(JournalResult result);
  void resetState();
  bool readAndValidateSlot(uint8_t slot, uint32_t& generation,
                           size_t& bytes);
  bool decodeLoadedSnapshot(size_t bytes);
  bool encodeSnapshot(uint32_t generation, size_t& bytes);
  int findPeer(const uint8_t publicKey[kDiscoveryPublicKeyBytes]) const;
  int allocationPeer(bool& evicted);
  static bool sequenceAfter(uint32_t candidate, uint32_t reference);

  JournalStorage* storage_ = nullptr;
  JournalCrypto* crypto_ = nullptr;
  PeerSlot peers_[kDiscoveryPeerCapacity]{};
  DiscoveryEvent events_[kDiscoveryEventCapacity]{};
  uint8_t eventStart_ = 0;
  JournalStatus status_{};
  // Serialization, ciphertext, and readback all reuse member storage so no
  // multi-kilobyte automatic buffer is ever placed on the ESP32 task stack.
  uint8_t scratch_[kDiscoverySnapshotCapacity]{};
  uint8_t cryptScratch_[kDiscoverySnapshotCapacity]{};
};

}  // namespace discovery
}  // namespace kitsu868
