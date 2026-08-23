#include "../src/kitsu_transport_scope.h"

#include <SHA256.h>

#include <assert.h>
#include <string.h>

namespace {

using kitsu868::mesh::calculateDefaultTransportCode;
using kitsu868::mesh::kDefaultTransportScopeKey;
using kitsu868::mesh::kDefaultTransportScopeKeyFingerprint;
using kitsu868::mesh::kDefaultTransportScopeName;
using kitsu868::mesh::kDefaultTransportScopeTag;
using kitsu868::mesh::kTransportScopeKeyBytes;

void expectWireCode(uint8_t payloadType, const uint8_t* payload,
                    size_t payloadBytes, uint8_t first, uint8_t second) {
  uint16_t code = 0U;
  assert(calculateDefaultTransportCode(payloadType, payload, payloadBytes,
                                       code));
  const uint8_t* wire = reinterpret_cast<const uint8_t*>(&code);
  assert(wire[0] == first);
  assert(wire[1] == second);
}

}  // namespace

int main() {
  assert(strcmp(kDefaultTransportScopeName, "EU") == 0);
  assert(strcmp(kDefaultTransportScopeTag, "#EU") == 0);

  // Pinned companion-v1.17.1 TransportKeyStore::getAutoKeyFor() hashes the
  // public region tag and truncates SHA-256 to the 16-byte transport key.
  uint8_t derived[32]{};
  SHA256 sha;
  sha.update(kDefaultTransportScopeTag, strlen(kDefaultTransportScopeTag));
  sha.finalize(derived, sizeof(derived));
  assert(memcmp(derived, kDefaultTransportScopeKey,
                kTransportScopeKeyBytes) == 0);
  const uint8_t expectedKey[kTransportScopeKeyBytes] = {
      0x04, 0x25, 0x4d, 0xa6, 0xa9, 0xae, 0x90, 0x20,
      0xad, 0x2a, 0xf9, 0x15, 0x17, 0x41, 0x33, 0x47,
  };
  assert(memcmp(kDefaultTransportScopeKey, expectedKey,
                sizeof(expectedKey)) == 0);
  uint8_t keyFingerprint[32]{};
  SHA256 keyFingerprintSha;
  keyFingerprintSha.update(kDefaultTransportScopeKey,
                           kTransportScopeKeyBytes);
  keyFingerprintSha.finalize(keyFingerprint, sizeof(keyFingerprint));
  constexpr char kHex[] = "0123456789ABCDEF";
  char fingerprintHex[65]{};
  for (size_t index = 0U; index < sizeof(keyFingerprint); ++index) {
    fingerprintHex[index * 2U] = kHex[keyFingerprint[index] >> 4U];
    fingerprintHex[index * 2U + 1U] = kHex[keyFingerprint[index] & 0x0fU];
  }
  assert(strcmp(fingerprintHex,
                kDefaultTransportScopeKeyFingerprint) == 0);

  const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o'};
  expectWireCode(0x05U, nullptr, 0U, 0x6bU, 0xc2U);
  expectWireCode(0x05U, hello, sizeof(hello), 0x8fU, 0x7aU);
  expectWireCode(0x04U, hello, sizeof(hello), 0xb9U, 0x61U);

  // The exact upstream algorithm reserves numeric codes 0000 and FFFF.
  // These four-byte payloads are deterministic preimages for the two cases.
  const uint8_t zeroCodePayload[] = {0x87U, 0x71U, 0x00U, 0x00U};
  const uint8_t ffCodePayload[] = {0xc2U, 0x68U, 0x00U, 0x00U};
  expectWireCode(0x05U, zeroCodePayload, sizeof(zeroCodePayload), 0x01U,
                 0x00U);
  expectWireCode(0x05U, ffCodePayload, sizeof(ffCodePayload), 0xfeU, 0xffU);

  uint16_t output = 123U;
  assert(!calculateDefaultTransportCode(0x05U, nullptr, 1U, output));
  assert(output == 0U);
  return 0;
}
