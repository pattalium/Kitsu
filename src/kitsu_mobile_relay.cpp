#include "kitsu_mobile_relay.h"

#include <new>
#include <stdio.h>
#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

enum class RequestKind : uint8_t {
  Invalid = 0,
  RelayConfigure,
  EnrollmentPull,
  EnrollmentPush,
  UplinkPull,
  DownlinkPush,
};

enum class ValueKind : uint8_t { String = 0, Unsigned, Boolean };

struct Span {
  const uint8_t* data = nullptr;
  size_t bytes = 0U;
};

struct Field {
  Span key{};
  Span value{};
  ValueKind kind = ValueKind::String;
  bool boolean = false;
};

struct Request {
  RequestKind kind = RequestKind::Invalid;
  size_t offset = 0U;
  size_t total = 0U;
  Span data{};
  bool final = false;
  Span gatewayUuid{};
  Span caCertificate{};
};

constexpr size_t kMaximumFields = 6U;

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

bool allZero(const uint8_t* input, size_t bytes) {
  if (!input) return true;
  uint8_t aggregate = 0U;
  for (size_t i = 0U; i < bytes; ++i) aggregate |= input[i];
  return aggregate == 0U;
}

void skipWhitespace(const uint8_t* input, size_t bytes, size_t& cursor) {
  while (cursor < bytes &&
         (input[cursor] == ' ' || input[cursor] == '\t' ||
          input[cursor] == '\r' || input[cursor] == '\n')) {
    ++cursor;
  }
}

bool parseString(const uint8_t* input, size_t bytes, size_t& cursor,
                 Span& output) {
  output = Span{};
  skipWhitespace(input, bytes, cursor);
  if (cursor >= bytes || input[cursor++] != '"') return false;
  const size_t start = cursor;
  while (cursor < bytes && input[cursor] != '"') {
    const uint8_t value = input[cursor];
    if (value < 0x20U || value > 0x7eU || value == '\\') return false;
    ++cursor;
  }
  if (cursor >= bytes) return false;
  output.data = input + start;
  output.bytes = cursor - start;
  ++cursor;
  return true;
}

bool same(const Span& value, const char* expected) {
  const size_t expectedBytes = expected ? strlen(expected) : 0U;
  return expected && value.bytes == expectedBytes &&
      memcmp(value.data, expected, expectedBytes) == 0;
}

bool same(const Span& left, const Span& right) {
  return left.bytes == right.bytes &&
      memcmp(left.data, right.data, left.bytes) == 0;
}

bool parseUnsigned(const Span& value, size_t maximum, size_t& output) {
  output = 0U;
  if (value.bytes == 0U || value.bytes > 10U ||
      (value.bytes > 1U && value.data[0] == '0')) {
    return false;
  }
  for (size_t i = 0U; i < value.bytes; ++i) {
    if (value.data[i] < '0' || value.data[i] > '9') return false;
    const size_t digit = static_cast<size_t>(value.data[i] - '0');
    if (output > (maximum - digit) / 10U) return false;
    output = output * 10U + digit;
  }
  return true;
}

bool parseObject(const uint8_t* json, size_t jsonBytes,
                 Field fields[kMaximumFields], size_t& count) {
  count = 0U;
  if (!json || jsonBytes == 0U ||
      jsonBytes > companion::kMaximumEnvelopePayloadBytes ||
      !companion::validUtf8(json, jsonBytes)) {
    return false;
  }
  size_t cursor = 0U;
  skipWhitespace(json, jsonBytes, cursor);
  if (cursor >= jsonBytes || json[cursor++] != '{') return false;
  skipWhitespace(json, jsonBytes, cursor);
  if (cursor < jsonBytes && json[cursor] == '}') return false;

  while (cursor < jsonBytes) {
    if (count == kMaximumFields ||
        !parseString(json, jsonBytes, cursor, fields[count].key)) {
      return false;
    }
    for (size_t i = 0U; i < count; ++i) {
      if (same(fields[i].key, fields[count].key)) return false;
    }
    skipWhitespace(json, jsonBytes, cursor);
    if (cursor >= jsonBytes || json[cursor++] != ':') return false;
    skipWhitespace(json, jsonBytes, cursor);
    if (cursor >= jsonBytes) return false;
    if (json[cursor] == '"') {
      fields[count].kind = ValueKind::String;
      if (!parseString(json, jsonBytes, cursor, fields[count].value)) {
        return false;
      }
    } else if (json[cursor] >= '0' && json[cursor] <= '9') {
      fields[count].kind = ValueKind::Unsigned;
      const size_t start = cursor;
      while (cursor < jsonBytes && json[cursor] >= '0' &&
             json[cursor] <= '9') {
        ++cursor;
      }
      fields[count].value.data = json + start;
      fields[count].value.bytes = cursor - start;
    } else if (cursor + 4U <= jsonBytes &&
               memcmp(json + cursor, "true", 4U) == 0) {
      fields[count].kind = ValueKind::Boolean;
      fields[count].boolean = true;
      cursor += 4U;
    } else if (cursor + 5U <= jsonBytes &&
               memcmp(json + cursor, "false", 5U) == 0) {
      fields[count].kind = ValueKind::Boolean;
      fields[count].boolean = false;
      cursor += 5U;
    } else {
      return false;
    }
    ++count;
    skipWhitespace(json, jsonBytes, cursor);
    if (cursor >= jsonBytes) return false;
    if (json[cursor] == '}') {
      ++cursor;
      skipWhitespace(json, jsonBytes, cursor);
      return cursor == jsonBytes;
    }
    if (json[cursor++] != ',') return false;
    skipWhitespace(json, jsonBytes, cursor);
  }
  return false;
}

const Field* find(const Field* fields, size_t count, const char* name,
                  ValueKind kind) {
  for (size_t i = 0U; i < count; ++i) {
    if (fields[i].kind == kind && same(fields[i].key, name)) {
      return fields + i;
    }
  }
  return nullptr;
}

bool parseCanonicalUuid(const Span& value,
                        uint8_t output[kEnrollmentUuidBytes]) {
  if (!output || value.bytes != 36U) return false;
  memset(output, 0, kEnrollmentUuidBytes);
  size_t write = 0U;
  int high = -1;
  for (size_t i = 0U; i < value.bytes; ++i) {
    if (i == 8U || i == 13U || i == 18U || i == 23U) {
      if (value.data[i] != '-' || high >= 0) return false;
      continue;
    }
    const uint8_t c = value.data[i];
    int nibble = -1;
    if (c >= '0' && c <= '9') {
      nibble = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      nibble = c - 'a' + 10;
    }
    if (nibble < 0) return false;
    if (high < 0) {
      high = nibble;
    } else {
      if (write >= kEnrollmentUuidBytes) return false;
      output[write++] = static_cast<uint8_t>((high << 4U) | nibble);
      high = -1;
    }
  }
  return write == kEnrollmentUuidBytes && high < 0 &&
      !allZero(output, kEnrollmentUuidBytes);
}

RequestKind requestKind(const Span& value) {
  if (same(value, "relay_configure")) return RequestKind::RelayConfigure;
  if (same(value, "enrollment_pull")) return RequestKind::EnrollmentPull;
  if (same(value, "enrollment_push")) return RequestKind::EnrollmentPush;
  if (same(value, "uplink_pull")) return RequestKind::UplinkPull;
  if (same(value, "downlink_push")) return RequestKind::DownlinkPush;
  return RequestKind::Invalid;
}

bool decodeRequest(const uint8_t* json, size_t jsonBytes, Request& output) {
  output = Request{};
  Field fields[kMaximumFields]{};
  size_t count = 0U;
  if (!parseObject(json, jsonBytes, fields, count)) return false;
  const Field* schema = find(fields, count, "schema", ValueKind::String);
  const Field* kind = find(fields, count, "kind", ValueKind::String);
  if (!schema || !same(schema->value, kMobileRelayExchangeSchema) || !kind) {
    return false;
  }
  output.kind = requestKind(kind->value);
  if (output.kind == RequestKind::Invalid) return false;

  if (output.kind == RequestKind::RelayConfigure) {
    const Field* gateway =
        find(fields, count, "gateway_id", ValueKind::String);
    const Field* ca =
        find(fields, count, "ca_cert_der_b64", ValueKind::String);
    if (count != 4U || !gateway || !ca || ca->value.bytes == 0U ||
        ca->value.bytes > companion::base64UrlEncodedBytes(
                              kMobileRelayMaximumGatewayCaBytes)) {
      return false;
    }
    output.gatewayUuid = gateway->value;
    output.caCertificate = ca->value;
    return true;
  }

  const Field* offset = find(fields, count, "offset", ValueKind::Unsigned);
  if (!offset || !parseUnsigned(offset->value,
                                kMobileRelayMaximumEnrollmentResponseBytes,
                                output.offset)) {
    return false;
  }
  if (output.kind == RequestKind::EnrollmentPull ||
      output.kind == RequestKind::UplinkPull) {
    return count == 3U;
  }

  const Field* total = find(fields, count, "total", ValueKind::Unsigned);
  const Field* data = find(fields, count, "data_b64", ValueKind::String);
  const Field* final = find(fields, count, "final", ValueKind::Boolean);
  if (count != 6U || !total || !data || !final ||
      !parseUnsigned(total->value, kMobileRelayMaximumEnrollmentResponseBytes,
                     output.total) ||
      output.total == 0U || data->value.bytes == 0U ||
      data->value.bytes >
          companion::base64UrlEncodedBytes(kMobileRelayChunkBytes)) {
    return false;
  }
  output.data = data->value;
  output.final = final->boolean;
  return true;
}

const char* responseKind(RequestKind kind) {
  switch (kind) {
    case RequestKind::RelayConfigure: return "configure";
    case RequestKind::EnrollmentPull:
    case RequestKind::EnrollmentPush: return "enrollment";
    case RequestKind::UplinkPull: return "uplink";
    case RequestKind::DownlinkPush: return "downlink";
    default: return "relay";
  }
}

bool encodeReceipt(RequestKind kind, bool accepted, size_t nextOffset,
                   bool complete, MobileRelayResult result,
                   uint8_t* output, size_t capacity, size_t& outputBytes) {
  outputBytes = 0U;
  if (!output || capacity == 0U) return false;
  int used = 0;
  if (accepted) {
    used = snprintf(
        reinterpret_cast<char*>(output), capacity,
        "{\"schema\":\"%s\",\"kind\":\"%s\",\"accepted\":true,"
        "\"next_offset\":%lu,\"complete\":%s,\"error_code\":null}",
        kMobileRelayReceiptSchema, responseKind(kind),
        static_cast<unsigned long>(nextOffset), complete ? "true" : "false");
  } else {
    used = snprintf(
        reinterpret_cast<char*>(output), capacity,
        "{\"schema\":\"%s\",\"kind\":\"%s\",\"accepted\":false,"
        "\"next_offset\":%lu,\"complete\":%s,\"error_code\":\"%s\"}",
        kMobileRelayReceiptSchema, responseKind(kind),
        static_cast<unsigned long>(nextOffset), complete ? "true" : "false",
        mobileRelayResultName(result));
  }
  if (used <= 0 || static_cast<size_t>(used) >= capacity) return false;
  outputBytes = static_cast<size_t>(used);
  return true;
}

bool encodeChunk(RequestKind requestKindValue, bool available, size_t offset,
                 size_t total, const uint8_t* data, size_t dataBytes,
                 bool final, uint8_t* output, size_t capacity,
                 size_t& outputBytes) {
  outputBytes = 0U;
  if (!output || capacity == 0U || (!data && dataBytes != 0U) ||
      dataBytes > kMobileRelayChunkBytes) {
    return false;
  }
  const int prefix = snprintf(
      reinterpret_cast<char*>(output), capacity,
      "{\"schema\":\"%s\",\"kind\":\"%s\",\"available\":%s,"
      "\"offset\":%lu,\"total\":%lu,\"data_b64\":\"",
      kMobileRelayChunkSchema, responseKind(requestKindValue),
      available ? "true" : "false", static_cast<unsigned long>(offset),
      static_cast<unsigned long>(total));
  if (prefix <= 0 || static_cast<size_t>(prefix) >= capacity) return false;
  size_t encodedBytes = 0U;
  if (!companion::encodeBase64Url(
          data, dataBytes, reinterpret_cast<char*>(output) + prefix,
          capacity - static_cast<size_t>(prefix), encodedBytes)) {
    return false;
  }
  const size_t cursor = static_cast<size_t>(prefix) + encodedBytes;
  const int suffix = snprintf(
      reinterpret_cast<char*>(output) + cursor, capacity - cursor,
      "\",\"final\":%s}", final ? "true" : "false");
  if (suffix <= 0 || static_cast<size_t>(suffix) >= capacity - cursor) {
    return false;
  }
  outputBytes = cursor + static_cast<size_t>(suffix);
  return outputBytes <= companion::kMaximumEnvelopePayloadBytes;
}

bool validRelayCredentials(const GatewayLanCredentialView& view) {
  return !allZero(view.companionUuid, kLanUuidBytes) &&
      !allZero(view.gatewayUuid, kLanUuidBytes) && view.keyVersion != 0U &&
      view.backendHmacSecret &&
      view.backendHmacSecretBytes == kEnrollmentSecretBytes &&
      !allZero(view.backendHmacSecret, kEnrollmentSecretBytes);
}

}  // namespace

const char* mobileRelayResultName(MobileRelayResult result) {
  switch (result) {
    case MobileRelayResult::Ok: return "ok";
    case MobileRelayResult::NotBegun: return "not_begun";
    case MobileRelayResult::InvalidRequest: return "invalid_request";
    case MobileRelayResult::AuthorizationRequired:
      return "authorization_required";
    case MobileRelayResult::PhysicalConfirmationRequired:
      return "physical_confirmation_required";
    case MobileRelayResult::EnrollmentUnavailable:
      return "enrollment_unavailable";
    case MobileRelayResult::GatewayConfigurationFailed:
      return "gateway_configuration_failed";
    case MobileRelayResult::Busy: return "busy";
    case MobileRelayResult::Oversize: return "oversize";
    case MobileRelayResult::OffsetMismatch: return "offset_mismatch";
    case MobileRelayResult::OutOfMemory: return "out_of_memory";
    case MobileRelayResult::CredentialsUnavailable:
      return "credentials_unavailable";
    case MobileRelayResult::CredentialsInvalid:
      return "credentials_invalid";
    case MobileRelayResult::SequenceStoreFailed:
      return "sequence_store_failed";
    case MobileRelayResult::CryptoFailed: return "crypto_failed";
    case MobileRelayResult::UnexpectedAck: return "unexpected_ack";
    case MobileRelayResult::GatewayFrameRejected:
      return "gateway_frame_rejected";
    case MobileRelayResult::ActionStoreFailed:
      return "action_store_failed";
    case MobileRelayResult::ActionSinkFailed: return "action_sink_failed";
    case MobileRelayResult::OutputTooSmall: return "output_too_small";
    case MobileRelayResult::EnrollmentBackendMalformed:
      return "backend_malformed";
    case MobileRelayResult::EnrollmentResponseMismatch:
      return "response_mismatch";
    case MobileRelayResult::EnrollmentInvalidCertificate:
      return "invalid_certificate";
    case MobileRelayResult::EnrollmentDecryptionFailed:
      return "decryption_failed";
    case MobileRelayResult::EnrollmentCommitFailed:
      return "enrollment_commit_failed";
    case MobileRelayResult::EnrollmentInvalid: return "enrollment_invalid";
    case MobileRelayResult::EnrollmentTrustFailed:
      return "enrollment_trust_failed";
    case MobileRelayResult::StorageAllocationFailed:
      return "storage_allocation_failed";
    case MobileRelayResult::StorageReadFailed: return "storage_read_failed";
    case MobileRelayResult::StorageWriteFailed: return "storage_write_failed";
    case MobileRelayResult::StorageReadbackFailed:
      return "storage_readback_failed";
    case MobileRelayResult::StorageCorrupt: return "storage_corrupt";
  }
  return "invalid_request";
}

KitsuMobileRelay::KitsuMobileRelay() = default;

KitsuMobileRelay::~KitsuMobileRelay() { stop(); }

bool KitsuMobileRelay::begin(
    GatewayLanCredentialProvider& credentials,
    GatewayLanSequenceStore& sequences, companion::CompanionCrypto& crypto,
    LanActionReplayStore& replayStore, GatewayLanActionSink& actionSink,
    MobileRelayGatewayConfigSink& gatewayConfig,
    MobileRelayEnrollmentDelegate& enrollment) {
  if (begun_) return false;
  credentials_ = &credentials;
  sequences_ = &sequences;
  crypto_ = &crypto;
  replayStore_ = &replayStore;
  actionSink_ = &actionSink;
  gatewayConfig_ = &gatewayConfig;
  enrollment_ = &enrollment;
  begun_ = true;
  return true;
}

void KitsuMobileRelay::clearUpload() {
  if (upload_) {
    secureZero(upload_, uploadTotal_);
    delete[] upload_;
  }
  upload_ = nullptr;
  uploadBytes_ = 0U;
  uploadTotal_ = 0U;
  uploadKind_ = UploadKind::None;
}

void KitsuMobileRelay::clearUplink() {
  if (uplink_) {
    secureZero(uplink_, uplinkBytes_);
    delete[] uplink_;
  }
  uplink_ = nullptr;
  uplinkBytes_ = 0U;
  uplinkSequence_ = 0U;
  uplinkKeyVersion_ = 0U;
  secureZero(uplinkCompanionUuid_, sizeof(uplinkCompanionUuid_));
  secureZero(uplinkGatewayUuid_, sizeof(uplinkGatewayUuid_));
}

void KitsuMobileRelay::clearPendingPayloads() {
  for (size_t i = 0U; i < pendingPayloadCount_; ++i) {
    if (pending_[i].bytes) {
      secureZero(pending_[i].bytes, pending_[i].payloadBytes);
      delete[] pending_[i].bytes;
    }
    secureZero(&pending_[i], sizeof(pending_[i]));
  }
  pendingPayloadCount_ = 0U;
}

void KitsuMobileRelay::stop() {
  clearUpload();
  clearUplink();
  clearPendingPayloads();
  credentials_ = nullptr;
  sequences_ = nullptr;
  crypto_ = nullptr;
  replayStore_ = nullptr;
  actionSink_ = nullptr;
  gatewayConfig_ = nullptr;
  enrollment_ = nullptr;
  nextTxSequence_ = 0U;
  lastTxSequence_ = 0U;
  begun_ = false;
}

void KitsuMobileRelay::onBleDisconnected() { clearUpload(); }

bool KitsuMobileRelay::reserveSequence(uint64_t& output) {
  output = 0U;
  if (!sequences_ || !sequences_->remoteConnectivityAllowed()) return false;
  if (nextTxSequence_ == 0U || nextTxSequence_ > lastTxSequence_) {
    uint64_t first = 0U;
    uint64_t last = 0U;
    if (!sequences_->reserveTx(kGatewayLanSequenceReservation, first, last) ||
        first == 0U || last < first) {
      return false;
    }
    nextTxSequence_ = first;
    lastTxSequence_ = last;
  }
  output = nextTxSequence_++;
  return output != 0U && output <= 0x7fffffffffffffffULL;
}

bool KitsuMobileRelay::canEnqueueDevicePayload(size_t payloadBytes) const {
  return canEnqueueDevicePayloads(1U, payloadBytes);
}

bool KitsuMobileRelay::canEnqueueDevicePayloads(
    size_t payloadCount, size_t worstCasePayloadBytes) const {
  if (!begun_ || !credentials_ || !sequences_ || payloadCount == 0U ||
      worstCasePayloadBytes == 0U ||
      worstCasePayloadBytes > kLanMaximumDevicePayloadBytes ||
      !credentials_->remoteConnectivityAllowed() ||
      !sequences_->remoteConnectivityAllowed()) {
    return false;
  }
  const size_t immediateSlot = uplink_ ? 0U : 1U;
  const size_t queuedSlots =
      kMobileRelayPendingPayloadDepth - pendingPayloadCount_;
  return payloadCount <= immediateSlot + queuedSlots;
}

MobileRelayResult KitsuMobileRelay::encodeUplink(
    const char* payloadType, const uint8_t* payload, size_t payloadBytes,
    int64_t issuedEpoch, uint64_t* assignedSequence) {
  if (assignedSequence) *assignedSequence = 0U;
  if (!begun_) return MobileRelayResult::NotBegun;
  if (uplink_) return MobileRelayResult::Busy;
  if (!payloadType || !payload || payloadBytes == 0U ||
      payloadBytes > kLanMaximumDevicePayloadBytes) {
    return MobileRelayResult::InvalidRequest;
  }
  if (!credentials_->remoteConnectivityAllowed() ||
      !sequences_->remoteConnectivityAllowed()) {
    return MobileRelayResult::AuthorizationRequired;
  }

  GatewayLanCredentialView view{};
  if (!credentials_->acquire(view)) {
    return MobileRelayResult::CredentialsUnavailable;
  }
  if (!validRelayCredentials(view)) {
    credentials_->release(view);
    return MobileRelayResult::CredentialsInvalid;
  }
  uint64_t sequence = 0U;
  if (!reserveSequence(sequence)) {
    credentials_->release(view);
    return MobileRelayResult::SequenceStoreFailed;
  }
  uint8_t nonce[kLanNonceBytes]{};
  uint8_t requestId[kLanUuidBytes]{};
  if (!crypto_->randomBytes(nonce, sizeof(nonce)) ||
      !crypto_->randomBytes(requestId, sizeof(requestId))) {
    secureZero(nonce, sizeof(nonce));
    secureZero(requestId, sizeof(requestId));
    credentials_->release(view);
    return MobileRelayResult::CryptoFailed;
  }
  requestId[6] = static_cast<uint8_t>((requestId[6] & 0x0fU) | 0x40U);
  requestId[8] = static_cast<uint8_t>((requestId[8] & 0x3fU) | 0x80U);

  uint8_t* scratch = new (std::nothrow) uint8_t[kLanMaximumFrameBytes];
  if (!scratch) {
    secureZero(nonce, sizeof(nonce));
    secureZero(requestId, sizeof(requestId));
    credentials_->release(view);
    return MobileRelayResult::OutOfMemory;
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
    delete[] scratch;
    credentials_->release(view);
    return encoded == LanResult::CryptoFailed
        ? MobileRelayResult::CryptoFailed
        : MobileRelayResult::InvalidRequest;
  }

  uint8_t* exact = new (std::nothrow) uint8_t[frameBytes];
  if (!exact) {
    secureZero(scratch, kLanMaximumFrameBytes);
    delete[] scratch;
    credentials_->release(view);
    return MobileRelayResult::OutOfMemory;
  }
  memcpy(exact, scratch, frameBytes);
  secureZero(scratch, kLanMaximumFrameBytes);
  delete[] scratch;
  uplink_ = exact;
  uplinkBytes_ = frameBytes;
  uplinkSequence_ = sequence;
  uplinkKeyVersion_ = view.keyVersion;
  memcpy(uplinkCompanionUuid_, view.companionUuid,
         sizeof(uplinkCompanionUuid_));
  memcpy(uplinkGatewayUuid_, view.gatewayUuid, sizeof(uplinkGatewayUuid_));
  if (assignedSequence) *assignedSequence = sequence;
  credentials_->release(view);
  return MobileRelayResult::Ok;
}

MobileRelayResult KitsuMobileRelay::enqueueDevicePayload(
    const char* payloadType, const uint8_t* payload, size_t payloadBytes,
    int64_t issuedEpoch, uint64_t* assignedSequence) {
  if (assignedSequence) *assignedSequence = 0U;
  if (!canEnqueueDevicePayloads(1U, payloadBytes) || !payloadType ||
      !payload) {
    return begun_ && uplink_ &&
                   pendingPayloadCount_ == kMobileRelayPendingPayloadDepth
        ? MobileRelayResult::Busy
        : MobileRelayResult::InvalidRequest;
  }
  if (!uplink_) {
    return encodeUplink(payloadType, payload, payloadBytes, issuedEpoch,
                        assignedSequence);
  }

  const size_t typeBytes = strlen(payloadType);
  if (typeBytes == 0U || typeBytes > kLanMaximumPayloadTypeBytes ||
      !companion::validUtf8(payload, payloadBytes)) {
    return MobileRelayResult::InvalidRequest;
  }
  uint8_t* exact = new (std::nothrow) uint8_t[payloadBytes];
  if (!exact) return MobileRelayResult::OutOfMemory;
  memcpy(exact, payload, payloadBytes);
  PendingPayload& queued = pending_[pendingPayloadCount_++];
  queued.bytes = exact;
  queued.payloadBytes = payloadBytes;
  queued.issuedEpoch = issuedEpoch;
  memcpy(queued.payloadType, payloadType, typeBytes);
  queued.payloadType[typeBytes] = '\0';
  return MobileRelayResult::Ok;
}

void KitsuMobileRelay::promotePendingPayload() {
  if (uplink_ || pendingPayloadCount_ == 0U) return;
  PendingPayload queued = pending_[0];
  if (encodeUplink(queued.payloadType, queued.bytes, queued.payloadBytes,
                   queued.issuedEpoch, nullptr) != MobileRelayResult::Ok) {
    return;
  }
  secureZero(queued.bytes, queued.payloadBytes);
  delete[] queued.bytes;
  for (size_t i = 1U; i < pendingPayloadCount_; ++i) {
    pending_[i - 1U] = pending_[i];
  }
  --pendingPayloadCount_;
  secureZero(&pending_[pendingPayloadCount_],
             sizeof(pending_[pendingPayloadCount_]));
}

MobileRelayResult KitsuMobileRelay::handleCompletedDownlink(
    int64_t nowEpoch, bool clockValid) {
  if (!upload_ || uploadKind_ != UploadKind::Downlink ||
      uploadBytes_ == 0U || uploadBytes_ != uploadTotal_ ||
      uploadBytes_ > kLanMaximumFrameBytes) {
    return MobileRelayResult::InvalidRequest;
  }
  GatewayLanCredentialView view{};
  if (!credentials_->acquire(view)) {
    return MobileRelayResult::CredentialsUnavailable;
  }
  if (!validRelayCredentials(view)) {
    credentials_->release(view);
    return MobileRelayResult::CredentialsInvalid;
  }
  uint8_t* params =
      new (std::nothrow) uint8_t[kLanMaximumActionParamsBytes];
  if (!params) {
    credentials_->release(view);
    return MobileRelayResult::OutOfMemory;
  }
  memset(params, 0, kLanMaximumActionParamsBytes);
  LanGatewayFrame decoded{};
  const LanResult decodedResult = decodeGatewayFrame(
      upload_, uploadBytes_, view.companionUuid, view.keyVersion, nowEpoch,
      clockValid, view.backendHmacSecret, *crypto_, *replayStore_, decoded,
      params, kLanMaximumActionParamsBytes);

  MobileRelayResult result = MobileRelayResult::Ok;
  bool acknowledgedUplink = false;
  if (decodedResult == LanResult::GatewayAck) {
    if (!uplink_ || decoded.deviceSequence != uplinkSequence_ ||
        view.keyVersion != uplinkKeyVersion_ ||
        memcmp(view.companionUuid, uplinkCompanionUuid_, kLanUuidBytes) != 0 ||
        memcmp(view.gatewayUuid, uplinkGatewayUuid_, kLanUuidBytes) != 0) {
      result = MobileRelayResult::UnexpectedAck;
    } else {
      clearUplink();
      acknowledgedUplink = true;
    }
  } else if (decodedResult == LanResult::ActionFresh) {
    const uint64_t highWater = sequences_->rxHighWater();
    if (highWater == UINT64_MAX ||
        !sequences_->acceptNextRx(highWater + 1U)) {
      result = MobileRelayResult::SequenceStoreFailed;
    } else if (!actionSink_->acceptAuthenticatedAction(
                   upload_, uploadBytes_, decoded, params,
                   decoded.parameterBytes, nowEpoch)) {
      result = MobileRelayResult::ActionSinkFailed;
    }
  } else if (decodedResult == LanResult::ActionDuplicate) {
    if (!actionSink_->repeatAuthenticatedAction(
            upload_, uploadBytes_, decoded, params, decoded.parameterBytes,
            nowEpoch)) {
      result = MobileRelayResult::ActionSinkFailed;
    }
  } else if (decodedResult == LanResult::ReplayStoreFailed) {
    result = MobileRelayResult::ActionStoreFailed;
  } else if (decodedResult == LanResult::CryptoFailed) {
    result = MobileRelayResult::CryptoFailed;
  } else {
    result = MobileRelayResult::GatewayFrameRejected;
  }
  secureZero(params, kLanMaximumActionParamsBytes);
  delete[] params;
  credentials_->release(view);
  if (acknowledgedUplink) promotePendingPayload();
  return result;
}

bool KitsuMobileRelay::handleExchange(
    const uint8_t* requestJson, size_t requestBytes,
    const MobileRelayGuards& guards, int64_t nowEpoch, bool clockValid,
    uint8_t* responseJson, size_t responseCapacity, size_t& responseBytes,
    MobileRelayExchangeOutcome* outcome) {
  responseBytes = 0U;
  MobileRelayExchangeOutcome local{};
  if (!begun_) {
    local.result = MobileRelayResult::NotBegun;
    if (outcome) *outcome = local;
    return false;
  }
  Request request{};
  if (!decodeRequest(requestJson, requestBytes, request)) {
    local.result = MobileRelayResult::InvalidRequest;
    if (outcome) *outcome = local;
    return false;
  }
  if (!guards.authenticatedController) {
    local.result = MobileRelayResult::AuthorizationRequired;
    const bool encoded = encodeReceipt(
        request.kind, false, uploadBytes_, false, local.result, responseJson,
        responseCapacity, responseBytes);
    if (outcome) *outcome = local;
    return encoded;
  }

  if (request.kind == RequestKind::RelayConfigure) {
    if (upload_) {
      local.result = MobileRelayResult::Busy;
    } else {
      uint8_t gatewayId[kEnrollmentUuidBytes]{};
      uint8_t* ca =
          new (std::nothrow) uint8_t[kMobileRelayMaximumGatewayCaBytes];
      size_t caBytes = 0U;
      if (!ca) {
        local.result = MobileRelayResult::OutOfMemory;
      } else if (!parseCanonicalUuid(request.gatewayUuid, gatewayId) ||
                 !companion::decodeBase64Url(
                     reinterpret_cast<const char*>(request.caCertificate.data),
                     request.caCertificate.bytes, ca,
                     kMobileRelayMaximumGatewayCaBytes, caBytes) ||
                 caBytes == 0U) {
        local.result = MobileRelayResult::GatewayConfigurationFailed;
      } else {
        const MobileRelayGatewayConfigResult configured =
            gatewayConfig_->commitMobileRelayGateway(
                gatewayId, ca, caBytes);
        if (configured == MobileRelayGatewayConfigResult::Failed) {
          local.result = MobileRelayResult::GatewayConfigurationFailed;
        } else {
          if (configured == MobileRelayGatewayConfigResult::Changed) {
            clearUplink();
            clearPendingPayloads();
            nextTxSequence_ = 0U;
            lastTxSequence_ = 0U;
            local.gatewayConfigurationChanged = true;
          }
          local.result = MobileRelayResult::Ok;
          local.gatewayConfigured = true;
        }
      }
      secureZero(gatewayId, sizeof(gatewayId));
      if (ca) {
        secureZero(ca, kMobileRelayMaximumGatewayCaBytes);
        delete[] ca;
      }
    }
    const bool accepted = local.result == MobileRelayResult::Ok;
    const bool encoded = encodeReceipt(
        request.kind, accepted, 0U, true, local.result, responseJson,
        responseCapacity, responseBytes);
    if (outcome) *outcome = local;
    return encoded;
  }

  const bool enrollmentRequest =
      request.kind == RequestKind::EnrollmentPull ||
      request.kind == RequestKind::EnrollmentPush;
  if (enrollmentRequest &&
      (!guards.enrollmentPrgConfirmed || !guards.enrollmentActive)) {
    local.result = guards.enrollmentPrgConfirmed
        ? MobileRelayResult::EnrollmentUnavailable
        : MobileRelayResult::PhysicalConfirmationRequired;
    const bool encoded = encodeReceipt(
        request.kind, false, uploadBytes_, false, local.result, responseJson,
        responseCapacity, responseBytes);
    if (outcome) *outcome = local;
    return encoded;
  }

  if (request.kind == RequestKind::EnrollmentPull) {
    uint8_t* claim = new (std::nothrow)
        uint8_t[kMobileRelayMaximumEnrollmentRequestBytes];
    if (!claim) {
      local.result = MobileRelayResult::OutOfMemory;
    } else {
      memset(claim, 0, kMobileRelayMaximumEnrollmentRequestBytes);
      size_t claimBytes = 0U;
      if (!enrollment_->buildMobileRelayEnrollmentRequest(
              claim, kMobileRelayMaximumEnrollmentRequestBytes,
              claimBytes) ||
          claimBytes == 0U ||
          claimBytes > kMobileRelayMaximumEnrollmentRequestBytes ||
          request.offset >= claimBytes) {
        local.result = MobileRelayResult::EnrollmentUnavailable;
      } else {
        const size_t remaining = claimBytes - request.offset;
        const size_t chunkBytes = remaining < kMobileRelayChunkBytes
            ? remaining
            : kMobileRelayChunkBytes;
        const bool final = request.offset + chunkBytes == claimBytes;
        local.result = encodeChunk(
            request.kind, true, request.offset, claimBytes,
            claim + request.offset, chunkBytes, final, responseJson,
            responseCapacity, responseBytes)
            ? MobileRelayResult::Ok
            : MobileRelayResult::OutputTooSmall;
      }
      secureZero(claim, kMobileRelayMaximumEnrollmentRequestBytes);
      delete[] claim;
    }
    if (outcome) *outcome = local;
    return local.result == MobileRelayResult::Ok;
  }

  if (request.kind == RequestKind::UplinkPull) {
    if (!uplink_) promotePendingPayload();
    if (!uplink_) {
      local.result = encodeChunk(request.kind, false, 0U, 0U, nullptr, 0U,
                                 false, responseJson, responseCapacity,
                                 responseBytes)
          ? MobileRelayResult::Ok
          : MobileRelayResult::OutputTooSmall;
    } else if (request.offset >= uplinkBytes_) {
      local.result = MobileRelayResult::OffsetMismatch;
    } else {
      const size_t remaining = uplinkBytes_ - request.offset;
      const size_t chunkBytes = remaining < kMobileRelayChunkBytes
          ? remaining
          : kMobileRelayChunkBytes;
      local.result = encodeChunk(
          request.kind, true, request.offset, uplinkBytes_,
          uplink_ + request.offset, chunkBytes,
          request.offset + chunkBytes == uplinkBytes_, responseJson,
          responseCapacity, responseBytes)
          ? MobileRelayResult::Ok
          : MobileRelayResult::OutputTooSmall;
    }
    if (outcome) *outcome = local;
    return local.result == MobileRelayResult::Ok;
  }

  const UploadKind requestedUpload =
      request.kind == RequestKind::EnrollmentPush
          ? UploadKind::Enrollment
          : UploadKind::Downlink;
  const size_t maximumTotal = requestedUpload == UploadKind::Enrollment
      ? kMobileRelayMaximumEnrollmentResponseBytes
      : kLanMaximumFrameBytes;
  if (request.total > maximumTotal) {
    local.result = MobileRelayResult::Oversize;
  } else {
    uint8_t* decoded = new (std::nothrow) uint8_t[kMobileRelayChunkBytes];
    if (!decoded) {
      local.result = MobileRelayResult::OutOfMemory;
    } else {
      memset(decoded, 0, kMobileRelayChunkBytes);
      size_t decodedBytes = 0U;
      const bool decodedOk = companion::decodeBase64Url(
          reinterpret_cast<const char*>(request.data.data), request.data.bytes,
          decoded, kMobileRelayChunkBytes, decodedBytes);
      if (!decodedOk || decodedBytes == 0U ||
          request.offset > request.total ||
          decodedBytes > request.total - request.offset ||
          request.final != (request.offset + decodedBytes == request.total)) {
        local.result = MobileRelayResult::InvalidRequest;
      } else if (upload_ &&
                 (uploadKind_ != requestedUpload ||
                  uploadTotal_ != request.total)) {
        local.result = MobileRelayResult::Busy;
      } else if ((!upload_ && request.offset != 0U) ||
                 (upload_ && request.offset != uploadBytes_)) {
        local.result = MobileRelayResult::OffsetMismatch;
      } else {
        if (!upload_) {
          upload_ = new (std::nothrow) uint8_t[request.total];
          if (!upload_) {
            local.result = MobileRelayResult::OutOfMemory;
          } else {
            memset(upload_, 0, request.total);
            uploadTotal_ = request.total;
            uploadKind_ = requestedUpload;
          }
        }
        if (upload_) {
          memcpy(upload_ + uploadBytes_, decoded, decodedBytes);
          uploadBytes_ += decodedBytes;
          local.result = MobileRelayResult::Ok;
        }
      }
      secureZero(decoded, kMobileRelayChunkBytes);
      delete[] decoded;
    }
  }

  if (local.result == MobileRelayResult::Ok && request.final) {
    if (requestedUpload == UploadKind::Enrollment) {
      local.result = enrollment_->installMobileRelayEnrollmentResponse(
          upload_, uploadBytes_);
      if (local.result == MobileRelayResult::Ok) {
        local.enrollmentCompleted = true;
      }
    } else {
      local.result = handleCompletedDownlink(nowEpoch, clockValid);
      local.downlinkCompleted = local.result == MobileRelayResult::Ok;
    }
    clearUpload();
  }

  const bool accepted = local.result == MobileRelayResult::Ok;
  const bool complete = request.final && accepted;
  const size_t nextOffset = complete
      ? request.total
      : (upload_ ? uploadBytes_ : 0U);
  const bool encoded = encodeReceipt(
      request.kind, accepted, nextOffset, complete, local.result,
      responseJson, responseCapacity, responseBytes);
  if (outcome) *outcome = local;
  return encoded;
}

MobileRelayStatus KitsuMobileRelay::status() const {
  MobileRelayStatus output{};
  output.begun = begun_;
  output.uploadActive = upload_ != nullptr;
  output.uplinkPending = uplink_ != nullptr;
  output.uploadBytes = uploadBytes_;
  output.uploadTotal = uploadTotal_;
  output.uplinkBytes = uplinkBytes_;
  output.uplinkSequence = uplinkSequence_;
  output.pendingPayloads = pendingPayloadCount_;
  return output;
}

}  // namespace connectivity
}  // namespace kitsu868
