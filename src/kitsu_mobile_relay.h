#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_gateway_lan_runtime.h"

namespace kitsu868 {
namespace connectivity {

constexpr char kMobileRelayOperation[] = "mobile.relay.exchange";
constexpr char kMobileRelayExchangeSchema[] =
    "kitsu.mobile-relay.exchange.v1";
constexpr char kMobileRelayChunkSchema[] =
    "kitsu.mobile-relay.chunk.v1";
constexpr char kMobileRelayReceiptSchema[] =
    "kitsu.mobile-relay.receipt.v1";

constexpr size_t kMobileRelayChunkBytes = 8U * 1024U;
constexpr size_t kMobileRelayMaximumEnrollmentRequestBytes =
    kEnrollmentMaximumRequestBytes;
constexpr size_t kMobileRelayMaximumEnrollmentResponseBytes = 32U * 1024U;
constexpr size_t kMobileRelayMaximumGatewayCaBytes = 8U * 1024U;
constexpr size_t kMobileRelayPendingPayloadDepth = 2U;

enum class MobileRelayResult : uint8_t {
  Ok = 0,
  NotBegun,
  InvalidRequest,
  AuthorizationRequired,
  PhysicalConfirmationRequired,
  EnrollmentUnavailable,
  GatewayConfigurationFailed,
  Busy,
  Oversize,
  OffsetMismatch,
  OutOfMemory,
  CredentialsUnavailable,
  CredentialsInvalid,
  SequenceStoreFailed,
  CryptoFailed,
  UnexpectedAck,
  GatewayFrameRejected,
  ActionStoreFailed,
  ActionSinkFailed,
  OutputTooSmall,
};

const char* mobileRelayResultName(MobileRelayResult result);

struct MobileRelayGuards {
  // Authenticated means the request arrived through the existing verified
  // BLE operation envelope. The relay never accepts an unauthenticated
  // shortcut around KitsuBleSession.
  bool authenticatedController = false;
  // This is true only while the existing gateway enrollment coordinator is
  // ReadyForWifi: owner begin and the local PRG hold have already succeeded,
  // and KitsuEnrollmentRecipient owns the pending private material.
  bool enrollmentPrgConfirmed = false;
  bool enrollmentActive = false;
};

struct MobileRelayExchangeOutcome {
  MobileRelayResult result = MobileRelayResult::InvalidRequest;
  bool gatewayConfigured = false;
  bool gatewayConfigurationChanged = false;
  bool enrollmentCompleted = false;
  bool downlinkCompleted = false;
};

// Stores only the logical gateway UUID and enrollment issuer CA. No Wi-Fi,
// host, SNI, SPKI, bootstrap port, or steady LAN port is synthesized.
enum class MobileRelayGatewayConfigResult : uint8_t {
  Unchanged = 0,
  Changed,
  Failed,
};

class MobileRelayGatewayConfigSink {
 public:
  virtual ~MobileRelayGatewayConfigSink() = default;
  virtual MobileRelayGatewayConfigResult commitMobileRelayGateway(
      const uint8_t gatewayUuid[kEnrollmentUuidBytes],
      const uint8_t* caCertificateDer, size_t caCertificateBytes) = 0;
};

// This narrow seam deliberately delegates enrollment to the already active
// KitsuEnrollmentRecipient and the existing EnrollmentCredentialSink. The
// relay owns chunk assembly only; it is not a second enrollment state machine.
class MobileRelayEnrollmentDelegate {
 public:
  virtual ~MobileRelayEnrollmentDelegate() = default;
  virtual bool buildMobileRelayEnrollmentRequest(
      uint8_t* output, size_t outputCapacity, size_t& outputBytes) = 0;
  virtual bool installMobileRelayEnrollmentResponse(
      const uint8_t* response, size_t responseBytes) = 0;
};

struct MobileRelayStatus {
  bool begun = false;
  bool uploadActive = false;
  bool uplinkPending = false;
  size_t uploadBytes = 0U;
  size_t uploadTotal = 0U;
  size_t uplinkBytes = 0U;
  uint64_t uplinkSequence = 0U;
  size_t pendingPayloads = 0U;
};

// Authenticated BLE transports byte-exact backend documents. One ordered
// upload assembly and one signed device uplink are bounded in RAM. Device
// envelopes and backend ACK/actions use the existing LAN codec, HMAC, durable
// sequence store, replay store, and action dispatcher unchanged.
class KitsuMobileRelay {
 public:
  KitsuMobileRelay();
  ~KitsuMobileRelay();

  KitsuMobileRelay(const KitsuMobileRelay&) = delete;
  KitsuMobileRelay& operator=(const KitsuMobileRelay&) = delete;

  bool begin(GatewayLanCredentialProvider& credentials,
             GatewayLanSequenceStore& sequences,
             companion::CompanionCrypto& crypto,
             LanActionReplayStore& replayStore,
             GatewayLanActionSink& actionSink,
             MobileRelayGatewayConfigSink& gatewayConfig,
             MobileRelayEnrollmentDelegate& enrollment);
  void stop();

  // Drops only a partial phone-to-device upload when the BLE owner session
  // closes. A signed uplink remains available for a later authenticated owner
  // session until its exact backend ACK arrives.
  void onBleDisconnected();

  bool handleExchange(const uint8_t* requestJson, size_t requestBytes,
                      const MobileRelayGuards& guards, int64_t nowEpoch,
                      bool clockValid, uint8_t* responseJson,
                      size_t responseCapacity, size_t& responseBytes,
                      MobileRelayExchangeOutcome* outcome = nullptr);

  bool canEnqueueDevicePayload(size_t payloadBytes) const;
  bool canEnqueueDevicePayloads(size_t payloadCount,
                                size_t worstCasePayloadBytes) const;
  MobileRelayResult enqueueDevicePayload(
      const char* payloadType, const uint8_t* payload, size_t payloadBytes,
      int64_t issuedEpoch, uint64_t* assignedSequence = nullptr);

  MobileRelayStatus status() const;

 private:
  enum class UploadKind : uint8_t { None = 0, Enrollment, Downlink };

  struct PendingPayload {
    uint8_t* bytes = nullptr;
    size_t payloadBytes = 0U;
    int64_t issuedEpoch = 0;
    char payloadType[kLanMaximumPayloadTypeBytes + 1U]{};
  };

  void clearUpload();
  void clearUplink();
  void clearPendingPayloads();
  bool reserveSequence(uint64_t& output);
  MobileRelayResult encodeUplink(
      const char* payloadType, const uint8_t* payload, size_t payloadBytes,
      int64_t issuedEpoch, uint64_t* assignedSequence);
  void promotePendingPayload();
  MobileRelayResult handleCompletedDownlink(int64_t nowEpoch,
                                             bool clockValid);

  GatewayLanCredentialProvider* credentials_ = nullptr;
  GatewayLanSequenceStore* sequences_ = nullptr;
  companion::CompanionCrypto* crypto_ = nullptr;
  LanActionReplayStore* replayStore_ = nullptr;
  GatewayLanActionSink* actionSink_ = nullptr;
  MobileRelayGatewayConfigSink* gatewayConfig_ = nullptr;
  MobileRelayEnrollmentDelegate* enrollment_ = nullptr;
  bool begun_ = false;

  uint8_t* upload_ = nullptr;
  size_t uploadBytes_ = 0U;
  size_t uploadTotal_ = 0U;
  UploadKind uploadKind_ = UploadKind::None;

  uint8_t* uplink_ = nullptr;
  size_t uplinkBytes_ = 0U;
  uint64_t uplinkSequence_ = 0U;
  uint8_t uplinkCompanionUuid_[kLanUuidBytes]{};
  uint8_t uplinkGatewayUuid_[kLanUuidBytes]{};
  uint32_t uplinkKeyVersion_ = 0U;
  PendingPayload pending_[kMobileRelayPendingPayloadDepth]{};
  size_t pendingPayloadCount_ = 0U;

  uint64_t nextTxSequence_ = 0U;
  uint64_t lastTxSequence_ = 0U;
};

static_assert(kMobileRelayChunkBytes <=
                  companion::kMaximumEnvelopePayloadBytes,
              "mobile relay chunk exceeds authenticated BLE payload");
static_assert(kMobileRelayMaximumEnrollmentRequestBytes <=
                  companion::kMaximumEnvelopePayloadBytes,
              "enrollment request no longer fits the BLE relay bound");
static_assert(sizeof(KitsuMobileRelay) <= 512U,
              "mobile relay permanent state exceeded its RAM budget");

}  // namespace connectivity
}  // namespace kitsu868
