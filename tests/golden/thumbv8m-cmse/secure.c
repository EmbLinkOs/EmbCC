/* The Secure image: a minimal secure boot that sets up the SAU, the IDAU's
 * Non-secure Callable region and the memory protection controller, points
 * the Non-secure state at its image and enters it -- through a
 * cmse_nonsecure_call pointer -- and the cmse_nonsecure_entry functions the
 * Non-secure image calls through their SG veneers (see ns.c). Built with
 * -mcmse; the UART is the Secure side's alone. */
#include <arm_cmse.h>

#define SECRET 0x5ec1e7e1u

#define UART0_DR   (*(volatile unsigned *)0x40200000u)
#define UART0_CTRL (*(volatile unsigned *)0x40200008u)
/* the SAU */
#define SAU_CTRL   (*(volatile unsigned *)0xE000EDD0u)
#define SAU_RNR    (*(volatile unsigned *)0xE000EDD8u)
#define SAU_RBAR   (*(volatile unsigned *)0xE000EDDCu)
#define SAU_RLAR   (*(volatile unsigned *)0xE000EDE0u)
/* the IoT Kit's Secure Privilege Control: NSCCFG bit 0, CODENSC, makes the
 * IDAU's region 0x1xxxxxxx Non-secure Callable wherever the SAU says so */
#define NSCCFG     (*(volatile unsigned *)0x50080014u)
/* the memory protection controller in front of SSRAM1 */
#define MPC_CFG    (*(volatile unsigned *)0x58007014u)
#define MPC_IDX    (*(volatile unsigned *)0x58007018u)
#define MPC_LUT    (*(volatile unsigned *)0x5800701Cu)
/* the Non-secure state's VTOR, through the SCB's Non-secure alias */
#define VTOR_NS    (*(volatile unsigned *)0xE002ED08u)

#define NS_BASE    0x00200000u      /* the Non-secure image (ns.c) */
#define NS_END     0x00400000u

extern unsigned char __sg_start[], __sg_end[], __bss_start[], __bss_end[];

static void putc_(int c)
{
    UART0_CTRL = 1u;
    UART0_DR = (unsigned)c & 0xffu;
}
static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}
static void puthex(unsigned v)
{
    for (int k = 28; k >= 0; k -= 4)
        putc_("0123456789abcdef"[(v >> k) & 15u]);
}
static void putdec(int v)
{
    char b[12];
    int n = 0;
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
    if (v < 0) putc_('-');
    do { b[n++] = (char)('0' + (int)(u % 10u)); u /= 10u; } while (u);
    while (n)
        putc_(b[--n]);
}

/* ---- what the Non-secure image calls ----------------------------------- */

int __attribute__((cmse_nonsecure_entry)) s_add(int a, int b)
{
    return a + b;
}

/* One line per check the Non-secure side makes. */
void __attribute__((cmse_nonsecure_entry)) s_report(int what, int ok,
                                                    unsigned v)
{
    puts_(ok ? "ok " : "FAIL ");
    putdec(what);
    putc_(' ');
    puthex(v);
    putc_('\n');
}

void __attribute__((cmse_nonsecure_entry)) s_done(int fails)
{
    puts_("==EXIT ");
    putdec(fails);
    puts_("==\n");
    for (;;)
        ;
}

/* Leaves a secret in every register the AAPCS lets a callee change and
 * in the flags, as Secure code computing with one would: the return must
 * clear them (the Non-secure caller reads them back, ns.c probe). */
unsigned __attribute__((cmse_nonsecure_entry)) s_secret(unsigned x)
{
    unsigned r = x ^ 0x1234u;
    __asm__ volatile("ldr r1, =0x5ec1e7e1\n\t"
                     "mov r2, r1\n\t"
                     "mov r3, r1\n\t"
                     "mov r12, r1\n\t"
                     "ldr r1, =0xf8000000\n\t"
                     "msr apsr_nzcvq, r1\n\t"
                     "ldr r1, =0x5ec1e7e1"
                     ::: "r1", "r2", "r3", "r12", "cc");
    return r;
}

/* Calls the Non-secure function it is handed through a
 * cmse_nonsecure_call pointer, with a secret in every register that is not
 * an argument and in the flags: the call must clear them (the callee reads
 * them, ns.c ns_cb). The secret is put there by a naked caller, because an
 * asm statement may not clobber r4-r11 here: do_ns_call is compiled as
 * usual, and every register it leaves alone still holds the secret when
 * its call sequence runs. */
typedef int __attribute__((cmse_nonsecure_call)) ns_cb_t(int, int);

__attribute__((noinline)) int do_ns_call(ns_cb_t *f)
{
    return f(40, 2);
}

__attribute__((naked)) int secret_then_call(ns_cb_t *f)
{
    __asm__ volatile("push {r3-r7, lr}\n\t"
                     "mov r4, r8\n\tmov r5, r9\n\t"
                     "mov r6, r10\n\tmov r7, r11\n\t"
                     "push {r4-r7}\n\t"
                     "ldr r1, =0x5ec1e7e1\n\t"
                     "mov r2, r1\n\tmov r3, r1\n\tmov r4, r1\n\t"
                     "mov r5, r1\n\tmov r6, r1\n\tmov r7, r1\n\t"
                     "mov r8, r1\n\tmov r9, r1\n\tmov r10, r1\n\t"
                     "mov r11, r1\n\tmov r12, r1\n\t"
                     "ldr r1, =0xf8000000\n\t"
                     "msr apsr_nzcvq, r1\n\t"
                     "ldr r1, =0x5ec1e7e1\n\t"
                     "bl do_ns_call\n\t"
                     "pop {r4-r7}\n\t"
                     "mov r8, r4\n\tmov r9, r5\n\t"
                     "mov r10, r6\n\tmov r11, r7\n\t"
                     "pop {r3-r7, pc}");
}

int __attribute__((cmse_nonsecure_entry)) s_call_back(int (*cb)(int, int))
{
    ns_cb_t *f = cmse_nsfptr_create((ns_cb_t *)cb);
    return secret_then_call(f) + 1;
}

/* A Non-secure pointer checked with <arm_cmse.h> before it is read:
 * -1 when [p, p + n) is not memory the Non-secure state may read. */
int __attribute__((cmse_nonsecure_entry)) s_sum(const int *p, int n)
{
    int s = 0;
    if (!cmse_check_address_range((void *)p, (unsigned)n * sizeof *p,
                                  CMSE_NONSECURE | CMSE_MPU_READ))
        return -1;
    for (int k = 0; k < n; k++)
        s += p[k];
    return s;
}

/* What TT says about an address, from the Secure side (TTA: as the
 * Non-secure state sees it). */
unsigned __attribute__((cmse_nonsecure_entry)) s_tta(void *p)
{
    return cmse_TTA(p).value;
}

/* ---- the boot ----------------------------------------------------------- */

typedef void __attribute__((cmse_nonsecure_call)) ns_entry_t(void);

void s_main(void)
{
    unsigned bs, *nsvec = (unsigned *)NS_BASE;
    for (unsigned char *d = __bss_start; d < __bss_end; d++)
        *d = 0;
    puts_("S: boot\n");
    /* SSRAM1's upper 2 MiB to the Non-secure state, in the MPC: one bit a
     * block, 32 blocks a word of the look-up table */
    bs = 1u << (MPC_CFG + 5);
    for (unsigned a = NS_BASE; a < NS_END; a += bs * 32u) {
        MPC_IDX = a / bs / 32u;
        MPC_LUT = 0xffffffffu;
    }
    /* the SAU: the Non-secure image's memory, and the veneers Non-secure
     * Callable */
    SAU_RNR = 0;
    SAU_RBAR = NS_BASE;
    SAU_RLAR = ((NS_END - 1u) & ~31u) | 1u;
    SAU_RNR = 1;
    SAU_RBAR = (unsigned)__sg_start;
    SAU_RLAR = (((unsigned)__sg_end - 1u) & ~31u) | 3u;
    SAU_CTRL = 1u;
    NSCCFG |= 1u;
    __asm__ volatile("dsb\n\tisb" ::: "memory");
    /* the Non-secure state's vector table and stack, then its reset
     * handler -- through a Non-secure function pointer: BLXNS */
    VTOR_NS = NS_BASE;
    __asm__ volatile("msr msp_ns, %0" :: "r"(nsvec[0]));
    {
        ns_entry_t *ns_reset = cmse_nsfptr_create((ns_entry_t *)nsvec[1]);
        ns_reset();
    }
    puts_("S: the Non-secure reset returned\n==EXIT 99==\n");
    for (;;)
        ;
}
