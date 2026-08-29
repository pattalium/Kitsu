#include "../src/kitsu_ble_gatt.h"

#include <assert.h>

#include <utility>
#include <vector>

#include "Arduino.h"
#include "NimBLEDevice.h"

using kitsu868::connectivity::BleFrameDelegate;
using kitsu868::connectivity::BleLinkEvent;
using kitsu868::connectivity::BleLinkStatus;
using kitsu868::connectivity::KitsuBleGattLink;
using kitsu868::connectivity::kBleNumericComparisonTimeoutMs;
using kitsu868::connectivity::kBleNotifyCompletionTimeoutMs;

namespace fake_nimble {
inline uint8_t clientConfigurationPermissions = 0U;
}

extern "C" void ble_gatts_set_clt_cfg_perm_flags(uint8_t flags) {
  fake_nimble::clientConfigurationPermissions = flags;
}

namespace {

constexpr uint16_t kFirstHandle = 7U;
constexpr uint16_t kSecondHandle = 11U;
constexpr uint16_t kThirdHandle = 13U;

class TestDelegate final : public BleFrameDelegate {
 public:
  void onBleFrame(const uint8_t*, size_t) override { ++frameCount; }
  void onBleLinkEvent(BleLinkEvent event,
                      const BleLinkStatus& status) override {
    events.emplace_back(event, status);
  }

  size_t frameCount = 0U;
  std::vector<std::pair<BleLinkEvent, BleLinkStatus>> events{};
};

struct Fixture {
  Fixture() {
    NimBLEDevice::testReset();
    setFakeMillis(0U);
    assert(link.begin("Kitsu TEST", delegate));
    server = NimBLEDevice::testServer();
    assert(server != nullptr);
    rx = server->testCharacteristicAt(0U);
    tx = server->testCharacteristicAt(1U);
    assert(rx != nullptr && tx != nullptr);
  }

  ~Fixture() { link.end(); }

  void connect(uint16_t handle, uint32_t nowMillis) {
    setFakeMillis(nowMillis);
    server->simulateConnect(NimBLEConnInfo(handle, 247U));
  }

  void secureAndSubscribe(uint16_t handle, uint32_t nowMillis,
                          uint16_t mtu = 247U) {
    setFakeMillis(nowMillis);
    server->simulateConnect(NimBLEConnInfo(handle, mtu));
    server->simulateAuthenticationComplete(
        NimBLEConnInfo(handle, mtu, true, true, true, 16U));
    tx->simulateSubscribe(handle, 1U);
    link.loop(nowMillis);
  }

  TestDelegate delegate{};
  KitsuBleGattLink link{};
  NimBLEServer* server = nullptr;
  NimBLECharacteristic* rx = nullptr;
  NimBLECharacteristic* tx = nullptr;
};

bool hasEvent(const TestDelegate& delegate, BleLinkEvent expected) {
  for (const auto& record : delegate.events) {
    if (record.first == expected) return true;
  }
  return false;
}

void testCorrectPasskeyIsSurfacedAndAccepted() {
  Fixture fixture;
  assert(fixture.link.openPairingWindow(100U, 1000U));
  fixture.connect(kFirstHandle, 110U);
  assert(NimBLEDevice::securityRequests.back() == kFirstHandle);

  setFakeMillis(120U);
  fixture.server->simulateConfirmPasskey(kFirstHandle, 123456U);
  BleLinkStatus status = fixture.link.status(120U);
  assert(status.connected);
  assert(status.connectionHandle == kFirstHandle);
  assert(status.pairingWindowOpen);
  assert(status.pairingWindowRemainingMs == 980U);
  assert(status.numericComparisonPending);
  assert(status.numericComparison == 123456U);

  fixture.link.loop(120U);
  assert(fixture.delegate.events.size() == 2U);
  assert(fixture.delegate.events[0].first == BleLinkEvent::Connected);
  assert(fixture.delegate.events[1].first == BleLinkEvent::NumericComparison);
  assert(fixture.delegate.events[1].second.numericComparisonPending);
  assert(fixture.delegate.events[1].second.numericComparison == 123456U);

  assert(fixture.link.confirmNumericComparison(true));
  assert(NimBLEDevice::numericConfirmations.size() == 1U);
  assert(NimBLEDevice::numericConfirmations.back().handle == kFirstHandle);
  assert(NimBLEDevice::numericConfirmations.back().accepted);
  status = fixture.link.status(121U);
  assert(!status.numericComparisonPending);
  assert(status.numericComparison == 0U);
  assert(status.pairingWindowOpen);
  assert(!fixture.link.confirmNumericComparison(true));
}

void testWrongHandleClosedWindowAndDuplicatePromptAreRejected() {
  Fixture fixture;
  assert(fixture.link.openPairingWindow(1000U, 1000U));
  fixture.connect(kFirstHandle, 1010U);

  setFakeMillis(1020U);
  fixture.server->simulateConfirmPasskey(kSecondHandle, 111111U);
  assert(NimBLEDevice::numericConfirmations.size() == 1U);
  assert(NimBLEDevice::numericConfirmations.back().handle == kSecondHandle);
  assert(!NimBLEDevice::numericConfirmations.back().accepted);
  assert(!fixture.link.status(1020U).numericComparisonPending);

  fixture.server->simulateConfirmPasskey(kFirstHandle, 222222U);
  assert(fixture.link.status(1020U).numericComparisonPending);
  fixture.server->simulateConfirmPasskey(kFirstHandle, 333333U);
  assert(NimBLEDevice::numericConfirmations.size() == 2U);
  assert(NimBLEDevice::numericConfirmations.back().handle == kFirstHandle);
  assert(!NimBLEDevice::numericConfirmations.back().accepted);
  assert(fixture.link.status(1020U).numericComparison == 222222U);
  assert(fixture.link.confirmNumericComparison(false));

  fixture.link.closePairingWindow();
  setFakeMillis(1030U);
  fixture.server->simulateConfirmPasskey(kFirstHandle, 444444U);
  assert(NimBLEDevice::numericConfirmations.size() == 4U);
  assert(!NimBLEDevice::numericConfirmations.back().accepted);
  assert(!fixture.link.status(1030U).numericComparisonPending);

  assert(fixture.link.openPairingWindow(2000U, 100U));
  setFakeMillis(2100U);
  fixture.server->simulateConfirmPasskey(kFirstHandle, 555555U);
  assert(NimBLEDevice::numericConfirmations.size() == 5U);
  assert(!NimBLEDevice::numericConfirmations.back().accepted);
  assert(!fixture.link.status(2100U).numericComparisonPending);
}

void testExplicitCancelWindowExpiryAndNumericTimeoutReject() {
  {
    Fixture fixture;
    assert(fixture.link.openPairingWindow(3000U, 1000U));
    fixture.connect(kFirstHandle, 3010U);
    setFakeMillis(3020U);
    fixture.server->simulateConfirmPasskey(kFirstHandle, 101010U);
    fixture.link.closePairingWindow();
    assert(NimBLEDevice::numericConfirmations.size() == 1U);
    assert(!NimBLEDevice::numericConfirmations.back().accepted);
    const BleLinkStatus status = fixture.link.status(3020U);
    assert(!status.pairingWindowOpen);
    assert(!status.numericComparisonPending);
  }

  {
    Fixture fixture;
    assert(fixture.link.openPairingWindow(4000U, 100U));
    fixture.connect(kFirstHandle, 4010U);
    setFakeMillis(4020U);
    fixture.server->simulateConfirmPasskey(kFirstHandle, 202020U);
    fixture.link.loop(4100U);
    assert(NimBLEDevice::numericConfirmations.size() == 1U);
    assert(!NimBLEDevice::numericConfirmations.back().accepted);
    const BleLinkStatus status = fixture.link.status(4100U);
    assert(!status.pairingWindowOpen);
    assert(!status.numericComparisonPending);
  }

  {
    Fixture fixture;
    assert(fixture.link.openPairingWindow(5000U, 60000U));
    fixture.connect(kFirstHandle, 5010U);
    setFakeMillis(5020U);
    fixture.server->simulateConfirmPasskey(kFirstHandle, 303030U);
    fixture.link.loop(5020U + kBleNumericComparisonTimeoutMs);
    assert(NimBLEDevice::numericConfirmations.size() == 1U);
    assert(!NimBLEDevice::numericConfirmations.back().accepted);
    assert(!fixture.link.status(5020U + kBleNumericComparisonTimeoutMs)
                .numericComparisonPending);
    assert(fixture.delegate.events.back().first == BleLinkEvent::FrameTimedOut);
  }
}

void testWindowAndSingleConnectionSurviveDisconnectReconnect() {
  Fixture fixture;
  assert(fixture.link.openPairingWindow(6000U, 1000U));
  fixture.connect(kFirstHandle, 6010U);
  fixture.link.loop(6010U);

  setFakeMillis(6020U);
  fixture.server->simulateConfirmPasskey(kFirstHandle, 404040U);
  assert(fixture.link.status(6020U).numericComparisonPending);
  fixture.server->simulateDisconnect(kFirstHandle);
  BleLinkStatus status = fixture.link.status(6020U);
  assert(!status.connected);
  assert(status.advertising);
  assert(status.connectionHandle == BLE_HS_CONN_HANDLE_NONE);
  assert(!status.numericComparisonPending);
  assert(status.pairingWindowOpen);
  assert(status.pairingWindowRemainingMs == 980U);

  fixture.connect(kSecondHandle, 6200U);
  fixture.server->simulateAuthenticationComplete(
      NimBLEConnInfo(kSecondHandle, 247U, true, true, true, 16U));
  fixture.link.loop(6200U);
  status = fixture.link.status(6200U);
  assert(status.connected);
  assert(status.connectionHandle == kSecondHandle);
  assert(status.secureConnections);
  assert(status.encrypted);
  assert(status.authenticated);
  assert(status.bonded);
  assert(status.pairingWindowOpen);
  assert(status.pairingWindowRemainingMs == 800U);
  assert(fixture.delegate.events.back().first ==
         BleLinkEvent::LinkAuthenticated);

  // A late callback from the dead link must not disturb the replacement.
  fixture.server->simulateDisconnect(kFirstHandle);
  status = fixture.link.status(6201U);
  assert(status.connected);
  assert(status.connectionHandle == kSecondHandle);
  setFakeMillis(6202U);
  fixture.server->simulateConfirmPasskey(kFirstHandle, 505050U);
  assert(!NimBLEDevice::numericConfirmations.back().accepted);
  assert(!fixture.link.status(6202U).numericComparisonPending);

  // The adapter accepts exactly one live connection. A concurrent connection
  // is rejected without replacing the active handle (Android reports the
  // equivalent race as a first-link GATT close, commonly status 22).
  fixture.connect(kThirdHandle, 6203U);
  assert(fixture.server->disconnectCalls.back() == kThirdHandle);
  status = fixture.link.status(6203U);
  assert(status.connected);
  assert(status.connectionHandle == kSecondHandle);

  setFakeMillis(6204U);
  fixture.server->simulateConfirmPasskey(kSecondHandle, 606060U);
  assert(fixture.link.status(6204U).numericComparisonPending);
  assert(fixture.link.confirmNumericComparison(true));
  assert(NimBLEDevice::numericConfirmations.back().handle == kSecondHandle);
  assert(NimBLEDevice::numericConfirmations.back().accepted);
}

void testNotifySuccessUsesInternalNonzeroHandleToken() {
  Fixture fixture;
  fixture.secureAndSubscribe(kSecondHandle, 10U);
  fixture.tx->statusOnNotify = true;
  fixture.tx->notifyStatusCode = 0;
  static const uint8_t response[] = {'{', '"', 'x', '"', ':', '1', '}'};
  assert(fixture.link.queueFrame(response, sizeof(response)));
  fixture.link.loop(11U);
  assert(fixture.tx->notifiedHandles.size() == 1U);
  assert(fixture.tx->notifiedHandles[0] == kSecondHandle);
  const BleLinkStatus status = fixture.link.status(11U);
  assert(!status.requestInFlight);
  assert(status.notifyStatusAvailable && status.lastNotifyStatus == 0);
  assert(!hasEvent(fixture.delegate, BleLinkEvent::ProtocolViolation));
}

void testNotifyFalseWithoutCallbackRetriesWithoutAdvancing() {
  Fixture fixture;
  fixture.secureAndSubscribe(kFirstHandle, 20U);
  fixture.tx->notifyResult = false;
  static const uint8_t response[] = {'{', '}'};
  assert(fixture.link.queueFrame(response, sizeof(response)));
  fixture.link.loop(21U);
  assert(fixture.tx->notifiedHandles.size() == 1U);
  assert(fixture.link.status(21U).requestInFlight);
  fixture.link.loop(28U);
  assert(fixture.tx->notifiedHandles.size() == 1U);

  fixture.tx->notifyResult = true;
  fixture.tx->statusOnNotify = true;
  fixture.tx->notifyStatusCode = 0;
  fixture.link.loop(29U);
  assert(fixture.tx->notifiedHandles.size() == 2U);
  assert(fixture.tx->notifiedValues[0] == fixture.tx->notifiedValues[1]);
  assert(!fixture.link.status(29U).requestInFlight);
  assert(fixture.server->disconnectCalls.empty());
}

void testRetryableNotifyStatusIsBoundedAndNeverProtocolViolation() {
  Fixture fixture;
  fixture.secureAndSubscribe(kFirstHandle, 100U);
  fixture.tx->statusOnNotify = true;
  fixture.tx->notifyStatusCode = 15;  // BLE_HS_EBUSY
  static const uint8_t response[] = {'{', '}'};
  assert(fixture.link.queueFrame(response, sizeof(response)));
  fixture.link.loop(101U);
  fixture.link.loop(109U);
  fixture.link.loop(125U);
  fixture.link.loop(149U);
  assert(fixture.tx->notifiedHandles.size() == 4U);
  fixture.link.loop(150U);
  assert(fixture.server->disconnectCalls.size() == 1U);
  assert(fixture.server->disconnectCalls.back() == kFirstHandle);
  assert(hasEvent(fixture.delegate, BleLinkEvent::TransportFailure));
  assert(!hasEvent(fixture.delegate, BleLinkEvent::ProtocolViolation));
}

void testTerminalNotifyStatusAndMissingCallbackTimeout() {
  {
    Fixture fixture;
    fixture.secureAndSubscribe(kThirdHandle, 200U);
    fixture.tx->statusOnNotify = true;
    fixture.tx->notifyStatusCode = 22;  // BLE_HS_ENOTSYNCED
    static const uint8_t response[] = {'{', '}'};
    assert(fixture.link.queueFrame(response, sizeof(response)));
    fixture.link.loop(201U);
    fixture.link.loop(202U);
    assert(fixture.server->disconnectCalls.back() == kThirdHandle);
    assert(hasEvent(fixture.delegate, BleLinkEvent::TransportFailure));
    assert(!hasEvent(fixture.delegate, BleLinkEvent::ProtocolViolation));
  }

  {
    Fixture fixture;
    fixture.secureAndSubscribe(kFirstHandle, 300U);
    fixture.tx->statusOnNotify = false;
    fixture.tx->notifyResult = true;
    static const uint8_t response[] = {'{', '}'};
    assert(fixture.link.queueFrame(response, sizeof(response)));
    const uint32_t first = 301U;
    fixture.link.loop(first);
    assert(fixture.tx->notifiedHandles.size() == 1U);
    fixture.link.loop(first + kBleNotifyCompletionTimeoutMs);
    fixture.link.loop(first + kBleNotifyCompletionTimeoutMs + 8U);
    assert(fixture.tx->notifiedHandles.size() == 2U);
    fixture.link.loop(first + 2U * kBleNotifyCompletionTimeoutMs + 8U);
    fixture.link.loop(first + 2U * kBleNotifyCompletionTimeoutMs + 24U);
    assert(fixture.tx->notifiedHandles.size() == 3U);
    fixture.link.loop(first + 3U * kBleNotifyCompletionTimeoutMs + 24U);
    fixture.link.loop(first + 3U * kBleNotifyCompletionTimeoutMs + 48U);
    assert(fixture.tx->notifiedHandles.size() == 4U);
    fixture.link.loop(first + 4U * kBleNotifyCompletionTimeoutMs + 48U);
    fixture.link.loop(first + 4U * kBleNotifyCompletionTimeoutMs + 49U);
    assert(fixture.tx->notifiedHandles.size() == 4U);
    assert(fixture.server->disconnectCalls.size() == 1U);
    assert(hasEvent(fixture.delegate, BleLinkEvent::TransportFailure));
  }
}

void testGenerationClearsStaleDisconnectAndQueuedFailure() {
  Fixture fixture;
  fixture.secureAndSubscribe(kFirstHandle, 1000U);
  fixture.tx->statusOnNotify = true;
  fixture.tx->notifyStatusCode = 22;
  static const uint8_t response[] = {'{', '}'};
  assert(fixture.link.queueFrame(response, sizeof(response)));
  fixture.link.loop(1001U);  // queues transport failure for generation A
  const uint32_t generationA = fixture.link.status(1001U).connectionGeneration;

  setFakeMillis(1002U);
  fixture.server->simulateDisconnect(kFirstHandle, 22);
  fixture.connect(kFirstHandle, 1003U);  // same numeric handle, generation B
  fixture.server->simulateAuthenticationComplete(
      NimBLEConnInfo(kFirstHandle, 23U, true, true, true, 16U));
  fixture.tx->simulateSubscribe(kFirstHandle, 1U);
  fixture.tx->simulateStatus(0);  // delayed A callback: no active token
  fixture.link.loop(1003U);

  const BleLinkStatus current = fixture.link.status(1003U);
  assert(current.connected && current.connectionHandle == kFirstHandle);
  assert(current.connectionGeneration != generationA);
  assert(fixture.server->disconnectCalls.empty());
  assert(!hasEvent(fixture.delegate, BleLinkEvent::TransportFailure));
  bool sawStaleDisconnectDiagnostic = false;
  for (const auto& event : fixture.delegate.events) {
    if (event.first == BleLinkEvent::Disconnected) {
      sawStaleDisconnectDiagnostic = true;
      assert(!event.second.eventMatchesCurrentConnection);
      assert(event.second.lastDisconnectReason == 22);
      assert(event.second.lastDisconnectedGeneration == generationA);
    }
  }
  assert(sawStaleDisconnectDiagnostic);

  fixture.tx->notifyStatusCode = 0;
  assert(fixture.link.queueFrame(response, sizeof(response)));
  fixture.link.loop(1004U);
  assert(!fixture.link.status(1004U).requestInFlight);
}

void testClosedGenerationDropsQueuedConnectAndAuthenticationEvents() {
  Fixture fixture;
  fixture.connect(kFirstHandle, 1200U);
  fixture.server->simulateAuthenticationComplete(
      NimBLEConnInfo(kFirstHandle, 247U, true, true, true, 16U));
  fixture.tx->simulateSubscribe(kFirstHandle, 1U);
  fixture.server->simulateDisconnect(kFirstHandle, 19);

  fixture.link.loop(1201U);
  assert(fixture.delegate.events.size() == 1U);
  assert(fixture.delegate.events[0].first == BleLinkEvent::Disconnected);
  assert(!fixture.delegate.events[0].second.eventMatchesCurrentConnection);
  assert(fixture.delegate.events[0].second.lastDisconnectReason == 19);
}

void testWriteFromDepartedHandleCannotCloseReplacementLink() {
  Fixture fixture;
  fixture.secureAndSubscribe(kFirstHandle, 1500U);
  fixture.server->simulateDisconnect(kFirstHandle, 19);
  fixture.secureAndSubscribe(kSecondHandle, 1501U);
  fixture.delegate.events.clear();
  fixture.server->disconnectCalls.clear();

  static const uint8_t staleFrame[] = {0U, 0U, 0U, 2U, '{', '}'};
  fixture.rx->simulateWrite(kFirstHandle, staleFrame, sizeof(staleFrame));
  fixture.link.loop(1502U);

  const BleLinkStatus current = fixture.link.status(1502U);
  assert(current.connected && current.connectionHandle == kSecondHandle);
  assert(fixture.server->disconnectCalls.empty());
  assert(!hasEvent(fixture.delegate, BleLinkEvent::ProtocolViolation));
}

void testIdleUnsubscribeAndNeverSubscribedWriteCloseBoundedly() {
  {
    Fixture fixture;
    fixture.secureAndSubscribe(kFirstHandle, 1700U);
    fixture.delegate.events.clear();
    fixture.server->disconnectCalls.clear();
    fixture.tx->simulateSubscribe(kFirstHandle, 0U);
    assert(!fixture.link.status(1701U).requestInFlight);
    fixture.link.loop(1701U);
    assert(fixture.server->disconnectCalls.size() == 1U);
    assert(fixture.server->disconnectCalls.back() == kFirstHandle);
    assert(hasEvent(fixture.delegate, BleLinkEvent::TransportFailure));
  }

  {
    Fixture fixture;
    fixture.connect(kSecondHandle, 1800U);
    fixture.server->simulateAuthenticationComplete(
        NimBLEConnInfo(kSecondHandle, 247U, true, true, true, 16U));
    fixture.link.loop(1800U);
    fixture.delegate.events.clear();
    fixture.server->disconnectCalls.clear();
    static const uint8_t frame[] = {0U, 0U, 0U, 2U, '{', '}'};
    fixture.rx->simulateWrite(kSecondHandle, frame, sizeof(frame));
    assert(!fixture.link.status(1801U).requestInFlight);
    fixture.link.loop(1801U);
    assert(fixture.server->disconnectCalls.size() == 1U);
    assert(fixture.server->disconnectCalls.back() == kSecondHandle);
    assert(hasEvent(fixture.delegate, BleLinkEvent::ProtocolViolation));
  }
}

void testUnsubscribeAbortsQueuedResponseAndMultiChunkCompletesExactlyOnce() {
  {
    Fixture fixture;
    fixture.secureAndSubscribe(kFirstHandle, 2000U);
    fixture.tx->notifyResult = true;
    fixture.tx->statusOnNotify = false;
    static const uint8_t response[] = {'{', '}'};
    assert(fixture.link.queueFrame(response, sizeof(response)));
    fixture.link.loop(2001U);
    fixture.tx->simulateSubscribe(kFirstHandle, 0U);
    assert(!fixture.link.status(2002U).requestInFlight);
    fixture.link.loop(2002U);
    assert(fixture.server->disconnectCalls.back() == kFirstHandle);
    assert(hasEvent(fixture.delegate, BleLinkEvent::TransportFailure));
  }

  {
    Fixture fixture;
    fixture.secureAndSubscribe(kSecondHandle, 3000U, 23U);
    fixture.tx->statusOnNotify = true;
    fixture.tx->notifyStatusCode = 0;
    uint8_t response[50]{};
    for (size_t index = 0U; index < sizeof(response); ++index) {
      response[index] = static_cast<uint8_t>('a' + (index % 26U));
    }
    assert(fixture.link.queueFrame(response, sizeof(response)));
    fixture.link.loop(3001U);
    fixture.link.loop(3002U);
    fixture.link.loop(3003U);
    assert(fixture.tx->notifiedValues.size() == 3U);  // (4 + 50) / 20
    assert(fixture.tx->notifiedValues[0].size() == 20U);
    assert(fixture.tx->notifiedValues[1].size() == 20U);
    assert(fixture.tx->notifiedValues[2].size() == 14U);
    assert(!fixture.link.status(3003U).requestInFlight);
  }
}

}  // namespace

int main() {
  testCorrectPasskeyIsSurfacedAndAccepted();
  testWrongHandleClosedWindowAndDuplicatePromptAreRejected();
  testExplicitCancelWindowExpiryAndNumericTimeoutReject();
  testWindowAndSingleConnectionSurviveDisconnectReconnect();
  testNotifySuccessUsesInternalNonzeroHandleToken();
  testNotifyFalseWithoutCallbackRetriesWithoutAdvancing();
  testRetryableNotifyStatusIsBoundedAndNeverProtocolViolation();
  testTerminalNotifyStatusAndMissingCallbackTimeout();
  testGenerationClearsStaleDisconnectAndQueuedFailure();
  testClosedGenerationDropsQueuedConnectAndAuthenticationEvents();
  testWriteFromDepartedHandleCannotCloseReplacementLink();
  testIdleUnsubscribeAndNeverSubscribedWriteCloseBoundedly();
  testUnsubscribeAbortsQueuedResponseAndMultiChunkCompletesExactlyOnce();
  return 0;
}
