#pragma once

#include <stdint.h>

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN

static inline uint16_t kitsu_host_bswap16(uint16_t value) {
  return static_cast<uint16_t>((value >> 8U) | (value << 8U));
}

static inline uint32_t kitsu_host_bswap32(uint32_t value) {
  return ((value & 0x000000ffUL) << 24U) |
      ((value & 0x0000ff00UL) << 8U) |
      ((value & 0x00ff0000UL) >> 8U) |
      ((value & 0xff000000UL) >> 24U);
}

static inline uint64_t kitsu_host_bswap64(uint64_t value) {
  return (static_cast<uint64_t>(kitsu_host_bswap32(
              static_cast<uint32_t>(value))) << 32U) |
      kitsu_host_bswap32(static_cast<uint32_t>(value >> 32U));
}

#define htole16(x) (x)
#define le16toh(x) (x)
#define htobe16(x) kitsu_host_bswap16(static_cast<uint16_t>(x))
#define be16toh(x) kitsu_host_bswap16(static_cast<uint16_t>(x))
#define htole32(x) (x)
#define le32toh(x) (x)
#define htobe32(x) kitsu_host_bswap32(static_cast<uint32_t>(x))
#define be32toh(x) kitsu_host_bswap32(static_cast<uint32_t>(x))
#define htole64(x) (x)
#define le64toh(x) (x)
#define htobe64(x) kitsu_host_bswap64(static_cast<uint64_t>(x))
#define be64toh(x) kitsu_host_bswap64(static_cast<uint64_t>(x))
