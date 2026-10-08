/* A local split by scalar replacement (SROA) into a piece no member
 * types -- eight bytes written and read across two four-byte words, four
 * across two halfwords. The piece is the integer exactly its size: it was
 * `long` for eight bytes, four on every 32-bit target, and `int` for four,
 * two on AVR, and the slot held half of it -- ColdFire's backend refused
 * the eight-byte read at -O1 and up. may_alias makes the cast accesses
 * C for GCC too; EmbCC does no type-based alias analysis either way. */
// expect-exit: 42
#include <stdint.h>

typedef uint64_t __attribute__((may_alias)) u64a;
typedef uint32_t __attribute__((may_alias)) u32a;
struct words { uint32_t a, b; };
struct halves { uint16_t a, b; };

__attribute__((noinline)) static uint64_t through_words(uint64_t v)
{
    struct words y;
    *(u64a *)&y = v;
    return *(u64a *)&y;
}

__attribute__((noinline)) static uint32_t through_halves(uint32_t v)
{
    struct halves y;
    *(u32a *)&y = v;
    return *(u32a *)&y;
}

static volatile uint64_t v8 = 0x1122334455667788ULL;
static volatile uint32_t v4 = 0x11223344UL;

int main(void)
{
    if (through_words(v8) != 0x1122334455667788ULL) return 1;
    if (through_halves(v4) != 0x11223344UL) return 2;
    return 42;
}
