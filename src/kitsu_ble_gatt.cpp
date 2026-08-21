#include "kitsu_ble_gatt.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <new>
#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr size_t kGattAttributeMaximumBytes = 512U;
constexpr size_t kEventQueueCapacity = 8U;
constexpr uint16_t kNoConnection = BLE_HS_CONN_HANDLE_NONE;

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace

struct KitsuBleGattLink::Impl {
  explicit Impl(KitsuBleGattLink* owner)
      : owner(owner), serverCallbacks(this), characteristicCallbacks(this) {}

  struct EventRecord {
    BleLinkEvent event = BleLinkEvent::Connected;
  };

  class ServerCallbacks final : public NimBLEServerCallbacks {
   public:
    explicit ServerCallbacks(Impl* implementation) : impl(implementation) {}

    void onConnect(NimBLEServer* server,
                   NimBLEConnInfo& connection) override {
      impl->onConnect(server, connection);
    }

    void onDisconnect(NimBLEServer*, NimBLEConnInfo& connection,
                      int) override {
      impl->onDisconnect(connection);
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
    parser.begin(rxStorage, sizeof(rxStorage),
                 companion::kMaximumHandshakeFrameBytes,
                 companion::kFrameAssemblyTimeoutMs);
  }

  void pushEventLocked(BleLinkEvent event) {
    const uint8_t next = static_cast<uint8_t>(
        (eventWrite + 1U) % kEventQueueCapacity);
    if (next == eventRead) {
      eventRead = static_cast<uint8_t>(
          (eventRead + 1U) % kEventQueueCapacity);
    }
    events[eventWrite].event = event;
    eventWrite = next;
  }

  void requestDisconnectLocked(BleLinkEvent event) {
    pushEventLocked(event);
    disconnectRequested = true;
  }

  void onConnect(NimBLEServer* callbackServer,
                 NimBLEConnInfo& connection) {
    const uint16_t handle = connection.getConnHandle();
    bool reject = false;
    portENTER_CRITICAL(&mux);
    if (connected && connectionHandle != handle) {
      reject = true;
    } else {
      clearConnectionLocked();
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

  void onDisconnect(NimBLEConnInfo& connection) {
    portENTER_CRITICAL(&mux);
    if (connectionHandle == connection.getConnHandle()) {
      clearConnectionLocked();
      advertising = true;
      pushEventLocked(BleLinkEvent::Disconnected);
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
    if (connectionHandle != connection.getConnHandle() ||
        !encrypted || !authenticated || !bonded || requestInFlight ||
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
      notifySubscribed = (subscription & 0x01U) != 0U;
    }
    portEXIT_CRITICAL(&mux);
  }

  void onNotifyStatus(NimBLEConnInfo& connection, int code) {
    if (code == 0) return;
    portENTER_CRITICAL(&mux);
    if (connectionHandle == connection.getConnHandle()) {
      requestDisconnectLocked(BleLinkEvent::ProtocolViolation);
    }
    portEXIT_CRITICAL(&mux);
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
  bool numericPending = false;
  bool applicationAuthenticated = false;
  bool pendingApplicationAuthentication = false;
  bool applicationAuthenticationPending = false;
  bool deliveringFrame = false;
  bool frameReady = false;
  bool requestInFlight = false;
  bool txQueued = false;
  bool disconnectRequested = false;
  uint16_t connectionHandle = kNoConnection;
  uint16_t mtu = 23U;
  uint32_t numericComparison = 0U;
  uint32_t numericDeadline = 0U;
  uint32_t pairingWindowDeadline = 0U;
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
    if (impl_->numericPending) rejectNumeric = true;
  }
  if (impl_->numericPending &&
      deadlineReached(nowMillis, impl_->numericDeadline)) {
    rejectNumeric = true;
    impl_->pushEventLocked(BleLinkEvent::FrameTimedOut);
  }
  handle = impl_->connectionHandle;
  disconnectNow = impl_->disconnectRequested;
  impl_->disconnectRequested = false;
  portEXIT_CRITICAL(&impl_->mux);

  if (rejectNumeric) confirmNumericComparison(false);
  if (disconnectNow && impl_->server && handle != kNoConnection) {
    impl_->server->disconnect(handle);
  }

  // Deliver link events in order from the Arduino loop, not the BLE host task.
  for (;;) {
    BleLinkEvent event = BleLinkEvent::Connected;
    bool available = false;
    portENTER_CRITICAL(&impl_->mux);
    if (impl_->eventRead != impl_->eventWrite) {
      event = impl_->events[impl_->eventRead].event;
      impl_->eventRead = static_cast<uint8_t>(
          (impl_->eventRead + 1U) % kEventQueueCapacity);
      available = true;
    }
    portEXIT_CRITICAL(&impl_->mux);
    if (!available) break;
    impl_->delegate->onBleLinkEvent(event, status(nowMillis));
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
  if (impl_->frameReady && !impl_->requestInFlight &&
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

  // At most one MTU chunk per Arduino iteration gives NimBLE bounded
  // backpressure without blocking the creature/radio loop.
  const uint8_t* chunk = nullptr;
  size_t chunkBytes = 0U;
  uint16_t txHandle = kNoConnection;
  portENTER_CRITICAL(&impl_->mux);
  if (impl_->txQueued && impl_->connected && impl_->encrypted &&
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
  }
  portEXIT_CRITICAL(&impl_->mux);

  if (chunk && impl_->tx->notify(chunk, chunkBytes, txHandle)) {
    portENTER_CRITICAL(&impl_->mux);
    if (impl_->connectionHandle == txHandle && impl_->txQueued) {
      impl_->txOffset += chunkBytes;
      if (impl_->txOffset == impl_->txBytes) {
        impl_->txQueued = false;
        impl_->txBytes = 0U;
        impl_->txOffset = 0U;
        impl_->requestInFlight = false;
      }
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
  portEXIT_CRITICAL(&impl_->mux);
  if (reject) confirmNumericComparison(false);
}

bool KitsuBleGattLink::confirmNumericComparison(bool accept) {
  if (!impl_ || !impl_->server) return false;
  uint16_t handle = kNoConnection;
  portENTER_CRITICAL(&impl_->mux);
  if (impl_->numericPending) {
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
  portEXIT_CRITICAL(&impl_->mux);
  return true;
}

void KitsuBleGattLink::disconnect() {
  if (!impl_) return;
  portENTER_CRITICAL(&impl_->mux);
  impl_->disconnectRequested = true;
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
bool KitsuBleGattLink::setApplicationAuthenticated(bool) { return false; }
bool KitsuBleGattLink::queueFrame(const uint8_t*, size_t) { return false; }
void KitsuBleGattLink::disconnect() {}
BleLinkStatus KitsuBleGattLink::status(uint32_t) const {
  return BleLinkStatus{};
}

}  // namespace connectivity
}  // namespace kitsu868

#endif
