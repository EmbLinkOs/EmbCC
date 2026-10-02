/* Variable-length arrays on the embedded targets, run and compared with
 * the host (tests/golden/vla-exec.sh). A VLA moves the stack pointer at
 * run time, so the frame is addressed from a frame-base register (r7 on
 * Thumb, s0 on RISC-V) and only outgoing call arguments stay at the live
 * sp. Each function below leans on one piece of that. */
void writec(int c);
void puts_(const char *s);
typedef unsigned u32;
static void hx(u32 v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}

/* the basic case, and sizeof of a VLA */
__attribute__((noinline)) u32 squares(int n)
{
    int a[n];
    for (int i = 0; i < n; i++) a[i] = i * i;
    u32 s = (u32)sizeof a;
    for (int i = 0; i < n; i++) s += (u32)a[i];
    return s;
}
/* a VLA in a loop body: released every iteration, or 2000 iterations of
 * 400 bytes run the stack out */
__attribute__((noinline)) u32 in_loop(int iters, int n)
{
    u32 h = 0;
    for (int k = 0; k < iters; k++) {
        unsigned char b[n + (k & 3)];
        for (int i = 0; i < (int)sizeof b; i++) b[i] = (unsigned char)(i ^ k);
        h = h * 31 + b[sizeof b - 1] + (u32)sizeof b;
    }
    return h;
}
/* a call with STACK arguments while a VLA exists: they must land at the
 * live sp, below the array, where the callee looks */
__attribute__((noinline)) u32 many(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f,
                                   u32 g, u32 h, u32 i, u32 j, u32 k)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h +
           9 * i + 10 * j + 11 * k;
}
__attribute__((noinline)) u32 call_under(int n)
{
    u32 v[n];
    for (int i = 0; i < n; i++) v[i] = (u32)i * 7u + 1u;
    u32 r = many(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8],
                 v[9], v[n - 1]);
    return r + v[n / 2];
}
/* rows of a 2-D VLA, stepped by a pointer to the row type: the stride is
 * the row's run-time size */
__attribute__((noinline)) u32 rows(int nr, int nc)
{
    unsigned char m[nr][nc];
    for (int r = 0; r < nr; r++)
        for (int c = 0; c < nc; c++)
            m[r][c] = (unsigned char)(r * 17 + c);
    u32 h = 0;
    unsigned char (*row)[nc] = m;
    for (int r = 0; r < nr; r++) { h = h * 5 + (*row)[nc - 1]; row += 1; }
    row -= 2;
    return h + (*row)[0] + (u32)sizeof m + (u32)sizeof *row;
}
/* recursion, each level with its own VLA */
__attribute__((noinline)) u32 rec(int depth, int n)
{
    int a[n];
    for (int i = 0; i < n; i++) a[i] = depth * 100 + i;
    u32 below = depth ? rec(depth - 1, n + 1) : 0;
    return below * 3 + (u32)a[n - 1] + (u32)a[0];
}
/* a big fixed frame AND a VLA: slots past every short offset, addressed
 * from the frame base */
__attribute__((noinline)) u32 bigframe(int n)
{
    volatile u32 fixed[400];
    u32 v[n];
    for (int i = 0; i < 400; i++) fixed[i] = (u32)i * 3u;
    for (int i = 0; i < n; i++) v[i] = fixed[i * 7 % 400] + (u32)i;
    return v[n - 1] + fixed[399] + v[0];
}

int main(void)
{
    hx(squares(1)); hx(squares(37));
    hx(in_loop(2000, 100));
    hx(call_under(16));
    hx(rows(9, 13)); hx(rows(3, 1));
    hx(rec(6, 5));
    hx(bigframe(50));
    puts_("\n==END==\n");
    return 0;
}
