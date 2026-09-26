/* The ARMv7-M exercise program (D-015): everything the backend claims to
 * lower, printed, so that what it computes can be compared against what
 * clang computes for the same source on the same emulated board.
 *
 * Output goes through tests/harness/thumb/io.c — the lm3s6965's UART —
 * and the run ends with a sentinel, because a bare-metal image has no
 * way to exit and the harness judges it by what it printed. */
extern void writec(int c);
extern void puts_(const char *s);
extern void putn(long v);
static void nl(void) { writec('\n'); }

struct pt { int x, y; short tag; unsigned char f; };
static struct pt pts[4];
static int grid[3][4];
static const char *names[3] = { "alpha", "be", "gamma!" };
static int counter;

static int addup(const struct pt *p, int n)
{
    int t = 0;
    for (int i = 0; i < n; i++) t += p[i].x * 10 + p[i].y + p[i].tag + p[i].f;
    return t;
}

static int classify(int v)
{
    switch (v) {
    case 0: return 100;
    case 1: case 2: return 200;
    case 7: return 300;
    default: return v < 0 ? -1 : v * 2;
    }
}

static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }
static int fib(int n) { return n < 2 ? n : fib(n-1) + fib(n-2); }
static int bits(unsigned v) { int c = 0; while (v) { c += (int)(v & 1u); v >>= 1; } return c; }
static int (*fp)(int);

static int slen(const char *s) { const char *p = s; while (*p) p++; return (int)(p - s); }

int main(void)
{
    for (int i = 0; i < 4; i++) {
        pts[i].x = i; pts[i].y = 10 - i;
        pts[i].tag = (short)(i * 1000 - 1500);
        pts[i].f = (unsigned char)(200 + i * 30);
    }
    putn(addup(pts, 4)); nl();

    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            grid[r][c] = r * 4 + c;
    { int t = 0; for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) t += grid[r][c] * (c & 1 ? -1 : 1);
      putn(t); nl(); }

    for (int v = -2; v <= 8; v++) putn(classify(v));
    nl();

    putn(gcd(1071, 462)); putn(gcd(17, 5)); putn(fib(15)); nl();
    putn(bits(0xdeadbeefu)); putn(bits(0)); putn(bits(0xffffffffu)); nl();

    for (int i = 0; i < 3; i++) putn(slen(names[i]));
    nl();

    fp = classify;
    putn(fp(7)); putn(fp(3)); nl();

    { int i = 0, s = 0;
      do { if (i == 3) { i++; continue; } if (i == 9) break; s += i; i++; } while (i < 20);
      putn(s); putn(i); nl(); }

    { unsigned u = 0xF0F0F0F0u; int sh = 0; long acc = 0;
      for (sh = 0; sh < 32; sh += 5) acc += (long)(int)(u >> sh) + (long)(int)((int)u >> sh);
      putn(acc); nl(); }

    { int a = -7, b = 3;
      putn(a / b); putn(a % b); putn((-a) / b); putn((-a) % b);
      putn(a * b); putn(a << 2); putn(a >> 2); nl();
      unsigned ua = 0xfffffff9u;
      putn((long)(ua / 3u)); putn((long)(ua % 3u)); putn((long)(ua >> 3)); nl(); }

    { counter = 0;
      for (int i = 0; i < 100; i++) if (i % 7 == 0 || i % 11 == 0) counter += i;
      putn(counter); nl(); }

    { char buf[24]; int n = 0;
      for (const char *s = names[2]; *s; s++) buf[n++] = (char)(*s - 32);
      buf[n] = 0;
      for (int i = 0; i < n; i++) writec(buf[i]);
      nl(); }
    /* Bitfields, which are how a peripheral's registers are written —
     * and which ride on the 64-bit lowering, since the front end
     * assembles a field's storage unit in a 64-bit accumulator. */
    { struct ctl { unsigned en:1, mode:3, prio:4, chan:5, rsv:19; } c;
      struct wide { unsigned a:12, b:12, e:8; } w;
      c.en = 1; c.mode = 5; c.prio = 9; c.chan = 27; c.rsv = 0;
      putn(c.en); putn(c.mode); putn(c.prio); putn(c.chan);
      w.a = 4000; w.b = 3000; w.e = 200;
      putn(w.a); putn(w.b); putn(w.e); nl(); }

    puts_("==END==\n");
    return 0;
}
