#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_companion_protocol.h"

namespace kitsu868 {
namespace connectivity {

// Frozen Kitsu companion GATT surface.  Frames are always a uint32 big-endian
// byte count followed by exactly that many UTF-8 JSON bytes.
constexpr char kKitsuGattServiceUuid[] =
    "7f820001-735b-4b57-9a48-5f5f4b495453";
constexpr char kKitsuGattRxUuid[] =
    "7f820002-735b-4b57-9a48-5f5f4b495453";
constexpr char kKitsuGattTxUuid[] =
    "7f820003-735b-4b57-9a48-5f5f4b495453";
constexpr uint32_t kBlePairingWindowMaximumMs = 60000UL;
constexpr uint32_t kBleNumericComparisonTimeoutMs = 30000UL;

enum class BleLinkEvent : uint8_t {
  Connected = 0,
  NumericComparison,
  LinkAuthenticated,
  LinkRejected,
  FrameTimedOut,
  ProtocolViolation,
  Disconnected,
};

struct BleLinkStatus {
  bool begun = false;
  bool advertising = false;
  bool connected = false;
  bool secureConnections = false;
  bool encrypted = false;
  bool authenticated = false;
  bool bonded = false;
  bool notifySubscribed = false;
  bool pairingWindowOpen = false;
  bool numericComparisonPending = false;
  bool applicationAuthenticated = false;
  bool requestInFlight = false;
  uint16_t connectionHandle = 0xffffU;
  uint16_t mtu = 23U;
  uint32_t numericComparison = 0U;
  uint32_t pairingWindowRemainingMs = 0U;
};

class BleFrameDelegate {
 public:
  virtual ~BleFrameDelegate() = default;

  // Called only from KitsuBleGattLink::loop(), never from the NimBLE host task.
  // The input remains valid for the duration of the call.  A normal request
  // handler should queue its framed response synchronously or shortly after.
  virtual void onBleFrame(const uint8_t* json, size_t jsonBytes) = 0;
  virtual void onBleLinkEvent(BleLinkEvent event,
                              const BleLinkStatus& status) {
    (void)event;
    (void)status;
  }
};

// NimBLE adapter only.  Controller proofs, session HKDF, envelope replay
// checks, pairing grants, and operation authorization live above this class so
// the same protocol engine can be host-tested without a Bluetooth stack.
class KitsuBleGattLink {
 public:
  KitsuBleGattLink();
  ~KitsuBleGattLink();

  KitsuBleGattLink(const KitsuBleGattLink&) = delete;
  KitsuBleGattLink& operator=(const KitsuBleGattLink&) = delete;

  bool begin(const char* deviceName, BleFrameDelegate& delegate);
  void end();
  void loop(uint32_t nowMillis);

  bool openPairingWindow(uint32_t nowMillis, uint32_t durationMs);
  void closePairingWindow();
  bool confirmNumericComparison(bool accept);

  // Stops advertising, rejects new links, and disconnects the current link
  // while the owner is using the physical controller-recovery UI. This lock
  // is RAM-only and does not grant or expose any BLE operation.
  bool setLocalControllerRecoveryLocked(bool locked);

  // Switches the inbound bound from the 1 KiB pre-auth/handshake maximum to
  // the frozen 16 KiB envelope maximum.  This also drops any partial frame.
  bool setApplicationAuthenticated(bool authenticated);

  // Exactly one outbound frame is buffered.  TX is notification-only and is
  // chunked to the peer MTU; false means the caller must retry later.
  bool queueFrame(const uint8_t* json, size_t jsonBytes);
  void disconnect();
  BleLinkStatus status(uint32_t nowMillis) const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace connectivity
}  // namespace kitsu868
