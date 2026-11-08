/* A member read straight out of a struct a call returns -- an RTOS's
 * `emb_time_now().ticks` -- or out of a compound literal. The value sits
 * in a slot of the caller's own, aligned as its type, so the read is a
 * plain aligned load; a strict-alignment target (Xtensa, ARMv6-M, MIPS)
 * read it a byte at a time, eight loads and seven shifts for a 64-bit
 * member. A packed struct's member stays a byte-wise read, and those
 * must still give the right values. */
// expect-exit: 42
typedef unsigned long long u64;
typedef struct { u64 ticks; } tm;
typedef struct { char c; short h; int w; u64 d; } mixed;
typedef struct __attribute__((packed)) { char c; int w; u64 d; } packed;

__attribute__((noinline)) static tm now(u64 base) { tm t = { base + 0x100000000ull }; return t; }
__attribute__((noinline)) static mixed mk(int k)
{
    mixed m = { (char)k, (short)(k * 3), k * 1000, (u64)k << 40 };
    return m;
}
__attribute__((noinline)) static packed pk(int k)
{
    packed p = { (char)k, k * 7, (u64)k << 33 | 5 };
    return p;
}

int main(void)
{
    volatile u64 b = 7;
    volatile int k = 3;
    if (now(b).ticks != 0x100000007ull) return 1;
    if (mk(k).c != 3 || mk(k).h != 9 || mk(k).w != 3000) return 2;
    if (mk(k).d != (u64)3 << 40) return 3;
    if (pk(k).c != 3 || pk(k).w != 21 || pk(k).d != ((u64)3 << 33 | 5)) return 4;
    if (((mixed){ 1, 2, (int)k, b }).d != 7 || ((mixed){ 1, 2, (int)k, b }).w != 3)
        return 5;
    return 42;
}
