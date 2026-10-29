/* A volatile read wider than what is used of it: every byte is read, as
 * C says it is accessed, and the bytes nothing uses go nowhere. On AVR
 * they went into the registers after the destination's -- r28:r29, the
 * frame pointer, after a pair in X -- and the program ran off into the
 * weeds (found by verify/fuzz/run-avr.sh): a four-byte global read for
 * its low two, a four-byte local likewise, a two-byte global read for its
 * low one, each with locals in the frame live across it. */
// expect-exit: 42

typedef unsigned long u32;
typedef unsigned short u16;
typedef unsigned char u8;

static volatile u32 vz = 0;
static volatile u32 g32 = 0x12345678UL;
static volatile u16 g16 = 0xabcd;
static short gv;

__attribute__((noinline)) static int frame_user(int k)
{
    volatile char buf[6];           /* a frame, read through Y */
    for (int j = 0; j < 6; j++)
        buf[j] = (char)(j + k);
    gv = (short)(-22585 + vz);      /* two bytes of a four-byte volatile */
    u16 lo = (u16)g32;
    u8 b = (u8)g16;                 /* one of two */
    int s = 0;
    for (int j = 0; j < 6; j++)
        s += buf[j];
    return s + (lo == 0x5678) + (b == 0xcd) + (gv == -22585);
}

int main(void)
{
    volatile u32 v = 0x12345678UL;
    u16 s = 0;
    for (int k = 0; k < 4; k++) {
        u16 lo = (u16)v;            /* two bytes of a four-byte volatile local */
        s = (u16)(s + lo + (unsigned)k);
        v = v + 0x10001UL;
    }
    if (s != 23020)
        return 1;
    if (frame_user(3) != 6 * 3 + 15 + 3)
        return 2;
    return 42;
}
