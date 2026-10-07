/* Jump tables on MIPS32 (tests/golden/mips-switch.sh). leaf() calls
 * nothing, so its $ra is live across the table's `bal` and must come
 * back; nonleaf() calls in its cases; both take values below, inside and
 * above the range, and the holes go to the default. */
void putn(long v);

__attribute__((noinline)) int twice(int x) { return 2 * x; }

__attribute__((noinline)) int leaf(int v)
{
    switch (v) {
    case 10: return 1;
    case 11: return 2;
    case 12: return 3;
    case 14: return 5;
    case 15: return 6;
    case 16: return 7;
    case 17: return 8;
    default: return 0;
    }
}

__attribute__((noinline)) int nonleaf(int v)
{
    switch (v) {
    case -3: return twice(1);
    case -2: return twice(2);
    case -1: return twice(3);
    case 0:  return twice(4);
    case 1:  return twice(5);
    case 3:  return twice(7);
    case 4:  return twice(8);
    default: return twice(100);
    }
}

int main(void)
{
    long s = 0;
    for (int v = 5; v <= 20; v++)           /* 0 0 0 0 0 1 2 3 0 5 6 7 8 0 0 0 */
        s = s * 3 + leaf(v);
    putn(s);
    for (int v = -5; v <= 6; v++)
        putn(nonleaf(v));
    return 42;
}
