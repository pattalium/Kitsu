#include "mesh_discovery_journal.h"

#include <math.h>
#include <string.h>

namespace kitsu868 {
namespace discovery {
namespace {

constexpr uint8_t kOuterMagic[4] = {'K', 'J', 'A', 'E'};
constexpr uint8_t kPlainMagic[4] = {'K', 'J', 'P', 'L'};
constexpr uint16_t kFormatVersion = 1U;
constexpr size_t kOuterHeaderBytes = 44U;
constexpr size_t kPlainHeaderBytes = 32U;
constexpr size_t kPeerSerializedBytes = 107U;
constexpr size_t kEventSerializedBytes = 57U;
constexpr size_t kPlainCrcBytes = 4U;

static_assert(kOuterHeaderBytes + kPlainHeaderBytes +
                      (kDiscoveryPeerCapacity * kPeerSerializedBytes) +
                      (kDiscoveryEventCapacity * kEventSerializedBytes) +
                      kPlainCrcBytes <=
                  kDiscoverySnapshotCapacity,
              "discovery snapshot exceeds its persistence slot");

size_t boundedLength(const char* text, size_t capacity) {
  if (!text) return 0U;
  size_t bytes = 0U;
  while (bytes < capacity && text[bytes] != '\0') ++bytes;
  return bytes;
}

bool allByte(const uint8_t* input, size_t bytes, uint8_t value) {
  for (size_t i = 0; i < bytes; ++i) {
    if (input[i] != value) return false;
  }
  return true;
}

void putU16(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
  output[2] = static_cast<uint8_t>(value >> 16U);
  output[3] = static_cast<uint8_t>(value >> 24U);
}

uint16_t getU16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U);
}

uint32_t getU32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) |
         (static_cast<uint32_t>(input[1]) << 8U) |
         (static_cast<uint32_t>(input[2]) << 16U) |
         (static_cast<uint32_t>(input[3]) << 24U);
}

uint32_t crc32(const uint8_t* input, size_t bytes) {
  uint32_t crc = 0xffffffffUL;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= input[i];
    for (uint8_t bit = 0; bit < 8U; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(
          -static_cast<int32_t>(crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320UL & mask);
    }
  }
  return ~crc;
}

int16_t encodeHundredths(float value) {
  if (!isfinite(value)) return 0;
  float scaled = value * 100.0f;
  if (scaled > 32767.0f) scaled = 32767.0f;
  if (scaled < -32768.0f) scaled = -32768.0f;
  return static_cast<int16_t>(lroundf(scaled));
}

float decodeHundredths(uint16_t encoded) {
  return static_cast<float>(static_cast<int16_t>(encoded)) / 100.0f;
}

void incrementSaturated(uint32_t& value) {
  if (value != UINT32_MAX) ++value;
}

bool generationAfter(uint32_t candidate, uint32_t reference) {
  return static_cast<int32_t>(candidate - reference) > 0;
}

}  // namespace

const char* journalResultName(JournalResult result) {
  switch (result) {
    case JournalResult::Ok: return "ok";
    case JournalResult::NotBegun: return "not_begun";
    case JournalResult::InvalidArgument: return "invalid_argument";
    case JournalResult::StorageReadFailed: return "storage_read_failed";
    case JournalResult::StorageWriteFailed: return "storage_write_failed";
    case JournalResult::ReadbackFailed: return "readback_failed";
    case JournalResult::CryptoFailed: return "crypto_failed";
    case JournalResult::CorruptSnapshot: return "corrupt_snapshot";
    default: return "unknown";
  }
}

MeshDiscoveryJournal::MeshDiscoveryJournal() { resetState(); }

JournalResult MeshDiscoveryJournal::setResult(JournalResult result) {
  status_.lastResult = result;
  return result;
}

void MeshDiscoveryJournal::resetState() {
  memset(peers_, 0, sizeof(peers_));
  memset(events_, 0, sizeof(events_));
  eventStart_ = 0U;
  status_ = JournalStatus{};
}

bool MeshDiscoveryJournal::sequenceAfter(uint32_t candidate,
                                         uint32_t reference) {
  return static_cast<int32_t>(candidate - reference) > 0;
}

bool MeshDiscoveryJournal::readAndValidateSlot(uint8_t slot,
                                                uint32_t& generation,
                                                size_t& bytes) {
  generation = 0U;
  bytes = 0U;
  size_t loaded = 0U;
  if (!storage_->readSlot(slot, scratch_, sizeof(scratch_), loaded)) {
    status_.lastResult = JournalResult::StorageReadFailed;
    return false;
  }
  bytes = loaded;
  if (loaded == 0U) return false;
  if (loaded < kOuterHeaderBytes || loaded > sizeof(scratch_) ||
      memcmp(scratch_, kOuterMagic, sizeof(kOuterMagic)) != 0 ||
      getU16(scratch_ + 4U) != kFormatVersion ||
      getU16(scratch_ + 6U) != kOuterHeaderBytes) {
    status_.lastResult = JournalResult::CorruptSnapshot;
    return false;
  }

  generation = getU32(scratch_ + 8U);
  const size_t plaintextBytes = getU16(scratch_ + 12U);
  const size_t ciphertextBytes = getU16(scratch_ + 14U);
  if (plaintextBytes == 0U || plaintextBytes != ciphertextBytes ||
      kOuterHeaderBytes + ciphertextBytes != loaded ||
      plaintextBytes > sizeof(cryptScratch_)) {
    status_.lastResult = JournalResult::CorruptSnapshot;
    return false;
  }

  if (!crypto_->open(generation, scratch_ + 16U,
                     scratch_ + kOuterHeaderBytes, ciphertextBytes,
                     scratch_ + 28U, cryptScratch_)) {
    status_.lastResult = JournalResult::CorruptSnapshot;
    return false;
  }

  if (plaintextBytes < kPlainHeaderBytes + kPlainCrcBytes ||
      memcmp(cryptScratch_, kPlainMagic, sizeof(kPlainMagic)) != 0 ||
      getU16(cryptScratch_ + 4U) != kFormatVersion ||
      getU16(cryptScratch_ + 6U) != plaintextBytes ||
      crc32(cryptScratch_, plaintextBytes - kPlainCrcBytes) !=
          getU32(cryptScratch_ + plaintextBytes - kPlainCrcBytes)) {
    status_.lastResult = JournalResult::CorruptSnapshot;
    return false;
  }

  const uint8_t peerCount = cryptScratch_[28U];
  const uint8_t eventCount = cryptScratch_[29U];
  const size_t expected = kPlainHeaderBytes +
                          (static_cast<size_t>(peerCount) *
                           kPeerSerializedBytes) +
                          (static_cast<size_t>(eventCount) *
                           kEventSerializedBytes) +
                          kPlainCrcBytes;
  if (peerCount > kDiscoveryPeerCapacity ||
      eventCount > kDiscoveryEventCapacity || expected != plaintextBytes) {
    status_.lastResult = JournalResult::CorruptSnapshot;
    return false;
  }
  return true;
}

bool MeshDiscoveryJournal::decodeLoadedSnapshot(size_t bytes) {
  if (bytes < kOuterHeaderBytes) return false;
  const size_t plaintextBytes = getU16(scratch_ + 12U);
  if (plaintextBytes < kPlainHeaderBytes + kPlainCrcBytes) return false;

  const uint8_t peerCount = cryptScratch_[28U];
  const uint8_t eventCount = cryptScratch_[29U];
  size_t cursor = kPlainHeaderBytes;

  memset(peers_, 0, sizeof(peers_));
  memset(events_, 0, sizeof(events_));
  eventStart_ = 0U;

  status_.newestSequence = getU32(cryptScratch_ + 8U);
  status_.totalSightings = getU32(cryptScratch_ + 12U);
  status_.duplicateSightings = getU32(cryptScratch_ + 16U);
  status_.evictions = getU32(cryptScratch_ + 20U);
  status_.lruClock = getU32(cryptScratch_ + 24U);
  status_.peerCount = peerCount;
  status_.eventCount = eventCount;

  for (uint8_t i = 0U; i < peerCount; ++i) {
    PeerSlot& slot = peers_[i];
    DiscoveryPeer& peer = slot.value;
    slot.valid = true;
    memcpy(peer.publicKey, cryptScratch_ + cursor,
           kDiscoveryPublicKeyBytes);
    cursor += kDiscoveryPublicKeyBytes;
    const uint8_t nameBytes = cryptScratch_[cursor++];
    if (nameBytes > kDiscoveryNameBytes) return false;
    memcpy(peer.name, cryptScratch_ + cursor, nameBytes);
    peer.name[nameBytes] = '\0';
    cursor += kDiscoveryNameBytes;
    peer.type = cryptScratch_[cursor++];
    const uint8_t flags = cryptScratch_[cursor++];
    peer.kitsuNamed = (flags & 0x01U) != 0U;
    peer.hasLocation = (flags & 0x02U) != 0U;
    peer.lastObserved.epochValid = (flags & 0x04U) != 0U;
    peer.lastHop.valid = (flags & 0x08U) != 0U;
    peer.latitudeE6 = static_cast<int32_t>(getU32(cryptScratch_ + cursor));
    cursor += 4U;
    peer.longitudeE6 = static_cast<int32_t>(getU32(cryptScratch_ + cursor));
    cursor += 4U;
    peer.senderAdvertTimestamp = getU32(cryptScratch_ + cursor);
    cursor += 4U;
    peer.lastObserved.epoch = getU32(cryptScratch_ + cursor);
    cursor += 4U;
    peer.lastObserved.bootId = getU32(cryptScratch_ + cursor);
    cursor += 4U;
    peer.lastObserved.millis = getU32(cryptScratch_ + cursor);
    cursor += 4U;
    peer.lastHop.rssi = decodeHundredths(getU16(cryptScratch_ + cursor));
    cursor += 2U;
    peer.lastHop.snr = decodeHundredths(getU16(cryptScratch_ + cursor));
    cursor += 2U;
    peer.lastSequence = getU32(cryptScratch_ + cursor);
    cursor += 4U;
    peer.sightingCount = getU32(cryptScratch_ + cursor);
    cursor += 4U;
    peer.lru = getU32(cryptScratch_ + cursor);
    cursor += 4U;
  }

  for (uint8_t i = 0U; i < eventCount; ++i) {
    DiscoveryEvent& event = events_[i];
    event.sequence = getU32(cryptScratch_ + cursor);
    cursor += 4U;
    memcpy(event.publicKey, cryptScratch_ + cursor,
           kDiscoveryPublicKeyBytes);
    cursor += kDiscoveryPublicKeyBytes;
    event.senderAdvertTimestamp = getU32(cryptScratch_ + cursor);
    cursor += 4U;
    const uint8_t flags = cryptScratch_[cursor++];
    event.observed.epochValid = (flags & 0x01U) != 0U;
    event.lastHop.valid = (flags & 0x02U) != 0U;
    event.observed.epoch = getU32(cryptScratch_ + cursor);
    cursor += 4U;
    event.observed.bootId = getU32(cryptScratch_ + cursor);
    cursor += 4U;
    event.observed.millis = getU32(cryptScratch_ + cursor);
    cursor += 4U;
    event.lastHop.rssi = decodeHundredths(getU16(cryptScratch_ + cursor));
    cursor += 2U;
    event.lastHop.snr = decodeHundredths(getU16(cryptScratch_ + cursor));
    cursor += 2U;
  }

  return cursor + kPlainCrcBytes == plaintextBytes;
}

JournalResult MeshDiscoveryJournal::begin(JournalStorage& storage,
                                           JournalCrypto& crypto) {
  resetState();
  storage_ = &storage;
  crypto_ = &crypto;

  uint32_t generations[kDiscoveryJournalSlots]{};
  size_t bytes[kDiscoveryJournalSlots]{};
  bool valid[kDiscoveryJournalSlots]{};
  for (uint8_t slot = 0U; slot < kDiscoveryJournalSlots; ++slot) {
    status_.lastResult = JournalResult::Ok;
    valid[slot] = readAndValidateSlot(slot, generations[slot], bytes[slot]);
    if (!valid[slot] &&
        status_.lastResult == JournalResult::StorageReadFailed) {
      return setResult(JournalResult::StorageReadFailed);
    }
  }

  int chosen = -1;
  if (valid[0]) chosen = 0;
  if (valid[1] &&
      (chosen < 0 || generationAfter(generations[1], generations[0]))) {
    chosen = 1;
  }

  if (chosen >= 0) {
    uint32_t generation = 0U;
    size_t loaded = 0U;
    status_.lastResult = JournalResult::Ok;
    if (!readAndValidateSlot(static_cast<uint8_t>(chosen), generation,
                             loaded)) {
      return setResult(status_.lastResult == JournalResult::StorageReadFailed
                           ? JournalResult::StorageReadFailed
                           : JournalResult::CorruptSnapshot);
    }
    if (!decodeLoadedSnapshot(loaded)) {
      return setResult(JournalResult::CorruptSnapshot);
    }
    status_.activeSlot = static_cast<int8_t>(chosen);
    status_.generation = generation;
    status_.committedBytes = static_cast<uint16_t>(loaded);
  }

  status_.begun = true;
  status_.dirty = false;
  return setResult(JournalResult::Ok);
}

int MeshDiscoveryJournal::findPeer(
    const uint8_t publicKey[kDiscoveryPublicKeyBytes]) const {
  for (size_t i = 0U; i < kDiscoveryPeerCapacity; ++i) {
    if (peers_[i].valid &&
        memcmp(peers_[i].value.publicKey, publicKey,
               kDiscoveryPublicKeyBytes) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int MeshDiscoveryJournal::allocationPeer(bool& evicted) {
  evicted = false;
  for (size_t i = 0U; i < kDiscoveryPeerCapacity; ++i) {
    if (!peers_[i].valid) return static_cast<int>(i);
  }

  size_t oldest = 0U;
  uint32_t greatestAge = status_.lruClock - peers_[0].value.lru;
  for (size_t i = 1U; i < kDiscoveryPeerCapacity; ++i) {
    const uint32_t age = status_.lruClock - peers_[i].value.lru;
    if (age > greatestAge) {
      oldest = i;
      greatestAge = age;
    }
  }
  evicted = true;
  return static_cast<int>(oldest);
}

RecordResult MeshDiscoveryJournal::record(
    const AdvertObservation& observation) {
  RecordResult result{};
  if (!status_.begun || !storage_ || !crypto_) {
    result.result = setResult(JournalResult::NotBegun);
    return result;
  }
  if (allByte(observation.publicKey, kDiscoveryPublicKeyBytes, 0x00U) ||
      allByte(observation.publicKey, kDiscoveryPublicKeyBytes, 0xffU) ||
      (observation.hasLocation &&
       (observation.latitudeE6 < -90000000L ||
        observation.latitudeE6 > 90000000L ||
        observation.longitudeE6 < -180000000L ||
        observation.longitudeE6 > 180000000L)) ||
      (observation.lastHop.valid &&
       (!isfinite(observation.lastHop.rssi) ||
        !isfinite(observation.lastHop.snr)))) {
    result.result = setResult(JournalResult::InvalidArgument);
    return result;
  }

  int peerIndex = findPeer(observation.publicKey);
  const bool existed = peerIndex >= 0;
  bool evicted = false;
  if (!existed) peerIndex = allocationPeer(evicted);
  PeerSlot& slot = peers_[static_cast<size_t>(peerIndex)];
  const bool firstThisBoot = !existed ||
      slot.value.lastObserved.bootId != observation.observed.bootId;

  uint32_t sightings = existed ? slot.value.sightingCount : 0U;
  incrementSaturated(sightings);
  slot.valid = true;
  DiscoveryPeer& peer = slot.value;
  memcpy(peer.publicKey, observation.publicKey, kDiscoveryPublicKeyBytes);
  memset(peer.name, 0, sizeof(peer.name));
  const size_t nameBytes = boundedLength(observation.name,
                                         kDiscoveryNameBytes);
  memcpy(peer.name, observation.name, nameBytes);
  peer.type = observation.type;
  peer.kitsuNamed = observation.kitsuNamed;
  peer.hasLocation = observation.hasLocation;
  peer.latitudeE6 = observation.hasLocation ? observation.latitudeE6 : 0;
  peer.longitudeE6 = observation.hasLocation ? observation.longitudeE6 : 0;
  peer.senderAdvertTimestamp = observation.senderAdvertTimestamp;
  peer.lastObserved = observation.observed;
  peer.lastHop = observation.lastHop;
  peer.sightingCount = sightings;
  ++status_.newestSequence;
  peer.lastSequence = status_.newestSequence;
  ++status_.lruClock;
  peer.lru = status_.lruClock;

  if (!existed && !evicted) ++status_.peerCount;
  if (existed) incrementSaturated(status_.duplicateSightings);
  if (evicted) incrementSaturated(status_.evictions);
  incrementSaturated(status_.totalSightings);

  uint8_t eventIndex = 0U;
  if (status_.eventCount < kDiscoveryEventCapacity) {
    eventIndex = static_cast<uint8_t>(
        (eventStart_ + status_.eventCount) % kDiscoveryEventCapacity);
    ++status_.eventCount;
  } else {
    eventIndex = eventStart_;
    eventStart_ = static_cast<uint8_t>(
        (eventStart_ + 1U) % kDiscoveryEventCapacity);
  }
  DiscoveryEvent& event = events_[eventIndex];
  event.sequence = status_.newestSequence;
  memcpy(event.publicKey, observation.publicKey, kDiscoveryPublicKeyBytes);
  event.senderAdvertTimestamp = observation.senderAdvertTimestamp;
  event.observed = observation.observed;
  event.lastHop = observation.lastHop;

  status_.dirty = true;
  result.result = setResult(JournalResult::Ok);
  result.sequence = event.sequence;
  result.urgent = firstThisBoot;
  result.newPeer = !existed;
  result.evictedPeer = evicted;
  return result;
}

bool MeshDiscoveryJournal::encodeSnapshot(uint32_t generation,
                                           size_t& bytes) {
  uint8_t* output = cryptScratch_;
  memset(output, 0, sizeof(cryptScratch_));
  memcpy(output, kPlainMagic, sizeof(kPlainMagic));
  putU16(output + 4U, kFormatVersion);
  putU32(output + 8U, status_.newestSequence);
  putU32(output + 12U, status_.totalSightings);
  putU32(output + 16U, status_.duplicateSightings);
  putU32(output + 20U, status_.evictions);
  putU32(output + 24U, status_.lruClock);
  output[28U] = status_.peerCount;
  output[29U] = status_.eventCount;

  size_t cursor = kPlainHeaderBytes;
  uint8_t encodedPeers = 0U;
  for (size_t i = 0U; i < kDiscoveryPeerCapacity; ++i) {
    if (!peers_[i].valid) continue;
    const DiscoveryPeer& peer = peers_[i].value;
    memcpy(output + cursor, peer.publicKey, kDiscoveryPublicKeyBytes);
    cursor += kDiscoveryPublicKeyBytes;
    const size_t nameBytes = boundedLength(peer.name, kDiscoveryNameBytes);
    output[cursor++] = static_cast<uint8_t>(nameBytes);
    memcpy(output + cursor, peer.name, nameBytes);
    cursor += kDiscoveryNameBytes;
    output[cursor++] = peer.type;
    uint8_t flags = 0U;
    if (peer.kitsuNamed) flags |= 0x01U;
    if (peer.hasLocation) flags |= 0x02U;
    if (peer.lastObserved.epochValid) flags |= 0x04U;
    if (peer.lastHop.valid) flags |= 0x08U;
    output[cursor++] = flags;
    putU32(output + cursor, static_cast<uint32_t>(peer.latitudeE6));
    cursor += 4U;
    putU32(output + cursor, static_cast<uint32_t>(peer.longitudeE6));
    cursor += 4U;
    putU32(output + cursor, peer.senderAdvertTimestamp);
    cursor += 4U;
    putU32(output + cursor, peer.lastObserved.epoch);
    cursor += 4U;
    putU32(output + cursor, peer.lastObserved.bootId);
    cursor += 4U;
    putU32(output + cursor, peer.lastObserved.millis);
    cursor += 4U;
    putU16(output + cursor,
           static_cast<uint16_t>(encodeHundredths(peer.lastHop.rssi)));
    cursor += 2U;
    putU16(output + cursor,
           static_cast<uint16_t>(encodeHundredths(peer.lastHop.snr)));
    cursor += 2U;
    putU32(output + cursor, peer.lastSequence);
    cursor += 4U;
    putU32(output + cursor, peer.sightingCount);
    cursor += 4U;
    putU32(output + cursor, peer.lru);
    cursor += 4U;
    ++encodedPeers;
  }
  if (encodedPeers != status_.peerCount) return false;

  for (uint8_t ordinal = 0U; ordinal < status_.eventCount; ++ordinal) {
    const uint8_t index = static_cast<uint8_t>(
        (eventStart_ + ordinal) % kDiscoveryEventCapacity);
    const DiscoveryEvent& event = events_[index];
    putU32(output + cursor, event.sequence);
    cursor += 4U;
    memcpy(output + cursor, event.publicKey, kDiscoveryPublicKeyBytes);
    cursor += kDiscoveryPublicKeyBytes;
    putU32(output + cursor, event.senderAdvertTimestamp);
    cursor += 4U;
    uint8_t flags = 0U;
    if (event.observed.epochValid) flags |= 0x01U;
    if (event.lastHop.valid) flags |= 0x02U;
    output[cursor++] = flags;
    putU32(output + cursor, event.observed.epoch);
    cursor += 4U;
    putU32(output + cursor, event.observed.bootId);
    cursor += 4U;
    putU32(output + cursor, event.observed.millis);
    cursor += 4U;
    putU16(output + cursor,
           static_cast<uint16_t>(encodeHundredths(event.lastHop.rssi)));
    cursor += 2U;
    putU16(output + cursor,
           static_cast<uint16_t>(encodeHundredths(event.lastHop.snr)));
    cursor += 2U;
  }

  const size_t plaintextBytes = cursor + kPlainCrcBytes;
  if (kOuterHeaderBytes + plaintextBytes > sizeof(scratch_) ||
      plaintextBytes > UINT16_MAX) {
    return false;
  }
  putU16(output + 6U, static_cast<uint16_t>(plaintextBytes));
  putU32(output + cursor, crc32(output, cursor));

  memset(scratch_, 0, sizeof(scratch_));
  memcpy(scratch_, kOuterMagic, sizeof(kOuterMagic));
  putU16(scratch_ + 4U, kFormatVersion);
  putU16(scratch_ + 6U, static_cast<uint16_t>(kOuterHeaderBytes));
  putU32(scratch_ + 8U, generation);
  putU16(scratch_ + 12U, static_cast<uint16_t>(plaintextBytes));
  putU16(scratch_ + 14U, static_cast<uint16_t>(plaintextBytes));
  if (!crypto_->randomNonce(scratch_ + 16U) ||
      !crypto_->seal(generation, scratch_ + 16U, cryptScratch_,
                     plaintextBytes, scratch_ + kOuterHeaderBytes,
                     scratch_ + 28U)) {
    return false;
  }
  bytes = kOuterHeaderBytes + plaintextBytes;
  return true;
}

JournalResult MeshDiscoveryJournal::flush() {
  if (!status_.begun || !storage_ || !crypto_) {
    return setResult(JournalResult::NotBegun);
  }
  if (!status_.dirty) return setResult(JournalResult::Ok);

  const uint32_t nextGeneration = status_.generation + 1U;
  size_t bytes = 0U;
  if (!encodeSnapshot(nextGeneration, bytes)) {
    return setResult(JournalResult::CryptoFailed);
  }
  const uint8_t target = status_.activeSlot < 0
      ? 0U
      : static_cast<uint8_t>(status_.activeSlot ^ 1);
  if (!storage_->writeSlot(target, scratch_, bytes)) {
    return setResult(JournalResult::StorageWriteFailed);
  }

  uint32_t readGeneration = 0U;
  size_t readBytes = 0U;
  status_.lastResult = JournalResult::Ok;
  if (!readAndValidateSlot(target, readGeneration, readBytes) ||
      readGeneration != nextGeneration || readBytes != bytes) {
    return setResult(JournalResult::ReadbackFailed);
  }

  status_.activeSlot = static_cast<int8_t>(target);
  status_.generation = nextGeneration;
  status_.committedBytes = static_cast<uint16_t>(bytes);
  status_.dirty = false;
  return setResult(JournalResult::Ok);
}

bool MeshDiscoveryJournal::peerAt(size_t ordinal,
                                  DiscoveryPeer& output) const {
  size_t found = 0U;
  for (size_t i = 0U; i < kDiscoveryPeerCapacity; ++i) {
    if (!peers_[i].valid) continue;
    if (found == ordinal) {
      output = peers_[i].value;
      return true;
    }
    ++found;
  }
  return false;
}

bool MeshDiscoveryJournal::eventAfter(uint32_t afterSequence,
                                      DiscoveryEvent& output) const {
  for (uint8_t ordinal = 0U; ordinal < status_.eventCount; ++ordinal) {
    const uint8_t index = static_cast<uint8_t>(
        (eventStart_ + ordinal) % kDiscoveryEventCapacity);
    if (sequenceAfter(events_[index].sequence, afterSequence)) {
      output = events_[index];
      return true;
    }
  }
  return false;
}

JournalStatus MeshDiscoveryJournal::status() const { return status_; }

}  // namespace discovery
}  // namespace kitsu868

