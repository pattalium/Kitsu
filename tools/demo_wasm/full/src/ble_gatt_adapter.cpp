#include "Arduino.h"
#include "kitsu_ble_gatt.h"

#include <new>

namespace kitsu868 {
namespace connectivity {
namespace {

// NimBLE exposes attributes of at most 512 bytes. The browser writes and
// consumes these buffers as individual GATT chunks; all length framing,
// bounds, timeouts, request serialization, and link gating remain here.
constexpr size_t kGattAttributeMaximumBytes = 512U;
constexpr uint16_t kHostMaximumMtu = 517U;
constexpr uint16_t kHostConnectionHandle = 1U;
constexpr size_t kEventQueueCapacity = 8U;
constexpr uint16_t kNoConnection = 0xffffU;

struct EventRecord {
  BleLinkEvent event = BleLinkEvent::Connected;
};

struct HostBleState {
  BleFrameDelegate* delegate = nullptr;
  companion::LengthFrameParser parser{};
  uint8_t rxStorage[companion::kMaximumFrameBytes]{};
  uint8_t txStorage[companion::kMaximumFrameBytes +
                    companion::kFrameHeaderBytes]{};
  uint8_t hostRxChunk[kGattAttributeMaximumBytes]{};
  uint8_t hostTxChunk[kGattAttributeMaximumBytes]{};
  size_t hostTxChunkBytes = 0U;
  bool hostTxChunkCompletesRequest = false;
  uint32_t hostTxChunkRevision = 0U;
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
  uint16_t connectionHandle = kNoConnection;
  uint16_t mtu = 23U;
  uint32_t numericComparison = 0U;
  uint32_t numericDeadline = 0U;
  uint32_t pairingWindowDeadline = 0U;
};

struct HostBleStatusViewV1 {
  uint32_t abiVersion;
  uint32_t bytes;
  uint32_t begun;
  uint32_t advertising;
  uint32_t connected;
  uint32_t secureConnections;
  uint32_t encrypted;
  uint32_t authenticated;
  uint32_t bonded;
  uint32_t notifySubscribed;
  uint32_t pairingWindowOpen;
  uint32_t numericComparisonPending;
  uint32_t applicationAuthenticated;
  uint32_t requestInFlight;
  uint32_t connectionHandle;
  uint32_t mtu;
  uint32_t numericComparison;
  uint32_t pairingWindowRemainingMs;
  uint32_t rxFrameReady;
  uint32_t txQueued;
  uint32_t txChunkBytes;
  uint32_t txChunkRevision;
};

HostBleState host{};
HostBleStatusViewV1 hostStatus{};

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

void pushEvent(BleLinkEvent event) {
  const uint8_t next =
      static_cast<uint8_t>((host.eventWrite + 1U) % kEventQueueCapacity);
  if (next == host.eventRead) {
    host.eventRead =
        static_cast<uint8_t>((host.eventRead + 1U) % kEventQueueCapacity);
  }
  host.events[host.eventWrite].event = event;
  host.eventWrite = next;
}

bool beginParser(size_t maximumFrameBytes) {
  return host.parser.begin(host.rxStorage, sizeof(host.rxStorage),
                           maximumFrameBytes,
                           companion::kFrameAssemblyTimeoutMs);
}

void clearConnection() {
  host.connected = false;
  host.secureConnections = false;
  host.encrypted = false;
  host.authenticated = false;
  host.bonded = false;
  host.notifySubscribed = false;
  host.applicationAuthenticated = false;
  host.pendingApplicationAuthentication = false;
  host.applicationAuthenticationPending = false;
  host.deliveringFrame = false;
  host.frameReady = false;
  host.numericPending = false;
  host.numericComparison = 0U;
  host.numericDeadline = 0U;
  host.connectionHandle = kNoConnection;
  host.mtu = 23U;
  host.requestInFlight = false;
  host.txQueued = false;
  host.txBytes = 0U;
  host.txOffset = 0U;
  host.hostTxChunkBytes = 0U;
  host.hostTxChunkCompletesRequest = false;
  beginParser(companion::kMaximumHandshakeFrameBytes);
}

void requestDisconnect(BleLinkEvent event) {
  pushEvent(event);
  host.disconnectRequested = true;
}

void rejectNumeric(BleLinkEvent event) {
  if (!host.numericPending) return;
  host.numericPending = false;
  host.numericComparison = 0U;
  host.numericDeadline = 0U;
  requestDisconnect(event);
}

BleLinkStatus currentStatus(uint32_t nowMillis) {
  BleLinkStatus output{};
  output.begun = host.begun;
  output.advertising = host.advertising;
  output.connected = host.connected;
  output.secureConnections = host.secureConnections;
  output.encrypted = host.encrypted;
  output.authenticated = host.authenticated;
  output.bonded = host.bonded;
  output.notifySubscribed = host.notifySubscribed;
  output.pairingWindowOpen = host.pairingWindowOpen;
  output.numericComparisonPending = host.numericPending;
  output.applicationAuthenticated = host.applicationAuthenticated;
  output.requestInFlight = host.requestInFlight || host.txQueued;
  output.connectionHandle = host.connectionHandle;
  output.mtu = host.mtu;
  output.numericComparison = host.numericComparison;
  if (host.pairingWindowOpen &&
      !deadlineReached(nowMillis, host.pairingWindowDeadline)) {
    output.pairingWindowRemainingMs =
        host.pairingWindowDeadline - nowMillis;
  }
  return output;
}

bool hostConnect(uint16_t mtu) {
  if (!host.begun || !host.advertising || host.connected ||
      host.localControllerRecoveryLocked || mtu < 23U ||
      mtu > kHostMaximumMtu) {
    return false;
  }
  clearConnection();
  host.connected = true;
  host.advertising = false;
  host.connectionHandle = kHostConnectionHandle;
  host.mtu = mtu;
  pushEvent(BleLinkEvent::Connected);
  return true;
}

bool hostAuthenticateReturningController() {
  if (!host.connected || host.numericPending) return false;
  host.secureConnections = true;
  host.encrypted = true;
  host.authenticated = true;
  host.bonded = true;
  pushEvent(BleLinkEvent::LinkAuthenticated);
  return true;
}

}  // namespace

struct KitsuBleGattLink::Impl {};

KitsuBleGattLink::KitsuBleGattLink() : impl_(nullptr) {}

KitsuBleGattLink::~KitsuBleGattLink() { end(); }

bool KitsuBleGattLink::begin(const char* deviceName,
                             BleFrameDelegate& delegate) {
  if (!deviceName || deviceName[0] == '\0' || impl_ || host.begun) {
    return false;
  }
  Impl* implementation = new (std::nothrow) Impl();
  if (!implementation) return false;
  impl_ = implementation;
  host.delegate = &delegate;
  host.begun = true;
  host.advertising = true;
  if (!beginParser(companion::kMaximumHandshakeFrameBytes)) {
    end();
    return false;
  }
  return true;
}

void KitsuBleGattLink::end() {
  if (!impl_) return;
  clearConnection();
  host.delegate = nullptr;
  host.begun = false;
  host.advertising = false;
  host.pairingWindowOpen = false;
  host.localControllerRecoveryLocked = false;
  host.pairingWindowDeadline = 0U;
  host.disconnectRequested = false;
  host.eventRead = 0U;
  host.eventWrite = 0U;
  delete impl_;
  impl_ = nullptr;
}

void KitsuBleGattLink::loop(uint32_t nowMillis) {
  if (!impl_ || !host.begun) return;

  if (host.pairingWindowOpen &&
      deadlineReached(nowMillis, host.pairingWindowDeadline)) {
    host.pairingWindowOpen = false;
    host.pairingWindowDeadline = 0U;
    rejectNumeric(BleLinkEvent::LinkRejected);
  }
  if (host.numericPending &&
      deadlineReached(nowMillis, host.numericDeadline)) {
    rejectNumeric(BleLinkEvent::FrameTimedOut);
  }

  if (host.disconnectRequested) {
    const bool wasConnected = host.connected;
    host.disconnectRequested = false;
    clearConnection();
    host.advertising = !host.localControllerRecoveryLocked;
    if (wasConnected) pushEvent(BleLinkEvent::Disconnected);
  }

  for (;;) {
    if (host.eventRead == host.eventWrite) break;
    const BleLinkEvent event = host.events[host.eventRead].event;
    host.eventRead =
        static_cast<uint8_t>((host.eventRead + 1U) % kEventQueueCapacity);
    if (host.delegate) {
      host.delegate->onBleLinkEvent(event, currentStatus(nowMillis));
    }
  }

  if (!host.frameReady && host.connected) {
    const companion::FrameResult polled = host.parser.poll(nowMillis);
    if (polled == companion::FrameResult::TimedOut) {
      requestDisconnect(BleLinkEvent::FrameTimedOut);
    }
  }

  const uint8_t* frame = nullptr;
  size_t frameBytes = 0U;
  if (host.frameReady && !host.requestInFlight &&
      host.parser.frame(frame, frameBytes)) {
    host.requestInFlight = true;
    host.deliveringFrame = true;
    if (host.delegate) host.delegate->onBleFrame(frame, frameBytes);
    host.parser.consume();
    host.frameReady = false;
    host.deliveringFrame = false;
    if (host.applicationAuthenticationPending) {
      host.applicationAuthenticated =
          host.pendingApplicationAuthentication;
      beginParser(host.applicationAuthenticated
                      ? companion::kMaximumFrameBytes
                      : companion::kMaximumHandshakeFrameBytes);
      host.applicationAuthenticationPending = false;
    }
  }

  // One notification-sized chunk per firmware iteration, exactly like the
  // NimBLE adapter. The browser acknowledges the staged notification before
  // another can be staged.
  if (host.hostTxChunkBytes == 0U && host.txQueued && host.connected &&
      host.encrypted && host.authenticated && host.bonded &&
      host.notifySubscribed) {
    const size_t mtuPayload =
        host.mtu > 3U ? static_cast<size_t>(host.mtu - 3U) : 20U;
    const size_t maximumChunk =
        mtuPayload < kGattAttributeMaximumBytes
            ? mtuPayload
            : kGattAttributeMaximumBytes;
    const size_t remaining = host.txBytes - host.txOffset;
    const size_t chunkBytes =
        remaining < maximumChunk ? remaining : maximumChunk;
    std::memcpy(host.hostTxChunk, host.txStorage + host.txOffset,
                chunkBytes);
    host.hostTxChunkBytes = chunkBytes;
    host.hostTxChunkCompletesRequest =
        host.requestInFlight && host.txOffset + chunkBytes == host.txBytes;
    ++host.hostTxChunkRevision;
    if (host.hostTxChunkRevision == 0U) ++host.hostTxChunkRevision;
    host.txOffset += chunkBytes;
    if (host.txOffset == host.txBytes) {
      host.txQueued = false;
      host.txBytes = 0U;
      host.txOffset = 0U;
    }
  }
}

bool KitsuBleGattLink::openPairingWindow(uint32_t nowMillis,
                                         uint32_t durationMs) {
  if (!impl_ || !host.begun || host.localControllerRecoveryLocked ||
      durationMs == 0U || durationMs > kBlePairingWindowMaximumMs) {
    return false;
  }
  host.pairingWindowOpen = true;
  host.pairingWindowDeadline = nowMillis + durationMs;
  return true;
}

void KitsuBleGattLink::closePairingWindow() {
  if (!impl_) return;
  host.pairingWindowOpen = false;
  host.pairingWindowDeadline = 0U;
  rejectNumeric(BleLinkEvent::LinkRejected);
}

bool KitsuBleGattLink::confirmNumericComparison(bool accept) {
  if (!impl_ || !host.numericPending || !host.connected) return false;
  host.numericPending = false;
  host.numericComparison = 0U;
  host.numericDeadline = 0U;
  if (!accept) {
    requestDisconnect(BleLinkEvent::LinkRejected);
    return true;
  }
  host.secureConnections = true;
  host.encrypted = true;
  host.authenticated = true;
  host.bonded = true;
  host.pairingWindowOpen = false;
  host.pairingWindowDeadline = 0U;
  pushEvent(BleLinkEvent::LinkAuthenticated);
  return true;
}

bool KitsuBleGattLink::setLocalControllerRecoveryLocked(bool locked) {
  if (!impl_ || !host.begun) return false;
  host.localControllerRecoveryLocked = locked;
  if (locked) {
    host.pairingWindowOpen = false;
    host.pairingWindowDeadline = 0U;
    rejectNumeric(BleLinkEvent::LinkRejected);
    if (host.connected) host.disconnectRequested = true;
    host.advertising = false;
  } else if (!host.connected) {
    host.advertising = true;
  }
  return true;
}

bool KitsuBleGattLink::setApplicationAuthenticated(bool authenticated) {
  if (!impl_ || !host.begun) return false;
  if (host.deliveringFrame) {
    host.pendingApplicationAuthentication = authenticated;
    host.applicationAuthenticationPending = true;
    return true;
  }
  host.applicationAuthenticated = authenticated;
  host.requestInFlight = false;
  host.frameReady = false;
  return beginParser(authenticated ? companion::kMaximumFrameBytes
                                   : companion::kMaximumHandshakeFrameBytes);
}

bool KitsuBleGattLink::queueFrame(const uint8_t* json, size_t jsonBytes) {
  if (!impl_ || !host.begun || !json || jsonBytes == 0U) return false;
  const size_t maximum = host.applicationAuthenticated
                             ? companion::kMaximumFrameBytes
                             : companion::kMaximumHandshakeFrameBytes;
  if (host.txQueued || jsonBytes > maximum || !host.connected ||
      !host.encrypted || !host.authenticated || !host.bonded) {
    return false;
  }
  size_t framedBytes = 0U;
  const companion::FrameResult encoded = companion::encodeLengthFrame(
      json, jsonBytes, host.txStorage, sizeof(host.txStorage), framedBytes);
  if (encoded != companion::FrameResult::Ready) return false;
  host.txBytes = framedBytes;
  host.txOffset = 0U;
  host.txQueued = true;
  return true;
}

void KitsuBleGattLink::disconnect() {
  if (impl_) host.disconnectRequested = true;
}

BleLinkStatus KitsuBleGattLink::status(uint32_t nowMillis) const {
  return impl_ ? currentStatus(nowMillis) : BleLinkStatus{};
}

extern "C" uint8_t* kitsu_emulator_ble_rx_chunk_buffer() {
  return host.hostRxChunk;
}

extern "C" uint32_t kitsu_emulator_ble_rx_chunk_capacity() {
  return sizeof(host.hostRxChunk);
}

extern "C" uint32_t kitsu_emulator_ble_rx_chunk_commit(uint32_t bytes) {
  if (!host.delegate || !host.connected || !host.encrypted ||
      !host.authenticated || !host.bonded || host.requestInFlight ||
      host.frameReady || bytes == 0U || bytes > sizeof(host.hostRxChunk)) {
    if (host.connected) requestDisconnect(BleLinkEvent::ProtocolViolation);
    return 0U;
  }
  const companion::FrameResult result =
      host.parser.feed(host.hostRxChunk, bytes, kitsu_hal_millis());
  if (result == companion::FrameResult::Ready) {
    host.frameReady = true;
    return 1U;
  }
  if (result == companion::FrameResult::NeedMore) return 1U;
  requestDisconnect(result == companion::FrameResult::TimedOut
                        ? BleLinkEvent::FrameTimedOut
                        : BleLinkEvent::ProtocolViolation);
  return 0U;
}

extern "C" uint8_t* kitsu_emulator_ble_tx_chunk_buffer() {
  return host.hostTxChunk;
}

extern "C" uint32_t kitsu_emulator_ble_tx_chunk_bytes() {
  return static_cast<uint32_t>(host.hostTxChunkBytes);
}

extern "C" uint32_t kitsu_emulator_ble_tx_chunk_revision() {
  return host.hostTxChunkRevision;
}

extern "C" uint32_t kitsu_emulator_ble_tx_chunk_consume() {
  if (host.hostTxChunkBytes == 0U) return 0U;
  const bool completedRequest = host.hostTxChunkCompletesRequest;
  host.hostTxChunkBytes = 0U;
  host.hostTxChunkCompletesRequest = false;
  if (completedRequest) host.requestInFlight = false;
  return 1U;
}

extern "C" uint32_t kitsu_emulator_ble_connect(uint32_t mtu) {
  if (mtu > UINT16_MAX) return 0U;
  return hostConnect(static_cast<uint16_t>(mtu)) ? 1U : 0U;
}

extern "C" uint32_t kitsu_emulator_ble_authenticate_returning() {
  return hostAuthenticateReturningController() ? 1U : 0U;
}

extern "C" uint32_t kitsu_emulator_ble_connect_authenticated() {
  return hostConnect(kHostMaximumMtu) && hostAuthenticateReturningController()
             ? 1U
             : 0U;
}

extern "C" uint32_t kitsu_emulator_ble_connect_pairing(uint32_t passkey) {
  if (!host.pairingWindowOpen || passkey > 999999U ||
      !hostConnect(kHostMaximumMtu)) {
    return 0U;
  }
  host.numericPending = true;
  host.numericComparison = passkey;
  host.numericDeadline = kitsu_hal_millis() +
                         kBleNumericComparisonTimeoutMs;
  pushEvent(BleLinkEvent::NumericComparison);
  return 1U;
}

extern "C" uint32_t kitsu_emulator_ble_set_mtu(uint32_t mtu) {
  if (!host.connected || mtu < 23U || mtu > kHostMaximumMtu) return 0U;
  host.mtu = static_cast<uint16_t>(mtu);
  return 1U;
}

extern "C" uint32_t kitsu_emulator_ble_set_notify_subscription(
    uint32_t subscribed) {
  if (!host.connected) return 0U;
  host.notifySubscribed = subscribed != 0U;
  return 1U;
}

extern "C" void kitsu_emulator_ble_disconnect() {
  if (host.connected) host.disconnectRequested = true;
}

extern "C" uint32_t* kitsu_emulator_ble_status_view() {
  const BleLinkStatus status = currentStatus(kitsu_hal_millis());
  hostStatus.abiVersion = 1U;
  hostStatus.bytes = sizeof(hostStatus);
  hostStatus.begun = status.begun ? 1U : 0U;
  hostStatus.advertising = status.advertising ? 1U : 0U;
  hostStatus.connected = status.connected ? 1U : 0U;
  hostStatus.secureConnections = status.secureConnections ? 1U : 0U;
  hostStatus.encrypted = status.encrypted ? 1U : 0U;
  hostStatus.authenticated = status.authenticated ? 1U : 0U;
  hostStatus.bonded = status.bonded ? 1U : 0U;
  hostStatus.notifySubscribed = status.notifySubscribed ? 1U : 0U;
  hostStatus.pairingWindowOpen = status.pairingWindowOpen ? 1U : 0U;
  hostStatus.numericComparisonPending =
      status.numericComparisonPending ? 1U : 0U;
  hostStatus.applicationAuthenticated =
      status.applicationAuthenticated ? 1U : 0U;
  hostStatus.requestInFlight = status.requestInFlight ? 1U : 0U;
  hostStatus.connectionHandle = status.connectionHandle;
  hostStatus.mtu = status.mtu;
  hostStatus.numericComparison = status.numericComparison;
  hostStatus.pairingWindowRemainingMs = status.pairingWindowRemainingMs;
  hostStatus.rxFrameReady = host.frameReady ? 1U : 0U;
  hostStatus.txQueued = host.txQueued ? 1U : 0U;
  hostStatus.txChunkBytes = static_cast<uint32_t>(host.hostTxChunkBytes);
  hostStatus.txChunkRevision = host.hostTxChunkRevision;
  return reinterpret_cast<uint32_t*>(&hostStatus);
}

extern "C" uint32_t kitsu_emulator_ble_status_view_bytes() {
  return sizeof(hostStatus);
}

}  // namespace connectivity
}  // namespace kitsu868
