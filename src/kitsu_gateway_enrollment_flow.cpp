#include "kitsu_gateway_enrollment_flow.h"

#include <stdio.h>
#include <string.h>

#include "kitsu_companion_protocol.h"

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr char kBeginSchema[] = "kitsu.gateway-enrollment.begin.v1";
constexpr char kFinishSchema[] = "kitsu.gateway-enrollment.finish.v1";
constexpr char kReceiptSchema[] = "kitsu.gateway-enrollment.receipt.v1";
constexpr char kEventSchema[] = "kitsu.gateway-enrollment.event.v1";

struct Span {
  const uint8_t* data = nullptr;
  size_t bytes = 0U;
};

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

bool constantTimeEqual(const void* left, const void* right, size_t bytes) {
  if ((!left || !right) && bytes != 0U) return false;
  const uint8_t* a = static_cast<const uint8_t*>(left);
  const uint8_t* b = static_cast<const uint8_t*>(right);
  uint8_t difference = 0U;
  for (size_t i = 0U; i < bytes; ++i) {
    difference |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return difference == 0U;
}

void skipWhitespace(const uint8_t* input, size_t bytes, size_t& cursor) {
  while (cursor < bytes &&
         (input[cursor] == ' ' || input[cursor] == '\t' ||
          input[cursor] == '\r' || input[cursor] == '\n')) {
    ++cursor;
  }
}

bool consume(const uint8_t* input, size_t bytes, size_t& cursor,
             uint8_t expected) {
  skipWhitespace(input, bytes, cursor);
  if (cursor >= bytes || input[cursor] != expected) return false;
  ++cursor;
  return true;
}

bool parseAsciiString(const uint8_t* input, size_t bytes, size_t& cursor,
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

int hexNibble(uint8_t value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

bool parseCanonicalUuid(const Span& value,
                        uint8_t output[kEnrollmentUuidBytes]) {
  if (!output || value.bytes != 36U) return false;
  static const uint8_t hyphens[] = {8U, 13U, 18U, 23U};
  size_t nextHyphen = 0U;
  size_t write = 0U;
  int high = -1;
  for (size_t i = 0U; i < value.bytes; ++i) {
    if (nextHyphen < sizeof(hyphens) && i == hyphens[nextHyphen]) {
      if (value.data[i] != '-' || high >= 0) return false;
      ++nextHyphen;
      continue;
    }
    const int nibble = hexNibble(value.data[i]);
    if (nibble < 0) return false;
    if (high < 0) {
      high = nibble;
    } else {
      if (write >= kEnrollmentUuidBytes) return false;
      output[write++] = static_cast<uint8_t>((high << 4U) | nibble);
      high = -1;
    }
  }
  return nextHyphen == sizeof(hyphens) && high < 0 &&
      write == kEnrollmentUuidBytes && !allZero(output, kEnrollmentUuidBytes);
}

void formatUuid(const uint8_t input[kEnrollmentUuidBytes], char output[37]) {
  static const char hex[] = "0123456789abcdef";
  size_t write = 0U;
  for (size_t i = 0U; i < kEnrollmentUuidBytes; ++i) {
    if (i == 4U || i == 6U || i == 8U || i == 10U) output[write++] = '-';
    output[write++] = hex[input[i] >> 4U];
    output[write++] = hex[input[i] & 0x0fU];
  }
  output[write] = '\0';
}

bool parseObject(const uint8_t* json, size_t jsonBytes, bool begin,
                 uint8_t enrollmentId[kEnrollmentUuidBytes],
                 char claimToken[kGatewayEnrollmentClaimTokenBytes + 1U]) {
  if (!json || jsonBytes == 0U || jsonBytes > 512U || !enrollmentId ||
      (begin && !claimToken) || !companion::validUtf8(json, jsonBytes)) {
    return false;
  }
  memset(enrollmentId, 0, kEnrollmentUuidBytes);
  if (claimToken) memset(claimToken, 0,
                         kGatewayEnrollmentClaimTokenBytes + 1U);
  size_t cursor = 0U;
  if (!consume(json, jsonBytes, cursor, '{')) return false;
  bool sawSchema = false;
  bool sawEnrollment = false;
  bool sawToken = false;
  const size_t expectedFields = begin ? 3U : 2U;
  for (size_t ordinal = 0U; ordinal < expectedFields; ++ordinal) {
    Span key{};
    Span value{};
    if (!parseAsciiString(json, jsonBytes, cursor, key) ||
        !consume(json, jsonBytes, cursor, ':') ||
        !parseAsciiString(json, jsonBytes, cursor, value)) {
      return false;
    }
    if (same(key, "schema")) {
      if (sawSchema || !same(value, begin ? kBeginSchema : kFinishSchema)) {
        return false;
      }
      sawSchema = true;
    } else if (same(key, "enrollment_id")) {
      if (sawEnrollment || !parseCanonicalUuid(value, enrollmentId)) {
        return false;
      }
      sawEnrollment = true;
    } else if (begin && same(key, "claim_token")) {
      if (sawToken ||
          value.bytes != kGatewayEnrollmentClaimTokenBytes) {
        return false;
      }
      uint8_t decoded[32]{};
      size_t decodedBytes = 0U;
      const bool valid = companion::decodeBase64Url(
          reinterpret_cast<const char*>(value.data), value.bytes, decoded,
          sizeof(decoded), decodedBytes) &&
          decodedBytes == sizeof(decoded);
      secureZero(decoded, sizeof(decoded));
      if (!valid) return false;
      memcpy(claimToken, value.data, value.bytes);
      claimToken[value.bytes] = '\0';
      sawToken = true;
    } else {
      return false;
    }
    skipWhitespace(json, jsonBytes, cursor);
    if (ordinal + 1U < expectedFields) {
      if (cursor >= jsonBytes || json[cursor++] != ',') return false;
    }
  }
  if (!consume(json, jsonBytes, cursor, '}')) return false;
  skipWhitespace(json, jsonBytes, cursor);
  return cursor == jsonBytes && sawSchema && sawEnrollment &&
      (begin ? sawToken : !sawToken);
}

bool encodeDocument(const char* schema,
                    const GatewayEnrollmentReceipt& receipt,
                    uint8_t* output, size_t capacity, size_t& outputBytes) {
  outputBytes = 0U;
  if (!schema || !output || capacity == 0U ||
      (receipt.hasEnrollmentId &&
       allZero(receipt.enrollmentId, kEnrollmentUuidBytes))) {
    return false;
  }
  char uuid[37]{};
  if (receipt.hasEnrollmentId) formatUuid(receipt.enrollmentId, uuid);
  char encoded[512]{};
  int used = 0;
  if (receipt.error == GatewayEnrollmentError::None) {
    used = snprintf(
        encoded, sizeof(encoded),
        "{\"schema\":\"%s\",\"accepted\":%s,\"state\":\"%s\"," 
        "\"enrollment_id\":%s%s%s,\"expires_in_ms\":%lu," 
        "\"error_code\":null}",
        schema, receipt.accepted ? "true" : "false",
        gatewayEnrollmentFlowStateName(receipt.state),
        receipt.hasEnrollmentId ? "\"" : "null",
        receipt.hasEnrollmentId ? uuid : "",
        receipt.hasEnrollmentId ? "\"" : "",
        static_cast<unsigned long>(receipt.expiresInMs));
  } else {
    used = snprintf(
        encoded, sizeof(encoded),
        "{\"schema\":\"%s\",\"accepted\":%s,\"state\":\"%s\"," 
        "\"enrollment_id\":%s%s%s,\"expires_in_ms\":%lu," 
        "\"error_code\":\"%s\"}",
        schema, receipt.accepted ? "true" : "false",
        gatewayEnrollmentFlowStateName(receipt.state),
        receipt.hasEnrollmentId ? "\"" : "null",
        receipt.hasEnrollmentId ? uuid : "",
        receipt.hasEnrollmentId ? "\"" : "",
        static_cast<unsigned long>(receipt.expiresInMs),
        gatewayEnrollmentErrorName(receipt.error));
  }
  if (used <= 0 || static_cast<size_t>(used) >= sizeof(encoded) ||
      static_cast<size_t>(used) > capacity) {
    secureZero(encoded, sizeof(encoded));
    return false;
  }
  memcpy(output, encoded, static_cast<size_t>(used));
  outputBytes = static_cast<size_t>(used);
  secureZero(encoded, sizeof(encoded));
  return true;
}

GatewayEnrollmentError guardError(const GatewayEnrollmentGuards& guards) {
  if (!guards.authenticatedController) {
    return GatewayEnrollmentError::InvalidRequest;
  }
  if (!guards.storageReady) return GatewayEnrollmentError::StorageFailed;
  if (!guards.gatewayConfigured) return GatewayEnrollmentError::NotConfigured;
  if (guards.alreadyEnrolled) return GatewayEnrollmentError::AlreadyEnrolled;
  if (!guards.trustedClock) return GatewayEnrollmentError::TimeUnset;
  if (!guards.remoteConnectivityAllowed) {
    return GatewayEnrollmentError::ConnectivityUnavailable;
  }
  return GatewayEnrollmentError::None;
}

}  // namespace

const char* gatewayEnrollmentFlowStateName(GatewayEnrollmentFlowState state) {
  switch (state) {
    case GatewayEnrollmentFlowState::Idle: return "idle";
    case GatewayEnrollmentFlowState::PhysicalConfirmationRequired:
      return "physical_confirmation_required";
    case GatewayEnrollmentFlowState::PhysicalConfirmed:
      return "physical_confirmed";
    case GatewayEnrollmentFlowState::ReadyForWifi: return "ready_for_wifi";
    case GatewayEnrollmentFlowState::Bootstrapping: return "bootstrapping";
    case GatewayEnrollmentFlowState::Enrolled: return "enrolled";
    case GatewayEnrollmentFlowState::Failed: return "failed";
    case GatewayEnrollmentFlowState::Expired: return "expired";
  }
  return "failed";
}

const char* gatewayEnrollmentErrorName(GatewayEnrollmentError error) {
  switch (error) {
    case GatewayEnrollmentError::None: return "none";
    case GatewayEnrollmentError::InvalidRequest: return "invalid_request";
    case GatewayEnrollmentError::NotConfigured: return "not_configured";
    case GatewayEnrollmentError::AlreadyEnrolled: return "already_enrolled";
    case GatewayEnrollmentError::TimeUnset: return "time_unset";
    case GatewayEnrollmentError::ConnectivityUnavailable:
      return "connectivity_unavailable";
    case GatewayEnrollmentError::Busy: return "busy";
    case GatewayEnrollmentError::PhysicalConfirmationRequired:
      return "physical_confirmation_required";
    case GatewayEnrollmentError::Expired: return "expired";
    case GatewayEnrollmentError::StorageFailed: return "storage_failed";
    case GatewayEnrollmentError::BootstrapFailed: return "bootstrap_failed";
  }
  return "storage_failed";
}

bool decodeGatewayEnrollmentBegin(
    const uint8_t* json, size_t jsonBytes,
    uint8_t enrollmentId[kEnrollmentUuidBytes],
    char claimToken[kGatewayEnrollmentClaimTokenBytes + 1U]) {
  return parseObject(json, jsonBytes, true, enrollmentId, claimToken);
}

bool decodeGatewayEnrollmentFinish(
    const uint8_t* json, size_t jsonBytes,
    uint8_t enrollmentId[kEnrollmentUuidBytes]) {
  return parseObject(json, jsonBytes, false, enrollmentId, nullptr);
}

bool encodeGatewayEnrollmentReceipt(const GatewayEnrollmentReceipt& receipt,
                                    uint8_t* output, size_t outputCapacity,
                                    size_t& outputBytes) {
  return encodeDocument(kReceiptSchema, receipt, output, outputCapacity,
                        outputBytes);
}

bool encodeGatewayEnrollmentEvent(const GatewayEnrollmentReceipt& receipt,
                                  uint8_t* output, size_t outputCapacity,
                                  size_t& outputBytes) {
  return encodeDocument(kEventSchema, receipt, output, outputCapacity,
                        outputBytes);
}

KitsuGatewayEnrollmentFlow::KitsuGatewayEnrollmentFlow() = default;

KitsuGatewayEnrollmentFlow::~KitsuGatewayEnrollmentFlow() { abort(); }

bool KitsuGatewayEnrollmentFlow::activeAuthorizationState() const {
  return state_ == GatewayEnrollmentFlowState::PhysicalConfirmationRequired ||
      state_ == GatewayEnrollmentFlowState::PhysicalConfirmed;
}

bool KitsuGatewayEnrollmentFlow::sameEnrollmentId(
    const uint8_t id[kEnrollmentUuidBytes]) const {
  return hasEnrollmentId_ && id &&
      memcmp(id, enrollmentId_, kEnrollmentUuidBytes) == 0;
}

bool KitsuGatewayEnrollmentFlow::deadlineReached(uint32_t nowMillis) const {
  return deadline_ != 0U &&
      static_cast<int32_t>(nowMillis - deadline_) >= 0;
}

uint32_t KitsuGatewayEnrollmentFlow::remaining(uint32_t nowMillis) const {
  if (deadline_ == 0U || deadlineReached(nowMillis)) return 0U;
  return deadline_ - nowMillis;
}

void KitsuGatewayEnrollmentFlow::fillReceipt(
    bool accepted, GatewayEnrollmentFlowState state,
    GatewayEnrollmentError error, uint32_t nowMillis,
    GatewayEnrollmentReceipt& receipt) const {
  receipt = GatewayEnrollmentReceipt{};
  receipt.accepted = accepted;
  receipt.state = state;
  receipt.error = error;
  receipt.hasEnrollmentId = hasEnrollmentId_;
  if (hasEnrollmentId_) {
    memcpy(receipt.enrollmentId, enrollmentId_, kEnrollmentUuidBytes);
  }
  receipt.expiresInMs = remaining(nowMillis);
}

void KitsuGatewayEnrollmentFlow::clearAttemptSecrets() {
  secureZero(claimToken_, sizeof(claimToken_));
  if (recipient_) recipient_->abort();
  recipient_ = nullptr;
  deadline_ = 0U;
}

void KitsuGatewayEnrollmentFlow::beginOperation(
    const uint8_t* json, size_t jsonBytes,
    const GatewayEnrollmentGuards& guards, uint32_t nowMillis,
    GatewayEnrollmentReceipt& receipt) {
  uint8_t id[kEnrollmentUuidBytes]{};
  char token[kGatewayEnrollmentClaimTokenBytes + 1U]{};
  if (!decodeGatewayEnrollmentBegin(json, jsonBytes, id, token)) {
    receipt = GatewayEnrollmentReceipt{};
    receipt.state = GatewayEnrollmentFlowState::Idle;
    receipt.error = GatewayEnrollmentError::InvalidRequest;
    secureZero(id, sizeof(id));
    secureZero(token, sizeof(token));
    return;
  }
  poll(nowMillis, nullptr);
  if (activeAuthorizationState() && sameEnrollmentId(id) &&
      constantTimeEqual(token, claimToken_,
                        kGatewayEnrollmentClaimTokenBytes)) {
    fillReceipt(true, state_, GatewayEnrollmentError::None, nowMillis,
                receipt);
    secureZero(id, sizeof(id));
    secureZero(token, sizeof(token));
    return;
  }
  if (state_ == GatewayEnrollmentFlowState::ReadyForWifi ||
      state_ == GatewayEnrollmentFlowState::Bootstrapping ||
      activeAuthorizationState()) {
    receipt = GatewayEnrollmentReceipt{};
    receipt.state = state_;
    receipt.error = GatewayEnrollmentError::Busy;
    receipt.hasEnrollmentId = true;
    memcpy(receipt.enrollmentId, id, sizeof(id));
    secureZero(id, sizeof(id));
    secureZero(token, sizeof(token));
    return;
  }
  const GatewayEnrollmentError guard = guardError(guards);
  if (guard != GatewayEnrollmentError::None) {
    receipt = GatewayEnrollmentReceipt{};
    receipt.state = GatewayEnrollmentFlowState::Idle;
    receipt.error = guard;
    receipt.hasEnrollmentId = true;
    memcpy(receipt.enrollmentId, id, sizeof(id));
    secureZero(id, sizeof(id));
    secureZero(token, sizeof(token));
    return;
  }

  clearAttemptSecrets();
  memcpy(enrollmentId_, id, sizeof(enrollmentId_));
  hasEnrollmentId_ = true;
  memcpy(claimToken_, token, kGatewayEnrollmentClaimTokenBytes);
  claimToken_[kGatewayEnrollmentClaimTokenBytes] = '\0';
  deadline_ = nowMillis + kGatewayEnrollmentPhysicalWindowMs;
  if (deadline_ == 0U) deadline_ = 1U;
  state_ = GatewayEnrollmentFlowState::PhysicalConfirmationRequired;
  lastError_ = GatewayEnrollmentError::None;
  fillReceipt(true, state_, GatewayEnrollmentError::None, nowMillis, receipt);
  secureZero(id, sizeof(id));
  secureZero(token, sizeof(token));
}

bool KitsuGatewayEnrollmentFlow::confirmPhysical(
    uint32_t nowMillis, GatewayEnrollmentReceipt& eventReceipt) {
  poll(nowMillis, nullptr);
  if (state_ != GatewayEnrollmentFlowState::PhysicalConfirmationRequired &&
      state_ != GatewayEnrollmentFlowState::PhysicalConfirmed) {
    return false;
  }
  state_ = GatewayEnrollmentFlowState::PhysicalConfirmed;
  lastError_ = GatewayEnrollmentError::None;
  fillReceipt(true, state_, GatewayEnrollmentError::None, nowMillis,
              eventReceipt);
  return true;
}

void KitsuGatewayEnrollmentFlow::finishOperation(
    const uint8_t* json, size_t jsonBytes,
    const GatewayEnrollmentGuards& guards,
    const uint8_t expectedGatewayUuid[kEnrollmentUuidBytes],
    const char* hardwareUid, size_t hardwareUidBytes, uint32_t nowMillis,
    companion::CompanionCrypto& hashes, EnrollmentPlatformCrypto& platform,
    KitsuEnrollmentRecipient& recipient, GatewayEnrollmentReceipt& receipt) {
  uint8_t id[kEnrollmentUuidBytes]{};
  if (!decodeGatewayEnrollmentFinish(json, jsonBytes, id)) {
    receipt = GatewayEnrollmentReceipt{};
    receipt.state = state_;
    receipt.error = GatewayEnrollmentError::InvalidRequest;
    return;
  }
  poll(nowMillis, nullptr);
  receipt = GatewayEnrollmentReceipt{};
  receipt.hasEnrollmentId = true;
  memcpy(receipt.enrollmentId, id, sizeof(id));
  if (state_ == GatewayEnrollmentFlowState::ReadyForWifi &&
      sameEnrollmentId(id) && recipient.active()) {
    fillReceipt(true, GatewayEnrollmentFlowState::ReadyForWifi,
                GatewayEnrollmentError::None, nowMillis, receipt);
    secureZero(id, sizeof(id));
    return;
  }
  if (!sameEnrollmentId(id)) {
    receipt.state = state_;
    receipt.error = state_ == GatewayEnrollmentFlowState::Expired
        ? GatewayEnrollmentError::Expired
        : GatewayEnrollmentError::InvalidRequest;
    secureZero(id, sizeof(id));
    return;
  }
  if (state_ == GatewayEnrollmentFlowState::Expired) {
    fillReceipt(false, state_, GatewayEnrollmentError::Expired, nowMillis,
                receipt);
    secureZero(id, sizeof(id));
    return;
  }
  if (state_ == GatewayEnrollmentFlowState::PhysicalConfirmationRequired) {
    fillReceipt(false, state_,
                GatewayEnrollmentError::PhysicalConfirmationRequired,
                nowMillis, receipt);
    secureZero(id, sizeof(id));
    return;
  }
  if (state_ != GatewayEnrollmentFlowState::PhysicalConfirmed) {
    fillReceipt(false, state_, GatewayEnrollmentError::Busy, nowMillis,
                receipt);
    secureZero(id, sizeof(id));
    return;
  }
  const GatewayEnrollmentError guard = guardError(guards);
  if (guard != GatewayEnrollmentError::None) {
    fillReceipt(false, state_, guard, nowMillis, receipt);
    secureZero(id, sizeof(id));
    return;
  }
  if (!expectedGatewayUuid || !hardwareUid || hardwareUidBytes == 0U) {
    fillReceipt(false, state_, GatewayEnrollmentError::StorageFailed,
                nowMillis, receipt);
    secureZero(id, sizeof(id));
    return;
  }
  const EnrollmentResult result = recipient.begin(
      enrollmentId_, expectedGatewayUuid, hardwareUid, hardwareUidBytes,
      claimToken_, kGatewayEnrollmentClaimTokenBytes,
      guards.authenticatedController, true, guards.remoteConnectivityAllowed,
      hashes, platform);
  secureZero(claimToken_, sizeof(claimToken_));
  if (result != EnrollmentResult::Ok) {
    state_ = GatewayEnrollmentFlowState::Failed;
    lastError_ = result == EnrollmentResult::RemoteConnectivityUnavailable
        ? GatewayEnrollmentError::ConnectivityUnavailable
        : GatewayEnrollmentError::StorageFailed;
    deadline_ = 0U;
    fillReceipt(false, state_, lastError_, nowMillis, receipt);
    secureZero(id, sizeof(id));
    return;
  }
  recipient_ = &recipient;
  state_ = GatewayEnrollmentFlowState::ReadyForWifi;
  lastError_ = GatewayEnrollmentError::None;
  deadline_ = nowMillis + kGatewayEnrollmentBootstrapWindowMs;
  if (deadline_ == 0U) deadline_ = 1U;
  fillReceipt(true, state_, GatewayEnrollmentError::None, nowMillis, receipt);
  secureZero(id, sizeof(id));
}

void KitsuGatewayEnrollmentFlow::onBleDisconnected() {
  if (!activeAuthorizationState()) return;
  clearAttemptSecrets();
  secureZero(enrollmentId_, sizeof(enrollmentId_));
  hasEnrollmentId_ = false;
  state_ = GatewayEnrollmentFlowState::Idle;
  lastError_ = GatewayEnrollmentError::None;
}

bool KitsuGatewayEnrollmentFlow::poll(
    uint32_t nowMillis, GatewayEnrollmentReceipt* transition) {
  if ((activeAuthorizationState() ||
       state_ == GatewayEnrollmentFlowState::ReadyForWifi) &&
      deadlineReached(nowMillis)) {
    clearAttemptSecrets();
    secureZero(enrollmentId_, sizeof(enrollmentId_));
    hasEnrollmentId_ = false;
    state_ = GatewayEnrollmentFlowState::Expired;
    lastError_ = GatewayEnrollmentError::Expired;
    if (transition) {
      fillReceipt(false, state_, lastError_, nowMillis, *transition);
    }
    return true;
  }
  return false;
}

bool KitsuGatewayEnrollmentFlow::markBootstrapping(uint32_t nowMillis) {
  if (state_ != GatewayEnrollmentFlowState::ReadyForWifi || !recipient_ ||
      !recipient_->active() || deadlineReached(nowMillis)) {
    return false;
  }
  state_ = GatewayEnrollmentFlowState::Bootstrapping;
  deadline_ = 0U;
  return true;
}

void KitsuGatewayEnrollmentFlow::completeBootstrap(
    bool installed, GatewayEnrollmentError failure) {
  clearAttemptSecrets();
  state_ = installed ? GatewayEnrollmentFlowState::Enrolled
                     : GatewayEnrollmentFlowState::Failed;
  lastError_ = installed ? GatewayEnrollmentError::None
                         : failure;
}

void KitsuGatewayEnrollmentFlow::abort() {
  clearAttemptSecrets();
  secureZero(enrollmentId_, sizeof(enrollmentId_));
  hasEnrollmentId_ = false;
  state_ = GatewayEnrollmentFlowState::Idle;
  lastError_ = GatewayEnrollmentError::None;
}

GatewayEnrollmentFlowStatus KitsuGatewayEnrollmentFlow::status(
    uint32_t nowMillis) const {
  GatewayEnrollmentFlowStatus output{};
  output.state = state_;
  output.lastError = lastError_;
  output.hasEnrollmentId = hasEnrollmentId_;
  if (hasEnrollmentId_) {
    memcpy(output.enrollmentId, enrollmentId_, sizeof(output.enrollmentId));
  }
  output.expiresInMs = remaining(nowMillis);
  return output;
}

}  // namespace connectivity
}  // namespace kitsu868
