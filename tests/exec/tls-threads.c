/* _Thread_local on bare metal, as an RTOS uses it: each thread has its
 * own copy of the thread block, made from the template the linker
 * describes (__tdata_start/__tdata_size, then __tbss_size of zeros), and
 * the thread pointer selects it -- __aeabi_read_tp() on ARM, which the
 * RTOS defines, and the tp register on RISC-V. EmbCC made every
 * thread-local one object all threads shared. Two blocks here, switched
 * between: each keeps its own values, starts from the template's (5 and
 * zeros), and an initialized and a zeroed thread-local and an array
 * are all in the block. Elsewhere (a hosted target, whose OS has its own
 * thread blocks) the program only returns 42. */
// expect-exit: 42
#if (defined(__arm__) || defined(__riscv)) && !defined(__linux__)
#define BARE_TLS 1
_Thread_local int counter = 5;
_Thread_local char buf[16];
_Thread_local long long wide = 0x1122334455667788LL;

extern char __tdata_start[], __tdata_size[], __tbss_size[], __tls_align[];

static unsigned char blocks[2][128] __attribute__((aligned(16)));
static void *tps[2];
static void *cur_tp;

#if defined(__arm__)
void *__aeabi_read_tp(void) { return cur_tp; }
#endif

static void use_thread(int k)
{
    cur_tp = tps[k];
#if defined(__riscv)
    __asm__ volatile("mv tp, %0" : : "r"(cur_tp) : "memory");
#endif
}

static void make_thread(int k)
{
    unsigned long al = (unsigned long)__tls_align, n = (unsigned long)__tdata_size;
    unsigned long z = (unsigned long)__tbss_size;
#if defined(__arm__)
    /* the 8-byte control block, then the block at its alignment */
    unsigned long tcb = (8 + al - 1) & ~(al - 1);
    unsigned char *b = blocks[k] + tcb;
    tps[k] = blocks[k];
#else
    unsigned char *b = blocks[k];
    tps[k] = b;
#endif
    for (unsigned long i = 0; i < n; i++)
        b[i] = (unsigned char)__tdata_start[i];
    for (unsigned long i = 0; i < z; i++)
        b[n + i] = 0;
}

/* Every access is in a function of its own, run after the switch, as a
 * thread's code is: within one function the thread pointer is fixed (a
 * context switch is not in the middle of one), and GCC and EmbCC alike
 * compute a thread-local's address once there. */
#define NI __attribute__((noinline))
NI static int bump(int by) { counter += by; return counter; }
NI static int get_counter(void) { return counter; }
NI static char get_buf(int i) { return buf[i]; }
NI static void set_buf(int i, char c) { buf[i] = c; }
NI static long long get_wide(void) { return wide; }
NI static void set_wide(long long v) { wide = v; }
NI static int *where(void) { return &counter; }
#endif

int main(void)
{
#ifdef BARE_TLS
    make_thread(0);
    make_thread(1);
    use_thread(0);
    if (get_counter() != 5 || get_buf(3) != 0 ||
        get_wide() != 0x1122334455667788LL) return 1;
    if (bump(1) != 6) return 2;
    set_buf(0, 'a');
    set_wide(7);
    int *at0 = where();
    use_thread(1);
    if (get_counter() != 5 || get_buf(0) != 0 ||
        get_wide() != 0x1122334455667788LL) return 3;
    if (bump(10) != 15) return 4;
    if (where() == at0) return 7;            /* two objects, not one */
    use_thread(0);
    if (get_counter() != 6 || get_buf(0) != 'a' || get_wide() != 7) return 5;
    use_thread(1);
    if (get_counter() != 15 || get_buf(0) != 0) return 6;
#endif
    return 42;
}
