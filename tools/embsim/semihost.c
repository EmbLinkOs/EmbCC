/* semihost.c -- ARM semihosting: the calls an image makes with
 * `bkpt 0xab` (operation in r0, parameter block in r1).
 *
 * Console output goes where the UART's does (sim_out), and SYS_WRITE to
 * handle 2 to stderr; SYS_EXIT and SYS_EXIT_EXTENDED end the run with a
 * status; SYS_CLOCK counts the core's estimated cycles, at the MPS2's
 * 25 MHz. The rest are the small calls a C library makes at start-up,
 * answered as a host with nothing to offer would. Memory is the core's
 * (struct semi_ops): an access that faults stops the call, and the core
 * takes the fault. */
#include "sim.h"

int semihost(struct sim *s, const struct semi_ops *m, void *ctx, u32 op,
             u32 arg, u32 *ret)
{
    switch (op) {
    case 0x03:                          /* SYS_WRITEC */
        sim_out(s, (int)m->ld(ctx, arg, 1));
        return 0;
    case 0x04:                          /* SYS_WRITE0 */
        for (u32 a = arg;; a++) {
            u32 c = m->ld(ctx, a, 1);
            if (!c || m->faulted(ctx))
                break;
            sim_out(s, (int)c);
        }
        return 0;
    case 0x05: {                        /* SYS_WRITE */
        u32 fh = m->ld(ctx, arg, 4), buf = m->ld(ctx, arg + 4, 4);
        u32 len = m->ld(ctx, arg + 8, 4);
        for (u32 i = 0; i < len && !m->faulted(ctx); i++) {
            int c = (int)m->ld(ctx, buf + i, 1);
            if (fh == 2)
                fputc(c, stderr);
            else
                sim_out(s, c);
        }
        *ret = 0;
        return 1;
    }
    case 0x01:                          /* SYS_OPEN: only ":tt" */
        *ret = 1;
        return 1;
    case 0x02:                          /* SYS_CLOSE */
    case 0x09:                          /* SYS_ISTTY */
        *ret = op == 9;
        return 1;
    case 0x06:                          /* SYS_READ: nothing to read */
        *ret = m->ld(ctx, arg + 8, 4);
        return 1;
    case 0x07:                          /* SYS_READC: recorded, replayed */
        *ret = (u32)sim_readc(s);
        return 1;
    case 0x0c:
        *ret = 0;
        return 1;
    case 0x10:                          /* SYS_CLOCK: centiseconds */
        *ret = (u32)(s->cycles / 250000);   /* at the MPS2's 25 MHz */
        return 1;
    case 0x11:
        *ret = 0;
        return 1;
    case 0x13:
        *ret = 0;
        return 1;
    case 0x15:                          /* SYS_GET_CMDLINE: empty */
        m->st(ctx, m->ld(ctx, arg, 4), 1, 0);
        m->st(ctx, arg + 4, 4, 0);
        *ret = 0;
        return 1;
    case 0x16: {                        /* SYS_HEAPINFO: unknown */
        u32 blk = m->ld(ctx, arg, 4);
        for (int i = 0; i < 4; i++)
            m->st(ctx, blk + 4 * (u32)i, 4, 0);
        *ret = 0;
        return 1;
    }
    case 0x18:                          /* SYS_EXIT */
        s->exit_status = arg == 0x20026 ? 0 : 1;
        sim_end(s, END_EXIT, "SYS_EXIT (0x%x)", arg);
        return 0;
    case 0x20:                          /* SYS_EXIT_EXTENDED */
        s->exit_status =
            m->ld(ctx, arg, 4) == 0x20026 ? (int)m->ld(ctx, arg + 4, 4) : 1;
        sim_end(s, END_EXIT, "SYS_EXIT_EXTENDED (%d)", s->exit_status);
        return 0;
    }
    *ret = (u32)-1;
    return 1;
}
