/* EmbDBG's live target: a client for the GDB remote serial protocol.
 *
 * WHY THIS, AND WHY IT NEEDS NO KERNEL. EmbDBG v0 is a static reader and
 * crash analyser, and EmbDBG's original design parked live debugging
 * behind the EmbLinkOS kernel's own contract (CAP_DEBUG, syscalls 69-75)
 * — correctly, for a process running on EmbLinkOS. But that is not the
 * only live target this compiler has any more. Firmware is debugged
 * through a STUB that speaks the GDB remote serial protocol: QEMU
 * provides one with `-gdb tcp::PORT`, and OpenOCD provides the same one
 * over JTAG or SWD to a real Cortex-M or RISC-V chip. Neither needs the
 * kernel, and both exist today.
 *
 * So this is the same move §3 made for DWARF: the bridge comes first,
 * because it needs no consumer we have not built. `debug-live.sh`
 * already proves a real gdb can debug EmbCC's output through that stub;
 * this is what lets EmbDBG do it too, on the two targets where gdb is
 * not the tool a user reaches for and where `-g` does not work yet.
 *
 * ---- the protocol, and the three things that bite ---------------------
 *
 * A packet is `$DATA#CC`, CC being the low byte of the sum of DATA, and
 * the receiver answers `+` or `-`. That much is easy. The three that are
 * not:
 *
 *   * REPLIES ARE RUN-LENGTH ENCODED. `x*!` means an `x` followed by
 *     (0x21 - 29) = 4 more of them. A client that does not expand runs
 *     reads a register dump of the wrong length and silently
 *     misattributes every register after the first zero-filled one — and
 *     a register dump is mostly zeroes, so this fires immediately.
 *   * BINARY DATA IS ESCAPED. `}` means the next byte is XORed with
 *     0x20, so a literal `#`, `$`, `}` or `*` survives.
 *   * THE `g` PACKET'S LAYOUT IS THE TARGET'S, not the protocol's. It is
 *     a flat concatenation in an order only the stub knows, which is why
 *     the register tables below are per-architecture and why the test
 *     checks a PC it can predict rather than trusting them.
 */
#define _POSIX_C_SOURCE 200809L
#include "remote.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- register layouts ---------------------------------------------------
 *
 * Where each register sits in the `g` packet, as the stub concatenates
 * them. Only the ones a debugger needs by NAME are listed — the program
 * counter, the stack and frame pointers, and the general registers a
 * user asks to see; `g` still returns everything, and rsp_reg() indexes
 * into it.
 *
 * These are read off the stub's own target description rather than
 * invented, and the golden test pins the one that matters: it sets a
 * breakpoint at an address it computed itself and requires the PC read
 * back through this table to equal it. A wrong offset for `pc` fails
 * there rather than by symbolizing garbage.
 */
#define R(n, off, sz) { n, off, sz }

/* x86-64: rax rbx rcx rdx rsi rdi rbp rsp r8..r15 rip eflags cs ss ds es fs gs */
static const struct rsp_regdef regs_x86_64[] = {
    R("rax", 0, 8),   R("rbx", 8, 8),   R("rcx", 16, 8),  R("rdx", 24, 8),
    R("rsi", 32, 8),  R("rdi", 40, 8),  R("rbp", 48, 8),  R("rsp", 56, 8),
    R("r8", 64, 8),   R("r9", 72, 8),   R("r10", 80, 8),  R("r11", 88, 8),
    R("r12", 96, 8),  R("r13", 104, 8), R("r14", 112, 8), R("r15", 120, 8),
    R("rip", 128, 8), R("eflags", 136, 4),
    { NULL, 0, 0 }
};

/* AArch64: x0..x30, sp, pc, cpsr. */
static const struct rsp_regdef regs_aarch64[] = {
    R("x0", 0, 8),    R("x1", 8, 8),    R("x2", 16, 8),   R("x3", 24, 8),
    R("x4", 32, 8),   R("x5", 40, 8),   R("x6", 48, 8),   R("x7", 56, 8),
    R("x8", 64, 8),   R("x19", 152, 8), R("x20", 160, 8), R("x21", 168, 8),
    R("x29", 232, 8), R("x30", 240, 8),
    R("sp", 248, 8),  R("pc", 256, 8),  R("cpsr", 264, 4),
    { NULL, 0, 0 }
};

/* ARM (M-profile): r0..r12, sp, lr, pc, then xpsr. Sixteen four-byte
 * core registers and no floating-point block between them -- older gdb
 * layouts put eight 12-byte FPA registers before cpsr, and QEMU's
 * org.gnu.gdb.arm.core does not. */
static const struct rsp_regdef regs_arm[] = {
    R("r0", 0, 4),   R("r1", 4, 4),   R("r2", 8, 4),   R("r3", 12, 4),
    R("r4", 16, 4),  R("r5", 20, 4),  R("r6", 24, 4),  R("r7", 28, 4),
    R("r8", 32, 4),  R("r9", 36, 4),  R("r10", 40, 4), R("r11", 44, 4),
    R("r12", 48, 4), R("sp", 52, 4),  R("lr", 56, 4),  R("pc", 60, 4),
    R("xpsr", 64, 4),
    { NULL, 0, 0 }
};

/* RISC-V: x0..x31 then pc, each XLEN bytes. Built at run time because
 * the stride is the only thing that differs between the two widths --
 * writing the table twice would be two places to get `pc` wrong. */
static struct rsp_regdef regs_riscv[36];
static const char *const rv_abi_name[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0",   "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6",   "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

static const struct rsp_regdef *riscv_table(int wb)
{
    int i;
    for (i = 0; i < 32; i++) {
        regs_riscv[i].name = rv_abi_name[i];
        regs_riscv[i].off = i * wb;
        regs_riscv[i].size = wb;
    }
    regs_riscv[32].name = "pc";
    regs_riscv[32].off = 32 * wb;
    regs_riscv[32].size = wb;
    regs_riscv[33].name = NULL;
    return regs_riscv;
}

const struct rsp_regdef *rsp_regs_for(const char *arch)
{
    if (strcmp(arch, "aarch64") == 0) return regs_aarch64;
    if (strcmp(arch, "arm") == 0)     return regs_arm;
    if (strcmp(arch, "riscv32") == 0) return riscv_table(4);
    if (strcmp(arch, "riscv64") == 0) return riscv_table(8);
    return regs_x86_64;
}

/* The three a debugger needs whatever the machine is called. */
const char *rsp_pc_name(const char *arch)
{
    if (strcmp(arch, "x86_64") == 0) return "rip";
    return "pc";
}
const char *rsp_sp_name(const char *arch)
{
    if (strcmp(arch, "x86_64") == 0) return "rsp";
    return "sp";
}
const char *rsp_fp_name(const char *arch)
{
    if (strcmp(arch, "x86_64") == 0)  return "rbp";
    if (strcmp(arch, "aarch64") == 0) return "x29";
    if (strcmp(arch, "arm") == 0)     return "r7";
    return "s0";                      /* RISC-V's frame pointer is x8 */
}

/* ---- the wire ----------------------------------------------------------- */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int read_byte(struct rsp *r)
{
    unsigned char c;
    for (;;) {
        ssize_t n = read(r->fd, &c, 1);
        if (n == 1) return c;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
}

int rsp_send(struct rsp *r, const char *cmd)
{
    char buf[RSP_MAX + 8];
    unsigned sum = 0;
    size_t n = strlen(cmd), i;
    if (n + 5 > sizeof buf) return -1;
    for (i = 0; i < n; i++) sum += (unsigned char)cmd[i];
    snprintf(buf, sizeof buf, "$%s#%02x", cmd, sum & 0xff);
    if (write_all(r->fd, buf, strlen(buf)) < 0) return -1;
    if (r->noack) return 0;
    /* The stub acks before replying. A `-` means it saw a bad checksum,
     * which for a client that computes it correctly means the link is
     * broken rather than the packet; say so instead of looping. */
    for (;;) {
        int c = read_byte(r);
        if (c < 0) return -1;
        if (c == '+') return 0;
        if (c == '-') return -1;
        /* Anything else is a stray notification; keep looking. */
    }
}

/* Receive one packet into r->pkt, expanding run-length encoding and
 * un-escaping. Returns its length, or -1. */
int rsp_recv(struct rsp *r)
{
    int c;
    unsigned sum = 0;
    int n = 0;

    do {
        c = read_byte(r);
        if (c < 0) return -1;
        /* `%` introduces an asynchronous notification, which this client
         * does not use; skip to the end of it. */
        if (c == '%') {
            while ((c = read_byte(r)) >= 0 && c != '#')
                ;
            read_byte(r); read_byte(r);
            c = 0;
        }
    } while (c != '$');

    for (;;) {
        c = read_byte(r);
        if (c < 0) return -1;
        if (c == '#') break;
        sum += (unsigned)c;
        if (c == '}') {
            /* Escaped: the next byte is the real one, XOR 0x20. */
            int e = read_byte(r);
            if (e < 0) return -1;
            sum += (unsigned)e;
            c = e ^ 0x20;
        } else if (c == '*') {
            /* A RUN: the previous character repeated (this one - 29)
             * more times. Reply packets are mostly zeroes, so a client
             * that skips this reads short and every register after the
             * first run is the wrong one. */
            int rep = read_byte(r);
            if (rep < 0 || n == 0) return -1;
            sum += (unsigned)rep;
            int count = rep - 29;
            char prev = r->pkt[n - 1];
            while (count-- > 0 && n < RSP_MAX) r->pkt[n++] = prev;
            continue;
        }
        if (n < RSP_MAX) r->pkt[n++] = (char)c;
    }
    {
        int h = read_byte(r), l = read_byte(r);
        if (h < 0 || l < 0) return -1;
        unsigned want = (unsigned)((hexval(h) << 4) | hexval(l));
        if ((sum & 0xff) != want) {
            if (!r->noack) write_all(r->fd, "-", 1);
            return -1;
        }
    }
    if (!r->noack && write_all(r->fd, "+", 1) < 0) return -1;
    r->pkt[n] = 0;
    return n;
}

static int rsp_xchg(struct rsp *r, const char *cmd)
{
    if (rsp_send(r, cmd) < 0) return -1;
    return rsp_recv(r);
}

int rsp_connect(struct rsp *r, const char *host, const char *port)
{
    struct addrinfo hints, *res, *a;
    int fd = -1;

    memset(r, 0, sizeof *r);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0)
        return -1;
    for (a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;
    /* Nagle would sit on every one-packet request waiting for a second;
     * this protocol is a strict request/response ping-pong and every
     * exchange would pay the delay. */
    {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    r->fd = fd;

    /* The handshake is optional -- every stub answers the packets below
     * without it -- but it is what tells us the maximum packet size, and
     * asking is how a client finds out rather than assuming. */
    if (rsp_xchg(r, "qSupported:multiprocess-;swbreak+;hwbreak+") < 0) {
        close(fd);
        return -1;
    }
    return 0;
}

void rsp_close(struct rsp *r)
{
    if (r->fd >= 0) {
        rsp_send(r, "D");          /* detach: let the target run on */
        close(r->fd);
        r->fd = -1;
    }
}

/* ---- registers and memory ------------------------------------------------ */

int rsp_read_regs(struct rsp *r)
{
    int n = rsp_xchg(r, "g");
    if (n <= 0 || r->pkt[0] == 'E') return -1;
    r->nregbytes = 0;
    for (int i = 0; i + 1 < n && r->nregbytes < (int)sizeof r->regbuf; i += 2) {
        int h = hexval((unsigned char)r->pkt[i]);
        int l = hexval((unsigned char)r->pkt[i + 1]);
        if (h < 0 || l < 0) break;
        r->regbuf[r->nregbytes++] = (unsigned char)((h << 4) | l);
    }
    return r->nregbytes;
}

int rsp_reg(struct rsp *r, const struct rsp_regdef *tab, const char *name,
            unsigned long long *out)
{
    for (const struct rsp_regdef *d = tab; d->name; d++) {
        if (strcmp(d->name, name) != 0) continue;
        if (d->off + d->size > r->nregbytes) return -1;
        unsigned long long v = 0;
        /* Little-endian: every target here is. */
        for (int i = d->size - 1; i >= 0; i--)
            v = (v << 8) | r->regbuf[d->off + i];
        *out = v;
        return 0;
    }
    return -1;
}

int rsp_read_mem(struct rsp *r, unsigned long long addr, unsigned char *buf,
                 int len)
{
    char cmd[64];
    int got = 0;
    while (got < len) {
        int want = len - got;
        if (want > 256) want = 256;      /* well inside any stub's limit */
        snprintf(cmd, sizeof cmd, "m%llx,%x",
                 (unsigned long long)(addr + (unsigned)got), (unsigned)want);
        int n = rsp_xchg(r, cmd);
        if (n <= 0 || r->pkt[0] == 'E') return got;
        int k = 0;
        for (int i = 0; i + 1 < n && k < want; i += 2) {
            int h = hexval((unsigned char)r->pkt[i]);
            int l = hexval((unsigned char)r->pkt[i + 1]);
            if (h < 0 || l < 0) break;
            buf[got + k] = (unsigned char)((h << 4) | l);
            k++;
        }
        if (k == 0) return got;
        got += k;
    }
    return got;
}

/* ---- execution ----------------------------------------------------------- */

int rsp_break(struct rsp *r, unsigned long long addr, int len, int set)
{
    char cmd[64];
    /* Z0 is a SOFTWARE breakpoint, which a stub implements by writing a
     * trap instruction -- so `len` is the length of the instruction it
     * replaces, and getting it wrong on a variable-length machine leaves
     * the target corrupted. QEMU accepts the architecture's minimum. */
    snprintf(cmd, sizeof cmd, "%c0,%llx,%x", set ? 'Z' : 'z', addr,
             (unsigned)len);
    int n = rsp_xchg(r, cmd);
    if (n < 0) return -1;
    if (n == 0) return -2;                 /* empty: stub cannot do it */
    return strcmp(r->pkt, "OK") == 0 ? 0 : -1;
}

/* Run, and describe why we stopped. A stop reply is `S AA` or
 * `T AA key:value;...`; the signal is what a caller reports, and the
 * keys carry register values this client re-reads with `g` instead --
 * one path to a register rather than two that can disagree. */
static int run_and_wait(struct rsp *r, const char *cmd, int *sig)
{
    if (rsp_send(r, cmd) < 0) return -1;
    for (;;) {
        int n = rsp_recv(r);
        if (n < 0) return -1;
        if (n == 0) continue;
        switch (r->pkt[0]) {
        case 'O':                          /* console output from the target */
            continue;
        case 'S': case 'T':
            if (sig && n >= 3) {
                int h = hexval((unsigned char)r->pkt[1]);
                int l = hexval((unsigned char)r->pkt[2]);
                *sig = (h >= 0 && l >= 0) ? (h << 4) | l : 0;
            }
            return 0;
        case 'W': case 'X':                /* the target exited */
            if (sig) *sig = -1;
            return 0;
        default:
            continue;
        }
    }
}

int rsp_cont(struct rsp *r, int *sig) { return run_and_wait(r, "c", sig); }
int rsp_step(struct rsp *r, int *sig) { return run_and_wait(r, "s", sig); }

int rsp_halt_reason(struct rsp *r, int *sig)
{
    int n = rsp_xchg(r, "?");
    if (n < 0) return -1;
    if (sig && n >= 3 && (r->pkt[0] == 'S' || r->pkt[0] == 'T')) {
        int h = hexval((unsigned char)r->pkt[1]);
        int l = hexval((unsigned char)r->pkt[2]);
        *sig = (h >= 0 && l >= 0) ? (h << 4) | l : 0;
    }
    return 0;
}
