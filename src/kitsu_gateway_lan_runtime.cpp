#include "kitsu_gateway_lan_runtime.h"

#include <stdlib.h>
#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

bool allZero(const uint8_t* input, size_t bytes) {
  if (!input) return true;
  uint8_t combined = 0U;
  for (size_t i = 0U; i < bytes; ++i) combined |= input[i];
  return combined == 0U;
}

bool validFqdn(const char* value) {
  if (!value) return false;
  const size_t bytes = strlen(value);
  if (bytes == 0U || bytes > 253U || value[0] == '.' ||
      value[bytes - 1U] == '.') {
    return false;
  }
  size_t labelBytes = 0U;
  bool hasDot = false;
  bool hasNonNumericLabelByte = false;
  for (size_t i = 0U; i < bytes; ++i) {
    const char c = value[i];
    if (c == '.') {
      if (labelBytes == 0U || labelBytes > 63U || value[i - 1U] == '-') {
        return false;
      }
      labelBytes = 0U;
      hasDot = true;
      continue;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-')) {
      return false;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-') {
      hasNonNumericLabelByte = true;
    }
    if (labelBytes == 0U && c == '-') return false;
    ++labelBytes;
  }
  return hasDot && hasNonNumericLabelByte && labelBytes != 0U &&
         labelBytes <= 63U &&
         value[bytes - 1U] != '-';
}

bool validIpv4(const char* value) {
  if (!value) return false;
  const size_t bytes = strlen(value);
  if (bytes < 7U || bytes > 15U) return false;
  size_t start = 0U;
  uint8_t groups = 0U;
  for (size_t i = 0U; i <= bytes; ++i) {
    if (i != bytes && value[i] != '.') continue;
    const size_t count = i - start;
    if (count == 0U || count > 3U ||
        (count > 1U && value[start] == '0')) {
      return false;
    }
    uint16_t part = 0U;
    for (size_t j = start; j < i; ++j) {
      if (value[j] < '0' || value[j] > '9') return false;
      part = static_cast<uint16_t>(part * 10U + value[j] - '0');
    }
    if (part > 255U) return false;
    ++groups;
    start = i + 1U;
  }
  return groups == 4U;
}

bool hexDigit(char value) {
  return (value >= '0' && value <= '9') ||
         (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

bool validIpv6(const char* value) {
  if (!value) return false;
  const size_t bytes = strlen(value);
  if (bytes < 2U || bytes > 45U) return false;
  bool compressed = false;
  for (size_t i = 0U; i + 1U < bytes; ++i) {
    if (i + 2U < bytes && value[i] == ':' && value[i + 1U] == ':' &&
        value[i + 2U] == ':') return false;
    if (value[i] == ':' && value[i + 1U] == ':') {
      if (compressed) return false;
      compressed = true;
      ++i;
    }
  }
  if ((value[0] == ':' && value[1] != ':') ||
      (value[bytes - 1U] == ':' && value[bytes - 2U] != ':')) {
    return false;
  }
  uint8_t groups = 0U;
  size_t start = 0U;
  while (start < bytes) {
    if (value[start] == ':') {
      ++start;
      continue;
    }
    size_t end = start;
    while (end < bytes && value[end] != ':') ++end;
    bool dotted = false;
    for (size_t i = start; i < end; ++i) dotted = dotted || value[i] == '.';
    if (dotted) {
      char embedded[16]{};
      if (end != bytes || end - start >= sizeof(embedded)) return false;
      memcpy(embedded, value + start, end - start);
      if (!validIpv4(embedded)) return false;
      groups = static_cast<uint8_t>(groups + 2U);
    } else {
      if (end - start == 0U || end - start > 4U) return false;
      for (size_t i = start; i < end; ++i) {
        if (!hexDigit(value[i])) return false;
      }
      ++groups;
    }
    start = end + 1U;
  }
  return compressed ? groups < 8U : groups == 8U;
}

bool validEndpointHost(const char* value) {
  return validIpv4(value) || validIpv6(value) || validFqdn(value);
}

bool timeReached(uint32_t now, uint32_t target) {
  return static_cast<int32_t>(now - target) >= 0;
}

}  // namespace

const char* gatewayLanRuntimeResultName(GatewayLanRuntimeResult result) {
  switch (result) {
    case GatewayLanRuntimeResult::Ok: return "ok";
    case GatewayLanRuntimeResult::NotBegun: return "not_begun";
    case GatewayLanRuntimeResult::InvalidArgument: return "invalid_argument";
    case GatewayLanRuntimeResult::RemoteConnectivityUnavailable:
      return "remote_connectivity_unavailable";
    case GatewayLanRuntimeResult::TimeUnavailable:
      return "time_unavailable";
    case GatewayLanRuntimeResult::CredentialsUnavailable:
      return "credentials_unavailable";
    case GatewayLanRuntimeResult::CredentialsInvalid:
      return "credentials_invalid";
    case GatewayLanRuntimeResult::CredentialChanged:
      return "credential_changed";
    case GatewayLanRuntimeResult::QueueFull: return "queue_full";
    case GatewayLanRuntimeResult::OutOfMemory: return "out_of_memory";
    case GatewayLanRuntimeResult::SequenceStoreFailed:
      return "sequence_store_failed";
    case GatewayLanRuntimeResult::CryptoFailed: return "crypto_failed";
    case GatewayLanRuntimeResult::TransportFailed: return "transport_failed";
    case GatewayLanRuntimeResult::TrustRejected: return "trust_rejected";
    case GatewayLanRuntimeResult::AckTimedOut: return "ack_timed_out";
    case GatewayLanRuntimeResult::UnexpectedAck: return "unexpected_ack";
    case GatewayLanRuntimeResult::GatewayFrameRejected:
      return "gateway_frame_rejected";
    case GatewayLanRuntimeResult::ActionStoreFailed:
      return "action_store_failed";
    case GatewayLanRuntimeResult::ActionSinkFailed:
      return "action_sink_failed";
  }
  return "invalid_argument";
}

bool KitsuDeviceSecurityLanSequenceStore::remoteConnectivityAllowed() const {
  return security_.remoteConnectivityAllowed();
}

bool KitsuDeviceSecurityLanSequenceStore::reserveTx(uint16_t blockSize,
                                                     uint64_t& first,
                                                     uint64_t& last) {
  return security_.reserveLanTxSequenceBlock(blockSize, first, last) ==
         SecurityResult::Ok;
}

bool KitsuDeviceSecurityLanSequenceStore::acceptNextRx(uint64_t sequence) {
  return security_.acceptLanRxSequence(sequence) == SecurityResult::Ok;
}

uint64_t KitsuDeviceSecurityLanSequenceStore::rxHighWater() const {
  return security_.status().lanRxHighWater;
}

KitsuGatewayLanRuntime::KitsuGatewayLanRuntime() = default;

KitsuGatewayLanRuntime::~KitsuGatewayLanRuntime() { stop(); }

GatewayLanRuntimeResult KitsuGatewayLanRuntime::setResult(
    GatewayLanRuntimeResult result) {
  status_.lastResult = result;
  return result;
}

GatewayLanRuntimeResult KitsuGatewayLanRuntime::begin(
    GatewayLanCredentialProvider& credentials,
    GatewayLanSequenceStore& sequences, companion::CompanionCrypto& crypto,
    LanActionReplayStore& replayStore, GatewayLanActionSink& actionSink,
    GatewayLanTlsTransport& transport) {
  stop();
  credentials_ = &credentials;
  sequences_ = &sequences;
  crypto_ = &crypto;
  replayStore_ = &replayStore;
  actionSink_ = &actionSink;
  transport_ = &transport;
  status_ = GatewayLanRuntimeStatus{};
  status_.begun = true;
  status_.lastResult = GatewayLanRuntimeResult::Ok;
  backoffMs_ = kGatewayLanInitialBackoffMs;
  if (!credentials.remoteConnectivityAllowed() ||
      !sequences.remoteConnectivityAllowed()) {
    return setResult(GatewayLanRuntimeResult::RemoteConnectivityUnavailable);
  }
  return GatewayLanRuntimeResult::Ok;
}

void KitsuGatewayLanRuntime::stop() {
  if (transport_) transport_->close();
  clearQueue();
  secureZero(&session_, sizeof(session_));
  secureZero(&pendingSession_, sizeof(pendingSession_));
  connectPending_ = false;
  credentials_ = nullptr;
  sequences_ = nullptr;
  crypto_ = nullptr;
  replayStore_ = nullptr;
  actionSink_ = nullptr;
  transport_ = nullptr;
  nextTxSequence_ = 0U;
  lastTxSequence_ = 0U;
  backoffMs_ = kGatewayLanInitialBackoffMs;
  status_ = GatewayLanRuntimeStatus{};
}

bool KitsuGatewayLanRuntime::validCredentials(
    const GatewayLanCredentialView& view) {
  if (!validEndpointHost(view.host) || !validFqdn(view.serverName) ||
      view.port == 0U || !view.caCertificateDer ||
      view.caCertificateBytes == 0U ||
      view.caCertificateBytes > kGatewayLanMaximumCaBytes ||
      allZero(view.spkiSha256, sizeof(view.spkiSha256)) ||
      allZero(view.companionUuid, sizeof(view.companionUuid)) ||
      allZero(view.gatewayUuid, sizeof(view.gatewayUuid)) ||
      view.keyVersion == 0U || !view.privateKey ||
      view.privateKeyBytes != kEnrollmentPrivateKeyBytes ||
      allZero(view.privateKey, view.privateKeyBytes) ||
      !view.leafCertificateDer || view.leafCertificateBytes == 0U ||
      view.leafCertificateBytes > kGatewayLanMaximumCertificateBytes ||
      view.certificateChainCount == 0U ||
      view.certificateChainCount > kEnrollmentMaximumChainCertificates ||
      !view.backendHmacSecret ||
      view.backendHmacSecretBytes != kEnrollmentSecretBytes ||
      allZero(view.backendHmacSecret, view.backendHmacSecretBytes)) {
    return false;
  }
  for (size_t i = 0U; i < view.certificateChainCount; ++i) {
    if (!view.certificateChainDer[i] ||
        view.certificateChainBytes[i] == 0U ||
        view.certificateChainBytes[i] > kGatewayLanMaximumCertificateBytes) {
      return false;
    }
  }
  return true;
}

bool KitsuGatewayLanRuntime::evidenceAccepted(
    const GatewayLanTlsEvidence& evidence) {
  return evidence.serverChainVerified && evidence.serverNameVerified &&
         evidence.spkiMatched && evidence.alpnMatched &&
         evidence.tlsVersionAtLeast12 && evidence.systemTimeValid &&
         evidence.clientCredentialPresented &&
         evidence.clientCertificateBindsCompanion &&
         !evidence.plaintextFallbackUsed && !evidence.redirectFollowed;
}

bool KitsuGatewayLanRuntime::identityMatches(
    const SessionIdentity& identity,
    const GatewayLanCredentialView& view) {
  return identity.valid && identity.keyVersion == view.keyVersion &&
         memcmp(identity.companionUuid, view.companionUuid,
                kLanUuidBytes) == 0 &&
         memcmp(identity.gatewayUuid, view.gatewayUuid,
                kLanUuidBytes) == 0 &&
         memcmp(identity.backendHmacSecret, view.backendHmacSecret,
                sizeof(identity.backendHmacSecret)) == 0;
}

void KitsuGatewayLanRuntime::snapshotIdentity(
    const GatewayLanCredentialView& view, SessionIdentity& identity) {
  secureZero(&identity, sizeof(identity));
  memcpy(identity.companionUuid, view.companionUuid, kLanUuidBytes);
  memcpy(identity.gatewayUuid, view.gatewayUuid, kLanUuidBytes);
  memcpy(identity.backendHmacSecret, view.backendHmacSecret,
         sizeof(identity.backendHmacSecret));
  identity.keyVersion = view.keyVersion;
  identity.valid = true;
}

bool KitsuGatewayLanRuntime::reserveSequence(uint64_t& output) {
  output = 0U;
  if (nextTxSequence_ == 0U || nextTxSequence_ > lastTxSequence_) {
    uint64_t first = 0U;
    uint64_t last = 0U;
    if (!sequences_ ||
        !sequences_->reserveTx(kGatewayLanSequenceReservation, first, last) ||
        first == 0U || last < first ||
        last > 0x7fffffffffffffffULL) {
      nextTxSequence_ = 0U;
      lastTxSequence_ = 0U;
      return false;
    }
    nextTxSequence_ = first;
    lastTxSequence_ = last;
  }
  output = nextTxSequence_++;
  return output != 0U && output <= 0x7fffffffffffffffULL;
}

GatewayLanRuntimeResult KitsuGatewayLanRuntime::enqueueDevicePayload(
    const char* payloadType, const uint8_t* payload, size_t payloadBytes,
    int64_t issuedEpoch, uint64_t* assignedSequence) {
  if (assignedSequence) *assignedSequence = 0U;
  if (!status_.begun || !credentials_ || !sequences_ || !crypto_) {
    return setResult(GatewayLanRuntimeResult::NotBegun);
  }
  if (!payloadType || !payload || payloadBytes == 0U ||
      payloadBytes > kLanMaximumDevicePayloadBytes) {
    return setResult(GatewayLanRuntimeResult::InvalidArgument);
  }
  if (!credentials_->remoteConnectivityAllowed() ||
      !sequences_->remoteConnectivityAllowed()) {
    closeConnection();
    return setResult(GatewayLanRuntimeResult::RemoteConnectivityUnavailable);
  }
  if (status_.queuedFrames >= kGatewayLanQueueDepth) {
    return setResult(GatewayLanRuntimeResult::QueueFull);
  }

  GatewayLanCredentialView view{};
  if (!credentials_->acquire(view)) {
    return setResult(GatewayLanRuntimeResult::CredentialsUnavailable);
  }
  GatewayLanRuntimeResult result = GatewayLanRuntimeResult::Ok;
  if (!validCredentials(view)) {
    result = GatewayLanRuntimeResult::CredentialsInvalid;
  } else if (status_.queuedFrames != 0U &&
             (queue_[0].keyVersion != view.keyVersion ||
              memcmp(queue_[0].companionUuid, view.companionUuid,
                     kLanUuidBytes) != 0 ||
              memcmp(queue_[0].gatewayUuid, view.gatewayUuid,
                     kLanUuidBytes) != 0)) {
    result = GatewayLanRuntimeResult::CredentialChanged;
  } else if ((session_.valid && !identityMatches(session_, view)) ||
             (pendingSession_.valid &&
              !identityMatches(pendingSession_, view))) {
    result = GatewayLanRuntimeResult::CredentialChanged;
  }
  if (result != GatewayLanRuntimeResult::Ok) {
    credentials_->release(view);
    return setResult(result);
  }

  uint64_t sequence = 0U;
  if (!reserveSequence(sequence)) {
    credentials_->release(view);
    return setResult(GatewayLanRuntimeResult::SequenceStoreFailed);
  }
  uint8_t nonce[kLanNonceBytes]{};
  uint8_t requestId[kLanUuidBytes]{};
  if (!crypto_->randomBytes(nonce, sizeof(nonce)) ||
      !crypto_->randomBytes(requestId, sizeof(requestId))) {
    secureZero(nonce, sizeof(nonce));
    secureZero(requestId, sizeof(requestId));
    credentials_->release(view);
    return setResult(GatewayLanRuntimeResult::CryptoFailed);
  }
  // Canonical non-nil RFC 4122 UUID text is emitted by the codec. Set the
  // random UUID version/variant bits so downstream UUID parsers accept it.
  requestId[6] = static_cast<uint8_t>((requestId[6] & 0x0fU) | 0x40U);
  requestId[8] = static_cast<uint8_t>((requestId[8] & 0x3fU) | 0x80U);

  uint8_t* scratch = static_cast<uint8_t*>(malloc(kLanMaximumFrameBytes));
  if (!scratch) {
    secureZero(nonce, sizeof(nonce));
    secureZero(requestId, sizeof(requestId));
    credentials_->release(view);
    return setResult(GatewayLanRuntimeResult::OutOfMemory);
  }
  memset(scratch, 0, kLanMaximumFrameBytes);
  size_t frameBytes = 0U;
  const LanResult encoded = encodeDeviceEnvelope(
      view.companionUuid, view.gatewayUuid, sequence, issuedEpoch, nonce,
      requestId, view.keyVersion, payloadType, payload, payloadBytes,
      view.backendHmacSecret, *crypto_, scratch, kLanMaximumFrameBytes,
      frameBytes);
  secureZero(nonce, sizeof(nonce));
  secureZero(requestId, sizeof(requestId));
  if (encoded != LanResult::Ok || frameBytes == 0U ||
      frameBytes > kLanMaximumFrameBytes) {
    secureZero(scratch, kLanMaximumFrameBytes);
    free(scratch);
    credentials_->release(view);
    return setResult(encoded == LanResult::CryptoFailed
                         ? GatewayLanRuntimeResult::CryptoFailed
                         : GatewayLanRuntimeResult::InvalidArgument);
  }
  if (status_.queuedBytes + frameBytes > kGatewayLanMaximumQueuedBytes) {
    secureZero(scratch, kLanMaximumFrameBytes);
    free(scratch);
    credentials_->release(view);
    return setResult(GatewayLanRuntimeResult::QueueFull);
  }
  uint8_t* exact = static_cast<uint8_t*>(malloc(frameBytes));
  if (!exact) {
    secureZero(scratch, kLanMaximumFrameBytes);
    free(scratch);
    credentials_->release(view);
    return setResult(GatewayLanRuntimeResult::OutOfMemory);
  }
  memcpy(exact, scratch, frameBytes);
  secureZero(scratch, kLanMaximumFrameBytes);
  free(scratch);

  QueuedFrame& queued = queue_[status_.queuedFrames];
  queued.bytes = exact;
  queued.frameBytes = frameBytes;
  queued.sequence = sequence;
  queued.keyVersion = view.keyVersion;
  memcpy(queued.companionUuid, view.companionUuid, kLanUuidBytes);
  memcpy(queued.gatewayUuid, view.gatewayUuid, kLanUuidBytes);
  ++status_.queuedFrames;
  status_.queuedBytes += frameBytes;
  if (assignedSequence) *assignedSequence = sequence;
  credentials_->release(view);
  return setResult(GatewayLanRuntimeResult::Ok);
}

GatewayLanRuntimeResult KitsuGatewayLanRuntime::ensureConnected(
    uint32_t nowMillis) {
  if (status_.connected && transport_ && transport_->connected()) {
    return GatewayLanRuntimeResult::Ok;
  }
  if (connectPending_) {
    GatewayLanTlsEvidence evidence{};
    const GatewayLanIoResult connected = transport_->pollConnect(evidence);
    if (connected == GatewayLanIoResult::WouldBlock) {
      return GatewayLanRuntimeResult::Ok;
    }
    if (connected != GatewayLanIoResult::Ok ||
        !evidenceAccepted(evidence)) {
      connectionFailed(connected == GatewayLanIoResult::TimeUnavailable
                           ? GatewayLanRuntimeResult::TimeUnavailable
                           : connected == GatewayLanIoResult::Ok
                                 ? GatewayLanRuntimeResult::TrustRejected
                                 : GatewayLanRuntimeResult::TransportFailed,
                       nowMillis);
      return status_.lastResult;
    }
    secureZero(&session_, sizeof(session_));
    session_ = pendingSession_;
    secureZero(&pendingSession_, sizeof(pendingSession_));
    connectPending_ = false;
  } else {
    if (!timeReached(nowMillis, status_.nextAttemptMillis)) {
      return GatewayLanRuntimeResult::Ok;
    }
    GatewayLanCredentialView view{};
    if (!credentials_->acquire(view)) {
      connectionFailed(GatewayLanRuntimeResult::CredentialsUnavailable,
                       nowMillis);
      return GatewayLanRuntimeResult::CredentialsUnavailable;
    }
    if (!validCredentials(view)) {
      credentials_->release(view);
      connectionFailed(GatewayLanRuntimeResult::CredentialsInvalid, nowMillis);
      return GatewayLanRuntimeResult::CredentialsInvalid;
    }
    if (status_.queuedFrames != 0U &&
        (queue_[0].keyVersion != view.keyVersion ||
         memcmp(queue_[0].companionUuid, view.companionUuid,
                kLanUuidBytes) != 0 ||
         memcmp(queue_[0].gatewayUuid, view.gatewayUuid,
                kLanUuidBytes) != 0)) {
      credentials_->release(view);
      clearQueue();
      connectionFailed(GatewayLanRuntimeResult::CredentialChanged, nowMillis);
      return GatewayLanRuntimeResult::CredentialChanged;
    }

    GatewayLanTlsEvidence evidence{};
    const GatewayLanIoResult connected = transport_->beginConnect(
        view, kGatewayLanAlpn, kGatewayLanConnectTimeoutMs, evidence);
    if (connected == GatewayLanIoResult::WouldBlock) {
      snapshotIdentity(view, pendingSession_);
      connectPending_ = true;
      credentials_->release(view);
      return GatewayLanRuntimeResult::Ok;
    }
    if (connected != GatewayLanIoResult::Ok ||
        !evidenceAccepted(evidence)) {
      credentials_->release(view);
      connectionFailed(connected == GatewayLanIoResult::TimeUnavailable
                           ? GatewayLanRuntimeResult::TimeUnavailable
                           : connected == GatewayLanIoResult::Ok
                                 ? GatewayLanRuntimeResult::TrustRejected
                                 : GatewayLanRuntimeResult::TransportFailed,
                       nowMillis);
      return status_.lastResult;
    }
    snapshotIdentity(view, session_);
    credentials_->release(view);
  }
  status_.connected = true;
  status_.waitingForAck = false;
  status_.consecutiveFailures = 0U;
  status_.nextAttemptMillis = nowMillis;
  backoffMs_ = kGatewayLanInitialBackoffMs;
  for (size_t i = 0U; i < status_.queuedFrames; ++i) {
    queue_[i].sentOnConnection = false;
    queue_[i].sentAtMillis = 0U;
  }
  return setResult(GatewayLanRuntimeResult::Ok);
}

void KitsuGatewayLanRuntime::connectionFailed(
    GatewayLanRuntimeResult result, uint32_t nowMillis) {
  closeConnection();
  if (status_.consecutiveFailures != UINT32_MAX) {
    ++status_.consecutiveFailures;
  }
  status_.nextAttemptMillis = nowMillis + backoffMs_;
  if (backoffMs_ < kGatewayLanMaximumBackoffMs) {
    const uint32_t doubled = backoffMs_ > kGatewayLanMaximumBackoffMs / 2U
        ? kGatewayLanMaximumBackoffMs
        : backoffMs_ * 2U;
    backoffMs_ = doubled;
  }
  setResult(result);
}

void KitsuGatewayLanRuntime::closeConnection() {
  if (transport_) transport_->close();
  connectPending_ = false;
  status_.connected = false;
  status_.waitingForAck = false;
  for (size_t i = 0U; i < status_.queuedFrames; ++i) {
    queue_[i].sentOnConnection = false;
    queue_[i].sentAtMillis = 0U;
  }
  secureZero(&session_, sizeof(session_));
  secureZero(&pendingSession_, sizeof(pendingSession_));
}

void KitsuGatewayLanRuntime::popQueue() {
  if (status_.queuedFrames == 0U) return;
  if (queue_[0].bytes) {
    secureZero(queue_[0].bytes, queue_[0].frameBytes);
    free(queue_[0].bytes);
  }
  if (status_.queuedBytes >= queue_[0].frameBytes) {
    status_.queuedBytes -= queue_[0].frameBytes;
  } else {
    status_.queuedBytes = 0U;
  }
  for (size_t i = 1U; i < status_.queuedFrames; ++i) {
    queue_[i - 1U] = queue_[i];
  }
  --status_.queuedFrames;
  secureZero(&queue_[status_.queuedFrames], sizeof(QueuedFrame));
  status_.waitingForAck = false;
}

void KitsuGatewayLanRuntime::clearQueue() {
  while (status_.queuedFrames != 0U) popQueue();
  status_.queuedBytes = 0U;
}

GatewayLanRuntimeResult KitsuGatewayLanRuntime::handleFrame(
    const uint8_t* frame, size_t frameBytes, int64_t nowEpoch,
    bool clockValid, uint32_t nowMillis) {
  if (!frame || frameBytes == 0U || frameBytes > kLanMaximumFrameBytes ||
      !session_.valid) {
    connectionFailed(GatewayLanRuntimeResult::GatewayFrameRejected,
                     nowMillis);
    return GatewayLanRuntimeResult::GatewayFrameRejected;
  }
  uint8_t* params =
      static_cast<uint8_t*>(malloc(kLanMaximumActionParamsBytes));
  if (!params) {
    connectionFailed(GatewayLanRuntimeResult::OutOfMemory, nowMillis);
    return GatewayLanRuntimeResult::OutOfMemory;
  }
  memset(params, 0, kLanMaximumActionParamsBytes);
  LanGatewayFrame decoded{};
  const LanResult result = decodeGatewayFrame(
      frame, frameBytes, session_.companionUuid, session_.keyVersion,
      nowEpoch, clockValid, session_.backendHmacSecret, *crypto_,
      *replayStore_, decoded, params, kLanMaximumActionParamsBytes);

  GatewayLanRuntimeResult runtimeResult = GatewayLanRuntimeResult::Ok;
  if (result == LanResult::GatewayAck) {
    if (status_.queuedFrames == 0U ||
        !queue_[0].sentOnConnection ||
        decoded.deviceSequence != queue_[0].sequence) {
      runtimeResult = GatewayLanRuntimeResult::UnexpectedAck;
    } else {
      status_.lastAckedSequence = decoded.deviceSequence;
      status_.lastSpoolRecordId = decoded.spoolRecordId;
      popQueue();
    }
  } else if (result == LanResult::ActionFresh) {
    const uint64_t highWater = sequences_->rxHighWater();
    if (highWater == UINT64_MAX ||
        !sequences_->acceptNextRx(highWater + 1U)) {
      runtimeResult = GatewayLanRuntimeResult::SequenceStoreFailed;
    } else if (!actionSink_->acceptAuthenticatedAction(
                   frame, frameBytes, decoded, params,
                   decoded.parameterBytes, nowEpoch)) {
      runtimeResult = GatewayLanRuntimeResult::ActionSinkFailed;
    } else {
      status_.acceptedActionCount = highWater + 1U;
    }
  } else if (result == LanResult::ActionDuplicate) {
    // Never execute a duplicate. The separate repeat callback exists only so
    // a durable sink can re-emit a previously committed result after an
    // interrupted outbound upload.
    if (!actionSink_->repeatAuthenticatedAction(
            frame, frameBytes, decoded, params, decoded.parameterBytes,
            nowEpoch)) {
      runtimeResult = GatewayLanRuntimeResult::ActionSinkFailed;
    }
  } else if (result == LanResult::ReplayStoreFailed) {
    runtimeResult = GatewayLanRuntimeResult::ActionStoreFailed;
  } else {
    runtimeResult = GatewayLanRuntimeResult::GatewayFrameRejected;
  }
  secureZero(params, kLanMaximumActionParamsBytes);
  free(params);
  if (runtimeResult != GatewayLanRuntimeResult::Ok) {
    connectionFailed(runtimeResult, nowMillis);
    return runtimeResult;
  }
  return setResult(GatewayLanRuntimeResult::Ok);
}

GatewayLanRuntimeResult KitsuGatewayLanRuntime::poll(
    uint32_t nowMillis, int64_t nowEpoch, bool clockValid) {
  if (!status_.begun || !credentials_ || !sequences_ || !crypto_ ||
      !replayStore_ || !actionSink_ || !transport_) {
    return setResult(GatewayLanRuntimeResult::NotBegun);
  }
  if (!credentials_->remoteConnectivityAllowed() ||
      !sequences_->remoteConnectivityAllowed()) {
    closeConnection();
    return setResult(GatewayLanRuntimeResult::RemoteConnectivityUnavailable);
  }
  const GatewayLanRuntimeResult connection = ensureConnected(nowMillis);
  if (connection != GatewayLanRuntimeResult::Ok || !status_.connected) {
    return connection;
  }
  if (!transport_->connected()) {
    connectionFailed(GatewayLanRuntimeResult::TransportFailed, nowMillis);
    return GatewayLanRuntimeResult::TransportFailed;
  }

  if (status_.queuedFrames != 0U && !queue_[0].sentOnConnection) {
    const GatewayLanIoResult written = transport_->writeOneFrame(
        queue_[0].bytes, queue_[0].frameBytes, kGatewayLanIoTimeoutMs);
    if (written == GatewayLanIoResult::WouldBlock) {
      return setResult(GatewayLanRuntimeResult::Ok);
    }
    if (written != GatewayLanIoResult::Ok) {
      connectionFailed(GatewayLanRuntimeResult::TransportFailed, nowMillis);
      return GatewayLanRuntimeResult::TransportFailed;
    }
    queue_[0].sentOnConnection = true;
    queue_[0].sentAtMillis = nowMillis;
    status_.waitingForAck = true;
  }

  const uint8_t* frame = nullptr;
  size_t frameBytes = 0U;
  const GatewayLanIoResult received = transport_->receiveOneFrame(
      frame, frameBytes, nowMillis, kGatewayLanIoTimeoutMs);
  if (received == GatewayLanIoResult::Ok) {
    const GatewayLanRuntimeResult handled =
        handleFrame(frame, frameBytes, nowEpoch, clockValid, nowMillis);
    transport_->consumeReceivedFrame();
    return handled;
  }
  if (received != GatewayLanIoResult::WouldBlock) {
    connectionFailed(GatewayLanRuntimeResult::TransportFailed, nowMillis);
    return GatewayLanRuntimeResult::TransportFailed;
  }
  if (status_.queuedFrames != 0U && queue_[0].sentOnConnection &&
      static_cast<uint32_t>(nowMillis - queue_[0].sentAtMillis) >=
          kGatewayLanAckTimeoutMs) {
    connectionFailed(GatewayLanRuntimeResult::AckTimedOut, nowMillis);
    return GatewayLanRuntimeResult::AckTimedOut;
  }
  return setResult(GatewayLanRuntimeResult::Ok);
}

GatewayLanRuntimeStatus KitsuGatewayLanRuntime::status() const {
  return status_;
}

}  // namespace connectivity
}  // namespace kitsu868
