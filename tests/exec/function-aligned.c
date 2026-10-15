/* __attribute__((aligned(N))) on a function: its first instruction is on
 * N, and __alignof__ of it says so (GCC's align-3.c, where the attribute
 * is on the prototype only). It was parsed and dropped, so the function
 * landed wherever it fell and __alignof__ measured the pointer it decays
 * to. */
// expect-exit: 42
#include <stdint.h>

void on_proto(void) __attribute__((aligned(256)));
void on_proto(void) {}

__attribute__((aligned(64), noinline)) static int on_defn(int x)
{
    return x + 1;
}

__attribute__((noinline)) static int plain(int x) { return x - 1; }

/* the address the instruction is at: a Thumb function's pointer has its
 * low bit set, and AVR's counts words of flash, not bytes */
static uintptr_t at(uintptr_t p)
{
#if defined(__thumb__)
    p &= ~(uintptr_t)1;
#endif
#if defined(__AVR__)
    p *= 2;
#endif
    return p;
}

int main(void)
{
    int ok = 0;
    ok += (at((uintptr_t)on_proto) & 255) == 0;
    ok += (at((uintptr_t)on_defn) & 63) == 0;
    ok += __alignof__(on_proto) == 256;
    ok += __alignof__(on_defn) == 64;
    /* with nothing said, a power of two no larger than an instruction */
    ok += __alignof__(plain) >= 1 && __alignof__(plain) <= 4 &&
          (__alignof__(plain) & (__alignof__(plain) - 1)) == 0;
    on_proto();
    ok += on_defn(1) + plain(1) == 2;
    return ok == 6 ? 42 : ok;
}
