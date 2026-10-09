// expect-exit: 42
/* A struct, union or enum tag declared in a block belongs to that block
 * (C11 6.2.1p4, 6.7.2.3p4). Two functions may each define the same tag --
 * CMSIS's cmsis_gcc.h defines `union llreg_u` inside every __SMLALD-style
 * intrinsic, and EmbCC's one flat tag namespace called the second a
 * redefinition -- and an inner block's definition hides an outer one until
 * the block closes. */
struct s { int a; int b; };

static unsigned lo_word(unsigned long long v)
{
    union llreg_u { unsigned w32[2]; unsigned long long w64; } u;
    u.w64 = v;
    return u.w32[0];
}

static unsigned hi_word(unsigned long long v)
{
    union llreg_u { unsigned w32[2]; unsigned long long w64; } u;
    u.w64 = v;
    return u.w32[1];
}

static int shadow(void)
{
    struct s outer = { 1, 2 };
    int r;
    {
        struct s { char c[5]; } inner;          /* a new type, here only */
        r = (int)sizeof inner;                  /* 5 */
        inner.c[0] = 3;
        r += inner.c[0];                        /* 8 */
    }
    {
        enum s { S_A = 10, S_B } e = S_B;       /* the tag again, as an enum */
        r += (int)e;                            /* 19 */
    }
    struct s again = { 4, 5 };                  /* the file-scope one again */
    return r + outer.a + again.b + (int)(sizeof again / sizeof(int));  /* 27 */
}

int main(void)
{
    unsigned long long v = 0x0000000500000007ull;
    int r = (int)(lo_word(v) + hi_word(v));     /* 12 */
    r += ({ struct t { int x; } q = { 3 }; q.x; });   /* 15 */
    r += ({ struct t { int x, y; } q = { 0, 0 }; (int)(sizeof q / sizeof(int)); });  /* 17 */
    if (r != 17)
        return 1;
    if (shadow() != 27)
        return 2;
    return r + 25;
}
