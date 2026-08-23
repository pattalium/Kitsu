#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kitsu868 {
namespace mesh {

constexpr size_t kRepeatWireMaximumPathBytes = 64U;
constexpr uint8_t kRepeatWireRouteMask = 0x03U;
constexpr uint8_t kRepeatWireRouteTransportFlood = 0x00U;
constexpr uint8_t kRepeatWireRouteFlood = 0x01U;
constexpr uint8_t kRepeatWireRouteDirect = 0x02U;
constexpr uint8_t kRepeatWireRouteTransportDirect = 0x03U;
constexpr uint8_t kRepeatWireTypeShift = 2U;
constexpr uint8_t kRepeatWireTypeMask = 0x0fU;
constexpr uint8_t kRepeatWireVersionShift = 6U;
constexpr uint8_t kRepeatWireVersionMask = 0x03U;
constexpr uint8_t kRepeatWirePayloadVersion1 = 0x00U;

enum class RepeatWireDecodeStatus : uint8_t {
  Ok = 0,
  InvalidArgument,
  UnsupportedVersion,
  TruncatedTransport,
  InvalidPath,
  MissingPayload,
};

// Non-owning view of a physically received MeshCore v1 frame. Decoding is
// deliberately independent of Packet allocation and the delayed inbound
// queue, so local echo evidence remains available during pool exhaustion.
struct RepeatWireView {
  uint8_t payloadType = 0U;
  uint8_t route = 0U;
  // Both legacy FLOOD and TRANSPORT_FLOOD accumulate repeater path hashes.
  // Transport scoping changes the routing prefix, not flood semantics.
  bool flood = false;
  bool hasTransportCodes = false;
  uint16_t transportCodes[2]{};
  uint8_t pathHashSize = 0U;
  uint8_t pathCount = 0U;
  const uint8_t* path = nullptr;
  size_t pathBytes = 0U;
  const uint8_t* payload = nullptr;
  size_t payloadBytes = 0U;

  const uint8_t* lastHopToken() const {
    return pathCount == 0U || !path
        ? nullptr
        : path + pathBytes - pathHashSize;
  }
};

// Immutable routing prefix captured from the exact flood packet that reached
// the radio.  Payload fingerprints intentionally exclude route/path bytes so
// forwarded copies remain correlatable; this binding closes the other half of
// that contract by requiring a returned copy to retain the same legacy-vs-
// transport shape, transport codes, and path-hash width as the sent packet.
struct FloodRouteBinding {
  bool transportScoped = false;
  uint16_t transportCodes[2]{};
  uint8_t pathHashSize = 0U;
};

bool validFloodRouteBinding(const FloodRouteBinding& binding);
bool sameFloodRouteBinding(const FloodRouteBinding& left,
                           const FloodRouteBinding& right);

// Extracts only flood routing metadata.  The returned binding does not claim
// which public region a transport code represents; correlation compares it
// with the actual code captured from the transmitted packet.
bool floodRouteBindingFromWire(const RepeatWireView& wire,
                               FloodRouteBinding& output);

RepeatWireDecodeStatus decodeRepeatWire(const uint8_t* raw, size_t rawBytes,
                                        RepeatWireView& output);

}  // namespace mesh
}  // namespace kitsu868
