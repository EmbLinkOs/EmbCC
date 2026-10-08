/* ColdFire assembly on the board (tests/golden/coldfire-asm.sh): inline asm
 * of every operand kind, %sr, %ccr, %usp and %vbr with a trap through the
 * vector table, numeric labels, values live across templates that clobber;
 * a file-scope asm function, a naked function; and forms.S, called from
 * here and calling back. It prints what it computed; model.c, compiled for
 * the HOST, prints what each should be, and the two lines must agree. */
void putn(long v);
void puts_(const char *s);

/* forms.S */
int asm_sum(const int *p, int n);
int asm_call_c(int x);
int asm_tail(int x);
int asm_relax(int x);
int asm_alu(int a, int b);
int asm_bits(unsigned char *p);
int asm_table(int k);
int asm_var_bump(void);
extern int asm_var;
int asm_div(int a, int b);
int asm_swap(int x);
int asm_link(int x);
int asm_ipl(void);
void asm_trash(void);
void asm_trash_saved(void);
void trap_handler(void);
extern int trap_count;

int c_helper(int x) { return x * 7 + 1; }
int c_helper2(int x) { return x * 3 - 2; }

/* ---- inline asm ---- */
static int add2(int a, int b)
{
    int r;
    __asm__("move.l %1,%0\n\tadd.l %2,%0" : "=&d"(r) : "d"(a), "d"(b));
    return r;
}

static int supervisor(void)
{
    int r;
    __asm__ volatile("move.w %%sr,%0" : "=d"(r));
    return (r >> 13) & 1;
}

static int ipl_raised(void)
{
    int old, r;
    __asm__ volatile("move.w %%sr,%0\n\tmove.w #0x2700,%%sr\n\t"
                     "move.w %%sr,%1\n\tmove.w %0,%%sr"
                     : "=&d"(old), "=&d"(r) :: "cc", "memory");
    return (r >> 8) & 7;
}

static int ccr_rt(int v)
{
    int r;
    __asm__ volatile("move.w %1,%%ccr\n\tmove.w %%ccr,%0" : "=d"(r) : "d"(v));
    return r & 0x1f;
}

static unsigned usp_rt(unsigned v)
{
    unsigned old, r;
    __asm__ volatile("move.l %%usp,%0\n\tmove.l %2,%%usp\n\t"
                     "move.l %%usp,%1\n\tmove.l %0,%%usp"
                     : "=&a"(old), "=&a"(r) : "a"(v));
    return r;
}

/* the vector table the harness pointed %vbr at; a trap through it */
#define VECTORS ((void (**)(void))0x40000000u)
static int trap_twice(void)
{
    VECTORS[32 + 3] = trap_handler;
    __asm__ volatile("movec %0,%%vbr" :: "a"(0x40000000u));
    __asm__ volatile("trap #3\n\ttrap #3" ::: "memory");
    return trap_count;
}

static int loop3(int n)
{
    int r = 0;
    __asm__("1:\taddq.l #3,%0\n\tsubq.l #1,%1\n\tbne 1b" : "+d"(r), "+d"(n));
    return r;
}

static int fwd(int x)
{
    __asm__("cmp.l #10,%0\n\tbge 2f\n\tadd.l #100,%0\n2:\taddq.l #1,%0"
            : "+d"(x) :: "cc");
    return x;
}

int gmem = 5;
static int mem_ops(int v)
{
    __asm__ volatile("move.l %1,%0" : "=m"(gmem) : "d"(v));
    __asm__ volatile("add.l %1,%0" : "+d"(v) : "m"(gmem));
    return v;
}

static int imm_ops(void)
{
    int r;
    __asm__("move.l %1,%0\n\tadd.l %2,%0" : "=d"(r) : "i"(1000), "n"(-7));
    return r;
}

/* values live across a template that names callee-saved registers, and
 * one that changes some without a clobber list */
static int live_across(int a, int b)
{
    int x = a * 3, y = b + 11, z = a ^ b, w = a - b;
    __asm__ volatile("moveq #0,%%d2\n\tmoveq #0,%%d3\n\tmoveq #0,%%d4\n\t"
                     "moveq #0,%%d5\n\tmoveq #0,%%d6\n\tmoveq #0,%%d7\n\t"
                     "move.l %%d2,%%a2\n\tmove.l %%d2,%%a3\n\t"
                     "move.l %%d2,%%a4\n\tmove.l %%d2,%%a5"
                     ::: "d2", "d3", "d4", "d5", "d6", "d7", "a2", "a3", "a4",
                         "a5");
    /* basic asm: its text as written, a single % */
    __asm__ volatile("moveq #-1,%d0\n\tmoveq #-1,%d1\n\tsub.l %a0,%a0");
    return x + y * 2 + z * 3 + w * 5;
}

/* templates that call code changing registers the templates never name */
static int clobber_call(int a, int b)
{
    int x = a * 3, y = b + 11, z = a ^ b, w = a - b, v = a + b * 2;
    void (*f)(void) = asm_trash, (*g)(void) = asm_trash_saved;
    __asm__ volatile("jsr (%0)" :: "a"(f)
                     : "d0", "d1", "a0", "a1", "memory", "cc");
    __asm__ volatile("jsr (%0)" :: "a"(g)
                     : "d2", "d3", "d4", "d5", "d6", "d7", "a2", "a3", "a4",
                       "a5", "memory", "cc");
    return x + y * 2 + z * 3 + w * 5 + v * 7;
}

__attribute__((noinline)) static int around_clobber(int a)
{
    int p = a * 5, q = a * 7, r = a * 11, s = a * 13, t = a + 1, u = a ^ 99;
    int m = clobber_call(a, 3);
    return m + p + q * 2 + r * 3 + s * 4 + t * 5 + u * 6;
}

static unsigned char bits_byte = 0x10;
static int bit_ops(void)
{
    unsigned char *p = &bits_byte;
    int r;
    __asm__ volatile("bset #0,(%0)\n\tbclr #4,(%0)\n\tbchg #7,(%0)"
                     :: "a"(p) : "memory");
    __asm__("btst #7,(%1)\n\tsne %0\n\textb.l %0\n\tneg.l %0"
            : "=d"(r) : "a"(p) : "cc");
    return bits_byte * 10 + r;
}

static int divrem(int a, int b, int *rem)
{
    int q, r;
    __asm__("move.l %2,%0\n\trems.l %3,%1:%0\n\tdivs.l %3,%0"
            : "=&d"(q), "=&d"(r) : "d"(a), "d"(b));
    *rem = r;
    return q;
}

static int multi_out(int a, int *hi)
{
    int lo, h;
    __asm__("move.l %2,%0\n\tlsl.l #4,%0\n\tmove.l %2,%1\n\tlsr.l #4,%1"
            : "=&d"(lo), "=&d"(h) : "d"(a));
    *hi = h;
    return lo;
}

__attribute__((naked)) int naked_add(int a, int b)
{
    __asm__("move.l 4(%%sp),%%d0\n\tadd.l 8(%%sp),%%d0\n\taddq.l %0,%%d0\n\t"
            "rts" :: "i"(5));
}

__asm__("\t.global block_mul\n"
        "block_mul:\n"
        "\tmove.l 4(%sp),%d0\n"
        "\tmuls.l 8(%sp),%d0\n"
        "\trts\n");
int block_mul(int a, int b);

static const int tbl[5] = { 3, 1, 4, 1, 5 };

int main(void)
{
    int hi, lo, rem;
    putn(add2(30, 12));
    putn(supervisor());
    putn(ipl_raised());
    putn(ccr_rt(4));
    putn(ccr_rt(0x1b));
    putn((long)usp_rt(0x47ff0000u));
    putn(trap_twice());
    putn(loop3(7));
    putn(fwd(3));
    putn(fwd(30));
    putn(mem_ops(9));
    putn(gmem);
    putn(imm_ops());
    putn(live_across(17, 5));
    putn(around_clobber(9));
    putn(bit_ops());
    putn(divrem(-100, 7, &rem));
    putn(rem);
    lo = multi_out(0x1234, &hi);
    putn(lo);
    putn(hi);
    putn(naked_add(10, 20));
    putn(block_mul(6, -7));
    puts_("\n");
    putn(asm_sum(tbl, 5));
    putn(asm_call_c(5));
    putn(asm_tail(3));
    putn(asm_relax(4));
    putn(asm_alu(100, 7));
    {
        unsigned char b = 0x0f;
        putn(asm_bits(&b));
        putn(b);
    }
    putn(asm_table(0));
    putn(asm_table(2));
    putn(asm_var_bump());
    putn(asm_var);
    putn(asm_div(-100, 7));
    putn(asm_swap(0x12345680));
    putn(asm_link(37));
    putn(asm_ipl());
    puts_("\n");
    return 0;
}
