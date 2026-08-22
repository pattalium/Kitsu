#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kitsu868 {
namespace connectivity {

// Frozen local BLE firmware-update protocol.  The signed application is
// always written to the non-running OTA slot.  Its final sector is a private
// append-only resume journal and can never contain application bytes.
constexpr uint8_t kBleOtaProtocolVersion = 1U;
constexpr uint32_t kBleOtaAppPartitionBytes = 0x330000UL;
constexpr uint32_t kBleOtaJournalBytes = 0x1000UL;
constexpr uint32_t kBleOtaMaximumImageBytes =
    kBleOtaAppPartitionBytes - kBleOtaJournalBytes;
constexpr uint32_t kBleOtaChunkBytes = 4096UL;
constexpr uint32_t kBleOtaCheckpointBytes = 64UL * 1024UL;
constexpr uint32_t kBleOtaHealthyConfirmationMs = 30000UL;
constexpr uint32_t kBleOtaRebootDelayMs = 750UL;
constexpr uint32_t kBleOtaRebootFallbackMs = 5000UL;
constexpr size_t kBleOtaManifestMaximumBytes = 1024U;
constexpr size_t kBleOtaSignatureBytes = 64U;
constexpr size_t kBleOtaDigestBytes = 32U;
constexpr size_t kBleOtaVersionMaximumBytes = 32U;

enum class BleOtaState : uint8_t {
  Idle = 0,
  Receiving,
  ReadyToReboot,
  PendingVerify,
  Confirmed,
  RolledBack,
  Failed,
};

enum class BleOtaBootState : uint8_t {
  Unknown = 0,
  New,
  PendingVerify,
  Valid,
  Invalid,
  Aborted,
};

struct BleOtaPartition {
  char label[16]{};
  uint32_t address = 0U;
  uint32_t size = 0U;
  uint8_t subtype = 0xffU;
};

struct BleOtaStatus {
  BleOtaState state = BleOtaState::Idle;
  bool begun = false;
  bool updateIdValid = false;
  bool resumed = false;
  bool rebootScheduled = false;
  uint8_t updateId[kBleOtaDigestBytes]{};
  uint32_t imageBytes = 0U;
  uint32_t nextOffset = 0U;
  const char* runningVersion = nullptr;
  const char* error = nullptr;
};

// This narrow boundary keeps all protocol, journal, hashing and resume logic
// host-testable.  The ESP32 implementation below resolves exactly app0/app1;
// callers cannot nominate a partition, address, or flash range over BLE.
class BleOtaPlatform {
 public:
  virtual ~BleOtaPlatform() = default;

  virtual bool resolvePartitions(BleOtaPartition& running,
                                 BleOtaPartition& inactive) = 0;
  virtual bool readPartition(const BleOtaPartition& partition,
                             uint32_t offset, uint8_t* output,
                             size_t outputBytes) = 0;
  virtual bool erasePartition(const BleOtaPartition& partition,
                              uint32_t offset, uint32_t eraseBytes) = 0;
  virtual bool writePartition(const BleOtaPartition& partition,
                              uint32_t offset, const uint8_t* input,
                              size_t inputBytes) = 0;
  virtual bool verifyEspApplication(const BleOtaPartition& partition,
                                    uint32_t imageBytes) = 0;
  virtual bool verifyEd25519(const uint8_t publicKey[kBleOtaDigestBytes],
                             const uint8_t* message, size_t messageBytes,
                             const uint8_t signature[kBleOtaSignatureBytes]) = 0;
  virtual BleOtaBootState bootState(
      const BleOtaPartition& partition) = 0;
  virtual bool setBootPartition(const BleOtaPartition& partition) = 0;
  virtual bool markRunningValid() = 0;
  virtual bool rollbackRunningAndRestart() = 0;
  virtual void restart() = 0;
};

class KitsuBleOta {
 public:
  KitsuBleOta();
  ~KitsuBleOta();

  KitsuBleOta(const KitsuBleOta&) = delete;
  KitsuBleOta& operator=(const KitsuBleOta&) = delete;

  // Call after local storage/security initialization and before BLE begins.
  // runningVersion must remain valid only for this call; it is copied.
  bool begin(BleOtaPlatform& platform, const char* runningVersion);

  // Direct authenticated BLE operation seam.  The caller must route only the
  // six frozen firmware.update.* operation names here after the existing BLE
  // application session has authenticated the controller and envelope.
  bool handleRequest(const char* operation, const uint8_t* payload,
                     size_t payloadBytes, uint8_t* response,
                     size_t responseCapacity, size_t& responseBytes);

  // A pending image is deliberately not confirmed during Arduino init.
  // Call once after critical initialization.  `healthy=false` is an explicit
  // failure and immediately requests rollback.  Then call loop() from every
  // healthy main-loop iteration; 30 continuous seconds are required.
  bool finishCriticalInitialization(bool healthy, uint32_t nowMillis);
  void loop(uint32_t nowMillis, bool criticalHealth, bool transmitIdle);

  BleOtaStatus status() const;
  static const char* stateName(BleOtaState state);

 private:
  friend class KitsuBleOtaTestAccess;
  struct Sha256Context {
    uint32_t state[8]{};
    uint64_t totalBytes = 0U;
    uint8_t block[64]{};
    size_t blockBytes = 0U;
  };

  struct ParsedManifest {
    char releaseId[65]{};
    char firmwareVersion[kBleOtaVersionMaximumBytes + 1U]{};
    uint32_t imageBytes = 0U;
    uint8_t imageSha256[kBleOtaDigestBytes]{};
  };

  bool loadExistingState();
  static void shaStart(Sha256Context& context);
  static void shaTransform(Sha256Context& context,
                           const uint8_t block[64]);
  static void shaUpdate(Sha256Context& context, const uint8_t* input,
                        size_t inputBytes);
  static bool shaFinish(Sha256Context& context,
                        uint8_t output[kBleOtaDigestBytes]);
  static bool parseManifest(const uint8_t* input, size_t inputBytes,
                            ParsedManifest& output);
  bool bindPendingRunningImage();
  bool prepareNewUpdate(const ParsedManifest& manifest,
                        const uint8_t updateId[kBleOtaDigestBytes]);
  bool bindResumedManifest(const ParsedManifest& manifest,
                           const uint8_t updateId[kBleOtaDigestBytes]);
  bool rebuildPrefixHash();
  bool hashPartition(const BleOtaPartition& partition, uint32_t bytes,
                     uint8_t output[kBleOtaDigestBytes]);
  bool comparePartition(uint32_t offset, const uint8_t* expected,
                        size_t expectedBytes);
  bool readJournal(const BleOtaPartition& partition, bool& present,
                   uint8_t& journalState, uint32_t& journalOffset,
                   uint16_t& sequence, uint16_t& nextSlot,
                   uint8_t updateId[kBleOtaDigestBytes],
                   uint8_t imageSha256[kBleOtaDigestBytes],
                   uint32_t& imageBytes,
                   char targetVersion[kBleOtaVersionMaximumBytes + 1U]);
  bool appendCheckpoint(uint8_t journalState, uint32_t nextOffset);
  bool writeJournalHeader(const ParsedManifest& manifest,
                          const uint8_t updateId[kBleOtaDigestBytes]);
  bool clearForIdle();
  bool fail(const char* error, bool terminal = false);
  bool rollback(const char* error);
  bool encodeReceipt(bool ok, bool replayed, bool scheduled,
                     uint8_t* response, size_t responseCapacity,
                     size_t& responseBytes) const;
  bool handleStatus(const uint8_t* payload, size_t payloadBytes,
                    uint8_t* response, size_t responseCapacity,
                    size_t& responseBytes);
  bool handleBegin(const uint8_t* payload, size_t payloadBytes,
                   uint8_t* response, size_t responseCapacity,
                   size_t& responseBytes);
  bool handleWrite(const uint8_t* payload, size_t payloadBytes,
                   uint8_t* response, size_t responseCapacity,
                   size_t& responseBytes);
  bool handleFinish(const uint8_t* payload, size_t payloadBytes,
                    uint8_t* response, size_t responseCapacity,
                    size_t& responseBytes);
  bool handleReboot(const uint8_t* payload, size_t payloadBytes,
                    uint8_t* response, size_t responseCapacity,
                    size_t& responseBytes);
  bool handleAbort(const uint8_t* payload, size_t payloadBytes,
                   uint8_t* response, size_t responseCapacity,
                   size_t& responseBytes);

  BleOtaPlatform* platform_ = nullptr;
  BleOtaPartition running_{};
  BleOtaPartition inactive_{};
  BleOtaState state_ = BleOtaState::Idle;
  bool begun_ = false;
  bool manifestLoaded_ = false;
  bool resumed_ = false;
  bool updateIdValid_ = false;
  bool rebootScheduled_ = false;
  bool confirmationArmed_ = false;
  bool prefixHashReady_ = false;
  bool journalHeaderReady_ = false;
  uint8_t updateId_[kBleOtaDigestBytes]{};
  uint8_t expectedImageSha256_[kBleOtaDigestBytes]{};
  char runningVersion_[kBleOtaVersionMaximumBytes + 1U]{};
  char targetVersion_[kBleOtaVersionMaximumBytes + 1U]{};
  uint32_t imageBytes_ = 0U;
  uint32_t nextOffset_ = 0U;
  uint32_t persistedOffset_ = 0U;
  uint32_t erasedThrough_ = 0U;
  uint16_t journalSequence_ = 0U;
  uint16_t journalNextSlot_ = 0U;
  uint32_t healthySince_ = 0U;
  uint32_t rebootAt_ = 0U;
  uint32_t rebootFallbackAt_ = 0U;
  const char* error_ = nullptr;
  Sha256Context prefixHash_{};
  uint8_t manifestScratch_[kBleOtaManifestMaximumBytes]{};
  uint8_t chunkScratch_[kBleOtaChunkBytes]{};
  uint8_t ioScratch_[256]{};
};

#if defined(ARDUINO_ARCH_ESP32)
// Production adapter.  Its implementation uses only ESP-IDF's partition,
// image-validation and OTA-selection APIs plus the already-vendored MeshCore
// Ed25519 verifier.  No Wi-Fi, HTTP, server, eFuse or whole-flash API exists.
class Esp32KitsuBleOtaPlatform final : public BleOtaPlatform {
 public:
  bool resolvePartitions(BleOtaPartition& running,
                         BleOtaPartition& inactive) override;
  bool readPartition(const BleOtaPartition& partition, uint32_t offset,
                     uint8_t* output, size_t outputBytes) override;
  bool erasePartition(const BleOtaPartition& partition, uint32_t offset,
                      uint32_t eraseBytes) override;
  bool writePartition(const BleOtaPartition& partition, uint32_t offset,
                      const uint8_t* input, size_t inputBytes) override;
  bool verifyEspApplication(const BleOtaPartition& partition,
                            uint32_t imageBytes) override;
  bool verifyEd25519(const uint8_t publicKey[kBleOtaDigestBytes],
                     const uint8_t* message, size_t messageBytes,
                     const uint8_t signature[kBleOtaSignatureBytes]) override;
  BleOtaBootState bootState(const BleOtaPartition& partition) override;
  bool setBootPartition(const BleOtaPartition& partition) override;
  bool markRunningValid() override;
  bool rollbackRunningAndRestart() override;
  void restart() override;

 private:
  const void* native(const BleOtaPartition& partition) const;
  const void* runningNative_ = nullptr;
  const void* inactiveNative_ = nullptr;
};
#endif

}  // namespace connectivity
}  // namespace kitsu868
