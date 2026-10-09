/* sifive-test.c -- the SiFive test device of QEMU's virt board, at
 * 0x100000: a store of 0x5555 ends the run with status 0, 0x3333 with
 * the status in the upper halfword, and 0x7777 resets the machine --
 * what tests/harness/riscv's boot.c and its drivers write when main
 * returns. */
#include <stdlib.h>

#include "devices.h"

struct sifive_test {
    struct sim *sim;
};

static void test_write(void *ctx, u32 off, int n, u32 v)
{
    struct sim *s = ((struct sifive_test *)ctx)->sim;
    if (off != 0 || n < 2 || s->state != RUN)
        return;
    switch (v & 0xffff) {
    case 0x5555:
        s->exit_status = 0;
        sim_end(s, END_EXIT, "the test device: pass");
        return;
    case 0x3333:
        s->exit_status = (int)(v >> 16);
        sim_end(s, END_EXIT, "the test device: fail (%u)", v >> 16);
        return;
    case 0x7777:
        sim_reset(s, 0);
        return;
    }
}

const struct dev_ops sifive_test_ops = {
    "sifive-test", 0, test_write, 0, 0, 0,
};

void *sifive_test_create(struct sim *s, const struct dev_desc *d)
{
    struct sifive_test *t = calloc(1, sizeof *t);
    (void)d;
    if (!t)
        die("out of memory");
    t->sim = s;
    return t;
}
