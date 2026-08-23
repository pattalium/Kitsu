#pragma once

#include <stdint.h>

namespace kitsu868 {
namespace mesh {

// MeshCore's default score delay is appropriate for repeaters competing to
// forward a flood. Kitsu is an endpoint and never forwards, so retaining an
// inbound flood in the packet pool has no protocol benefit.
constexpr int endpointFloodReceiveDelayMs(float, uint32_t) { return 0; }

}  // namespace mesh
}  // namespace kitsu868
