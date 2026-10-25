/* <inttypes.h>'s macros print and scan each <stdint.h> type whole. PRId64
 * was "ld" everywhere, and on a 32-bit target -- where int64_t is long
 * long -- printf read half of it and the next argument from the other
 * half; tests/golden/inttypes-formats.sh has clang check every macro's
 * type, and this the values it moves. */
// expect-exit: 42
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char b[96];
    snprintf(b, sizeof b, "%" PRId64 " %" PRIu64 " %d", INT64_MIN, UINT64_MAX,
             7);
    if (strcmp(b, "-9223372036854775808 18446744073709551615 7") != 0)
        return 1;
    snprintf(b, sizeof b, "%" PRIx64 " %" PRIX32 " %" PRIo16 " %" PRIi8,
             (uint64_t)0x123456789abcdef0u, (uint32_t)0xdeadbeefu,
             (uint16_t)0777, (int8_t)-5);
    if (strcmp(b, "123456789abcdef0 DEADBEEF 777 -5") != 0)
        return 2;
    snprintf(b, sizeof b, "%" PRIdMAX " %" PRIuPTR " %" PRIdFAST8 " %" PRIuLEAST64
             " %d", INTMAX_MAX, (uintptr_t)4096, (int_fast8_t)-3,
             (uint_least64_t)1 << 40, 9);
    if (strcmp(b, "9223372036854775807 4096 -3 1099511627776 9") != 0)
        return 3;

    int64_t s64;
    uint64_t u64;
    int32_t s32;
    uint16_t u16;
    int8_t s8 = 0, guard = 99;
    if (sscanf("-1234567890123 fedcba9876543210 -77 65535 -100",
               "%" SCNd64 " %" SCNx64 " %" SCNd32 " %" SCNu16 " %" SCNd8,
               &s64, &u64, &s32, &u16, &s8) != 5)
        return 4;
    if (s64 != -1234567890123 || u64 != 0xfedcba9876543210u || s32 != -77 ||
        u16 != 65535 || s8 != -100 || guard != 99)
        return 5;
    intmax_t m;
    int_least32_t l32;
    if (sscanf("9223372036854775807 2147483647", "%" SCNdMAX " %" SCNdLEAST32,
               &m, &l32) != 2 || m != INTMAX_MAX || l32 != 2147483647)
        return 6;
    return 42;
}
