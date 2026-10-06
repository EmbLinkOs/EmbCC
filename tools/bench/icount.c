/* A QEMU TCG plugin that counts the guest instructions a run executes and
 * writes the total to the file named by its `out=` argument at exit.
 *
 *   cc -shared -fPIC -I$(qemu include dir) -o icount.so icount.c
 *   qemu-system-arm -plugin ./icount.so,out=n.txt ...
 *
 * An instruction count is what tools/bench/run.sh compares between
 * compilers: unlike time under emulation it does not move with the
 * host's load, though it is not a cycle count (a load and an add count
 * the same). Each translation block adds its length when it executes,
 * per vCPU, through an inline counter rather than a callback. Written
 * for the plugin API of QEMU 11 (callbacks take userdata, not an id). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static struct qemu_plugin_scoreboard *counts;
static qemu_plugin_u64 count;
static char out_path[1024];

static void tb_trans(struct qemu_plugin_tb *tb, void *p)
{
    (void)p;
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64, count, qemu_plugin_tb_n_insns(tb));
}

static void at_exit(void *p)
{
    (void)p;
    uint64_t total = qemu_plugin_u64_sum(count);
    FILE *f = out_path[0] ? fopen(out_path, "w") : stderr;
    if (f) {
        fprintf(f, "%llu\n", (unsigned long long)total);
        if (f != stderr)
            fclose(f);
    }
    qemu_plugin_scoreboard_free(counts);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    (void)info;
    for (int i = 0; i < argc; i++)
        if (strncmp(argv[i], "out=", 4) == 0) {
            strncpy(out_path, argv[i] + 4, sizeof out_path - 1);
            out_path[sizeof out_path - 1] = 0;
        }
    counts = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    count = qemu_plugin_scoreboard_u64(counts);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
