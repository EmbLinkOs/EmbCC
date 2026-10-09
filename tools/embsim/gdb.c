/* gdb.c -- the GDB remote serial protocol server: `embsim IMAGE --gdb
 * PORT`, so gdb, lldb and embdbg debug firmware in the simulator.
 *
 * It answers as QEMU's stub does -- the same register numbers, the same
 * `g` packet, the same stop replies -- so a debugger sees the same
 * machine on either, and tests/golden/embsim-gdb.sh holds the two to the
 * same transcript. Beyond QEMU's:
 *   - a memory map (qXfer:memory-map:read) from the board's regions, so
 *     gdb's `load` programs flash (vFlashErase, vFlashWrite) and its
 *     breakpoints there are hardware ones;
 *   - `monitor` commands for the simulator: reset, stats, insns, cycles;
 *   - QStartNoAckMode.
 *
 * The server is part of the run loop. While the target runs, the socket
 * is looked at every 16384 instructions for the interrupt (0x03), so a
 * debugger can stop it without the run paying for a system call per
 * instruction. Breakpoints are checked before each instruction, and
 * watchpoints by the bus on each of the core's accesses; like QEMU's, a
 * watchpoint stops the target after the instruction that made the
 * access. Time is the core's cycles, as without a debugger: a run that
 * stops at a breakpoint and continues gives the same counts as one that
 * did not stop.
 *
 * Breakpoints are kept here, not written into memory, so one in flash
 * works and the image is never changed. */
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "sim.h"

#define PKT_MAX 0x4000                  /* the PacketSize we announce */
#define NBP 64

enum { STOP_TRAP, STOP_INT, STOP_WATCH, STOP_LOCKUP, STOP_END, STOP_LOST };
enum { END_DETACH, END_KILL, END_EXITED, END_LOST };

struct gdb {
    struct sim *s;
    int fd;
    int noack, multiprocess;
    u8 in[4096];
    int ilen, ipos;
    char pkt[PKT_MAX + 1];              /* the packet received, unescaped */
    int plen;
    char out[2 * PKT_MAX + 64];         /* a packet as it is sent */
    char reply[PKT_MAX + 64];           /* a reply's text */
    int stop_sig;                       /* the last stop, for `?` */
    char stop_extra[48];
    struct { u32 addr; int type; } bp[NBP];
    int nbp;
};

static const char hexd[] = "0123456789abcdef";

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* a hexadecimal number at *p, which moves past it */
static u32 hexnum(const char **p)
{
    u32 v = 0;
    int d;
    while ((d = hexval((unsigned char)**p)) >= 0) {
        v = v << 4 | (u32)d;
        (*p)++;
    }
    return v;
}

/* ---- the wire ---------------------------------------------------------- */

/* the next byte from the debugger, or -1 when it has gone */
static int getbyte(struct gdb *g)
{
    if (g->ipos == g->ilen) {
        int n = net_read(g->fd, g->in, (int)sizeof g->in);
        if (n <= 0)
            return -1;
        g->ilen = n;
        g->ipos = 0;
    }
    return g->in[g->ipos++];
}

/* a byte waiting now, without blocking: the byte, -1 when none is, -2
 * when the debugger has gone */
static int pollbyte(struct gdb *g)
{
    if (g->ipos == g->ilen && !net_ready(g->fd, 0))
        return -1;
    int c = getbyte(g);
    return c < 0 ? -2 : c;
}

static int send_raw(struct gdb *g, const char *data, int n)
{
    char *b = g->out;
    unsigned sum = 0;
    int k = 0;
    b[k++] = '$';
    for (int i = 0; i < n; i++) {
        u8 c = (u8)data[i];
        if (c == '$' || c == '#' || c == '}' || c == '*') {
            b[k++] = '}';
            sum += '}';
            c ^= 0x20;
        }
        b[k++] = (char)c;
        sum += c;
    }
    b[k++] = '#';
    b[k++] = hexd[(sum >> 4) & 15];
    b[k++] = hexd[sum & 15];
    for (;;) {
        if (net_write(g->fd, b, k) < 0)
            return -1;
        if (g->noack)
            return 0;
        int c = getbyte(g);
        while (c >= 0 && c != '+' && c != '-')
            c = getbyte(g);
        if (c < 0)
            return -1;
        if (c == '+')
            return 0;
    }
}

static int send(struct gdb *g, const char *s)
{
    return send_raw(g, s, (int)strlen(s));
}

/* Receive a packet into g->pkt: its length, or -1 when the debugger has
 * gone. Acknowledges it unless acknowledgements are off. */
static int recv_packet(struct gdb *g)
{
    for (;;) {
        int c = getbyte(g);
        while (c >= 0 && c != '$')
            c = getbyte(g);
        if (c < 0)
            return -1;
        unsigned sum = 0;
        int n = 0;
        for (;;) {
            c = getbyte(g);
            if (c < 0)
                return -1;
            if (c == '#')
                break;
            sum += (unsigned)c;
            if (c == '}') {
                c = getbyte(g);
                if (c < 0)
                    return -1;
                sum += (unsigned)c;
                c ^= 0x20;
            }
            if (n < PKT_MAX)
                g->pkt[n++] = (char)c;
        }
        int h = getbyte(g), l = getbyte(g);
        if (h < 0 || l < 0)
            return -1;
        g->pkt[n] = 0;
        g->plen = n;
        if (g->noack)
            return n;
        if ((unsigned)(hexval(h) << 4 | hexval(l)) == (sum & 0xff)) {
            if (net_write(g->fd, "+", 1) < 0)
                return -1;
            return n;
        }
        if (net_write(g->fd, "-", 1) < 0)
            return -1;
    }
}

/* console output for a monitor command: O packets, a line at most each */
static void mon_out(struct gdb *g, const char *text)
{
    char buf[512];
    while (*text) {
        int k = 0;
        buf[k++] = 'O';
        while (*text && k < (int)sizeof buf - 3) {
            int nl = *text == '\n';
            buf[k++] = hexd[(u8)*text >> 4];
            buf[k++] = hexd[(u8)*text & 15];
            text++;
            if (nl)
                break;
        }
        buf[k] = 0;
        send(g, buf);
    }
}

/* ---- the target -------------------------------------------------------- */

static const char *thread(struct gdb *g)
{
    return g->multiprocess ? "p01.01" : "01";
}

static void set_stop(struct gdb *g, int sig, const char *extra)
{
    g->stop_sig = sig;
    snprintf(g->stop_extra, sizeof g->stop_extra, "%s", extra);
}

/* T SIG thread:ID; and what else the stop says, in the thread form the
 * debugger asked for */
static void send_stop(struct gdb *g)
{
    char b[96];
    snprintf(b, sizeof b, "T%02xthread:%s;%s", g->stop_sig, thread(g),
             g->stop_extra);
    send(g, b);
}

static int bp_at(struct gdb *g, u32 pc)
{
    for (int i = 0; i < g->nbp; i++)
        if (g->bp[i].addr == pc)
            return 1;
    return 0;
}

static int bp_set(struct gdb *g, int type, u32 addr, int insert)
{
    for (int i = 0; i < g->nbp; i++)
        if (g->bp[i].addr == addr && g->bp[i].type == type) {
            if (!insert)
                g->bp[i] = g->bp[--g->nbp];
            return 0;
        }
    if (!insert)
        return 0;
    if (g->nbp == NBP)
        return -1;
    g->bp[g->nbp].addr = addr;
    g->bp[g->nbp].type = type;
    g->nbp++;
    return 0;
}

/* Run (or step one instruction) until something stops the target. */
static int resume(struct gdb *g, int step)
{
    struct sim *s = g->s;
    struct cpu *c = s->cpu;
    unsigned n = 0;
    if (s->state == END_LOCKUP)
        return STOP_LOCKUP;
    if (s->state != RUN)
        return STOP_END;
    s->bus.watch_hit = 0;
    for (;;) {
        if (!step && g->nbp && bp_at(g, c->ops->pc(c)))
            return STOP_TRAP;
        sim_step(s);
        if (s->bus.watch_hit)
            return STOP_WATCH;
        if (s->state == END_IDLE) {
            /* Nothing can interrupt the core: as a part asleep in WFI,
             * it waits for the debugger. */
            s->state = RUN;
            rec_wake(s);
            if (step)
                return STOP_TRAP;
            for (;;) {
                if (!net_ready(g->fd, -1))
                    continue;
                int ch = pollbyte(g);
                if (ch == 3)
                    return STOP_INT;
                if (ch == -2)
                    return STOP_LOST;
            }
        }
        if (s->state == END_LOCKUP)
            return STOP_LOCKUP;
        if (s->state != RUN)
            return STOP_END;
        if (step)
            return STOP_TRAP;
        if (!(++n & 0x3fff)) {
            int ch = pollbyte(g);
            if (ch == 3)
                return STOP_INT;
            if (ch == -2)
                return STOP_LOST;
        }
    }
}

/* The stop reply for why the target stopped; 1 when the target has
 * exited (W). */
static int report(struct gdb *g, int why)
{
    struct sim *s = g->s;
    char extra[48];
    switch (why) {
    case STOP_INT:
        set_stop(g, 2, "");
        break;
    case STOP_WATCH:
        snprintf(extra, sizeof extra, "%s:%x;",
                 s->bus.watch_kind == WATCH_READ ? "rwatch"
                 : s->bus.watch_kind == WATCH_ACCESS ? "awatch" : "watch",
                 s->bus.watch_addr);
        set_stop(g, 5, extra);
        break;
    case STOP_LOCKUP:
        set_stop(g, 11, "");            /* SIGSEGV */
        break;
    case STOP_END: {
        int st = s->state == END_EXIT ? s->exit_status
               : s->state == END_BUDGET ? 4 : 0;
        snprintf(extra, sizeof extra, "W%02x", st & 0xff);
        send(g, extra);
        return 1;
    }
    default:
        set_stop(g, 5, "");             /* SIGTRAP */
        break;
    }
    send_stop(g);
    return 0;
}

/* ---- registers and memory --------------------------------------------- */

static int put_reg(struct gdb *g, int n, char *o)
{
    u8 b[16];
    int k = g->s->cpu->ops->reg_read(g->s->cpu, n, b);
    for (int i = 0; i < k; i++) {
        o[2 * i] = hexd[b[i] >> 4];
        o[2 * i + 1] = hexd[b[i] & 15];
    }
    o[2 * k] = 0;
    return 2 * k;
}

/* `len` hex digits at p into bytes; the count, or -1 when they are not
 * hex */
static int unhex(const char *p, u8 *b, int len)
{
    for (int i = 0; i < len; i++) {
        int h = hexval((u8)p[2 * i]), l = hexval((u8)p[2 * i + 1]);
        if (h < 0 || l < 0)
            return -1;
        b[i] = (u8)(h << 4 | l);
    }
    return len;
}

/* debugger accesses, by the widest aligned piece, so a device register
 * is read once as a word rather than four times as bytes */
static int mem_read(struct gdb *g, u32 a, u8 *b, int len)
{
    struct bus *bus = &g->s->bus;
    int i = 0;
    while (i < len) {
        u32 x = a + (u32)i, v;
        int n = (len - i >= 4 && !(x & 3)) ? 4 : (len - i >= 2 && !(x & 1)) ? 2 : 1;
        if (bus_debug_read(bus, x, n, &v))
            break;
        for (int k = 0; k < n; k++)
            b[i + k] = (u8)(v >> (8 * k));
        i += n;
    }
    return i;
}

static int mem_write(struct gdb *g, u32 a, const u8 *b, int len)
{
    struct bus *bus = &g->s->bus;
    rec_mem(g->s, a, b, len);
    for (int i = 0; i < len;) {
        u32 x = a + (u32)i, v = 0;
        int n = (len - i >= 4 && !(x & 3)) ? 4 : (len - i >= 2 && !(x & 1)) ? 2 : 1;
        for (int k = 0; k < n; k++)
            v |= (u32)b[i + k] << (8 * k);
        if (bus_debug_write(bus, x, n, v))
            return -1;
        i += n;
    }
    return 0;
}

/* ---- the memory map ---------------------------------------------------- */

/* The board's address space for gdb: memory as ram or flash (flash with
 * its erase block), and the device and bit-band spaces as ram, so gdb
 * reads registers there. gdb wants no overlaps: memory first, then the
 * widest of the rest that overlaps nothing taken. */
struct span { u32 base, size; int flash; };

static int overlaps(const struct span *t, int n, u32 base, u32 size)
{
    for (int i = 0; i < n; i++)
        if (base - t[i].base < t[i].size || t[i].base - base < size)
            return 1;
    return 0;
}

static const char *memory_map(struct gdb *g, char *buf, size_t cap)
{
    struct bus *b = &g->s->bus;
    struct span t[BUS_REGIONS + BUS_DEVICES + BUS_ALIASES];
    int n = 0;
    for (int i = 0; i < b->nrg; i++)
        if (!overlaps(t, n, b->rg[i].base, b->rg[i].size)) {
            t[n].base = b->rg[i].base;
            t[n].size = b->rg[i].size;
            t[n++].flash = b->rg[i].kind == MEM_FLASH;
        }
    for (;;) {
        u32 best = 0, base = 0;
        for (int i = 0; i < b->ndev + b->nbb; i++) {
            u32 bs = i < b->ndev ? b->dev[i].base : b->bb[i - b->ndev].base;
            u32 sz = i < b->ndev ? b->dev[i].size : b->bb[i - b->ndev].size;
            if (sz > best && !overlaps(t, n, bs, sz)) {
                best = sz;
                base = bs;
            }
        }
        if (!best)
            break;
        t[n].base = base;
        t[n].size = best;
        t[n++].flash = 0;
    }
    /* in address order */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && t[j].base < t[j - 1].base; j--) {
            struct span x = t[j];
            t[j] = t[j - 1];
            t[j - 1] = x;
        }
    size_t k = (size_t)snprintf(buf, cap,
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE memory-map PUBLIC \"+//IDN gnu.org//DTD GDB Memory Map "
        "V1.0//EN\" \"http://sourceware.org/gdb/gdb-memory-map.dtd\">\n"
        "<memory-map>\n");
    for (int i = 0; i < n && k < cap; i++) {
        /* the last byte of a span that runs to the top of the space */
        u64 len = t[i].size;
        if (t[i].flash)
            k += (size_t)snprintf(buf + k, cap - k,
                "<memory type=\"flash\" start=\"0x%x\" length=\"0x%llx\">"
                "<property name=\"blocksize\">0x400</property></memory>\n",
                t[i].base, (unsigned long long)len);
        else
            k += (size_t)snprintf(buf + k, cap - k,
                "<memory type=\"ram\" start=\"0x%x\" length=\"0x%llx\"/>\n",
                t[i].base, (unsigned long long)len);
    }
    if (k < cap)
        snprintf(buf + k, cap - k, "</memory-map>\n");
    return buf;
}

/* qXfer: the slice of `doc` at off, len, with 'm' (more) or 'l' (last) */
static void xfer(struct gdb *g, const char *doc, const char *args)
{
    const char *p = args;
    u32 off = hexnum(&p), len;
    if (*p++ != ',') {
        send(g, "E01");
        return;
    }
    len = hexnum(&p);
    size_t total = strlen(doc);
    if (len > PKT_MAX - 2)
        len = PKT_MAX - 2;
    if (off >= total) {
        send(g, "l");
        return;
    }
    size_t n = total - off < len ? total - off : len;
    g->pkt[0] = off + n < total ? 'm' : 'l';
    memcpy(g->pkt + 1, doc + off, n);
    send_raw(g, g->pkt, (int)n + 1);
}

/* ---- monitor commands -------------------------------------------------- */

static void monitor(struct gdb *g, const char *hex)
{
    char cmd[256], line[256];
    struct sim *s = g->s;
    int n = (int)strlen(hex) / 2;
    if (n >= (int)sizeof cmd)
        n = (int)sizeof cmd - 1;
    if (unhex(hex, (u8 *)cmd, n) < 0) {
        send(g, "E01");
        return;
    }
    cmd[n] = 0;
    while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\n'))
        cmd[--n] = 0;
    if (!strcmp(cmd, "help")) {
        mon_out(g, "reset         reset the core and the devices; memory "
                   "keeps what is in it\n"
                   "reload        load the image again, and reset\n"
                   "stats         instructions and estimated cycles so far\n"
                   "insns         the instructions run so far\n"
                   "cycles        the estimated cycles so far\n");
    } else if (!strcmp(cmd, "reset") || !strcmp(cmd, "reload")) {
        int reload = cmd[2] == 'l';
        rec_reset(s, reload);
        sim_reset(s, reload);
        set_stop(g, 5, "");
        mon_out(g, reload ? "embsim: the image reloaded, and reset\n"
                          : "embsim: reset\n");
    } else if (!strcmp(cmd, "stats")) {
        snprintf(line, sizeof line,
                 "%llu instructions, %llu cycles (est.)\n",
                 (unsigned long long)s->insns, (unsigned long long)s->cycles);
        mon_out(g, line);
    } else if (!strcmp(cmd, "insns") || !strcmp(cmd, "instructions")) {
        snprintf(line, sizeof line, "%llu\n", (unsigned long long)s->insns);
        mon_out(g, line);
    } else if (!strcmp(cmd, "cycles")) {
        snprintf(line, sizeof line, "%llu\n", (unsigned long long)s->cycles);
        mon_out(g, line);
    } else {
        snprintf(line, sizeof line,
                 "embsim: unknown monitor command '%s' (monitor help)\n", cmd);
        mon_out(g, line);
    }
    send(g, "OK");
}

/* ---- the session ------------------------------------------------------- */

/* A resume packet's action for our thread: 's' (step), 'c' (continue),
 * or 0 when it is not one. */
static int vcont_action(const char *p)
{
    int act = 0;
    while (*p == ';') {
        int a = p[1];
        p += 2;
        if (a == 'C' || a == 'S')       /* the signal is not delivered */
            while (hexval((u8)*p) >= 0)
                p++;
        const char *tid = 0;
        if (*p == ':')
            tid = ++p;
        while (*p && *p != ';')
            p++;
        /* ours: no thread, all threads, or thread 1 */
        int mine = !tid || strstr(tid, "-1") == tid || tid[0] == '1' ||
                   !strncmp(tid, "p1.1", 4) || !strncmp(tid, "p1.-1", 5) ||
                   !strncmp(tid, "p01.01", 6);
        if (mine && !act)
            act = (a == 's' || a == 'S') ? 's' : (a == 'c' || a == 'C') ? 'c' : 0;
    }
    return act;
}

static int session(struct gdb *g)
{
    struct sim *s = g->s;
    struct cpu *c = s->cpu;
    for (;;) {
        if (recv_packet(g) < 0)
            return END_LOST;
        char *p = g->pkt;
        char *o = g->reply;
        switch (p[0]) {
        case '?':
            send_stop(g);
            continue;
        case 'g': {
            const int *r = c->ops->gdb_g_regs(c);
            int k = 0;
            for (; *r >= 0; r++)
                k += put_reg(g, *r, o + k);
            send(g, o);
            continue;
        }
        case 'G': {
            const int *r = c->ops->gdb_g_regs(c);
            const char *q = p + 1;
            u8 b[16], tmp[16];
            int ok = 1;
            for (; *r >= 0 && ok; r++) {
                int sz = c->ops->reg_read(c, *r, tmp);
                if ((int)strlen(q) < 2 * sz || unhex(q, b, sz) < 0)
                    ok = 0;
                else {
                    c->ops->reg_write(c, *r, b);
                    rec_reg(s, *r, b, sz);
                    q += 2 * sz;
                }
            }
            send(g, ok ? "OK" : "E01");
            continue;
        }
        case 'p': {
            const char *q = p + 1;
            int n = (int)hexnum(&q);
            send(g, put_reg(g, n, o) ? o : "E14");
            continue;
        }
        case 'P': {
            const char *q = p + 1;
            int n = (int)hexnum(&q);
            u8 b[16], tmp[16];
            int sz = c->ops->reg_read(c, n, tmp);
            if (*q++ != '=' || !sz || (int)strlen(q) < 2 * sz ||
                unhex(q, b, sz) < 0)
                send(g, "E14");
            else {
                c->ops->reg_write(c, n, b);
                rec_reg(s, n, b, sz);
                send(g, "OK");
            }
            continue;
        }
        case 'm': {
            const char *q = p + 1;
            u32 a = hexnum(&q), len;
            u8 b[PKT_MAX / 2];
            if (*q++ != ',') {
                send(g, "E01");
                continue;
            }
            len = hexnum(&q);
            if (len > sizeof b)
                len = sizeof b;
            int got = mem_read(g, a, b, (int)len);
            if (!got && len) {
                send(g, "E14");
                continue;
            }
            for (int i = 0; i < got; i++) {
                o[2 * i] = hexd[b[i] >> 4];
                o[2 * i + 1] = hexd[b[i] & 15];
            }
            o[2 * got] = 0;
            send(g, o);
            continue;
        }
        case 'M':
        case 'X': {
            const char *q = p + 1;
            u32 a = hexnum(&q), len;
            u8 b[PKT_MAX];
            if (*q++ != ',') {
                send(g, "E01");
                continue;
            }
            len = hexnum(&q);
            if (*q++ != ':' || len > sizeof b) {
                send(g, "E01");
                continue;
            }
            if (p[0] == 'M') {
                if ((u32)strlen(q) < 2 * len || unhex(q, b, (int)len) < 0) {
                    send(g, "E01");
                    continue;
                }
            } else {
                if ((u32)(g->plen - (q - p)) < len) {
                    send(g, "E01");
                    continue;
                }
                memcpy(b, q, len);
            }
            send(g, mem_write(g, a, b, (int)len) ? "E14" : "OK");
            continue;
        }
        case 'c':
        case 's':
        case 'C':
        case 'S': {
            /* c [ADDR], s [ADDR], C SIG[;ADDR], S SIG[;ADDR] */
            const char *q = p + 1;
            if (p[0] == 'C' || p[0] == 'S') {
                hexnum(&q);
                if (*q == ';')
                    q++;
                else
                    q = "";
            }
            if (*q) {
                /* the address into the program counter, at its width */
                u8 b[16];
                u32 a = hexnum(&q);
                int sz = c->ops->reg_read(c, c->ops->pc_regnum, b);
                for (int i = 0; i < sz; i++)
                    b[i] = (u8)(i < 4 ? a >> (8 * i) : 0);
                c->ops->reg_write(c, c->ops->pc_regnum, b);
                rec_reg(s, c->ops->pc_regnum, b, sz);
            }
            int why = resume(g, p[0] == 's' || p[0] == 'S');
            if (why == STOP_LOST)
                return END_LOST;
            if (report(g, why))
                return END_EXITED;
            continue;
        }
        case 'v':
            if (!strcmp(p, "vCont?")) {
                send(g, "vCont;c;C;s;S");
            } else if (!strncmp(p, "vCont;", 5)) {
                int act = vcont_action(p + 5);
                if (!act) {
                    send(g, "E01");
                    continue;
                }
                int why = resume(g, act == 's');
                if (why == STOP_LOST)
                    return END_LOST;
                if (report(g, why))
                    return END_EXITED;
            } else if (!strncmp(p, "vKill", 5)) {
                send(g, "OK");
                return END_KILL;
            } else if (!strncmp(p, "vFlashErase:", 12)) {
                const char *q = p + 12;
                u32 a = hexnum(&q), len = 0;
                if (*q++ == ',')
                    len = hexnum(&q);
                int bad = 0;
                for (u32 i = 0; i < len && !bad; i++)
                    bad = bus_debug_write(&s->bus, a + i, 1, 0xff) != 0;
                if (!bad && s->rec && len) {
                    u8 *ff = malloc(len);
                    if (!ff)
                        die("out of memory");
                    memset(ff, 0xff, len);
                    rec_mem(s, a, ff, (int)len);
                    free(ff);
                }
                send(g, bad ? "E01" : "OK");
            } else if (!strncmp(p, "vFlashWrite:", 12)) {
                const char *q = p + 12;
                u32 a = hexnum(&q);
                if (*q++ != ':') {
                    send(g, "E01");
                    continue;
                }
                int len = g->plen - (int)(q - p);
                send(g, mem_write(g, a, (const u8 *)q, len) ? "E01" : "OK");
            } else if (!strcmp(p, "vFlashDone")) {
                send(g, "OK");
            } else
                send(g, "");
            continue;
        case 'Z':
        case 'z': {
            int type = p[1] - '0', insert = p[0] == 'Z';
            const char *q = p + 2;
            if (*q++ != ',' || type < 0 || type > 4) {
                send(g, "");
                continue;
            }
            u32 a = hexnum(&q), len = 0;
            if (*q++ == ',')
                len = hexnum(&q);
            int r;
            if (type <= 1)
                r = bp_set(g, type, a, insert);
            else if (insert)
                r = bus_watch_add(&s->bus, type, a, len);
            else
                r = bus_watch_remove(&s->bus, type, a, len), r = 0;
            send(g, r ? "E01" : "OK");
            continue;
        }
        case 'k':
            return END_KILL;
        case 'D':
            send(g, "OK");
            return END_DETACH;
        case 'H':
        case 'T':
            send(g, "OK");
            continue;
        case 'q':
            if (!strncmp(p, "qSupported", 10)) {
                g->multiprocess = strstr(p, "multiprocess+") != 0;
                snprintf(o, 256, "PacketSize=%x;qXfer:features:read+;"
                         "qXfer:memory-map:read+;vContSupported+;"
                         "QStartNoAckMode+%s", PKT_MAX,
                         g->multiprocess ? ";multiprocess+" : "");
                send(g, o);
            } else if (!strncmp(p, "qXfer:features:read:", 20)) {
                char annex[64];
                const char *q = p + 20, *e = strchr(q, ':');
                const char *doc;
                if (!e || e - q >= (int)sizeof annex) {
                    send(g, "E00");
                    continue;
                }
                memcpy(annex, q, (size_t)(e - q));
                annex[e - q] = 0;
                doc = c->ops->gdb_xml(c, annex);
                if (!doc)
                    send(g, "E00");
                else
                    xfer(g, doc, e + 1);
            } else if (!strncmp(p, "qXfer:memory-map:read::", 23)) {
                static char map[4096];
                xfer(g, memory_map(g, map, sizeof map), p + 23);
            } else if (!strcmp(p, "qAttached") ||
                       !strncmp(p, "qAttached:", 10)) {
                send(g, "1");
            } else if (!strcmp(p, "qC")) {
                snprintf(o, 64, "QC%s", thread(g));
                send(g, o);
            } else if (!strcmp(p, "qfThreadInfo")) {
                snprintf(o, 64, "m%s", thread(g));
                send(g, o);
            } else if (!strcmp(p, "qsThreadInfo")) {
                send(g, "l");
            } else if (!strncmp(p, "qThreadExtraInfo,", 17)) {
                char name[96];
                int k = 0;
                snprintf(name, sizeof name, "%s [%s]", s->board->name,
                         s->cpu->ops->name);
                for (const char *t = name; *t; t++) {
                    o[k++] = hexd[(u8)*t >> 4];
                    o[k++] = hexd[(u8)*t & 15];
                }
                o[k] = 0;
                send(g, o);
            } else if (!strncmp(p, "qRcmd,", 6)) {
                monitor(g, p + 6);
            } else
                send(g, "");
            continue;
        case 'Q':
            if (!strcmp(p, "QStartNoAckMode")) {
                send(g, "OK");
                g->noack = 1;
            } else
                send(g, "");
            continue;
        default:
            send(g, "");
            continue;
        }
    }
}

void gdb_serve(struct sim *s, const char *host, int port, int wait)
{
    static struct gdb gs;
    struct gdb *g = &gs;
    int l = net_listen(host, port);
    if (l < 0)
        die("the GDB server cannot listen on port %d", port);
    fprintf(stderr, "embsim: GDB server on %s:%d%s\n", host ? host : "localhost",
            port, wait ? ", waiting for a connection" : "");
    memset(g, 0, sizeof *g);
    g->s = s;
    g->fd = -1;
    if (wait) {
        while (g->fd < 0)
            g->fd = net_accept(l, -1);
        set_stop(g, 5, "");
    } else {
        /* run, and look for a debugger as for the interrupt; a core that
         * nothing can wake waits for one, as under a debugger */
        unsigned n = 0;
        while (s->state == RUN || s->state == END_IDLE) {
            if (s->state == END_IDLE) {
                s->state = RUN;
                rec_wake(s);
                while (g->fd < 0)
                    g->fd = net_accept(l, -1);
                break;
            }
            sim_step(s);
            if (!(++n & 0x3fff) && (g->fd = net_accept(l, 0)) >= 0)
                break;
        }
        if (g->fd < 0)
            return;                     /* the run ended on its own */
        set_stop(g, 2, "");
    }
    int how = session(g);
    net_close(g->fd);
    if (how == END_KILL) {
        rec_kill(s);
        s->exit_status = 0;
        sim_end(s, END_EXIT, "the debugger killed it");
    } else if (how == END_DETACH || how == END_LOST) {
        /* the target runs on, as a board does when the probe lets go */
        s->bus.nwatch = 0;
        sim_run(s);
    }
}
