/* POSIX's pseudo-random generators: random and its state functions, the
 * *rand48 family, and rand_r.
 *
 * Each produces the SAME sequence as glibc's for the same seed. These
 * generators are not good -- none of them is fit for anything an attacker
 * may see -- but programs print their output, and tests compare what was
 * printed, and a port whose random(3) disagrees with the system it came
 * from looks broken when it is not. The algorithms are the historical
 * BSD and System V ones, which is what glibc implements.
 *
 * Not thread-safe, as POSIX permits for all but rand_r; the *_r-free
 * interfaces share one state by definition.
 */
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

/* ---- random: BSD's additive feedback generator -----------------------------
 *
 * r[i] = r[i - deg] + r[i - deg + sep], 32 bits wrapping, of which the top
 * 31 are returned. The state is a caller-supplied int32_t array whose first
 * word records the generator type and the read position, so setstate can
 * hand a state back and resume it exactly: word 0 is
 * TYPES * (rear index) + type, as BSD lays it out. Type 0, an 8-byte state,
 * is a plain linear congruential generator instead. */
#define TYPES 5
static const int degrees[TYPES] = { 0, 7, 15, 31, 63 };
static const int seps[TYPES]    = { 0, 3, 1, 3, 1 };

static int32_t deftab[32];      /* the default state, as initstate(1, ., 128) */
static int32_t *state = deftab + 1;
static int type = 3, deg = 31, sep = 3;
static int32_t *fptr, *rptr;    /* front and rear, into state */
static int inited;

static int32_t next_raw(void)
{
    if (type == 0) {
        state[0] = (int32_t)(((uint32_t)state[0] * 1103515245u + 12345u) &
                             0x7fffffff);
        return state[0];
    }
    int32_t *end = state + deg;
    uint32_t v = (uint32_t)*fptr + (uint32_t)*rptr;
    *fptr = (int32_t)v;
    if (++fptr >= end) {
        fptr = state;
        ++rptr;
    } else if (++rptr >= end)
        rptr = state;
    return (int32_t)(v >> 1);
}

__attribute__((weak)) void srandom(unsigned seed)
{
    inited = 1;
    if (seed == 0)
        seed = 1;
    state[0] = (int32_t)seed;
    if (type == 0)
        return;
    /* 16807 * word mod 2^31 - 1 without overflow, by Schrage's method --
     * and exactly glibc's arithmetic, signed, so a seed above INT_MAX
     * starts the same sequence there and here. */
    int32_t word = (int32_t)seed;
    for (int i = 1; i < deg; i++) {
        int32_t hi = word / 127773, lo = word % 127773;
        word = 16807 * lo - 2836 * hi;
        if (word < 0)
            word += 2147483647;
        state[i] = word;
    }
    fptr = state + sep;
    rptr = state;
    for (int i = 10 * deg; i > 0; i--)
        next_raw();
}

__attribute__((weak)) long random(void)
{
    if (!inited)
        srandom(1);
    return next_raw();
}

static void save_position(void)
{
    state[-1] = type == 0 ? 0 : (int32_t)(TYPES * (rptr - state) + type);
}

/* A state of n bytes: the larger the state, the longer the period. Less
 * than 8 bytes holds nothing to generate from. */
__attribute__((weak)) char *initstate(unsigned seed, char *buf, size_t n)
{
    if (n < 8) {
        errno = EINVAL;
        return NULL;
    }
    if (!inited)
        srandom(1);
    char *old = (char *)(state - 1);
    save_position();
    type = n < 32 ? 0 : n < 64 ? 1 : n < 128 ? 2 : n < 256 ? 3 : 4;
    deg = degrees[type];
    sep = seps[type];
    state = (int32_t *)(void *)buf + 1;
    srandom(seed);
    save_position();
    return old;
}

__attribute__((weak)) char *setstate(char *buf)
{
    int32_t *ns = (int32_t *)(void *)buf + 1;
    int t = ns[-1] % TYPES;
    if (t < 0 || t >= TYPES) {
        errno = EINVAL;
        return NULL;
    }
    if (!inited)
        srandom(1);
    char *old = (char *)(state - 1);
    save_position();
    type = t;
    deg = degrees[t];
    sep = seps[t];
    state = ns;
    if (t != 0) {
        int rear = ns[-1] / TYPES;
        rptr = ns + rear;
        fptr = ns + (rear + sep) % deg;
    }
    return old;
}

/* ---- the *rand48 family: System V's 48-bit generator -----------------------
 *
 * x' = a * x + c mod 2^48, with a and c changeable by lcong48. Each
 * function returns a different slice of the new x. The ones taking an
 * xsubi array keep the 48 bits in the caller's three shorts rather than
 * in the shared state, but share a and c, as POSIX specifies. */
static unsigned short x48[3];
static uint64_t a48 = 0x5DEECE66Dull;
static unsigned short c48 = 0xB;

static uint64_t step48(unsigned short xs[3])
{
    uint64_t x = (uint64_t)xs[0] | (uint64_t)xs[1] << 16 |
                 (uint64_t)xs[2] << 32;
    x = (a48 * x + c48) & 0xFFFFFFFFFFFFull;
    xs[0] = (unsigned short)x;
    xs[1] = (unsigned short)(x >> 16);
    xs[2] = (unsigned short)(x >> 32);
    return x;
}

/* all 48 bits, as a fraction: exact, since a double holds 53 */
__attribute__((weak)) double erand48(unsigned short xs[3]) { return (double)step48(xs) * 0x1p-48; }
__attribute__((weak)) double drand48(void)                 { return erand48(x48); }
__attribute__((weak)) long   nrand48(unsigned short xs[3]) { return (long)(step48(xs) >> 17); }
__attribute__((weak)) long   lrand48(void)                 { return nrand48(x48); }
/* the top 32 bits, signed */
__attribute__((weak)) long jrand48(unsigned short xs[3])
{
    return (long)(int32_t)(uint32_t)(step48(xs) >> 16);
}
__attribute__((weak)) long mrand48(void) { return jrand48(x48); }

__attribute__((weak)) void srand48(long seed)
{
    x48[0] = 0x330E;
    x48[1] = (unsigned short)seed;
    x48[2] = (unsigned short)((unsigned long)seed >> 16);
    a48 = 0x5DEECE66Dull;
    c48 = 0xB;
}

__attribute__((weak)) unsigned short *seed48(unsigned short s[3])
{
    static unsigned short old[3];
    for (int i = 0; i < 3; i++) {
        old[i] = x48[i];
        x48[i] = s[i];
    }
    a48 = 0x5DEECE66Dull;
    c48 = 0xB;
    return old;
}

__attribute__((weak)) void lcong48(unsigned short p[7])
{
    for (int i = 0; i < 3; i++)
        x48[i] = p[i];
    a48 = (uint64_t)p[3] | (uint64_t)p[4] << 16 | (uint64_t)p[5] << 32;
    c48 = p[6];
}

/* ---- rand_r: the one with its state in the caller's hands ------------------
 *
 * Three steps of C's example generator, 11 + 10 + 10 bits of them: what
 * glibc returns, so the same seed gives the same numbers. */
__attribute__((weak)) int rand_r(unsigned *seed)
{
    unsigned next = *seed;
    next = next * 1103515245u + 12345u;
    unsigned r = (next / 65536) % 2048;
    next = next * 1103515245u + 12345u;
    r = r << 10 ^ (next / 65536) % 1024;
    next = next * 1103515245u + 12345u;
    r = r << 10 ^ (next / 65536) % 1024;
    *seed = next;
    return (int)r;
}
