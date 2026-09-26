/* Indexed access to floating-point memory, called OUT OF LINE.
 *
 * `double *p; p[i]` is the most ordinary indexed float access there is,
 * and on x86-64 at -O2 it silently read byte i instead of byte 8*i. The
 * address-generation fusion emits `shl idx, #3` as nothing, on the
 * promise that the `add` two instructions later folds it into a SIB
 * scale; the add then refused to fold a float-class load, and the shift
 * was simply gone. Both sites now ask one predicate (addr_fold_ok).
 *
 * The bug hid because the caller normally INLINES the accessor and the
 * inlined copy takes a different path -- tests/exec/arrays.c and every
 * float test in the tree passed throughout. So each accessor here is
 * reached through a volatile function pointer, which no inliner may see
 * through, and the indices are not constants.
 */
// expect-exit: 42

static double  ld(double *p, int i)            { return p[i]; }
static void    sd(double *p, int i, double v)  { p[i] = v; }
static float   lf(float *p, int i)             { return p[i]; }
static void    sf(float *p, int i, float v)    { p[i] = v; }
/* The same shape with an integer element, which never lost its shift --
 * it is here so a change that breaks the fusion outright fails on both
 * classes and cannot be mistaken for a float-only problem. */
static long    li(long *p, int i)              { return p[i]; }
static void    si(long *p, int i, long v)      { p[i] = v; }

static double (*volatile p_ld)(double *, int)         = ld;
static void   (*volatile p_sd)(double *, int, double) = sd;
static float  (*volatile p_lf)(float *, int)          = lf;
static void   (*volatile p_sf)(float *, int, float)   = sf;
static long   (*volatile p_li)(long *, int)           = li;
static void   (*volatile p_si)(long *, int, long)     = si;

int main(void)
{
    double d[8];
    float  f[8];
    long   n[8];
    volatile int k;

    for (k = 0; k < 8; k++) {
        p_sd(d, k, (double)k * 0.5 + 1.0);
        p_sf(f, k, (float)k * 0.25f + 2.0f);
        p_si(n, k, (long)k * 7 + 3);
    }
    for (k = 0; k < 8; k++) {
        if (p_ld(d, k) != (double)k * 0.5 + 1.0)  return 1;
        if (p_lf(f, k) != (float)k * 0.25f + 2.0f) return 2;
        if (p_li(n, k) != (long)k * 7 + 3)        return 3;
    }
    /* Reading an element the loop did not just write catches a store
     * that landed at the wrong address but was read back the same wrong
     * way -- two halves of one bug agreeing with each other. */
    if (d[0] != 1.0 || d[7] != 4.5)   return 4;
    if (f[0] != 2.0f || f[7] != 3.75f) return 5;
    if (n[0] != 3 || n[7] != 52)      return 6;
    return 42;
}
