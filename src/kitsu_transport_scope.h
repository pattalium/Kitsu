#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kitsu868 {
namespace mesh {

struct RepeatWireView;

// MeshCore companion-v1.17.1 derives a public region transport key from the
// SHA-256 of the region name with exactly one leading '#', then retains the
// first 16 bytes. Kitsu uses the canonical public region "#EU" only for a
// channel whose persisted configuration explicitly says region_scope=EU.
// Absent scope remains MeshCore's legacy FLOOD; radio profile never implies a
// transport region.
constexpr char kDefaultTransportScopeName[] = "EU";
constexpr char kDefaultTransportScopeTag[] = "#EU";
constexpr size_t kTransportScopeKeyBytes = 16U;

// Full SHA-256 fingerprint of the public 16-byte routing key below. This is
// diagnostic identity evidence, not a secret or authentication credential.
constexpr char kDefaultTransportScopeKeyFingerprint[] =
    "BB8D70297813E2B6F66A8A5F2889F7F6306CD81F5CAA6E2BB5C60BBFB33ED5B8";

extern const uint8_t
    kDefaultTransportScopeKey[kTransportScopeKeyBytes];

// Exact TransportKey::calcTransportCode algorithm pinned from MeshCore
// companion-v1.17.1 (commit d92964352441e53b93e8667b802e04f6e072b39e):
// HMAC-SHA256(key, payload_type || immutable_payload), first two bytes in
// MeshCore's native uint16 representation, reserving 0000 and FFFF.
bool calculateTransportCode(
    const uint8_t key[kTransportScopeKeyBytes], uint8_t payloadType,
    const uint8_t* payload, size_t payloadBytes, uint16_t& output);

inline bool calculateDefaultTransportCode(
    uint8_t payloadType, const uint8_t* payload, size_t payloadBytes,
    uint16_t& output) {
  return calculateTransportCode(kDefaultTransportScopeKey, payloadType,
                                payload, payloadBytes, output);
}

// Exact immutable transport-shape gate for a locally heard returned copy of
// one of Kitsu's #EU floods. A legacy rewrap, a different region code, a home
// code, or a different path-hash mode is not evidence of our transmitted wire
// packet even when type+payload happen to match.
bool isExactDefaultScopedRepeat(const RepeatWireView& wire);

}  // namespace mesh
}  // namespace kitsu868
