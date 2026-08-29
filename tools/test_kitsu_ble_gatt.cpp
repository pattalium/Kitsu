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
  }

  ~Fixture() { link.end(); }

  void connect(uint16_t handle, uint32_t nowMillis) {
    setFakeMillis(nowMillis);
    server->simulateConnect(NimBLEConnInfo(handle, 247U));
  }

  TestDelegate delegate{};
  KitsuBleGattLink link{};
  NimBLEServer* server = nullptr;
};

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

}  // namespace

int main() {
  testCorrectPasskeyIsSurfacedAndAccepted();
  testWrongHandleClosedWindowAndDuplicatePromptAreRejected();
  testExplicitCancelWindowExpiryAndNumericTimeoutReject();
  testWindowAndSingleConnectionSurviveDisconnectReconnect();
  return 0;
}
