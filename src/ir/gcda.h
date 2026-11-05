/* A .gcda file -- GCC 16's -- merged into another of the same program:
 * the runtime's (lib/gcov/gcov.c), when a program's run finds the file of
 * an earlier one, and the host's (embcc --gcov-extract), when a board
 * printed its counts on a console. One file's counts are added to the
 * other's, and their summaries combined, when the two are the same
 * program: the same header (magic, version, stamp) and the same records,
 * tag by tag and length by length, with the same function records. Any
 * difference -- the program was rebuilt -- and nothing is merged: the
 * new file replaces the old, as GCC's libgcov does.
 *
 * Plain C, no library: the runtime has none it may assume. Both files are
 * in one byte order (they were written by one target); which order is
 * read from the magic. */

#ifndef EMBCC_GCDA_H
#define EMBCC_GCDA_H

#define GCDA_MAGIC 0x67636461u
#define GCDA_TAG_FUNCTION 0x01000000u
#define GCDA_TAG_COUNTER_ARCS 0x01a10000u
#define GCDA_TAG_OBJECT_SUMMARY 0xa1000000u

static unsigned gcda_rd(const unsigned char *p, int be)
{
    return be ? (unsigned)p[3] | (unsigned)p[2] << 8 | (unsigned)p[1] << 16 |
                (unsigned)p[0] << 24
              : (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 |
                (unsigned)p[3] << 24;
}

static void gcda_wr(unsigned char *p, unsigned v, int be)
{
    for (int k = 0; k < 4; k++)
        p[k] = (unsigned char)(v >> (be ? 24 - 8 * k : 8 * k));
}

/* GCC 12's format on ("B2..*"): lengths in bytes, a checksum word in the
 * header (16 bytes of it); before, lengths in words (12) */
static int gcda_bytes(const unsigned char *p, int be)
{
    unsigned v = gcda_rd(p + 4, be);
    int c0 = (int)(v >> 24) & 0xff;
    return c0 > 'B' || (c0 == 'B' && (int)((v >> 16) & 0xff) >= '2');
}

/* -1 when p is not a .gcda; else 1 big-endian, 0 little */
static int gcda_order(const unsigned char *p, unsigned long n)
{
    if (n < 16)
        return -1;
    if (gcda_rd(p, 0) == GCDA_MAGIC)
        return 0;
    if (gcda_rd(p, 1) == GCDA_MAGIC)
        return 1;
    return -1;
}

/* Add old's counts to img (in place); 1 when they were merged, 0 when the
 * two are not the same program's (img is then left as it was). */
static int gcda_merge(unsigned char *img, unsigned long n,
                      const unsigned char *old, unsigned long on)
{
    int be = gcda_order(img, n);
    if (be < 0 || gcda_order(old, on) != be)
        return 0;
    for (int k = 4; k < 12; k++)              /* version and stamp */
        if (img[k] != old[k])
            return 0;
    int bytes = gcda_bytes(img, be);
    unsigned unit = bytes ? 1 : 4, h = bytes ? 16 : 12;
    if (bytes)
        for (int k = 12; k < 16; k++)         /* the checksum */
            if (img[k] != old[k])
                return 0;
    /* first pass: the same records */
    unsigned long a = h, b = h;
    for (;;) {
        if (a + 4 > n || b + 4 > on)
            return 0;
        unsigned ta = gcda_rd(img + a, be), tb = gcda_rd(old + b, be);
        if (ta != tb)
            return 0;
        if (!ta)
            break;
        if (a + 8 > n || b + 8 > on)
            return 0;
        unsigned la = gcda_rd(img + a + 4, be) * unit;
        unsigned lb = gcda_rd(old + b + 4, be) * unit;
        if (la != lb || a + 8 + la > n || b + 8 + lb > on)
            return 0;
        if (ta == GCDA_TAG_FUNCTION)
            for (unsigned k = 0; k < la; k++)
                if (img[a + 8 + k] != old[b + 8 + k])
                    return 0;
        a += 8 + la;
        b += 8 + lb;
    }
    /* second pass: add */
    unsigned long long max = 0;
    unsigned long sum_at = 0;
    a = b = h;
    for (;;) {
        unsigned t = gcda_rd(img + a, be);
        if (!t)
            break;
        unsigned l = gcda_rd(img + a + 4, be) * unit;
        if (t == GCDA_TAG_COUNTER_ARCS) {
            for (unsigned k = 0; k + 8 <= l; k += 8) {
                unsigned char *p = img + a + 8 + k;
                const unsigned char *q = old + b + 8 + k;
                unsigned long long v =
                    ((unsigned long long)gcda_rd(p + 4, be) << 32 |
                     gcda_rd(p, be)) +
                    ((unsigned long long)gcda_rd(q + 4, be) << 32 |
                     gcda_rd(q, be));
                gcda_wr(p, (unsigned)v, be);
                gcda_wr(p + 4, (unsigned)(v >> 32), be);
                if (v > max)
                    max = v;
            }
        } else if (t == GCDA_TAG_OBJECT_SUMMARY && l >= 8) {
            sum_at = a + 8;
            gcda_wr(img + a + 8, gcda_rd(img + a + 8, be) +
                                 gcda_rd(old + b + 8, be), be);
        }
        a += 8 + l;
        b += 8 + l;
    }
    if (sum_at)
        gcda_wr(img + sum_at + 4, max > 0xffffffffu ? 0xffffffffu
                                                    : (unsigned)max, be);
    return 1;
}

#endif
