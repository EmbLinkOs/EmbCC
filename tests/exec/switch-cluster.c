/* Switches that are dense but for a few cases (-Os on ARM: a table over
 * the dense run, an equality test for each case outside it -- irgen's
 * switch_cluster), checked against the same mapping written as a chain of
 * ifs, for every value in and around each switch's span: the outliers,
 * both ends of the run, the holes inside it, the default on either side,
 * and the values that wrap when the run's low end is subtracted. */
// expect-exit: 42
__attribute__((noinline)) int conv(int c)
{
    switch (c) {
    case 0:   return 1;
    case '%': return 2;
    case 'a': return 3;  case 'b': return 4;  case 'c': return 5;
    case 'd': return 6;  case 'e': return 7;  case 'f': return 8;
    case 'g': return 9;  case 'i': return 10; case 'n': return 11;
    case 'o': return 12; case 'p': return 13; case 's': return 14;
    case 'u': return 15; case 'x': return 16;
    case 'A': return 17; case 'E': return 18; case 'X': return 19;
    default:  return -1;
    }
}

static int conv_ref(int c)
{
    static const char k[] = "\0%abcdefginopsuxAEX";
    for (int i = 0; i < 19; i++)
        if (c == k[i])
            return i + 1;
    return -1;
}

__attribute__((noinline)) int neg(int x)
{
    switch (x) {
    case -1000: return 50;
    case -3: return 51; case -2: return 52; case -1: return 53;
    case 0: return 54;  case 1: return 55;  case 2: return 56;
    case 4: return 57;  case 5: return 58;
    case 70000: return 59;
    default: return 0;
    }
}

static int neg_ref(int x)
{
    static const int k[] = { -1000, -3, -2, -1, 0, 1, 2, 4, 5, 70000 };
    for (int i = 0; i < 10; i++)
        if (x == k[i])
            return 50 + i;
    return 0;
}

__attribute__((noinline)) unsigned uns(unsigned x)
{
    switch (x) {
    case 0xffffffffu: return 9;
    case 10: return 1; case 11: return 2; case 12: return 3;
    case 14: return 4; case 15: return 5; case 16: return 6;
    case 0x80000000u: return 7;
    default: return 0;
    }
}

static unsigned uns_ref(unsigned x)
{
    static const unsigned k[] = { 0xffffffffu, 10, 11, 12, 14, 15, 16,
                                  0x80000000u };
    static const unsigned r[] = { 9, 1, 2, 3, 4, 5, 6, 7 };
    for (int i = 0; i < 8; i++)
        if (x == k[i])
            return r[i];
    return 0;
}

/* A dense switch whose first case's body is longer than a byte table's
 * reach (510 bytes on Thumb-2): the later cases need the halfword form. */
static volatile int vv;
__attribute__((noinline)) int far(int x)
{
    switch (x) {
    case 0:
        vv = vv * 3 + 0;
        vv = vv * 3 + 1;
        vv = vv * 3 + 2;
        vv = vv * 3 + 3;
        vv = vv * 3 + 4;
        vv = vv * 3 + 5;
        vv = vv * 3 + 6;
        vv = vv * 3 + 7;
        vv = vv * 3 + 8;
        vv = vv * 3 + 9;
        vv = vv * 3 + 10;
        vv = vv * 3 + 11;
        vv = vv * 3 + 12;
        vv = vv * 3 + 13;
        vv = vv * 3 + 14;
        vv = vv * 3 + 15;
        vv = vv * 3 + 16;
        vv = vv * 3 + 17;
        vv = vv * 3 + 18;
        vv = vv * 3 + 19;
        vv = vv * 3 + 20;
        vv = vv * 3 + 21;
        vv = vv * 3 + 22;
        vv = vv * 3 + 23;
        vv = vv * 3 + 24;
        vv = vv * 3 + 25;
        vv = vv * 3 + 26;
        vv = vv * 3 + 27;
        vv = vv * 3 + 28;
        vv = vv * 3 + 29;
        vv = vv * 3 + 30;
        vv = vv * 3 + 31;
        vv = vv * 3 + 32;
        vv = vv * 3 + 33;
        vv = vv * 3 + 34;
        vv = vv * 3 + 35;
        vv = vv * 3 + 36;
        vv = vv * 3 + 37;
        vv = vv * 3 + 38;
        vv = vv * 3 + 39;
        vv = vv * 3 + 40;
        vv = vv * 3 + 41;
        vv = vv * 3 + 42;
        vv = vv * 3 + 43;
        vv = vv * 3 + 44;
        vv = vv * 3 + 45;
        vv = vv * 3 + 46;
        vv = vv * 3 + 47;
        vv = vv * 3 + 48;
        vv = vv * 3 + 49;
        vv = vv * 3 + 50;
        vv = vv * 3 + 51;
        vv = vv * 3 + 52;
        vv = vv * 3 + 53;
        vv = vv * 3 + 54;
        vv = vv * 3 + 55;
        vv = vv * 3 + 56;
        vv = vv * 3 + 57;
        vv = vv * 3 + 58;
        vv = vv * 3 + 59;
        vv = vv * 3 + 60;
        vv = vv * 3 + 61;
        vv = vv * 3 + 62;
        vv = vv * 3 + 63;
        vv = vv * 3 + 64;
        vv = vv * 3 + 65;
        vv = vv * 3 + 66;
        vv = vv * 3 + 67;
        vv = vv * 3 + 68;
        vv = vv * 3 + 69;
        vv = vv * 3 + 70;
        vv = vv * 3 + 71;
        vv = vv * 3 + 72;
        vv = vv * 3 + 73;
        vv = vv * 3 + 74;
        vv = vv * 3 + 75;
        vv = vv * 3 + 76;
        vv = vv * 3 + 77;
        vv = vv * 3 + 78;
        vv = vv * 3 + 79;
        vv = vv * 3 + 80;
        vv = vv * 3 + 81;
        vv = vv * 3 + 82;
        vv = vv * 3 + 83;
        vv = vv * 3 + 84;
        vv = vv * 3 + 85;
        vv = vv * 3 + 86;
        vv = vv * 3 + 87;
        vv = vv * 3 + 88;
        vv = vv * 3 + 89;
        return vv & 0xff;
    case 1: return 101;
    case 2: return 102;
    case 3: return 103;
    case 4: return 104;
    case 5: return 105;
    default: return -1;
    }
}

int main(void)
{
    int bad = 0;
    for (int c = -300; c <= 300; c++)
        if (conv(c) != conv_ref(c)) bad |= 1;
    for (int x = -1100; x <= 1100; x++)
        if (neg(x) != neg_ref(x)) bad |= 2;
    for (int x = 69990; x <= 70010; x++)
        if (neg(x) != neg_ref(x)) bad |= 2;
    {
        int far[] = { -2147483647 - 1, 2147483647, -1001, -999, 69999, 70001 };
        for (int k = 0; k < 6; k++)
            if (neg(far[k]) != neg_ref(far[k])) bad |= 4;
    }
    for (unsigned x = 0; x <= 40; x++)
        if (uns(x) != uns_ref(x)) bad |= 8;
    {
        unsigned far[] = { 0xffffffffu, 0xfffffffeu, 0x80000000u,
                           0x7fffffffu, 0x80000001u };
        for (int k = 0; k < 5; k++)
            if (uns(far[k]) != uns_ref(far[k])) bad |= 16;
    }
    for (int x = -2; x <= 7; x++) {
        int want = x >= 1 && x <= 5 ? 100 + x : x == 0 ? -2 : -1;
        int got = far(x);
        if (want == -2 ? got < 0 || got > 255 : got != want) bad |= 32;
    }
    return bad ? bad : 42;
}
