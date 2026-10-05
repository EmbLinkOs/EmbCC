/* A task-notify routine in FreeRTOS's shape (xTaskGenericNotifyFromISR):
 * one task pointer read and written by nearly every statement, five more
 * values live across calls, a switch, and a list unlinked and relinked.
 *
 * On Cortex-M the callee-saved registers are renamed after allocation so
 * that the busiest value -- the task pointer -- is in a low register and
 * its loads and stores take the 16-bit forms
 * (tests/golden/thumb-lowregs.sh checks that). Renaming every use of a
 * register is only right if it is EVERY use, so this runs each action and
 * checks every field and every link. */
// expect-exit: 42
int printf(const char *fmt, ...);

struct item { struct item *next, *prev; void *owner; int value; };
struct tcb {
    int state;
    struct item gen;
    struct item ev;
    int prio;
    unsigned notify[2];
    unsigned char nstate[2];
};

volatile unsigned top_ready;
struct item ready[8];
int sched_suspended, yield_pending, asserts, basepri, raised;

__attribute__((noinline)) void assert_fail(const char *f, int line)
{
    (void)f;
    asserts += line;
}
__attribute__((noinline)) unsigned raise_basepri(void)
{
    unsigned old = (unsigned)basepri;
    basepri = 0xa0;
    raised++;
    return old;
}
__attribute__((noinline)) void restore_basepri(unsigned v) { basepri = (int)v; }

__attribute__((noinline))
int notify_isr(struct tcb *t, unsigned idx, unsigned val, int action,
               unsigned *prev, int *woken)
{
    int ret = 1;
    if (!t) assert_fail("t", 1);
    if (idx >= 2) assert_fail("idx", 2);
    unsigned saved = raise_basepri();
    if (prev) *prev = t->notify[idx];
    unsigned char old = t->nstate[idx];
    t->nstate[idx] = 2;
    switch (action) {
    case 1: t->notify[idx] |= val; break;
    case 2: t->notify[idx]++; break;
    case 3: t->notify[idx] = val; break;
    case 4: if (old != 2) t->notify[idx] = val; else ret = 0; break;
    default: break;
    }
    if (old == 1) {
        if (t->ev.owner) assert_fail("ev", 3);
        if (!sched_suspended) {
            t->gen.prev->next = t->gen.next;
            t->gen.next->prev = t->gen.prev;
            t->gen.owner = 0;
            top_ready |= 1u << t->prio;
            struct item *l = &ready[t->prio];
            t->gen.next = l;
            t->gen.prev = l->prev;
            l->prev->next = &t->gen;
            l->prev = &t->gen;
            t->gen.owner = l;
            l->value++;
        }
        if (t->prio > 3) {
            if (woken) *woken = 1;
            yield_pending = 1;
        }
    }
    restore_basepri(saved);
    return ret;
}

static struct item waitlist;

static void park(struct tcb *t, int prio)
{
    /* t alone on `waitlist`, waiting (state 1) on notification 1 */
    waitlist.next = waitlist.prev = &t->gen;
    t->gen.next = t->gen.prev = &waitlist;
    t->gen.owner = &waitlist;
    t->ev.owner = 0;
    t->prio = prio;
    t->nstate[1] = 1;
}

int main(void)
{
    struct tcb t = { 0 };
    int bad = 0, woken = 0;
    unsigned prev = 99;
    for (int i = 0; i < 8; i++)
        ready[i].next = ready[i].prev = &ready[i];
    basepri = 5;

    t.notify[0] = 0x10;
    if (notify_isr(&t, 0, 0x03, 1, &prev, &woken) != 1 || t.notify[0] != 0x13 ||
        prev != 0x10 || t.nstate[0] != 2) { printf("or\n"); bad++; }
    if (notify_isr(&t, 0, 0, 2, 0, 0) != 1 || t.notify[0] != 0x14) { printf("inc\n"); bad++; }
    if (notify_isr(&t, 0, 7, 3, &prev, 0) != 1 || t.notify[0] != 7 || prev != 0x14) {
        printf("set\n"); bad++;
    }
    /* overwrite refused: nstate[0] is already 2 */
    if (notify_isr(&t, 0, 9, 4, 0, 0) != 0 || t.notify[0] != 7) { printf("nooverwrite\n"); bad++; }
    if (basepri != 5 || raised != 4) { printf("basepri %d %d\n", basepri, raised); bad++; }

    park(&t, 5);
    if (notify_isr(&t, 1, 0x40, 3, &prev, &woken) != 1 || t.notify[1] != 0x40) {
        printf("wake value\n"); bad++;
    }
    if (ready[5].next != &t.gen || ready[5].prev != &t.gen || t.gen.next != &ready[5] ||
        t.gen.prev != &ready[5] || t.gen.owner != &ready[5] || ready[5].value != 1) {
        printf("relink\n"); bad++;
    }
    if (waitlist.next != &waitlist || waitlist.prev != &waitlist) { printf("unlink\n"); bad++; }
    if (top_ready != 1u << 5 || !woken || !yield_pending) { printf("ready bits\n"); bad++; }

    sched_suspended = 1;
    yield_pending = 0; woken = 0;
    park(&t, 2);
    if (notify_isr(&t, 1, 0, 2, 0, &woken) != 1 || t.notify[1] != 0x41) { printf("susp inc\n"); bad++; }
    if (waitlist.next != &t.gen || woken || yield_pending) { printf("suspended moved it\n"); bad++; }
    if (asserts != 0) { printf("asserts %d\n", asserts); bad++; }

    if (bad) { printf("lowregs-notify: %d failures\n", bad); return 1; }
    return 42;
}
