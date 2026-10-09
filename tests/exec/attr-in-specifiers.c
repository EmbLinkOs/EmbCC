/* GNU attributes among the declaration specifiers, after a qualifier or a
 * storage class and before the type: `static const __attribute__((x)) T`.
 * GCC and clang take an attribute anywhere a qualifier can stand, and it
 * applies to the declaration. EmbCC took one only before the first
 * specifier or after the type, so `const __attribute__((unused)) int` at
 * any scope, and `static __attribute__((aligned(4))) char` inside a
 * function, were syntax errors. The alignments test that each attribute
 * ARRIVED, not merely that it parsed: dropped, a layout below changes.
 */
// expect-exit: 42
#include <stddef.h>
#include <stdint.h>

char pad0 = 1;
const __attribute__((aligned(64))) char g64[3] = { 1, 2, 3 };
char pad1 = 2;
volatile __attribute__((aligned(32))) int gv32 = 5;
static const __attribute__((unused)) int never_read = 9;

struct m {
    char a;
    const __attribute__((aligned(16))) char c;
    volatile __attribute__((aligned(8))) short s;
    /* a leading attribute and one among the specifiers, together */
    __attribute__((aligned(4))) const __attribute__((unused)) char d;
};

static int take(const __attribute__((unused)) int x, int y) { return y; }

int main(void)
{
    static char pad2 = 3;
    static __attribute__((aligned(64))) char s64[3];
    static const __attribute__((aligned(32))) signed char sb[4] = { -1, -2, -3, -4 };
    register __attribute__((unused)) int r = 0;
    const __attribute__((unused)) int k = 7;

    if (offsetof(struct m, c) != 16) return 1;
    if (offsetof(struct m, s) != 24) return 2;
    if (offsetof(struct m, d) != 28) return 3;
    if (sizeof(struct m) != 32) return 3;
    if (__alignof__(s64) != 64 || __alignof__(sb) != 32) return 10;
    if ((uintptr_t)g64 & 63) return 4;
    if ((uintptr_t)&gv32 & 31) return 5;
    if ((uintptr_t)s64 & 63) return 6;
    if ((uintptr_t)sb & 31) return 7;
    if (g64[2] != 3 || gv32 != 5 || sb[3] != -4) return 9;
    s64[0] = (char)(pad0 + pad1 + pad2);
    return take(r, k) + 29 + s64[0] + (int)(const __attribute__((unused)) int)0;
}
