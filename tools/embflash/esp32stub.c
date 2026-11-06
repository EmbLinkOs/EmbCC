/* embflash's flasher stub for the ESP32: loaded into RAM through the ROM
 * (MEM_BEGIN, MEM_DATA, MEM_END) and run there, it takes over the serial
 * line from the ROM's loader and writes, reads and checks the flash faster
 * than the ROM does:
 *
 *   - blocks of 16 KiB, not 1 KiB, so a block's answer is waited for a
 *     sixteenth as often;
 *   - compressed blocks: LZ4's block format, each block on its own;
 *   - the flash read back as a stream, acknowledged by the programmer,
 *     not 64 bytes per request;
 *   - the MD5 of a range computed here.
 *
 * Its packets are the ROM's: SLIP frames, a request's direction, command,
 * size and checksum, then its data; an answer's direction, command, size
 * and value, its data, then the status bytes (status, error, 0, 0). The
 * commands are the ROM's where they mean the same thing, and these:
 *
 *   0xD0 ERASE_FLASH       the whole chip
 *   0xD1 ERASE_REGION      offset, size (both whole 4 KiB sectors)
 *   0xD2 READ_FLASH        offset, size, packet size, packets in flight:
 *                          after its answer, the data as frames of the
 *                          packet size, each acknowledged by a frame of
 *                          four bytes (the count received), then the MD5
 *                          of all of it, 16 bytes
 *   0xE0 FLASH_LZ_BEGIN    as FLASH_BEGIN, the sizes uncompressed
 *   0xE1 FLASH_LZ_DATA     compressed size, sequence, size uncompressed, 0,
 *                          then the block, its checksum over what was sent
 *
 * The flash is written with the ROM's own routines, the ones its loader
 * writes with (SPIEraseSector, SPIWrite...), on the flash the ROM attached
 * (SPI_ATTACH, sent to the ROM before the stub). Each address was read off
 * the chip (a D0WD-V3) as a windowed `entry`.
 *
 * Built by EmbCC; embflash carries the bytes (esp32stub.inc, made by
 * esp32stub.sh), and tests/golden/embflash-esp.sh checks they are this
 * source's. */
#define REG(a) (*(volatile unsigned *)(a))
#define UART0_FIFO   REG(0x3FF40000u)
#define UART0_STATUS REG(0x3FF4001Cu)
#define WDT_KEY 0x50D83AA1u

typedef int (*spi_rw_fn)(unsigned addr, unsigned *buf, int len);
typedef int (*spi_n_fn)(unsigned n);
typedef int (*spi_v_fn)(void);
#define SPIRead        ((spi_rw_fn)0x40062ed8u)
#define SPIWrite       ((spi_rw_fn)0x40062d50u)
#define SPIEraseSector ((spi_n_fn)0x40062cccu)
#define SPIEraseBlock  ((spi_n_fn)0x40062c4cu)
#define SPIEraseChip   ((spi_v_fn)0x40062c14u)
#define SPIUnlock      ((spi_v_fn)0x400628b0u)

#define BLOCK 16384u
#define ERR_INVALID 0x05        /* the ROM's error codes */
#define ERR_FAILED  0x06
#define ERR_CRC     0x07
#define ERR_WRITE   0x08
#define ERR_READ    0x09

static unsigned char g_rx[BLOCK + 256] __attribute__((aligned(4)));
static unsigned char g_out[BLOCK] __attribute__((aligned(4)));
static unsigned g_buf[1024];                /* 4 KiB of flash at a time */
static unsigned g_wr_addr, g_wr_seq, g_wr_end;
static unsigned g_pending;          /* a frame READ_FLASH took that was not its ack */
static int g_wr_on, g_unlocked;

static unsigned get32(const unsigned char *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

/* ---- the line ----------------------------------------------------------- */

static void tx(unsigned char c)
{
    while (((UART0_STATUS >> 16) & 0xff) >= 126)
        ;
    UART0_FIFO = c;
}

static void tx_slip(const unsigned char *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (p[i] == 0xC0) { tx(0xDB); tx(0xDC); }
        else if (p[i] == 0xDB) { tx(0xDB); tx(0xDD); }
        else tx(p[i]);
    }
}

static int rx(void)
{
    while ((UART0_STATUS & 0xff) == 0)
        ;
    return (int)(UART0_FIFO & 0xff);
}

/* the next frame into g_rx: its length (frames too long are cut) */
static unsigned rx_frame(void)
{
    int c;
    while ((c = rx()) != 0xC0)
        ;
    unsigned n = 0;
    for (;;) {
        c = rx();
        if (c == 0xC0) {
            if (n == 0)
                continue;
            return n;
        }
        if (c == 0xDB) {
            c = rx();
            c = c == 0xDC ? 0xC0 : c == 0xDD ? 0xDB : c;
        }
        if (n < sizeof g_rx)
            g_rx[n++] = (unsigned char)c;
    }
}

/* an answer: the header, DATA, then the status bytes */
static void answer(unsigned cmd, unsigned value, const unsigned char *data, unsigned n, int err)
{
    unsigned char h[8] = { 1, (unsigned char)cmd, (unsigned char)((n + 4) & 0xff),
                           (unsigned char)((n + 4) >> 8), (unsigned char)value,
                           (unsigned char)(value >> 8), (unsigned char)(value >> 16),
                           (unsigned char)(value >> 24) };
    unsigned char st[4] = { (unsigned char)(err != 0), (unsigned char)err, 0, 0 };
    tx(0xC0);
    tx_slip(h, 8);
    if (n)
        tx_slip(data, n);
    tx_slip(st, 4);
    tx(0xC0);
}

/* ---- MD5 (RFC 1321), a block at a time ---------------------------------- */

struct md5 { unsigned h[4], len; unsigned char buf[64]; unsigned n; };
static const unsigned md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
static const unsigned char md5_r[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

static void md5_block(struct md5 *m, const unsigned char *p)
{
    unsigned w[16], a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3];
    for (int i = 0; i < 16; i++)
        w[i] = get32(p + 4 * i);
    for (int i = 0; i < 64; i++) {
        unsigned f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        unsigned t = d, x = a + f + md5_k[i] + w[g], s = md5_r[(i >> 4) * 4 + (i & 3)];
        d = c;
        c = b;
        b = b + ((x << s) | (x >> (32 - s)));
        a = t;
    }
    m->h[0] += a; m->h[1] += b; m->h[2] += c; m->h[3] += d;
}

static void md5_init(struct md5 *m)
{
    m->h[0] = 0x67452301; m->h[1] = 0xefcdab89; m->h[2] = 0x98badcfe; m->h[3] = 0x10325476;
    m->len = 0;
    m->n = 0;
}

static void md5_add(struct md5 *m, const unsigned char *p, unsigned n)
{
    m->len += n;
    while (n) {
        unsigned k = 64 - m->n < n ? 64 - m->n : n;
        for (unsigned i = 0; i < k; i++)
            m->buf[m->n + i] = p[i];
        m->n += k;
        p += k;
        n -= k;
        if (m->n == 64) {
            md5_block(m, m->buf);
            m->n = 0;
        }
    }
}

static void md5_end(struct md5 *m, unsigned char out[16])
{
    unsigned bits = m->len * 8, hi = m->len >> 29;
    unsigned char pad = 0x80, z = 0;
    md5_add(m, &pad, 1);
    while (m->n != 56)
        md5_add(m, &z, 1);
    unsigned char l[8] = { (unsigned char)bits, (unsigned char)(bits >> 8),
                           (unsigned char)(bits >> 16), (unsigned char)(bits >> 24),
                           (unsigned char)hi, 0, 0, 0 };
    md5_add(m, l, 8);
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++)
            out[4 * i + k] = (unsigned char)(m->h[i] >> (8 * k));
}

/* ---- LZ4's block format ----------------------------------------------------
 * Sequences of a token (literals in the high nibble, the match's length less
 * four in the low; 15 continues in bytes until one is not 255), the literals,
 * and the match's offset back (two bytes, little-endian). The last sequence
 * is literals only. -1 for a block that does not decode within the room. */
static int lz4_decode(const unsigned char *s, unsigned sn, unsigned char *d, unsigned dn)
{
    const unsigned char *se = s + sn;
    unsigned char *d0 = d, *de = d + dn;
    while (s < se) {
        unsigned t = *s++, lit = t >> 4, c;
        if (lit == 15)
            do {
                if (s >= se)
                    return -1;
                c = *s++;
                lit += c;
            } while (c == 255);
        if (lit > (unsigned)(se - s) || lit > (unsigned)(de - d))
            return -1;
        while (lit--)
            *d++ = *s++;
        if (s >= se)
            break;
        if (se - s < 2)
            return -1;
        unsigned off = (unsigned)s[0] | (unsigned)s[1] << 8, ml = t & 15;
        s += 2;
        if (off == 0 || off > (unsigned)(d - d0))
            return -1;
        if (ml == 15)
            do {
                if (s >= se)
                    return -1;
                c = *s++;
                ml += c;
            } while (c == 255);
        ml += 4;
        if (ml > (unsigned)(de - d))
            return -1;
        const unsigned char *m = d - off;
        while (ml--)
            *d++ = *m++;
    }
    return (int)(d - d0);
}

/* ---- the flash ------------------------------------------------------------ */

/* After an erase, the flash's own status (SPI1's read-status command,
 * RDSR, SPI_CMD bit 27) until its write-in-progress bit is clear: the
 * answer then means the erase is done, whatever the flash. On the
 * D0WD-V3 board's flash a 64 KiB block erases in under 2 ms (the ROM's
 * call returns by then, and a write issued at once does not wait), so
 * there it is seen to pass at once. Bounded: a part that never says so is
 * an error, not a hang. */
#define SPI1_CMD       REG(0x3FF42000u)
#define SPI1_RD_STATUS REG(0x3FF42010u)
static int flash_wait(void)
{
    for (unsigned long n = 0; n < 400000000ul; n++) {
        SPI1_RD_STATUS = 0;
        SPI1_CMD = 1u << 27;
        while (SPI1_CMD != 0)
            ;
        if ((SPI1_RD_STATUS & 1) == 0)
            return 0;
    }
    return -1;
}

static int unlock(void)
{
    if (!g_unlocked && SPIUnlock() != 0)
        return -1;
    g_unlocked = 1;
    return 0;
}

/* whole 4 KiB sectors from addr, 64 KiB blocks where they fit */
static int erase(unsigned addr, unsigned size)
{
    unsigned s = addr / 4096, e = (addr + size + 4095) / 4096;
    if (unlock())
        return -1;
    while (s < e) {
        if ((s & 15) == 0 && e - s >= 16) {
            if (SPIEraseBlock(s / 16) != 0)
                return -1;
            s += 16;
        } else {
            if (SPIEraseSector(s) != 0)
                return -1;
            s++;
        }
    }
    return flash_wait();
}

/* N bytes at P to the region FLASH_BEGIN opened, whole words */
static int write_next(const unsigned char *p, unsigned n)
{
    unsigned w = (n + 3) & ~3u;
    if (g_wr_addr + w > g_wr_end + 4096)    /* the erased region, its last sector whole */
        return -1;
    for (unsigned i = n; i < w; i++)
        ((unsigned char *)p)[i] = 0xFF;
    if (SPIWrite(g_wr_addr, (unsigned *)p, (int)w) != 0)
        return -1;
    g_wr_addr += w;
    return 0;
}

static int read_flash(unsigned addr, unsigned n)
{
    return SPIRead(addr, g_buf, (int)((n + 3) & ~3u)) == 0 ? 0 : -1;
}

/* ---- the commands ------------------------------------------------------- */

static void do_read(unsigned off, unsigned size, unsigned pkt, unsigned inflight)
{
    struct md5 m;
    unsigned sent = 0, acked = 0;
    if (pkt == 0 || pkt > sizeof g_buf || inflight == 0) {
        answer(0xD2, 0, 0, 0, ERR_INVALID);
        return;
    }
    answer(0xD2, 0, 0, 0, 0);
    md5_init(&m);
    while (acked < size) {
        if (sent < size && sent - acked < pkt * inflight) {
            unsigned n = size - sent < pkt ? size - sent : pkt;
            if (read_flash(off + sent, n) != 0) {
                answer(0xD2, 0, 0, 0, ERR_READ);
                return;
            }
            tx(0xC0);
            tx_slip((const unsigned char *)g_buf, n);
            tx(0xC0);
            md5_add(&m, (const unsigned char *)g_buf, n);
            sent += n;
            continue;
        }
        /* an acknowledgement; anything else ends the read, and is the
         * next command (the programmer has given up on this one) */
        unsigned n = rx_frame();
        if (n != 4) {
            g_pending = n;
            return;
        }
        acked = get32(g_rx);
    }
    unsigned char dg[16];
    md5_end(&m, dg);
    tx(0xC0);
    tx_slip(dg, 16);
    tx(0xC0);
}

static void command(unsigned n)
{
    if (n < 8 || g_rx[0] != 0) {
        answer(n >= 2 ? g_rx[1] : 0, 0, 0, 0, ERR_INVALID);
        return;
    }
    unsigned cmd = g_rx[1], size = (unsigned)g_rx[2] | (unsigned)g_rx[3] << 8;
    unsigned csum = get32(g_rx + 4);
    unsigned char *d = g_rx + 8;
    if (8 + size != n) {
        answer(cmd, 0, 0, 0, ERR_INVALID);
        return;
    }
    switch (cmd) {
    case 0x08:                                  /* SYNC: still here */
        answer(cmd, 0, 0, 0, 0);
        return;
    case 0x02:                                  /* FLASH_BEGIN */
    case 0xE0: {                                /* FLASH_LZ_BEGIN */
        if (size < 16) break;
        unsigned esize = get32(d), off = get32(d + 12);
        if (off % 4096 || get32(d + 8) > BLOCK) {
            answer(cmd, 0, 0, 0, ERR_INVALID);
            return;
        }
        if (erase(off, esize) != 0) {
            answer(cmd, 0, 0, 0, ERR_FAILED);
            return;
        }
        g_wr_addr = off;
        g_wr_end = off + esize;
        g_wr_seq = 0;
        g_wr_on = 1;
        answer(cmd, 0, 0, 0, 0);
        return;
    }
    case 0x03:                                  /* FLASH_DATA */
    case 0xE1: {                                /* FLASH_LZ_DATA */
        if (size < 16 || !g_wr_on) break;
        unsigned len = get32(d), seq = get32(d + 4), ulen = get32(d + 8);
        const unsigned char *p = d + 16;
        unsigned sum = 0xEF;
        if (len != size - 16 || seq != g_wr_seq) {
            answer(cmd, 0, 0, 0, ERR_INVALID);
            return;
        }
        for (unsigned i = 0; i < len; i++)
            sum ^= p[i];
        if ((sum & 0xff) != (csum & 0xff)) {
            answer(cmd, 0, 0, 0, ERR_CRC);
            return;
        }
        if (cmd == 0xE1) {
            if (ulen > BLOCK || lz4_decode(p, len, g_out, BLOCK) != (int)ulen) {
                answer(cmd, 0, 0, 0, ERR_INVALID);
                return;
            }
            p = g_out;
            len = ulen;
        } else if (len > BLOCK) {
            answer(cmd, 0, 0, 0, ERR_INVALID);
            return;
        }
        if (write_next(p, len) != 0) {
            answer(cmd, 0, 0, 0, ERR_WRITE);
            return;
        }
        g_wr_seq++;
        answer(cmd, 0, 0, 0, 0);
        return;
    }
    case 0x04:                                  /* FLASH_END: written as it came */
        g_wr_on = 0;
        answer(cmd, 0, 0, 0, 0);
        return;
    case 0x13: {                                /* SPI_FLASH_MD5 */
        if (size < 16) break;
        unsigned off = get32(d), len = get32(d + 4);
        struct md5 m;
        md5_init(&m);
        for (unsigned at = 0; at < len; ) {
            unsigned k = len - at < sizeof g_buf ? len - at : sizeof g_buf;
            if (read_flash(off + at, k) != 0) {
                answer(cmd, 0, 0, 0, ERR_READ);
                return;
            }
            md5_add(&m, (const unsigned char *)g_buf, k);
            at += k;
        }
        unsigned char dg[16];
        md5_end(&m, dg);
        answer(cmd, 0, dg, 16, 0);
        return;
    }
    case 0xD0:                                  /* ERASE_FLASH */
        answer(cmd, 0, 0, 0, unlock() == 0 && SPIEraseChip() == 0 && flash_wait() == 0
                             ? 0 : ERR_FAILED);
        return;
    case 0xD1:                                  /* ERASE_REGION */
        if (size < 8) break;
        if (get32(d) % 4096 || get32(d + 4) % 4096) {
            answer(cmd, 0, 0, 0, ERR_INVALID);
            return;
        }
        answer(cmd, 0, 0, 0, erase(get32(d), get32(d + 4)) == 0 ? 0 : ERR_FAILED);
        return;
    case 0xD2:                                  /* READ_FLASH */
        if (size < 16) break;
        do_read(get32(d), get32(d + 4), get32(d + 8), get32(d + 12));
        return;
    default:
        break;
    }
    answer(cmd, 0, 0, 0, ERR_INVALID);
}

extern char __bss_start[], __bss_end[];

/* MEM_END's jump: interrupts off (the ROM's loader reads the line in an
 * interrupt, which would take the stub's bytes), the watchdogs off (a chip
 * erase is longer than they wait), .bss zeroed, the greeting -- a frame
 * "EMBS" -- and then the commands, for ever. */
__attribute__((section(".text.stub_main"))) void stub_main(void)
{
    __asm__ volatile("movi a2, 0\n\twsr a2, intenable\n\trsync" ::: "a2");
    REG(0x3FF480A4u) = WDT_KEY; REG(0x3FF4808Cu) = 0; REG(0x3FF480A4u) = 0;
    REG(0x3FF5F064u) = WDT_KEY; REG(0x3FF5F048u) = 0; REG(0x3FF5F064u) = 0;
    for (char *p = __bss_start; p < __bss_end; p++)
        *p = 0;
    static const unsigned char hi[4] = { 'E', 'M', 'B', 'S' };
    tx(0xC0);
    tx_slip(hi, 4);
    tx(0xC0);
    for (;;) {
        unsigned n = g_pending ? g_pending : rx_frame();
        g_pending = 0;
        command(n);
    }
}
