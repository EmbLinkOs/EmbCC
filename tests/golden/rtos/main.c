void puts_(const char *s);
void putn(long v);
void reset(void);
void PendSV_Handler(void);
void SVC_Handler(void);
void start_first(void);

struct tcb { unsigned *sp; };
struct tcb tcbs[2];
struct tcb *current;
static unsigned stk[2][256] __attribute__((aligned(8)));
void *vtable[16] __attribute__((aligned(256)));
static volatile int a_done, lost;

void switch_context(void)
{
    current = current == &tcbs[0] ? &tcbs[1] : &tcbs[0];
}

void never_here(void)
{
    puts_("start_first returned\n==END==\n");
    for (;;) {}
}

static void fault(void)
{
    puts_("fault\n==END==\n");
    for (;;) {}
}

static void yield(void)
{
    *(volatile unsigned *)0xE000ED04u = 1u << 28;    /* ICSR.PENDSVSET */
    __asm__ volatile("dsb\n\tisb" ::: "memory");
}

/* Each task leaves its own value in s20 -- a register the PendSV handler
 * saves only through the vstmdb/vldmia path -- and checks it after every
 * switch. */
static void taskA(void)
{
    for (int k = 0; k < 3; k++) {
        unsigned v = 0x11110000u + (unsigned)k, back;
        __asm__ volatile("vmov s20, %0" :: "r"(v));
        puts_("A"); putn(k);
        yield();
        __asm__ volatile("vmov %0, s20" : "=r"(back));
        if (back != v) lost++;
    }
    a_done = 1;
    for (;;) yield();
}

static void taskB(void)
{
    for (int k = 0; k < 3; k++) {
        unsigned v = 0x22220000u + (unsigned)k, back;
        __asm__ volatile("vmov s20, %0" :: "r"(v));
        puts_("B"); putn(k);
        yield();
        __asm__ volatile("vmov %0, s20" : "=r"(back));
        if (back != v) lost++;
    }
    while (!a_done) yield();
    puts_("\nlost "); putn(lost);
    puts_("\n==END==\n");
    for (;;) {}
}

static void init_task(int i, void (*fn)(void))
{
    unsigned *sp = &stk[i][256];
    *--sp = 0x01000000u;                     /* xPSR: Thumb */
    *--sp = (unsigned)fn & ~1u;              /* pc */
    *--sp = (unsigned)never_here;            /* lr */
    for (int k = 0; k < 5; k++) *--sp = 0;   /* r12, r3-r0 */
    *--sp = 0xFFFFFFFDu;                     /* EXC_RETURN: thread, PSP */
    for (int k = 0; k < 8; k++) *--sp = 0;   /* r11-r4 */
    tcbs[i].sp = sp;
}

int main(void)
{
    for (int i = 2; i < 16; i++) vtable[i] = (void *)fault;
    vtable[0] = (void *)0x20010000u;
    vtable[1] = (void *)reset;
    vtable[11] = (void *)SVC_Handler;
    vtable[14] = (void *)PendSV_Handler;
    init_task(0, taskA);
    init_task(1, taskB);
    current = &tcbs[0];
    start_first();
    return 0;
}
