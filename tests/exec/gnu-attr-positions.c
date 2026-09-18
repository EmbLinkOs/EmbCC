/* GNU attributes in the positions the EmbLinkOS kernel writes them, and a
 * block-scope declaration with no declarator — the three constructs that kept
 * kstring.c, power_x86.c and syscalls.c from compiling. gcc referees the
 * layouts: a packed attribute ignored in its new position would change a
 * sizeof below, not merely fail to parse.
 */
// expect-exit: 42
#include <stdint.h>

/* between the type and the declarator (kstring.c) */
typedef uint64_t __attribute__((may_alias)) word_t;
/* after a pointer's star */
static int *__attribute__((unused)) spare;

/* before the tag (gcc does NOT accept one between the tag and the '{') */
struct __attribute__((packed)) before_tag { char c; int i; };
/* after the body, the long-supported position, for contrast */
struct after_body { char c; long l; } __attribute__((packed));

int main(void)
{
    /* declares only its enumerators (syscalls.c) */
    enum { BAND = 64 };
    /* declares only its tag */
    struct pair { int a, b; };
    struct pair p = { BAND, 2 };
    /* anonymous and packed, as power_x86.c's null IDT descriptor */
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idt = { 0, 0 };

    if (sizeof(struct before_tag) != 5) return 1;
    if (sizeof(struct after_body) != 9) return 2;
    if (sizeof idt != 10) return 3;
    if (p.a != 64 || p.b != 2) return 4;
    idt.base = 0x1122334455667788ULL;
    if (idt.base != 0x1122334455667788ULL || idt.limit != 0) return 5;

    /* word_t reads eight bytes through any pointer, as kstring.c uses it */
    unsigned char bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    word_t w = *(const word_t *)bytes;
    if ((w & 0xff) != 1 || (w >> 56) != 8) return 6;
    (void)spare;
    return 42;
}
