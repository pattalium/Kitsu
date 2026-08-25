#include "kitsu_mesh_transport.h"

#include <cstring>

namespace kitsu868 {
namespace mesh {
namespace {

constexpr size_t kInjectedAdvertCapacity = 8U;
ReceivedAdvert injectedAdverts[kInjectedAdvertCapacity]{};
size_t injectedAdvertRead = 0U;
size_t injectedAdvertWrite = 0U;
size_t injectedAdvertCount = 0U;
uint8_t radioIo[512]{};

bool popInjectedAdvert(ReceivedAdvert& output) {
  if (injectedAdvertCount == 0U) return false;
  output = injectedAdverts[injectedAdvertRead];
  injectedAdvertRead = (injectedAdvertRead + 1U) % kInjectedAdvertCapacity;
  --injectedAdvertCount;
  return true;
}

}  // namespace

struct KitsuMeshTransport::Impl {
  Settings settings{};
  ClientIdentity identity{};
  bool identityReady = false;
  bool active = true;
  bool transmit = false;
  bool epochValid = false;
  uint32_t epoch = 0U;
  ContactRecord contacts[kMeshContactCapacity]{};
  size_t contactCount = 0U;
  ChannelRecord channels[kMeshChannelCapacity]{};
  MessagingStorageStatus storage{true,
                                 2U,
                                 false,
                                 false,
                                 1U,
                                 MessagingStorageWriteResult::NotAttempted,
                                 MessagingStorageReason::Ready};
  uint32_t receivedAdverts = 0U;
};

const char* transportStatusName(TransportStatus status) {
  switch (status) {
    case TransportStatus::Ok: return "ok";
    case TransportStatus::Disabled: return "disabled";
    case TransportStatus::TimeUnset: return "time_unset";
    case TransportStatus::TxLocked: return "tx_locked";
    default: return "injected";
  }
}

const char* messagingStorageWriteResultName(MessagingStorageWriteResult) {
  return "not_attempted";
}

const char* messagingStorageReasonName(MessagingStorageReason reason) {
  return reason == MessagingStorageReason::Ready ? "ready" : "injected";
}

KitsuMeshTransport::KitsuMeshTransport() : impl_(new Impl()) {}
KitsuMeshTransport::~KitsuMeshTransport() { delete impl_; }

TransportStatus KitsuMeshTransport::begin(const Settings& settings,
                                          const ClientIdentity& identity) {
  impl_->settings = settings;
  impl_->identity = identity;
  impl_->identityReady = true;
  // This scaffold represents an attached injected-data radio even while the
  // persisted production radio switch is at its safe disabled default.
  impl_->active = true;
  return TransportStatus::Ok;
}

TransportStatus KitsuMeshTransport::applySettings(const Settings& settings) {
  impl_->settings = settings;
  impl_->transmit = false;
  return TransportStatus::Ok;
}

void KitsuMeshTransport::loop() {}
bool KitsuMeshTransport::identityReady() const { return impl_->identityReady; }
bool KitsuMeshTransport::active() const { return impl_->active; }
int16_t KitsuMeshTransport::radioCode() const { return 0; }
uint32_t KitsuMeshTransport::profileId() const {
  return impl_->settings.radio.profileId;
}

TransportStatus KitsuMeshTransport::setEpoch(uint32_t epochSeconds) {
  if (epochSeconds == 0U) return TransportStatus::InvalidTime;
  impl_->epoch = epochSeconds;
  impl_->epochValid = true;
  return TransportStatus::Ok;
}

bool KitsuMeshTransport::timeValid() const { return impl_->epochValid; }
uint32_t KitsuMeshTransport::currentEpoch() const { return impl_->epoch; }

bool KitsuMeshTransport::unlockTransmit(const Settings& settings,
                                        bool explicitUserApproval) {
  impl_->transmit = explicitUserApproval && settings.enabled;
  return impl_->transmit;
}

void KitsuMeshTransport::lockTransmit() { impl_->transmit = false; }
bool KitsuMeshTransport::transmitAllowed(const Settings&) const {
  return impl_->transmit;
}

TransportStatus KitsuMeshTransport::exportSignedAdvert(
    const Settings&, const CurrentLocationOnce&, char*, size_t,
    size_t&) {
  return TransportStatus::Disabled;
}

TransportStatus KitsuMeshTransport::introduce(
    AdvertScope, const Settings&, const CurrentLocationOnce&) {
  return TransportStatus::Ok;
}

TransportStatus KitsuMeshTransport::advertiseReadiness(
    const Settings&, const CurrentLocationOnce&, uint32_t& retryAfterMs) const {
  retryAfterMs = 0U;
  return TransportStatus::Ok;
}

TransportStatus KitsuMeshTransport::introduceOnce(
    AdvertScope scope, const Settings& settings,
    const CurrentLocationOnce& current, bool explicitUserApproval) {
  return explicitUserApproval ? introduce(scope, settings, current)
                              : TransportStatus::TxLocked;
}

bool KitsuMeshTransport::takeAdvert(ReceivedAdvert& output) {
  if (!popInjectedAdvert(output)) return false;
  ++impl_->receivedAdverts;
  return true;
}

bool KitsuMeshTransport::lastFloodAdvertStatus(FloodAdvertStatus&) {
  return false;
}
bool KitsuMeshTransport::takeFloodAdvertStatusChanged() { return false; }
bool KitsuMeshTransport::lastNearbyAdvertStatus(NearbyAdvertStatus&) {
  return false;
}
bool KitsuMeshTransport::takeNearbyAdvertStatusChanged() { return false; }

bool KitsuMeshTransport::publicKeyHex(char* output,
                                      size_t outputCapacity) const {
  if (!output || outputCapacity < 65U) return false;
  std::memset(output, '0', 64U);
  output[64] = '\0';
  return true;
}

size_t KitsuMeshTransport::contactCount() const { return impl_->contactCount; }

bool KitsuMeshTransport::contactAt(size_t index, ContactRecord& output) const {
  if (index >= impl_->contactCount) return false;
  output = impl_->contacts[index];
  return true;
}

TransportStatus KitsuMeshTransport::upsertContact(
    const uint8_t publicKey[32], const char* name, uint8_t type) {
  return stageObservedContact(publicKey, name, type, 0U);
}

TransportStatus KitsuMeshTransport::stageObservedContact(
    const uint8_t publicKey[32], const char* name, uint8_t type,
    uint32_t advertTimestamp) {
  if (!publicKey) return TransportStatus::InvalidArgument;
  if (impl_->contactCount >= kMeshContactCapacity) {
    return TransportStatus::ContactTableFull;
  }
  ContactRecord& contact = impl_->contacts[impl_->contactCount++];
  std::memcpy(contact.publicKey, publicKey, sizeof(contact.publicKey));
  std::strncpy(contact.name, name ? name : "Injected",
               sizeof(contact.name) - 1U);
  contact.type = type;
  contact.lastAdvertTimestamp = advertTimestamp;
  return TransportStatus::Ok;
}

TransportStatus KitsuMeshTransport::removeContact(const uint8_t[32]) {
  return TransportStatus::ContactNotFound;
}

bool KitsuMeshTransport::channelAt(uint8_t slot, ChannelRecord& output) const {
  if (slot >= kMeshChannelCapacity) return false;
  output = impl_->channels[slot];
  return true;
}

TransportStatus KitsuMeshTransport::setChannel(
    uint8_t slot, const char* name, const uint8_t[32],
    ChannelRegionScope regionScope) {
  if (slot >= kMeshChannelCapacity) return TransportStatus::ChannelNotFound;
  ChannelRecord& channel = impl_->channels[slot];
  channel = ChannelRecord{};
  channel.slot = slot;
  channel.configured = true;
  channel.regionScope = regionScope;
  std::strncpy(channel.name, name ? name : "Injected",
               sizeof(channel.name) - 1U);
  return TransportStatus::Ok;
}

TransportStatus KitsuMeshTransport::clearChannel(uint8_t slot) {
  if (slot >= kMeshChannelCapacity) return TransportStatus::ChannelNotFound;
  impl_->channels[slot] = ChannelRecord{};
  return TransportStatus::Ok;
}

TransportStatus KitsuMeshTransport::sendDirectText(
    const Settings&, const uint8_t[32], const char*, uint8_t,
    uint32_t& queuedTimestamp, uint32_t& expectedAck, MessageRoute& route) {
  queuedTimestamp = impl_->epoch;
  expectedAck = impl_->epoch ^ 0xa5a5U;
  route = MessageRoute::Flood;
  return impl_->transmit ? TransportStatus::Ok : TransportStatus::TxLocked;
}

TransportStatus KitsuMeshTransport::sendChannelText(
    const Settings&, uint8_t, const char*, uint32_t& queuedTimestamp) {
  queuedTimestamp = impl_->epoch;
  return impl_->transmit ? TransportStatus::Ok : TransportStatus::TxLocked;
}

TransportStatus KitsuMeshTransport::sendDirectTextOnce(
    const Settings& settings, const uint8_t publicKey[32], const char* text,
    uint8_t attempt, bool explicitUserApproval, uint32_t& queuedTimestamp,
    uint32_t& expectedAck, MessageRoute& route) {
  if (!explicitUserApproval) return TransportStatus::TxLocked;
  const bool previous = impl_->transmit;
  impl_->transmit = true;
  const TransportStatus result = sendDirectText(
      settings, publicKey, text, attempt, queuedTimestamp, expectedAck, route);
  impl_->transmit = previous;
  return result;
}

TransportStatus KitsuMeshTransport::sendChannelTextOnce(
    const Settings& settings, uint8_t slot, const char* text,
    bool explicitUserApproval, uint32_t& queuedTimestamp) {
  if (!explicitUserApproval) return TransportStatus::TxLocked;
  const bool previous = impl_->transmit;
  impl_->transmit = true;
  const TransportStatus result =
      sendChannelText(settings, slot, text, queuedTimestamp);
  impl_->transmit = previous;
  return result;
}

bool KitsuMeshTransport::takeMessage(ReceivedMessage&) { return false; }
bool KitsuMeshTransport::takeDelivery(DeliveryEvent&) { return false; }
bool KitsuMeshTransport::repeatDiagnostics(RepeatDiagnostics& output) const {
  output = RepeatDiagnostics{};
  return true;
}
bool KitsuMeshTransport::directSendPending() const { return false; }
bool KitsuMeshTransport::sendInProgress() const { return false; }
uint32_t KitsuMeshTransport::droppedMessageCount() const { return 0U; }
uint32_t KitsuMeshTransport::droppedDeliveryCount() const { return 0U; }
bool KitsuMeshTransport::messagingStorageReady() const { return true; }
bool KitsuMeshTransport::messagingStorageStatus(
    MessagingStorageStatus& output) const {
  output = impl_->storage;
  return true;
}
TransportStatus KitsuMeshTransport::resetMessagingState() {
  return TransportStatus::Ok;
}
uint32_t KitsuMeshTransport::receivedAdvertCount() const {
  return impl_->receivedAdverts;
}
uint32_t KitsuMeshTransport::droppedAdvertCount() const { return 0U; }
uint32_t KitsuMeshTransport::queuedAdvertCount() const {
  return static_cast<uint32_t>(injectedAdvertCount);
}

extern "C" uint8_t* kitsu_emulator_radio_io_buffer() { return radioIo; }
extern "C" uint32_t kitsu_emulator_radio_io_capacity() {
  return sizeof(radioIo);
}
extern "C" uint32_t kitsu_emulator_radio_inject_advert(
    uint32_t nameBytes, uint32_t type, uint32_t timestamp,
    uint32_t hasLocation, int32_t latitudeE6, int32_t longitudeE6,
    float rssi, float snr) {
  if (nameBytes > 32U || 32U + nameBytes > sizeof(radioIo) ||
      injectedAdvertCount >= kInjectedAdvertCapacity) {
    return 0U;
  }
  ReceivedAdvert advert{};
  advert.type = static_cast<uint8_t>(type);
  advert.timestamp = timestamp;
  std::memcpy(advert.publicKey, radioIo, sizeof(advert.publicKey));
  std::memcpy(advert.publicKeyPrefix, radioIo,
              sizeof(advert.publicKeyPrefix));
  std::memcpy(advert.name, radioIo + 32U, nameBytes);
  advert.name[nameBytes] = '\0';
  advert.hasLocation = hasLocation != 0U;
  advert.location.latitudeE6 = latitudeE6;
  advert.location.longitudeE6 = longitudeE6;
  advert.kitsuNamed = nameBytes >= 5U &&
      std::memcmp(advert.name, "Kitsu", 5U) == 0;
  advert.rssi = rssi;
  advert.snr = snr;
  injectedAdverts[injectedAdvertWrite] = advert;
  injectedAdvertWrite =
      (injectedAdvertWrite + 1U) % kInjectedAdvertCapacity;
  ++injectedAdvertCount;
  return 1U;
}

}  // namespace mesh
}  // namespace kitsu868
