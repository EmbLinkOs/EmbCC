/* Switches dense enough to be jump tables, driven through every value in
 * and around their range: the lowest case, the highest, one below, one
 * above, the holes, the default, a value far away on both sides, and
 * the wrap-around values (INT_MIN, UINT_MAX) that an index computed at
 * the wrong width would send into the table. Cases that fall through,
 * cases sharing a body, a switch on a char, on an unsigned, on a 64-bit
 * value, nested switches, a switch inside a loop whose case bodies
 * `continue` and `break`, and one whose case jumps back to a label
 * placed BEFORE the switch (so a table entry points backwards). Every
 * expectation is written out, never computed by another switch. */
// expect-exit: 42
#define NI __attribute__((noinline))

NI int dense0(int v)            /* 0..7, no default */
{
    switch (v) {
    case 0: return 10; case 1: return 11; case 2: return 12; case 3: return 13;
    case 4: return 14; case 5: return 15; case 6: return 16; case 7: return 17;
    }
    return -1;
}
NI int holes(int v)             /* 10..21 with holes and a default */
{
    switch (v) {
    case 10: return 1; case 11: return 2; case 13: return 3; case 14: return 4;
    case 17: return 5; case 21: return 6;
    default: return 99;
    }
}
NI int neg(int v)               /* -5..2 */
{
    switch (v) {
    case -5: return 5; case -4: return 4; case -3: return 3; case -2: return 2;
    case -1: return 1; case 0: return 0; case 1: return -1; case 2: return -2;
    default: return 77;
    }
}
NI int fall(int v)              /* fallthrough and shared bodies */
{
    int r = 0;
    switch (v) {
    case 1: r += 1;             /* falls into 2 */
    case 2: r += 10; break;
    case 3: case 4: case 5: r = 100 + v; break;
    case 6: r = 6;              /* falls into default */
    default: r += 1000; break;
    case 7: r = 7; break;
    }
    return r;
}
NI long uns(unsigned v)         /* an unsigned switch near the top of the range */
{
    switch (v) {
    case 0xFFFFFFF0u: return 1; case 0xFFFFFFF1u: return 2; case 0xFFFFFFF3u: return 3;
    case 0xFFFFFFF4u: return 4; case 0xFFFFFFFFu: return 5;
    default: return 0;
    }
}
NI int chr(char c)
{
    switch (c) {
    case 'a': return 1; case 'b': return 2; case 'c': return 3; case 'd': return 4;
    case 'f': return 6; case 'g': return 7;
    default: return 0;
    }
}
NI int wide(long long v)        /* a 64-bit switch value */
{
    switch (v) {
    case 1000000000000LL: return 1; case 1000000000001LL: return 2;
    case 1000000000002LL: return 3; case 1000000000004LL: return 4;
    case 1000000000005LL: return 5;
    default: return 0;
    }
}
NI int nested(int a, int b)
{
    switch (a) {
    case 0: return 100;
    case 1:
        switch (b) {
        case 0: return 10; case 1: return 11; case 2: return 12; case 3: return 13;
        default: return 19;
        }
    case 2: return 200;
    case 3: return 300;
    case 4:
        switch (b) { case 5: return 45; case 6: return 46; case 7: return 47; case 8: return 48; }
        return 49;
    default: return -1;
    }
}
NI long inloop(const int *a, int n)
{
    long s = 0;
    for (int i = 0; i < n; i++) {
        switch (a[i] & 7) {
        case 0: continue;
        case 1: s += 1; break;
        case 2: s += 2; continue;
        case 3: s += 3; break;
        case 4: s *= 2; break;
        case 5: if (s > 1000) return s; s += 5; break;
        case 6: s += 6; /* falls through */
        case 7: s += 7; break;
        }
        s += 100;
    }
    return s;
}
NI int backwards(int v, int rounds)
{
    int n = 0;
again:
    n++;
    switch (v) {                /* `case 2` jumps back above the switch */
    case 0: return n * 10;
    case 1: return n * 11;
    case 2: if (n < rounds) goto again; return n * 12;
    case 3: return n * 13;
    case 4: return n * 14;
    default: return -n;
    }
}
NI int backward2(int v)         /* `case 4: goto again;` -- an empty case block */
{                               /* that CFG cleanup forwards: the table entry */
    int n = 0;                  /* then names a label BEFORE the switch */
again:
    n++;
    if (n > 5) return -n;
    switch (v) {
    case 0: return n;
    case 1: v = 3; goto again;
    case 2: return 2 * n;
    case 3: return 3 * n;
    case 4: goto again;
    default: return 100;
    }
}
NI int big(int v)               /* 40 cases: wider than any short form */
{
    switch (v) {
#define C(k) case k: return k * 3 + 1;
    C(0) C(1) C(2) C(3) C(4) C(5) C(6) C(7) C(8) C(9) C(10) C(11) C(12) C(13) C(14)
    C(15) C(16) C(17) C(18) C(19) C(20) C(21) C(22) C(23) C(24) C(25) C(26) C(27)
    C(28) C(29) C(30) C(31) C(32) C(33) C(34) C(35) C(36) C(37) C(38) C(39)
#undef C
    default: return -1;
    }
}
NI int minmax(int v)            /* cases at the very bottom of int */
{
    switch (v) {
    case -2147483647 - 1: return 1; case -2147483647: return 2; case -2147483646: return 3;
    case -2147483644: return 4;
    default: return 0;
    }
}

int main(void)
{
    int bad = 0;
    static const int d0v[] = { -1, 0, 1, 7, 8, 100, -2147483647 - 1, 2147483647 };
    static const int d0r[] = { -1, 10, 11, 17, -1, -1, -1, -1 };
    for (int k = 0; k < 8; k++) if (dense0(d0v[k]) != d0r[k]) bad |= 1;
    static const int hv[] = { 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 21, 22, -2147483647 - 1, 2147483647, 0 };
    static const int hr[] = { 99, 1, 2, 99, 3, 4, 99, 99, 5, 99, 99, 6, 99, 99, 99, 99 };
    for (int k = 0; k < 16; k++) if (holes(hv[k]) != hr[k]) bad |= 2;
    static const int nv[] = { -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, -2147483647 - 1, 2147483647 };
    static const int nr[] = { 77, 5, 4, 3, 2, 1, 0, -1, -2, 77, 77, 77 };
    for (int k = 0; k < 12; k++) if (neg(nv[k]) != nr[k]) bad |= 4;
    static const int fv[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, -1 };
    static const int fr[] = { 1000, 11, 10, 103, 104, 105, 1006, 7, 1000, 1000 };
    for (int k = 0; k < 10; k++) if (fall(fv[k]) != fr[k]) bad |= 8;
    if (uns(0xFFFFFFF0u) != 1 || uns(0xFFFFFFF1u) != 2 || uns(0xFFFFFFF2u) != 0 ||
        uns(0xFFFFFFF3u) != 3 || uns(0xFFFFFFF4u) != 4 || uns(0xFFFFFFFFu) != 5 ||
        uns(0xFFFFFFEFu) != 0 || uns(0) != 0 || uns(15) != 0 || uns(0x7FFFFFFFu) != 0) bad |= 16;
    if (chr('a') != 1 || chr('b') != 2 || chr('c') != 3 || chr('d') != 4 || chr('e') != 0 ||
        chr('f') != 6 || chr('g') != 7 || chr('h') != 0 || chr('`') != 0 || chr(0) != 0 ||
        chr((char)0xff) != 0 || chr((char)0x80) != 0) bad |= 32;
    if (wide(1000000000000LL) != 1 || wide(1000000000001LL) != 2 || wide(1000000000002LL) != 3 ||
        wide(1000000000003LL) != 0 || wide(1000000000004LL) != 4 || wide(1000000000005LL) != 5 ||
        wide(1000000000006LL) != 0 || wide(999999999999LL) != 0 || wide(0) != 0 ||
        wide(-1000000000000LL) != 0 || wide(1000000000000LL + 0x100000000LL) != 0 ||
        wide(1000000000001LL - 0x100000000LL) != 0) bad |= 64;
    if (nested(0, 0) != 100 || nested(1, 0) != 10 || nested(1, 3) != 13 || nested(1, 4) != 19 ||
        nested(1, -1) != 19 || nested(2, 9) != 200 || nested(3, 0) != 300 || nested(4, 5) != 45 ||
        nested(4, 8) != 48 || nested(4, 9) != 49 || nested(4, 4) != 49 || nested(5, 5) != -1 ||
        nested(-1, 0) != -1) bad |= 128;
    {
        int a[16]; for (int i = 0; i < 16; i++) a[i] = i * 5 + 3;
        /* a[i]&7 for i=0..15: 3,0,5,2,7,4,1,6,3,0,5,2,7,4,1,6 */
        /* s: +3+100=103; cont; +5+100=208; +2 cont=210; +7+100=317; *2+100=734; +1+100=835; +6+7+100=948;
           +3+100=1051; cont; (s>1000) return 1051 */
        if (inloop(a, 16) != 1051) bad |= 256;
        if (inloop(a, 4) != 210) bad |= 256;
        if (inloop(a, 0) != 0) bad |= 256;
    }
    if (backwards(0, 3) != 10 || backwards(1, 3) != 11 || backwards(2, 3) != 36 || backwards(2, 1) != 12 ||
        backwards(3, 3) != 13 || backwards(4, 3) != 14 || backwards(5, 3) != -1 || backwards(-1, 3) != -1) bad |= 512;
    if (backward2(0) != 1 || backward2(1) != 6 || backward2(2) != 2 || backward2(3) != 3 ||
        backward2(4) != -6 || backward2(5) != 100 || backward2(-1) != 100) bad |= 4096;
    for (int v = -3; v < 45; v++) if (big(v) != (v >= 0 && v < 40 ? v * 3 + 1 : -1)) bad |= 1024;
    if (big(-2147483647 - 1) != -1 || big(2147483647) != -1) bad |= 1024;
    if (minmax(-2147483647 - 1) != 1 || minmax(-2147483647) != 2 || minmax(-2147483646) != 3 ||
        minmax(-2147483645) != 0 || minmax(-2147483644) != 4 || minmax(-2147483643) != 0 ||
        minmax(0) != 0 || minmax(2147483647) != 0 || minmax(-1) != 0) bad |= 2048;
    if (bad) return bad;
    return 42;
}
