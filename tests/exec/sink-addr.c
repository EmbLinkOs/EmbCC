/* An address computed some instructions before the access it feeds --
 * `stack[sp++] = prog[pc++]` computes the store address, loads the
 * value, then stores -- is moved down next to its access so the backend
 * can fold it into the access's addressing mode. These are the shapes
 * where the move must keep the old operand: post-increments of the index
 * between the address and the access, a call and a volatile access in
 * between, an index rewritten after the address is formed, 2-D and
 * widened indexes, and loads and stores of every width. Expectations are
 * closed forms or fixed tables. */
// expect-exit: 42
#define NI __attribute__((noinline))
static int g;
NI int bump(int x) { g += x; return x + 1; }
static volatile int vol;

NI unsigned run(const int *prog, int n)        /* a tiny stack machine */
{
    unsigned st[32]; int sp = 0, pc = 0;
    while (pc < n) {
        int op = prog[pc++];
        if (op == 0) st[sp++] = (unsigned)prog[pc++];          /* push imm */
        else if (op == 1) { sp--; st[sp - 1] += st[sp]; }       /* add */
        else if (op == 2) { st[sp] = st[sp - 1]; sp++; }        /* dup */
        else if (op == 3) { sp--; st[sp - 1] *= st[sp]; }       /* mul */
    }
    return st[sp - 1];
}
NI long through_call(long *a, int i)
{
    long *p = &a[i];                 /* the address... */
    int j = bump(i);                 /* ...a call in between... */
    *p = j;                          /* ...then the store */
    return a[i] + a[j];
}
NI int through_volatile(int *a, int i)
{
    int *p = &a[i + 1];
    vol = i;
    int v = vol;
    *p = v * 3;
    return a[i + 1];
}
NI int index_moves(int *a, int i)
{
    int *p = &a[i];
    i = i * 2 + 1;                   /* the index changes after the address */
    *p = i;
    return a[(i - 1) / 2] + i;
}
static short grid[6][7];
NI int two_d(int r, int c, short v)
{
    short old = grid[r][c];
    int k = r + c;
    grid[r][c] = (short)(v + k);
    return old + grid[r][c];
}
NI long long wide_idx(long long *a, unsigned char k, long long v)
{
    long long *p = &a[k];
    long long w = v * 3;
    *p = w;
    return a[k] - v;
}
NI unsigned char bytes(unsigned char *b, int i)
{
    unsigned char *p = &b[i * 3];
    unsigned char x = b[i];
    *p = (unsigned char)(x + 1);
    return b[i * 3];
}
int main(void)
{
    /* (2 + 3) * (2 + 3) + 7 = 32 */
    static const int prog[] = { 0, 2, 0, 3, 1, 2, 3, 0, 7, 1 };
    if (run(prog, 10) != 32) return 1;
    long a[8] = { 0 };
    g = 0;
    if (through_call(a, 2) != 3 + 0 || a[2] != 3 || g != 2) return 2;
    int b[8] = { 0 };
    if (through_volatile(b, 3) != 9 || b[4] != 9) return 3;
    int c[8] = { 0 };
    /* i = 2: p = &c[2]; i = 5; c[2] = 5; returns c[2] + 5 = 10 */
    if (index_moves(c, 2) != 10 || c[2] != 5) return 4;
    grid[2][3] = 100;
    if (two_d(2, 3, 7) != 100 + 12 || grid[2][3] != 12) return 5;
    long long ll[300] = { 0 };
    if (wide_idx(ll, 250, 5) != 10 || ll[250] != 15) return 6;
    unsigned char by[30];
    for (int i = 0; i < 30; i++) by[i] = (unsigned char)(i * 7);
    if (bytes(by, 4) != (unsigned char)(28 + 1) || by[12] != 29) return 7;
    return 42;
}
