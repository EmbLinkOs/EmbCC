/* embflash -- put a firmware image into a target's memory, through the
 * GDB remote serial protocol.
 *
 *   embflash IMAGE --gdb TARGET [--base ADDR] [--verify] [--run]
 *                  [--entry ADDR] [--vector-table ADDR] [--monitor CMD]...
 *                  [--dry-run] [--timeout SECONDS] [-v]
 *
 * One protocol reaches nearly every way of programming a part: QEMU's
 * gdbstub, OpenOCD, pyOCD, SEGGER's J-Link GDB server and the Black Magic
 * Probe all speak it. TARGET is where the server listens:
 *
 *   HOST:PORT        a TCP port (OpenOCD's 3333, a J-Link GDB server's 2331)
 *   unix:PATH        a Unix-domain socket (QEMU's -gdb chardev)
 *   serial:DEV[@B]   a serial line at B baud (115200 when left out) -- the
 *                    Black Magic Probe's GDB port
 *
 * IMAGE is an ELF file (its stored bytes at their LOAD addresses, as embpack
 * packs them), Intel HEX (.hex), Motorola S-records (.srec, .s19, .s37,
 * .mot), or raw bytes (.bin, with --base ADDR saying where they go).
 *
 * How each byte gets there depends on what the server says about the
 * target's memory (qXfer:memory-map:read):
 *
 *   flash  erased in the server's blocks, then written and committed with
 *          vFlashErase / vFlashWrite / vFlashDone -- the server runs the
 *          part's flash algorithm (OpenOCD, the Black Magic Probe, pyOCD);
 *   ram    and anything a server does not map: written as memory, with
 *          X (binary) where the server takes it and M (hex) where not
 *          (QEMU's gdbstub writes its flash this way too: it has no map).
 *
 * --verify reads every byte back (m) and compares. --run starts the
 * program: on a Cortex-M the stack pointer and the entry come from the
 * vector table at the image's lowest address (or --vector-table), as the
 * core itself takes them at reset; elsewhere the pc is the ELF entry (or
 * --entry). The registers are found by NAME in the server's target
 * description, so no table of register numbers is kept here. --monitor
 * sends a command to the server first (`reset halt` for OpenOCD), one per
 * flag.
 *
 * ISO C for everything but the transports, which are POSIX sockets and
 * termios; docs/manual/tools/embflash.md is the reference. Real probes are
 * reached through their GDB servers -- this program has been tested against
 * QEMU and against a server that models a part's flash, not yet against
 * hardware. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1          /* and the baud rates POSIX leaves out */
#define _DEFAULT_SOURCE 1
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#if defined(__unix__) || defined(__APPLE__)
#define EMBFLASH_POSIX 1
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <netdb.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

typedef unsigned long long u64;
typedef unsigned int u32;

static int g_verbose;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "embflash: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open %s", path);
    size_t cap = 1 << 16, n = 0;
    unsigned char *p = xmalloc(cap);
    for (;;) {
        if (n == cap)
            p = xrealloc(p, cap *= 2);
        size_t got = fread(p + n, 1, cap - n, f);
        if (got == 0)
            break;
        n += got;
    }
    fclose(f);
    *len = n;
    return p;
}

/* ==== the image: bytes at addresses ======================================
 *
 * Whatever the file's format, it becomes a sorted list of chunks, each a
 * run of bytes at a load address, adjacent runs merged. */
struct chunk { u64 addr; size_t len; unsigned char *b; };
static struct chunk *g_ch;
static int g_nch, g_capch;
static u64 g_entry;
static int g_have_entry;
static unsigned g_machine;          /* ELF e_machine, 0 for the others */

static void add_bytes(u64 addr, const unsigned char *p, size_t n)
{
    if (n == 0)
        return;
    if (g_nch && g_ch[g_nch - 1].addr + g_ch[g_nch - 1].len == addr) {
        struct chunk *c = &g_ch[g_nch - 1];
        c->b = xrealloc(c->b, c->len + n);
        memcpy(c->b + c->len, p, n);
        c->len += n;
        return;
    }
    if (g_nch == g_capch)
        g_ch = xrealloc(g_ch, (size_t)(g_capch = g_capch ? 2 * g_capch : 16)
                              * sizeof *g_ch);
    g_ch[g_nch].addr = addr;
    g_ch[g_nch].len = n;
    g_ch[g_nch].b = xmalloc(n);
    memcpy(g_ch[g_nch].b, p, n);
    g_nch++;
}

static int chunk_cmp(const void *a, const void *b)
{
    const struct chunk *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

/* sort, refuse overlaps, merge what touches */
static void finish_chunks(const char *path)
{
    if (g_nch == 0)
        die("%s: no bytes to program", path);
    qsort(g_ch, (size_t)g_nch, sizeof *g_ch, chunk_cmp);
    int m = 0;
    for (int k = 0; k < g_nch; k++) {
        if (m && g_ch[k].addr < g_ch[m - 1].addr + g_ch[m - 1].len)
            die("%s: two pieces of the image overlap at 0x%llx", path,
                g_ch[k].addr);
        if (m && g_ch[m - 1].addr + g_ch[m - 1].len == g_ch[k].addr) {
            struct chunk *c = &g_ch[m - 1];
            c->b = xrealloc(c->b, c->len + g_ch[k].len);
            memcpy(c->b + c->len, g_ch[k].b, g_ch[k].len);
            c->len += g_ch[k].len;
            free(g_ch[k].b);
        } else {
            g_ch[m++] = g_ch[k];
        }
    }
    g_nch = m;
}

/* ---- ELF: the stored bytes at their load addresses (as embpack packs) -- */

static unsigned char *g_e;
static size_t g_elen;
static int g_ebe, g_e64;

static u64 erd(size_t off, int n)
{
    u64 v = 0;
    if (off + (size_t)n > g_elen || off + (size_t)n < off)
        die("truncated ELF file");
    for (int k = 0; k < n; k++)
        v = (v << 8) | g_e[off + (size_t)(g_ebe ? k : n - 1 - k)];
    return v;
}

static void load_elf(const char *path, unsigned char *buf, size_t len)
{
    g_e = buf; g_elen = len;
    if (len < 52 || (buf[4] != 1 && buf[4] != 2) || (buf[5] != 1 && buf[5] != 2))
        die("%s: an ELF class or byte order this does not know", path);
    g_e64 = buf[4] == 2;
    g_ebe = buf[5] == 2;
    int w = g_e64 ? 8 : 4;
    if (erd(16, 2) != 2)
        die("%s: not a linked image (an object file? program the output "
            "of the link)", path);
    g_machine = (unsigned)erd(18, 2);
    g_entry = erd(24, w);
    g_have_entry = 1;
    u64 phoff = erd(g_e64 ? 32 : 28, w), shoff = erd(g_e64 ? 40 : 32, w);
    unsigned phentsize = (unsigned)erd(g_e64 ? 54 : 42, 2);
    unsigned phnum = (unsigned)erd(g_e64 ? 56 : 44, 2);
    unsigned shentsize = (unsigned)erd(g_e64 ? 58 : 46, 2);
    unsigned shnum = (unsigned)erd(g_e64 ? 60 : 48, 2);
    if (shoff == 0 || shnum == 0)
        die("%s: no section headers (a stripped image cannot be programmed "
            "here: pack it with embpack first)", path);
    /* the load segments, to take a section's run address to its load one */
    u64 *pv = xmalloc((phnum + 1) * sizeof *pv), *pp = xmalloc((phnum + 1) * sizeof *pp),
        *pm = xmalloc((phnum + 1) * sizeof *pm);
    int nph = 0;
    for (unsigned k = 0; k < phnum; k++) {
        size_t o = (size_t)phoff + (size_t)k * phentsize;
        if (erd(o, 4) != 1)
            continue;
        pv[nph] = erd(o + (g_e64 ? 16 : 8), w);
        pp[nph] = erd(o + (g_e64 ? 24 : 12), w);
        pm[nph] = erd(o + (g_e64 ? 40 : 20), w);
        nph++;
    }
    for (unsigned k = 0; k < shnum; k++) {
        size_t o = (size_t)shoff + (size_t)k * shentsize;
        unsigned type = (unsigned)erd(o + 4, 4);
        u64 flags = erd(o + 8, w);
        u64 addr = erd(o + (g_e64 ? 16 : 12), w);
        u64 off = erd(o + (g_e64 ? 24 : 16), w);
        u64 size = erd(o + (g_e64 ? 32 : 20), w);
        if (!(flags & 2) || type == 8 || type == 0 || size == 0)
            continue;                    /* not allocated, or .bss */
        if (off + size > len)
            die("%s: a section runs past the end of the file", path);
        u64 lma = addr;
        for (int j = 0; j < nph; j++)
            if (addr >= pv[j] && addr - pv[j] < pm[j]) {
                lma = addr - pv[j] + pp[j];
                break;
            }
        add_bytes(lma, buf + off, (size_t)size);
    }
    free(pv); free(pp); free(pm);
}

/* ---- Intel HEX ---------------------------------------------------------- */

static int hexv(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* n bytes of hex at p into out; -1 on a bad digit */
static int unhex(const char *p, int n, unsigned char *out)
{
    for (int k = 0; k < n; k++) {
        int h = hexv((unsigned char)p[2 * k]), l = hexv((unsigned char)p[2 * k + 1]);
        if (h < 0 || l < 0)
            return -1;
        out[k] = (unsigned char)(h << 4 | l);
    }
    return 0;
}

static void load_ihex(const char *path, const char *text)
{
    u64 upper = 0;
    int line = 0, ended = 0;
    for (const char *p = text; *p; ) {
        line++;
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        while (n && (p[n - 1] == '\r' || p[n - 1] == ' '))
            n--;
        if (n == 0) { p = e ? e + 1 : p + n; continue; }
        if (p[0] != ':' || n < 11 || (n - 1) % 2)
            die("%s:%d: not an Intel HEX record", path, line);
        unsigned char r[300];
        int nb = (int)(n - 1) / 2;
        if (nb > (int)sizeof r || unhex(p + 1, nb, r))
            die("%s:%d: not an Intel HEX record", path, line);
        int cnt = r[0];
        if (cnt + 5 != nb)
            die("%s:%d: the record's length does not match its count", path, line);
        unsigned sum = 0;
        for (int k = 0; k < nb; k++)
            sum += r[k];
        if (sum & 0xff)
            die("%s:%d: bad checksum", path, line);
        unsigned off = (unsigned)r[1] << 8 | r[2];
        switch (r[3]) {
        case 0: add_bytes(upper + off, r + 4, (size_t)cnt); break;
        case 1: ended = 1; break;
        case 2: upper = ((u64)r[4] << 8 | r[5]) << 4; break;          /* segment */
        case 3: g_entry = ((u64)r[4] << 8 | r[5]) * 16 + ((u64)r[6] << 8 | r[7]);
                g_have_entry = 1; break;
        case 4: upper = ((u64)r[4] << 8 | r[5]) << 16; break;         /* linear */
        case 5: g_entry = (u64)r[4] << 24 | (u64)r[5] << 16 | (u64)r[6] << 8 | r[7];
                g_have_entry = 1; break;
        default: die("%s:%d: record type %d is not Intel HEX", path, line, r[3]);
        }
        if (ended)
            break;
        p = e ? e + 1 : p + n;
    }
    if (!ended)
        die("%s: no end-of-file record (truncated?)", path);
}

/* ---- Motorola S-records ------------------------------------------------- */

static void load_srec(const char *path, const char *text)
{
    int line = 0;
    for (const char *p = text; *p; ) {
        line++;
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        while (n && (p[n - 1] == '\r' || p[n - 1] == ' '))
            n--;
        if (n == 0) { p = e ? e + 1 : p + n; continue; }
        if (p[0] != 'S' || n < 4 || !isdigit((unsigned char)p[1]) || n % 2)
            die("%s:%d: not an S-record", path, line);
        unsigned char r[300];
        int nb = (int)(n - 2) / 2;
        if (nb > (int)sizeof r || unhex(p + 2, nb, r))
            die("%s:%d: not an S-record", path, line);
        if (r[0] + 1 != nb)
            die("%s:%d: the record's length does not match its count", path, line);
        unsigned sum = 0;
        for (int k = 0; k < nb; k++)
            sum += r[k];
        if ((sum & 0xff) != 0xff)
            die("%s:%d: bad checksum", path, line);
        int t = p[1] - '0';
        int alen = t == 1 || t == 9 ? 2 : t == 2 || t == 8 ? 3 : t == 3 || t == 7 ? 4 : 0;
        u64 a = 0;
        for (int k = 0; k < alen; k++)
            a = a << 8 | r[1 + k];
        if (t >= 1 && t <= 3)
            add_bytes(a, r + 1 + alen, (size_t)(nb - 2 - alen));
        else if (t >= 7 && t <= 9) {
            g_entry = a; g_have_entry = 1;
        }
        p = e ? e + 1 : p + n;
    }
}

static int has_suffix(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    if (a < b)
        return 0;
    for (size_t k = 0; k < b; k++)
        if (tolower((unsigned char)s[a - b + k]) != suf[k])
            return 0;
    return 1;
}

static void load_image(const char *path, int have_base, u64 base)
{
    size_t len;
    unsigned char *buf = slurp(path, &len);
    if (len >= 4 && memcmp(buf, "\177ELF", 4) == 0) {
        if (have_base)
            die("--base is for a raw binary; an ELF image carries its own "
                "addresses");
        load_elf(path, buf, len);
    } else if (has_suffix(path, ".hex") || has_suffix(path, ".ihex") ||
               (len && buf[0] == ':')) {
        buf = xrealloc(buf, len + 1); buf[len] = 0;
        load_ihex(path, (const char *)buf);
    } else if (has_suffix(path, ".srec") || has_suffix(path, ".s19") ||
               has_suffix(path, ".s28") || has_suffix(path, ".s37") ||
               has_suffix(path, ".mot")) {
        buf = xrealloc(buf, len + 1); buf[len] = 0;
        load_srec(path, (const char *)buf);
    } else {
        if (!have_base)
            die("%s: a raw binary has no addresses of its own: say where it "
                "goes with --base ADDR", path);
        add_bytes(base, buf, len);
    }
    finish_chunks(path);
}

/* ==== the transport ====================================================== */

static int g_fd = -1;
static int g_timeout_ms = 10000;
static int g_detached;              /* D answered: the server may hang up */

#ifdef EMBFLASH_POSIX
static void open_target(const char *t)
{
    /* a server that hangs up is reported (tx sees EPIPE), not a silent
     * death by SIGPIPE */
    signal(SIGPIPE, SIG_IGN);
    if (!strncmp(t, "unix:", 5)) {
        struct sockaddr_un a;
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        if (strlen(t + 5) >= sizeof a.sun_path)
            die("%s: a Unix socket path this long does not fit (%u bytes "
                "at most); use a shorter or relative one", t + 5,
                (unsigned)sizeof a.sun_path - 1);
        strcpy(a.sun_path, t + 5);
        g_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (g_fd < 0 || connect(g_fd, (struct sockaddr *)&a, sizeof a) < 0)
            die("cannot connect to %s: %s", t, strerror(errno));
        return;
    }
    if (!strncmp(t, "serial:", 7)) {
        char dev[512];
        snprintf(dev, sizeof dev, "%s", t + 7);
        long baud = 115200;
        char *at = strrchr(dev, '@');
        if (at) { *at = 0; baud = strtol(at + 1, NULL, 10); }
        g_fd = open(dev, O_RDWR | O_NOCTTY);
        if (g_fd < 0)
            die("cannot open %s: %s", dev, strerror(errno));
        struct termios tio;
        if (tcgetattr(g_fd, &tio) == 0) {
            /* raw: no echo, no line editing, no translation, 8N1 */
            tio.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                                       INLCR | IGNCR | ICRNL | IXON);
            tio.c_oflag &= ~(tcflag_t)OPOST;
            tio.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
            tio.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
            tio.c_cflag |= CS8 | CREAD | CLOCAL;
            tio.c_cc[VMIN] = 1;
            tio.c_cc[VTIME] = 0;
            speed_t sp = baud == 9600 ? B9600 : baud == 19200 ? B19200
                       : baud == 38400 ? B38400 : baud == 57600 ? B57600
                       : B115200;
            cfsetispeed(&tio, sp);
            cfsetospeed(&tio, sp);
            tcsetattr(g_fd, TCSANOW, &tio);
        }
        return;
    }
    /* HOST:PORT */
    char host[256];
    const char *colon = strrchr(t, ':');
    if (!colon || colon == t)
        die("%s: a target is HOST:PORT, unix:PATH or serial:DEV[@BAUD]", t);
    snprintf(host, sizeof host, "%.*s", (int)(colon - t), t);
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, colon + 1, &hints, &res);
    if (rc)
        die("%s: %s", t, gai_strerror(rc));
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        g_fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (g_fd < 0)
            continue;
        if (connect(g_fd, r->ai_addr, r->ai_addrlen) == 0)
            break;
        close(g_fd);
        g_fd = -1;
    }
    freeaddrinfo(res);
    if (g_fd < 0)
        die("cannot connect to %s", t);
    int one = 1;
    setsockopt(g_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

static void tx(const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(g_fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (g_detached)
                return;                 /* the ack of D's reply: it may close first */
            die("the connection failed: %s", strerror(errno));
        }
        p += w; n -= (size_t)w;
    }
}

/* one byte, or -1 after the timeout */
static int rx_byte(void)
{
    static unsigned char buf[4096];
    static size_t have, at;
    if (at < have)
        return buf[at++];
    struct pollfd pf = { g_fd, POLLIN, 0 };
    int r = poll(&pf, 1, g_timeout_ms);
    if (r <= 0)
        return -1;
    ssize_t n = read(g_fd, buf, sizeof buf);
    if (n <= 0)
        die("the server closed the connection");
    have = (size_t)n; at = 0;
    return buf[at++];
}
#else
static void open_target(const char *t)
{
    die("%s: this host has no socket or serial support built in", t);
}
static void tx(const char *p, size_t n) { (void)p; (void)n; }
static int rx_byte(void) { return -1; }
#endif

/* ==== the GDB remote serial protocol ======================================
 *
 * $packet#cs, acknowledged with + or - until QStartNoAckMode; in a reply,
 * `}` escapes the next byte (xor 0x20) and `*` repeats the previous one
 * (run-length). A reply may be preceded by console output (O...) from a
 * monitor command, which is passed through to stderr. */
static int g_noack;
static size_t g_pktmax = 4096;      /* the server's PacketSize */
static char *g_rep;                 /* the last reply, NUL-terminated */
static size_t g_replen, g_repcap;

static void send_packet(const char *body, size_t n)
{
    static char *out;
    static size_t cap;
    if (n + 4 > cap)
        out = xrealloc(out, cap = n + 64);
    unsigned sum = 0;
    out[0] = '$';
    memcpy(out + 1, body, n);
    for (size_t k = 0; k < n; k++)
        sum += (unsigned char)body[k];
    snprintf(out + 1 + n, 4, "#%02x", sum & 0xff);
    for (int tries = 0; ; tries++) {
        tx(out, n + 4);
        if (g_noack)
            return;
        int c;
        do
            c = rx_byte();
        while (c != '+' && c != '-' && c != -1);
        if (c == '+')
            return;
        if (c == -1 || tries == 5)
            die("the server did not acknowledge a packet");
        if (g_verbose)
            fprintf(stderr, "embflash: resending a packet the server NAKed\n");
    }
}

/* Read one reply into g_rep; console packets (O<hex>) are printed and
 * skipped. Returns the reply length. */
static size_t recv_packet(void)
{
    for (;;) {
        int c;
        do {
            c = rx_byte();
            if (c == -1)
                die("no reply from the server within %d s", g_timeout_ms / 1000);
        } while (c != '$');
        g_replen = 0;
        unsigned sum = 0;
        int prev = -1, esc = 0;
        for (;;) {
            c = rx_byte();
            if (c == -1)
                die("the server's reply was cut off");
            if (c == '#')
                break;
            sum += (unsigned)c;
            int ch;
            if (esc) { ch = c ^ 0x20; esc = 0; }
            else if (c == '}') { esc = 1; continue; }
            else if (c == '*' && prev >= 0) {
                int r = rx_byte();
                if (r == -1)
                    die("the server's reply was cut off");
                sum += (unsigned)r;
                for (int k = 0; k < r - 29; k++) {
                    if (g_replen + 2 > g_repcap)
                        g_rep = xrealloc(g_rep, g_repcap = g_repcap * 2 + 256);
                    g_rep[g_replen++] = (char)prev;
                }
                continue;
            }
            else ch = c;
            if (g_replen + 2 > g_repcap)
                g_rep = xrealloc(g_rep, g_repcap = g_repcap * 2 + 256);
            g_rep[g_replen++] = (char)ch;
            prev = ch;
        }
        int h = rx_byte(), l = rx_byte();
        g_rep[g_replen] = 0;
        int ok = h >= 0 && l >= 0 && hexv(h) >= 0 && hexv(l) >= 0 &&
                 (unsigned)(hexv(h) << 4 | hexv(l)) == (sum & 0xff);
        if (!g_noack) {
            tx(ok ? "+" : "-", 1);
            if (!ok)
                continue;               /* the server sends it again */
        }
        if (g_replen >= 1 && g_rep[0] == 'O' && g_replen % 2 == 1 &&
            strcmp(g_rep, "OK") != 0) {
            /* console output from a monitor command */
            unsigned char b[256];
            for (size_t k = 1; k + 1 < g_replen; k += 2) {
                if (unhex(g_rep + k, 1, b) == 0)
                    fputc(b[0], stderr);
            }
            continue;
        }
        return g_replen;
    }
}

static const char *request(const char *body)
{
    send_packet(body, strlen(body));
    recv_packet();
    if (g_verbose > 1)
        fprintf(stderr, "embflash: %.60s -> %.60s\n", body, g_rep);
    return g_rep;
}

static void expect_ok(const char *what)
{
    if (strcmp(g_rep, "OK") != 0)
        die("%s: the server answered '%s'", what, g_rep);
}

/* ==== the target's memory, as the server describes it ==================== */

struct region { u64 start, len, block; int flash; };
static struct region *g_reg;
static int g_nreg;

/* a qXfer object, read whole (the l/m chunks joined) */
static char *qxfer(const char *object, const char *annex)
{
    size_t cap = 4096, n = 0;
    char *all = xmalloc(cap);
    for (;;) {
        char req[256];
        snprintf(req, sizeof req, "qXfer:%s:read:%s:%zx,%zx", object, annex,
                 n, g_pktmax > 64 ? g_pktmax - 32 : 512);
        const char *r = request(req);
        if (g_replen == 0 || r[0] == 'E') {
            free(all);
            return NULL;
        }
        if (n + g_replen + 1 > cap)
            all = xrealloc(all, cap = (n + g_replen) * 2 + 1);
        memcpy(all + n, r + 1, g_replen - 1);
        n += g_replen - 1;
        if (r[0] == 'l')
            break;
        if (r[0] != 'm') {
            free(all);
            return NULL;
        }
    }
    all[n] = 0;
    return all;
}

/* the value of attr="..." in tag text [p, end), or NULL */
static int xml_attr(const char *p, const char *end, const char *attr, char *out,
                    size_t outn)
{
    size_t al = strlen(attr);
    for (const char *q = p; q + al + 2 < end; q++) {
        if (!strncmp(q, attr, al) && q[al] == '=' && (q[al + 1] == '"' || q[al + 1] == '\'')) {
            char quote = q[al + 1];
            const char *v = q + al + 2, *ve = memchr(v, quote, (size_t)(end - v));
            if (!ve)
                return 0;
            snprintf(out, outn, "%.*s", (int)(ve - v), v);
            return 1;
        }
    }
    return 0;
}

static void read_memory_map(void)
{
    char *x = qxfer("memory-map", "");
    if (!x)
        return;
    for (const char *p = x; (p = strstr(p, "<memory")) != NULL; ) {
        const char *tagend = strchr(p, '>');
        if (!tagend)
            break;
        if (!isspace((unsigned char)p[7])) {     /* <memory-map> around them */
            p = tagend;
            continue;
        }
        /* <memory .../> has no body; a flash region's body holds its
         * block size and ends at its own </memory> */
        const char *end = tagend[-1] == '/' ? NULL : strstr(tagend, "</memory>");
        char type[32] = "", start[64] = "", length[64] = "";
        xml_attr(p, tagend, "type", type, sizeof type);
        xml_attr(p, tagend, "start", start, sizeof start);
        xml_attr(p, tagend, "length", length, sizeof length);
        struct region r = { strtoull(start, NULL, 0), strtoull(length, NULL, 0), 0,
                            !strcmp(type, "flash") };
        if (r.flash && end) {
            const char *bs = strstr(p, "name=\"blocksize\"");
            if (bs && bs < end) {
                const char *v = strchr(bs, '>');
                if (v)
                    r.block = strtoull(v + 1, NULL, 0);
            }
            if (r.block == 0)
                die("the server maps flash at 0x%llx with no block size", r.start);
        }
        g_reg = xrealloc(g_reg, (size_t)(g_nreg + 1) * sizeof *g_reg);
        g_reg[g_nreg++] = r;
        p = end ? end : tagend;
    }
    free(x);
}

static const struct region *region_of(u64 a)
{
    for (int k = 0; k < g_nreg; k++)
        if (a >= g_reg[k].start && a - g_reg[k].start < g_reg[k].len)
            return &g_reg[k];
    return NULL;
}

/* ==== writing ============================================================= */

static int g_x_ok = -1;             /* does the server take X? -1: not asked */

static void write_mem(u64 addr, const unsigned char *b, size_t n)
{
    /* each packet holds what fits: hex doubles the bytes, X escapes some */
    size_t room = g_pktmax > 64 ? g_pktmax - 48 : 512;
    char *pk = xmalloc(room * 2 + 64);
    while (n) {
        if (g_x_ok != 0) {
            size_t k = 0, len = 0, hdr;
            /* binary, escaping $ # } * */
            char head[64];
            size_t take = 0, sz = 0;
            while (take < n) {
                unsigned char c = b[take];
                size_t need = (c == '$' || c == '#' || c == '}' || c == '*') ? 2 : 1;
                if (sz + need > room)
                    break;
                sz += need; take++;
            }
            hdr = (size_t)snprintf(head, sizeof head, "X%llx,%zx:", addr, take);
            memcpy(pk, head, hdr);
            len = hdr;
            for (k = 0; k < take; k++) {
                unsigned char c = b[k];
                if (c == '$' || c == '#' || c == '}' || c == '*') {
                    pk[len++] = '}';
                    pk[len++] = (char)(c ^ 0x20);
                } else
                    pk[len++] = (char)c;
            }
            send_packet(pk, len);
            recv_packet();
            if (g_x_ok == -1) {
                g_x_ok = strcmp(g_rep, "OK") == 0;
                if (!g_x_ok) {
                    if (g_replen && g_rep[0] == 'E')
                        die("writing 0x%llx: the server refused it (%s)", addr, g_rep);
                    continue;           /* not supported: hex from here on */
                }
            } else
                expect_ok("a memory write");
            addr += take; b += take; n -= take;
            continue;
        }
        size_t take = n < room / 2 ? n : room / 2;
        size_t len = (size_t)sprintf(pk, "M%llx,%zx:", addr, take);
        for (size_t k = 0; k < take; k++)
            len += (size_t)sprintf(pk + len, "%02x", b[k]);
        send_packet(pk, len);
        recv_packet();
        if (strcmp(g_rep, "OK") != 0)
            die("writing %zu bytes at 0x%llx: the server answered '%s'",
                take, addr, g_rep);
        addr += take; b += take; n -= take;
    }
    free(pk);
}

/* vFlashWrite, escaped binary, as many packets as it takes */
static void flash_write(u64 addr, const unsigned char *b, size_t n)
{
    size_t room = g_pktmax > 64 ? g_pktmax - 48 : 512;
    char *pk = xmalloc(room + 64);
    while (n) {
        size_t take = 0, sz = 0;
        while (take < n) {
            unsigned char c = b[take];
            size_t need = (c == '$' || c == '#' || c == '}' || c == '*') ? 2 : 1;
            if (sz + need > room)
                break;
            sz += need; take++;
        }
        size_t len = (size_t)sprintf(pk, "vFlashWrite:%llx:", addr);
        for (size_t k = 0; k < take; k++) {
            unsigned char c = b[k];
            if (c == '$' || c == '#' || c == '}' || c == '*') {
                pk[len++] = '}';
                pk[len++] = (char)(c ^ 0x20);
            } else
                pk[len++] = (char)c;
        }
        send_packet(pk, len);
        recv_packet();
        if (strcmp(g_rep, "OK") != 0)
            die("vFlashWrite at 0x%llx: the server answered '%s'", addr, g_rep);
        addr += take; b += take; n -= take;
    }
    free(pk);
}

/* ==== reading back ======================================================== */

static int verify_range(u64 addr, const unsigned char *b, size_t n)
{
    size_t room = g_pktmax > 64 ? (g_pktmax - 32) / 2 : 256;
    if (room > 4096)
        room = 4096;
    while (n) {
        size_t take = n < room ? n : room;
        char req[64];
        snprintf(req, sizeof req, "m%llx,%zx", addr, take);
        const char *r = request(req);
        if (g_replen != take * 2)
            die("reading 0x%llx back: the server answered '%.40s'", addr, r);
        for (size_t k = 0; k < take; k++) {
            unsigned char got;
            if (unhex(r + 2 * k, 1, &got))
                die("reading 0x%llx back: not hex", addr);
            if (got != b[k]) {
                fprintf(stderr, "embflash: verify failed at 0x%llx: wrote 0x%02x, "
                        "read 0x%02x\n", addr + k, b[k], got);
                return 0;
            }
        }
        addr += take; b += take; n -= take;
    }
    return 1;
}

/* ==== registers, by name from the target description ==================== */

struct regdesc { char name[32]; int num, bits; };
static struct regdesc *g_rd;
static int g_nrd;
static int g_mprofile;              /* org.gnu.gdb.arm.m-profile */

static void read_features(const char *annex, int *next)
{
    char *x = qxfer("features", annex);
    if (!x)
        return;
    if (strstr(x, "org.gnu.gdb.arm.m-profile"))
        g_mprofile = 1;
    for (const char *p = x; (p = strstr(p, "<xi:include")) != NULL; p++) {
        const char *e = strchr(p, '>');
        char href[128];
        if (e && xml_attr(p, e, "href", href, sizeof href))
            read_features(href, next);
    }
    for (const char *p = x; (p = strstr(p, "<reg ")) != NULL; p++) {
        const char *e = strchr(p, '>');
        if (!e)
            break;
        char name[32] = "", num[16] = "", bits[16] = "";
        xml_attr(p, e, "name", name, sizeof name);
        xml_attr(p, e, "regnum", num, sizeof num);
        xml_attr(p, e, "bitsize", bits, sizeof bits);
        int n = num[0] ? atoi(num) : *next;
        *next = n + 1;
        g_rd = xrealloc(g_rd, (size_t)(g_nrd + 1) * sizeof *g_rd);
        snprintf(g_rd[g_nrd].name, sizeof g_rd[g_nrd].name, "%s", name);
        g_rd[g_nrd].num = n;
        g_rd[g_nrd].bits = bits[0] ? atoi(bits) : 32;
        g_nrd++;
    }
    free(x);
}

static const struct regdesc *reg_named(const char *a, const char *b)
{
    for (int k = 0; k < g_nrd; k++)
        if (!strcmp(g_rd[k].name, a) || (b && !strcmp(g_rd[k].name, b)))
            return &g_rd[k];
    return NULL;
}

/* write a register in the target's byte order (little-endian here: the
 * targets with an M-profile or RISC-V description are) */
static void set_reg(const struct regdesc *r, u64 v, int big)
{
    char pk[64];
    int nb = r->bits / 8;
    int len = sprintf(pk, "P%x=", r->num);
    for (int k = 0; k < nb; k++) {
        int sh = big ? 8 * (nb - 1 - k) : 8 * k;
        len += sprintf(pk + len, "%02x", (unsigned)(v >> sh) & 0xff);
    }
    request(pk);
    expect_ok("setting a register");
}

/* the image's bytes at a, n of them (for the vector table) */
static int image_word(u64 a, int n, int big, u64 *out)
{
    for (int k = 0; k < g_nch; k++) {
        const struct chunk *c = &g_ch[k];
        if (a >= c->addr && a + (u64)n <= c->addr + c->len) {
            u64 v = 0;
            for (int j = 0; j < n; j++) {
                int idx = big ? j : n - 1 - j;
                v = v << 8 | c->b[a - c->addr + (u64)idx];
            }
            *out = v;
            return 1;
        }
    }
    return 0;
}

/* ==== main ================================================================ */

static u64 parse_num(const char *s, const char *what)
{
    char *e;
    u64 v = strtoull(s, &e, 0);
    if (e == s || *e)
        die("%s: not a number: %s", what, s);
    return v;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: embflash IMAGE --gdb TARGET [--base ADDR] [--verify] [--run]\n"
        "                [--entry ADDR] [--vector-table ADDR] [--monitor CMD]...\n"
        "                [--dry-run] [--timeout SECONDS] [-v]\n"
        "  TARGET: HOST:PORT | unix:PATH | serial:DEV[@BAUD]\n"
        "  IMAGE:  ELF, Intel HEX (.hex), S-records (.srec .s19 .s37 .mot),\n"
        "          or raw bytes (.bin with --base)\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *image = NULL, *target = NULL;
    const char *monitor[16];
    int nmon = 0, verify = 0, run = 0, dry = 0, have_base = 0;
    int have_entry_opt = 0, have_vt = 0;
    u64 base = 0, entry_opt = 0, vt = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--gdb") && i + 1 < argc) target = argv[++i];
        else if (!strcmp(a, "--base") && i + 1 < argc) {
            base = parse_num(argv[++i], "--base"); have_base = 1;
        } else if (!strcmp(a, "--entry") && i + 1 < argc) {
            entry_opt = parse_num(argv[++i], "--entry"); have_entry_opt = 1;
        } else if (!strcmp(a, "--vector-table") && i + 1 < argc) {
            vt = parse_num(argv[++i], "--vector-table"); have_vt = 1;
        } else if (!strcmp(a, "--monitor") && i + 1 < argc) {
            if (nmon == 16) die("at most 16 --monitor commands");
            monitor[nmon++] = argv[++i];
        } else if (!strcmp(a, "--timeout") && i + 1 < argc)
            g_timeout_ms = (int)parse_num(argv[++i], "--timeout") * 1000;
        else if (!strcmp(a, "--verify")) verify = 1;
        else if (!strcmp(a, "--run")) run = 1;
        else if (!strcmp(a, "--dry-run")) dry = 1;
        else if (!strcmp(a, "-v")) g_verbose++;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) usage();
        else if (a[0] == '-') die("unknown option %s", a);
        else if (!image) image = a;
        else die("one image at a time (%s and %s)", image, a);
    }
    if (!image || (!target && !dry))
        usage();
    load_image(image, have_base, base);

    u64 total = 0;
    for (int k = 0; k < g_nch; k++)
        total += g_ch[k].len;
    if (dry || g_verbose) {
        printf("%s: %llu bytes in %d piece%s\n", image, total, g_nch,
               g_nch == 1 ? "" : "s");
        for (int k = 0; k < g_nch; k++)
            printf("  0x%08llx  %8zu bytes\n", g_ch[k].addr, g_ch[k].len);
        if (g_have_entry)
            printf("  entry 0x%llx\n", g_entry);
    }
    if (dry && !target)
        return 0;

    open_target(target);
    /* what the server can do, and the largest packet it takes */
    const char *sup = request("qSupported:multiprocess-;xmlRegisters=arm");
    const char *ps = strstr(sup, "PacketSize=");
    if (ps)
        g_pktmax = (size_t)strtoull(ps + 11, NULL, 16);
    if (g_pktmax < 256)
        g_pktmax = 256;
    if (g_pktmax > 65536)
        g_pktmax = 65536;
    int mmap = strstr(sup, "qXfer:memory-map:read+") != NULL;
    int feats = strstr(sup, "qXfer:features:read+") != NULL;
    int noack = strstr(sup, "QStartNoAckMode+") != NULL;
    if (noack) {
        request("QStartNoAckMode");
        if (!strcmp(g_rep, "OK"))
            g_noack = 1;
    }
    for (int k = 0; k < nmon; k++) {
        char pk[1024];
        int len = sprintf(pk, "qRcmd,");
        for (const char *c = monitor[k]; *c && len < 1000; c++)
            len += sprintf(pk + len, "%02x", (unsigned char)*c);
        request(pk);
        if (strcmp(g_rep, "OK") != 0)
            die("monitor %s: the server answered '%s'", monitor[k], g_rep);
    }
    if (mmap)
        read_memory_map();

    /* the plan: per piece, flash (by blocks) or memory */
    int nflash = 0;
    for (int k = 0; k < g_nch; k++) {
        u64 a = g_ch[k].addr, e = a + g_ch[k].len;
        for (u64 p = a; p < e; ) {
            const struct region *r = region_of(p);
            if (r && r->flash) nflash++;
            u64 next = r ? r->start + r->len : e;
            if (!r) {
                /* up to the next mapped region */
                next = e;
                for (int j = 0; j < g_nreg; j++)
                    if (g_reg[j].start > p && g_reg[j].start < next)
                        next = g_reg[j].start;
            }
            p = next < e ? next : e;
        }
    }
    if (dry) {
        printf("server: packets of %zu bytes%s%s\n", g_pktmax,
               mmap ? ", a memory map" : ", no memory map (written as memory)",
               g_nreg ? "" : "");
        for (int k = 0; k < g_nreg; k++)
            printf("  %s 0x%08llx +0x%llx%s\n", g_reg[k].flash ? "flash" : "ram  ",
                   g_reg[k].start, g_reg[k].len, "");
        return 0;
    }

    /* flash first: erase every block the image touches, write, commit */
    if (nflash) {
        for (int k = 0; k < g_nreg; k++) {
            const struct region *r = &g_reg[k];
            if (!r->flash)
                continue;
            /* the blocks of this region the image touches, in runs */
            u64 nblk = (r->len + r->block - 1) / r->block;
            char *touch = calloc((size_t)nblk ? (size_t)nblk : 1, 1);
            if (!touch) die("out of memory");
            for (int j = 0; j < g_nch; j++) {
                u64 a = g_ch[j].addr, e = a + g_ch[j].len;
                if (e <= r->start || a >= r->start + r->len)
                    continue;
                u64 lo = a > r->start ? a : r->start;
                u64 hi = e < r->start + r->len ? e : r->start + r->len;
                for (u64 b = (lo - r->start) / r->block; b * r->block + r->start < hi; b++)
                    touch[b] = 1;
            }
            for (u64 b = 0; b < nblk; ) {
                if (!touch[b]) { b++; continue; }
                u64 b0 = b;
                while (b < nblk && touch[b]) b++;
                char pk[96];
                snprintf(pk, sizeof pk, "vFlashErase:%llx,%llx",
                         r->start + b0 * r->block, (b - b0) * r->block);
                request(pk);
                if (strcmp(g_rep, "OK") != 0)
                    die("erasing 0x%llx: the server answered '%s'",
                        r->start + b0 * r->block, g_rep);
            }
            free(touch);
        }
        for (int j = 0; j < g_nch; j++) {
            u64 a = g_ch[j].addr, e = a + g_ch[j].len;
            for (int k = 0; k < g_nreg; k++) {
                const struct region *r = &g_reg[k];
                if (!r->flash || e <= r->start || a >= r->start + r->len)
                    continue;
                u64 lo = a > r->start ? a : r->start;
                u64 hi = e < r->start + r->len ? e : r->start + r->len;
                flash_write(lo, g_ch[j].b + (lo - a), (size_t)(hi - lo));
            }
        }
        request("vFlashDone");
        if (strcmp(g_rep, "OK") != 0)
            die("vFlashDone: the server answered '%s'", g_rep);
    }
    /* then everything that is not flash, as memory */
    for (int j = 0; j < g_nch; j++) {
        u64 a = g_ch[j].addr, e = a + g_ch[j].len;
        for (u64 p = a; p < e; ) {
            const struct region *r = region_of(p);
            u64 end = r ? r->start + r->len : e;
            if (!r)
                for (int k = 0; k < g_nreg; k++)
                    if (g_reg[k].start > p && g_reg[k].start < end)
                        end = g_reg[k].start;
            if (end > e) end = e;
            if (!r || !r->flash)
                write_mem(p, g_ch[j].b + (p - a), (size_t)(end - p));
            p = end;
        }
    }
    printf("embflash: wrote %llu bytes in %d piece%s%s\n", total, g_nch,
           g_nch == 1 ? "" : "s", nflash ? " (flash erased and programmed by "
           "the server)" : "");
    if (verify) {
        for (int j = 0; j < g_nch; j++)
            if (!verify_range(g_ch[j].addr, g_ch[j].b, g_ch[j].len))
                die("verification failed");
        printf("embflash: verified\n");
    }
    if (run) {
        int next = 0;
        if (feats)
            read_features("target.xml", &next);
        const struct regdesc *pc = reg_named("pc", NULL);
        if (!pc)
            die("--run: the server describes no register named pc");
        int big = g_machine && g_ebe;
        if (g_mprofile) {
            /* a Cortex-M starts from its vector table: SP at +0, the reset
             * handler at +4 (its low bit is the Thumb state, not an
             * address bit) */
            u64 base_vt = have_vt ? vt : g_ch[0].addr, sp = 0, rst = 0;
            if (!image_word(base_vt, 4, big, &sp) ||
                !image_word(base_vt + 4, 4, big, &rst))
                die("--run: no vector table in the image at 0x%llx "
                    "(--vector-table)", base_vt);
            const struct regdesc *rsp = reg_named("sp", "msp");
            const struct regdesc *xpsr = reg_named("xpsr", NULL);
            if (!rsp)
                die("--run: the server describes no register named sp");
            set_reg(rsp, sp, big);
            set_reg(pc, rst & ~1ULL, big);
            if (xpsr)
                set_reg(xpsr, 1u << 24, big);        /* the Thumb bit */
            printf("embflash: running from 0x%llx, sp 0x%llx\n", rst & ~1ULL, sp);
        } else {
            u64 e = have_entry_opt ? entry_opt : g_entry;
            if (!have_entry_opt && !g_have_entry)
                die("--run: the image has no entry point; give one with --entry");
            set_reg(pc, e, big);
            printf("embflash: running from 0x%llx\n", e);
        }
        /* continue, then let go of the target without waiting for it */
        send_packet("c", 1);
    }
    /* a detach is polite where the target is still stopped; after `c` the
     * server would only answer it once the target stops again */
    if (!run) {
        send_packet("D", 1);
        g_detached = 1;
        recv_packet();
    }
    return 0;
}
