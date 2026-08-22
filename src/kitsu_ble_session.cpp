#include "kitsu_ble_session.h"

#include <stdio.h>
#include <string.h>

namespace kitsu868 {
namespace connectivity {
namespace {

constexpr size_t kMaximumControlFields = 10U;
constexpr uint32_t kCloseDelayMs = 250UL;

void secureZero(void* memory, size_t bytes) {
  volatile uint8_t* output = static_cast<volatile uint8_t*>(memory);
  while (bytes-- != 0U) *output++ = 0U;
}

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

bool constantTimeEqual(const uint8_t* left, const uint8_t* right,
                       size_t bytes) {
  uint8_t difference = 0U;
  for (size_t i = 0U; i < bytes; ++i) difference |= left[i] ^ right[i];
  return difference == 0U;
}

bool allZero(const uint8_t* input, size_t bytes) {
  uint8_t combined = 0U;
  for (size_t i = 0U; i < bytes; ++i) combined |= input[i];
  return combined == 0U;
}

bool validDeviceUid(const char* uid) {
  if (!uid || uid[0] != 'K' || uid[1] != 'T' || uid[6] != '\0') {
    return false;
  }
  for (size_t i = 2U; i < 6U; ++i) {
    if (!((uid[i] >= '0' && uid[i] <= '9') ||
          (uid[i] >= 'A' && uid[i] <= 'F'))) {
      return false;
    }
  }
  return true;
}

struct Span {
  const uint8_t* data = nullptr;
  size_t bytes = 0U;
  bool escaped = false;
};

enum class FlatValueKind : uint8_t { String = 0, Unsigned };

struct FlatField {
  Span key{};
  Span value{};
  FlatValueKind kind = FlatValueKind::String;
};

void skipWhitespace(const uint8_t* json, size_t bytes, size_t& cursor) {
  while (cursor < bytes &&
         (json[cursor] == ' ' || json[cursor] == '\t' ||
          json[cursor] == '\r' || json[cursor] == '\n')) {
    ++cursor;
  }
}

bool isHex(uint8_t input) {
  return (input >= '0' && input <= '9') ||
         (input >= 'a' && input <= 'f') ||
         (input >= 'A' && input <= 'F');
}

uint16_t hex4Value(const uint8_t* input) {
  uint16_t value = 0U;
  for (uint8_t i = 0U; i < 4U; ++i) {
    const uint8_t c = input[i];
    const uint8_t digit = c <= '9'
        ? static_cast<uint8_t>(c - '0')
        : c <= 'F' ? static_cast<uint8_t>(c - 'A' + 10U)
                   : static_cast<uint8_t>(c - 'a' + 10U);
    value = static_cast<uint16_t>((value << 4U) | digit);
  }
  return value;
}

bool parseString(const uint8_t* json, size_t bytes, size_t& cursor,
                 Span& output) {
  if (cursor >= bytes || json[cursor++] != '"') return false;
  const size_t start = cursor;
  bool escaped = false;
  while (cursor < bytes) {
    const uint8_t c = json[cursor++];
    if (c == '"') {
      output.data = json + start;
      output.bytes = cursor - start - 1U;
      output.escaped = escaped;
      return true;
    }
    if (c < 0x20U) return false;
    if (c != '\\') continue;
    escaped = true;
    if (cursor >= bytes) return false;
    const uint8_t escape = json[cursor++];
    if (escape == '"' || escape == '\\' || escape == '/' ||
        escape == 'b' || escape == 'f' || escape == 'n' ||
        escape == 'r' || escape == 't') {
      continue;
    }
    if (escape != 'u' || cursor + 4U > bytes) return false;
    for (uint8_t i = 0U; i < 4U; ++i) {
      if (!isHex(json[cursor + i])) return false;
    }
    const uint16_t first = hex4Value(json + cursor);
    cursor += 4U;
    if (first >= 0xd800U && first <= 0xdbffU) {
      if (cursor + 6U > bytes || json[cursor] != '\\' ||
          json[cursor + 1U] != 'u') {
        return false;
      }
      for (uint8_t i = 0U; i < 4U; ++i) {
        if (!isHex(json[cursor + 2U + i])) return false;
      }
      const uint16_t second = hex4Value(json + cursor + 2U);
      if (second < 0xdc00U || second > 0xdfffU) return false;
      cursor += 6U;
    } else if (first >= 0xdc00U && first <= 0xdfffU) {
      return false;
    }
  }
  return false;
}

bool sameSpan(const Span& left, const Span& right) {
  return left.bytes == right.bytes && !left.escaped && !right.escaped &&
         memcmp(left.data, right.data, left.bytes) == 0;
}

bool spanEquals(const Span& span, const char* text) {
  const size_t bytes = text ? strlen(text) : 0U;
  return !span.escaped && span.bytes == bytes &&
         memcmp(span.data, text, bytes) == 0;
}

bool parseFlatObject(const uint8_t* json, size_t bytes,
                     FlatField fields[kMaximumControlFields],
                     size_t& fieldCount) {
  fieldCount = 0U;
  if (!json || bytes == 0U ||
      bytes > companion::kMaximumHandshakeFrameBytes ||
      !companion::validUtf8(json, bytes)) {
    return false;
  }
  size_t cursor = 0U;
  skipWhitespace(json, bytes, cursor);
  if (cursor >= bytes || json[cursor++] != '{') return false;
  skipWhitespace(json, bytes, cursor);
  if (cursor < bytes && json[cursor] == '}') return false;
  while (cursor < bytes) {
    if (fieldCount >= kMaximumControlFields) return false;
    FlatField& field = fields[fieldCount];
    if (!parseString(json, bytes, cursor, field.key) || field.key.escaped ||
        field.key.bytes == 0U) {
      return false;
    }
    for (size_t i = 0U; i < fieldCount; ++i) {
      if (sameSpan(fields[i].key, field.key)) return false;
    }
    skipWhitespace(json, bytes, cursor);
    if (cursor >= bytes || json[cursor++] != ':') return false;
    skipWhitespace(json, bytes, cursor);
    if (cursor < bytes && json[cursor] == '"') {
      field.kind = FlatValueKind::String;
      if (!parseString(json, bytes, cursor, field.value)) return false;
    } else {
      field.kind = FlatValueKind::Unsigned;
      const size_t start = cursor;
      while (cursor < bytes && json[cursor] >= '0' && json[cursor] <= '9') {
        ++cursor;
      }
      if (cursor == start || (cursor - start > 1U && json[start] == '0')) {
        return false;
      }
      field.value.data = json + start;
      field.value.bytes = cursor - start;
      field.value.escaped = false;
    }
    ++fieldCount;
    skipWhitespace(json, bytes, cursor);
    if (cursor >= bytes) return false;
    if (json[cursor] == '}') {
      ++cursor;
      break;
    }
    if (json[cursor++] != ',') return false;
    skipWhitespace(json, bytes, cursor);
  }
  skipWhitespace(json, bytes, cursor);
  return cursor == bytes;
}

const FlatField* findField(const FlatField* fields, size_t fieldCount,
                           const char* name) {
  for (size_t i = 0U; i < fieldCount; ++i) {
    if (spanEquals(fields[i].key, name)) return fields + i;
  }
  return nullptr;
}

bool schemaMatches(const FlatField* fields, size_t fieldCount,
                   const char* const* names, size_t nameCount) {
  if (fieldCount != nameCount) return false;
  for (size_t i = 0U; i < nameCount; ++i) {
    if (!findField(fields, fieldCount, names[i])) return false;
  }
  return true;
}

bool validVersion(const FlatField* fields, size_t count) {
  const FlatField* version = findField(fields, count, "v");
  return version && version->kind == FlatValueKind::Unsigned &&
         version->value.bytes == 1U && version->value.data[0] == '1';
}

bool stringFieldEquals(const FlatField* fields, size_t count,
                       const char* fieldName, const char* expected) {
  const FlatField* field = findField(fields, count, fieldName);
  return field && field->kind == FlatValueKind::String &&
         spanEquals(field->value, expected);
}

bool decodeExact(const FlatField* fields, size_t count, const char* name,
                 uint8_t* output, size_t expectedBytes) {
  const FlatField* field = findField(fields, count, name);
  if (!field || field->kind != FlatValueKind::String ||
      field->value.escaped) {
    return false;
  }
  size_t decoded = 0U;
  return companion::decodeBase64Url(
             reinterpret_cast<const char*>(field->value.data),
             field->value.bytes, output, expectedBytes, decoded) &&
         decoded == expectedBytes;
}

bool validPlatform(const Span& value) {
  if (value.escaped || value.bytes == 0U || value.bytes > 16U) return false;
  for (size_t i = 0U; i < value.bytes; ++i) {
    const uint8_t c = value.data[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

class JsonWriter {
 public:
  JsonWriter(uint8_t* output, size_t capacity)
      : output_(output), capacity_(capacity) {}

  bool literal(const char* text) {
    if (!ok_ || !text) return false;
    const size_t bytes = strlen(text);
    if (used_ + bytes > capacity_) {
      ok_ = false;
      return false;
    }
    memcpy(output_ + used_, text, bytes);
    used_ += bytes;
    return true;
  }

  bool token(const char* text, size_t bytes) {
    if (!ok_ || (!text && bytes != 0U) || used_ + bytes > capacity_) {
      ok_ = false;
      return false;
    }
    if (bytes != 0U) memcpy(output_ + used_, text, bytes);
    used_ += bytes;
    return true;
  }

  bool unsignedValue(uint32_t value) {
    char decimal[11]{};
    const int bytes = snprintf(decimal, sizeof(decimal), "%lu",
                               static_cast<unsigned long>(value));
    return bytes > 0 && static_cast<size_t>(bytes) < sizeof(decimal) &&
           token(decimal, static_cast<size_t>(bytes));
  }

  bool ok() const { return ok_; }
  size_t size() const { return used_; }

 private:
  uint8_t* output_;
  size_t capacity_;
  size_t used_ = 0U;
  bool ok_ = true;
};

bool encodeB64(const uint8_t* input, size_t inputBytes, char* output,
               size_t outputCapacity, size_t& outputBytes) {
  return companion::encodeBase64Url(input, inputBytes, output,
                                    outputCapacity, outputBytes);
}

bool writeQuoted(JsonWriter& writer, const char* input, size_t bytes) {
  return writer.literal("\"") && writer.token(input, bytes) &&
         writer.literal("\"");
}

bool operationAllowed(const char* operation) {
  static const char* const operations[] = {
      "state.get",          "history.get",       "peers.get",
      "messages.get",       "channels.get",      "clock.sync",
      "mesh.configure",     "action.apply",      "wifi.configure",
      "wifi.retry",         "gateway.configure",  "gateway.forget",
      "gateway.enroll.begin",
      "gateway.enroll.finish",
      "mobile.relay.exchange",
  };
  for (size_t i = 0U; i < sizeof(operations) / sizeof(operations[0]); ++i) {
    if (strcmp(operation, operations[i]) == 0) return true;
  }
  return false;
}

}  // namespace

KitsuBleSession::KitsuBleSession() = default;

KitsuBleSession::~KitsuBleSession() {
  clearPendingPairing();
  clearSessionSecrets();
  secureZero(payloadScratch_, sizeof(payloadScratch_));
  secureZero(responseScratch_, sizeof(responseScratch_));
  secureZero(jsonScratch_, sizeof(jsonScratch_));
}

bool KitsuBleSession::begin(KitsuDeviceSecurity& security,
                            companion::CompanionCrypto& crypto,
                            BleSessionTransport& transport,
                            BleOperationDelegate& operations,
                            const char deviceUid[7]) {
  if (!security.ready() || !validDeviceUid(deviceUid) || begun_) return false;
  security_ = &security;
  crypto_ = &crypto;
  transport_ = &transport;
  operations_ = &operations;
  memcpy(deviceUid_, deviceUid, sizeof(deviceUid_));
  state_ = BleSessionState::Disconnected;
  begun_ = true;
  return true;
}

void KitsuBleSession::clearSessionSecrets() {
  secureZero(controllerId_, sizeof(controllerId_));
  secureZero(controllerRoot_, sizeof(controllerRoot_));
  secureZero(clientNonce_, sizeof(clientNonce_));
  secureZero(deviceNonce_, sizeof(deviceNonce_));
  secureZero(clientToDeviceKey_, sizeof(clientToDeviceKey_));
  secureZero(deviceToClientKey_, sizeof(deviceToClientKey_));
  controllerKnown_ = false;
  expectedClientSequence_ = 1U;
  nextDeviceSequence_ = 1U;
}

void KitsuBleSession::clearPendingPairing() {
  secureZero(pendingControllerId_, sizeof(pendingControllerId_));
  secureZero(pendingControllerRoot_, sizeof(pendingControllerRoot_));
  secureZero(pendingClientNonce_, sizeof(pendingClientNonce_));
  secureZero(pendingDeviceNonce_, sizeof(pendingDeviceNonce_));
}

void KitsuBleSession::resetForSecureLink(uint32_t nowMillis) {
  clearSessionSecrets();
  clearPendingPairing();
  transport_->setBleApplicationAuthenticated(false);
  state_ = BleSessionState::AwaitingHello;
  stateDeadline_ = nowMillis + kBleHandshakeTotalTimeoutMs;
  closeAt_ = 0U;
}

void KitsuBleSession::onSecureLinkEstablished(
    bool secureConnections, bool encrypted, bool authenticated, bool bonded,
    uint32_t nowMillis) {
  if (!begun_) return;
  secureConnections_ = secureConnections;
  linkEncrypted_ = encrypted;
  linkAuthenticated_ = authenticated;
  linkBonded_ = bonded;
  if (!secureConnections_ || !linkEncrypted_ || !linkAuthenticated_ ||
      !linkBonded_) {
    failAndClose(nowMillis);
    return;
  }
  if (backoffUntil_ != 0U && !deadlineReached(nowMillis, backoffUntil_)) {
    state_ = BleSessionState::Backoff;
    transport_->disconnectBle();
    return;
  }
  backoffUntil_ = 0U;
  resetForSecureLink(nowMillis);
}

void KitsuBleSession::onLinkClosed(uint32_t) {
  if (!begun_) return;
  clearPendingPairing();
  clearSessionSecrets();
  secureConnections_ = false;
  linkEncrypted_ = false;
  linkAuthenticated_ = false;
  linkBonded_ = false;
  pairingWindowOpen_ = false;
  pairingWindowDeadline_ = 0U;
  stateDeadline_ = 0U;
  closeAt_ = 0U;
  state_ = backoffUntil_ == 0U ? BleSessionState::Disconnected
                              : BleSessionState::Backoff;
}

void KitsuBleSession::setPairingWindow(bool open, uint32_t remainingMs,
                                       uint32_t nowMillis) {
  if (!begun_) return;
  pairingWindowOpen_ = open && remainingMs != 0U && remainingMs <= 60000UL;
  pairingWindowDeadline_ = pairingWindowOpen_
      ? nowMillis + remainingMs
      : 0U;
  if (!pairingWindowOpen_ &&
      (state_ == BleSessionState::PairingAwaitingPhysical ||
       state_ == BleSessionState::PairingAwaitingCommit)) {
    clearPendingPairing();
    state_ = BleSessionState::AwaitingHello;
  }
}

bool KitsuBleSession::sendControlError(const char* code) {
  if (!code) return false;
  JsonWriter writer(jsonScratch_, companion::kMaximumHandshakeFrameBytes);
  writer.literal("{\"v\":1,\"type\":\"error\",\"code\":");
  writeQuoted(writer, code, strlen(code));
  writer.literal("}");
  return writer.ok() && transport_->sendBleJson(jsonScratch_, writer.size());
}

void KitsuBleSession::failProof(uint32_t nowMillis) {
  clearPendingPairing();
  clearSessionSecrets();
  transport_->setBleApplicationAuthenticated(false);
  if (proofFailures_ < 0xffU) ++proofFailures_;
  sendControlError("auth_failed");
  if (proofFailures_ >= kBleMaximumProofFailures) {
    backoffUntil_ = nowMillis + kBleControllerBackoffMs;
    state_ = BleSessionState::Closing;
    closeAt_ = nowMillis + kCloseDelayMs;
  } else {
    state_ = BleSessionState::AwaitingHello;
  }
}

void KitsuBleSession::failAndClose(uint32_t nowMillis) {
  clearPendingPairing();
  clearSessionSecrets();
  transport_->setBleApplicationAuthenticated(false);
  state_ = BleSessionState::Closing;
  closeAt_ = nowMillis + kCloseDelayMs;
}

bool KitsuBleSession::handleClientHello(const uint8_t* json,
                                        size_t jsonBytes,
                                        uint32_t) {
  FlatField fields[kMaximumControlFields]{};
  size_t count = 0U;
  static const char* const schema[] = {
      "v", "type", "controller_id_b64", "client_nonce_b64"};
  if (!parseFlatObject(json, jsonBytes, fields, count) ||
      !schemaMatches(fields, count, schema,
                     sizeof(schema) / sizeof(schema[0])) ||
      !validVersion(fields, count) ||
      !stringFieldEquals(fields, count, "type", "client_hello") ||
      !decodeExact(fields, count, "controller_id_b64", controllerId_, 16U) ||
      !decodeExact(fields, count, "client_nonce_b64", clientNonce_, 16U) ||
      allZero(controllerId_, sizeof(controllerId_))) {
    return false;
  }
  controllerKnown_ = security_->findControllerRoot(controllerId_,
                                                    controllerRoot_);
  if (!controllerKnown_ &&
      !crypto_->randomBytes(controllerRoot_, sizeof(controllerRoot_))) {
    return false;
  }
  if (!crypto_->randomBytes(deviceNonce_, sizeof(deviceNonce_))) return false;

  uint8_t proof[32]{};
  if (companion::makeHandshakeProof(
          controllerRoot_, "device", controllerId_, clientNonce_,
          deviceNonce_, *crypto_, proof) != companion::ProtocolResult::Ok) {
    secureZero(proof, sizeof(proof));
    return false;
  }
  char deviceNonceB64[23]{};
  char proofB64[44]{};
  size_t deviceNonceBytes = 0U;
  size_t proofBytes = 0U;
  const bool encoded =
      encodeB64(deviceNonce_, sizeof(deviceNonce_), deviceNonceB64,
                sizeof(deviceNonceB64), deviceNonceBytes) &&
      encodeB64(proof, sizeof(proof), proofB64, sizeof(proofB64), proofBytes);
  secureZero(proof, sizeof(proof));
  if (!encoded || deviceNonceBytes != 22U || proofBytes != 43U) return false;

  JsonWriter writer(jsonScratch_, companion::kMaximumHandshakeFrameBytes);
  writer.literal("{\"v\":1,\"type\":\"device_hello\","
                 "\"device_nonce_b64\":");
  writeQuoted(writer, deviceNonceB64, deviceNonceBytes);
  writer.literal(",\"proof_b64\":");
  writeQuoted(writer, proofB64, proofBytes);
  writer.literal("}");
  if (!writer.ok() ||
      !transport_->sendBleJson(jsonScratch_, writer.size())) {
    return false;
  }
  state_ = BleSessionState::AwaitingClientAuth;
  return true;
}

bool KitsuBleSession::handleClientAuth(const uint8_t* json,
                                       size_t jsonBytes,
                                       uint32_t) {
  FlatField fields[kMaximumControlFields]{};
  size_t count = 0U;
  static const char* const schema[] = {"v", "type", "proof_b64"};
  uint8_t supplied[32]{};
  if (!parseFlatObject(json, jsonBytes, fields, count) ||
      !schemaMatches(fields, count, schema,
                     sizeof(schema) / sizeof(schema[0])) ||
      !validVersion(fields, count) ||
      !stringFieldEquals(fields, count, "type", "client_auth") ||
      !decodeExact(fields, count, "proof_b64", supplied, sizeof(supplied))) {
    secureZero(supplied, sizeof(supplied));
    return false;
  }
  uint8_t expected[32]{};
  const bool calculated = companion::makeHandshakeProof(
      controllerRoot_, "client", controllerId_, clientNonce_, deviceNonce_,
      *crypto_, expected) == companion::ProtocolResult::Ok;
  const bool valid = calculated && controllerKnown_ &&
                     constantTimeEqual(expected, supplied, sizeof(expected));
  secureZero(expected, sizeof(expected));
  secureZero(supplied, sizeof(supplied));
  if (!valid || companion::deriveBleSessionKeys(
                    controllerRoot_, clientNonce_, deviceNonce_, *crypto_,
                    clientToDeviceKey_, deviceToClientKey_) !=
                    companion::ProtocolResult::Ok) {
    return false;
  }

  uint8_t okProof[32]{};
  if (companion::makeHandshakeProof(
          controllerRoot_, "ok", controllerId_, clientNonce_, deviceNonce_,
          *crypto_, okProof) != companion::ProtocolResult::Ok) {
    secureZero(okProof, sizeof(okProof));
    return false;
  }
  char proofB64[44]{};
  size_t proofBytes = 0U;
  if (!encodeB64(okProof, sizeof(okProof), proofB64, sizeof(proofB64),
                 proofBytes) || proofBytes != 43U) {
    secureZero(okProof, sizeof(okProof));
    return false;
  }
  secureZero(okProof, sizeof(okProof));
  if (!transport_->setBleApplicationAuthenticated(true)) return false;
  JsonWriter writer(jsonScratch_, companion::kMaximumHandshakeFrameBytes);
  writer.literal("{\"v\":1,\"type\":\"device_ok\",\"proof_b64\":");
  writeQuoted(writer, proofB64, proofBytes);
  writer.literal("}");
  if (!writer.ok() ||
      !transport_->sendBleJson(jsonScratch_, writer.size())) {
    transport_->setBleApplicationAuthenticated(false);
    return false;
  }
  secureZero(controllerRoot_, sizeof(controllerRoot_));
  proofFailures_ = 0U;
  expectedClientSequence_ = 1U;
  nextDeviceSequence_ = 1U;
  stateDeadline_ = 0U;
  state_ = BleSessionState::Authenticated;
  return true;
}

bool KitsuBleSession::handlePairRequest(const uint8_t* json,
                                        size_t jsonBytes,
                                        uint32_t nowMillis) {
  FlatField fields[kMaximumControlFields]{};
  size_t count = 0U;
  static const char* const schema[] = {
      "v", "type", "client_nonce_b64", "label", "platform"};
  if (!parseFlatObject(json, jsonBytes, fields, count) ||
      !schemaMatches(fields, count, schema,
                     sizeof(schema) / sizeof(schema[0])) ||
      !validVersion(fields, count) ||
      !stringFieldEquals(fields, count, "type", "pair_request") ||
      !decodeExact(fields, count, "client_nonce_b64", pendingClientNonce_,
                   sizeof(pendingClientNonce_))) {
    return false;
  }
  const FlatField* label = findField(fields, count, "label");
  const FlatField* platform = findField(fields, count, "platform");
  if (!label || label->kind != FlatValueKind::String ||
      label->value.bytes == 0U || label->value.bytes > 96U || !platform ||
      platform->kind != FlatValueKind::String ||
      !validPlatform(platform->value)) {
    return false;
  }
  if (!pairingWindowOpen_ ||
      deadlineReached(nowMillis, pairingWindowDeadline_)) {
    clearPendingPairing();
    sendControlError("pairing_closed");
    return true;
  }
  if (security_->status().controllerCount >= kKitsuControllerCapacity) {
    clearPendingPairing();
    sendControlError("controller_full");
    return true;
  }
  if (!crypto_->randomBytes(pendingDeviceNonce_,
                            sizeof(pendingDeviceNonce_))) {
    return false;
  }

  char deviceNonceB64[23]{};
  size_t deviceNonceBytes = 0U;
  if (!encodeB64(pendingDeviceNonce_, sizeof(pendingDeviceNonce_),
                 deviceNonceB64, sizeof(deviceNonceB64), deviceNonceBytes) ||
      deviceNonceBytes != 22U) {
    return false;
  }
  const uint32_t remaining = pairingWindowDeadline_ - nowMillis;
  JsonWriter writer(jsonScratch_, companion::kMaximumHandshakeFrameBytes);
  writer.literal("{\"v\":1,\"type\":\"pair_pending\","
                 "\"device_nonce_b64\":");
  writeQuoted(writer, deviceNonceB64, deviceNonceBytes);
  writer.literal(",\"expires_in_ms\":");
  writer.unsignedValue(remaining > 60000UL ? 60000UL : remaining);
  writer.literal("}");
  if (!writer.ok() ||
      !transport_->sendBleJson(jsonScratch_, writer.size())) {
    return false;
  }
  state_ = BleSessionState::PairingAwaitingPhysical;
  stateDeadline_ = pairingWindowDeadline_;
  return true;
}

bool KitsuBleSession::confirmPendingPairing(uint32_t nowMillis) {
  if (!begun_ || state_ != BleSessionState::PairingAwaitingPhysical ||
      !pairingWindowOpen_ || deadlineReached(nowMillis, stateDeadline_) ||
      !secureConnections_ || !linkEncrypted_ || !linkAuthenticated_ ||
      !linkBonded_) {
    return false;
  }
  if (security_->generatePendingControllerRoot(
          secureConnections_, linkEncrypted_, linkBonded_, true,
          pendingControllerRoot_) != SecurityResult::Ok) {
    clearPendingPairing();
    sendControlError(security_->status().controllerCount >=
                             kKitsuControllerCapacity
                         ? "controller_full"
                         : "auth_failed");
    return false;
  }

  bool unique = false;
  uint8_t existingRoot[32]{};
  for (uint8_t attempt = 0U; attempt < 8U && !unique; ++attempt) {
    if (!crypto_->randomBytes(pendingControllerId_,
                              sizeof(pendingControllerId_)) ||
        allZero(pendingControllerId_, sizeof(pendingControllerId_))) {
      continue;
    }
    unique = !security_->findControllerRoot(pendingControllerId_,
                                            existingRoot);
    secureZero(existingRoot, sizeof(existingRoot));
  }
  if (!unique) {
    clearPendingPairing();
    failAndClose(nowMillis);
    return false;
  }

  uint8_t proof[32]{};
  if (companion::makePairingProof(
          pendingControllerRoot_, "device", pendingControllerId_, deviceUid_,
          pendingClientNonce_, pendingDeviceNonce_, *crypto_, proof) !=
      companion::ProtocolResult::Ok) {
    secureZero(proof, sizeof(proof));
    clearPendingPairing();
    failAndClose(nowMillis);
    return false;
  }
  char controllerB64[23]{};
  char rootB64[44]{};
  char clientNonceB64[23]{};
  char deviceNonceB64[23]{};
  char proofB64[44]{};
  size_t controllerBytes = 0U;
  size_t rootBytes = 0U;
  size_t clientNonceBytes = 0U;
  size_t deviceNonceBytes = 0U;
  size_t proofBytes = 0U;
  const bool encoded =
      encodeB64(pendingControllerId_, sizeof(pendingControllerId_),
                controllerB64, sizeof(controllerB64), controllerBytes) &&
      encodeB64(pendingControllerRoot_, sizeof(pendingControllerRoot_),
                rootB64, sizeof(rootB64), rootBytes) &&
      encodeB64(pendingClientNonce_, sizeof(pendingClientNonce_),
                clientNonceB64, sizeof(clientNonceB64), clientNonceBytes) &&
      encodeB64(pendingDeviceNonce_, sizeof(pendingDeviceNonce_),
                deviceNonceB64, sizeof(deviceNonceB64), deviceNonceBytes) &&
      encodeB64(proof, sizeof(proof), proofB64, sizeof(proofB64), proofBytes);
  secureZero(proof, sizeof(proof));
  if (!encoded || controllerBytes != 22U || rootBytes != 43U ||
      clientNonceBytes != 22U || deviceNonceBytes != 22U ||
      proofBytes != 43U) {
    clearPendingPairing();
    failAndClose(nowMillis);
    return false;
  }

  JsonWriter writer(jsonScratch_, companion::kMaximumHandshakeFrameBytes);
  writer.literal("{\"v\":1,\"type\":\"pair_grant\","
                 "\"controller_id_b64\":");
  writeQuoted(writer, controllerB64, controllerBytes);
  writer.literal(",\"root_b64\":");
  writeQuoted(writer, rootB64, rootBytes);
  writer.literal(",\"device_uid\":");
  writeQuoted(writer, deviceUid_, 6U);
  writer.literal(",\"client_nonce_b64\":");
  writeQuoted(writer, clientNonceB64, clientNonceBytes);
  writer.literal(",\"device_nonce_b64\":");
  writeQuoted(writer, deviceNonceB64, deviceNonceBytes);
  writer.literal(",\"proof_b64\":");
  writeQuoted(writer, proofB64, proofBytes);
  writer.literal("}");
  if (!writer.ok() ||
      !transport_->sendBleJson(jsonScratch_, writer.size())) {
    clearPendingPairing();
    failAndClose(nowMillis);
    return false;
  }
  state_ = BleSessionState::PairingAwaitingCommit;
  return true;
}

bool KitsuBleSession::handlePairCommit(const uint8_t* json,
                                       size_t jsonBytes,
                                       uint32_t nowMillis) {
  FlatField fields[kMaximumControlFields]{};
  size_t count = 0U;
  static const char* const schema[] = {"v", "type", "proof_b64"};
  uint8_t supplied[32]{};
  if (!parseFlatObject(json, jsonBytes, fields, count) ||
      !schemaMatches(fields, count, schema,
                     sizeof(schema) / sizeof(schema[0])) ||
      !validVersion(fields, count) ||
      !stringFieldEquals(fields, count, "type", "pair_commit") ||
      !decodeExact(fields, count, "proof_b64", supplied, sizeof(supplied))) {
    secureZero(supplied, sizeof(supplied));
    return false;
  }
  uint8_t expected[32]{};
  const bool calculated = companion::makePairingProof(
      pendingControllerRoot_, "client", pendingControllerId_, deviceUid_,
      pendingClientNonce_, pendingDeviceNonce_, *crypto_, expected) ==
      companion::ProtocolResult::Ok;
  const bool verified = calculated &&
      constantTimeEqual(expected, supplied, sizeof(expected));
  secureZero(expected, sizeof(expected));
  secureZero(supplied, sizeof(supplied));
  if (!verified) return false;

  uint8_t okProof[32]{};
  if (companion::makePairingProof(
          pendingControllerRoot_, "ok", pendingControllerId_, deviceUid_,
          pendingClientNonce_, pendingDeviceNonce_, *crypto_, okProof) !=
      companion::ProtocolResult::Ok) {
    secureZero(okProof, sizeof(okProof));
    return false;
  }
  const SecurityResult committed = security_->commitControllerAfterPairing(
      pendingControllerId_, pendingControllerRoot_, secureConnections_,
      linkEncrypted_, linkBonded_, true, true);
  if (committed != SecurityResult::Ok) {
    secureZero(okProof, sizeof(okProof));
    clearPendingPairing();
    sendControlError(committed == SecurityResult::ControllerTableFull
                         ? "controller_full"
                         : "auth_failed");
    return false;
  }

  char proofB64[44]{};
  size_t proofBytes = 0U;
  const bool encoded = encodeB64(okProof, sizeof(okProof), proofB64,
                                 sizeof(proofB64), proofBytes);
  secureZero(okProof, sizeof(okProof));
  if (!encoded || proofBytes != 43U) {
    clearPendingPairing();
    failAndClose(nowMillis);
    return false;
  }
  JsonWriter writer(jsonScratch_, companion::kMaximumHandshakeFrameBytes);
  writer.literal("{\"v\":1,\"type\":\"pair_ok\",\"proof_b64\":");
  writeQuoted(writer, proofB64, proofBytes);
  writer.literal("}");
  const bool sent = writer.ok() &&
      transport_->sendBleJson(jsonScratch_, writer.size());
  clearPendingPairing();
  state_ = BleSessionState::AwaitingHello;
  if (!sent) failAndClose(nowMillis);
  return sent;
}

bool KitsuBleSession::sendAuthenticated(
    companion::EnvelopeChannel channel, const uint8_t requestId[16],
    const char* operation, const uint8_t* payload, size_t payloadBytes) {
  if (state_ != BleSessionState::Authenticated || !requestId || !operation ||
      nextDeviceSequence_ == UINT64_MAX) {
    return false;
  }
  uint8_t nonce[16]{};
  if (!crypto_->randomBytes(nonce, sizeof(nonce))) return false;
  size_t jsonBytes = 0U;
  const companion::ProtocolResult encoded = companion::encodeEnvelope(
      channel, nextDeviceSequence_, nonce, requestId, operation, payload,
      payloadBytes, deviceToClientKey_, *crypto_, jsonScratch_,
      sizeof(jsonScratch_), jsonBytes);
  secureZero(nonce, sizeof(nonce));
  if (encoded != companion::ProtocolResult::Ok ||
      !transport_->sendBleJson(jsonScratch_, jsonBytes)) {
    return false;
  }
  ++nextDeviceSequence_;
  return true;
}

bool KitsuBleSession::handleAuthenticatedEnvelope(const uint8_t* json,
                                                   size_t jsonBytes,
                                                   uint32_t nowMillis) {
  companion::DecodedEnvelope request{};
  const companion::ProtocolResult decoded =
      companion::decodeAndVerifyEnvelope(
          json, jsonBytes, clientToDeviceKey_,
          companion::EnvelopeChannel::Request, expectedClientSequence_,
          *crypto_, request, payloadScratch_, sizeof(payloadScratch_));
  if (decoded != companion::ProtocolResult::Ok ||
      !operationAllowed(request.operation) ||
      expectedClientSequence_ == UINT64_MAX) {
    backoffUntil_ = nowMillis + kBleControllerBackoffMs;
    failAndClose(nowMillis);
    return false;
  }
  ++expectedClientSequence_;
  size_t responseBytes = 0U;
  if (!operations_->handleBleRequest(
          request, payloadScratch_, request.payloadBytes, responseScratch_,
          sizeof(responseScratch_), responseBytes) ||
      responseBytes == 0U || responseBytes > sizeof(responseScratch_)) {
    static const uint8_t rejected[] =
        "{\"ok\":false,\"error\":\"request_rejected\"}";
    memcpy(responseScratch_, rejected, sizeof(rejected) - 1U);
    responseBytes = sizeof(rejected) - 1U;
  }
  if (!sendAuthenticated(companion::EnvelopeChannel::Response,
                         request.requestId, request.operation,
                         responseScratch_, responseBytes)) {
    failAndClose(nowMillis);
    return false;
  }
  return true;
}

void KitsuBleSession::onFrame(const uint8_t* json, size_t jsonBytes,
                              uint32_t nowMillis) {
  if (!begun_ || !json || jsonBytes == 0U ||
      !secureConnections_ || !linkEncrypted_ || !linkAuthenticated_ ||
      !linkBonded_) {
    failAndClose(nowMillis);
    return;
  }
  if ((state_ == BleSessionState::AwaitingHello ||
       state_ == BleSessionState::AwaitingClientAuth) &&
      deadlineReached(nowMillis, stateDeadline_)) {
    sendControlError("timeout");
    failAndClose(nowMillis);
    return;
  }

  bool handled = false;
  switch (state_) {
    case BleSessionState::AwaitingHello: {
      FlatField fields[kMaximumControlFields]{};
      size_t count = 0U;
      if (parseFlatObject(json, jsonBytes, fields, count) &&
          validVersion(fields, count) &&
          stringFieldEquals(fields, count, "type", "client_hello")) {
        handled = handleClientHello(json, jsonBytes, nowMillis);
      } else if (parseFlatObject(json, jsonBytes, fields, count) &&
                 validVersion(fields, count) &&
                 stringFieldEquals(fields, count, "type", "pair_request")) {
        handled = handlePairRequest(json, jsonBytes, nowMillis);
      }
      break;
    }
    case BleSessionState::AwaitingClientAuth:
      handled = handleClientAuth(json, jsonBytes, nowMillis);
      break;
    case BleSessionState::Authenticated:
      handled = handleAuthenticatedEnvelope(json, jsonBytes, nowMillis);
      break;
    case BleSessionState::PairingAwaitingCommit:
      handled = handlePairCommit(json, jsonBytes, nowMillis);
      break;
    case BleSessionState::PairingAwaitingPhysical:
    case BleSessionState::Closing:
    case BleSessionState::Backoff:
    case BleSessionState::Disconnected:
    default:
      break;
  }
  if (!handled && state_ != BleSessionState::Closing) {
    failProof(nowMillis);
  }
}

void KitsuBleSession::loop(uint32_t nowMillis) {
  if (!begun_) return;
  if (pairingWindowOpen_ &&
      deadlineReached(nowMillis, pairingWindowDeadline_)) {
    pairingWindowOpen_ = false;
    pairingWindowDeadline_ = 0U;
  }
  if ((state_ == BleSessionState::AwaitingHello ||
       state_ == BleSessionState::AwaitingClientAuth ||
       state_ == BleSessionState::PairingAwaitingPhysical ||
       state_ == BleSessionState::PairingAwaitingCommit) &&
      stateDeadline_ != 0U && deadlineReached(nowMillis, stateDeadline_)) {
    clearPendingPairing();
    sendControlError("timeout");
    failAndClose(nowMillis);
  }
  if (state_ == BleSessionState::Closing && closeAt_ != 0U &&
      deadlineReached(nowMillis, closeAt_)) {
    closeAt_ = 0U;
    transport_->disconnectBle();
  }
  if (state_ == BleSessionState::Backoff && backoffUntil_ != 0U &&
      deadlineReached(nowMillis, backoffUntil_)) {
    backoffUntil_ = 0U;
    state_ = BleSessionState::Disconnected;
  }
}

void KitsuBleSession::cancelPendingPairing() {
  if (!begun_) return;
  clearPendingPairing();
  if (state_ == BleSessionState::PairingAwaitingPhysical ||
      state_ == BleSessionState::PairingAwaitingCommit) {
    state_ = BleSessionState::AwaitingHello;
  }
}

bool KitsuBleSession::sendEvent(const char* operation,
                                const uint8_t* payload,
                                size_t payloadBytes) {
  uint8_t requestId[16]{};
  if (!crypto_ || !crypto_->randomBytes(requestId, sizeof(requestId))) {
    return false;
  }
  const bool sent = sendAuthenticated(companion::EnvelopeChannel::Event,
                                      requestId, operation, payload,
                                      payloadBytes);
  secureZero(requestId, sizeof(requestId));
  return sent;
}

BleSessionStatus KitsuBleSession::status(uint32_t nowMillis) const {
  BleSessionStatus output{};
  output.state = state_;
  output.begun = begun_;
  output.secureLink = secureConnections_ && linkEncrypted_ &&
                      linkAuthenticated_ && linkBonded_;
  output.pairingWindowOpen = pairingWindowOpen_ &&
      !deadlineReached(nowMillis, pairingWindowDeadline_);
  output.physicalConfirmationPending =
      state_ == BleSessionState::PairingAwaitingPhysical;
  output.applicationAuthenticated =
      state_ == BleSessionState::Authenticated;
  output.proofFailures = proofFailures_;
  output.nextClientSequence = expectedClientSequence_;
  output.nextDeviceSequence = nextDeviceSequence_;
  if (stateDeadline_ != 0U && !deadlineReached(nowMillis, stateDeadline_)) {
    output.deadlineRemainingMs = stateDeadline_ - nowMillis;
  }
  if (backoffUntil_ != 0U && !deadlineReached(nowMillis, backoffUntil_)) {
    output.backoffRemainingMs = backoffUntil_ - nowMillis;
  }
  return output;
}

}  // namespace connectivity
}  // namespace kitsu868
