/* The AVR compiler runtime, sixty-four bits: multiply, divide and remainder.
 *
 * A SEPARATE object from lib/rt/avr.c on purpose. A runtime library is one
 * routine per object so that a program pays only for what it calls, and this
 * target makes that concrete: with the 64-bit helpers in the same file, an
 * ATmega328P program that never writes `long long` still carried them, and
 * at -O0 that took a test image to 34718 bytes on a part with 32768 of
 * flash. It failed as `CALL 0x8770` -- a call past the end of memory.
 *
 * The backend does eight-byte add, subtract, shift, compare and branch
 * itself, a byte at a time through memory: `ldd` and `std` do not touch
 * SREG on this machine, so the carry survives the loads that the next byte
 * needs. Multiply and divide are calls, for the same reason the four-byte
 * ones are -- AVR's `mul` is 8x8 into r1:r0 and destroys the zero register,
 * and there is no divide instruction at all.
 *
 * Same constraint as every other file here (rt.h): no routine may use the
 * operation it implements. These are shift-and-add and restoring division,
 * and every shift is a constant one or an `x += x`.
 *
 * The names are libgcc's, so an object of ours links beside one of theirs.
 */
#ifdef __AVR__

typedef unsigned long long u64_;
typedef long long          s64_;

s64_ __muldi3(s64_ a, s64_ b)
{
    u64_ x = (u64_)a, y = (u64_)b, r = 0;

    while (y) {
        if (y & 1)
            r += x;
        x += x;
        y >>= 1;
    }
    return (s64_)r;
}

static u64_ udivmod64(u64_ n, u64_ d, u64_ *rem)
{
    u64_ q = 0, r = 0;
    int i;

    if (d == 0) {
        if (rem)
            *rem = n;
        return ~(u64_)0;
    }
    for (i = 0; i < 64; i++) {
        r += r;
        if (n & 0x8000000000000000ULL)
            r |= 1;
        n += n;
        q += q;
        if (r >= d) {
            r -= d;
            q |= 1;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

u64_ __udivdi3(u64_ a, u64_ b) { return udivmod64(a, b, 0); }

u64_ __umoddi3(u64_ a, u64_ b)
{
    u64_ r;
    udivmod64(a, b, &r);
    return r;
}

static u64_ mag64(s64_ v, int *neg)
{
    if (v < 0) {
        *neg = !*neg;
        return -(u64_)v;
    }
    return (u64_)v;
}

s64_ __divdi3(s64_ a, s64_ b)
{
    int neg = 0;
    u64_ ua = mag64(a, &neg), ub = mag64(b, &neg);
    u64_ q = udivmod64(ua, ub, 0);
    return neg ? -(s64_)q : (s64_)q;
}

s64_ __moddi3(s64_ a, s64_ b)
{
    int dneg = 0, bneg = 0;
    u64_ r;
    u64_ ua = mag64(a, &dneg), ub = mag64(b, &bneg);
    udivmod64(ua, ub, &r);
    return dneg ? -(s64_)r : (s64_)r;    /* the DIVIDEND's sign */
}


#else

/* Not this machine. lib/rt is built by a wildcard for every target, and a
 * translation unit has to contain at least one declaration. */
typedef int embcc_rt_avr64_is_not_this_target;

#endif /* __AVR__ */
