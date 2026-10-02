/* A select tests its condition at the CONDITION's width, which is not the
 * width of the values it chooses between. if-convert turns
 *     if (c) r = a; else r = b;
 * into one select, and the branch it replaced knew how wide `c` was; the
 * select tested the arms' width instead. A 64-bit condition of 2^32 then
 * read as false on x86-64, aarch64 and ARMv7-M (tested at 32 bits), and a
 * 32-bit condition narrowed from 2^32 read as true on RV64 (tested at 64). */
// expect-exit: 42
static volatile unsigned long long pow32 = 0x100000000ULL;

__attribute__((noinline)) static int
pick64(unsigned long long c, int a, int b)
{ int r; if (c) r = a; else r = b; return r; }

__attribute__((noinline)) static long long
pick64w(unsigned long long c, long long a, long long b)
{ long long r; if (c) r = a; else r = b; return r; }

__attribute__((noinline)) static int
pick32(unsigned long long c, int a, int b)
{ int r; if ((unsigned)c) r = a; else r = b; return r; }

__attribute__((noinline)) static long long
pick32w(unsigned long long c, long long a, long long b)
{ int t = (int)c; long long r; if (t) r = a; else r = b; return r; }

__attribute__((noinline)) static void *
pickp(void *c, void *a, void *b)
{ void *r; if (c) r = a; else r = b; return r; }

int main(void)
{
    int x, y;
    if (pick64(pow32, 1, 2) != 1) return 1;
    if (pick64w(pow32, 3, 4) != 3) return 2;
    if (pick32(pow32, 5, 6) != 6) return 3;
    if (pick32w(pow32, 7, 8) != 8) return 4;
    if (pick64(pow32 + 1, 1, 2) != 1 || pick32(pow32 + 1, 5, 6) != 5)
        return 5;
    if (pick64(0, 1, 2) != 2 || pick32(0, 5, 6) != 6) return 6;
    if (sizeof(void *) == 8 &&
        pickp((void *)(unsigned long)pow32, &x, &y) != &x) return 7;
    return 42;
}
