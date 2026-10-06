// expect-exit: 42
/* Block copies and clears of many sizes -- struct assignment, a clear
 * loop the optimizer makes one memzero, a large struct returned by value
 * -- around the sizes where the Cortex-M backend changes method:
 * straight-line code up to 64 or 128 bytes, then a loop of 16 or 32
 * bytes a trip with the remainder after it. Each result is compared
 * byte by byte, including the bytes on either side that must not be
 * touched. */

/* Byte loops through a volatile pointer, so that neither these nor the
 * checks become the copies being tested (or calls to a libc the board
 * test does not link). */
static void vset(unsigned char *p, int c, long n)
{
    volatile unsigned char *q = p;
    for (long k = 0; k < n; k++) q[k] = (unsigned char)c;
}
static void vcopy(unsigned char *d, const unsigned char *s, long n)
{
    volatile unsigned char *q = d;
    for (long k = 0; k < n; k++) q[k] = s[k];
}
static int vdiff(const unsigned char *a, const unsigned char *b, long n)
{
    const volatile unsigned char *q = a;
    for (long k = 0; k < n; k++) if (q[k] != b[k]) return 1;
    return 0;
}

#define NI __attribute__((noinline))

#define S(n) struct s##n { unsigned char b[n]; };                           \
    NI static void cp##n(struct s##n *d, const struct s##n *s) { *d = *s; } \
    NI static struct s##n ret##n(const struct s##n *s) { return *s; }
S(3) S(60) S(64) S(65) S(127) S(128) S(129) S(131) S(160) S(161) S(300)
S(2048) S(4100)

/* Clears the optimizer turns into one memzero of n words. */
#define Z(n) NI static void z##n(unsigned *p) {                              \
    for (int i = 0; i < n; i++) p[i] = 0; }
Z(15) Z(16) Z(17) Z(32) Z(33) Z(40) Z(512)

static unsigned char src[4200], dst[4200 + 16];

static int check_copy(unsigned char *d, long n)
{
    const volatile unsigned char *q = d;
    if (vdiff(d + 8, src, n)) return 1;
    for (int k = 0; k < 8; k++)
        if (q[k] != 0xee || q[8 + n + k] != 0xee) return 1;
    return 0;
}

static void prime(void)
{
    vset(dst, 0xee, sizeof dst);
}

#define TRY(n) do {                                                          \
    prime(); cp##n((struct s##n *)(dst + 8), (const struct s##n *)src);      \
    if (check_copy(dst, n)) bad |= 1;                                     \
    prime(); { struct s##n r = ret##n((const struct s##n *)src);              \
      vcopy(dst + 8, r.b, n); }                                              \
    if (check_copy(dst, n)) bad |= 2;                                     \
} while (0)

#define TRYZ(n) do {                                                         \
    unsigned w[n + 4];                                                       \
    for (int k = 0; k < n + 4; k++) w[k] = 0x5a5a5a5au + (unsigned)k;        \
    z##n(w + 2);                                                             \
    for (int k = 0; k < n + 4; k++)                                          \
        if (w[k] != (k < 2 || k >= n + 2 ? 0x5a5a5a5au + (unsigned)k : 0u))  \
            bad |= 4;                                                        \
} while (0)

static struct s4100 big;

int main(void)
{
    int bad = 0;
    for (int k = 0; k < (int)sizeof src; k++)
        src[k] = (unsigned char)(k * 7 + 3);
    TRY(3); TRY(60); TRY(64); TRY(65); TRY(127); TRY(128); TRY(129);
    TRY(131); TRY(160); TRY(161); TRY(300); TRY(2048);
    prime(); cp4100((struct s4100 *)(dst + 8), (const struct s4100 *)src);
    if (check_copy(dst, 4100)) bad |= 8;
    big = ret4100((const struct s4100 *)src);
    if (vdiff(big.b, src, 4100)) bad |= 8;
    TRYZ(15); TRYZ(16); TRYZ(17); TRYZ(32); TRYZ(33); TRYZ(40); TRYZ(512);
    return bad ? bad : 42;
}
