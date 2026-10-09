/* The TriCore harness's UART: a QEMU TCG plugin.
 *
 * QEMU's tricore_testboard has RAM and a test device that ends the run,
 * and no serial port at all. So the harness's putchar stores each byte
 * to one word of the board's internal data RAM that nothing else uses
 * (io.c's UART_TX, 0xd000bff0), and this plugin watches stores to that
 * address and writes each byte to the host's standard output.
 *
 *   cc -shared -fPIC -I$(qemu include dir) -o putc.so putc.c
 *   qemu-system-tricore -M tricore_testboard -plugin ./putc.so ...
 *   (-plugin ./putc.so,addr=0x... watches another address)
 *
 * Only the byte stores are instrumented -- an instruction whose primary
 * opcode is one of TriCore's ST.B forms (BOL, BO, ABS, and the 16-bit
 * SRO/SSR/SSRO/SSR-postincrement ones) -- so every other instruction runs
 * at full speed. Each byte goes out with write(2) at once, unbuffered,
 * because the run ends by the test device calling exit() from inside
 * QEMU, and nothing buffered here would be flushed. Written for the
 * plugin API of QEMU 11. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t uart = 0xd000bff0;

static void on_store(unsigned int vcpu, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    (void)vcpu; (void)udata;
    if (vaddr != uart || !qemu_plugin_mem_is_store(info))
        return;
    {
        qemu_plugin_mem_value v = qemu_plugin_mem_get_value(info);
        unsigned char c = (unsigned char)v.data.u8;
        if (write(1, &c, 1) < 0)
            return;
    }
}

static int byte_store(uint8_t op1)
{
    switch (op1) {
    case 0xe9:          /* ST.B, BOL: long offset */
    case 0x89:          /* the BO stores: ST.B among them */
    case 0x25:          /* ST.B / ST.H, ABS */
    case 0x2c:          /* ST.B, SRO */
    case 0x34:          /* ST.B, SSR */
    case 0x24:          /* ST.B, SSR post-increment */
    case 0x28:          /* ST.B, SSRO */
        return 1;
    default:
        return 0;
    }
}

static void tb_trans(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    (void)udata;
    for (size_t k = 0; k < n; k++) {
        struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, k);
        uint8_t b[4] = { 0, 0, 0, 0 };
        size_t sz = qemu_plugin_insn_size(in);
        qemu_plugin_insn_data(in, b, sz < 4 ? sz : 4);
        if (byte_store(b[0]))
            qemu_plugin_register_vcpu_mem_cb(in, on_store,
                                             QEMU_PLUGIN_CB_NO_REGS,
                                             QEMU_PLUGIN_MEM_W, NULL);
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    (void)info;
    for (int i = 0; i < argc; i++)
        if (!strncmp(argv[i], "addr=", 5))
            uart = strtoull(argv[i] + 5, NULL, 0);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    return 0;
}
