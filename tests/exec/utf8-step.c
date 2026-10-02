/* A UTF-8 decoder stepped a byte at a time: lib/libc's mbrtowc, with its
 * libc calls replaced (a byte loop for memset, a global for errno). At
 * -O2 it gives the Thumb backend a compare-and-branch followed by a
 * jump, which the first pass merged into one inverted branch while still
 * measuring it as a `cbz` candidate. The second pass decides `cbz` at
 * the compare, before it can see the jump, and emitted both: one branch
 * more than the first pass counted, so every later branch took its
 * neighbour's short-or-long decision, and ARMv7-M refused the function
 * ("a relaxed branch no longer reaches its label"). Every target must
 * compile it, and decode: sequences of one to four bytes whole and a
 * byte at a time, the refusals (overlong, surrogate, past U+10FFFF, a
 * bad lead, a broken continuation), NUL, a reset, and no input. */
// expect-exit: 42
struct mbst { unsigned int w[3]; };
static int g_err;
struct state;
static void clear(struct state *k);
struct state {
    unsigned int wch;
    unsigned char want;
    unsigned char have;
    unsigned int lowbound;
};

static void clear(struct state *k)
{
    volatile unsigned char *p = (volatile unsigned char *)k;
    for (unsigned i = 0; i < sizeof *k; i++)
        p[i] = 0;
}
static struct state *st(struct mbst *ps)
{
    static struct mbst internal;
    return (struct state *)(ps ? ps : &internal);
}


static int is_surrogate(unsigned int c) { return c >= 0xD800 && c <= 0xDFFF; }
static int too_big(unsigned int c) { return c > 0x10FFFF; }

unsigned long u8_step(unsigned int *__restrict pwc, const char *__restrict s,
                      unsigned long n,
                      struct mbst *__restrict ps)
{
    struct state *k = st(ps);

    /* A null s means "reset to the initial state", and the standard
     * spells it as mbrtowc(0, "", 1, ps). */
    if (!s) {
        clear(k);
        return 0;
    }
    if (n == 0)
        return (unsigned long)-2;        /* incomplete: nothing to look at */

    unsigned long used = 0;
    while (used < n) {
        unsigned char b = (unsigned char)s[used++];

        if (k->want == 0) {
            if (b < 0x80) {
                if (pwc)
                    *pwc = (unsigned int)b;
                return b ? used : 0;   /* a NUL converts and returns 0 */
            }
            /* A continuation byte with nothing to continue, or one of
             * the two bytes that can never begin a sequence. */
            if (b < 0xC2 || b > 0xF4) {
                g_err = 84;
                return (unsigned long)-1;
            }
            if (b < 0xE0) { k->want = 1; k->wch = b & 0x1Fu; k->lowbound = 0x80; }
            else if (b < 0xF0) { k->want = 2; k->wch = b & 0x0Fu; k->lowbound = 0x800; }
            else { k->want = 3; k->wch = b & 0x07u; k->lowbound = 0x10000; }
            k->have = 1;
            continue;
        }

        if ((b & 0xC0u) != 0x80u) {
            /* A byte that is not a continuation, in the middle of a
             * sequence. The sequence is broken; the state is cleared so
             * the caller can resynchronise rather than stay wedged. */
            clear(k);
            g_err = 84;
            return (unsigned long)-1;
        }
        k->wch = (k->wch << 6) | (b & 0x3Fu);
        k->have++;
        if (--k->want)
            continue;

        unsigned int c = k->wch;
        unsigned int low = k->lowbound;
        clear(k);
        if (c < low || is_surrogate(c) || too_big(c)) {
            g_err = 84;
            return (unsigned long)-1;
        }
        if (pwc)
            *pwc = (unsigned int)c;
        return c ? used : 0;
    }
    /* Ran out of input mid-character: -2, and the state remembers where
     * we were so the next call continues rather than restarts. */
    return (unsigned long)-2;
}

int main(void)
{
    struct mbst m = { { 0, 0, 0 } };
    unsigned int wc = 0;
    const unsigned long BAD = (unsigned long)-1, MORE = (unsigned long)-2;
    if (u8_step(&wc, "A", 1, &m) != 1 || wc != 'A') return 1;
    if (u8_step(&wc, "\xC3\xA9", 2, &m) != 2 || wc != 0xE9) return 2;
    if (u8_step(&wc, "\xE2\x82\xAC", 3, &m) != 3 || wc != 0x20AC) return 3;
    if (u8_step(&wc, "\xF0\x9F\x98\x80", 4, &m) != 4 || wc != 0x1F600) return 4;
    if (u8_step(&wc, "\xE2", 1, &m) != MORE || u8_step(&wc, "\x82", 1, &m) != MORE) return 5;
    if (u8_step(&wc, "\xAC", 1, &m) != 1 || wc != 0x20AC) return 6;
    g_err = 0; if (u8_step(&wc, "\xE0\x80\x80", 3, &m) != BAD || g_err != 84) return 7;
    if (u8_step(&wc, "\xED\xA0\x80", 3, &m) != BAD) return 8;
    if (u8_step(&wc, "\xF4\x90\x80\x80", 4, &m) != BAD) return 9;
    if (u8_step(&wc, "\xFF", 1, &m) != BAD) return 10;
    if (u8_step(&wc, "\xC3\x41", 2, &m) != BAD || u8_step(&wc, "B", 1, &m) != 1 || wc != 'B') return 11;
    if (u8_step(&wc, "", 1, &m) != 0 || wc != 0) return 12;
    if (u8_step(0, 0, 0, &m) != 0 || u8_step(&wc, "x", 0, &m) != MORE) return 13;
    if (u8_step(&wc, "\xC3", 1, 0) != MORE || u8_step(&wc, "\xA9", 1, 0) != 1 || wc != 0xE9) return 14;
    return 42;
}
