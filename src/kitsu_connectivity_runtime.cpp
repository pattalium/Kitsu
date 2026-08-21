#include "kitsu_connectivity_runtime.h"

#include <string.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <time.h>
#endif

namespace kitsu868 {
namespace connectivity {

namespace {

#if defined(ARDUINO_ARCH_ESP32)
constexpr time_t kMinimumTrustedEpoch = static_cast<time_t>(1704067200UL);
constexpr time_t kMaximumTrustedEpoch = static_cast<time_t>(4102444800UL);
constexpr uint32_t kNetworkTimeStabilityMs = 2000UL;
volatile TrustedTimeSource gTrustedTimeSource = TrustedTimeSource::None;
#endif

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

}  // namespace

#if defined(ARDUINO_ARCH_ESP32)

bool trustedWallClock(int64_t& epoch, TrustedTimeSource* source) {
  epoch = static_cast<int64_t>(time(nullptr));
  const TrustedTimeSource observed = gTrustedTimeSource;
  if (source) *source = observed;
  return observed != TrustedTimeSource::None &&
         epoch >= static_cast<int64_t>(kMinimumTrustedEpoch) &&
         epoch <= static_cast<int64_t>(kMaximumTrustedEpoch);
}

const char* trustedTimeSourceName(TrustedTimeSource source) {
  switch (source) {
    case TrustedTimeSource::AuthenticatedBle: return "authenticated_ble";
    case TrustedTimeSource::NetworkTime: return "network_time";
    default: return "none";
  }
}

#endif

const char* wifiRuntimeStateName(WifiRuntimeState state) {
  switch (state) {
    case WifiRuntimeState::Unconfigured: return "unconfigured";
    case WifiRuntimeState::StorageUnavailable: return "storage_unavailable";
    case WifiRuntimeState::ConnectivityUnavailable:
      return "connectivity_unavailable";
    case WifiRuntimeState::BleActive: return "ble_active";
    case WifiRuntimeState::Grace: return "grace";
    case WifiRuntimeState::Connecting: return "connecting";
    case WifiRuntimeState::Connected: return "connected";
    case WifiRuntimeState::Backoff: return "backoff";
    default: return "connectivity_unavailable";
  }
}

const char* lanRuntimeStateName(LanRuntimeState state) {
  switch (state) {
    case LanRuntimeState::Unconfigured: return "unconfigured";
    case LanRuntimeState::StorageUnavailable: return "storage_unavailable";
    case LanRuntimeState::ConnectivityUnavailable:
      return "connectivity_unavailable";
    case LanRuntimeState::WifiPending: return "wifi_pending";
    case LanRuntimeState::EnrollmentPending: return "enrollment_pending";
    case LanRuntimeState::TimePending: return "time_pending";
    case LanRuntimeState::TlsPending: return "tls_pending";
    case LanRuntimeState::BleActive: return "ble_active";
    default: return "connectivity_unavailable";
  }
}

bool ConnectivityPolicy::deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

void ConnectivityPolicy::enterBackoff(uint32_t now) {
  if (status_.reconnectFailures < 16U) ++status_.reconnectFailures;
  uint32_t delayMs = kWifiReconnectInitialMs;
  for (uint8_t i = 1U; i < status_.reconnectFailures; ++i) {
    if (delayMs >= kWifiReconnectMaximumMs / 2U) {
      delayMs = kWifiReconnectMaximumMs;
      break;
    }
    delayMs *= 2U;
  }
  if (delayMs > kWifiReconnectMaximumMs) delayMs = kWifiReconnectMaximumMs;
  retryAt_ = now + delayMs;
  status_.wifiState = WifiRuntimeState::Backoff;
}

void ConnectivityPolicy::reset() {
  status_ = ConnectivityRuntimeStatus{};
  connectStartedAt_ = 0U;
  retryAt_ = 0U;
}

WifiPolicyAction ConnectivityPolicy::tick(
    uint32_t now, const ConnectivityPrerequisites& prerequisites,
    WifiLinkObservation observation) {
  if (!prerequisites.configStoreReady) {
    status_.wifiState = WifiRuntimeState::StorageUnavailable;
    status_.lanState = LanRuntimeState::StorageUnavailable;
    status_.reconnectFailures = 0U;
    retryAt_ = 0U;
    return observation == WifiLinkObservation::Down
               ? WifiPolicyAction::None
               : WifiPolicyAction::Stop;
  }
  // Wi-Fi is independently useful and independently provisioned. In
  // particular, it must associate before a gateway is selected so the owner
  // can verify the network and so gateway enrollment can hand off to an
  // already-live link. The LAN/bootstrap services enforce their own gateway
  // and enrollment prerequisites below this layer.
  if (!prerequisites.wifiConfigured) {
    status_.wifiState = WifiRuntimeState::Unconfigured;
    status_.lanState = LanRuntimeState::Unconfigured;
    status_.reconnectFailures = 0U;
    retryAt_ = 0U;
    return observation == WifiLinkObservation::Down
               ? WifiPolicyAction::None
               : WifiPolicyAction::Stop;
  }
  if (!prerequisites.remoteConnectivityAllowed) {
    status_.wifiState = WifiRuntimeState::ConnectivityUnavailable;
    status_.lanState = LanRuntimeState::ConnectivityUnavailable;
    status_.reconnectFailures = 0U;
    retryAt_ = 0U;
    return observation == WifiLinkObservation::Down
               ? WifiPolicyAction::None
               : WifiPolicyAction::Stop;
  }
  // ESP32-S3 arbitrates Wi-Fi/BLE coexistence. Keep only the STA association
  // alive while authenticated BLE has control; the enrollment/bootstrap and
  // steady LAN services remain stopped by main's BLE-priority gates. This
  // makes a successful Wi-Fi save observable and removes the cold-association
  // gap during the intentional BLE -> gateway handoff.
  status_.lanState = !prerequisites.gatewayConfigured
                         ? LanRuntimeState::Unconfigured
                         : prerequisites.authenticatedBleSession
                               ? LanRuntimeState::BleActive
                               : LanRuntimeState::WifiPending;
  if (observation == WifiLinkObservation::ConnectedDowngraded) {
    enterBackoff(now);
    return WifiPolicyAction::Stop;
  }
  if (observation == WifiLinkObservation::ConnectedAccepted) {
    status_.wifiState = WifiRuntimeState::Connected;
    if (!prerequisites.gatewayConfigured) {
      status_.lanState = LanRuntimeState::Unconfigured;
    } else if (prerequisites.authenticatedBleSession) {
      status_.lanState = LanRuntimeState::BleActive;
    } else if (!prerequisites.gatewayEnrolled) {
      status_.lanState = LanRuntimeState::EnrollmentPending;
    } else if (!prerequisites.timeValid) {
      status_.lanState = LanRuntimeState::TimePending;
    } else {
      status_.lanState = LanRuntimeState::TlsPending;
    }
    status_.reconnectFailures = 0U;
    retryAt_ = 0U;
    return WifiPolicyAction::None;
  }
  if (observation == WifiLinkObservation::Failed) {
    enterBackoff(now);
    return WifiPolicyAction::Stop;
  }
  if (observation == WifiLinkObservation::Connecting) {
    if (now - connectStartedAt_ >= kWifiConnectTimeoutMs) {
      enterBackoff(now);
      return WifiPolicyAction::Stop;
    }
    status_.wifiState = WifiRuntimeState::Connecting;
    return WifiPolicyAction::None;
  }
  if (retryAt_ != 0U && !deadlineReached(now, retryAt_)) {
    status_.wifiState = WifiRuntimeState::Backoff;
    return WifiPolicyAction::None;
  }
  retryAt_ = 0U;
  connectStartedAt_ = now;
  status_.wifiState = WifiRuntimeState::Connecting;
  return WifiPolicyAction::Start;
}

ConnectivityRuntimeStatus ConnectivityPolicy::status(uint32_t now) const {
  ConnectivityRuntimeStatus output = status_;
  if (output.wifiState == WifiRuntimeState::Backoff && retryAt_ != 0U &&
      !deadlineReached(now, retryAt_)) {
    output.retryRemainingMs = retryAt_ - now;
  }
  return output;
}

#if defined(ARDUINO_ARCH_ESP32)

bool Esp32WifiRuntime::begin(ConnectionConfigStore& store,
                             bool remoteConnectivityAllowed,
                             const char* hostname) {
  stop();
  if (!hostname || hostname[0] == '\0' || strlen(hostname) > 32U) {
    return false;
  }
  store_ = &store;
  remoteConnectivityAllowed_ = remoteConnectivityAllowed;
  memcpy(hostname_, hostname, strlen(hostname) + 1U);
  policy_.reset();
  credentialReloadRequested_ = false;
  begun_ = true;
  return true;
}

void Esp32WifiRuntime::requestCredentialReload() {
  if (begun_) credentialReloadRequested_ = true;
}

bool Esp32WifiRuntime::acceptedAuthMode() const {
  wifi_ap_record_t accessPoint{};
  if (esp_wifi_sta_get_ap_info(&accessPoint) != ESP_OK) return false;
  const wifi_auth_mode_t observed = accessPoint.authmode;
  secureZero(&accessPoint, sizeof(accessPoint));
  if (activeSecurity_ == WifiSecurity::Wpa3) {
    return observed == WIFI_AUTH_WPA3_PSK;
  }
  return observed == WIFI_AUTH_WPA2_PSK ||
         observed == WIFI_AUTH_WPA2_WPA3_PSK ||
         observed == WIFI_AUTH_WPA3_PSK;
}

WifiLinkObservation Esp32WifiRuntime::observeLink() const {
  if (startFailed_) return WifiLinkObservation::Failed;
  if (!associationStarted_) return WifiLinkObservation::Down;
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    return acceptedAuthMode() ? WifiLinkObservation::ConnectedAccepted
                              : WifiLinkObservation::ConnectedDowngraded;
  }
  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    return WifiLinkObservation::Failed;
  }
  return WifiLinkObservation::Connecting;
}

bool Esp32WifiRuntime::startAssociation() {
  startFailed_ = false;
  if (!remoteConnectivityAllowed_ || !store_ ||
      !store_->copyWifi(activeWifi_)) {
    secureZero(&activeWifi_, sizeof(activeWifi_));
    startFailed_ = true;
    return false;
  }
  activeSecurity_ = activeWifi_.security;
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname_);
  WiFi.setMinSecurity(activeSecurity_ == WifiSecurity::Wpa3
                          ? WIFI_AUTH_WPA3_PSK
                          : WIFI_AUTH_WPA2_PSK);
  const wl_status_t started = WiFi.begin(
      reinterpret_cast<const char*>(activeWifi_.ssid),
      activeWifi_.passphrase);
  // The IDF driver owns its connection state now.  Do not keep a second
  // plaintext passphrase in the application runtime.
  secureZero(&activeWifi_, sizeof(activeWifi_));
  associationStarted_ = true;
  if (started == WL_CONNECT_FAILED) startFailed_ = true;
  return !startFailed_;
}

void Esp32WifiRuntime::serviceNetworkTime(uint32_t now,
                                          bool wifiConnected) {
  if (!remoteConnectivityAllowed_ || !wifiConnected) {
    if (networkTimeStarted_) sntp_stop();
    networkTimeStarted_ = false;
    networkTimeCandidate_ = false;
    networkTimeCandidateAt_ = 0U;
    networkTimeCandidateEpoch_ = 0;
    return;
  }
  if (!networkTimeStarted_) {
    // configTime starts lwIP SNTP asynchronously. No DNS, UDP, or wait loop is
    // executed on the Arduino application loop. Three independent operators
    // provide bounded fallback if one source is unavailable.
    configTime(0, 0, "time.cloudflare.com", "time.google.com",
               "pool.ntp.org");
    networkTimeStarted_ = true;
    networkTimeCandidate_ = false;
  }
  const time_t epoch = time(nullptr);
  if (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED ||
      epoch < kMinimumTrustedEpoch || epoch > kMaximumTrustedEpoch) {
    networkTimeCandidate_ = false;
    return;
  }
  if (!networkTimeCandidate_) {
    networkTimeCandidate_ = true;
    networkTimeCandidateAt_ = now;
    networkTimeCandidateEpoch_ = epoch;
    return;
  }
  if (static_cast<uint32_t>(now - networkTimeCandidateAt_) <
      kNetworkTimeStabilityMs) {
    return;
  }
  const time_t minimumProgress = networkTimeCandidateEpoch_;
  const time_t maximumProgress = networkTimeCandidateEpoch_ + 30;
  if (epoch < minimumProgress || epoch > maximumProgress) {
    networkTimeCandidate_ = false;
    return;
  }
  gTrustedTimeSource = TrustedTimeSource::NetworkTime;
}

bool Esp32WifiRuntime::noteAuthenticatedTime(uint32_t epoch) {
  if (epoch < static_cast<uint32_t>(kMinimumTrustedEpoch) ||
      epoch > static_cast<uint32_t>(kMaximumTrustedEpoch)) {
    return false;
  }
  gTrustedTimeSource = TrustedTimeSource::AuthenticatedBle;
  return true;
}

void Esp32WifiRuntime::stopAssociation() {
  if (associationStarted_ || WiFi.getMode() != WIFI_MODE_NULL) {
    // Persistent storage is disabled before clearing the driver's in-RAM AP
    // configuration. Our encrypted raw store is the only durable authority.
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
  }
  associationStarted_ = false;
  startFailed_ = false;
  activeSecurity_ = WifiSecurity::Wpa2;
  if (networkTimeStarted_) sntp_stop();
  networkTimeStarted_ = false;
  networkTimeCandidate_ = false;
  networkTimeCandidateAt_ = 0U;
  networkTimeCandidateEpoch_ = 0;
  secureZero(&activeWifi_, sizeof(activeWifi_));
}

void Esp32WifiRuntime::loop(uint32_t now, bool authenticatedBleSession) {
  if (!begun_ || !store_) return;
  if (credentialReloadRequested_) {
    credentialReloadRequested_ = false;
    stopAssociation();
    policy_.reset();
  }
  const ConnectionConfigStatus configured = store_->status();
  const WifiLinkObservation observation = observeLink();
  // Association is useful on its own, but an owner who has only stored Wi-Fi
  // has not opted into gateway traffic yet. Start SNTP only once gateway
  // trust is configured; TLS still waits for the resulting trusted clock.
  serviceNetworkTime(
      now, configured.gatewayConfigured &&
               observation == WifiLinkObservation::ConnectedAccepted);
  int64_t wallClock = 0;
  const bool timeValid = trustedWallClock(wallClock);
  ConnectivityPrerequisites prerequisites{};
  prerequisites.configStoreReady = configured.begun;
  prerequisites.remoteConnectivityAllowed = remoteConnectivityAllowed_;
  prerequisites.wifiConfigured = configured.wifiConfigured;
  prerequisites.gatewayConfigured = configured.gatewayConfigured;
  prerequisites.gatewayEnrolled = configured.gatewayEnrolled;
  prerequisites.timeValid = timeValid;
  prerequisites.authenticatedBleSession = authenticatedBleSession;
  const WifiPolicyAction action =
      policy_.tick(now, prerequisites, observation);
  if (action == WifiPolicyAction::Stop) {
    stopAssociation();
  } else if (action == WifiPolicyAction::Start) {
    (void)startAssociation();
  }
}

void Esp32WifiRuntime::stop() {
  if (begun_) stopAssociation();
  policy_.reset();
  begun_ = false;
  store_ = nullptr;
  remoteConnectivityAllowed_ = false;
  credentialReloadRequested_ = false;
  memset(hostname_, 0, sizeof(hostname_));
}

ConnectivityRuntimeStatus Esp32WifiRuntime::status(uint32_t now) const {
  return policy_.status(now);
}

#endif

}  // namespace connectivity
}  // namespace kitsu868
