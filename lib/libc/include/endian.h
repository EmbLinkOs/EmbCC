/* <endian.h> -- the byte order, and conversions to a fixed one
 * (POSIX.1-2024 took these from the BSDs), on the compiler's swaps */
#ifndef _ENDIAN_H
#define _ENDIAN_H

#include <stdint.h>

#define LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#define BIG_ENDIAN    __ORDER_BIG_ENDIAN__
#define PDP_ENDIAN    __ORDER_PDP_ENDIAN__
#define BYTE_ORDER    __BYTE_ORDER__
#define __LITTLE_ENDIAN LITTLE_ENDIAN
#define __BIG_ENDIAN    BIG_ENDIAN
#define __BYTE_ORDER    BYTE_ORDER

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define htobe16(x) __builtin_bswap16(x)
#define htobe32(x) __builtin_bswap32(x)
#define htobe64(x) __builtin_bswap64(x)
#define htole16(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define htole64(x) ((uint64_t)(x))
#else
#define htobe16(x) ((uint16_t)(x))
#define htobe32(x) ((uint32_t)(x))
#define htobe64(x) ((uint64_t)(x))
#define htole16(x) __builtin_bswap16(x)
#define htole32(x) __builtin_bswap32(x)
#define htole64(x) __builtin_bswap64(x)
#endif
#define be16toh(x) htobe16(x)
#define be32toh(x) htobe32(x)
#define be64toh(x) htobe64(x)
#define le16toh(x) htole16(x)
#define le32toh(x) htole32(x)
#define le64toh(x) htole64(x)

#endif
