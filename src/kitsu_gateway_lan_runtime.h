#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_device_security.h"
#include "kitsu_enrollment.h"
#include "kitsu_lan_protocol.h"

namespace kitsu868 {
namespace connectivity {

constexpr char kGatewayLanAlpn[] = "kitsu-lan/1";
constexpr size_t kGatewayLanMaximumCaBytes = 8192U;
constexpr size_t kGatewayLanSpkiBytes = 32U;
constexpr size_t kGatewayLanMaximumCertificateBytes =
    kEnrollmentMaximumCertificateBytes;
constexpr size_t kGatewayLanQueueDepth = 4U;
constexpr size_t kGatewayLanMaximumQueuedBytes = 32U * 1024U;
constexpr uint32_t kGatewayLanConnectTimeoutMs = 10000UL;
constexpr uint32_t kGatewayLanIoTimeoutMs = 10000UL;
constexpr uint32_t kGatewayLanAckTimeoutMs = 15000UL;
constexpr uint32_t kGatewayLanInitialBackoffMs = 1000UL;
constexpr uint32_t kGatewayLanMaximumBackoffMs = 60000UL;
constexpr uint16_t kGatewayLanSequenceReservation = 16U;

// Immutable view borrowed from an encrypted credential store. The provider
// keeps every pointer valid until release() and must never source these bytes
// from NVS plaintext. A runtime snapshots only the small identity/HMAC fields
// needed after the TLS handshake; certificate and private-key views are not
// retained by the runtime.
struct GatewayLanCredentialView {
  const char* host = nullptr;
  const char* serverName = nullptr;
  uint16_t port = 0U;
  const uint8_t* caCertificateDer = nullptr;
  size_t caCertificateBytes = 0U;
  uint8_t spkiSha256[kGatewayLanSpkiBytes]{};

  uint8_t companionUuid[kLanUuidBytes]{};
  uint8_t gatewayUuid[kLanUuidBytes]{};
  uint32_t keyVersion = 0U;
  const uint8_t* privateKey = nullptr;
  size_t privateKeyBytes = 0U;
  const uint8_t* leafCertificateDer = nullptr;
  size_t leafCertificateBytes = 0U;
  const uint8_t* certificateChainDer[kEnrollmentMaximumChainCertificates]{};
  size_t certificateChainBytes[kEnrollmentMaximumChainCertificates]{};
  size_t certificateChainCount = 0U;
  const uint8_t* backendHmacSecret = nullptr;
  size_t backendHmacSecretBytes = 0U;
};

class GatewayLanCredentialProvider {
 public:
  virtual ~GatewayLanCredentialProvider() = default;
  virtual bool remoteConnectivityAllowed() const = 0;
  virtual bool acquire(GatewayLanCredentialView& output) = 0;
  virtual void release(GatewayLanCredentialView& view) = 0;
};

struct GatewayLanTlsEvidence {
  bool serverChainVerified = false;
  bool serverNameVerified = false;
  bool spkiMatched = false;
  bool alpnMatched = false;
  bool tlsVersionAtLeast12 = false;
  bool systemTimeValid = false;
  bool clientCredentialPresented = false;
  bool clientCertificateBindsCompanion = false;
  bool plaintextFallbackUsed = false;
  bool redirectFollowed = false;
};

enum class GatewayLanIoResult : uint8_t {
  Ok = 0,
  WouldBlock,
  Closed,
  TimedOut,
  TimeUnavailable,
  InvalidArgument,
  OutOfMemory,
  SecurityRejected,
  IoFailed,
};

// Owns exact received frame bytes until consumeReceivedFrame(). Implementors
// must hard-cap a uint32-BE frame at kLanMaximumFrameBytes, reject zero, apply
// an assembly timeout, and wipe frame storage on consume/close.
class GatewayLanTlsTransport {
 public:
  virtual ~GatewayLanTlsTransport() = default;
  // beginConnect() borrows credentials only for this call. Implementors must
  // parse or copy every value needed by later handshake steps before it
  // returns; they may not retain any pointer from the credential view.
  // A WouldBlock result starts exactly one in-flight connection, which the
  // caller advances only through pollConnect() until a terminal result or
  // close(). This prevents an Arduino loop from repeatedly decrypting the
  // large credential record while TCP/TLS is still progressing.
  virtual GatewayLanIoResult beginConnect(
      const GatewayLanCredentialView& credentials, const char* alpn,
      uint32_t timeoutMs, GatewayLanTlsEvidence& evidence) = 0;
  virtual GatewayLanIoResult pollConnect(
      GatewayLanTlsEvidence& evidence) = 0;
  virtual GatewayLanIoResult writeOneFrame(const uint8_t* frame,
                                           size_t frameBytes,
                                           uint32_t timeoutMs) = 0;
  virtual GatewayLanIoResult receiveOneFrame(const uint8_t*& frame,
                                             size_t& frameBytes,
                                             uint32_t nowMillis,
                                             uint32_t timeoutMs) = 0;
  virtual void consumeReceivedFrame() = 0;
  virtual bool connected() const = 0;
  virtual void close() = 0;
};

// Narrow adapter makes the durable sequence contract host-testable while the
// firmware adapter below calls the exact KitsuDeviceSecurity APIs.
class GatewayLanSequenceStore {
 public:
  virtual ~GatewayLanSequenceStore() = default;
  virtual bool remoteConnectivityAllowed() const = 0;
  virtual bool reserveTx(uint16_t blockSize, uint64_t& first,
                         uint64_t& last) = 0;
  virtual bool acceptNextRx(uint64_t sequence) = 0;
  virtual uint64_t rxHighWater() const = 0;
};

class KitsuDeviceSecurityLanSequenceStore final
    : public GatewayLanSequenceStore {
 public:
  explicit KitsuDeviceSecurityLanSequenceStore(KitsuDeviceSecurity& security)
      : security_(security) {}
  bool remoteConnectivityAllowed() const override;
  bool reserveTx(uint16_t blockSize, uint64_t& first,
                 uint64_t& last) override;
  bool acceptNextRx(uint64_t sequence) override;
  uint64_t rxHighWater() const override;

 private:
  KitsuDeviceSecurity& security_;
};

class GatewayLanActionSink {
 public:
  virtual ~GatewayLanActionSink() = default;
  // framedJson is the exact authenticated byte sequence received from the
  // gateway. No parsed/re-serialized substitute is ever delivered. This
  // fresh callback is made only after the replay ID is durably reserved.
  virtual bool acceptAuthenticatedAction(
      const uint8_t* framedJson, size_t framedJsonBytes,
      const LanGatewayFrame& metadata, const uint8_t* paramsJson,
      size_t paramsJsonBytes, int64_t acceptedEpoch) = 0;

  // A signed duplicate is never sent through the execution callback. A
  // deployed sink may use its durable outcome ledger to re-emit the exact
  // logical acceptance/result after an interrupted upload. It must never
  // execute the side effect again.
  virtual bool repeatAuthenticatedAction(
      const uint8_t* framedJson, size_t framedJsonBytes,
      const LanGatewayFrame& metadata, const uint8_t* paramsJson,
      size_t paramsJsonBytes, int64_t repeatedEpoch) = 0;
};

enum class GatewayLanRuntimeResult : uint8_t {
  Ok = 0,
  NotBegun,
  InvalidArgument,
  RemoteConnectivityUnavailable,
  TimeUnavailable,
  CredentialsUnavailable,
  CredentialsInvalid,
  CredentialChanged,
  QueueFull,
  OutOfMemory,
  SequenceStoreFailed,
  CryptoFailed,
  TransportFailed,
  TrustRejected,
  AckTimedOut,
  UnexpectedAck,
  GatewayFrameRejected,
  ActionStoreFailed,
  ActionSinkFailed,
};

const char* gatewayLanRuntimeResultName(GatewayLanRuntimeResult result);

struct GatewayLanRuntimeStatus {
  GatewayLanRuntimeResult lastResult = GatewayLanRuntimeResult::NotBegun;
  bool begun = false;
  bool connected = false;
  bool waitingForAck = false;
  size_t queuedFrames = 0U;
  size_t queuedBytes = 0U;
  uint32_t consecutiveFailures = 0U;
  uint32_t nextAttemptMillis = 0U;
  uint64_t lastAckedSequence = 0U;
  uint64_t lastSpoolRecordId = 0U;
  uint64_t acceptedActionCount = 0U;
};

// Single-threaded steady-LAN state machine. poll() never queues unbounded work:
// one frame may await an ACK and at most four exact envelopes/32 KiB are held.
// Reconnects retry the byte-identical signed envelope and use bounded
// exponential backoff. No log/body API exists here by design.
class KitsuGatewayLanRuntime {
 public:
  KitsuGatewayLanRuntime();
  ~KitsuGatewayLanRuntime();

  KitsuGatewayLanRuntime(const KitsuGatewayLanRuntime&) = delete;
  KitsuGatewayLanRuntime& operator=(const KitsuGatewayLanRuntime&) = delete;

  GatewayLanRuntimeResult begin(GatewayLanCredentialProvider& credentials,
                                GatewayLanSequenceStore& sequences,
                                companion::CompanionCrypto& crypto,
                                LanActionReplayStore& replayStore,
                                GatewayLanActionSink& actionSink,
                                GatewayLanTlsTransport& transport);
  void stop();

  GatewayLanRuntimeResult enqueueDevicePayload(
      const char* payloadType, const uint8_t* payload, size_t payloadBytes,
      int64_t issuedEpoch, uint64_t* assignedSequence = nullptr);

  GatewayLanRuntimeResult poll(uint32_t nowMillis, int64_t nowEpoch,
                               bool clockValid);
  GatewayLanRuntimeStatus status() const;

 private:
  struct QueuedFrame {
    uint8_t* bytes = nullptr;
    size_t frameBytes = 0U;
    uint64_t sequence = 0U;
    uint8_t companionUuid[kLanUuidBytes]{};
    uint8_t gatewayUuid[kLanUuidBytes]{};
    uint32_t keyVersion = 0U;
    bool sentOnConnection = false;
    uint32_t sentAtMillis = 0U;
  };

  struct SessionIdentity {
    uint8_t companionUuid[kLanUuidBytes]{};
    uint8_t gatewayUuid[kLanUuidBytes]{};
    uint32_t keyVersion = 0U;
    uint8_t backendHmacSecret[32]{};
    bool valid = false;
  };

  GatewayLanRuntimeResult setResult(GatewayLanRuntimeResult result);
  GatewayLanRuntimeResult ensureConnected(uint32_t nowMillis);
  GatewayLanRuntimeResult handleFrame(const uint8_t* frame,
                                      size_t frameBytes, int64_t nowEpoch,
                                      bool clockValid, uint32_t nowMillis);
  void connectionFailed(GatewayLanRuntimeResult result,
                        uint32_t nowMillis);
  void closeConnection();
  void clearQueue();
  void popQueue();
  bool reserveSequence(uint64_t& output);
  static bool identityMatches(const SessionIdentity& identity,
                              const GatewayLanCredentialView& view);
  static void snapshotIdentity(const GatewayLanCredentialView& view,
                               SessionIdentity& identity);
  static bool validCredentials(const GatewayLanCredentialView& view);
  static bool evidenceAccepted(const GatewayLanTlsEvidence& evidence);

  GatewayLanCredentialProvider* credentials_ = nullptr;
  GatewayLanSequenceStore* sequences_ = nullptr;
  companion::CompanionCrypto* crypto_ = nullptr;
  LanActionReplayStore* replayStore_ = nullptr;
  GatewayLanActionSink* actionSink_ = nullptr;
  GatewayLanTlsTransport* transport_ = nullptr;
  GatewayLanRuntimeStatus status_{};
  QueuedFrame queue_[kGatewayLanQueueDepth]{};
  SessionIdentity session_{};
  SessionIdentity pendingSession_{};
  bool connectPending_ = false;
  uint64_t nextTxSequence_ = 0U;
  uint64_t lastTxSequence_ = 0U;
  uint32_t backoffMs_ = kGatewayLanInitialBackoffMs;
};

static_assert(sizeof(KitsuGatewayLanRuntime) <= 1024U,
              "steady LAN runtime permanent RAM budget exceeded");

}  // namespace connectivity
}  // namespace kitsu868
