#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kitsu868 {
namespace mesh {

// MeshCore companion-v1.17.1 BaseChatMesh uses ten AES blocks for the
// timestamp/flags/text plaintext.  Keep this value protocol-pinned even though
// the Kitsu app has its own UI limits.
constexpr size_t kMeshTextBytes = 160;
constexpr size_t kMeshTextCapacity = kMeshTextBytes + 1U;
constexpr size_t kMeshPlaintextCapacity = 5U + kMeshTextBytes + 2U;

enum class TextCodecStatus : uint8_t {
  Ok = 0,
  NullArgument,
  EmptyText,
  TextTooLong,
  OutputTooSmall,
  PayloadTooShort,
  UnsupportedTextType,
  InvalidUtf8,
};

const char* textCodecStatusName(TextCodecStatus status);

struct DecodedTextPayload {
  uint32_t timestamp = 0;
  uint8_t attempt = 0;
  char text[kMeshTextCapacity]{};
  size_t textBytes = 0;
};

// Plain direct-message payload used inside Mesh::createDatagram
// (PAYLOAD_TYPE_TXT_MSG).  The terminating NUL is normally supplied by zero
// padding in MeshCore's cipher and is only encoded explicitly for attempts >3,
// exactly matching BaseChatMesh::composeMsgPacket.
TextCodecStatus encodeDirectTextPayload(uint32_t timestamp, uint8_t attempt,
                                        const char* text, uint8_t* output,
                                        size_t outputCapacity,
                                        size_t& outputBytes);

// Decode a decrypted direct text payload.  Cipher padding is accepted, the
// first NUL terminates text, and CLI/signed variants are deliberately rejected
// by this minimal plain-text client.
TextCodecStatus decodeDirectTextPayload(const uint8_t* input,
                                        size_t inputBytes,
                                        DecodedTextPayload& output);

// Group text carries "sender: text" as one encrypted string, matching
// BaseChatMesh::sendGroupMessage.  combined text is truncated only when the
// caller explicitly asks by passing a shorter textBytes; this helper never
// silently truncates.
TextCodecStatus encodeChannelTextPayload(uint32_t timestamp,
                                         const char* senderName,
                                         const char* text,
                                         size_t textBytes,
                                         uint8_t* output,
                                         size_t outputCapacity,
                                         size_t& outputBytes);

TextCodecStatus decodeChannelTextPayload(const uint8_t* input,
                                         size_t inputBytes,
                                         DecodedTextPayload& output);

// Terminal/app-facing strings must not carry malformed UTF-8 or control
// characters.  Newline, carriage return, tab, C0/C1 controls, surrogates and
// non-scalar code points are rejected.
bool validMeshTextUtf8(const char* text, size_t textBytes);

}  // namespace mesh
}  // namespace kitsu868
