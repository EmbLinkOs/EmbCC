#include "emit.h"

#include "../../driver/util.h"

/* One place appends, so nothing else can get the halfword order wrong.
 * AVR is little-endian and a 32-bit instruction is two halfwords in
 * program order, each little-endian in itself. */
static void hw(struct code *c, unsigned v)
{
    code_byte(c, (int)(v & 0xff));
    code_byte(c, (int)((v >> 8) & 0xff));
}

static void bad(const char *what, int got)
{
    internal_error("avr: %s (got %d) -- a wrong register here encodes a "
                   "DIFFERENT one and assembles cleanly", what, got);
}

/* ---- the two-register group ------------------------------------------
 *
 * 0000 opRd dddd rrrr: `d` is five bits split as bit 8 plus bits 7..4,
 * and `r` is five bits split as bit 9 plus bits 3..0. Eleven
 * instructions differ only in the opcode field, `mul` included. */
void avr_rr(struct code *c, enum avr_rr op, int d, int r)
{
    if (d < 0 || d > 31) bad("a destination register outside r0-r31", d);
    if (r < 0 || r > 31) bad("a source register outside r0-r31", r);
    hw(c, (unsigned)op | ((unsigned)(d & 0x1f) << 4) |
          ((unsigned)(r & 0x10) << 5) | (unsigned)(r & 0x0f));
}

/* ---- the immediate group ---------------------------------------------
 *
 * opKK dddd KKKK, and `dddd` is d-16: these reach ONLY r16-r31. That is
 * the tightest constraint on this machine and the reason a register
 * allocator here cannot treat all 32 registers alike -- a constant
 * cannot be loaded into r0-r15 at all without going through another
 * register. */
void avr_ri(struct code *c, enum avr_ri op, int d, int k)
{
    if (d < 16 || d > 31)
        bad("an immediate-form register outside r16-r31 (ldi and friends "
            "cannot reach r0-r15)", d);
    if (k < 0 || k > 255) bad("an 8-bit immediate out of range", k);
    hw(c, (unsigned)op | ((unsigned)(k & 0xf0) << 4) |
          ((unsigned)((d - 16) & 0x0f) << 4) | (unsigned)(k & 0x0f));
}

/* ---- the single-register group ---------------------------------------
 *
 * 1001 010d dddd ssss, where ssss picks the operation. There is no `lsl`
 * and no `rol` here: shifting LEFT by one is `add rd, rd`, which is the
 * same instruction, and the assembler spells it lsl as a courtesy. */
void avr_r1(struct code *c, enum avr_r1 op, int d)
{
    if (d < 0 || d > 31) bad("a register outside r0-r31", d);
    hw(c, 0x9400u | ((unsigned)(d & 0x1f) << 4) | (unsigned)op);
}

/* ---- 16-bit constant add/subtract, and the 16-bit move ---------------
 *
 * adiw/sbiw: 1001 011s KKdd KKKK. `dd` is (d-24)/2, so the ONLY
 * destinations are r24, r26, r28 and r30 -- the four high pairs. This is
 * how a pointer is advanced and how a 16-bit counter is stepped, and it
 * is why those four pairs are worth keeping free. */
static void adiw_sbiw(struct code *c, unsigned base, int d, int k)
{
    if (d != 24 && d != 26 && d != 28 && d != 30)
        bad("adiw/sbiw on a pair other than r24, r26, r28 or r30", d);
    if (k < 0 || k > 63) bad("adiw/sbiw immediate outside 0..63", k);
    /* 1001 011s KKdd KKKK: K is SPLIT -- its high two bits at 7:6 and its
     * low four at 3:0 -- with dd between them at 5:4. Reading it as
     * `dd` then a contiguous six-bit K gives the same bytes for
     * (r24, 3) and (r30, 63), which is what my first two hand-checks
     * happened to be; tools/avrcheck's (r26, 1) is what told them
     * apart. */
    hw(c, base | ((unsigned)(k & 0x30) << 2) |
          ((unsigned)((d - 24) / 2) << 4) | (unsigned)(k & 0x0f));
}

void avr_adiw(struct code *c, int d, int k) { adiw_sbiw(c, 0x9600u, d, k); }
void avr_sbiw(struct code *c, int d, int k) { adiw_sbiw(c, 0x9700u, d, k); }

/* movw copies a PAIR in one instruction, and both operands are pair
 * numbers -- d/2 and r/2 -- so both must be even. It is the only way to
 * move sixteen bits without two instructions. */
void avr_movw(struct code *c, int d, int r)
{
    if (d & 1) bad("movw to an odd register (it moves a PAIR)", d);
    if (r & 1) bad("movw from an odd register (it moves a PAIR)", r);
    hw(c, 0x0100u | ((unsigned)(d / 2) << 4) | (unsigned)(r / 2));
}

/* ---- memory ----------------------------------------------------------
 *
 * The low nibble encodes both WHICH pointer and how it moves, and the
 * three pointers are not contiguous in it -- X is 0xC, Y is 0x8, Z is
 * 0x0 -- which is why this is a switch and not arithmetic on the
 * register number. */
void avr_ldd(struct code *c, int d, int ptr, int q);
void avr_std(struct code *c, int ptr, int q, int r);

/* The post-increment and pre-decrement nibbles, for the 0x9000 group.
 *
 * PLAIN Y and Z are NOT in this group at all: `ld rd, Y` is encoded as
 * `ldd rd, Y+0` and `ld rd, Z` as `ldd rd, Z+0`, in the displaced group.
 * Only X has a plain form here.
 *
 * That is not a nicety. Nibble 0x0 in the 0x9000 group -- which is where
 * a naive "Z with no mode" lands -- IS `lds`, a THIRTY-TWO bit
 * instruction with an address halfword after it. Encoding `ld rd, Z` that
 * way emits half an instruction and everything after it is garbage.
 * tools/avrcheck caught it; nothing about the shape of the code would
 * have. */
static unsigned ptr_nibble(int ptr, enum avr_ptr_mode m)
{
    if (m == AVR_PTR_NONE) {
        if (ptr == AVR_X) return 0xC;
        bad("plain Y or Z routed through the 0x9000 group -- they are "
            "ldd/std with a zero displacement", ptr);
        return 0;
    }
    switch (ptr) {
    case AVR_X: return m == AVR_PTR_POST_INC ? 0xDu : 0xEu;
    case AVR_Y: return m == AVR_PTR_POST_INC ? 0x9u : 0xAu;
    case AVR_Z: return m == AVR_PTR_POST_INC ? 0x1u : 0x2u;
    default:
        bad("a memory access through a register that is not X, Y or Z", ptr);
        return 0;
    }
}

void avr_ld(struct code *c, int d, int ptr, enum avr_ptr_mode m)
{
    if (d < 0 || d > 31) bad("a register outside r0-r31", d);
    /* Plain Y and Z are the displaced form at zero -- see ptr_nibble. */
    if (m == AVR_PTR_NONE && ptr != AVR_X) {
        avr_ldd(c, d, ptr, 0);
        return;
    }
    hw(c, 0x9000u | ((unsigned)(d & 0x1f) << 4) | ptr_nibble(ptr, m));
}

void avr_st(struct code *c, int ptr, int r, enum avr_ptr_mode m)
{
    if (r < 0 || r > 31) bad("a register outside r0-r31", r);
    if (m == AVR_PTR_NONE && ptr != AVR_X) {
        avr_std(c, ptr, 0, r);
        return;
    }
    hw(c, 0x9200u | ((unsigned)(r & 0x1f) << 4) | ptr_nibble(ptr, m));
}

/* Displaced access: 10q0 qq0d dddd yqqq, and the six-bit displacement is
 * scattered across three fields. Only Y and Z -- there is no `ldd` off
 * X, which is the one asymmetry between the three pointers and the
 * reason X is usually the one used for a moving cursor. */
static void ldd_std(struct code *c, unsigned base, int d, int ptr, int q)
{
    unsigned y;
    if (ptr == AVR_Y)      y = 1;
    else if (ptr == AVR_Z) y = 0;
    else { bad("a displaced access off X (only Y and Z have one)", ptr); return; }
    if (q < 0 || q > 63) bad("a displacement outside 0..63", q);
    if (d < 0 || d > 31) bad("a register outside r0-r31", d);
    hw(c, base | ((unsigned)(q & 0x20) << 8) | ((unsigned)(q & 0x18) << 7) |
          ((unsigned)(d & 0x1f) << 4) | (y << 3) | (unsigned)(q & 0x07));
}

void avr_ldd(struct code *c, int d, int ptr, int q)
    { ldd_std(c, 0x8000u, d, ptr, q); }
void avr_std(struct code *c, int ptr, int q, int r)
    { ldd_std(c, 0x8200u, r, ptr, q); }

/* Absolute, and the only 32-bit DATA access: the address is a full
 * sixteen bits in the second halfword. */
void avr_lds(struct code *c, int d, int addr)
{
    if (d < 0 || d > 31) bad("a register outside r0-r31", d);
    hw(c, 0x9000u | ((unsigned)(d & 0x1f) << 4));
    hw(c, (unsigned)addr & 0xffffu);
}

void avr_sts(struct code *c, int addr, int r)
{
    if (r < 0 || r > 31) bad("a register outside r0-r31", r);
    hw(c, 0x9200u | ((unsigned)(r & 0x1f) << 4));
    hw(c, (unsigned)addr & 0xffffu);
}

/* PROGRAM SPACE. This is the Harvard half: `ld` reads SRAM and `lpm`
 * reads flash, and the same numeric address means different bytes to the
 * two. Anything the compiler leaves in flash and later dereferences has
 * to come through here. */
void avr_lpm(struct code *c, int d, int post_inc)
{
    if (d < 0 || d > 31) bad("a register outside r0-r31", d);
    hw(c, 0x9004u | ((unsigned)(d & 0x1f) << 4) | (post_inc ? 1u : 0u));
}

/* ---- I/O space ------------------------------------------------------- */
void avr_in(struct code *c, int d, int addr)
{
    if (addr < 0 || addr > 63) bad("an I/O address outside 0..63", addr);
    if (d < 0 || d > 31) bad("a register outside r0-r31", d);
    hw(c, 0xB000u | ((unsigned)(addr & 0x30) << 5) |
          ((unsigned)(d & 0x1f) << 4) | (unsigned)(addr & 0x0f));
}

void avr_out(struct code *c, int addr, int r)
{
    if (addr < 0 || addr > 63) bad("an I/O address outside 0..63", addr);
    if (r < 0 || r > 31) bad("a register outside r0-r31", r);
    hw(c, 0xB800u | ((unsigned)(addr & 0x30) << 5) |
          ((unsigned)(r & 0x1f) << 4) | (unsigned)(addr & 0x0f));
}

/* cbi/sbi reach only the LOW 32 I/O addresses, not all 64 -- a
 * distinction that matters because the timer and UART registers on an
 * ATmega328P are above it and cannot be bit-set this way. */
static void cbi_sbi(struct code *c, unsigned base, int addr, int bit)
{
    if (addr < 0 || addr > 31)
        bad("cbi/sbi above I/O address 31 (they reach only the low 32)", addr);
    if (bit < 0 || bit > 7) bad("a bit number outside 0..7", bit);
    hw(c, base | ((unsigned)(addr & 0x1f) << 3) | (unsigned)(bit & 7));
}

void avr_cbi(struct code *c, int addr, int bit) { cbi_sbi(c, 0x9800u, addr, bit); }
void avr_sbi(struct code *c, int addr, int bit) { cbi_sbi(c, 0x9A00u, addr, bit); }

/* ---- stack ----------------------------------------------------------- */
void avr_push(struct code *c, int r)
{
    if (r < 0 || r > 31) bad("a register outside r0-r31", r);
    hw(c, 0x920Fu | ((unsigned)(r & 0x1f) << 4));
}

void avr_pop(struct code *c, int r)
{
    if (r < 0 || r > 31) bad("a register outside r0-r31", r);
    hw(c, 0x900Fu | ((unsigned)(r & 0x1f) << 4));
}

/* ---- control flow ----------------------------------------------------
 *
 * Every displacement here is in WORDS and measured from the instruction
 * AFTER the branch. Both halves of that matter: a byte count encodes a
 * jump to twice the intended distance, and forgetting the +1 lands one
 * instruction short.
 */
int avr_br(struct code *c, enum avr_cond cond, int word_disp)
{
    int at = c->len;
    if (word_disp < -64 || word_disp > 63)
        bad("a conditional branch further than +-64 words (it needs a "
            "jump)", word_disp);
    /* 1111 0Skk kkkk ksss: S is the sense, sss the flag. The pairs in
     * enum avr_cond differ only in S, which is why one encoder serves
     * both of each. */
    hw(c, 0xF000u | (((unsigned)cond & 1u) << 10) |
          (((unsigned)word_disp & 0x7fu) << 3) | (((unsigned)cond >> 1) & 7u));
    return at;
}

/* ---- patching an already-emitted field --------------------------------
 *
 * The LINKER patches three of these fields too, and it works on raw bytes
 * of a section image rather than on a struct code. So the split layouts
 * live here, once, in functions that take a byte pointer -- and the
 * struct code forms below are two lines each on top of them. Writing the
 * layouts down a second time in src/link/link.c is exactly the
 * duplication that put a wrong `adiw` in this file to begin with. */
static unsigned rdhw(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static void wrhw(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

void avr_patch_br_at(unsigned char *p, int word_disp)
{
    wrhw(p, (rdhw(p) & ~0x03F8u) | (((unsigned)word_disp & 0x7fu) << 3));
}

void avr_patch_rjmp_at(unsigned char *p, int word_disp)
{
    wrhw(p, (rdhw(p) & 0xF000u) | ((unsigned)word_disp & 0x0fffu));
}

/* ldi's eight-bit immediate is SPLIT: the high nibble at bits 11:8 and
 * the low nibble at bits 3:0, with the register between them. Writing it
 * as one contiguous field would encode a different constant AND a
 * different register. */
void avr_patch_ldi_at(unsigned char *p, int k)
{
    wrhw(p, (rdhw(p) & ~0x0F0Fu) | (((unsigned)k & 0xf0u) << 4) |
            ((unsigned)k & 0x0fu));
}

/* The 22-bit word address of a 32-bit jmp/call: bits 21:17 at the first
 * halfword's 8:4, bit 16 at its bit 0, and bits 15:0 in the second
 * halfword. The operand is a BYTE address, halved here as everywhere
 * else in this file. */
void avr_patch_call_at(unsigned char *p, long byte_addr)
{
    unsigned long w = (unsigned long)byte_addr >> 1;
    wrhw(p, (rdhw(p) & ~0x01F1u) | (unsigned)((w >> 17) & 0x1fu) << 4 |
            (unsigned)((w >> 16) & 1u));
    wrhw(p + 2, (unsigned)(w & 0xffffu));
}

void avr_patch_br(struct code *c, int at, int word_disp)
{
    avr_patch_br_at(c->p + at, word_disp);
}

int avr_rjmp(struct code *c, int word_disp)
{
    int at = c->len;
    hw(c, 0xC000u | ((unsigned)word_disp & 0x0fffu));
    return at;
}

int avr_rcall(struct code *c, int word_disp)
{
    int at = c->len;
    hw(c, 0xD000u | ((unsigned)word_disp & 0x0fffu));
    return at;
}

void avr_patch_rjmp(struct code *c, int at, int word_disp)
{
    avr_patch_rjmp_at(c->p + at, word_disp);
}

/* The 32-bit forms take a WORD address, and every caller has a byte
 * address -- so the halving happens here, once, rather than at each call
 * site where half of them would forget. */
static void jmp_call(struct code *c, unsigned base, long byte_addr)
{
    unsigned long w = (unsigned long)byte_addr >> 1;
    if (byte_addr & 1)
        bad("a jump to an odd address (instructions are halfword-aligned)",
            (int)byte_addr);
    hw(c, base | (unsigned)((w >> 17) & 0x1fu) << 4 | (unsigned)((w >> 16) & 1u));
    hw(c, (unsigned)(w & 0xffffu));
}

void avr_jmp(struct code *c, long byte_addr)  { jmp_call(c, 0x940Cu, byte_addr); }
void avr_call(struct code *c, long byte_addr) { jmp_call(c, 0x940Eu, byte_addr); }
void avr_ret(struct code *c)   { hw(c, 0x9508u); }
/* 1001 0100 0sss 1000 sets, 1001 0100 1sss 1000 clears. `sei` is bset 7
 * and `cli` is bclr 7; nothing here special-cases them. */
void avr_bset(struct code *c, enum avr_sreg_bit b)
{ hw(c, 0x9408u | ((unsigned)b << 4)); }
void avr_bclr(struct code *c, enum avr_sreg_bit b)
{ hw(c, 0x9488u | ((unsigned)b << 4)); }
void avr_reti(struct code *c)  { hw(c, 0x9518u); }
void avr_nop(struct code *c)   { hw(c, 0x0000u); }
void avr_ijmp(struct code *c)  { hw(c, 0x9409u); }
void avr_icall(struct code *c) { hw(c, 0x9509u); }

/* ---- the vocabulary, for the referee ----------------------------------
 *
 * One line per form, generated from the same tables that encode, so an
 * instruction added above appears here without anyone remembering to add
 * it. tools/avrcheck feeds these lines to llvm-mc and compares the
 * bytes.
 *
 * The registers below are chosen to exercise both halves of every split
 * field: an r0-r15 and an r16-r31 operand where both are legal, an odd
 * and an even register, the lowest and highest immediate, and all three
 * pointers. A field that is only ever tested with one value is a field
 * whose encoding has not been checked -- which is how the first VFP
 * encoder passed with two wrong opcodes.
 */
struct rr_name { enum avr_rr op; const char *name; };
struct ri_name { enum avr_ri op; const char *name; };
struct r1_name { enum avr_r1 op; const char *name; };

static const struct rr_name g_rr[] = {
    { AVR_ADD, "add" }, { AVR_ADC, "adc" }, { AVR_SUB, "sub" },
    { AVR_CPSE, "cpse" },
    { AVR_SBC, "sbc" }, { AVR_AND, "and" }, { AVR_OR,  "or"  },
    { AVR_EOR, "eor" }, { AVR_MOV, "mov" }, { AVR_CP,  "cp"  },
    { AVR_CPC, "cpc" }, { AVR_MUL, "mul" }
};
static const struct ri_name g_ri[] = {
    { AVR_LDI, "ldi" }, { AVR_SUBI, "subi" }, { AVR_SBCI, "sbci" },
    { AVR_ANDI, "andi" }, { AVR_ORI, "ori" }, { AVR_CPI, "cpi" }
};
static const struct r1_name g_r1[] = {
    { AVR_COM, "com" }, { AVR_NEG, "neg" }, { AVR_SWAP, "swap" },
    { AVR_INC, "inc" }, { AVR_ASR, "asr" }, { AVR_LSR, "lsr" },
    { AVR_ROR, "ror" }, { AVR_DEC, "dec" }
};

/* Emit every form, either as text (f != NULL) or as bytes (c != NULL).
 * ONE function does both so the two cannot drift: a form printed but not
 * encoded, or encoded but not printed, would shift every comparison
 * after it and report the wrong instruction. */
static void avr_walk(FILE *f, struct code *c)
{
    static const int dpairs[4] = { 0, 1, 16, 31 };
    static const int spairs[4] = { 0, 15, 17, 31 };
    unsigned i;
    int k;

    for (i = 0; i < sizeof g_rr / sizeof g_rr[0]; i++)
        for (k = 0; k < 4; k++) {
            int d = dpairs[k], r = spairs[k];
            /* mul's operands are unrestricted like the rest; it is in
             * this table because it shares the layout. */
            if (f) fprintf(f, "%s r%d, r%d\n", g_rr[i].name, d, r);
            else   avr_rr(c, g_rr[i].op, d, r);
        }
    for (i = 0; i < sizeof g_ri / sizeof g_ri[0]; i++) {
        static const int ks[4] = { 0, 1, 42, 255 };
        for (k = 0; k < 4; k++) {
            int d = 16 + (k * 5);              /* 16, 21, 26, 31 */
            if (f) fprintf(f, "%s r%d, %d\n", g_ri[i].name, d, ks[k]);
            else   avr_ri(c, g_ri[i].op, d, ks[k]);
        }
    }
    for (i = 0; i < sizeof g_r1 / sizeof g_r1[0]; i++)
        for (k = 0; k < 4; k++) {
            int d = dpairs[k];
            if (f) fprintf(f, "%s r%d\n", g_r1[i].name, d);
            else   avr_r1(c, g_r1[i].op, d);
        }
    {
        static const int aw[4] = { 24, 26, 28, 30 };
        static const int akv[4] = { 0, 1, 32, 63 };
        for (k = 0; k < 4; k++) {
            if (f) fprintf(f, "adiw r%d, %d\n", aw[k], akv[k]);
            else   avr_adiw(c, aw[k], akv[k]);
            if (f) fprintf(f, "sbiw r%d, %d\n", aw[k], akv[k]);
            else   avr_sbiw(c, aw[k], akv[k]);
        }
        for (k = 0; k < 4; k++) {
            int d = k * 8, r = 30 - k * 8;      /* 0,8,16,24 / 30,22,14,6 */
            if (f) fprintf(f, "movw r%d, r%d\n", d, r & ~1);
            else   avr_movw(c, d, r & ~1);
        }
    }
    {
        static const int ptrs[3] = { AVR_X, AVR_Y, AVR_Z };
        static const char *pn[3] = { "X", "Y", "Z" };
        int p;
        for (p = 0; p < 3; p++)
            for (k = 0; k < 3; k++) {
                int d = k == 0 ? 0 : k == 1 ? 17 : 31;
                if (f) fprintf(f, "ld r%d, %s\n", d, pn[p]);
                else   avr_ld(c, d, ptrs[p], AVR_PTR_NONE);
                if (f) fprintf(f, "ld r%d, %s+\n", d, pn[p]);
                else   avr_ld(c, d, ptrs[p], AVR_PTR_POST_INC);
                if (f) fprintf(f, "ld r%d, -%s\n", d, pn[p]);
                else   avr_ld(c, d, ptrs[p], AVR_PTR_PRE_DEC);
                if (f) fprintf(f, "st %s, r%d\n", pn[p], d);
                else   avr_st(c, ptrs[p], d, AVR_PTR_NONE);
                if (f) fprintf(f, "st %s+, r%d\n", pn[p], d);
                else   avr_st(c, ptrs[p], d, AVR_PTR_POST_INC);
                if (f) fprintf(f, "st -%s, r%d\n", pn[p], d);
                else   avr_st(c, ptrs[p], d, AVR_PTR_PRE_DEC);
            }
        /* Displaced: Y and Z only, and the displacement's three fields
         * mean 0, 7, 8 and 63 are all worth trying -- they are the
         * boundaries between them. */
        for (p = 1; p < 3; p++) {
            static const int qs[4] = { 0, 7, 8, 63 };
            for (k = 0; k < 4; k++) {
                if (f) fprintf(f, "ldd r%d, %s+%d\n", 17, pn[p], qs[k]);
                else   avr_ldd(c, 17, ptrs[p], qs[k]);
                if (f) fprintf(f, "std %s+%d, r%d\n", pn[p], qs[k], 17);
                else   avr_std(c, ptrs[p], qs[k], 17);
            }
        }
    }
    {
        static const int as[3] = { 0, 0x100, 0xffff };
        for (k = 0; k < 3; k++) {
            if (f) fprintf(f, "lds r%d, %d\n", 17, as[k]);
            else   avr_lds(c, 17, as[k]);
            if (f) fprintf(f, "sts %d, r%d\n", as[k], 17);
            else   avr_sts(c, as[k], 17);
        }
        if (f) fprintf(f, "lpm r%d, Z\n", 17); else avr_lpm(c, 17, 0);
        if (f) fprintf(f, "lpm r%d, Z+\n", 17); else avr_lpm(c, 17, 1);
    }
    {
        static const int ios[4] = { 0, 15, 16, 63 };
        for (k = 0; k < 4; k++) {
            if (f) fprintf(f, "in r%d, %d\n", 17, ios[k]);
            else   avr_in(c, 17, ios[k]);
            if (f) fprintf(f, "out %d, r%d\n", ios[k], 17);
            else   avr_out(c, ios[k], 17);
        }
        for (k = 0; k < 4; k++) {
            int a = k * 10 > 31 ? 31 : k * 10;
            if (f) fprintf(f, "cbi %d, %d\n", a, k * 2 + 1);
            else   avr_cbi(c, a, k * 2 + 1);
            if (f) fprintf(f, "sbi %d, %d\n", a, k * 2 + 1);
            else   avr_sbi(c, a, k * 2 + 1);
        }
    }
    for (k = 0; k < 4; k++) {
        int r = dpairs[k];
        if (f) fprintf(f, "push r%d\n", r); else avr_push(c, r);
        if (f) fprintf(f, "pop r%d\n", r);  else avr_pop(c, r);
    }
    /* Both senses of every SREG bit, because the set/clear distinction is
     * ONE bit of the opcode and an inverted sense would make the
     * prologue's cli an sei -- interrupts enabled across a half-written
     * stack pointer, which is the one bug on this machine that would
     * only ever appear under load. */
    for (k = 0; k < 8; k++) {
        if (f) fprintf(f, "bset %d\n", k); else avr_bset(c, (enum avr_sreg_bit)k);
        if (f) fprintf(f, "bclr %d\n", k); else avr_bclr(c, (enum avr_sreg_bit)k);
    }
    if (f) fprintf(f, "ret\n");  else avr_ret(c);
    if (f) fprintf(f, "reti\n"); else avr_reti(c);
    if (f) fprintf(f, "nop\n");  else avr_nop(c);
    if (f) fprintf(f, "ijmp\n"); else avr_ijmp(c);
    if (f) fprintf(f, "icall\n"); else avr_icall(c);
    /* jmp/call take a byte address and halve it -- 0x1234 must come out
     * as the word address 0x091A, which is the Harvard detail most
     * likely to be encoded wrong. */
    {
        static const long addrs[3] = { 0, 0x1234, 0x3fffe };
        for (k = 0; k < 3; k++) {
            if (f) fprintf(f, "jmp %ld\n", addrs[k]);
            else   avr_jmp(c, addrs[k]);
            if (f) fprintf(f, "call %ld\n", addrs[k]);
            else   avr_call(c, addrs[k]);
        }
    }
}

void avr_vocabulary(FILE *f) { avr_walk(f, NULL); }
void avr_encode_vocabulary(struct code *c) { avr_walk(NULL, c); }

/* ---- the PC-relative forms, refereed the other way round --------------
 *
 * These were missing from the walk above, and the omission cost a real
 * miscompile: enum avr_cond numbered its flags in mnemonic order rather
 * than by SREG bit, so `breq` tested CARRY and `brlt` tested overflow.
 * Every encoding this file produced was self-consistent, which is all a
 * comparison against itself can establish. The MEANING of a condition is
 * llvm's to confirm, and it had never been asked -- `while (*s)` walked
 * past its NUL and printed 1700 bytes of RAM.
 *
 * They cannot go in the walk above, because llvm-mc leaves a
 * R_AVR_7_PCREL relocation even for a branch to a label in the same
 * section: its bytes carry a placeholder, so a byte comparison would
 * grade nothing. So the referee runs the other way -- llvm-objdump
 * DISASSEMBLES our bytes and the text is compared against what each form
 * was meant to be. That names the condition and decodes the
 * displacement, which is both halves of what can go wrong.
 *
 * llvm's AVR printer measures the displacement from the END of the
 * instruction, so a word displacement d prints as `.%+d` of 2*d bytes.
 */
static void avr_branch_walk(FILE *f, struct code *c)
{
    static const struct { const char *name; enum avr_cond c; } conds[8] = {
        /* llvm's preferred spellings: brlo IS brcs and brsh IS brcc, the
         * same opcode under the unsigned-comparison name. The referee
         * compares its text, so the text is its own. */
        { "brlo", AVR_BR_CS }, { "brsh", AVR_BR_CC },
        { "breq", AVR_BR_EQ }, { "brne", AVR_BR_NE },
        { "brmi", AVR_BR_MI }, { "brpl", AVR_BR_PL },
        { "brlt", AVR_BR_LT }, { "brge", AVR_BR_GE }
    };
    /* Zero, both ends of the seven-bit field, and values either side of
     * bit 6 -- where a field read as six bits would still have passed. */
    static const int ds[6] = { 0, 1, -1, 63, -64, -33 };
    int k, j;

    for (k = 0; k < 8; k++)
        for (j = 0; j < 6; j++) {
            if (f) fprintf(f, "%s\t.%+d\n", conds[k].name, 2 * ds[j]);
            else   avr_br(c, conds[k].c, ds[j]);
        }
    /* rjmp and rcall carry TWELVE bits, so the interesting values are the
     * ones a seven-bit reading would truncate. */
    {
        static const int rd[6] = { 0, 1, -1, 2047, -2048, -1000 };
        for (j = 0; j < 6; j++) {
            if (f) fprintf(f, "rjmp\t.%+d\n", 2 * rd[j]);
            else   avr_rjmp(c, rd[j]);
            if (f) fprintf(f, "rcall\t.%+d\n", 2 * rd[j]);
            else   avr_rcall(c, rd[j]);
        }
    }
}

void avr_branch_vocabulary(FILE *f) { avr_branch_walk(f, NULL); }
void avr_encode_branches(struct code *c) { avr_branch_walk(NULL, c); }
