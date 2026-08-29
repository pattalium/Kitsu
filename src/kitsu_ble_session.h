#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_companion_protocol.h"
#include "kitsu_device_security.h"

namespace kitsu868 {
namespace connectivity {

constexpr uint32_t kBleHandshakeTotalTimeoutMs = 10000UL;
constexpr uint32_t kBleControllerBackoffMs = 30000UL;
constexpr uint8_t kBleMaximumProofFailures = 3U;

enum class BleSessionState : uint8_t {
  Disconnected = 0,
  AwaitingHello,
  AwaitingClientAuth,
  Authenticated,
  PairingAwaitingPhysical,
  PairingAwaitingCommit,
  Closing,
  Backoff,
};

struct BleSessionStatus {
  BleSessionState state = BleSessionState::Disconnected;
  bool begun = false;
  bool secureLink = false;
  bool pairingWindowOpen = false;
  bool physicalConfirmationPending = false;
  bool applicationAuthenticated = false;
  // True only after one authenticated request has been verified and its
  // matching response has been accepted by the BLE transport on this link.
  bool authenticatedRequestBarrier = false;
  uint8_t proofFailures = 0U;
  uint64_t nextClientSequence = 1U;
  uint64_t nextDeviceSequence = 1U;
  uint32_t deadlineRemainingMs = 0U;
  uint32_t backoffRemainingMs = 0U;
};

class BleSessionTransport {
 public:
  virtual ~BleSessionTransport() = default;
  virtual bool sendBleJson(const uint8_t* json, size_t jsonBytes) = 0;
  virtual bool setBleApplicationAuthenticated(bool authenticated) = 0;
  virtual bool bleTransmitIdle() const = 0;
  virtual void disconnectBle() = 0;
};

class BleOperationDelegate {
 public:
  virtual ~BleOperationDelegate() = default;

  // The request has already passed strict outer parsing, HMAC validation,
  // exact sequence validation, UTF-8 validation, and JSON syntax validation.
  // Return one bounded JSON response payload.  Operation-specific schemas,
  // authorization and action-id idempotency belong here.
  virtual bool handleBleRequest(
      const companion::DecodedEnvelope& request, const uint8_t* payload,
      size_t payloadBytes, uint8_t* responsePayload,
      size_t responseCapacity, size_t& responseBytes) = 0;
};

// Host-testable application session layered above the encrypted/bonded GATT
// link.  Controller roots only prove the handshake and feed HKDF; independent
// per-direction keys authenticate all post-handshake envelopes.
class KitsuBleSession {
 public:
  KitsuBleSession();
  ~KitsuBleSession();

  KitsuBleSession(const KitsuBleSession&) = delete;
  KitsuBleSession& operator=(const KitsuBleSession&) = delete;

  bool begin(KitsuDeviceSecurity& security,
             companion::CompanionCrypto& crypto,
             BleSessionTransport& transport,
             BleOperationDelegate& operations, const char deviceUid[7]);

  void onSecureLinkEstablished(bool secureConnections, bool encrypted,
                               bool authenticated, bool bonded,
                               uint32_t nowMillis);
  void onLinkClosed(uint32_t nowMillis);
  void setPairingWindow(bool open, uint32_t remainingMs,
                        uint32_t nowMillis);
  void onFrame(const uint8_t* json, size_t jsonBytes,
               uint32_t nowMillis);
  void loop(uint32_t nowMillis);

  // Called only from the explicit Pair Phone screen after a PRG hold.
  bool confirmPendingPairing(uint32_t nowMillis);
  void cancelPendingPairing();

  bool sendEvent(const char* operation, const uint8_t* payload,
                 size_t payloadBytes);
  BleSessionStatus status(uint32_t nowMillis) const;

 private:
  void clearSessionSecrets();
  void clearPendingPairing();
  void resetForSecureLink(uint32_t nowMillis);
  void failProof(uint32_t nowMillis);
  void failAndClose(uint32_t nowMillis);
  bool sendControlError(const char* code);
  bool handleClientHello(const uint8_t* json, size_t jsonBytes,
                         uint32_t nowMillis);
  bool handleClientAuth(const uint8_t* json, size_t jsonBytes,
                        uint32_t nowMillis);
  bool handlePairRequest(const uint8_t* json, size_t jsonBytes,
                         uint32_t nowMillis);
  bool handlePairCommit(const uint8_t* json, size_t jsonBytes,
                        uint32_t nowMillis);
  bool handleAuthenticatedEnvelope(const uint8_t* json, size_t jsonBytes,
                                   uint32_t nowMillis);
  bool sendAuthenticated( companion::EnvelopeChannel channel,
                         const uint8_t requestId[16], const char* operation,
                         const uint8_t* payload, size_t payloadBytes);

  KitsuDeviceSecurity* security_ = nullptr;
  companion::CompanionCrypto* crypto_ = nullptr;
  BleSessionTransport* transport_ = nullptr;
  BleOperationDelegate* operations_ = nullptr;
  char deviceUid_[7]{};
  BleSessionState state_ = BleSessionState::Disconnected;
  bool begun_ = false;
  bool secureConnections_ = false;
  bool linkEncrypted_ = false;
  bool linkAuthenticated_ = false;
  bool linkBonded_ = false;
  bool pairingWindowOpen_ = false;
  bool controllerKnown_ = false;
  bool authenticatedRequestBarrier_ = false;
  uint8_t proofFailures_ = 0U;
  uint32_t stateDeadline_ = 0U;
  uint32_t pairingWindowDeadline_ = 0U;
  uint32_t closeAt_ = 0U;
  bool closeAfterTransmit_ = false;
  uint32_t backoffUntil_ = 0U;
  uint64_t expectedClientSequence_ = 1U;
  uint64_t nextDeviceSequence_ = 1U;

  uint8_t controllerId_[16]{};
  uint8_t controllerRoot_[32]{};
  uint8_t clientNonce_[16]{};
  uint8_t deviceNonce_[16]{};
  uint8_t clientToDeviceKey_[32]{};
  uint8_t deviceToClientKey_[32]{};

  uint8_t pendingControllerId_[16]{};
  uint8_t pendingControllerRoot_[32]{};
  uint8_t pendingClientNonce_[16]{};
  uint8_t pendingDeviceNonce_[16]{};

  uint8_t payloadScratch_[companion::kMaximumEnvelopePayloadBytes]{};
  uint8_t responseScratch_[companion::kMaximumEnvelopePayloadBytes]{};
  uint8_t jsonScratch_[companion::kMaximumFrameBytes]{};
};

}  // namespace connectivity
}  // namespace kitsu868
