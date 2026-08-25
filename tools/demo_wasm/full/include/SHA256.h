#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Source-compatible subset of the Arduino Cryptography Library SHA256 class.
// MeshCore and the Kitsu repeat trackers use only update/finalize and HMAC.
class SHA256 {
 public:
  SHA256() { reset(); }

  void reset() {
    state_[0] = 0x6a09e667U;
    state_[1] = 0xbb67ae85U;
    state_[2] = 0x3c6ef372U;
    state_[3] = 0xa54ff53aU;
    state_[4] = 0x510e527fU;
    state_[5] = 0x9b05688cU;
    state_[6] = 0x1f83d9abU;
    state_[7] = 0x5be0cd19U;
    totalBytes_ = 0U;
    blockBytes_ = 0U;
    std::memset(block_, 0, sizeof(block_));
  }

  void update(const void* input, size_t bytes) {
    if (!input || bytes == 0U) return;
    const auto* source = static_cast<const uint8_t*>(input);
    totalBytes_ += bytes;
    while (bytes != 0U) {
      const size_t copying = bytes < sizeof(block_) - blockBytes_
                                 ? bytes
                                 : sizeof(block_) - blockBytes_;
      std::memcpy(block_ + blockBytes_, source, copying);
      blockBytes_ += copying;
      source += copying;
      bytes -= copying;
      if (blockBytes_ == sizeof(block_)) {
        transform(block_);
        blockBytes_ = 0U;
      }
    }
  }

  void finalize(void* output, size_t bytes) {
    if (!output || bytes == 0U) return;
    const uint64_t bitCount = totalBytes_ * 8U;
    const uint8_t marker = 0x80U;
    update(&marker, 1U);
    const uint8_t zero = 0U;
    while (blockBytes_ != 56U) update(&zero, 1U);
    uint8_t length[8]{};
    for (uint8_t index = 0U; index < 8U; ++index) {
      length[7U - index] = static_cast<uint8_t>(bitCount >> (index * 8U));
    }
    update(length, sizeof(length));

    uint8_t digest[32]{};
    for (uint8_t word = 0U; word < 8U; ++word) {
      digest[word * 4U] = static_cast<uint8_t>(state_[word] >> 24U);
      digest[word * 4U + 1U] = static_cast<uint8_t>(state_[word] >> 16U);
      digest[word * 4U + 2U] = static_cast<uint8_t>(state_[word] >> 8U);
      digest[word * 4U + 3U] = static_cast<uint8_t>(state_[word]);
    }
    std::memcpy(output, digest, bytes < sizeof(digest) ? bytes
                                                       : sizeof(digest));
    std::memset(digest, 0, sizeof(digest));
  }

  void resetHMAC(const void* key, size_t keyBytes) {
    uint8_t normalized[64]{};
    if (keyBytes > sizeof(normalized)) {
      SHA256 hash;
      hash.update(key, keyBytes);
      hash.finalize(normalized, 32U);
    } else if (key && keyBytes != 0U) {
      std::memcpy(normalized, key, keyBytes);
    }
    uint8_t inner[64]{};
    for (size_t index = 0U; index < sizeof(inner); ++index) {
      inner[index] = normalized[index] ^ 0x36U;
      hmacOuter_[index] = normalized[index] ^ 0x5cU;
    }
    reset();
    update(inner, sizeof(inner));
    hmacReady_ = true;
    std::memset(normalized, 0, sizeof(normalized));
    std::memset(inner, 0, sizeof(inner));
  }

  void finalizeHMAC(const void*, size_t, void* output, size_t bytes) {
    if (!hmacReady_) {
      finalize(output, bytes);
      return;
    }
    uint8_t innerDigest[32]{};
    finalize(innerDigest, sizeof(innerDigest));
    reset();
    update(hmacOuter_, sizeof(hmacOuter_));
    update(innerDigest, sizeof(innerDigest));
    finalize(output, bytes);
    hmacReady_ = false;
    std::memset(innerDigest, 0, sizeof(innerDigest));
    std::memset(hmacOuter_, 0, sizeof(hmacOuter_));
  }

 private:
  static uint32_t rotateRight(uint32_t value, uint8_t bits) {
    return (value >> bits) | (value << (32U - bits));
  }

  void transform(const uint8_t input[64]) {
    static constexpr uint32_t constants[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };
    uint32_t words[64]{};
    for (uint8_t index = 0U; index < 16U; ++index) {
      const size_t offset = static_cast<size_t>(index) * 4U;
      words[index] = static_cast<uint32_t>(input[offset]) << 24U |
                     static_cast<uint32_t>(input[offset + 1U]) << 16U |
                     static_cast<uint32_t>(input[offset + 2U]) << 8U |
                     static_cast<uint32_t>(input[offset + 3U]);
    }
    for (uint8_t index = 16U; index < 64U; ++index) {
      const uint32_t left = words[index - 15U];
      const uint32_t right = words[index - 2U];
      const uint32_t sigma0 = rotateRight(left, 7U) ^
                              rotateRight(left, 18U) ^ (left >> 3U);
      const uint32_t sigma1 = rotateRight(right, 17U) ^
                              rotateRight(right, 19U) ^ (right >> 10U);
      words[index] = words[index - 16U] + sigma0 + words[index - 7U] +
                     sigma1;
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];
    for (uint8_t index = 0U; index < 64U; ++index) {
      const uint32_t bigSigma1 = rotateRight(e, 6U) ^
                                 rotateRight(e, 11U) ^ rotateRight(e, 25U);
      const uint32_t choose = (e & f) ^ (~e & g);
      const uint32_t temp1 = h + bigSigma1 + choose + constants[index] +
                             words[index];
      const uint32_t bigSigma0 = rotateRight(a, 2U) ^
                                 rotateRight(a, 13U) ^ rotateRight(a, 22U);
      const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t temp2 = bigSigma0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
    std::memset(words, 0, sizeof(words));
  }

  uint32_t state_[8]{};
  uint64_t totalBytes_ = 0U;
  uint8_t block_[64]{};
  size_t blockBytes_ = 0U;
  uint8_t hmacOuter_[64]{};
  bool hmacReady_ = false;
};
