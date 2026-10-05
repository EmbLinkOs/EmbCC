/* A returning exception handler on QEMU's malta (tests/golden/mips-exc.sh):
 * vector.S saves the context, calls exc_c below, and erets.
 *
 *  1. syscall, five times, from a NAKED function (its body is `syscall;
 *     jr $ra`): the handler writes 2n+1 into the saved v0 and resumes
 *     after the syscall, so each call returns a value only the handler
 *     computed -- the program observes the handler returning.
 *  2. The CP0 timer (Count/Compare, interrupt line 7), while a loop
 *     keeps many values in registers and HI/LO busy: at least three
 *     interrupts arrive inside it (QEMU runs with -icount, so the count is
 *     the same every run -- about fifty at -O2), and the loop's result
 *     must equal the same loop's result with interrupts off, so an
 *     interrupt anywhere in it changed nothing.
 *  3. File-scope asm with instructions: a function written as a block,
 *     calling back into C.
 *
 * Every result goes through putn; the script compares the line. */
void putn(long v);
void puts_(const char *s);

extern char exc_stub[], exc_stub_end[];

volatile unsigned syscalls, ticks, unexpected;
static unsigned period;

static unsigned rd_count(void)
{
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

static void wr_compare(unsigned v)
{
    __asm__ volatile("mtc0 %0, $11" : : "r"(v));
}

static unsigned rd_status(void)
{
    unsigned s;
    __asm__ volatile("mfc0 %0, $12" : "=r"(s));
    return s;
}

static void wr_status(unsigned s)
{
    __asm__ volatile("mtc0 %0, $12; ehb" : : "r"(s));
}

/* Called by vector.S: the frame's words are at, v0, v1, a0..a3, t0..t9,
 * ra, hi, lo. Returns where to resume. */
unsigned exc_c(unsigned cause, unsigned epc, unsigned *frame)
{
    unsigned code = (cause >> 2) & 31;
    if (code == 8 && !(cause & 0x80000000u)) {     /* Sys, not in a slot */
        syscalls++;
        frame[1] = frame[3] * 2 + 1;                /* v0 = 2 * a0 + 1 */
        return epc + 4;
    }
    if (code == 0 && (cause & (1u << 15))) {       /* timer: IP7 */
        ticks++;
        wr_compare(rd_count() + period);           /* re-arm, and ack */
        return epc;                                /* the interrupted one */
    }
    unexpected = cause;
    wr_compare(rd_count() - 1);                    /* quiet the timer */
    return epc + 4;
}

__attribute__((naked)) int sys(int n)
{
    __asm__("syscall");
    __asm__("jr $ra");
}

/* a function written as a file-scope block: asm_twice(x) = c_twice(x) +
 * c_twice(x), with its own frame and two calls into C */
int c_twice(int x) { return 2 * x; }
__asm__(".text\n"
        ".globl asm_twice\n"
        "asm_twice:\n"
        "  addiu $sp, $sp, -24\n"
        "  sw $ra, 20($sp)\n"
        "  sw $s0, 16($sp)\n"
        "  move $s0, $a0\n"
        "  jal c_twice\n"
        "  move $a0, $s0\n"
        "  move $s0, $v0\n"
        "  jal c_twice\n"
        "  addu $v0, $v0, $s0\n"
        "  lw $s0, 16($sp)\n"
        "  lw $ra, 20($sp)\n"
        "  addiu $sp, $sp, 24\n"
        "  jr $ra\n");
int asm_twice(int x);

/* many values live across the loop body, and HI/LO through the multiply */
static __attribute__((noinline)) unsigned churn(unsigned n)
{
    unsigned a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
    for (unsigned k = 0; k < n; k++) {
        a = a * 1103515245u + 12345u;
        b ^= a >> 3;
        c += b * 7u;
        d = (d << 5) ^ c ^ (d >> 2);
        e += d * a;
        f -= e ^ k;
        g = g * 31u + f;
        h += (unsigned)((unsigned long long)g * a >> 32);
    }
    return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h;
}

int main(void)
{
    volatile unsigned *vec = (volatile unsigned *)0x80000180u;
    unsigned want, got, guard;
    int sum = 0;

    for (unsigned k = 0; k < (unsigned)(exc_stub_end - exc_stub) / 4; k++)
        vec[k] = ((unsigned *)exc_stub)[k];

    for (int n = 1; n <= 5; n++)
        sum += sys(n);                      /* 3 + 5 + 7 + 9 + 11 */
    putn(sum);
    putn((long)syscalls);

    want = churn(20000);                    /* interrupts off */
    period = 3000;
    wr_compare(rd_count() + period);
    wr_status(rd_status() | (1u << 15) | 1u);          /* IM7, IE */
    got = churn(20000);
    putn(ticks >= 3);                       /* ...and some arrived in it */
    for (guard = 0; ticks < 3 && guard < 50000000u; guard++)
        ;
    wr_status(rd_status() & ~1u);
    wr_compare(rd_count() - 1);
    putn(ticks >= 3);
    putn(got == want);
    putn((long)unexpected);

    putn(asm_twice(21));
    return 42;
}
