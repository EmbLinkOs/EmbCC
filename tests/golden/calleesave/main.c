void writec(int c);
void puts_(const char *s);
typedef unsigned u32;
typedef u32 (*fn_t)(u32, u32, u32, u32);
u32 probe(fn_t f, u32 a, u32 *result);
u32 leaf(u32, u32, u32, u32);
u32 swapper(u32, u32, u32, u32);
u32 wide(u32, u32, u32, u32);
u32 bigframe(u32, u32, u32, u32);
u32 across(u32, u32, u32, u32);
u32 sw(u32, u32, u32, u32);
u32 divs(u32, u32, u32, u32);
u32 flt(u32, u32, u32, u32);
u32 vla(u32, u32, u32, u32);
u32 lowscr(u32, u32, u32, u32);
static void hx(u32 v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}
static fn_t fns[] = { leaf, swapper, wide, bigframe, across, sw, divs, flt, vla,
                      lowscr };
int main(void)
{
    for (unsigned k = 0; k < sizeof fns / sizeof fns[0]; k++)
        for (u32 a = 1; a < 9; a += 3) {
            u32 r = 0, bad = probe(fns[k], a * 123457u, &r);
            hx(r);
            if (bad) { puts_("CLOBBERED "); hx(k); hx(bad); }
        }
    puts_("\n==END==\n");
    return 0;
}
