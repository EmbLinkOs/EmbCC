/* `long long` bit-fields wider than a word, and ones in a long long's
 * second word, read and written. On an ILP32 target `long long` and
 * `long` share one type kind, and the storage unit of a `long long : 33`
 * was taken as `unsigned long` -- four bytes -- so a field in the unit's
 * second word read the word past it and a store wrote half the unit. */
// expect-exit: 42
struct ll2 { long long a : 33; long long b : 31; };
struct ll3 { unsigned long long lo : 20, mid : 30, hi : 14; };
static struct ll2 s2 = { -0x123456789LL, 0x2A2A2A2A };
static volatile long long vneg = -5, vbig = 0x1F0F0F0F0LL;

int main(void)
{
    if ((unsigned long long)s2.a != 0xDCBA9877ULL) return 1;
    if (s2.b != 0x2A2A2A2A) return 2;
    struct ll2 t = { 0, 0 };
    t.a = vneg;
    t.b = -3;
    if (t.a != -5 || t.b != -3) return 3;
    t.a = vbig;                         /* 0x1F0F0F0F0: bit 32 set, negative */
    if (t.a != 0x1F0F0F0F0LL - 0x200000000LL) return 4;
    if (t.b != -3) return 5;
    struct ll3 u = { 0xABCDE, 0x2BCDEF01, 0x3ABC };
    if (u.lo != 0xABCDE || u.mid != 0x2BCDEF01 || u.hi != 0x3ABC) return 6;
    u.mid += 1;
    if (u.lo != 0xABCDE || u.mid != 0x2BCDEF02 || u.hi != 0x3ABC) return 7;
    if (sizeof(struct ll2) != 8 || sizeof(struct ll3) != 8) return 8;
    return 42;
}
