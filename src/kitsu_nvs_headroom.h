#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kitsu868 {
namespace connectivity {

// These are the only retired NVS records this module is allowed to remove.
constexpr char kRetiredActionReplayNamespace[] = "wisp868";
constexpr char kRetiredActionReplayKey[] = "ble_act_v1";
constexpr char kRetiredBluedroidNamespace[] = "bt_config.conf";
constexpr char kRetiredBluedroidKey[] = "bt_cfg_key0";

// ESP-IDF 4.4 NVS v2 stores a variable-length value in one metadata entry,
// ceil(payload / 32) data entries, and one blob-index entry.
constexpr size_t kNvsEntryBytes = 32U;
constexpr size_t kNvsEntriesPerPage = 126U;
constexpr size_t nvsBlobEntries(size_t payloadBytes) {
  return 2U + (payloadBytes + kNvsEntryBytes - 1U) / kNvsEntryBytes;
}

constexpr size_t kSecuritySlotBytes = 364U;
constexpr size_t kSecurityTransactionEntries =
    nvsBlobEntries(kSecuritySlotBytes);

// One observed NimBLE-Arduino 2.5.1 controller bond: our_sec (88), peer_sec
// (88), rpa_rec (14), csfc_sec (8), and two cccd_sec records (16 each).
// local_irk and the nimble_bond namespace already exist before pairing opens.
constexpr size_t kNimbleBondEntries =
    nvsBlobEntries(88U) + nvsBlobEntries(88U) + nvsBlobEntries(14U) +
    nvsBlobEntries(8U) + 2U * nvsBlobEntries(16U);
constexpr size_t kPairingSafetyEntries = 12U;
constexpr size_t kPairingRequiredUsableEntries =
    kNimbleBondEntries + kSecurityTransactionEntries +
    kPairingSafetyEntries;
// IDF 4.4 reports the reserved garbage-collection page as free. Keep it plus
// the complete pairing/controller transaction budget available.
constexpr size_t kPairingRequiredFreeEntries =
    kNvsEntriesPerPage + kPairingRequiredUsableEntries;

static_assert(kSecurityTransactionEntries == 14U,
              "364-byte security slot must use 14 NVS entries");
static_assert(kNimbleBondEntries == 22U,
              "observed NimBLE controller bond must use 22 NVS entries");
static_assert(kPairingRequiredUsableEntries == 48U,
              "pairing headroom contract changed");
static_assert(kPairingRequiredFreeEntries == 174U,
              "pairing free-entry threshold changed");

struct NvsHeadroomStats {
  size_t usedEntries = 0U;
  size_t freeEntries = 0U;
  size_t totalEntries = 0U;
};

enum class NvsHeadroomResult : uint8_t {
  ReadyAlreadyClean = 0,
  ReadyReclaimed,
  RetiredActionReplayCleanupFailed,
  RetiredBluedroidCleanupFailed,
  StatsFailed,
  Insufficient,
};

struct NvsHeadroomStatus {
  NvsHeadroomResult result = NvsHeadroomResult::StatsFailed;
  bool retiredActionReplay = false;
  bool retiredBluedroid = false;
  NvsHeadroomStats stats{};
};

// Deliberately exposes no caller-selected namespace, key, or erase-all
// primitive. Implementations can remove only the two exact retired records.
class NvsHeadroomPlatform {
 public:
  virtual ~NvsHeadroomPlatform() = default;
  virtual bool eraseRetiredActionReplay(bool& changed) = 0;
  virtual bool eraseRetiredBluedroidConfig(bool& changed) = 0;
  virtual bool readStats(NvsHeadroomStats& output) = 0;
};

class KitsuNvsHeadroom {
 public:
  static NvsHeadroomStatus preparePairing(NvsHeadroomPlatform& platform);
};

bool pairingHeadroomReady(const NvsHeadroomStatus& status);
size_t pairingUsableEntries(const NvsHeadroomStatus& status);
const char* nvsHeadroomResultName(NvsHeadroomResult result);

#if defined(ARDUINO_ARCH_ESP32)

class Esp32NvsHeadroomPlatform final : public NvsHeadroomPlatform {
 public:
  bool eraseRetiredActionReplay(bool& changed) override;
  bool eraseRetiredBluedroidConfig(bool& changed) override;
  bool readStats(NvsHeadroomStats& output) override;
};

#endif  // ARDUINO_ARCH_ESP32

}  // namespace connectivity
}  // namespace kitsu868
