#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_enrollment.h"

namespace kitsu868 {
namespace connectivity {

constexpr char kGatewayBootstrapAlpn[] = "kitsu-bootstrap/1";
constexpr size_t kGatewayBootstrapSpkiBytes = 32U;
constexpr size_t kGatewayBootstrapMaximumCaBytes = 8192U;
constexpr size_t kGatewayBootstrapMaximumProxyRequestBytes = 16384U;
constexpr size_t kGatewayBootstrapMaximumProxyResponseBytes = 64U * 1024U;
constexpr size_t kGatewayBootstrapMaximumBackendResponseBytes = 32U * 1024U;

enum class GatewayBootstrapResult : uint8_t {
  ReconnectSteady = 0,
  InProgress,
  NotActive,
  InvalidArgument,
  RemoteConnectivityUnavailable,
  TimeUnavailable,
  TrustRejected,
  TransportFailed,
  ProxyMalformed,
  ProxyRejected,
  BackendMalformed,
  EnrollmentFailed,
};

const char* gatewayBootstrapResultName(GatewayBootstrapResult result);

// All fields come from the authenticated BLE gateway.configure operation and
// its encrypted, power-loss-safe store.  No insecure/no-verify mode exists.
struct GatewayBootstrapTrust {
  const char* host = nullptr;
  const char* serverName = nullptr;
  uint16_t port = 0U;
  const uint8_t* caCertificateDer = nullptr;
  size_t caCertificateBytes = 0U;
  uint8_t spkiSha256[kGatewayBootstrapSpkiBytes]{};
  uint8_t gatewayUuid[kEnrollmentUuidBytes]{};
};

struct GatewayBootstrapTlsEvidence {
  bool serverChainVerified = false;
  bool serverNameVerified = false;
  bool spkiMatched = false;
  bool alpnMatched = false;
  bool tlsVersionAtLeast12 = false;
  bool systemTimeChecked = false;
  bool systemTimeValid = false;
  bool plaintextFallbackUsed = false;
  bool redirectFollowed = false;
  // Bootstrap is server-authenticated TLS because a fresh device has no
  // certificate yet.  A client credential here indicates a confused path.
  bool clientCredentialPresented = false;
};

enum class GatewayBootstrapIoResult : uint8_t {
  Ok = 0,
  WouldBlock,
  Failed,
};

class GatewayBootstrapTransport {
 public:
  virtual ~GatewayBootstrapTransport() = default;

  // Sends one uint32-BE-length-prefixed request JSON and receives one such
  // response (hard maximum 64 KiB), using ALPN kitsu-bootstrap/1.  The method
  // must not follow redirects or fall back to plaintext.  It returns the
  // unframed response JSON only after TLS and frame validation.
  // Re-entrant, nonblocking exchange. The first call snapshots request/trust
  // state and starts a nonblocking TCP/TLS handshake; later calls advance at
  // most one bounded network step. Ok/Failed are terminal until close().
  virtual GatewayBootstrapIoResult exchangeOneFramedRequest(
      const GatewayBootstrapTrust& trust, const char* alpn,
      const uint8_t* requestJson, size_t requestBytes,
      uint8_t* responseJson, size_t responseCapacity, size_t& responseBytes,
      GatewayBootstrapTlsEvidence& evidence) = 0;
  virtual void close() = 0;
};

// Caller-owned/static workspace retains only the certificate material that
// must coexist while EnrollmentCredentialSink commits it. Request/response
// frames are checked transient allocations and are zeroed before free. This
// keeps the always-resident bootstrap cost near 20 KiB instead of the former
// ~144 KiB and never places a large frame on the Arduino loop stack.
struct GatewayBootstrapWorkspace {
  uint8_t leafCertificate[kEnrollmentMaximumCertificateBytes]{};
  uint8_t chainCertificates[kEnrollmentMaximumChainCertificates]
                           [kEnrollmentMaximumCertificateBytes]{};
};

constexpr size_t kGatewayBootstrapPermanentWorkspaceBytes =
    sizeof(GatewayBootstrapWorkspace);
// At the TLS exchange peak, the encoded request and maximum response coexist.
// mbedTLS allocations are separate and intentionally not hidden in this
// application-owned measurement.
constexpr size_t kGatewayBootstrapMaximumTransientBytes =
    kGatewayBootstrapMaximumProxyRequestBytes +
    kGatewayBootstrapMaximumProxyResponseBytes;
static_assert(kGatewayBootstrapPermanentWorkspaceBytes <= 20U * 1024U,
              "bootstrap permanent workspace exceeded its RAM budget");
static_assert(kGatewayBootstrapMaximumTransientBytes <= 80U * 1024U,
              "bootstrap transient frame budget unexpectedly grew");

// The companion parser is intentionally capped at 16 KiB.  Bootstrap has a
// separately frozen 64 KiB response bound, so its parser is separate rather
// than silently widening the BLE attack surface.
class GatewayBootstrapFrameParser {
 public:
  bool begin(uint8_t* storage, size_t storageBytes,
             uint32_t timeoutMs = 10000UL);
  companion::FrameResult feed(const uint8_t* input, size_t inputBytes,
                              uint32_t nowMillis);
  companion::FrameResult poll(uint32_t nowMillis) const;
  bool frame(const uint8_t*& output, size_t& outputBytes) const;
  void consume();

 private:
  uint8_t* storage_ = nullptr;
  size_t storageBytes_ = 0U;
  uint32_t timeoutMs_ = 0U;
  uint32_t startedAt_ = 0U;
  uint8_t header_[4]{};
  uint8_t headerBytes_ = 0U;
  size_t expectedBytes_ = 0U;
  size_t receivedBytes_ = 0U;
  bool started_ = false;
  bool ready_ = false;
};

// Exact frozen gateway proxy DTO codecs.  The request_b64/response_b64 fields
// bind the exact backend JSON bytes; the gateway never parses or receives the
// plaintext 32-byte backend-HMAC secret.
bool encodeGatewayBootstrapProxyRequest(
    const uint8_t enrollmentUuid[kEnrollmentUuidBytes],
    const uint8_t* backendRequest, size_t backendRequestBytes,
    uint8_t* output, size_t outputCapacity, size_t& outputBytes);

GatewayBootstrapResult decodeGatewayBootstrapProxyResponse(
    const uint8_t* json, size_t jsonBytes,
    const uint8_t expectedEnrollmentUuid[kEnrollmentUuidBytes],
    uint8_t* backendResponse, size_t backendResponseCapacity,
    size_t& backendResponseBytes);

// Parses the exact inner issuer success document, including strict nested
// sealed_secret and a 1..4 element DER issuer chain.  Pointers in output refer
// to workspace and remain valid until that workspace is cleared/reused.
GatewayBootstrapResult decodeBackendEnrollmentResponse(
    const uint8_t* json, size_t jsonBytes,
    const uint8_t enrollmentUuid[kEnrollmentUuidBytes],
    const uint8_t expectedGatewayUuid[kEnrollmentUuidBytes],
    GatewayBootstrapWorkspace& workspace, EnrollmentResponse& output);

class KitsuGatewayBootstrap {
 public:
  KitsuGatewayBootstrap();
  ~KitsuGatewayBootstrap();

  KitsuGatewayBootstrap(const KitsuGatewayBootstrap&) = delete;
  KitsuGatewayBootstrap& operator=(const KitsuGatewayBootstrap&) = delete;

  // Main-loop-safe orchestration: begin performs local bounded encoding only;
  // poll advances nonblocking network work and performs the final verified
  // credential commit on the caller thread.
  GatewayBootstrapResult beginExchangeAndInstall(
      const uint8_t enrollmentUuid[kEnrollmentUuidBytes],
      bool remoteConnectivityAllowed, const GatewayBootstrapTrust& trust,
      KitsuEnrollmentRecipient& recipient,
      GatewayBootstrapTransport& transport,
      EnrollmentCredentialSink& sink, GatewayBootstrapWorkspace& workspace);
  GatewayBootstrapResult pollExchangeAndInstall();
  void cancel();
  bool active() const;

 private:
  struct Implementation;
  Implementation* implementation_ = nullptr;
};

}  // namespace connectivity
}  // namespace kitsu868
