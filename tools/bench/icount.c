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
 * instruction also gets a cost, from the table in cost.h (which EmbSim
 * shares, so the simulator and the plugin charge the same).
 *
 * Neither model is cycle-accurate -- there are no wait states, no load-use
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

#include "cost.h"

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
