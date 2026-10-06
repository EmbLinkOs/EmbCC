/* Switches, small and sparse, against the host.
 *
 * Under -Os on ARMv7-M a dense switch of four cases or more is a tbh
 * table, and a sparse one a tree of compares in which a compare the one
 * before it already made -- `== k`, then `> k` -- reuses its flags. Each
 * switch below is asked every case value, the values beside them and
 * the extremes, and prints what it chose; the host runs the same source
 * for the reference.
 */
extern void writec(int c);
extern void puts_(const char *s);
extern void putn(long v);

#define NI __attribute__((noinline))

NI static int d4(unsigned x)
{
    switch (x) { case 0: return 10; case 1: return 11; case 2: return 12;
                 case 3: return 13; }
    return -1;
}
NI static int d5s(int x)              /* signed, below zero */
{
    switch (x) { case -2: return 20; case -1: return 21; case 0: return 22;
                 case 1: return 23; case 2: return 24; default: return -2; }
}
NI static int d5gap(unsigned char x)  /* a hole, and fallthrough */
{
    int r = 0;
    switch (x) {
    case 10: r += 1;               /* falls through */
    case 11: r += 2; break;
    case 13: r += 4; break;
    case 14: r += 8;               /* falls through */
    case 15: r += 16; break;
    default: r = -3;
    }
    return r;
}
NI static int d4big(unsigned x)       /* a base far from zero */
{
    switch (x) { case 100000: return 1; case 100001: return 2;
                 case 100002: return 3; case 100003: return 4; }
    return 0;
}
NI static int sparse(int x)           /* a tree: == k, then > k */
{
    switch (x) {
    case -1000: return 1; case 1: return 2; case 50: return 3;
    case 900: return 4; case 1000: return 5; case 7000: return 6;
    case 9000: return 7; case 70000: return 8; default: return 0;
    }
}
NI static int sparseu(unsigned x)
{
    switch (x) {
    case 3: return 1; case 30: return 2; case 300: return 3;
    case 3000: return 4; case 30000: return 5; case 0x80000000u: return 6;
    case 0xfffffffeu: return 7; default: return 0;
    }
}
/* two switches on one value, back to back: the second's compares may
 * not reuse flags across the label between them */
NI static int twice(int x)
{
    int r = 0;
    switch (x) { case 5: r = 1; break; case 60: r = 2; break;
                 case 700: r = 3; break; case 8000: r = 4; break;
                 case 90000: r = 5; break; }
    switch (x) { case 5: r += 10; break; case 61: r += 20; break;
                 case 700: r += 30; break; case 8001: r += 40; break;
                 case 90000: r += 50; break; }
    return r;
}

/* a label between two identical compares: the path that jumps to it
 * brings the flags of another compare, so the second must be made */
NI static int join(int x, int y)
{
    if (y > 3)
        goto M;
    if (x == 9)
        goto R1;
M:
    if (x == 9)
        return 2;
    return 0;
R1:
    return 1;
}

static const long probes[] = {
    -2147483647L - 1, -1001, -1000, -999, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6,
    9, 10, 11, 12, 13, 14, 15, 16, 29, 30, 31, 49, 50, 51, 59, 60, 61, 299,
    300, 301, 699, 700, 701, 899, 900, 901, 999, 1000, 1001, 2999, 3000,
    6999, 7000, 7001, 7999, 8000, 8001, 8999, 9000, 9001, 29999, 30000,
    69999, 70000, 70001, 89999, 90000, 99999, 100000, 100001, 100002,
    100003, 100004, 2147483647L,
};

int main(void)
{
    int n = (int)(sizeof probes / sizeof probes[0]);
    for (int k = 0; k < n; k++) {
        long p = probes[k];
        putn(d4((unsigned)p)); putn(d5s((int)p)); putn(d5gap((unsigned char)p));
        putn(d4big((unsigned)p)); putn(sparse((int)p));
        putn(sparseu((unsigned)p)); putn(sparseu((unsigned)p - 2u));
        putn(twice((int)p));
        writec('\n');
    }
    for (int x = 8; x <= 10; x++)
        for (int y = 2; y <= 5; y++)
            putn(join(x, y));
    writec('\n');
    putn(sparseu(0x80000000u)); putn(sparseu(0xfffffffeu));
    putn(sparseu(0xffffffffu)); writec('\n');
    puts_("==END==\n");
    return 0;
}
