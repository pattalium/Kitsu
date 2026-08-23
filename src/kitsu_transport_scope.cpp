#include "kitsu_transport_scope.h"
#include "kitsu_repeat_wire.h"

#include <SHA256.h>

namespace kitsu868 {
namespace mesh {

// First 16 bytes of SHA-256("#EU"). This is a public routing scope key, not a
// channel secret or an authentication credential. The host golden-vector test
// independently derives it from kDefaultTransportScopeTag on every run.
const uint8_t kDefaultTransportScopeKey[kTransportScopeKeyBytes] = {
    0x04, 0x25, 0x4d, 0xa6, 0xa9, 0xae, 0x90, 0x20,
    0xad, 0x2a, 0xf9, 0x15, 0x17, 0x41, 0x33, 0x47,
};

bool calculateTransportCode(
    const uint8_t key[kTransportScopeKeyBytes], uint8_t payloadType,
    const uint8_t* payload, size_t payloadBytes, uint16_t& output) {
  output = 0U;
  if (!key || (!payload && payloadBytes != 0U)) return false;

  SHA256 sha;
  sha.resetHMAC(key, kTransportScopeKeyBytes);
  sha.update(&payloadType, 1U);
  if (payloadBytes != 0U) sha.update(payload, payloadBytes);
  sha.finalizeHMAC(key, kTransportScopeKeyBytes, &output, sizeof(output));
  if (output == 0U) {
    output = 1U;
  } else if (output == 0xffffU) {
    output = 0xfffeU;
  }
  return true;
}

bool isExactDefaultScopedRepeat(const RepeatWireView& wire) {
  if (wire.route != kRepeatWireRouteTransportFlood ||
      !wire.hasTransportCodes || wire.pathHashSize != 1U ||
      wire.pathCount == 0U || wire.transportCodes[1] != 0U ||
      !wire.payload || wire.payloadBytes == 0U) {
    return false;
  }
  uint16_t expected = 0U;
  return calculateDefaultTransportCode(wire.payloadType, wire.payload,
                                       wire.payloadBytes, expected) &&
      wire.transportCodes[0] == expected;
}

}  // namespace mesh
}  // namespace kitsu868
