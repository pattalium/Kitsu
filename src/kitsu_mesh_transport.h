#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_mesh_config.h"
#include "kitsu_mesh_messages.h"

namespace kitsu868 {
namespace mesh {

// The largest MeshCore v1 packet is 255 bytes.  Hex encoding plus a trailing
// NUL therefore needs 511 bytes.
constexpr size_t kAdvertHexCapacity = 511;
constexpr size_t kMeshContactCapacity = 12;
constexpr size_t kMeshChannelCapacity = 4;
// Conservative app-facing bounds; inbound protocol decode still accepts the
// complete MeshCore v1.17.1 160-byte text field.
constexpr size_t kDirectOutboundTextBytes = 128;
constexpr size_t kChannelOutboundTextBytes = 128;

enum class AdvertScope : uint8_t {
  Nearby = 0,
  Flood = 1,
};

enum class TransportStatus : uint8_t {
  Ok = 0,
  Disabled,
  NoProfile,
  InvalidSettings,
  InvalidIdentity,
  IdentityStorageFailed,
  RadioInitFailed,
  TimeUnset,
  InvalidTime,
  LocationUnavailable,
  TxLocked,
  PacketPoolFull,
  OutputTooSmall,
  InvalidArgument,
  ContactNotFound,
  ContactNotClient,
  ContactTableFull,
  ChannelNotFound,
  TextTooLong,
  SendBusy,
  MessagingStorageFailed,
};

const char* transportStatusName(TransportStatus status);

// A signed, verified MeshCore advertisement observed by the transport.  The
// firmware remains a Client and never forwards packets.  This compact event
// is deliberately independent of upstream MeshCore classes so the rest of
// Kitsu does not depend on protocol internals.
struct ReceivedAdvert {
  uint8_t type = 0;
  uint32_t timestamp = 0;
  // Complete verified MeshCore identity.  Prefixes remain presentation-only;
  // durable discovery and companion APIs must never deduplicate on 32 bits.
  uint8_t publicKey[32]{};
  uint8_t publicKeyPrefix[4]{};
  char name[33]{};
  bool hasLocation = false;
  Coordinates location{};
  bool kitsuNamed = false;
  float rssi = 0.0f;
  float snr = 0.0f;
};

enum class MessageKind : uint8_t {
  Direct = 0,
  Channel = 1,
};

enum class MessageRoute : uint8_t {
  Flood = 0,
  Direct = 1,
};

enum class DeliveryState : uint8_t {
  Delivered = 0,
  TimedOut = 1,
  Cancelled = 2,
  TxFailed = 3,
  Sent = 4,
};

// Full contact keys are exposed to the local companion app so one-byte MeshCore
// routing hashes never become user-visible identity.  Shared secrets and
// channel secrets are intentionally absent from every public record.
struct ContactRecord {
  uint8_t publicKey[32]{};
  char name[33]{};
  uint8_t type = 0;
  bool pathKnown = false;
  uint32_t lastAdvertTimestamp = 0;
  bool pinned = false;
};

struct ChannelRecord {
  uint8_t slot = 0;
  char name[33]{};
  bool configured = false;
  uint8_t hash = 0;
};

struct ReceivedMessage {
  MessageKind kind = MessageKind::Direct;
  MessageRoute route = MessageRoute::Flood;
  // Direct sender identity is authenticated by ECDH/MAC.  MeshCore group
  // channel text proves channel-key possession only; its "sender: " prefix is
  // user-controlled and therefore remains explicitly unauthenticated.
  bool senderAuthenticated = false;
  uint32_t timestamp = 0;
  uint8_t publicKey[32]{};  // Direct sender; zero for channel traffic.
  uint8_t channelSlot = 0xff;
  char senderName[33]{};
  char text[kMeshTextCapacity]{};
  uint8_t hopCount = 0;
  float rssi = 0.0f;
  float snr = 0.0f;
};

struct DeliveryEvent {
  MessageKind kind = MessageKind::Direct;
  DeliveryState state = DeliveryState::Delivered;
  MessageRoute route = MessageRoute::Flood;
  uint32_t expectedAck = 0;
  uint8_t recipientPublicKey[32]{};
  uint8_t channelSlot = 0xff;
};

class KitsuMeshTransport {
 public:
  KitsuMeshTransport();
  ~KitsuMeshTransport();

  KitsuMeshTransport(const KitsuMeshTransport&) = delete;
  KitsuMeshTransport& operator=(const KitsuMeshTransport&) = delete;

  // Loads or creates the persistent Ed25519 MeshCore identity.  When Mesh is
  // enabled, it also initializes the SX1262 on the selected exact profile and
  // enters receive mode.  A disabled configuration still prepares signing so
  // status and identity remain available without energizing the radio.
  TransportStatus begin(const Settings& settings,
                        const ClientIdentity& identity);

  // Applies a persisted configuration.  Call lockTransmit() before changing
  // it; this function also locks and clears pending outbound packets itself.
  TransportStatus applySettings(const Settings& settings);
  void loop();

  bool identityReady() const;
  bool active() const;
  int16_t radioCode() const;
  uint32_t profileId() const;

  // MeshCore adverts carry epoch seconds.  The Heltec V3 has no RTC, so the
  // phone supplies current time for this boot.  Time is intentionally not
  // persisted.
  TransportStatus setEpoch(uint32_t epochSeconds);
  bool timeValid() const;
  uint32_t currentEpoch() const;

  // General TX is guarded twice: persisted policy plus an exact-profile,
  // volatile session gate. Every boot starts locked. A successfully
  // authenticated inbound direct TXT may receive one rate-limited standard
  // ACK/PATH through the separate packet-scoped permit below this API; that
  // never opens the general gate.
  bool unlockTransmit(const Settings& settings, bool explicitUserApproval);
  void lockTransmit();
  bool transmitAllowed(const Settings& settings) const;

  // Constructs a standard, signed Client advert and returns its complete
  // MeshCore wire packet as uppercase hex for the phone's map uploader.  This
  // performs no RF transmission and never exposes the private key.
  TransportStatus exportSignedAdvert(const Settings& settings,
                                     const CurrentLocationOnce& current,
                                     char* outputHex,
                                     size_t outputCapacity,
                                     size_t& outputLength);

  // Queues a standard Client advert for either direct zero-hop introduction
  // or normal flood routing.  No custom node type or reserved feature bits are
  // used.  The caller clears CurrentOnce only when Ok is returned.
  TransportStatus introduce(AdvertScope scope, const Settings& settings,
                            const CurrentLocationOnce& current);

  bool takeAdvert(ReceivedAdvert& output);
  bool publicKeyHex(char* output, size_t outputCapacity) const;

  // Contact/channel configuration is app-facing and persisted separately from
  // message contents.  Contact names and keys may come from a QR/card parser in
  // the phone app.  Channel secrets are write-only at this boundary.
  size_t contactCount() const;
  bool contactAt(size_t index, ContactRecord& output) const;
  TransportStatus upsertContact(const uint8_t publicKey[32],
                                const char* name, uint8_t type);
  // Rehydrates a verified Client advert from the encrypted discovery journal
  // into the RAM contact table. It never pins or writes NVS, so an explicit
  // send can use a previously observed full identity after reboot without
  // converting inbound RF into flash writes.
  TransportStatus stageObservedContact(const uint8_t publicKey[32],
                                       const char* name, uint8_t type,
                                       uint32_t advertTimestamp);
  TransportStatus removeContact(const uint8_t publicKey[32]);

  bool channelAt(uint8_t slot, ChannelRecord& output) const;
  TransportStatus setChannel(uint8_t slot, const char* name,
                             const uint8_t secret[32]);
  TransportStatus clearChannel(uint8_t slot);

  // Direct messages use a known full public key and learn a direct path from
  // the standard MeshCore ACK/path return.  Channel messages always flood.
  // Both require current phone time and the existing explicitly unlocked
  // general session gate. At most one direct delivery report is pending at a
  // time.
  TransportStatus sendDirectText(const Settings& settings,
                                 const uint8_t recipientPublicKey[32],
                                 const char* text, uint8_t attempt,
                                 uint32_t& queuedTimestamp,
                                 uint32_t& expectedAck,
                                 MessageRoute& route);
  TransportStatus sendChannelText(const Settings& settings, uint8_t slot,
                                  const char* text,
                                  uint32_t& queuedTimestamp);
  // Authenticated companion actions use a packet-scoped permit instead of
  // opening the general session gate. The permit is consumed by exactly the
  // next physical send attempt, expires quickly, and is revoked on every
  // lock/profile change. explicitUserApproval must represent a deliberate
  // owner action received over an authenticated local companion session.
  TransportStatus sendDirectTextOnce(
      const Settings& settings, const uint8_t recipientPublicKey[32],
      const char* text, uint8_t attempt, bool explicitUserApproval,
      uint32_t& queuedTimestamp, uint32_t& expectedAck,
      MessageRoute& route);
  TransportStatus sendChannelTextOnce(
      const Settings& settings, uint8_t slot, const char* text,
      bool explicitUserApproval, uint32_t& queuedTimestamp);
  bool takeMessage(ReceivedMessage& output);
  bool takeDelivery(DeliveryEvent& output);
  bool directSendPending() const;
  bool sendInProgress() const;
  uint32_t droppedMessageCount() const;
  uint32_t droppedDeliveryCount() const;

  bool messagingStorageReady() const;
  TransportStatus resetMessagingState();

  uint32_t receivedAdvertCount() const;
  uint32_t droppedAdvertCount() const;
  uint32_t queuedAdvertCount() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace mesh
}  // namespace kitsu868
