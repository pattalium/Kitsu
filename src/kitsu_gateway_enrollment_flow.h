#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_enrollment.h"

namespace kitsu868 {
namespace connectivity {

constexpr uint32_t kGatewayEnrollmentPhysicalWindowMs = 60000UL;
constexpr uint32_t kGatewayEnrollmentBootstrapWindowMs = 5UL * 60UL * 1000UL;
constexpr size_t kGatewayEnrollmentClaimTokenBytes = 43U;

enum class GatewayEnrollmentFlowState : uint8_t {
  Idle = 0,
  PhysicalConfirmationRequired,
  PhysicalConfirmed,
  ReadyForWifi,
  Bootstrapping,
  Enrolled,
  Failed,
  Expired,
};

enum class GatewayEnrollmentError : uint8_t {
  None = 0,
  InvalidRequest,
  NotConfigured,
  AlreadyEnrolled,
  TimeUnset,
  ConnectivityUnavailable,
  Busy,
  PhysicalConfirmationRequired,
  Expired,
  StorageFailed,
  BootstrapFailed,
};

const char* gatewayEnrollmentFlowStateName(GatewayEnrollmentFlowState state);
const char* gatewayEnrollmentErrorName(GatewayEnrollmentError error);

struct GatewayEnrollmentGuards {
  bool authenticatedController = false;
  bool storageReady = false;
  bool gatewayConfigured = false;
  bool alreadyEnrolled = false;
  bool trustedClock = false;
  bool remoteConnectivityAllowed = false;
};

struct GatewayEnrollmentReceipt {
  bool accepted = false;
  GatewayEnrollmentFlowState state = GatewayEnrollmentFlowState::Idle;
  GatewayEnrollmentError error = GatewayEnrollmentError::None;
  bool hasEnrollmentId = false;
  uint8_t enrollmentId[kEnrollmentUuidBytes]{};
  uint32_t expiresInMs = 0U;
};

struct GatewayEnrollmentFlowStatus {
  GatewayEnrollmentFlowState state = GatewayEnrollmentFlowState::Idle;
  GatewayEnrollmentError lastError = GatewayEnrollmentError::None;
  bool hasEnrollmentId = false;
  uint8_t enrollmentId[kEnrollmentUuidBytes]{};
  uint32_t expiresInMs = 0U;
};

// Strict codecs for the two authenticated direct-body operations. Unknown or
// duplicate fields, escaped strings, non-canonical UUIDs/base64url, and any
// missing field are rejected. Claim bytes remain only in caller-owned output.
bool decodeGatewayEnrollmentBegin(
    const uint8_t* json, size_t jsonBytes,
    uint8_t enrollmentId[kEnrollmentUuidBytes],
    char claimToken[kGatewayEnrollmentClaimTokenBytes + 1U]);
bool decodeGatewayEnrollmentFinish(
    const uint8_t* json, size_t jsonBytes,
    uint8_t enrollmentId[kEnrollmentUuidBytes]);

bool encodeGatewayEnrollmentReceipt(const GatewayEnrollmentReceipt& receipt,
                                    uint8_t* output, size_t outputCapacity,
                                    size_t& outputBytes);
bool encodeGatewayEnrollmentEvent(const GatewayEnrollmentReceipt& receipt,
                                  uint8_t* output, size_t outputCapacity,
                                  size_t& outputBytes);

// One owner-authorized enrollment attempt. begin() stages only the one-use
// claim token and enrollment UUID in bounded transient RAM. confirmPhysical()
// is called only from the PRG-hold path. finish() moves the token into the
// recipient, generates the CSR/HPKE/proof, and wipes its staging copy. The
// coordinator never exposes those values through status, receipts, or events.
class KitsuGatewayEnrollmentFlow {
 public:
  KitsuGatewayEnrollmentFlow();
  ~KitsuGatewayEnrollmentFlow();

  KitsuGatewayEnrollmentFlow(const KitsuGatewayEnrollmentFlow&) = delete;
  KitsuGatewayEnrollmentFlow& operator=(
      const KitsuGatewayEnrollmentFlow&) = delete;

  void beginOperation(const uint8_t* json, size_t jsonBytes,
                      const GatewayEnrollmentGuards& guards,
                      uint32_t nowMillis, GatewayEnrollmentReceipt& receipt);
  bool confirmPhysical(uint32_t nowMillis,
                       GatewayEnrollmentReceipt& eventReceipt);
  void finishOperation(
      const uint8_t* json, size_t jsonBytes,
      const GatewayEnrollmentGuards& guards,
      const uint8_t expectedGatewayUuid[kEnrollmentUuidBytes],
      const char* hardwareUid, size_t hardwareUidBytes,
      uint32_t nowMillis, companion::CompanionCrypto& hashes,
      EnrollmentPlatformCrypto& platform, KitsuEnrollmentRecipient& recipient,
      GatewayEnrollmentReceipt& receipt);

  // A disconnect before successful finish invalidates the physical gesture
  // and wipes the staged token. Once ReadyForWifi, disconnect is intentional:
  // it releases the already-associated Wi-Fi link to gateway bootstrap.
  void onBleDisconnected();
  bool poll(uint32_t nowMillis, GatewayEnrollmentReceipt* transition = nullptr);

  bool markBootstrapping(uint32_t nowMillis);
  void completeBootstrap(
      bool installed,
      GatewayEnrollmentError failure = GatewayEnrollmentError::BootstrapFailed);
  void abort();
  GatewayEnrollmentFlowStatus status(uint32_t nowMillis) const;

 private:
  void clearAttemptSecrets();
  void fillReceipt(bool accepted, GatewayEnrollmentFlowState state,
                   GatewayEnrollmentError error, uint32_t nowMillis,
                   GatewayEnrollmentReceipt& receipt) const;
  bool activeAuthorizationState() const;
  bool sameEnrollmentId(const uint8_t id[kEnrollmentUuidBytes]) const;
  bool deadlineReached(uint32_t nowMillis) const;
  uint32_t remaining(uint32_t nowMillis) const;

  GatewayEnrollmentFlowState state_ = GatewayEnrollmentFlowState::Idle;
  GatewayEnrollmentError lastError_ = GatewayEnrollmentError::None;
  bool hasEnrollmentId_ = false;
  uint8_t enrollmentId_[kEnrollmentUuidBytes]{};
  char claimToken_[kGatewayEnrollmentClaimTokenBytes + 1U]{};
  uint32_t deadline_ = 0U;
  KitsuEnrollmentRecipient* recipient_ = nullptr;
};

}  // namespace connectivity
}  // namespace kitsu868
