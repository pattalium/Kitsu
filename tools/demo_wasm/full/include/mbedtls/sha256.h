#pragma once

#include "../SHA256.h"

struct mbedtls_sha256_context {
  SHA256 hash{};
};

inline void mbedtls_sha256_init(mbedtls_sha256_context* context) {
  if (context) context->hash.reset();
}
inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context* context,
                                     int is224) {
  if (!context || is224 != 0) return -1;
  context->hash.reset();
  return 0;
}
inline int mbedtls_sha256_update_ret(mbedtls_sha256_context* context,
                                     const unsigned char* input,
                                     size_t bytes) {
  if (!context || (!input && bytes != 0U)) return -1;
  context->hash.update(input, bytes);
  return 0;
}
inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context* context,
                                     unsigned char output[32]) {
  if (!context || !output) return -1;
  context->hash.finalize(output, 32U);
  return 0;
}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
