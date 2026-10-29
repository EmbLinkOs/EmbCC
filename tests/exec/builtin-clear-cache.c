/* GCC's __builtin___clear_cache(begin, end), as a loader or a JIT uses
 * it: a function written into RAM as machine code, run, written again
 * with another answer, the range cleared, and run again -- which must
 * give the new answer. Each architecture's `return N` is two or three
 * instructions here; where there is none (or where RAM is not
 * executable to a program: the hosted systems) the program checks only
 * that the builtin compiles and evaluates its arguments once.
 *
 * Under QEMU, which notices code written over itself, this cannot show a
 * missing cache operation; it shows that each target's sequence -- dc
 * cvau and ic ivau, CP15's, synci and jr.hb, fence.i, icbi, flush --
 * runs and returns. */
// expect-exit: 42

typedef int (*fn_t)(void);

static unsigned char code[64] __attribute__((aligned(64)));
static int evals;
static unsigned char *at(unsigned char *p) { evals++; return p; }

/* (x86-64 too: the builtin emits nothing there, and an x86_64-elf
 * program may run on a Linux host, whose data is not executable) */
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32) || \
    defined(__x86_64__)
#define NO_EXEC 1
#endif

/* writes `return n` at code, and says how many bytes */
static int emit(int n)
{
    unsigned char *p = code;
    int k = 0;
#define B(x) (p[k++] = (unsigned char)(x))
#define W32LE(x) (B(x), B((x) >> 8), B((x) >> 16), B((x) >> 24))
#define W32BE(x) (B((x) >> 24), B((x) >> 16), B((x) >> 8), B(x))
#if defined(NO_EXEC)
    (void)n; (void)p;
#elif defined(__x86_64__)
    B(0xb8); W32LE((unsigned)n); B(0xc3);              /* mov eax, n; ret */
#elif defined(__aarch64__)
    W32LE(0x52800000u | ((unsigned)n << 5));           /* movz w0, #n */
    W32LE(0xd65f03c0u);                                /* ret */
#elif defined(__thumb__)
    B(n); B(0x20); B(0x70); B(0x47);                   /* movs r0, #n; bx lr */
#elif defined(__arm__)
    W32LE(0xe3a00000u | (unsigned)n);                  /* mov r0, #n */
    W32LE(0xe12fff1eu);                                /* bx lr */
#elif defined(__riscv)
    W32LE(0x00000513u | ((unsigned)n << 20));          /* li a0, n */
    W32LE(0x00008067u);                                /* ret */
#elif defined(__mips__) && defined(__MIPSEB__)
    W32BE(0x03e00008u);                                /* jr $ra */
    W32BE(0x24020000u | (unsigned)n);                  /* li $v0, n (slot) */
#elif defined(__mips__)
    W32LE(0x03e00008u);
    W32LE(0x24020000u | (unsigned)n);
#elif defined(__loongarch__)
    W32LE(0x02800004u | ((unsigned)n << 10));          /* addi.w $a0, $zero, n */
    W32LE(0x4c000020u);                                /* jirl $zero, $ra, 0 */
#elif defined(__PPC__) || defined(__powerpc__)
    W32BE(0x38600000u | (unsigned)n);                  /* li r3, n */
    W32BE(0x4e800020u);                                /* blr */
#elif defined(__sparc__)
    W32BE(0x81c3e008u);                                /* retl */
    W32BE(0x90102000u | (unsigned)n);                  /* mov n, %o0 (slot) */
#else
    (void)n; (void)p;
#endif
    return k;
}

int main(void)
{
    int bad = 0, len = emit(7);
    if (len) {
        __builtin___clear_cache((char *)code, (char *)code + len);
#if defined(__thumb__) && !defined(__aarch64__)
        fn_t f = (fn_t)((unsigned long)code | 1);
#else
        fn_t f = (fn_t)(void *)code;
#endif
        bad |= (f() != 7) << 0;
        emit(42);
        __builtin___clear_cache(at(code), at(code) + len);
        bad |= (f() != 42) << 1;
        bad |= (evals != 2) << 2;
    } else {
        __builtin___clear_cache(at(code), at(code) + sizeof code);
        bad |= (evals != 2) << 2;
    }
    return bad ? bad : 42;
}
