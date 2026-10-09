/* `x[i - k]` for several k with one i: the constants come out of the
 * index (pass_idxoff) and the accesses share `x + i`, each with a
 * negative displacement -- on ARMv7-M the `ldr rt, [rn, #-imm8]` form,
 * which reaches back 255 bytes and no further. Loads and stores of every
 * width, signed and unsigned, an unrolled filter, and displacements on
 * both sides of -255. Exit 0, or the number of the first wrong result. */
// expect-exit: 0

typedef __INT32_TYPE__ i32;
typedef __UINT32_TYPE__ u32;

static volatile i32 vk = 3;
static signed char sb[600];
static unsigned char ub[600];
static short sh[600];
static unsigned short uh[600];
static i32 w[600];

__attribute__((noinline)) static i32 taps_s16(const short *x, const short *h, i32 i)
{
    i32 acc = 0;
    for (i32 k = 0; k < 16; k++)
        acc += x[i - k] * h[k];
    return acc;
}

__attribute__((noinline)) static i32 bytes_back(i32 i)
{
    /* -1, -2, -100, -255 reach; -256 and -300 do not */
    return sb[i - 1] + 2 * sb[i - 2] + 3 * sb[i - 100] + 5 * sb[i - 255] +
           7 * sb[i - 256] + 11 * sb[i - 300] + ub[i - 1] + ub[i - 255] +
           ub[i - 256];
}

__attribute__((noinline)) static i32 halves_back(i32 i)
{
    /* 2-byte elements: -127 is -254 bytes, -128 is -256 */
    return sh[i - 1] + 2 * sh[i - 3] + 3 * sh[i - 127] + 5 * sh[i - 128] +
           uh[i - 2] + 7 * uh[i - 127] + 11 * uh[i - 129];
}

__attribute__((noinline)) static i32 words_back(i32 i)
{
    /* 4-byte elements: -63 is -252 bytes, -64 is -256 */
    return w[i - 1] - w[i - 2] + 3 * w[i - 63] + 5 * w[i - 64] + 7 * w[i - 70];
}

__attribute__((noinline)) static void store_back(i32 i, i32 v)
{
    w[i - 1] = v;
    w[i - 2] = v + 1;
    w[i - 63] = v + 2;
    w[i - 64] = v + 3;
    sh[i - 1] = (short)(v + 4);
    sh[i - 127] = (short)(v + 5);
    sh[i - 128] = (short)(v + 6);
    sb[i - 1] = (signed char)(v + 7);
    sb[i - 255] = (signed char)(v + 8);
    sb[i - 256] = (signed char)(v + 9);
    ub[i - 3] = (unsigned char)(v + 10);
}

static int nth, first_bad;

static void check(i32 got, i32 want)
{
    nth++;
    if (got != want && !first_bad)
        first_bad = nth;
}

int main(void)
{
    i32 k = vk;
    for (i32 n = 0; n < 600; n++) {
        sb[n] = (signed char)(n * 7 - 300);
        ub[n] = (unsigned char)(n * 11 + 3);
        sh[n] = (short)(n * 97 - 20000);
        uh[n] = (unsigned short)(n * 131 + 7);
        w[n] = n * 100003 - 7;
    }
    short h[16];
    for (i32 n = 0; n < 16; n++)
        h[n] = (short)(n * 1234 % 8000 - 4000);
    for (i32 i = 15; i < 40; i += k) {
        i32 want = 0;
        for (i32 j = 0; j < 16; j++)
            want += sh[i - j] * h[j];
        check(taps_s16(sh, h, i), want);
    }
    i32 i = 400 + k;
    check(bytes_back(i), sb[i - 1] + 2 * sb[i - 2] + 3 * sb[i - 100] + 5 * sb[i - 255] +
                         7 * sb[i - 256] + 11 * sb[i - 300] + ub[i - 1] + ub[i - 255] +
                         ub[i - 256]);
    check(halves_back(i), sh[i - 1] + 2 * sh[i - 3] + 3 * sh[i - 127] + 5 * sh[i - 128] +
                          uh[i - 2] + 7 * uh[i - 127] + 11 * uh[i - 129]);
    check(words_back(i), w[i - 1] - w[i - 2] + 3 * w[i - 63] + 5 * w[i - 64] + 7 * w[i - 70]);
    store_back(i, 1000 + k);
    check(w[i - 1], 1000 + k);
    check(w[i - 2], 1001 + k);
    check(w[i - 63], 1002 + k);
    check(w[i - 64], 1003 + k);
    check(sh[i - 1], (short)(1004 + k));
    check(sh[i - 127], (short)(1005 + k));
    check(sh[i - 128], (short)(1006 + k));
    check(sb[i - 1], (signed char)(1007 + k));
    check(sb[i - 255], (signed char)(1008 + k));
    check(sb[i - 256], (signed char)(1009 + k));
    check(ub[i - 3], (unsigned char)(1010 + k));
    check(w[i - 3], (i - 3) * 100003 - 7);
    return first_bad;
}
