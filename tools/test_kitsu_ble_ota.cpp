#include "../src/kitsu_ble_ota.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

using kitsu868::connectivity::BleOtaBootState;
using kitsu868::connectivity::BleOtaPartition;
using kitsu868::connectivity::BleOtaPlatform;
using kitsu868::connectivity::BleOtaState;
using kitsu868::connectivity::KitsuBleOta;
using kitsu868::connectivity::kBleOtaAppPartitionBytes;
using kitsu868::connectivity::kBleOtaDigestBytes;
using kitsu868::connectivity::kBleOtaHealthyConfirmationMs;
using kitsu868::connectivity::kBleOtaMaximumImageBytes;
using kitsu868::connectivity::kBleOtaSignatureBytes;
using kitsu868::connectivity::kKitsuApp0Offset;
using kitsu868::connectivity::kKitsuApp1Offset;

namespace kitsu868 {
namespace connectivity {

class KitsuBleOtaTestAccess {
 public:
  using Context = KitsuBleOta::Sha256Context;
  static void start(Context& context) { KitsuBleOta::shaStart(context); }
  static void update(Context& context, const uint8_t* input, size_t bytes) {
    KitsuBleOta::shaUpdate(context, input, bytes);
  }
  static bool finish(Context& context, uint8_t output[kBleOtaDigestBytes]) {
    return KitsuBleOta::shaFinish(context, output);
  }
};

}  // namespace connectivity
}  // namespace kitsu868

namespace {

constexpr uint32_t kImageBytes = 66048U;
constexpr char kImageSha[] =
    "891968beb7c5eddd2dd24957463901786c15b707d0dd64bcd24ef21444645d06";

const uint8_t kExpectedAuthority[kBleOtaDigestBytes] = {
    0x24U, 0x00U, 0x11U, 0xf1U, 0x49U, 0xe9U, 0xcfU, 0xb9U,
    0xfbU, 0x87U, 0xfaU, 0xb4U, 0xd9U, 0xcaU, 0x45U, 0x73U,
    0xc1U, 0xc7U, 0xffU, 0xb3U, 0x87U, 0x17U, 0x76U, 0x58U,
    0x00U, 0x05U, 0xecU, 0x41U, 0xaeU, 0xe5U, 0x13U, 0x82U,
};

std::string base64Url(const uint8_t* input, size_t bytes) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string output;
  output.reserve((bytes * 4U + 2U) / 3U);
  size_t cursor = 0U;
  while (cursor + 3U <= bytes) {
    const uint32_t value = (static_cast<uint32_t>(input[cursor]) << 16U) |
        (static_cast<uint32_t>(input[cursor + 1U]) << 8U) |
        input[cursor + 2U];
    output.push_back(alphabet[(value >> 18U) & 63U]);
    output.push_back(alphabet[(value >> 12U) & 63U]);
    output.push_back(alphabet[(value >> 6U) & 63U]);
    output.push_back(alphabet[value & 63U]);
    cursor += 3U;
  }
  if (bytes - cursor == 1U) {
    const uint32_t value = static_cast<uint32_t>(input[cursor]) << 16U;
    output.push_back(alphabet[(value >> 18U) & 63U]);
    output.push_back(alphabet[(value >> 12U) & 63U]);
  } else if (bytes - cursor == 2U) {
    const uint32_t value = (static_cast<uint32_t>(input[cursor]) << 16U) |
        (static_cast<uint32_t>(input[cursor + 1U]) << 8U);
    output.push_back(alphabet[(value >> 18U) & 63U]);
    output.push_back(alphabet[(value >> 12U) & 63U]);
    output.push_back(alphabet[(value >> 6U) & 63U]);
  }
  return output;
}

std::string makeManifest(const char* version = "0.12.0") {
  return std::string("{\"schema\":\"kitsu.ble-firmware.v1\","
                     "\"release_id\":\"test-1\","
                     "\"firmware_version\":\"") + version +
      "\",\"device_class\":\"heltec-wifi-lora-32-v3-esp32s3-8mb\","
      "\"image_format\":\"esp32s3-app\",\"image_bytes\":" +
      std::to_string(kImageBytes) + " ,BROKEN";
}

std::string exactManifest(const char* version = "0.12.0") {
  return std::string("{\"schema\":\"kitsu.ble-firmware.v1\","
                     "\"release_id\":\"test-1\","
                     "\"firmware_version\":\"") + version +
      "\",\"device_class\":\"heltec-wifi-lora-32-v3-esp32s3-8mb\","
      "\"image_format\":\"esp32s3-app\",\"image_bytes\":" +
      std::to_string(kImageBytes) +
      ",\"image_sha256\":\"" + kImageSha +
      "\",\"partition_bytes\":" +
      std::to_string(kBleOtaAppPartitionBytes) +
      ",\"chunk_bytes\":4096,"
      "\"rollback\":true}";
}

std::string beginRequest(const std::string& manifest,
                         uint8_t signatureByte = 0xa5U) {
  uint8_t signature[kBleOtaSignatureBytes]{};
  memset(signature, signatureByte, sizeof(signature));
  return std::string("{\"manifest_b64\":\"") +
      base64Url(reinterpret_cast<const uint8_t*>(manifest.data()),
                manifest.size()) +
      "\",\"signature_b64\":\"" + base64Url(signature, sizeof(signature)) +
      "\"}";
}

std::string writeRequest(const std::string& updateId, uint32_t offset,
                         const uint8_t* data, size_t bytes) {
  return std::string("{\"update_id\":\"") + updateId +
      "\",\"offset\":" + std::to_string(offset) +
      ",\"data_b64\":\"" + base64Url(data, bytes) + "\"}";
}

std::string idRequest(const std::string& updateId) {
  return std::string("{\"update_id\":\"") + updateId + "\"}";
}

class MemoryPlatform final : public BleOtaPlatform {
 public:
  MemoryPlatform()
      : app0(kBleOtaAppPartitionBytes, 0xffU),
        app1(kBleOtaAppPartitionBytes, 0xffU) {}

  bool resolvePartitions(BleOtaPartition& running,
                         BleOtaPartition& inactive) override {
    running = partition(runningApp1 ? "app1" : "app0",
                        runningApp1 ? kKitsuApp1Offset : kKitsuApp0Offset,
                        runningApp1 ? 0x11U : 0x10U);
    inactive = partition(runningApp1 ? "app0" : "app1",
                         runningApp1 ? kKitsuApp0Offset : kKitsuApp1Offset,
                         runningApp1 ? 0x10U : 0x11U);
    if (badInactiveSubtype) inactive.subtype = 0x82U;
    if (badInactiveAddress) inactive.address += 0x10000U;
    return resolveOk;
  }

  bool readPartition(const BleOtaPartition& partitionValue, uint32_t offset,
                     uint8_t* output, size_t outputBytes) override {
    std::vector<uint8_t>* storage = forPartition(partitionValue);
    if (!storage || !output || offset > storage->size() ||
        outputBytes > storage->size() - offset || failRead) {
      return false;
    }
    memcpy(output, storage->data() + offset, outputBytes);
    return true;
  }

  bool erasePartition(const BleOtaPartition& partitionValue, uint32_t offset,
                      uint32_t eraseBytes) override {
    std::vector<uint8_t>* storage = forPartition(partitionValue);
    if (!storage || offset > storage->size() ||
        eraseBytes > storage->size() - offset || failErase) {
      return false;
    }
    if (partitionValue.address == runningAddress()) wroteRunning = true;
    std::fill(storage->begin() + offset,
              storage->begin() + offset + eraseBytes,
              static_cast<uint8_t>(0xffU));
    eraseRanges.emplace_back(offset, eraseBytes);
    return true;
  }

  bool writePartition(const BleOtaPartition& partitionValue, uint32_t offset,
                      const uint8_t* input, size_t inputBytes) override {
    std::vector<uint8_t>* storage = forPartition(partitionValue);
    if (!storage || !input || offset > storage->size() ||
        inputBytes > storage->size() - offset || failWrite) {
      return false;
    }
    if (partitionValue.address == runningAddress()) wroteRunning = true;
    for (size_t index = 0U; index < inputBytes; ++index) {
      // Real NOR writes may only clear bits between erases.
      (*storage)[offset + index] &= input[index];
    }
    ++writeCallCount;
    if (corruptOnWriteCall == writeCallCount) {
      for (size_t index = 0U; index < inputBytes; ++index) {
        const uint8_t value = (*storage)[offset + index];
        if (value != 0U) {
          (*storage)[offset + index] = static_cast<uint8_t>(
              value & static_cast<uint8_t>(value - 1U));
          break;
        }
      }
    }
    writeRanges.emplace_back(offset, static_cast<uint32_t>(inputBytes));
    return true;
  }

  bool verifyEspApplication(const BleOtaPartition& partitionValue,
                            uint32_t imageBytes) override {
    std::vector<uint8_t>* storage = forPartition(partitionValue);
    ++imageVerifyCalls;
    return verifyImageOk && storage && imageBytes == kImageBytes &&
        (*storage)[0] == 0xe9U;
  }

  bool verifyEd25519(const uint8_t publicKey[kBleOtaDigestBytes],
                     const uint8_t*, size_t messageBytes,
                     const uint8_t signature[kBleOtaSignatureBytes]) override {
    ++signatureVerifyCalls;
    authorityPinned = memcmp(publicKey, kExpectedAuthority,
                             sizeof(kExpectedAuthority)) == 0;
    return authorityPinned && messageBytes != 0U &&
        std::all_of(signature, signature + kBleOtaSignatureBytes,
                    [](uint8_t value) { return value == 0xa5U; });
  }

  BleOtaBootState bootState(const BleOtaPartition& partitionValue) override {
    return partitionValue.address == kKitsuApp0Offset ? app0State : app1State;
  }

  bool setBootPartition(const BleOtaPartition& partitionValue) override {
    if (failBootSelect || !forPartition(partitionValue)) return false;
    selectedBootAddress = partitionValue.address;
    if (partitionValue.address != runningAddress()) {
      if (partitionValue.address == kKitsuApp0Offset) {
        app0State = BleOtaBootState::New;
      }
      else app1State = BleOtaBootState::New;
    }
    return true;
  }

  bool markRunningValid() override {
    ++markValidCalls;
    if (!markValidOk) return false;
    if (runningApp1) app1State = BleOtaBootState::Valid;
    else app0State = BleOtaBootState::Valid;
    return true;
  }

  bool rollbackRunningAndRestart() override {
    rollbackCalled = true;
    return rollbackOk;
  }

  void restart() override { restartCalled = true; }

  static BleOtaPartition partition(const char* label, uint32_t address,
                                   uint8_t subtype) {
    BleOtaPartition output{};
    memcpy(output.label, label, strlen(label) + 1U);
    output.address = address;
    output.size = kBleOtaAppPartitionBytes;
    output.subtype = subtype;
    return output;
  }

  uint32_t runningAddress() const {
    return runningApp1 ? kKitsuApp1Offset : kKitsuApp0Offset;
  }

  std::vector<uint8_t>* forPartition(const BleOtaPartition& value) {
    if (value.address == kKitsuApp0Offset && value.size == app0.size()) {
      return &app0;
    }
    if (value.address == kKitsuApp1Offset && value.size == app1.size()) {
      return &app1;
    }
    return nullptr;
  }

  std::vector<uint8_t> app0;
  std::vector<uint8_t> app1;
  std::vector<std::pair<uint32_t, uint32_t>> eraseRanges;
  std::vector<std::pair<uint32_t, uint32_t>> writeRanges;
  BleOtaBootState app0State = BleOtaBootState::Valid;
  BleOtaBootState app1State = BleOtaBootState::Unknown;
  uint32_t selectedBootAddress = kKitsuApp0Offset;
  int signatureVerifyCalls = 0;
  int imageVerifyCalls = 0;
  int markValidCalls = 0;
  int writeCallCount = 0;
  int corruptOnWriteCall = -1;
  bool runningApp1 = false;
  bool badInactiveSubtype = false;
  bool badInactiveAddress = false;
  bool resolveOk = true;
  bool verifyImageOk = true;
  bool markValidOk = true;
  bool rollbackOk = true;
  bool failRead = false;
  bool failErase = false;
  bool failWrite = false;
  bool failBootSelect = false;
  bool authorityPinned = false;
  bool wroteRunning = false;
  bool rollbackCalled = false;
  bool restartCalled = false;
};

std::string call(KitsuBleOta& ota, const char* operation,
                 const std::string& request) {
  uint8_t response[768]{};
  size_t responseBytes = 0U;
  const bool encoded = ota.handleRequest(
      operation, reinterpret_cast<const uint8_t*>(request.data()),
      request.size(), response, sizeof(response), responseBytes);
  assert(encoded);
  assert(responseBytes != 0U);
  return std::string(reinterpret_cast<const char*>(response), responseBytes);
}

std::string updateIdFrom(const std::string& receipt) {
  const std::string marker = "\"update_id\":\"";
  const size_t start = receipt.find(marker);
  assert(start != std::string::npos);
  const size_t idStart = start + marker.size();
  assert(idStart + 64U <= receipt.size());
  return receipt.substr(idStart, 64U);
}

void expect(const std::string& receipt, const char* token) {
  if (receipt.find(token) == std::string::npos) {
    fprintf(stderr, "missing token %s in %s\n", token, receipt.c_str());
  }
  assert(receipt.find(token) != std::string::npos);
}

std::vector<uint8_t> fixtureImage() {
  std::vector<uint8_t> image(kImageBytes);
  for (size_t index = 0U; index < image.size(); ++index) {
    image[index] = static_cast<uint8_t>(index * 37U + 11U);
  }
  image[0] = 0xe9U;
  return image;
}

void testSha256KnownVector() {
  using Access = kitsu868::connectivity::KitsuBleOtaTestAccess;
  Access::Context context{};
  uint8_t digest[kBleOtaDigestBytes]{};
  Access::start(context);
  static const uint8_t message[] = {'a', 'b', 'c'};
  Access::update(context, message, sizeof(message));
  assert(Access::finish(context, digest));
  static const uint8_t expected[kBleOtaDigestBytes] = {
      0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
      0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
      0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
      0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU,
  };
  if (memcmp(digest, expected, sizeof(digest)) != 0) {
    fprintf(stderr, "bad sha: ");
    for (uint8_t value : digest) fprintf(stderr, "%02x", value);
    fprintf(stderr, "\n");
  }
  assert(memcmp(digest, expected, sizeof(digest)) == 0);
}

void testSignatureAndManifestFailClosed() {
  MemoryPlatform platform;
  KitsuBleOta ota;
  assert(ota.begin(platform, "0.11.4"));
  const size_t erasesBefore = platform.eraseRanges.size();

  const std::string badSignature = call(
      ota, "firmware.update.begin", beginRequest(exactManifest(), 0x55U));
  expect(badSignature, "\"ok\":false");
  expect(badSignature, "\"error\":\"invalid_signature\"");
  assert(platform.eraseRanges.size() == erasesBefore);
  assert(platform.authorityPinned);

  const std::string malformed = call(
      ota, "firmware.update.begin", beginRequest(makeManifest()));
  expect(malformed, "\"error\":\"malformed_request\"");
  assert(platform.eraseRanges.size() == erasesBefore);
}

void testResumeFinishAndThirtySecondConfirmation() {
  MemoryPlatform platform;
  KitsuBleOta first;
  assert(first.begin(platform, "0.11.4"));
  const std::string manifest = exactManifest();
  std::string receipt = call(first, "firmware.update.begin",
                             beginRequest(manifest));
  expect(receipt, "\"state\":\"receiving\"");
  expect(receipt, "\"chunk_bytes\":4096");
  const std::string updateId = updateIdFrom(receipt);
  assert(platform.authorityPinned);
  assert(!platform.wroteRunning);

  const std::vector<uint8_t> image = fixtureImage();
  uint32_t offset = 0U;
  size_t chunk = 256U;
  while (offset < 65792U) {
    chunk = std::min<size_t>(chunk, 65792U - offset);
    receipt = call(first, "firmware.update.write",
                   writeRequest(updateId, offset, image.data() + offset,
                                chunk));
    expect(receipt, "\"ok\":true");
    offset += static_cast<uint32_t>(chunk);
    chunk = 4096U;
  }
  assert(offset == 65792U);

  // A new firmware object models a power loss.  It recovers the durable
  // 64-KiB checkpoint and erases only the inactive suffix, bounding replay.
  KitsuBleOta resumed;
  assert(resumed.begin(platform, "0.11.4"));
  assert(resumed.status().state == BleOtaState::Receiving);
  assert(resumed.status().nextOffset == 65536U);
  assert(resumed.status().resumed);
  receipt = call(resumed, "firmware.update.status", "{}");
  expect(receipt, "\"error\":null");
  receipt = call(resumed, "firmware.update.begin", beginRequest(manifest));
  expect(receipt, "\"resumed\":true");
  expect(receipt, "\"error\":null");

  offset = 65536U;
  while (offset < image.size()) {
    const size_t bytes = std::min<size_t>(256U, image.size() - offset);
    receipt = call(resumed, "firmware.update.write",
                   writeRequest(updateId, offset, image.data() + offset,
                                bytes));
    expect(receipt, "\"ok\":true");
    offset += static_cast<uint32_t>(bytes);
  }
  // Lost-response replay is accepted only when the on-flash bytes match.
  receipt = call(resumed, "firmware.update.write",
                 writeRequest(updateId, kImageBytes - 256U,
                              image.data() + kImageBytes - 256U, 256U));
  expect(receipt, "\"replayed\":true");
  std::vector<uint8_t> wrong(image.end() - 256, image.end());
  wrong[0] ^= 1U;
  receipt = call(resumed, "firmware.update.write",
                 writeRequest(updateId, kImageBytes - 256U, wrong.data(),
                              wrong.size()));
  expect(receipt, "\"ok\":false");
  expect(receipt, "\"error\":\"chunk_mismatch\"");
  // A transient request error must never leak into a later successful receipt.
  receipt = call(resumed, "firmware.update.status", "{}");
  expect(receipt, "\"ok\":true");
  expect(receipt, "\"state\":\"receiving\"");
  expect(receipt, "\"error\":null");

  receipt = call(resumed, "firmware.update.finish", idRequest(updateId));
  expect(receipt, "\"ok\":true");
  expect(receipt, "\"state\":\"ready_to_reboot\"");
  assert(platform.imageVerifyCalls == 1);
  assert(platform.selectedBootAddress == kKitsuApp1Offset);
  receipt = call(resumed, "firmware.update.reboot", idRequest(updateId));
  expect(receipt, "\"scheduled\":true");
  resumed.loop(10U, true, false);
  resumed.loop(760U, true, false);
  assert(!platform.restartCalled);
  resumed.loop(760U, true, true);
  assert(platform.restartCalled);
  assert(!platform.wroteRunning);
  for (const auto& range : platform.eraseRanges) {
    assert(range.second <= 8192U);
  }

  // Boot the newly selected slot.  Its signed raw hash/journal binding is
  // checked immediately, but validity is withheld for 30 healthy seconds.
  platform.runningApp1 = true;
  platform.app1State = BleOtaBootState::PendingVerify;
  platform.app0State = BleOtaBootState::Valid;
  KitsuBleOta booted;
  assert(booted.begin(platform, "0.12.0"));
  assert(booted.status().state == BleOtaState::PendingVerify);
  assert(booted.finishCriticalInitialization(true, 1000U));
  booted.loop(1000U + kBleOtaHealthyConfirmationMs - 1U, true, true);
  assert(platform.markValidCalls == 0);
  booted.loop(1000U + kBleOtaHealthyConfirmationMs, true, true);
  assert(platform.markValidCalls == 1);
  assert(booted.status().state == BleOtaState::Confirmed);
}

void testFailureRollbackAndRecoveryDowngrade() {
  MemoryPlatform platform;
  const std::vector<uint8_t> image = fixtureImage();
  memcpy(platform.app1.data(), image.data(), image.size());
  // Build a complete journal using the normal path, then model first boot.
  KitsuBleOta installer;
  assert(installer.begin(platform, "0.12.0"));
  std::string receipt = call(installer, "firmware.update.begin",
                             beginRequest(exactManifest("0.11.0")));
  const std::string id = updateIdFrom(receipt);
  uint32_t offset = 0U;
  while (offset < image.size()) {
    const size_t bytes = std::min<size_t>(4096U, image.size() - offset);
    receipt = call(installer, "firmware.update.write",
                   writeRequest(id, offset, image.data() + offset, bytes));
    expect(receipt, "\"ok\":true");
    offset += static_cast<uint32_t>(bytes);
  }
  receipt = call(installer, "firmware.update.finish", idRequest(id));
  expect(receipt, "\"ok\":true");
  // Same-version/downgrade is intentionally accepted for signed recovery.
  expect(receipt, "\"state\":\"ready_to_reboot\"");

  platform.runningApp1 = true;
  platform.app1State = BleOtaBootState::PendingVerify;
  KitsuBleOta unhealthy;
  assert(unhealthy.begin(platform, "0.11.0"));
  assert(!unhealthy.finishCriticalInitialization(false, 0U));
  assert(platform.rollbackCalled);
}

void testAbortRestoresRunningBootSelection() {
  MemoryPlatform platform;
  KitsuBleOta ota;
  assert(ota.begin(platform, "0.11.4"));
  std::string receipt = call(ota, "firmware.update.begin",
                             beginRequest(exactManifest()));
  const std::string id = updateIdFrom(receipt);
  receipt = call(ota, "firmware.update.abort", idRequest(id));
  expect(receipt, "\"ok\":true");
  expect(receipt, "\"state\":\"idle\"");
  expect(receipt, "\"update_id\":null");
  assert(platform.selectedBootAddress == platform.runningAddress());
  assert(!platform.wroteRunning);
}

void testJournalReadbackAndFailedAbortRecovery() {
  const std::string manifest = exactManifest();
  const std::string packageBegin = beginRequest(manifest);
  const std::string anyId(64U, '0');

  MemoryPlatform badHeader;
  badHeader.corruptOnWriteCall = 1;
  KitsuBleOta first;
  assert(first.begin(badHeader, "0.11.4"));
  std::string receipt = call(first, "firmware.update.begin", packageBegin);
  expect(receipt, "\"state\":\"failed\"");
  expect(receipt, "\"error\":\"journal_failed\"");
  receipt = call(first, "firmware.update.status", "{}");
  expect(receipt, "\"ok\":true");
  expect(receipt, "\"state\":\"failed\"");
  expect(receipt, "\"error\":\"journal_failed\"");
  receipt = call(first, "firmware.update.abort", idRequest(anyId));
  expect(receipt, "\"ok\":true");
  expect(receipt, "\"state\":\"idle\"");
  expect(receipt, "\"error\":null");

  MemoryPlatform badCheckpoint;
  badCheckpoint.corruptOnWriteCall = 2;
  KitsuBleOta second;
  assert(second.begin(badCheckpoint, "0.11.4"));
  receipt = call(second, "firmware.update.begin", packageBegin);
  const std::string realId = updateIdFrom(receipt);
  expect(receipt, "\"state\":\"failed\"");
  receipt = call(second, "firmware.update.abort", idRequest(realId));
  expect(receipt, "\"ok\":true");
  expect(receipt, "\"state\":\"idle\"");
}

void testPartitionInvariantAndDataReadback() {
  MemoryPlatform badPartition;
  badPartition.badInactiveSubtype = true;
  KitsuBleOta rejected;
  assert(!rejected.begin(badPartition, "0.11.4"));
  assert(badPartition.eraseRanges.empty());
  assert(badPartition.writeRanges.empty());

  MemoryPlatform badAddress;
  badAddress.badInactiveAddress = true;
  KitsuBleOta addressRejected;
  assert(!addressRejected.begin(badAddress, "0.11.4"));
  assert(badAddress.eraseRanges.empty());
  assert(badAddress.writeRanges.empty());

  MemoryPlatform corruptData;
  // Begin writes and reads back the journal header and initial checkpoint;
  // corrupt the first image-data write after those two durable records.
  corruptData.corruptOnWriteCall = 3;
  KitsuBleOta ota;
  assert(ota.begin(corruptData, "0.11.4"));
  std::string receipt = call(ota, "firmware.update.begin",
                             beginRequest(exactManifest()));
  const std::string id = updateIdFrom(receipt);
  const std::vector<uint8_t> image = fixtureImage();
  receipt = call(ota, "firmware.update.write",
                 writeRequest(id, 0U, image.data(), 256U));
  expect(receipt, "\"ok\":false");
  expect(receipt, "\"state\":\"failed\"");
  expect(receipt, "\"error\":\"flash_read_failed\"");
  receipt = call(ota, "firmware.update.status", "{}");
  expect(receipt, "\"ok\":true");
  expect(receipt, "\"state\":\"failed\"");
  expect(receipt, "\"error\":\"flash_read_failed\"");
  receipt = call(ota, "firmware.update.abort", idRequest(id));
  expect(receipt, "\"ok\":true");
  expect(receipt, "\"state\":\"idle\"");
  expect(receipt, "\"error\":null");
  assert(!corruptData.wroteRunning);
}

void testStrictSemverGrammar() {
  const char* accepted[] = {
      "0.20.3",
      "0.20.3-rc.1+build.7",
      "1.0.0-alpha.0",
      "1.0.0-x-y-z.--",
      "1.0.0+001",
      "2147483648.999999999.0",
  };
  for (const char* version : accepted) {
    MemoryPlatform platform;
    KitsuBleOta ota;
    assert(ota.begin(platform, version));
  }

  const char* rejected[] = {
      "0.20.3+",
      "0.20.3-",
      "0.20.3-rc.1+",
      "0.20.3-rc..1",
      "0.20.3+build..7",
      "0.20.3-01",
      "0.20.3-rc.01",
      "01.20.3",
      "0.020.3",
      "0.20.03",
      "0.20",
      "0.20.3_rc1",
      "0.20.3++build",
      "0.20.3-rc+build+again",
      "999999999999999999999.0.0",
      "9223372036854775808.0.0",
  };
  for (const char* version : rejected) {
    MemoryPlatform platform;
    KitsuBleOta ota;
    assert(!ota.begin(platform, version));
  }
}

void testPendingImageWithoutValidJournalRollsBackAtBegin() {
  MemoryPlatform missingJournal;
  missingJournal.runningApp1 = true;
  missingJournal.app1State = BleOtaBootState::PendingVerify;
  KitsuBleOta missing;
  assert(!missing.begin(missingJournal, "0.12.0"));
  assert(missingJournal.rollbackCalled);
  assert(missing.status().state == BleOtaState::Failed);
  assert(std::string(missing.status().error) == "image_invalid");
  assert(!missingJournal.wroteRunning);

  MemoryPlatform corruptJournal;
  corruptJournal.runningApp1 = true;
  corruptJournal.app1State = BleOtaBootState::PendingVerify;
  memcpy(corruptJournal.app1.data() + kBleOtaMaximumImageBytes,
         "BAD!", 4U);
  KitsuBleOta corrupt;
  assert(!corrupt.begin(corruptJournal, "0.12.0"));
  assert(corruptJournal.rollbackCalled);
  assert(corrupt.status().state == BleOtaState::Failed);
  assert(std::string(corrupt.status().error) == "image_invalid");
  assert(!corruptJournal.wroteRunning);
}

}  // namespace

int main() {
  testSha256KnownVector();
  testSignatureAndManifestFailClosed();
  testResumeFinishAndThirtySecondConfirmation();
  testFailureRollbackAndRecoveryDowngrade();
  testAbortRestoresRunningBootSelection();
  testJournalReadbackAndFailedAbortRecovery();
  testPartitionInvariantAndDataReadback();
  testStrictSemverGrammar();
  testPendingImageWithoutValidJournalRollsBackAtBegin();
  static_assert(kBleOtaMaximumImageBytes == 0x2ff000UL,
                "the final inactive-app sector is the OTA journal");
  return 0;
}
