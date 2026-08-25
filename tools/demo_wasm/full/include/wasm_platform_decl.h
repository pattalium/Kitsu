#pragma once

// main.cpp intentionally names the ESP32 production adapters. Host builds do
// not receive those declarations, so the full-firmware WebAssembly target
// supplies source-compatible adapters before main.cpp includes its headers.

#include "Arduino.h"
#include "kitsu_ble_ota.h"
#include "kitsu_companion_protocol.h"
#include "kitsu_device_security.h"
#include "kitsu_legacy_connectivity_retirement.h"
#include "mesh_discovery_journal.h"

namespace kitsu868 {
namespace connectivity {

class Esp32DeviceSecurityStorage final : public DeviceSecurityStorage {
 public:
  Esp32DeviceSecurityStorage();
  bool begin(const char* partitionLabel = nullptr);
  void end();
  bool readSlot(uint8_t slot, uint8_t* output, size_t capacity,
                size_t& outputBytes) override;
  bool writeSlot(uint8_t slot, const uint8_t* input,
                 size_t inputBytes) override;
  bool clearSlot(uint8_t slot) override;
  uint32_t exportPersistent(uint8_t* output, uint32_t capacity) const;
  bool importPersistent(const uint8_t* input, uint32_t bytes);
  void clearPersistent();

 private:
  uint8_t slots_[kSecuritySlots][kSecurityBlobCapacity]{};
  size_t sizes_[kSecuritySlots]{};
  bool begun_ = false;
};

class Esp32DeviceSecurityPlatform final : public DeviceSecurityPlatform {
 public:
  SecurityMode securityMode() const override;
  bool deriveWrappingKey(const uint8_t hardwareId[8],
                         uint8_t output[kKitsuSecretBytes]) override;
  bool randomBytes(uint8_t* output, size_t outputBytes) override;
  bool seal(const uint8_t key[kKitsuSecretBytes], uint32_t generation,
            const uint8_t nonce[kSecurityNonceBytes],
            const uint8_t* plaintext, size_t plaintextBytes,
            uint8_t* ciphertext, uint8_t tag[kSecurityTagBytes]) override;
  bool open(const uint8_t key[kKitsuSecretBytes], uint32_t generation,
            const uint8_t nonce[kSecurityNonceBytes],
            const uint8_t* ciphertext, size_t ciphertextBytes,
            const uint8_t tag[kSecurityTagBytes],
            uint8_t* plaintext) override;
  bool hkdfSha256(const uint8_t* inputKey, size_t inputKeyBytes,
                  const uint8_t* salt, size_t saltBytes,
                  const uint8_t* info, size_t infoBytes,
                  uint8_t* output, size_t outputBytes) override;
};

class Esp32LegacyConnectivityRetirementPlatform final
    : public LegacyConnectivityRetirementPlatform {
 public:
  bool inspectPartition(LegacyConnectivityPartition& output) override;
  bool readPartition(size_t offset, uint8_t* output,
                     size_t outputBytes) override;
  bool eraseEntirePartition() override;
  bool clearLegacyReplayNamespace(bool& changed) override;
};

class Esp32KitsuBleOtaPlatform final : public BleOtaPlatform {
 public:
  Esp32KitsuBleOtaPlatform();
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
  uint8_t* persistentPartition(uint32_t slot);
  uint32_t activeBootSlot() const;
  bool restoreActiveBootSlot(uint32_t slot);
  void clearPersistent();

 private:
  uint8_t* bytesFor(const BleOtaPartition& partition);
  const uint8_t* bytesFor(const BleOtaPartition& partition) const;
  void initialize();

  uint8_t app0_[kBleOtaAppPartitionBytes]{};
  uint8_t app1_[kBleOtaAppPartitionBytes]{};
  bool initialized_ = false;
  bool app0Running_ = true;
};

class Esp32JournalStorage final : public discovery::JournalStorage {
 public:
  Esp32JournalStorage();
  bool begin(const char* partitionLabel = nullptr);
  void end();
  bool readSlot(uint8_t slot, uint8_t* output, size_t capacity,
                size_t& outputBytes) override;
  bool writeSlot(uint8_t slot, const uint8_t* input,
                 size_t inputBytes) override;
  uint32_t exportPersistent(uint8_t* output, uint32_t capacity) const;
  bool importPersistent(const uint8_t* input, uint32_t bytes);
  void clearPersistent();

 private:
  uint8_t slots_[discovery::kDiscoveryJournalSlots]
                [discovery::kDiscoverySnapshotCapacity]{};
  size_t sizes_[discovery::kDiscoveryJournalSlots]{};
  bool begun_ = false;
};

class Esp32JournalCrypto final : public discovery::JournalCrypto {
 public:
  bool setKey(const uint8_t key[kKitsuSecretBytes]);
  bool randomNonce(
      uint8_t output[discovery::kDiscoveryNonceBytes]) override;
  bool seal(uint32_t generation,
            const uint8_t nonce[discovery::kDiscoveryNonceBytes],
            const uint8_t* plaintext, size_t plaintextBytes,
            uint8_t* ciphertext,
            uint8_t tag[discovery::kDiscoveryTagBytes]) override;
  bool open(uint32_t generation,
            const uint8_t nonce[discovery::kDiscoveryNonceBytes],
            const uint8_t* ciphertext, size_t ciphertextBytes,
            const uint8_t tag[discovery::kDiscoveryTagBytes],
            uint8_t* plaintext) override;

 private:
  uint8_t key_[kKitsuSecretBytes]{};
  bool keyed_ = false;
};

class Esp32CompanionCrypto final : public companion::CompanionCrypto {
 public:
  bool randomBytes(uint8_t* output, size_t outputBytes) override;
  bool sha256(const companion::CryptoPart* parts, size_t partCount,
              uint8_t output[32]) override;
  bool hmacSha256(const uint8_t key[companion::kEnvelopeKeyBytes],
                  const companion::CryptoPart* parts, size_t partCount,
                  uint8_t output[companion::kEnvelopeMacBytes]) override;
  bool hkdfSha256(
      const uint8_t inputKey[companion::kEnvelopeKeyBytes],
      const uint8_t* salt, size_t saltBytes, const uint8_t* info,
      size_t infoBytes,
      uint8_t output[companion::kEnvelopeKeyBytes]) override;
};

}  // namespace connectivity
}  // namespace kitsu868
