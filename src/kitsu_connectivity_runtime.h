#pragma once

#include <stdint.h>

#include "kitsu_connectivity_config.h"

namespace kitsu868 {
namespace connectivity {

constexpr uint32_t kWifiConnectTimeoutMs = 20000UL;
constexpr uint32_t kWifiReconnectInitialMs = 5000UL;
constexpr uint32_t kWifiReconnectMaximumMs = 300000UL;

enum class WifiRuntimeState : uint8_t {
  Unconfigured = 0,
  StorageUnavailable,
  ConnectivityUnavailable,
  BleActive,
  Grace,
  Connecting,
  Connected,
  Backoff,
};

enum class LanRuntimeState : uint8_t {
  Unconfigured = 0,
  StorageUnavailable,
  ConnectivityUnavailable,
  WifiPending,
  EnrollmentPending,
  TimePending,
  TlsPending,
  BleActive,
};

const char* wifiRuntimeStateName(WifiRuntimeState state);
const char* lanRuntimeStateName(LanRuntimeState state);

enum class WifiLinkObservation : uint8_t {
  Down = 0,
  Connecting,
  ConnectedAccepted,
  ConnectedDowngraded,
  Failed,
};

enum class WifiPolicyAction : uint8_t { None = 0, Start, Stop };

struct ConnectivityPrerequisites {
  bool configStoreReady = false;
  bool remoteConnectivityAllowed = false;
  bool wifiConfigured = false;
  bool gatewayConfigured = false;
  bool gatewayEnrolled = false;
  bool timeValid = false;
  bool authenticatedBleSession = false;
};

struct ConnectivityRuntimeStatus {
  WifiRuntimeState wifiState = WifiRuntimeState::Unconfigured;
  LanRuntimeState lanState = LanRuntimeState::Unconfigured;
  uint8_t reconnectFailures = 0U;
  uint32_t retryRemainingMs = 0U;
};

// Pure state machine: no Arduino, radio, Wi-Fi, clock, or allocation calls.
// The platform wrapper performs only the explicit Start/Stop actions returned
// here, making BLE priority and owner-controlled connectivity host-testable.
class ConnectivityPolicy {
 public:
  WifiPolicyAction tick(uint32_t now,
                        const ConnectivityPrerequisites& prerequisites,
                        WifiLinkObservation observation);
  ConnectivityRuntimeStatus status(uint32_t now) const;
  void reset();

 private:
  void enterBackoff(uint32_t now);
  static bool deadlineReached(uint32_t now, uint32_t deadline);

  ConnectivityRuntimeStatus status_{};
  uint32_t connectStartedAt_ = 0U;
  uint32_t retryAt_ = 0U;
};

#if defined(ARDUINO_ARCH_ESP32)

enum class TrustedTimeSource : uint8_t {
  None = 0,
  AuthenticatedBle,
  NetworkTime,
};

// TLS must consult this provenance gate, not merely a plausible libc clock.
// A retained/garbage RTC value after reset therefore cannot silently enable
// certificate validation. Authenticated BLE clock.sync and a completed SNTP
// synchronization are the only runtime sources.
bool trustedWallClock(int64_t& epoch,
                      TrustedTimeSource* source = nullptr);
const char* trustedTimeSourceName(TrustedTimeSource source);

class Esp32WifiRuntime {
 public:
  bool begin(ConnectionConfigStore& store, bool remoteConnectivityAllowed,
             const char* hostname);
  void loop(uint32_t now, bool authenticatedBleSession);
  // Called only after an authenticated, read-back-verified Wi-Fi commit. The
  // next loop drops any old driver association and applies the new encrypted
  // generation immediately.
  void requestCredentialReload();
  bool noteAuthenticatedTime(uint32_t epoch);
  void stop();
  ConnectivityRuntimeStatus status(uint32_t now) const;

 private:
  WifiLinkObservation observeLink() const;
  bool acceptedAuthMode() const;
  bool startAssociation();
  void stopAssociation();
  void serviceNetworkTime(uint32_t now, bool wifiConnected);

  ConnectionConfigStore* store_ = nullptr;
  ConnectivityPolicy policy_{};
  WifiConfig activeWifi_{};
  bool begun_ = false;
  bool remoteConnectivityAllowed_ = false;
  bool credentialReloadRequested_ = false;
  bool associationStarted_ = false;
  bool startFailed_ = false;
  bool networkTimeStarted_ = false;
  bool networkTimeCandidate_ = false;
  uint32_t networkTimeCandidateAt_ = 0U;
  int64_t networkTimeCandidateEpoch_ = 0;
  WifiSecurity activeSecurity_ = WifiSecurity::Wpa2;
  char hostname_[33]{};
};

#endif

}  // namespace connectivity
}  // namespace kitsu868
