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
constexpr uint32_t kBleNotifyCompletionTimeoutMs = 250UL;
constexpr uint32_t kBleNotifyRetryBaseMs = 8UL;
constexpr uint8_t kBleNotifyMaximumAttempts = 4U;

enum class BleLinkEvent : uint8_t {
  Connected = 0,
  NumericComparison,
  LinkAuthenticated,
  LinkRejected,
  FrameTimedOut,
  ProtocolViolation,
  // Local NimBLE/host transport failure. This is deliberately distinct from
  // a malformed or unauthenticated peer frame.
  TransportFailure,
  Disconnected,
};

// Volatile diagnostic classification for the most recently closed BLE link.
// This never contains an address, controller identifier, key, or other peer
// material, and is deliberately not persisted.
enum class BleCloseCause : uint8_t {
  None = 0,
  RemoteUserTerminated,
  SupervisionTimeout,
  Unknown,
  LinkRejected,
  FrameTimedOut,
  ProtocolViolation,
  TransportFailure,
  SecureLinkRejected,
  HandshakeTimeout,
  AuthenticationFailed,
  SessionProtocolViolation,
  ResponseSendFailed,
  PairingFailed,
  PairingTimeout,
  ControllerForget,
  AuthenticationBackoff,
  ApplicationRequest,
  ControllerRecovery,
};

inline const char* bleCloseCauseName(BleCloseCause cause) {
  switch (cause) {
    case BleCloseCause::None: return "none";
    case BleCloseCause::RemoteUserTerminated:
      return "remote_user_terminated";
    case BleCloseCause::SupervisionTimeout: return "supervision_timeout";
    case BleCloseCause::Unknown: return "unknown";
    case BleCloseCause::LinkRejected: return "link_rejected";
    case BleCloseCause::FrameTimedOut: return "frame_timed_out";
    case BleCloseCause::ProtocolViolation: return "protocol_violation";
    case BleCloseCause::TransportFailure: return "transport_failure";
    case BleCloseCause::SecureLinkRejected: return "secure_link_rejected";
    case BleCloseCause::HandshakeTimeout: return "handshake_timeout";
    case BleCloseCause::AuthenticationFailed: return "authentication_failed";
    case BleCloseCause::SessionProtocolViolation:
      return "session_protocol_violation";
    case BleCloseCause::ResponseSendFailed: return "response_send_failed";
    case BleCloseCause::PairingFailed: return "pairing_failed";
    case BleCloseCause::PairingTimeout: return "pairing_timeout";
    case BleCloseCause::ControllerForget: return "controller_forget";
    case BleCloseCause::AuthenticationBackoff:
      return "authentication_backoff";
    case BleCloseCause::ApplicationRequest: return "application_request";
    case BleCloseCause::ControllerRecovery: return "controller_recovery";
  }
  return "none";
}

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
  uint32_t connectionGeneration = 0U;
  uint32_t eventConnectionGeneration = 0U;
  uint16_t eventConnectionHandle = 0xffffU;
  bool eventMatchesCurrentConnection = false;
  bool disconnectReasonAvailable = false;
  int32_t lastDisconnectReason = 0;
  uint16_t lastDisconnectedHandle = 0xffffU;
  uint32_t lastDisconnectedGeneration = 0U;
  uint32_t lastDisconnectAtMillis = 0U;
  bool closeTelemetryAvailable = false;
  BleCloseCause lastCloseCause = BleCloseCause::None;
  bool lastCloseWasLocal = false;
  bool notifyStatusAvailable = false;
  int32_t lastNotifyStatus = 0;
  uint32_t lastNotifyStatusAtMillis = 0U;
};

struct BleBondClearStatus {
  bool attempted = false;
  bool deleteSucceeded = false;
  bool verifiedEmpty = false;
  int bondsBefore = -1;
  int bondsAfter = -1;
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

  // Deletes only NimBLE's peer bond records. The caller must hold the local
  // recovery lock, prove there is no live link, and independently verify that
  // application controller roots remain byte-for-byte unchanged.
  bool clearAllBondsForLocalRecovery(BleBondClearStatus& status);
  int bondCount() const;

  // Switches the inbound bound from the 1 KiB pre-auth/handshake maximum to
  // the frozen 16 KiB envelope maximum.  This also drops any partial frame.
  bool setApplicationAuthenticated(bool authenticated);

  // Exactly one outbound frame is buffered.  TX is notification-only and is
  // chunked to the peer MTU; false means the caller must retry later.
  bool queueFrame(const uint8_t* json, size_t jsonBytes);
  void disconnect();
  void disconnect(BleCloseCause cause);
  BleLinkStatus status(uint32_t nowMillis) const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace connectivity
}  // namespace kitsu868
