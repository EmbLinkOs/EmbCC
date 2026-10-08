/* __builtin_return_address(0) and __builtin_frame_address(0), level 0:
 * every target has them, whether or not its code keeps a frame-pointer
 * chain (where it does not, only level 0 exists).
 *
 * - the return address is inside the caller's code -- SPARC's is the call
 *   instruction itself, the others' the one after it -- and stays the
 *   same after the function has made calls of its own;
 * - the frame address is near the function's own locals (above them
 *   where it is the stack pointer at entry, below them where it is the
 *   frame pointer at the frame's foot, as AArch64's and PowerPC's), and
 *   a callee's is below its caller's: the stack grows down everywhere;
 * - both hold in a function with a variable-length array, whose frame
 *   is addressed from a frame register, in a variadic one, and in a
 *   leaf.
 *
 * Code addresses are compared as integers: on AVR they are word
 * addresses, as its function pointers are. */
// expect-exit: 42

typedef unsigned long uaddr;

static int bad;
static volatile int vn = 7;

#define NEAR(a, b) ((a) > (b) ? (a) - (b) < 4096 : (b) - (a) < 4096)

__attribute__((noinline)) static void *leaf_ra(void)
{
    return __builtin_return_address(0);
}

__attribute__((noinline)) static void *leaf_fa(volatile char **local)
{
    volatile char x = 1;
    *local = &x;
    return __builtin_frame_address(0);
}

__attribute__((noinline)) static void *calls_then_ra(void)
{
    void *before = __builtin_return_address(0);
    volatile char *p;
    leaf_fa(&p);
    leaf_ra();
    void *after = __builtin_return_address(0);
    if (before != after)
        bad |= 1;
    return after;
}

__attribute__((noinline)) static void *vla_fa(volatile char **local, void **ra)
{
    volatile char buf[vn];
    buf[0] = 2;
    *local = &buf[0];
    *ra = __builtin_return_address(0);
    return __builtin_frame_address(0);
}

/* a variadic function pushes its register arguments above the saved
 * registers (32-bit ARM): its frame address is above them too */
__attribute__((noinline)) static void *va_fa(volatile char **local,
                                             void **ra, int n, ...)
{
    volatile char x = (char)n;
    *local = &x;
    *ra = __builtin_return_address(0);
    return __builtin_frame_address(0);
}

__attribute__((noinline)) static int caller(void)
{
    uaddr lo = (uaddr)(void *)caller, hi = lo + 8192;
    uaddr r = (uaddr)leaf_ra();
    if (!(r > lo && r < hi))
        bad |= 2;
    r = (uaddr)calls_then_ra();
    if (!(r > lo && r < hi))
        bad |= 4;

    volatile char mine = 3, *theirs;
    uaddr fa_me = (uaddr)__builtin_frame_address(0);
    uaddr fa_callee = (uaddr)leaf_fa(&theirs);
    if (!NEAR(fa_callee, (uaddr)theirs))
        bad |= 8;
    if (!NEAR(fa_me, (uaddr)&mine))
        bad |= 16;
    if (!(fa_callee < fa_me))
        bad |= 32;

    void *vra;
    uaddr fa_vla = (uaddr)vla_fa(&theirs, &vra);
    if (!NEAR(fa_vla, (uaddr)theirs) || !(fa_vla < fa_me))
        bad |= 64;
    if (!((uaddr)vra > lo && (uaddr)vra < hi))
        bad |= 128;

    uaddr fa_va = (uaddr)va_fa(&theirs, &vra, 3, 1, 2, 3);
    if (!NEAR(fa_va, (uaddr)theirs) || !(fa_va < fa_me) ||
        !((uaddr)vra > lo && (uaddr)vra < hi))
        bad |= 512;
    return mine;
}

int main(void)
{
    if (caller() != 3)
        bad |= 256;
    return bad ? 1 + (bad & 0x3f) + (bad >> 9) * 64 : 42;
}
