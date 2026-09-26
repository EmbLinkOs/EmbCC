/* EmbDBG's live target: a GDB remote serial protocol client.
 *
 * The stub on the other end is QEMU's (`-gdb tcp::PORT`) or OpenOCD's
 * (the same protocol, over JTAG or SWD to a real chip), so one client
 * covers both the emulator the tests run on and the board a user has on
 * the bench. remote.c's header says why this is EmbCC's business at all
 * when the EmbLinkOS debug contract is not built.
 *
 * Deliberately NOT part of embdbg_core.h: EmbLD links embdbg.c for the
 * .embdbg writer and has no business carrying a TCP client.
 */
#ifndef EMBCC_TOOLS_EMBDBG_REMOTE_H
#define EMBCC_TOOLS_EMBDBG_REMOTE_H

#define RSP_MAX 16384

struct rsp {
    int fd;
    int noack;
    char pkt[RSP_MAX + 1];
    unsigned char regbuf[1024];
    int nregbytes;
};

/* Where one register sits in the `g` packet. The layout is the TARGET's,
 * not the protocol's -- a flat concatenation in an order only the stub
 * knows -- which is why these are per-architecture tables. */
struct rsp_regdef {
    const char *name;
    int off;              /* byte offset into the `g` reply */
    int size;             /* 4 or 8 */
};

/* `arch` is one of "x86_64", "aarch64", "arm", "riscv32", "riscv64". */
const struct rsp_regdef *rsp_regs_for(const char *arch);
const char *rsp_pc_name(const char *arch);
const char *rsp_sp_name(const char *arch);
const char *rsp_fp_name(const char *arch);

int  rsp_connect(struct rsp *r, const char *host, const char *port);
void rsp_close(struct rsp *r);

int  rsp_send(struct rsp *r, const char *cmd);
int  rsp_recv(struct rsp *r);

int  rsp_read_regs(struct rsp *r);
int  rsp_reg(struct rsp *r, const struct rsp_regdef *tab, const char *name,
             unsigned long long *out);
int  rsp_read_mem(struct rsp *r, unsigned long long addr, unsigned char *buf,
                  int len);

/* set != 0 inserts a software breakpoint, 0 removes it. Returns 0, -1 on
 * error, or -2 when the stub answered "unsupported". */
int  rsp_break(struct rsp *r, unsigned long long addr, int len, int set);

/* Each returns 0 and sets *sig to the stop signal, or -1 when the target
 * has exited. */
int  rsp_cont(struct rsp *r, int *sig);
int  rsp_step(struct rsp *r, int *sig);
int  rsp_halt_reason(struct rsp *r, int *sig);

#endif
