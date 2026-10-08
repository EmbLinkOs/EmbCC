/* RX assembly on the board (tests/golden/rx-asm.sh): inline asm of every
 * operand kind, the control registers, numeric labels, values live across
 * templates that clobber; a file-scope asm function, a naked function;
 * and forms.S, assembled by EmbCC or by GNU as, called from here and
 * calling back. Each line printed is the values the program computed,
 * checked against what C computes for the same thing where it can. */
void putn(long v);
void puts_(const char *s);

/* forms.S */
int asm_sum(const int *p, int n);
int asm_call_c(int x);
int asm_relax(int x);
int asm_alu(int a, int b);
int asm_bits(unsigned char *p);
int asm_table(int k);
int asm_copy(char *d, const char *s, int n);
unsigned asm_usp_roundtrip(unsigned v);
int asm_var_bump(void);
extern int asm_var;
int asm_div(int a, int b);
int asm_long_branch(int x);
int asm_tail(int x);
void asm_trash(void);           /* writes r1-r5, r14, r15 */
void asm_trash_saved(void);     /* writes r6-r12 */

int c_helper(int x) { return x * 7 + 1; }

static int fails;
static void check(long got, long want)
{
    if (got != want) {
        fails++;
        puts_("[bad: want ");
        putn(want);
        puts_("] ");
    }
    putn(got);
}

/* ---- inline asm ---- */
static int add3(int a, int b)
{
    int r;
    __asm__("add %1, %2, %0" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

static unsigned psw_c(int set)
{
    unsigned r;
    if (set)
        __asm__ volatile("setpsw c\n\tmvfc psw, %0" : "=r"(r) :: "cc");
    else
        __asm__ volatile("clrpsw c\n\tmvfc psw, %0" : "=r"(r) :: "cc");
    return r & 1;
}

static int isp_is_sp(void)
{
    unsigned isp, sp;
    __asm__ volatile("mvfc isp, %0\n\tmov.l r0, %1" : "=r"(isp), "=r"(sp));
    return isp == sp;
}

static unsigned usp_rt(unsigned v)
{
    unsigned old, r;
    __asm__ volatile("mvfc usp, %0\n\tmvtc %2, usp\n\tmvfc usp, %1\n\t"
                     "mvtc %0, usp" : "=&r"(old), "=&r"(r) : "r"(v));
    return r;
}

static unsigned intb_rt(void)
{
    unsigned old, r;
    __asm__ volatile("mvfc intb, %0\n\tmvtc #0x1234560, intb\n\t"
                     "mvfc intb, %1\n\tmvtc %0, intb" : "=&r"(old), "=r"(r));
    return r;
}

static int loop3(int n)
{
    int r = 0;
    __asm__("1:\tadd #3, %0\n\tsub #1, %1\n\tbne 1b" : "+r"(r), "+r"(n));
    return r;
}

static int fwd(int x)
{
    __asm__("cmp #10, %0\n\tbge 2f\n\tadd #100, %0\n2:\tadd #1, %0"
            : "+r"(x) :: "cc");
    return x;
}

int gmem = 5;
static int mem_ops(int v)
{
    __asm__ volatile("mov.l %1, %0" : "=m"(gmem) : "r"(v));
    __asm__ volatile("add %1, %0" : "+r"(v) : "m"(gmem));
    return v;
}

static int imm_ops(void)
{
    int r;
    __asm__("mov.l %1, %0\n\tadd %2, %0" : "=r"(r) : "i"(1000), "n"(-7));
    return r;
}

/* values live across a template that clobbers callee-saved registers,
 * and one that changes r3 and r4 without saying so */
static int live_across(int a, int b)
{
    int x = a * 3, y = b + 11, z = a ^ b, w = a - b;
    __asm__ volatile("mov.l #0, r6\n\tmov.l #0, r7\n\tmov.l #0, r8\n\t"
                     "mov.l #0, r9\n\tmov.l #0, r10\n\tmov.l #0, r11\n\t"
                     "mov.l #0, r12\n\tmov.l #0, r1\n\tmov.l #0, r2"
                     ::: "r1", "r2", "r6", "r7", "r8", "r9", "r10", "r11",
                         "r12");
    __asm__ volatile("mov.l #-1, r3\n\tmov.l #-1, r4");
    return x + y * 2 + z * 3 + w * 5;
}

static unsigned char bits_byte = 0x10;
static int bit_ops(void)
{
    unsigned char *p = &bits_byte;
    int r;
    __asm__ volatile("bset #0, [%0].b\n\tbclr #4, [%0].b\n\tbnot #7, [%0].b"
                     :: "r"(p) : "memory");
    __asm__("btst #7, [%1].b\n\tscne.l %0" : "=r"(r) : "r"(p) : "cc");
    return bits_byte * 10 + r;
}

static int string_copy(char *d, const char *s, int n)
{
    register char *rd __asm__("r1") = d;
    register const char *rs __asm__("r2") = s;
    register int rn __asm__("r3") = n;
    __asm__ volatile("smovf" : "+r"(rd), "+r"(rs), "+r"(rn) :: "memory");
    return (int)(rd - d);
}

/* a call from a template, the caller-saved registers clobbered */
static int call_from_asm(int x)
{
    int r, keep = x + 1000;
    int (*fp)(int) = c_helper;
    __asm__ volatile("mov.l %2, r1\n\tjsr %1\n\tmov.l r1, %0"
                     : "=r"(r) : "r"(fp), "r"(x)
                     : "r1", "r2", "r3", "r4", "r5", "r14", "r15", "memory",
                       "cc");
    return r + keep;
}

/* templates that call code changing registers the templates never name:
 * the clobber lists alone must keep the live values out of them, and the
 * callee-saved ones must be saved for this function's caller */
static int clobber_call(int a, int b)
{
    int x = a * 3, y = b + 11, z = a ^ b, w = a - b, v = a + b * 2;
    void (*f)(void) = asm_trash, (*g)(void) = asm_trash_saved;
    __asm__ volatile("jsr %0" :: "r"(f)
                     : "r1", "r2", "r3", "r4", "r5", "r14", "r15", "memory",
                       "cc");
    __asm__ volatile("jsr %0" :: "r"(g)
                     : "r6", "r7", "r8", "r9", "r10", "r11", "r12", "memory",
                       "cc");
    return x + y * 2 + z * 3 + w * 5 + v * 7;
}

__attribute__((noinline)) static int around_clobber(int a)
{
    int p = a * 5, q = a * 7, r = a * 11, s = a * 13, t = a + 1, u = a ^ 99;
    int m = clobber_call(a, 3);
    return m + p + q * 2 + r * 3 + s * 4 + t * 5 + u * 6;
}

static int multi_out(int a, int *hi)
{
    int lo, h;
    __asm__("mov.l %2, %0\n\tshll #4, %0\n\tmov.l %2, %1\n\tshlr #4, %1"
            : "=&r"(lo), "=&r"(h) : "r"(a));
    *hi = h;
    return lo;
}

__attribute__((naked)) int naked_add(int a, int b)
{
    __asm__("add r2, r1\n\tadd %0, r1\n\trts" :: "i"(5));
}

__asm__("\t.global _block_mul\n"
        "_block_mul:\n"
        "\tmul r2, r1\n"
        "\trts\n");
int block_mul(int a, int b);

static const int tbl[5] = { 3, 1, 4, 1, 5 };

int main(void)
{
    char buf[16];
    int hi, lo;
    check(add3(30, 12), 42);
    check(psw_c(1), 1);
    check(psw_c(0), 0);
    check(isp_is_sp(), 1);
    check((long)usp_rt(0x01ff0000u), 0x01ff0000L);
    check((long)intb_rt(), 0x1234560L);
    check(loop3(7), 21);
    check(fwd(3), 104);
    check(fwd(30), 31);
    check(mem_ops(9), 18);
    check(gmem, 9);
    check(imm_ops(), 993);
    check(live_across(17, 5), 17 * 3 + (5 + 11) * 2 + (17 ^ 5) * 3 + 12 * 5);
    check(bit_ops(), 0x81 * 10 + 1);
    check(string_copy(buf, "hello, rx", 10), 10);
    check(buf[7], 'r');
    check(call_from_asm(6), 6 * 7 + 1 + 1006);
    check(around_clobber(9), (9 * 3 + 14 * 2 + (9 ^ 3) * 3 + 6 * 5 + 15 * 7) +
                             45 + 63 * 2 + 99 * 3 + 117 * 4 + 10 * 5 +
                             (9 ^ 99) * 6);
    lo = multi_out(0x1234, &hi);
    check(lo, 0x12340);
    check(hi, 0x123);
    check(naked_add(10, 20), 35);
    check(block_mul(6, 7), 42);
    puts_("\n");
    /* forms.S */
    check(asm_sum(tbl, 5), 14);
    check(asm_call_c(5), 5 * 7 + 1 + 5);
    check(asm_relax(4), 4 + 1 + 2 + 3 + 4);
    check(asm_alu(100, 7), ((100 + 7) * 3 - 7 - (100 & 7) + (100 | 7) +
                            (100 ^ 7) + (100 >> 2) + (100 << 3) +
                            ((unsigned)-100 >> 28)));
    {
        unsigned char b = 0x0f;
        int r = asm_bits(&b);
        check(r, 1);
        check(b, 0x8e);
    }
    check(asm_table(0), 10);
    check(asm_table(2), 30);
    check(asm_copy(buf, "abcdefg", 7), 'g');
    check(buf[3], 'd');
    check((long)asm_usp_roundtrip(0x01fe0000u), 0x01fe0000L);
    check(asm_var_bump(), 8);
    check(asm_var, 8);
    check(asm_div(-100, 7), -100 / 7 * 1000 + (unsigned)100 / 7);
    check(asm_long_branch(3), 3 * 2 + 1);
    check(asm_tail(3), 3 * 7 + 1);
    puts_("\n");
    return fails;
}
