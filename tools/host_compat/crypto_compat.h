#pragma once

#include <stdint.h>

// The embedded Crypto library expresses rotates with GCC statement
// expressions. Suppress that header only in the MSVC host-test build and
// provide equivalent side-effect-safe inline operations.
#define CRYPTO_ROTATEUTIL_H

static inline uint32_t kitsu_host_rotr32(uint32_t value, uint8_t bits) {
  return (value >> bits) | (value << (32U - bits));
}

#define rightRotate2(a) kitsu_host_rotr32(static_cast<uint32_t>(a), 2U)
#define rightRotate6(a) kitsu_host_rotr32(static_cast<uint32_t>(a), 6U)
#define rightRotate7(a) kitsu_host_rotr32(static_cast<uint32_t>(a), 7U)
#define rightRotate11(a) kitsu_host_rotr32(static_cast<uint32_t>(a), 11U)
#define rightRotate13(a) kitsu_host_rotr32(static_cast<uint32_t>(a), 13U)
#define rightRotate17(a) kitsu_host_rotr32(static_cast<uint32_t>(a), 17U)
#define rightRotate18(a) kitsu_host_rotr32(static_cast<uint32_t>(a), 18U)
#define rightRotate19(a) kitsu_host_rotr32(static_cast<uint32_t>(a), 19U)
#define rightRotate22(a) kitsu_host_rotr32(static_cast<uint32_t>(a), 22U)
#define rightRotate25(a) kitsu_host_rotr32(static_cast<uint32_t>(a), 25U)
