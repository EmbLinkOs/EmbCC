/* GCC's __builtin_powi, powif and powil (libgcc's __powi*f2, lib/rt's
 * here), __builtin_popcountg on every unsigned width, and
 * __builtin_extract_return_addr / frob_return_addr, which undo each
 * other. powi of small integers is exact in every format, so the
 * answers are compared exactly. */
// expect-exit: 42

static volatile double two = 2.0, three = 3.0, half = 0.5;
static volatile float ftwo = 2.0f;
static volatile long double ltwo = 2.0L;
static volatile int k10 = 10, km3 = -3, k0 = 0;

int main(void)
{
    int bad = 0;
    bad |= (__builtin_powi(two, k10) != 1024.0 ||
            __builtin_powi(three, 5) != 243.0 ||
            __builtin_powi(two, km3) != 0.125 ||
            __builtin_powi(half, 2) != 0.25 ||
            __builtin_powi(three, k0) != 1.0) << 0;
    bad |= (__builtin_powif(ftwo, k10) != 1024.0f ||
            __builtin_powif(ftwo, km3) != 0.125f) << 1;
    bad |= (__builtin_powil(ltwo, 70) != 1180591620717411303424.0L ||
            __builtin_powil(ltwo, km3) != 0.125L) << 2;
    bad |= (__builtin_popcountg((unsigned char)0xff) != 8 ||
            __builtin_popcountg((unsigned short)0x8001) != 2 ||
            __builtin_popcountg(0xf0f0f0f0u) != 16 ||
            __builtin_popcountg(0xffffffffffffffffull) != 64 ||
            __builtin_popcountg((unsigned long)1) != 1) << 3;
    {
        void *r = __builtin_return_address(0);
        bad |= (__builtin_frob_return_addr(__builtin_extract_return_addr(r))
                != r) << 4;
    }
    return bad ? bad : 42;
}
