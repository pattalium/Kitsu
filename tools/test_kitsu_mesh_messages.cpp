#include "../src/kitsu_mesh_messages.h"

#include <assert.h>
#include <string.h>

using kitsu868::mesh::DecodedTextPayload;
using kitsu868::mesh::TextCodecStatus;

int main() {
  uint8_t payload[200]{};
  size_t bytes = 0;
  assert(kitsu868::mesh::encodeDirectTextPayload(
             0x78563412UL, 0, "hello fox", payload, sizeof(payload), bytes) ==
         TextCodecStatus::Ok);
  assert(bytes == 14);
  assert(payload[0] == 0x12 && payload[3] == 0x78 && payload[4] == 0);

  // Simulate MeshCore cipher zero padding on receive.
  DecodedTextPayload decoded{};
  assert(kitsu868::mesh::decodeDirectTextPayload(payload, 32, decoded) ==
         TextCodecStatus::Ok);
  assert(decoded.timestamp == 0x78563412UL);
  assert(decoded.attempt == 0);
  assert(strcmp(decoded.text, "hello fox") == 0);

  memset(payload, 0, sizeof(payload));
  assert(kitsu868::mesh::encodeDirectTextPayload(
             123, 7, "retry", payload, sizeof(payload), bytes) ==
         TextCodecStatus::Ok);
  assert(bytes == 12 && payload[11] == 7);
  assert(kitsu868::mesh::decodeDirectTextPayload(payload, 16, decoded) ==
         TextCodecStatus::Ok);
  assert(decoded.attempt == 7 && strcmp(decoded.text, "retry") == 0);

  memset(payload, 0, sizeof(payload));
  assert(kitsu868::mesh::encodeChannelTextPayload(
             456, "Kitsu KTDEAD", "hi public", 9, payload,
             sizeof(payload), bytes) == TextCodecStatus::Ok);
  assert(kitsu868::mesh::decodeChannelTextPayload(payload, 32, decoded) ==
         TextCodecStatus::Ok);
  assert(strcmp(decoded.text, "Kitsu KTDEAD: hi public") == 0);

  char tooLong[162];
  memset(tooLong, 'x', sizeof(tooLong));
  tooLong[161] = 0;
  assert(kitsu868::mesh::encodeDirectTextPayload(
             1, 0, tooLong, payload, sizeof(payload), bytes) ==
         TextCodecStatus::TextTooLong);

  const char badUtf8[] = {'x', static_cast<char>(0xc0),
                          static_cast<char>(0x80), 0};
  assert(kitsu868::mesh::encodeDirectTextPayload(
             1, 0, badUtf8, payload, sizeof(payload), bytes) ==
         TextCodecStatus::InvalidUtf8);
  const char newline[] = "hello\nworld";
  assert(kitsu868::mesh::encodeDirectTextPayload(
             1, 0, newline, payload, sizeof(payload), bytes) ==
         TextCodecStatus::InvalidUtf8);

  memset(payload, 0, sizeof(payload));
  payload[0] = 1;
  assert(kitsu868::mesh::decodeDirectTextPayload(payload, 6, decoded) ==
         TextCodecStatus::EmptyText);
  assert(kitsu868::mesh::decodeChannelTextPayload(payload, 6, decoded) ==
         TextCodecStatus::EmptyText);

  assert(kitsu868::mesh::encodeDirectTextPayload(
             1, 0, "", payload, sizeof(payload), bytes) ==
         TextCodecStatus::EmptyText);

  memset(payload, 0, sizeof(payload));
  assert(kitsu868::mesh::encodeChannelTextPayload(
             1, "Fox", "", 0, payload, sizeof(payload), bytes) ==
         TextCodecStatus::EmptyText);

  memset(payload, 0, sizeof(payload));
  payload[0] = 1;
  memcpy(payload + 5, "Fox: ", 5);
  assert(kitsu868::mesh::decodeChannelTextPayload(payload, 10, decoded) ==
         TextCodecStatus::EmptyText);

  payload[0] = 1;
  payload[1] = payload[2] = payload[3] = 0;
  payload[4] = 1U << 2U;  // CLI text, intentionally unsupported.
  payload[5] = 'x';
  assert(kitsu868::mesh::decodeDirectTextPayload(payload, 6, decoded) ==
         TextCodecStatus::UnsupportedTextType);
  return 0;
}
