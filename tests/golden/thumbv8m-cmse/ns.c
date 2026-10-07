/* The Non-secure image: it calls the Secure image only through the
 * functions in its import library (embld --out-implib), which are the
 * addresses of their SG veneers -- and reads, after each kind of crossing,
 * the registers and the flags the other side left, which must not hold a
 * Secure secret (ACLE's CMSE). The results go back through s_report, the
 * UART being the Secure side's. */
#define SECRET 0x5ec1e7e1u

int s_add(int a, int b);
void s_report(int what, int ok, unsigned v);
void s_done(int fails);
unsigned s_secret(unsigned x);
int s_call_back(int (*cb)(int, int));
int s_sum(const int *p, int n);
unsigned s_tta(void *p);
int s_call_raw(int (*cb)(int, int));

void probe(unsigned *out);
int ns_cb(int a, int b);

/* What ns_cb saw on entry: APSR, r8-r12, then r0-r7. */
unsigned ns_seen[14];
static int fails;
static int ns_buf[4] = { 10, 20, 30, 40 };

static void check(int what, int ok, unsigned v)
{
    s_report(what, ok, v);
    if (!ok)
        fails++;
}

/* r0-r3, r12 and APSR as s_secret leaves them, read at once. */
__attribute__((naked)) void probe(unsigned *out)
{
    __asm__ volatile("push {r4, lr}\n\t"
                     "mov r4, r0\n\t"
                     "movs r0, #0x77\n\t"
                     "bl s_secret\n\t"
                     "str r0, [r4]\n\t"
                     "str r1, [r4, #4]\n\t"
                     "str r2, [r4, #8]\n\t"
                     "str r3, [r4, #12]\n\t"
                     "mov r0, r12\n\t"
                     "str r0, [r4, #16]\n\t"
                     "mrs r0, apsr\n\t"
                     "str r0, [r4, #20]\n\t"
                     "pop {r4, pc}");
}

/* Called by the Secure state through a cmse_nonsecure_call pointer: keeps
 * APSR and every register as it arrived in ns_seen, and returns a + b. */
__attribute__((naked)) int ns_cb(int a, int b)
{
    __asm__ volatile("push {r0-r7, lr}\n\t"
                     "mrs r0, apsr\n\t"
                     "mov r1, r8\n\t"
                     "mov r2, r9\n\t"
                     "mov r3, r10\n\t"
                     "mov r4, r11\n\t"
                     "mov r5, r12\n\t"
                     "push {r0-r5}\n\t"
                     "ldr r6, =ns_seen\n\t"
                     "mov r1, sp\n\t"
                     "movs r7, #0\n"
                     "1:\n\t"
                     "ldr r0, [r1, r7]\n\t"
                     "str r0, [r6, r7]\n\t"
                     "adds r7, #4\n\t"
                     "cmp r7, #56\n\t"
                     "bne 1b\n\t"
                     "add sp, #24\n\t"
                     "ldr r0, [sp]\n\t"
                     "ldr r1, [sp, #4]\n\t"
                     "adds r0, r0, r1\n\t"
                     "str r0, [sp]\n\t"
                     "pop {r0-r7, pc}");
}

int main(void)
{
    unsigned r[6];
    int bad, v;
    /* an entry function, through its veneer */
    v = s_add(40, 2);
    check(1, v == 42, (unsigned)v);
    /* the registers and flags an entry function returns with */
    probe(r);
    check(2, r[0] == (0x77u ^ 0x1234u), r[0]);
    bad = 0;
    for (int k = 1; k <= 4; k++)
        bad |= r[k] == SECRET;
    check(3, !bad, r[1]);
    check(4, (r[5] & 0xf8000000u) == 0, r[5]);
    /* a call back into the Non-secure state through a cmse_nonsecure_call
     * pointer, and what the callee found in the registers and flags */
    v = s_call_back(ns_cb);
    check(5, v == 43, (unsigned)v);
    check(6, ns_seen[6] == 40 && ns_seen[7] == 2, ns_seen[6]);
    bad = 0;
    for (int k = 1; k <= 5; k++)            /* r8-r12 */
        bad |= ns_seen[k] == SECRET;
    for (int k = 8; k <= 13; k++)           /* r2-r7 */
        bad |= ns_seen[k] == SECRET;
    check(7, !bad, ns_seen[8]);
    check(8, (ns_seen[0] & 0xf8000000u) == 0, ns_seen[0]);
    /* <arm_cmse.h> on the Secure side: a Non-secure buffer passes
     * cmse_check_address_range, a Secure address does not */
    v = s_sum(ns_buf, 4);
    check(9, v == 100, (unsigned)v);
    v = s_sum((const int *)0x10100000, 4);
    check(10, v == -1, (unsigned)v);
    /* TTA on a Non-secure and a Secure address: the `secure` bit, 22 */
    check(11, !(s_tta(ns_buf) & (1u << 22)) &&
              (s_tta((void *)0x10100000) & (1u << 22)), s_tta(ns_buf));
    /* a Non-secure function pointer with its Thumb bit, called as is */
    v = s_call_raw(ns_cb);
    check(12, v == 11, (unsigned)v);
    s_done(fails);
    return 0;
}
