/* A QEMU TCG plugin that counts the guest instructions a run executes and
 * estimates the cycles they would take, and writes both to the file
 * named by its `out=` argument at exit: the count on the first line, the
 * estimate on the second.
 *
 *   cc -shared -fPIC -I$(qemu include dir) -o icount.so icount.c
 *   qemu-system-arm -plugin ./icount.so,out=n.txt ...
 *
 * An instruction count is what tools/bench/run.sh compares between
 * compilers: unlike time under emulation it does not move with the
 * host's load. But it is not a cycle count, and the difference decides
 * real questions: an IT block executes both of its moves where a branch
 * skips one, so by count a branch always wins, while on a Cortex-M4 the
 * taken branch costs two cycles more than the move it skipped; and a
 * divide counts one where it takes up to twelve cycles. So each
 * instruction also gets a cost:
 *
 *   Cortex-M4 (the Technical Reference Manual's table, simplified): 1,
 *   a load 2, a load or store multiple (push, pop) 1 + the registers, a
 *   load or store pair 3, sdiv/udiv 7 (2 to 12 by the operands), vdiv
 *   and vsqrt 14; and a taken branch 2 more (the pipeline refill).
 *
 *   RV32 (no one core: a plain in-order pipeline): 1, a load 2, a
 *   divide or remainder 16, and a taken branch or jump 2 more.
 *
 * Neither is a simulator -- there are no wait states, no load-use
 * stalls, no flash -- but both charge the things a compiler chooses
 * between. Static costs are added per translation block, inline, as the
 * count is; whether a block's last branch was taken is seen from where
 * the next block starts, by a callback per block executed. Written for
 * the plugin API of QEMU 11 (callbacks take userdata, not an id). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static struct qemu_plugin_scoreboard *counts, *costs;
static qemu_plugin_u64 count, cost;
static char out_path[1024];
static int is_arm;

/* What a block ends in, for the next one to judge: whether its last
 * instruction can branch, and where execution goes when it does not. */
struct tb_end {
    uint64_t start, fall;
    int branch;
};
static const struct tb_end *prev_tb;
static uint64_t taken;

static int popcount16(unsigned v)
{
    int n = 0;
    for (v &= 0xffff; v; v &= v - 1)
        n++;
    return n;
}

/* A Thumb instruction's cost and whether it may branch. */
static int arm_cost(const uint8_t *p, size_t n, int *branch)
{
    unsigned h1 = p[0] | p[1] << 8;
    *branch = 0;
    if (n == 2) {
        if ((h1 & 0xff00) == 0x4700) {                  /* bx, blx */
            *branch = 1;
            return 1;
        }
        if ((h1 & 0xf800) == 0x4800)                    /* ldr literal */
            return 2;
        if ((h1 & 0xf000) == 0x5000)                    /* ld/st register */
            return ((h1 >> 9) & 7) >= 3 ? 2 : 1;
        if ((h1 & 0xe000) == 0x6000 || (h1 & 0xf000) == 0x8000 ||
            (h1 & 0xf000) == 0x9000)                    /* ld/st immediate */
            return h1 & 0x0800 ? 2 : 1;
        if ((h1 & 0xfe00) == 0xb400)                    /* push */
            return 1 + popcount16(h1 & 0x1ff);
        if ((h1 & 0xfe00) == 0xbc00) {                  /* pop */
            *branch = (h1 & 0x100) != 0;
            return 1 + popcount16(h1 & 0x1ff);
        }
        if ((h1 & 0xf000) == 0xc000)                    /* stm, ldm */
            return 1 + popcount16(h1 & 0xff);
        if ((h1 & 0xf500) == 0xb100) {                  /* cbz, cbnz */
            *branch = 1;
            return 1;
        }
        if ((h1 & 0xf000) == 0xd000 && (h1 & 0x0e00) != 0x0e00) {
            *branch = 1;                                /* b<cond> */
            return 1;
        }
        if ((h1 & 0xf800) == 0xe000) {                  /* b */
            *branch = 1;
            return 1;
        }
        return 1;
    }
    unsigned h2 = p[2] | p[3] << 8;
    if ((h1 & 0xf800) == 0xf000 && (h2 & 0x8000)) {
        /* b, bl and the miscellaneous control space; only the branches
         * have bit 12 or a condition outside 111x */
        if ((h2 & 0x5000) || (h1 & 0x0380) != 0x0380)
            *branch = 1;
        return 1;
    }
    if ((h1 & 0xfff0) == 0xe8d0 && (h2 & 0xffe0) == 0xf000) {
        *branch = 1;                                    /* tbb, tbh */
        return 2;
    }
    if ((h1 & 0xfe40) == 0xe800) {                      /* ldm, stm */
        if ((h1 & 0x0010) && (h2 & 0x8000))
            *branch = 1;                                /* pop.w {.., pc} */
        return 1 + popcount16(h2);
    }
    if ((h1 & 0xfff0) == 0xe850)                        /* ldrex */
        return 2;
    if ((h1 & 0xfe40) == 0xe840 && (h1 & 0x0120))       /* ldrd, strd */
        return 3;
    if ((h1 & 0xfe00) == 0xf800) {                      /* ld/st single */
        if ((h1 & 0x0010) && (h2 & 0xf000) == 0xf000 &&
            ((h1 >> 5) & 3) == 2)
            *branch = 1;                                /* ldr pc */
        return h1 & 0x0010 ? 2 : 1;
    }
    if ((h1 & 0xffd0) == 0xfb90)                        /* sdiv, udiv */
        return 7;
    if ((h1 & 0xfe10) == 0xec10 && (h1 & 0x0100))       /* vldr, vldm */
        return 2;
    if ((h1 & 0xffb0) == 0xee80 && (h2 & 0x0e50) == 0x0a00)
        return 14;                                      /* vdiv */
    if ((h1 & 0xffbf) == 0xeeb1 && (h2 & 0x0ed0) == 0x0ac0)
        return 14;                                      /* vsqrt */
    return 1;
}

/* A RISC-V instruction's cost and whether it may branch (with C). */
static int rv_cost(const uint8_t *p, size_t n, int *branch)
{
    *branch = 0;
    if (n == 2) {
        unsigned h = p[0] | p[1] << 8, q = h & 3, f3 = h >> 13;
        if (q == 0)
            return f3 == 2 || f3 == 3 ? 2 : 1;          /* c.lw / c.flw */
        if (q == 1) {
            if (f3 == 1 || f3 == 5 || f3 == 6 || f3 == 7)
                *branch = 1;                            /* c.jal c.j c.b*z */
            return 1;
        }
        if (f3 == 2 || f3 == 3)                         /* c.lwsp */
            return 2;
        if (f3 == 4 && ((h >> 2) & 0x1f) == 0 && ((h >> 7) & 0x1f))
            *branch = 1;                                /* c.jr, c.jalr */
        return 1;
    }
    uint32_t w = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
    unsigned op = w & 0x7f;
    if (op == 0x03 || op == 0x07)                       /* loads */
        return 2;
    if (op == 0x63 || op == 0x6f || op == 0x67) {       /* b*, jal, jalr */
        *branch = 1;
        return 1;
    }
    if (op == 0x33 && (w >> 25) == 1 && ((w >> 12) & 7) >= 4)
        return 16;                                      /* div, rem */
    return 1;
}

static void tb_exec(unsigned int vcpu, void *ud)
{
    (void)vcpu;
    const struct tb_end *e = ud;
    if (prev_tb && prev_tb->branch && e->start != prev_tb->fall)
        taken++;
    prev_tb = e;
}

static void tb_trans(struct qemu_plugin_tb *tb, void *p)
{
    (void)p;
    size_t n = qemu_plugin_tb_n_insns(tb);
    uint64_t c = 0;
    struct tb_end *e = calloc(1, sizeof *e);
    e->start = qemu_plugin_tb_vaddr(tb);
    for (size_t k = 0; k < n; k++) {
        struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, k);
        uint8_t b[4] = { 0, 0, 0, 0 };
        size_t sz = qemu_plugin_insn_size(in);
        qemu_plugin_insn_data(in, b, sz < 4 ? sz : 4);
        int br;
        c += is_arm ? arm_cost(b, sz, &br) : rv_cost(b, sz, &br);
        if (k == n - 1) {
            e->branch = br;
            e->fall = qemu_plugin_insn_vaddr(in) + sz;
        }
    }
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64, count, n);
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64, cost, c);
    qemu_plugin_register_vcpu_tb_exec_cb(tb, tb_exec, QEMU_PLUGIN_CB_NO_REGS,
                                         e);
}

static void at_exit(void *p)
{
    (void)p;
    uint64_t total = qemu_plugin_u64_sum(count);
    uint64_t cyc = qemu_plugin_u64_sum(cost) + 2 * taken;
    FILE *f = out_path[0] ? fopen(out_path, "w") : stderr;
    if (f) {
        fprintf(f, "%llu\n%llu\n", (unsigned long long)total,
                (unsigned long long)cyc);
        if (f != stderr)
            fclose(f);
    }
    qemu_plugin_scoreboard_free(counts);
    qemu_plugin_scoreboard_free(costs);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    is_arm = info && info->target_name &&
             strncmp(info->target_name, "arm", 3) == 0;
    for (int i = 0; i < argc; i++)
        if (strncmp(argv[i], "out=", 4) == 0) {
            strncpy(out_path, argv[i] + 4, sizeof out_path - 1);
            out_path[sizeof out_path - 1] = 0;
        }
    counts = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    count = qemu_plugin_scoreboard_u64(counts);
    costs = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    cost = qemu_plugin_scoreboard_u64(costs);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
