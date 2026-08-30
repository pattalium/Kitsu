#include "kitsu_ble_gatt.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <new>
#include <string.h>

// NimBLE-Arduino 2.5.1 exposes this pinned host function from ble_gatts.c but
// does not surface its declaration through the public C++ wrapper.
extern "C" void ble_gatts_set_clt_cfg_perm_flags(uint8_t flags);

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr size_t kGattAttributeMaximumBytes = 512U;
constexpr size_t kEventQueueCapacity = 8U;
constexpr uint16_t kNoConnection = BLE_HS_CONN_HANDLE_NONE;

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

// NimBLE host result values are stable in the pinned 2.5.1 ble_hs.h.  Keep
// this adapter independent of private NimBLE headers while still making the
// retry policy explicit and host-testable.
bool notifyStatusSucceeded(int code) {
  return code == 0 || code == 14;  // BLE_HS_EDONE is indication-complete.
}

bool notifyStatusRetryable(int code) {
  switch (code) {
    case 1:   // BLE_HS_EAGAIN
    case 2:   // BLE_HS_EALREADY (defensive)
    case 6:   // BLE_HS_ENOMEM
    case 15:  // BLE_HS_EBUSY
    case 20:  // BLE_HS_ENOMEM_EVT
      return true;
    default:
      return false;
  }
}

BleCloseCause closeCauseForEvent(BleLinkEvent event) {
  switch (event) {
    case BleLinkEvent::LinkRejected:
      return BleCloseCause::LinkRejected;
    case BleLinkEvent::FrameTimedOut:
      return BleCloseCause::FrameTimedOut;
    case BleLinkEvent::ProtocolViolation:
      return BleCloseCause::ProtocolViolation;
    case BleLinkEvent::TransportFailure:
      return BleCloseCause::TransportFailure;
    default:
      return BleCloseCause::ApplicationRequest;
  }
}

BleCloseCause closeCauseForUnexpectedHciReason(int reason) {
  switch (reason) {
    case 0x13:  // BLE_ERR_REM_USER_CONN_TERM
      return BleCloseCause::RemoteUserTerminated;
    case 0x08:  // BLE_ERR_CONN_SPVN_TMO
      return BleCloseCause::SupervisionTimeout;
    default:
      return BleCloseCause::Unknown;
  }
}

}  // namespace

struct KitsuBleGattLink::Impl {
  explicit Impl(KitsuBleGattLink* owner)
      : owner(owner), serverCallbacks(this), characteristicCallbacks(this) {}

  struct EventRecord {
    BleLinkEvent event = BleLinkEvent::Connected;
    uint32_t generation = 0U;
    uint16_t handle = kNoConnection;
    bool disconnectReasonAvailable = false;
    int disconnectReason = 0;
    uint32_t disconnectAtMillis = 0U;
    bool closeTelemetryAvailable = false;
    BleCloseCause closeCause = BleCloseCause::None;
    bool closeWasLocal = false;
  };

  class ServerCallbacks final : public NimBLEServerCallbacks {
   public:
    explicit ServerCallbacks(Impl* implementation) : impl(implementation) {}

    void onConnect(NimBLEServer* server,
                   NimBLEConnInfo& connection) override {
      impl->onConnect(server, connection);
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo& connection,
                      int reason) override {
      impl->onDisconnect(connection, reason, millis());
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo& connection) override {
      impl->onMtu(mtu, connection);
    }

    void onConfirmPassKey(NimBLEConnInfo& connection,
                          uint32_t passkey) override {
      impl->onConfirmPasskey(connection, passkey);
    }

    void onAuthenticationComplete(NimBLEConnInfo& connection) override {
      impl->onAuthenticationComplete(connection);
    }

   private:
    Impl* impl;
  };

  class CharacteristicCallbacks final
      : public NimBLECharacteristicCallbacks {
   public:
    explicit CharacteristicCallbacks(Impl* implementation)
        : impl(implementation) {}

    void onWrite(NimBLECharacteristic* characteristic,
                 NimBLEConnInfo& connection) override {
      impl->onWrite(characteristic, connection);
    }

    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& connection,
                     uint16_t subscription) override {
      impl->onSubscribe(connection, subscription);
    }

    void onStatus(NimBLECharacteristic*, NimBLEConnInfo& connection,
                  int code) override {
      impl->onNotifyStatus(connection, code);
    }

   private:
    Impl* impl;
  };

  void clearConnectionLocked() {
    connected = false;
    secureConnections = false;
    encrypted = false;
    authenticated = false;
    bonded = false;
    notifySubscribed = false;
    applicationAuthenticated = false;
    pendingApplicationAuthentication = false;
    applicationAuthenticationPending = false;
    deliveringFrame = false;
    numericPending = false;
    numericComparison = 0U;
    numericDeadline = 0U;
    connectionHandle = kNoConnection;
    mtu = 23U;
    requestInFlight = false;
    frameReady = false;
    txQueued = false;
    txBytes = 0U;
    txOffset = 0U;
    notifyAttemptActive = false;
    notifyStatusReady = false;
    notifyAttemptHandle = kNoConnection;
    notifyAttemptGeneration = 0U;
    notifyAttemptOffset = 0U;
    notifyAttemptBytes = 0U;
    notifyAttemptDeadline = 0U;
    notifyRetryAt = 0U;
    notifyAttemptCount = 0U;
    disconnectRequested = false;
    disconnectIssued = false;
    disconnectHandle = kNoConnection;
    disconnectGeneration = 0U;
    pendingLocalClose = false;
    pendingLocalCloseCause = BleCloseCause::None;
    pendingLocalCloseHandle = kNoConnection;
    pendingLocalCloseGeneration = 0U;
    parser.begin(rxStorage, sizeof(rxStorage),
                 companion::kMaximumHandshakeFrameBytes,
                 companion::kFrameAssemblyTimeoutMs);
  }

  void pushEventLocked(BleLinkEvent event, uint32_t generation,
                       uint16_t handle) {
    const uint8_t next = static_cast<uint8_t>(
        (eventWrite + 1U) % kEventQueueCapacity);
    if (next == eventRead) {
      eventRead = static_cast<uint8_t>(
          (eventRead + 1U) % kEventQueueCapacity);
    }
    events[eventWrite] = EventRecord{};
    events[eventWrite].event = event;
    events[eventWrite].generation = generation;
    events[eventWrite].handle = handle;
    eventWrite = next;
  }

  void pushEventLocked(BleLinkEvent event) {
    pushEventLocked(event, connectionGeneration, connectionHandle);
  }

  void markLocalCloseLocked(BleCloseCause cause) {
    if (!connected || connectionHandle == kNoConnection) return;
    const bool sameConnection = pendingLocalClose &&
        pendingLocalCloseHandle == connectionHandle &&
        pendingLocalCloseGeneration == connectionGeneration;
    // Preserve the first reason which actually initiated this generation's
    // close. Follow-on callbacks must not relabel the diagnostic.
    if (sameConnection) return;
    pendingLocalClose = true;
    pendingLocalCloseCause = cause;
    pendingLocalCloseHandle = connectionHandle;
    pendingLocalCloseGeneration = connectionGeneration;
  }

  void requestDisconnectLocked(BleLinkEvent event) {
    pushEventLocked(event);
    if (connected && connectionHandle != kNoConnection) {
      markLocalCloseLocked(closeCauseForEvent(event));
      disconnectRequested = true;
      disconnectIssued = true;
      disconnectHandle = connectionHandle;
      disconnectGeneration = connectionGeneration;
    }
  }

  void onConnect(NimBLEServer* callbackServer,
                 NimBLEConnInfo& connection) {
    const uint16_t handle = connection.getConnHandle();
    bool reject = false;
    portENTER_CRITICAL(&mux);
    if (localControllerRecoveryLocked ||
        (connected && connectionHandle != handle)) {
      reject = true;
    } else {
      clearConnectionLocked();
      ++connectionGeneration;
      if (connectionGeneration == 0U) ++connectionGeneration;
      connected = true;
      advertising = false;
      connectionHandle = handle;
      mtu = connection.getMTU();
      pushEventLocked(BleLinkEvent::Connected);
    }
    portEXIT_CRITICAL(&mux);
    if (reject) {
      callbackServer->disconnect(handle);
      return;
    }
    // Every connection is promoted to encryption immediately.  A returning
    // bonded controller normally completes without another numeric prompt.
    if (!NimBLEDevice::startSecurity(handle)) {
      portENTER_CRITICAL(&mux);
      if (connectionHandle == handle) {
        requestDisconnectLocked(BleLinkEvent::LinkRejected);
      }
      portEXIT_CRITICAL(&mux);
    }
  }

  void onDisconnect(NimBLEConnInfo& connection, int reason,
                    uint32_t nowMillis) {
    portENTER_CRITICAL(&mux);
    if (connectionHandle == connection.getConnHandle()) {
      const uint32_t departedGeneration = connectionGeneration;
      const uint16_t departedHandle = connectionHandle;
      const bool localClose = pendingLocalClose &&
          pendingLocalCloseHandle == departedHandle &&
          pendingLocalCloseGeneration == departedGeneration;
      const BleCloseCause closeCause = localClose
          ? pendingLocalCloseCause
          : closeCauseForUnexpectedHciReason(reason);
      disconnectReasonAvailable = true;
      lastDisconnectReason = reason;
      lastDisconnectedHandle = departedHandle;
      lastDisconnectedGeneration = departedGeneration;
      lastDisconnectAtMillis = nowMillis;
      closeTelemetryAvailable = true;
      lastCloseCause = closeCause;
      lastCloseWasLocal = localClose;
      clearConnectionLocked();
      advertising = !localControllerRecoveryLocked;
      pushEventLocked(BleLinkEvent::Disconnected, departedGeneration,
                      departedHandle);
      const uint8_t eventIndex = static_cast<uint8_t>(
          (eventWrite + kEventQueueCapacity - 1U) % kEventQueueCapacity);
      events[eventIndex].disconnectReasonAvailable = true;
      events[eventIndex].disconnectReason = reason;
      events[eventIndex].disconnectAtMillis = nowMillis;
      events[eventIndex].closeTelemetryAvailable = true;
      events[eventIndex].closeCause = closeCause;
      events[eventIndex].closeWasLocal = localClose;
    }
    portEXIT_CRITICAL(&mux);
  }

  void onMtu(uint16_t updatedMtu, NimBLEConnInfo& connection) {
    portENTER_CRITICAL(&mux);
    if (connectionHandle == connection.getConnHandle()) {
      mtu = updatedMtu < 23U ? 23U : updatedMtu;
    }
    portEXIT_CRITICAL(&mux);
  }

  void onConfirmPasskey(NimBLEConnInfo& connection, uint32_t passkey) {
    const uint16_t handle = connection.getConnHandle();
    bool reject = false;
    portENTER_CRITICAL(&mux);
    const uint32_t now = millis();
    if (connectionHandle != handle || !pairingWindowOpen ||
        deadlineReached(now, pairingWindowDeadline) || numericPending) {
      reject = true;
    } else {
      numericPending = true;
      numericComparison = passkey;
      numericDeadline = now + kBleNumericComparisonTimeoutMs;
      pushEventLocked(BleLinkEvent::NumericComparison);
    }
    portEXIT_CRITICAL(&mux);
    if (reject) NimBLEDevice::injectConfirmPasskey(connection, false);
  }

  void onAuthenticationComplete(NimBLEConnInfo& connection) {
    const uint16_t handle = connection.getConnHandle();
    const bool accepted = connection.isEncrypted() &&
                          connection.isAuthenticated() &&
                          connection.isBonded() &&
                          connection.getSecKeySize() == 16U;
    portENTER_CRITICAL(&mux);
    if (connectionHandle == handle) {
      encrypted = connection.isEncrypted();
      authenticated = connection.isAuthenticated();
      bonded = connection.isBonded();
      // begin() configures NimBLE's SC-only policy.  Consequently an
      // authenticated 128-bit bond accepted by the host cannot be a legacy
      // SMP bond; returning legacy bonds are rejected by NimBLE as well.
      secureConnections = accepted;
      numericPending = false;
      numericComparison = 0U;
      numericDeadline = 0U;
      if (accepted) {
        pushEventLocked(BleLinkEvent::LinkAuthenticated);
      } else {
        requestDisconnectLocked(BleLinkEvent::LinkRejected);
      }
    }
    portEXIT_CRITICAL(&mux);
  }

  void onWrite(NimBLECharacteristic* characteristic,
               NimBLEConnInfo& connection) {
    const NimBLEAttValue& value = characteristic->getValue();
    const uint8_t* input = value.data();
    const size_t inputBytes = value.size();
    portENTER_CRITICAL(&mux);
    // A callback from a departed/other connection is not evidence that the
    // current controller violated the protocol. In particular, never let a
    // late host callback for handle A close a replacement link B.
    if (connectionHandle != connection.getConnHandle()) {
      portEXIT_CRITICAL(&mux);
      return;
    }
    // A client can receive the final response notification before a delayed
    // NOTIFY_TX callback is retired by loop().  Accept one next frame into the
    // independent RX buffer during that interval.  frameReady keeps this queue
    // bounded to one request, and loop() will not deliver it until
    // requestInFlight clears.
    if (!encrypted || !authenticated || !bonded || !notifySubscribed ||
        frameReady || inputBytes == 0U) {
      requestDisconnectLocked(BleLinkEvent::ProtocolViolation);
      portEXIT_CRITICAL(&mux);
      return;
    }
    const companion::FrameResult result = parser.feed(input, inputBytes,
                                                       millis());
    if (result == companion::FrameResult::Ready) {
      frameReady = true;
    } else if (result != companion::FrameResult::NeedMore) {
      requestDisconnectLocked(
          result == companion::FrameResult::TimedOut
              ? BleLinkEvent::FrameTimedOut
              : BleLinkEvent::ProtocolViolation);
    }
    portEXIT_CRITICAL(&mux);
  }

  void onSubscribe(NimBLEConnInfo& connection, uint16_t subscription) {
    portENTER_CRITICAL(&mux);
    if (connectionHandle == connection.getConnHandle()) {
      const bool wasSubscribed = notifySubscribed;
      notifySubscribed = (subscription & 0x01U) != 0U;
      if (wasSubscribed && !notifySubscribed) {
        notifyAttemptActive = false;
        notifyStatusReady = false;
        notifyRetryAt = 0U;
        notifyAttemptCount = 0U;
        txQueued = false;
        txBytes = 0U;
        txOffset = 0U;
        requestInFlight = false;
        requestDisconnectLocked(BleLinkEvent::TransportFailure);
      }
    }
    portEXIT_CRITICAL(&mux);
  }

  void onNotifyStatus(NimBLEConnInfo& connection, int code) {
    // NimBLE-Arduino 2.5.1 does not populate ConnInfo for NOTIFY_TX.  Bind
    // completion to the one internally recorded attempt rather than trusting
    // connection.getConnHandle(), which is commonly zero/default here.
    (void)connection;
    portENTER_CRITICAL(&mux);
    if (notifyAttemptActive && connected &&
        notifyAttemptGeneration == connectionGeneration &&
        notifyAttemptHandle == connectionHandle) {
      notifyAttemptStatus = code;
      notifyStatusReady = true;
      notifyStatusAvailable = true;
      lastNotifyStatus = code;
      lastNotifyStatusAtMillis = millis();
    }
    portEXIT_CRITICAL(&mux);
  }

  bool notifyAttemptMatchesLocked() const {
    return notifyAttemptActive && connected && txQueued &&
        notifyAttemptGeneration == connectionGeneration &&
        notifyAttemptHandle == connectionHandle &&
        txOffset <= txBytes && notifyAttemptOffset == txOffset &&
        notifyAttemptBytes != 0U &&
        notifyAttemptBytes <= txBytes - txOffset;
  }

  void completeNotifyAttemptLocked(uint32_t nowMillis, int code,
                                   bool retryableWithoutStatus = false) {
    if (!notifyAttemptMatchesLocked()) {
      notifyAttemptActive = false;
      notifyStatusReady = false;
      return;
    }

    const size_t completedBytes = notifyAttemptBytes;
    notifyAttemptActive = false;
    notifyStatusReady = false;
    notifyAttemptDeadline = 0U;
    notifyStatusAvailable = true;
    lastNotifyStatus = code;
    lastNotifyStatusAtMillis = nowMillis;

    if (notifyStatusSucceeded(code)) {
      txOffset += completedBytes;
      notifyAttemptCount = 0U;
      notifyRetryAt = 0U;
      if (txOffset == txBytes) {
        txQueued = false;
        txBytes = 0U;
        txOffset = 0U;
        requestInFlight = false;
      }
      return;
    }

    const bool retryable = retryableWithoutStatus ||
        notifyStatusRetryable(code);
    if (retryable && notifyAttemptCount < kBleNotifyMaximumAttempts &&
        notifySubscribed && !disconnectRequested) {
      notifyRetryAt = nowMillis +
          kBleNotifyRetryBaseMs * static_cast<uint32_t>(notifyAttemptCount);
      return;
    }
    notifyRetryAt = 0U;
    requestDisconnectLocked(BleLinkEvent::TransportFailure);
  }

  KitsuBleGattLink* owner;
  BleFrameDelegate* delegate = nullptr;
  NimBLEServer* server = nullptr;
  NimBLECharacteristic* rx = nullptr;
  NimBLECharacteristic* tx = nullptr;
  ServerCallbacks serverCallbacks;
  CharacteristicCallbacks characteristicCallbacks;

  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  companion::LengthFrameParser parser{};
  uint8_t rxStorage[companion::kMaximumFrameBytes]{};
  uint8_t txStorage[companion::kMaximumFrameBytes +
                    companion::kFrameHeaderBytes]{};
  size_t txBytes = 0U;
  size_t txOffset = 0U;

  EventRecord events[kEventQueueCapacity]{};
  uint8_t eventRead = 0U;
  uint8_t eventWrite = 0U;

  bool begun = false;
  bool advertising = false;
  bool connected = false;
  bool secureConnections = false;
  bool encrypted = false;
  bool authenticated = false;
  bool bonded = false;
  bool notifySubscribed = false;
  bool pairingWindowOpen = false;
  bool localControllerRecoveryLocked = false;
  bool numericPending = false;
  bool applicationAuthenticated = false;
  bool pendingApplicationAuthentication = false;
  bool applicationAuthenticationPending = false;
  bool deliveringFrame = false;
  bool frameReady = false;
  bool requestInFlight = false;
  bool txQueued = false;
  bool disconnectRequested = false;
  bool disconnectIssued = false;
  bool notifyAttemptActive = false;
  bool notifyStatusReady = false;
  bool disconnectReasonAvailable = false;
  bool closeTelemetryAvailable = false;
  bool notifyStatusAvailable = false;
  bool pendingLocalClose = false;
  uint16_t connectionHandle = kNoConnection;
  uint16_t disconnectHandle = kNoConnection;
  uint16_t notifyAttemptHandle = kNoConnection;
  uint16_t lastDisconnectedHandle = kNoConnection;
  uint16_t pendingLocalCloseHandle = kNoConnection;
  uint16_t mtu = 23U;
  uint32_t numericComparison = 0U;
  uint32_t numericDeadline = 0U;
  uint32_t pairingWindowDeadline = 0U;
  uint32_t connectionGeneration = 0U;
  uint32_t disconnectGeneration = 0U;
  uint32_t notifyAttemptGeneration = 0U;
  size_t notifyAttemptOffset = 0U;
  size_t notifyAttemptBytes = 0U;
  uint32_t notifyAttemptDeadline = 0U;
  uint32_t notifyRetryAt = 0U;
  uint8_t notifyAttemptCount = 0U;
  int notifyAttemptStatus = 0;
  int lastDisconnectReason = 0;
  int lastNotifyStatus = 0;
  uint32_t lastDisconnectAtMillis = 0U;
  uint32_t lastDisconnectedGeneration = 0U;
  uint32_t pendingLocalCloseGeneration = 0U;
  uint32_t lastNotifyStatusAtMillis = 0U;
  BleCloseCause pendingLocalCloseCause = BleCloseCause::None;
  BleCloseCause lastCloseCause = BleCloseCause::None;
  bool lastCloseWasLocal = false;
};

KitsuBleGattLink::KitsuBleGattLink() = default;

KitsuBleGattLink::~KitsuBleGattLink() { end(); }

bool KitsuBleGattLink::begin(const char* deviceName,
                             BleFrameDelegate& delegate) {
  if (!deviceName || deviceName[0] == '\0' || impl_) return false;
  Impl* implementation = new (std::nothrow) Impl(this);
  if (!implementation) return false;
  impl_ = implementation;
  implementation->delegate = &delegate;
  if (!implementation->parser.begin(
          implementation->rxStorage, sizeof(implementation->rxStorage),
          companion::kMaximumHandshakeFrameBytes,
          companion::kFrameAssemblyTimeoutMs)) {
    end();
    return false;
  }

  NimBLEDevice::init(deviceName);
  NimBLEDevice::setMTU(517U);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);
  NimBLEDevice::setSecurityAuth(true, true, true);
  // Reject legacy SMP instead of negotiating down. Reflashability does not
  // weaken the authenticated Bluetooth owner channel.
  ble_hs_cfg.sm_sc_only = 1U;
  // The automatically generated CCCD is otherwise readable/writable before
  // encryption. Securing it makes Android's successful descriptor callback a
  // concrete link-security boundary instead of a pre-encryption GATT-ready race.
  ble_gatts_set_clt_cfg_perm_flags(
      BLE_ATT_F_READ | BLE_ATT_F_WRITE | BLE_ATT_F_READ_ENC |
      BLE_ATT_F_READ_AUTHEN | BLE_ATT_F_WRITE_ENC |
      BLE_ATT_F_WRITE_AUTHEN);

  implementation->server = NimBLEDevice::createServer();
  if (!implementation->server) {
    end();
    return false;
  }
  implementation->server->setCallbacks(&implementation->serverCallbacks,
                                        false);
  implementation->server->advertiseOnDisconnect(true);

  NimBLEService* service = implementation->server->createService(
      kKitsuGattServiceUuid);
  if (!service) {
    end();
    return false;
  }
  implementation->rx = service->createCharacteristic(
      kKitsuGattRxUuid,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
          NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN,
      static_cast<uint16_t>(kGattAttributeMaximumBytes));
  implementation->tx = service->createCharacteristic(
      kKitsuGattTxUuid, NIMBLE_PROPERTY::NOTIFY,
      static_cast<uint16_t>(kGattAttributeMaximumBytes));
  if (!implementation->rx || !implementation->tx) {
    end();
    return false;
  }
  implementation->rx->setCallbacks(&implementation->characteristicCallbacks);
  implementation->tx->setCallbacks(&implementation->characteristicCallbacks);

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  if (!advertising || !advertising->setName(deviceName) ||
      !advertising->addServiceUUID(kKitsuGattServiceUuid)) {
    end();
    return false;
  }
  advertising->enableScanResponse(true);
  advertising->setMinInterval(160U);  // 100 ms
  advertising->setMaxInterval(240U);  // 150 ms
  if (!advertising->start()) {
    end();
    return false;
  }
  implementation->begun = true;
  implementation->advertising = true;
  return true;
}

void KitsuBleGattLink::end() {
  if (!impl_) return;
  NimBLEDevice::stopAdvertising();
  if (impl_->server && impl_->connected) {
    impl_->server->disconnect(impl_->connectionHandle);
  }
  NimBLEDevice::deinit(true);
  delete impl_;
  impl_ = nullptr;
}

void KitsuBleGattLink::loop(uint32_t nowMillis) {
  if (!impl_ || !impl_->begun) return;

  bool rejectNumeric = false;
  bool disconnectNow = false;
  uint16_t handle = kNoConnection;
  portENTER_CRITICAL(&impl_->mux);
  if (impl_->pairingWindowOpen &&
      deadlineReached(nowMillis, impl_->pairingWindowDeadline)) {
    impl_->pairingWindowOpen = false;
    impl_->pairingWindowDeadline = 0U;
    if (impl_->numericPending) {
      impl_->markLocalCloseLocked(BleCloseCause::LinkRejected);
      rejectNumeric = true;
    }
  }
  if (impl_->numericPending &&
      deadlineReached(nowMillis, impl_->numericDeadline)) {
    rejectNumeric = true;
    impl_->markLocalCloseLocked(BleCloseCause::FrameTimedOut);
    impl_->pushEventLocked(BleLinkEvent::FrameTimedOut);
  }
  disconnectNow = impl_->disconnectRequested && impl_->connected &&
      impl_->disconnectHandle == impl_->connectionHandle &&
      impl_->disconnectGeneration == impl_->connectionGeneration;
  handle = disconnectNow ? impl_->disconnectHandle : kNoConnection;
  if (impl_->disconnectRequested) {
    // Consume both the matching request and a stale token from a dead link.
    // A later connection can never inherit an earlier generation's close.
    impl_->disconnectRequested = false;
    impl_->disconnectHandle = kNoConnection;
    impl_->disconnectGeneration = 0U;
  }
  portEXIT_CRITICAL(&impl_->mux);

  if (rejectNumeric) confirmNumericComparison(false);
  if (disconnectNow && impl_->server && handle != kNoConnection) {
    impl_->server->disconnect(handle);
  }

  // Deliver link events in order from the Arduino loop, not the BLE host task.
  for (;;) {
    Impl::EventRecord record{};
    bool available = false;
    portENTER_CRITICAL(&impl_->mux);
    if (impl_->eventRead != impl_->eventWrite) {
      record = impl_->events[impl_->eventRead];
      impl_->eventRead = static_cast<uint8_t>(
          (impl_->eventRead + 1U) % kEventQueueCapacity);
      available = true;
    }
    portEXIT_CRITICAL(&impl_->mux);
    if (!available) break;
    BleLinkStatus eventStatus = status(nowMillis);
    eventStatus.eventConnectionGeneration = record.generation;
    eventStatus.eventConnectionHandle = record.handle;
    eventStatus.eventMatchesCurrentConnection =
        eventStatus.connected && record.generation != 0U &&
        record.generation == eventStatus.connectionGeneration &&
        record.handle == eventStatus.connectionHandle;
    if (record.disconnectReasonAvailable) {
      eventStatus.disconnectReasonAvailable = true;
      eventStatus.lastDisconnectReason = record.disconnectReason;
      eventStatus.lastDisconnectedHandle = record.handle;
      eventStatus.lastDisconnectedGeneration = record.generation;
      eventStatus.lastDisconnectAtMillis = record.disconnectAtMillis;
    }
    if (record.closeTelemetryAvailable) {
      eventStatus.closeTelemetryAvailable = true;
      eventStatus.lastCloseCause = record.closeCause;
      eventStatus.lastCloseWasLocal = record.closeWasLocal;
    }
    // A queued connect/auth/failure event must never mutate a session after
    // its link has closed or after generation B replaced generation A, even
    // when NimBLE reuses the same numeric handle. A disconnect remains an
    // explicit diagnostic event; the bridge decides whether its generation
    // was the latest closed link before clearing application state.
    if (!eventStatus.eventMatchesCurrentConnection &&
        record.event != BleLinkEvent::Disconnected) {
      continue;
    }
    impl_->delegate->onBleLinkEvent(record.event, eventStatus);
  }

  const uint8_t* frame = nullptr;
  size_t frameBytes = 0U;
  bool deliverFrame = false;
  portENTER_CRITICAL(&impl_->mux);
  if (!impl_->frameReady && impl_->connected) {
    const companion::FrameResult polled = impl_->parser.poll(nowMillis);
    if (polled == companion::FrameResult::TimedOut) {
      impl_->requestDisconnectLocked(BleLinkEvent::FrameTimedOut);
    }
  }
  // The outbound slot is shared by replies and unsolicited events.  An event
  // does not set requestInFlight, so txQueued must independently hold the next
  // request in the RX queue until the event notification has fully retired.
  if (impl_->frameReady && !impl_->requestInFlight && !impl_->txQueued &&
      impl_->parser.frame(frame, frameBytes)) {
    impl_->requestInFlight = true;
    impl_->deliveringFrame = true;
    deliverFrame = true;
  }
  portEXIT_CRITICAL(&impl_->mux);

  if (deliverFrame) {
    impl_->delegate->onBleFrame(frame, frameBytes);
    portENTER_CRITICAL(&impl_->mux);
    impl_->parser.consume();
    impl_->frameReady = false;
    impl_->deliveringFrame = false;
    if (impl_->applicationAuthenticationPending) {
      impl_->applicationAuthenticated =
          impl_->pendingApplicationAuthentication;
      impl_->parser.begin(
          impl_->rxStorage, sizeof(impl_->rxStorage),
          impl_->applicationAuthenticated
              ? companion::kMaximumFrameBytes
              : companion::kMaximumHandshakeFrameBytes,
          companion::kFrameAssemblyTimeoutMs);
      impl_->applicationAuthenticationPending = false;
    }
    portEXIT_CRITICAL(&impl_->mux);
  }

  // Complete or retry exactly one outstanding notification.  NimBLE 2.5.1
  // reports NOTIFY_TX without populated ConnInfo. Its notification callback
  // is normally synchronous with notify(), but the token and timeout also
  // tolerate delayed/missing callbacks in the host or a future stack change.
  portENTER_CRITICAL(&impl_->mux);
  if (impl_->notifyAttemptActive) {
    if (impl_->notifyStatusReady) {
      impl_->completeNotifyAttemptLocked(nowMillis,
                                         impl_->notifyAttemptStatus);
    } else if (deadlineReached(nowMillis, impl_->notifyAttemptDeadline)) {
      // A true return with no callback must not hold requestInFlight forever.
      impl_->completeNotifyAttemptLocked(nowMillis, -2, true);
    }
  }
  portEXIT_CRITICAL(&impl_->mux);

  // At most one MTU chunk attempt per Arduino iteration gives NimBLE bounded
  // backpressure without blocking the creature/radio loop.
  const uint8_t* chunk = nullptr;
  size_t chunkBytes = 0U;
  uint16_t txHandle = kNoConnection;
  uint32_t txGeneration = 0U;
  size_t txOffset = 0U;
  portENTER_CRITICAL(&impl_->mux);
  const bool retryReady = impl_->notifyRetryAt == 0U ||
      deadlineReached(nowMillis, impl_->notifyRetryAt);
  if (!impl_->notifyAttemptActive && !impl_->disconnectIssued &&
      retryReady && impl_->txQueued && impl_->connected && impl_->encrypted &&
      impl_->authenticated && impl_->bonded && impl_->notifySubscribed) {
    const size_t mtuPayload = impl_->mtu > 3U
        ? static_cast<size_t>(impl_->mtu - 3U)
        : 20U;
    const size_t maximumChunk = mtuPayload < kGattAttributeMaximumBytes
        ? mtuPayload
        : kGattAttributeMaximumBytes;
    const size_t remaining = impl_->txBytes - impl_->txOffset;
    chunkBytes = remaining < maximumChunk ? remaining : maximumChunk;
    chunk = impl_->txStorage + impl_->txOffset;
    txHandle = impl_->connectionHandle;
    txGeneration = impl_->connectionGeneration;
    txOffset = impl_->txOffset;
    impl_->notifyAttemptActive = true;
    impl_->notifyStatusReady = false;
    impl_->notifyAttemptHandle = txHandle;
    impl_->notifyAttemptGeneration = txGeneration;
    impl_->notifyAttemptOffset = txOffset;
    impl_->notifyAttemptBytes = chunkBytes;
    impl_->notifyAttemptDeadline = nowMillis +
        kBleNotifyCompletionTimeoutMs;
    impl_->notifyRetryAt = 0U;
    if (impl_->notifyAttemptCount < 0xffU) ++impl_->notifyAttemptCount;
  }
  portEXIT_CRITICAL(&impl_->mux);

  if (chunk) {
    const bool accepted = impl_->tx->notify(chunk, chunkBytes, txHandle);
    portENTER_CRITICAL(&impl_->mux);
    const bool sameAttempt = impl_->notifyAttemptActive &&
        impl_->notifyAttemptHandle == txHandle &&
        impl_->notifyAttemptGeneration == txGeneration &&
        impl_->notifyAttemptOffset == txOffset &&
        impl_->notifyAttemptBytes == chunkBytes;
    if (sameAttempt && impl_->notifyStatusReady) {
      // Handles a synchronous fake/host callback without waiting a loop turn.
      impl_->completeNotifyAttemptLocked(nowMillis,
                                         impl_->notifyAttemptStatus);
    } else if (sameAttempt && !accepted) {
      // mbuf allocation can make notify() return false with no callback and no
      // exposed host code. Treat it as bounded local backpressure.
      impl_->completeNotifyAttemptLocked(nowMillis, -1, true);
    }
    portEXIT_CRITICAL(&impl_->mux);
  }
}

bool KitsuBleGattLink::openPairingWindow(uint32_t nowMillis,
                                         uint32_t durationMs) {
  if (!impl_ || !impl_->begun || durationMs == 0U ||
      durationMs > kBlePairingWindowMaximumMs) {
    return false;
  }
  portENTER_CRITICAL(&impl_->mux);
  if (impl_->localControllerRecoveryLocked) {
    portEXIT_CRITICAL(&impl_->mux);
    return false;
  }
  impl_->pairingWindowOpen = true;
  impl_->pairingWindowDeadline = nowMillis + durationMs;
  portEXIT_CRITICAL(&impl_->mux);
  return true;
}

void KitsuBleGattLink::closePairingWindow() {
  if (!impl_) return;
  bool reject = false;
  portENTER_CRITICAL(&impl_->mux);
  impl_->pairingWindowOpen = false;
  impl_->pairingWindowDeadline = 0U;
  reject = impl_->numericPending;
  if (reject) impl_->markLocalCloseLocked(BleCloseCause::LinkRejected);
  portEXIT_CRITICAL(&impl_->mux);
  if (reject) confirmNumericComparison(false);
}

bool KitsuBleGattLink::setLocalControllerRecoveryLocked(bool locked) {
  if (!impl_ || !impl_->begun || !impl_->server) return false;
  bool rejectNumeric = false;
  bool connected = false;
  portENTER_CRITICAL(&impl_->mux);
  impl_->localControllerRecoveryLocked = locked;
  if (locked) {
    impl_->pairingWindowOpen = false;
    impl_->pairingWindowDeadline = 0U;
    rejectNumeric = impl_->numericPending;
    connected = impl_->connected;
    if (connected) {
      impl_->markLocalCloseLocked(BleCloseCause::ControllerRecovery);
      impl_->disconnectRequested = true;
      impl_->disconnectIssued = true;
      impl_->disconnectHandle = impl_->connectionHandle;
      impl_->disconnectGeneration = impl_->connectionGeneration;
    }
    impl_->advertising = false;
  } else {
    connected = impl_->connected;
  }
  portEXIT_CRITICAL(&impl_->mux);

  impl_->server->advertiseOnDisconnect(!locked);
  if (rejectNumeric) confirmNumericComparison(false);
  if (locked) {
    NimBLEDevice::stopAdvertising();
    return true;
  }
  if (!connected) {
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (!advertising || (!advertising->isAdvertising() &&
                         !advertising->start())) {
      return false;
    }
    portENTER_CRITICAL(&impl_->mux);
    impl_->advertising = true;
    portEXIT_CRITICAL(&impl_->mux);
  }
  return true;
}

bool KitsuBleGattLink::clearAllBondsForLocalRecovery(
    BleBondClearStatus& status) {
  status = BleBondClearStatus{};
  if (!impl_ || !impl_->begun) return false;
  bool allowed = false;
  portENTER_CRITICAL(&impl_->mux);
  allowed = impl_->localControllerRecoveryLocked && !impl_->connected &&
      !impl_->numericPending && !impl_->requestInFlight && !impl_->txQueued;
  portEXIT_CRITICAL(&impl_->mux);
  if (!allowed) return false;

  status.attempted = true;
  status.bondsBefore = NimBLEDevice::getNumBonds();
  status.deleteSucceeded = NimBLEDevice::deleteAllBonds();
  status.bondsAfter = NimBLEDevice::getNumBonds();
  status.verifiedEmpty = status.deleteSucceeded && status.bondsAfter == 0;
  return status.verifiedEmpty;
}

int KitsuBleGattLink::bondCount() const {
  if (!impl_ || !impl_->begun) return -1;
  return NimBLEDevice::getNumBonds();
}

bool KitsuBleGattLink::confirmNumericComparison(bool accept) {
  if (!impl_ || !impl_->server) return false;
  uint16_t handle = kNoConnection;
  portENTER_CRITICAL(&impl_->mux);
  if (impl_->numericPending) {
    if (!accept) {
      impl_->markLocalCloseLocked(BleCloseCause::LinkRejected);
    }
    handle = impl_->connectionHandle;
    impl_->numericPending = false;
    impl_->numericComparison = 0U;
    impl_->numericDeadline = 0U;
  }
  portEXIT_CRITICAL(&impl_->mux);
  if (handle == kNoConnection) return false;
  const NimBLEConnInfo connection = impl_->server->getPeerInfoByHandle(handle);
  return NimBLEDevice::injectConfirmPasskey(connection, accept);
}

bool KitsuBleGattLink::setApplicationAuthenticated(bool authenticated) {
  if (!impl_ || !impl_->begun) return false;
  portENTER_CRITICAL(&impl_->mux);
  if (impl_->deliveringFrame) {
    impl_->pendingApplicationAuthentication = authenticated;
    impl_->applicationAuthenticationPending = true;
    portEXIT_CRITICAL(&impl_->mux);
    return true;
  }
  impl_->applicationAuthenticated = authenticated;
  const bool begun = impl_->parser.begin(
      impl_->rxStorage, sizeof(impl_->rxStorage),
      authenticated ? companion::kMaximumFrameBytes
                    : companion::kMaximumHandshakeFrameBytes,
      companion::kFrameAssemblyTimeoutMs);
  impl_->requestInFlight = false;
  portEXIT_CRITICAL(&impl_->mux);
  return begun;
}

bool KitsuBleGattLink::queueFrame(const uint8_t* json, size_t jsonBytes) {
  if (!impl_ || !impl_->begun || !json || jsonBytes == 0U) return false;
  portENTER_CRITICAL(&impl_->mux);
  const size_t maximum = impl_->applicationAuthenticated
      ? companion::kMaximumFrameBytes
      : companion::kMaximumHandshakeFrameBytes;
  if (impl_->txQueued || jsonBytes > maximum || !impl_->connected ||
      !impl_->encrypted || !impl_->authenticated || !impl_->bonded) {
    portEXIT_CRITICAL(&impl_->mux);
    return false;
  }
  size_t framedBytes = 0U;
  const companion::FrameResult encoded = companion::encodeLengthFrame(
      json, jsonBytes, impl_->txStorage, sizeof(impl_->txStorage),
      framedBytes);
  if (encoded != companion::FrameResult::Ready) {
    portEXIT_CRITICAL(&impl_->mux);
    return false;
  }
  impl_->txBytes = framedBytes;
  impl_->txOffset = 0U;
  impl_->txQueued = true;
  impl_->notifyAttemptActive = false;
  impl_->notifyStatusReady = false;
  impl_->notifyRetryAt = 0U;
  impl_->notifyAttemptCount = 0U;
  portEXIT_CRITICAL(&impl_->mux);
  return true;
}

void KitsuBleGattLink::disconnect() {
  disconnect(BleCloseCause::ApplicationRequest);
}

void KitsuBleGattLink::disconnect(BleCloseCause cause) {
  if (!impl_) return;
  portENTER_CRITICAL(&impl_->mux);
  if (impl_->connected && impl_->connectionHandle != kNoConnection) {
    impl_->markLocalCloseLocked(
        cause == BleCloseCause::None ? BleCloseCause::ApplicationRequest
                                     : cause);
    impl_->disconnectRequested = true;
    impl_->disconnectIssued = true;
    impl_->disconnectHandle = impl_->connectionHandle;
    impl_->disconnectGeneration = impl_->connectionGeneration;
  }
  portEXIT_CRITICAL(&impl_->mux);
}

BleLinkStatus KitsuBleGattLink::status(uint32_t nowMillis) const {
  BleLinkStatus output{};
  if (!impl_) return output;
  portENTER_CRITICAL(&impl_->mux);
  output.begun = impl_->begun;
  output.advertising = impl_->advertising;
  output.connected = impl_->connected;
  output.secureConnections = impl_->secureConnections;
  output.encrypted = impl_->encrypted;
  output.authenticated = impl_->authenticated;
  output.bonded = impl_->bonded;
  output.notifySubscribed = impl_->notifySubscribed;
  output.pairingWindowOpen = impl_->pairingWindowOpen;
  output.numericComparisonPending = impl_->numericPending;
  output.applicationAuthenticated = impl_->applicationAuthenticated;
  output.requestInFlight = impl_->requestInFlight || impl_->txQueued;
  output.connectionHandle = impl_->connectionHandle;
  output.mtu = impl_->mtu;
  output.numericComparison = impl_->numericComparison;
  output.connectionGeneration = impl_->connectionGeneration;
  output.disconnectReasonAvailable = impl_->disconnectReasonAvailable;
  output.lastDisconnectReason = impl_->lastDisconnectReason;
  output.lastDisconnectedHandle = impl_->lastDisconnectedHandle;
  output.lastDisconnectedGeneration = impl_->lastDisconnectedGeneration;
  output.lastDisconnectAtMillis = impl_->lastDisconnectAtMillis;
  output.closeTelemetryAvailable = impl_->closeTelemetryAvailable;
  output.lastCloseCause = impl_->lastCloseCause;
  output.lastCloseWasLocal = impl_->lastCloseWasLocal;
  output.notifyStatusAvailable = impl_->notifyStatusAvailable;
  output.lastNotifyStatus = impl_->lastNotifyStatus;
  output.lastNotifyStatusAtMillis = impl_->lastNotifyStatusAtMillis;
  if (impl_->pairingWindowOpen &&
      !deadlineReached(nowMillis, impl_->pairingWindowDeadline)) {
    output.pairingWindowRemainingMs =
        impl_->pairingWindowDeadline - nowMillis;
  }
  portEXIT_CRITICAL(&impl_->mux);
  return output;
}

}  // namespace connectivity
}  // namespace kitsu868

#else

namespace kitsu868 {
namespace connectivity {

KitsuBleGattLink::KitsuBleGattLink() = default;
KitsuBleGattLink::~KitsuBleGattLink() = default;
bool KitsuBleGattLink::begin(const char*, BleFrameDelegate&) { return false; }
void KitsuBleGattLink::end() {}
void KitsuBleGattLink::loop(uint32_t) {}
bool KitsuBleGattLink::openPairingWindow(uint32_t, uint32_t) { return false; }
void KitsuBleGattLink::closePairingWindow() {}
bool KitsuBleGattLink::confirmNumericComparison(bool) { return false; }
bool KitsuBleGattLink::setLocalControllerRecoveryLocked(bool) {
  return false;
}
bool KitsuBleGattLink::clearAllBondsForLocalRecovery(BleBondClearStatus& status) {
  status = BleBondClearStatus{};
  return false;
}
int KitsuBleGattLink::bondCount() const { return -1; }
bool KitsuBleGattLink::setApplicationAuthenticated(bool) { return false; }
bool KitsuBleGattLink::queueFrame(const uint8_t*, size_t) { return false; }
void KitsuBleGattLink::disconnect() {}
void KitsuBleGattLink::disconnect(BleCloseCause) {}
BleLinkStatus KitsuBleGattLink::status(uint32_t) const {
  return BleLinkStatus{};
}

}  // namespace connectivity
}  // namespace kitsu868

#endif
