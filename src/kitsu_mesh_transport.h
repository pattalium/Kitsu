#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kitsu_advert_repeat_tracker.h"
#include "kitsu_channel_repeat_tracker.h"
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
// A successful owner-requested advert starts a short volatile admission
// cooldown. MeshCore's Dispatcher independently enforces the stronger 1%
// long-term airtime budget at the radio scheduler.
constexpr uint32_t kMeshAdvertiseCooldownMs = 30000UL;

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
  AdvertiseCooldown,
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

// Outbound channel routing is legacy-unscoped unless the owner explicitly
// imported/provisioned one supported region.  Radio profile selection never
// implies a transport region and inbound channel decryption remains route-
// agnostic.
enum class ChannelRegionScope : uint8_t {
  Legacy = 0,
  Eu = 1,
};

// Messaging persistence is intentionally reported separately from radio/chat
// availability. A validated legacy record remains usable without any boot-time
// write, while an explicit owner mutation promotes it transactionally to the
// compact alternating v2 records.
enum class MessagingStorageWriteResult : uint8_t {
  NotAttempted = 0,
  Saved,
  OpenFailed,
  ClearFailed,
  WriteFailed,
  VerifyFailed,
  Ambiguous,
};

enum class MessagingStorageReason : uint8_t {
  Ready = 0,
  LegacyMigrationPending,
  State2InvalidLegacyUsable,
  CompactPeerInvalid,
  CleanupPending,
  FreshInitializationPending,
  NamespaceOpenFailed,
  ReadFailed,
  MissingRecord,
  OrphanedLegacyRecord,
  State2Invalid,
  InvalidRecord,
  WriteFailed,
  VerifyFailed,
  CommitAmbiguous,
};

struct MessagingStorageStatus {
  bool usable = false;
  uint16_t persistedSchema = 0U;
  bool migrationPending = false;
  bool cleanupPending = false;
  uint32_t generation = 0U;
  MessagingStorageWriteResult lastWriteResult =
      MessagingStorageWriteResult::NotAttempted;
  MessagingStorageReason reason = MessagingStorageReason::MissingRecord;
};

const char* messagingStorageWriteResultName(
    MessagingStorageWriteResult result);
const char* messagingStorageReasonName(MessagingStorageReason reason);

enum class DeliveryState : uint8_t {
  Delivered = 0,
  TimedOut = 1,
  Cancelled = 2,
  TxFailed = 3,
  Sent = 4,
  // A copy of an already-sent flood channel packet returned with at least
  // one path hop. This is observation evidence, never an ACK or delivery.
  RepeatObserved = 5,
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
  ChannelRegionScope regionScope = ChannelRegionScope::Legacy;
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
  // The exact unique MeshCore message timestamp binds lifecycle updates to
  // one journal row even when multiple recent sends share a channel.
  uint32_t messageTimestamp = 0;
  uint32_t expectedAck = 0;
  uint8_t recipientPublicKey[32]{};
  uint8_t channelSlot = 0xff;
  // Available only for a delivered direct message. For a direct-routed send,
  // this is the number of path hashes used by the transmitted packet. For a
  // flood send, it is the authenticated PATH/ACK's copy of the received
  // packet path. It is a route relay count, never a count of every repeater
  // that may have overheard the RF packet.
  bool repeaterCountKnown = false;
  uint8_t repeaterCount = 0;
  // Outbound channel-only count of matching rebroadcast packet copies heard
  // locally. This is not a count of unique repeaters or recipients.
  bool repeatCountKnown = false;
  uint8_t repeatCount = 0;
  // Defined only with repeatCountKnown for an outbound sent channel message.
  // A false transition retains the final count after the 120-second window.
  bool repeatObservationOpen = false;
  uint8_t repeatSourceCount = 0U;
  ChannelRepeatSource repeatSources[kChannelRepeatSourceCapacity]{};
  bool repeatSourcesTruncated = false;
};

enum class RepeatDiagnosticResult : uint8_t {
  None = 0,
  NoActiveHash,
  WireMismatch,
  DigestMismatch,
  Recorded,
  Saturated,
};

constexpr size_t kRepeatDiagnosticHashBytes = 8U;
constexpr size_t kRepeatDiagnosticPathBytes = 64U;

// Volatile, non-secret evidence for controlled RF acceptance tests. Counters
// separate physical raw reception from parsed callbacks and exact journal
// correlation; the last path can be compared with an owned repeater's public
// key prefix but is not a collision-resistant identity.
struct RepeatDiagnostics {
  uint32_t txDoneFrames = 0U;
  uint32_t txFailedFrames = 0U;
  // Legacy software-state counter retained so physical proof remains visibly
  // distinguishable in diagnostics.
  uint32_t rxReadyAfterTx = 0U;
  uint32_t physicalRxConfirmedAfterTx = 0U;
  uint32_t syncTurnaroundCompleted = 0U;
  uint32_t syncTurnaroundStartFailures = 0U;
  uint32_t syncTurnaroundTimeouts = 0U;
  uint32_t rxRearmAttempts = 0U;
  uint32_t rxRearmRetries = 0U;
  uint32_t rxRearmFailures = 0U;
  uint8_t lastRxStartAttempts = 0U;
  bool lastRxStartCodeAvailable = false;
  int16_t lastRxStartCode = 0;
  bool lastRxStartSoftwareState = false;
  bool lastRxChipStatusAvailable = false;
  uint8_t lastRxChipStatus = 0U;
  bool lastTxDoneToStartReceiveMicrosAvailable = false;
  uint32_t lastTxDoneToStartReceiveMicros = 0U;
  bool lastTxDoneToRxConfirmedMicrosAvailable = false;
  uint32_t lastTxDoneToRxConfirmedMicros = 0U;
  // Non-mutating task-context probe taken while this snapshot is built. It
  // lets acceptance verify idle RX before authorizing another transmission.
  bool currentRxSoftwareState = false;
  bool currentRxChipStatusAvailable = false;
  uint8_t currentRxChipStatus = 0U;
  // Boot-scoped task-context receive observability. IRQ bit counters count
  // non-clearing GetIrqStatus snapshots containing each bit; they are
  // observations, not unique RF packet counts. No packet content is retained.
  uint32_t dio1Polls = 0U;
  uint32_t dio1HighPolls = 0U;
  uint32_t dio1HighEdges = 0U;
  uint32_t dio1Callbacks = 0U;
  uint32_t irqSamples = 0U;
  uint32_t irqDioAssertedSamples = 0U;
  uint32_t irqLowRateSamples = 0U;
  bool irqObservationOpen = false;
  uint32_t irqObservationRemainingMs = 0U;
  uint16_t lastIrqFlags = 0U;
  uint16_t lastDioIrqFlags = 0U;
  uint16_t lastLowRateIrqFlags = 0U;
  uint32_t irqRxDoneObservations = 0U;
  uint32_t irqCrcErrorObservations = 0U;
  uint32_t irqHeaderErrorObservations = 0U;
  uint32_t irqTimeoutObservations = 0U;
  uint32_t irqPreambleObservations = 0U;
  uint32_t irqHeaderValidObservations = 0U;
  uint32_t irqSyncWordValidObservations = 0U;
  uint32_t dioIrqRxDoneObservations = 0U;
  uint32_t dioIrqCrcErrorObservations = 0U;
  uint32_t dioIrqHeaderErrorObservations = 0U;
  uint32_t dioIrqTimeoutObservations = 0U;
  uint32_t dioIrqPreambleObservations = 0U;
  uint32_t dioIrqHeaderValidObservations = 0U;
  uint32_t dioIrqSyncWordValidObservations = 0U;
  uint32_t lowRateIrqRxDoneObservations = 0U;
  uint32_t lowRateIrqCrcErrorObservations = 0U;
  uint32_t lowRateIrqHeaderErrorObservations = 0U;
  uint32_t lowRateIrqTimeoutObservations = 0U;
  uint32_t lowRateIrqPreambleObservations = 0U;
  uint32_t lowRateIrqHeaderValidObservations = 0U;
  uint32_t lowRateIrqSyncWordValidObservations = 0U;
  uint32_t recvRawAttempts = 0U;
  uint32_t recvInterruptReadyAttempts = 0U;
  uint32_t recvPacketLengthSamples = 0U;
  uint32_t recvPacketLengthZero = 0U;
  bool lastRecvPacketLengthAvailable = false;
  uint16_t lastRecvPacketLength = 0U;
  uint32_t recvReadDataAttempts = 0U;
  uint32_t recvSuccessfulReads = 0U;
  uint32_t recvReadDataErrors = 0U;
  bool lastRecvReadDataErrorAvailable = false;
  int16_t lastRecvReadDataError = 0;
  uint32_t recvRxRestartAttempts = 0U;
  uint32_t recvRxRestartSuccesses = 0U;
  uint32_t recvRxRestartErrors = 0U;
  bool lastRecvRxRestartResultAvailable = false;
  int16_t lastRecvRxRestartResult = 0;
  bool lastRecvRxRestartErrorAvailable = false;
  int16_t lastRecvRxRestartError = 0;
  uint32_t shortFrameRejected = 0U;
  bool lastShortFrameLengthAvailable = false;
  uint8_t lastShortFrameLength = 0U;
  uint32_t maxMeshLoopGapMs = 0U;
  uint32_t scopedFloodTxDoneFrames = 0U;
  uint32_t unscopedFloodTxDoneFrames = 0U;
  uint32_t rawFrames = 0U;
  uint32_t parsedFrames = 0U;
  uint32_t rawRejected = 0U;
  uint32_t channelForwardCandidates = 0U;
  uint32_t channelHashMatches = 0U;
  uint32_t channelWireMismatches = 0U;
  uint32_t channelDigestMismatches = 0U;
  uint32_t channelExactMatches = 0U;
  uint32_t channelRecorded = 0U;
  uint32_t channelSaturated = 0U;
  uint32_t advertForwardCandidates = 0U;
  uint32_t advertHashMatches = 0U;
  uint32_t advertWireMismatches = 0U;
  uint32_t advertDigestMismatches = 0U;
  uint32_t advertExactMatches = 0U;
  uint32_t advertRecorded = 0U;
  uint32_t advertSaturated = 0U;
  bool lastFloodTxAvailable = false;
  bool lastFloodTxScoped = false;
  uint8_t lastFloodTxPayloadType = 0U;
  uint16_t lastFloodTxTransportCode = 0U;
  bool lastChannelAvailable = false;
  uint8_t lastChannelHash[kRepeatDiagnosticHashBytes]{};
  uint8_t lastPath[kRepeatDiagnosticPathBytes]{};
  uint8_t lastPathBytes = 0U;
  uint8_t lastPathHashSize = 0U;
  uint8_t lastPathCount = 0U;
  float lastRssi = 0.0f;
  float lastSnr = 0.0f;
  RepeatDiagnosticResult lastResult = RepeatDiagnosticResult::None;
  bool lastAdvertAvailable = false;
  uint8_t lastAdvertHash[kRepeatDiagnosticHashBytes]{};
  uint8_t lastAdvertPath[kRepeatDiagnosticPathBytes]{};
  uint8_t lastAdvertPathBytes = 0U;
  uint8_t lastAdvertPathHashSize = 0U;
  uint8_t lastAdvertPathCount = 0U;
  float lastAdvertRssi = 0.0f;
  float lastAdvertSnr = 0.0f;
  RepeatDiagnosticResult lastAdvertResult = RepeatDiagnosticResult::None;
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

  // Authenticated companion advertising never opens the general TX session.
  // Readiness reports every non-pack prerequisite and the exact remaining
  // volatile cooldown; the caller owns companion-pack admission. A successful
  // call queues one standard signed Client advert through MeshCore's normal
  // zero-hop or flood path and arms only the next physical send attempt.
  TransportStatus advertiseReadiness(
      const Settings& settings, const CurrentLocationOnce& current,
      uint32_t& retryAfterMs) const;
  TransportStatus introduceOnce(
      AdvertScope scope, const Settings& settings,
      const CurrentLocationOnce& current, bool explicitUserApproval);

  bool takeAdvert(ReceivedAdvert& output);
  // Volatile lifecycle/echo evidence for only the most recent owner-requested
  // Mesh-wide advert. Nearby zero-hop adverts never create or replace it.
  bool lastFloodAdvertStatus(FloodAdvertStatus& output);
  bool takeFloodAdvertStatusChanged();
  bool lastNearbyAdvertStatus(NearbyAdvertStatus& output);
  bool takeNearbyAdvertStatusChanged();
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
                             const uint8_t secret[32],
                             ChannelRegionScope regionScope =
                                 ChannelRegionScope::Legacy);
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
  bool repeatDiagnostics(RepeatDiagnostics& output) const;
  bool directSendPending() const;
  bool sendInProgress() const;
  uint32_t droppedMessageCount() const;
  uint32_t droppedDeliveryCount() const;

  bool messagingStorageReady() const;
  bool messagingStorageStatus(MessagingStorageStatus& output) const;
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
