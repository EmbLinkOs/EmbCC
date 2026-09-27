/* The AVR assembler: GNU AVR syntax to bytes, through the same encoders
 * the code generator uses. See asm.h for the shape and why.
 *
 * ---- the instruction set, and what is NOT here -------------------------
 *
 * The whole of AVR5 -- which is what an ATmega328P is -- plus the handful
 * of instructions only larger parts have (elpm, eijmp, eicall, spm). Those
 * last are encoded rather than refused, because this assembler has no
 * device description to know which part it is assembling for; a program
 * that uses one on a 328P gets an instruction the silicon does not
 * implement, which is the assembler's usual contract and what avr-as does
 * without -mmcu.
 *
 * The XMEGA-only atomics (xch, las, lac, lat) and des are NOT here, and
 * neither is any 16-bit-pointer variant of a part with more than 64 KB of
 * flash. Each refuses by name.
 *
 * ---- the operand shapes ------------------------------------------------
 *
 * Four, and the table below names which an instruction takes so that a
 * wrong one is a diagnostic rather than a plausible encoding:
 *
 *   a REGISTER        r0-r31, and the pointer halves XL/XH YL/YH ZL/ZH
 *   an IMMEDIATE      a constant expression, or `.±N` -- which src/as/gas.c
 *                     substitutes for a label and which is a BYTE
 *                     displacement from the START of the instruction (GNU's
 *                     convention for `.`, not llvm-objdump's end-relative
 *                     printing)
 *   a POINTER         X, Y, Z with post-increment, pre-decrement, or a
 *                     displacement off Y or Z
 *   a BIT NUMBER      which is just an immediate, range-checked
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "emit.h"
#include "../../elf/elf.h"

/* ---- errors ----------------------------------------------------------- */

struct as {
    struct code *out;
    char *err;
    int errlen;
    int failed;
    long pc;            /* bytes emitted so far in this template */
};

static void aerr(struct as *a, const char *fmt, ...)
{
    va_list ap;
    if (a->failed)
        return;              /* the first message is the useful one */
    a->failed = 1;
    va_start(ap, fmt);
    vsnprintf(a->err, (size_t)a->errlen, fmt, ap);
    va_end(ap);
}

/* ---- lexing ----------------------------------------------------------- */

static const char *skipws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int idc(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$'; }

/* A symbol NAME cannot begin with a digit -- that is a number. The
 * distinction matters only in avrasm_symform, where an operand is either a
 * constant or a symbol: without it `sts 0xC1, r24` was read as a store to a
 * symbol called "0xC1", which the linker then reported as undefined. A
 * plain `1` or `0x40` reaches the expression parser instead, which is where
 * every I/O address in a real source goes. */
static int sym_start(int c) { return isalpha(c) || c == '_' || c == '.' ||
                                     c == '$'; }

/* ---- registers -------------------------------------------------------- */

int avrasm_gpr(const char *name, int len)
{
    static const struct { const char *n; int r; } named[] = {
        { "XL", 26 }, { "XH", 27 }, { "YL", 28 }, { "YH", 29 },
        { "ZL", 30 }, { "ZH", 31 },
        /* The pointer PAIRS, by their low register. They have to be here
         * rather than only in ptr_name: src/as/gas.c asks this function
         * whether an identifier is a register in order to tell `Z` from a
         * label, and without them `lpm r0, Z+` tried to relocate against a
         * symbol called Z. parse_oper asks ptr_name FIRST, so a bare X, Y
         * or Z is still a pointer and not r26. */
        { "X", 26 }, { "Y", 28 }, { "Z", 30 },
        /* avr-libc's names for the two registers the ABI reserves. Kernel
         * headers use them, and an assembler that did not know them would
         * treat each as a label and try to relocate against it. */
        { "__tmp_reg__", 0 }, { "__zero_reg__", 1 }
    };
    int i;

    if (len <= 0)
        return -1;
    if ((name[0] == 'r' || name[0] == 'R') && len >= 2 && len <= 3) {
        int v = 0, k;
        for (k = 1; k < len; k++) {
            if (!isdigit((unsigned char)name[k]))
                return -1;
            v = v * 10 + (name[k] - '0');
        }
        /* r08 is not a register: a leading zero means this was something
         * else that happened to start with r. */
        if (len == 3 && name[1] == '0')
            return -1;
        return v <= 31 ? v : -1;
    }
    for (i = 0; i < (int)(sizeof named / sizeof named[0]); i++)
        if ((int)strlen(named[i].n) == len) {
            int k, ok = 1;
            for (k = 0; k < len; k++)
                if (toupper((unsigned char)name[k]) !=
                    toupper((unsigned char)named[i].n[k])) { ok = 0; break; }
            if (ok)
                return named[i].r;
        }
    return -1;
}

/* X, Y or Z as a pointer PAIR, by its low register. -1 otherwise. */
static int ptr_name(const char *s, int len)
{
    if (len != 1)
        return -1;
    switch (toupper((unsigned char)*s)) {
    case 'X': return AVR_X;
    case 'Y': return AVR_Y;
    case 'Z': return AVR_Z;
    default:  return -1;
    }
}

/* ---- expressions -----------------------------------------------------
 *
 * Enough of GNU as's expression syntax for real AVR sources: the four
 * arithmetic operators, the bitwise ones, shifts, unary minus and
 * complement, parentheses, character constants, and the byte-selection
 * functions lo8/hi8/hlo8/hh8 and their program-space forms.
 *
 * `.` is the address of the START of the current instruction, which is
 * GNU's convention. src/as/gas.c substitutes a label as `.±N`, so this is
 * how a branch to a label arrives and the only place the difference from
 * llvm-objdump's end-relative printing matters.
 */
struct expr_ctx {
    struct as *a;
    int reldot;          /* set when `.` appeared: the value is a
                          * displacement from this instruction, not
                          * an absolute number */
};

static long expr_or(const char **p, struct expr_ctx *x);

static long expr_prim(const char **p, struct expr_ctx *x)
{
    const char *s = skipws(*p);
    long v = 0;

    if (*s == '(') {
        s++;
        v = expr_or(&s, x);
        s = skipws(s);
        if (*s != ')')
            aerr(x->a, "an unclosed '(' in an operand");
        else
            s++;
        *p = s;
        return v;
    }
    if (*s == '-') { s++; *p = s; return -expr_prim(p, x); }
    if (*s == '+') { s++; *p = s; return  expr_prim(p, x); }
    if (*s == '~') { s++; *p = s; return ~expr_prim(p, x); }
    if (*s == '!') { s++; *p = s; return !expr_prim(p, x); }
    /* `.` -- this instruction's own address. A bare `.` is 0 here and the
     * displacement arithmetic that follows is the caller's business. */
    if (*s == '.' && !idc((unsigned char)s[1])) {
        x->reldot = 1;
        *p = s + 1;
        return 0;
    }
    if (*s == '\'') {
        /* A character constant, with the escapes an assembler sees. */
        s++;
        if (*s == '\\') {
            s++;
            switch (*s) {
            case 'n': v = '\n'; break; case 't': v = '\t'; break;
            case 'r': v = '\r'; break; case '0': v = '\0'; break;
            case 'b': v = '\b'; break; case 'f': v = '\f'; break;
            case 'a': v = '\a'; break; case 'v': v = '\v'; break;
            case '\\': v = '\\'; break; case '\'': v = '\''; break;
            default: v = (unsigned char)*s; break;
            }
            s++;
        } else {
            v = (unsigned char)*s++;
        }
        if (*s == '\'')
            s++;
        *p = s;
        return v;
    }
    /* The byte-selection functions. lo8/hi8 take a DATA address; the pm_
     * forms and lo8(gs(x)) take a program-space one, which is a WORD
     * address -- half the byte address, because that is what the machine
     * loads into Z for an icall. Getting that wrong calls twice as far
     * into flash and lands on a real instruction. */
    {
        static const struct { const char *n; int shift; int pm; } fn[] = {
            { "lo8", 0, 0 }, { "hi8", 8, 0 },
            { "hlo8", 16, 0 }, { "hh8", 16, 0 },
            { "pm_lo8", 0, 1 }, { "pm_hi8", 8, 1 }, { "pm_hh8", 16, 1 },
            { "gs", 0, 2 }
        };
        int i;
        for (i = 0; i < (int)(sizeof fn / sizeof fn[0]); i++) {
            size_t n = strlen(fn[i].n);
            if (strncmp(s, fn[i].n, n) == 0) {
                const char *q = skipws(s + n);
                if (*q != '(')
                    continue;
                q++;
                v = expr_or(&q, x);
                q = skipws(q);
                if (*q != ')')
                    aerr(x->a, "an unclosed '(' after %s", fn[i].n);
                else
                    q++;
                *p = q;
                if (fn[i].pm == 2)
                    return v >> 1;               /* gs(): the word address */
                if (fn[i].pm)
                    v >>= 1;
                return (v >> fn[i].shift) & 0xff;
            }
        }
    }
    if (isdigit((unsigned char)*s)) {
        char *end;
        /* 0x, 0b and a leading-zero octal, which strtol handles but for
         * binary -- so that one is read here. */
        if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
            const char *q = s + 2;
            v = 0;
            while (*q == '0' || *q == '1')
                v = v * 2 + (*q++ - '0');
            *p = q;
            return v;
        }
        v = strtol(s, &end, 0);
        *p = end;
        return v;
    }
    aerr(x->a, "'%.16s' is not a number, a register or an expression this "
               "assembler knows", s);
    *p = s + strlen(s);
    return 0;
}

static long expr_mul(const char **p, struct expr_ctx *x)
{
    long v = expr_prim(p, x);
    for (;;) {
        const char *s = skipws(*p);
        if (*s == '*') { s++; *p = s; v *= expr_prim(p, x); }
        else if (*s == '/') { s++; *p = s;
                              long d = expr_prim(p, x);
                              if (!d) { aerr(x->a, "a division by zero in an "
                                                   "operand"); return 0; }
                              v /= d; }
        else if (*s == '%') { s++; *p = s;
                              long d = expr_prim(p, x);
                              if (!d) { aerr(x->a, "a modulo by zero in an "
                                                   "operand"); return 0; }
                              v %= d; }
        else return v;
    }
}

static long expr_add(const char **p, struct expr_ctx *x)
{
    long v = expr_mul(p, x);
    for (;;) {
        const char *s = skipws(*p);
        if (*s == '+') { s++; *p = s; v += expr_mul(p, x); }
        else if (*s == '-') { s++; *p = s; v -= expr_mul(p, x); }
        else return v;
    }
}

static long expr_shift(const char **p, struct expr_ctx *x)
{
    long v = expr_add(p, x);
    for (;;) {
        const char *s = skipws(*p);
        if (s[0] == '<' && s[1] == '<') { s += 2; *p = s;
                                          v = (long)((unsigned long)v <<
                                                     expr_add(p, x)); }
        else if (s[0] == '>' && s[1] == '>') { s += 2; *p = s;
                                               v >>= expr_add(p, x); }
        else return v;
    }
}

static long expr_or(const char **p, struct expr_ctx *x)
{
    long v = expr_shift(p, x);
    for (;;) {
        const char *s = skipws(*p);
        /* `&&` and `||` are not here: no AVR source needs them in an
         * operand and accepting them would hide a typo for `&`. */
        if (*s == '&' && s[1] != '&') { s++; *p = s; v &= expr_shift(p, x); }
        else if (*s == '|' && s[1] != '|') { s++; *p = s; v |= expr_shift(p, x); }
        else if (*s == '^') { s++; *p = s; v ^= expr_shift(p, x); }
        else return v;
    }
}

/* ---- operands --------------------------------------------------------- */

enum opk { O_NONE, O_REG, O_IMM, O_PTR };

struct oper {
    enum opk k;
    int reg;                 /* O_REG: 0-31.  O_PTR: AVR_X/Y/Z */
    long imm;                /* O_IMM */
    int reldot;              /* O_IMM: a byte displacement from this
                              * instruction, not an absolute value */
    enum avr_ptr_mode mode;  /* O_PTR */
    long disp;               /* O_PTR: the q of Y+q / Z+q */
    int has_disp;
};

static void parse_oper(struct as *a, const char **p, struct oper *o)
{
    const char *s = skipws(*p);
    const char *id;
    int n;

    memset(o, 0, sizeof *o);
    /* -X: pre-decrement, which is the only operand that begins with a
     * minus and is not an expression. */
    if (*s == '-') {
        const char *q = skipws(s + 1);
        int pr = ptr_name(q, 1);
        if (pr >= 0 && !idc((unsigned char)q[1])) {
            o->k = O_PTR; o->reg = pr; o->mode = AVR_PTR_PRE_DEC;
            *p = q + 1;
            return;
        }
    }
    id = s;
    n = 0;
    while (idc((unsigned char)id[n]))
        n++;
    if (n) {
        int r;
        /* A pointer PAIR first: `X` names the pair, not r26, in every
         * instruction that takes one -- and avrasm_gpr answers for it too
         * (so that src/as/gas.c can tell it from a label), which would
         * otherwise make `ld r5, X` read X as a plain register. */
        if (n == 1) {
            int pr = ptr_name(id, 1);
            if (pr >= 0) {
                const char *q = id + 1;
                o->k = O_PTR; o->reg = pr; o->mode = AVR_PTR_NONE;
                if (*q == '+') {
                    q++;
                    if (idc((unsigned char)*q) || *q == '(' || *q == '-') {
                        /* Y+q: a displacement, not a post-increment. */
                        struct expr_ctx x; x.a = a; x.reldot = 0;
                        o->disp = expr_or(&q, &x);
                        o->has_disp = 1;
                    } else {
                        o->mode = AVR_PTR_POST_INC;
                    }
                }
                *p = q;
                return;
            }
        }
        r = avrasm_gpr(id, n);
        if (r >= 0 && !idc((unsigned char)id[n])) {
            o->k = O_REG; o->reg = r;
            *p = id + n;
            return;
        }
    }
    {
        struct expr_ctx x;
        x.a = a; x.reldot = 0;
        o->k = O_IMM;
        o->imm = expr_or(&s, &x);
        o->reldot = x.reldot;
        *p = s;
    }
}

/* ---- the instruction table -------------------------------------------- */

enum form {
    F_RR,        /* op rd, rr        any register */
    F_RI,        /* op rd, K         rd r16-31, K 0-255 */
    F_R1,        /* op rd            one register */
    F_ADIW,      /* op rd, K         rd 24/26/28/30, K 0-63 */
    F_MOVW,      /* movw rd, rr      even pairs */
    F_MULS,      /* op rd, rr        both r16-31 */
    F_MULSU,     /* op rd, rr        both r16-23 */
    F_LD, F_ST,  /* ld rd, X+ / st -Z, rr -- pointer either side */
    F_LDS, F_STS,
    F_LPM,       /* lpm | lpm rd, Z | lpm rd, Z+ */
    F_IN, F_OUT,
    F_BIT_IO,    /* cbi/sbi/sbic/sbis  A, b */
    F_BIT_REG,   /* bld/bst/sbrc/sbrs  rd, b */
    F_BR,        /* brXX  target */
    F_BRB,       /* brbs/brbc  bit, target */
    F_REL,       /* rjmp/rcall  target */
    F_ABS,       /* jmp/call  target */
    F_BARE,      /* ret/reti/nop/sleep/wdr/break/icall/ijmp/... */
    F_SREG,      /* sec/clc/sei/cli/... */
    F_BSET,      /* bset/bclr  n */
    F_PUSH, F_POP,
    F_TWIN,      /* clr/tst/lsl/rol -- one operand, encoded as rd, rd */
    F_SER,       /* ser rd -- ldi rd, 0xff */
    F_REFUSE     /* in the set, deliberately not implemented */
};

struct insn {
    const char *name;
    enum form form;
    unsigned aux;        /* the opcode, sub-opcode or bit number */
};

static const struct insn TAB[] = {
    /* two-register ALU */
    { "add",  F_RR, AVR_ADD }, { "adc",  F_RR, AVR_ADC },
    { "sub",  F_RR, AVR_SUB }, { "sbc",  F_RR, AVR_SBC },
    { "and",  F_RR, AVR_AND }, { "or",   F_RR, AVR_OR  },
    { "eor",  F_RR, AVR_EOR }, { "mov",  F_RR, AVR_MOV },
    { "cp",   F_RR, AVR_CP  }, { "cpc",  F_RR, AVR_CPC },
    { "cpse", F_RR, AVR_CPSE }, { "mul", F_RR, AVR_MUL },
    /* the same shape with one operand written once */
    { "clr",  F_TWIN, AVR_EOR }, { "tst", F_TWIN, AVR_AND },
    { "lsl",  F_TWIN, AVR_ADD }, { "rol", F_TWIN, AVR_ADC },
    /* signed and fractional multiply */
    { "muls",   F_MULS,  0x0200 },
    { "mulsu",  F_MULSU, 0x0300 }, { "fmul",   F_MULSU, 0x0308 },
    { "fmuls",  F_MULSU, 0x0380 }, { "fmulsu", F_MULSU, 0x0388 },
    /* immediate */
    { "subi", F_RI, AVR_SUBI }, { "sbci", F_RI, AVR_SBCI },
    { "andi", F_RI, AVR_ANDI }, { "ori",  F_RI, AVR_ORI  },
    { "cpi",  F_RI, AVR_CPI  }, { "ldi",  F_RI, AVR_LDI  },
    { "ser",  F_SER, 0 },
    /* one register */
    { "com", F_R1, AVR_COM }, { "neg",  F_R1, AVR_NEG },
    { "swap",F_R1, AVR_SWAP }, { "inc", F_R1, AVR_INC },
    { "asr", F_R1, AVR_ASR }, { "lsr",  F_R1, AVR_LSR },
    { "ror", F_R1, AVR_ROR }, { "dec",  F_R1, AVR_DEC },
    /* word */
    { "adiw", F_ADIW, 0 }, { "sbiw", F_ADIW, 1 }, { "movw", F_MOVW, 0 },
    /* memory */
    { "ld", F_LD, 0 }, { "st", F_ST, 0 },
    { "ldd", F_LD, 1 }, { "std", F_ST, 1 },
    { "lds", F_LDS, 0 }, { "sts", F_STS, 0 },
    { "lpm", F_LPM, 0 }, { "elpm", F_LPM, 1 },
    /* I/O */
    { "in", F_IN, 0 }, { "out", F_OUT, 0 },
    { "cbi", F_BIT_IO, 0x9800 }, { "sbi",  F_BIT_IO, 0x9A00 },
    { "sbic",F_BIT_IO, 0x9900 }, { "sbis", F_BIT_IO, 0x9B00 },
    /* register bits */
    { "bld", F_BIT_REG, 0xF800 }, { "bst",  F_BIT_REG, 0xFA00 },
    { "sbrc",F_BIT_REG, 0xFC00 }, { "sbrs", F_BIT_REG, 0xFE00 },
    /* stack */
    { "push", F_PUSH, 0 }, { "pop", F_POP, 0 },
    /* branches. aux is (SREG bit << 1) | sense, which is what
     * enum avr_cond already is -- so this table and the code generator
     * name a condition the same way or neither works. */
    { "brcs", F_BR, AVR_BR_CS }, { "brlo", F_BR, AVR_BR_CS },
    { "brcc", F_BR, AVR_BR_CC }, { "brsh", F_BR, AVR_BR_CC },
    { "breq", F_BR, AVR_BR_EQ }, { "brne", F_BR, AVR_BR_NE },
    { "brmi", F_BR, AVR_BR_MI }, { "brpl", F_BR, AVR_BR_PL },
    { "brlt", F_BR, AVR_BR_LT }, { "brge", F_BR, AVR_BR_GE },
    { "brvs", F_BR, 6 },         { "brvc", F_BR, 7 },
    { "brhs", F_BR, 10 },        { "brhc", F_BR, 11 },
    { "brts", F_BR, 12 },        { "brtc", F_BR, 13 },
    { "brie", F_BR, 14 },        { "brid", F_BR, 15 },
    { "brbs", F_BRB, 0 },        { "brbc", F_BRB, 1 },
    /* jumps */
    { "rjmp", F_REL, 0xC000 }, { "rcall", F_REL, 0xD000 },
    { "jmp",  F_ABS, 0 },      { "call",  F_ABS, 1 },
    /* SREG, by name */
    { "sec", F_SREG, (AVR_SREG_C << 1) | 0 },
    { "clc", F_SREG, (AVR_SREG_C << 1) | 1 },
    { "sez", F_SREG, (AVR_SREG_Z << 1) | 0 },
    { "clz", F_SREG, (AVR_SREG_Z << 1) | 1 },
    { "sen", F_SREG, (AVR_SREG_N << 1) | 0 },
    { "cln", F_SREG, (AVR_SREG_N << 1) | 1 },
    { "sev", F_SREG, (AVR_SREG_V << 1) | 0 },
    { "clv", F_SREG, (AVR_SREG_V << 1) | 1 },
    { "ses", F_SREG, (AVR_SREG_S << 1) | 0 },
    { "cls", F_SREG, (AVR_SREG_S << 1) | 1 },
    { "seh", F_SREG, (AVR_SREG_H << 1) | 0 },
    { "clh", F_SREG, (AVR_SREG_H << 1) | 1 },
    { "set", F_SREG, (AVR_SREG_T << 1) | 0 },
    { "clt", F_SREG, (AVR_SREG_T << 1) | 1 },
    { "sei", F_SREG, (AVR_SREG_I << 1) | 0 },
    { "cli", F_SREG, (AVR_SREG_I << 1) | 1 },
    { "bset", F_BSET, 0 }, { "bclr", F_BSET, 1 },
    /* no operands */
    { "ret",   F_BARE, 0x9508 }, { "reti",  F_BARE, 0x9518 },
    { "nop",   F_BARE, 0x0000 }, { "sleep", F_BARE, 0x9588 },
    { "wdr",   F_BARE, 0x95A8 }, { "break", F_BARE, 0x9598 },
    { "ijmp",  F_BARE, 0x9409 }, { "icall", F_BARE, 0x9509 },
    { "eijmp", F_BARE, 0x9419 }, { "eicall",F_BARE, 0x9519 },
    { "spm",   F_BARE, 0x95E8 },
    /* In the instruction set and deliberately absent: the XMEGA atomics
     * and the DES accelerator. Named here so the diagnostic says "not
     * implemented" rather than "unknown", which are different problems. */
    { "xch", F_REFUSE, 0 }, { "las", F_REFUSE, 0 },
    { "lac", F_REFUSE, 0 }, { "lat", F_REFUSE, 0 },
    { "des", F_REFUSE, 0 }
};

static const struct insn *find_insn(const char *s, int n)
{
    int i;
    for (i = 0; i < (int)(sizeof TAB / sizeof TAB[0]); i++) {
        if ((int)strlen(TAB[i].name) != n)
            continue;
        {
            int k, ok = 1;
            for (k = 0; k < n; k++)
                if (tolower((unsigned char)s[k]) != TAB[i].name[k]) {
                    ok = 0; break;
                }
            if (ok)
                return &TAB[i];
        }
    }
    return NULL;
}

/* ---- range checks ---------------------------------------------------- */

static int want_reg(struct as *a, const struct oper *o, const char *what)
{
    if (o->k != O_REG) {
        aerr(a, "%s takes a register here", what);
        return 0;
    }
    return 1;
}

static int want_imm(struct as *a, const struct oper *o, const char *what,
                    long lo, long hi, long *v)
{
    if (o->k != O_IMM) {
        aerr(a, "%s takes a constant here", what);
        return 0;
    }
    if (o->imm < lo || o->imm > hi) {
        aerr(a, "%s: %ld is outside %ld..%ld", what, o->imm, lo, hi);
        return 0;
    }
    *v = o->imm;
    return 1;
}

/* A branch or jump displacement, in WORDS, from a `.±N` byte value.
 *
 * `.` is the START of this instruction and the machine measures from the
 * one AFTER it, so two bytes come off before halving. An odd displacement
 * cannot be encoded at all -- instructions are halfword aligned -- and is
 * a diagnostic rather than a silent truncation. */
static int want_disp(struct as *a, const struct oper *o, const char *what,
                     long lo, long hi, int *v)
{
    long d;
    if (o->k != O_IMM) {
        aerr(a, "%s takes a label or a displacement here", what);
        return 0;
    }
    /* It has to be `.`-relative. GNU as reads a bare number as an ADDRESS
     * and computes the displacement from the current location -- which this
     * encoder does not know, because it sees one statement at a time. A
     * label arrives as `.±N` (src/as/gas.c substitutes it) and an inline
     * asm template writes `.±N` too, so requiring it costs nothing and
     * stops a bare `brne 4` from silently meaning "four bytes on" when the
     * source meant address four. */
    if (!o->reldot) {
        aerr(a, "%s needs a target written relative to `.` (a label, or "
                "`.%+ld`); a bare number here would be an ADDRESS, and one "
                "statement's encoder does not know where it is", what,
             o->imm);
        return 0;
    }
    d = o->imm - 2;
    if (d & 1) {
        aerr(a, "%s: a displacement of %ld bytes is odd, and every AVR "
                "instruction begins on a halfword", what, o->imm);
        return 0;
    }
    d /= 2;
    if (d < lo || d > hi) {
        aerr(a, "%s: %ld words is out of reach (%ld..%ld); it needs a longer "
                "jump", what, d, lo, hi);
        return 0;
    }
    *v = (int)d;
    return 1;
}

/* ---- one statement ---------------------------------------------------- */

static void one(struct as *a, const char *stmt)
{
    const char *p = skipws(stmt);
    const char *mn = p;
    int mnlen = 0;
    const struct insn *in;
    struct oper o[3];
    int nop = 0;
    long v;

    while (idc((unsigned char)p[mnlen]) && p[mnlen] != '.')
        mnlen++;
    /* A mnemonic may end in a dot on some machines; on AVR none does, so a
     * dot here is part of something else. */
    if (!mnlen) {
        aerr(a, "\"%.32s\" does not begin with an instruction", stmt);
        return;
    }
    in = find_insn(mn, mnlen);
    if (!in) {
        aerr(a, "'%.*s' is not an AVR instruction this assembler knows",
             mnlen, mn);
        return;
    }
    p = skipws(mn + mnlen);
    memset(o, 0, sizeof o);
    while (*p && nop < 3) {
        parse_oper(a, &p, &o[nop++]);
        if (a->failed)
            return;
        p = skipws(p);
        if (*p == ',') { p = skipws(p + 1); continue; }
        break;
    }
    if (*p) {
        aerr(a, "'%s' is left over after the operands of %.*s", p, mnlen, mn);
        return;
    }

    switch (in->form) {
    case F_RR:
        if (nop != 2) { aerr(a, "%.*s takes two registers", mnlen, mn); return; }
        if (!want_reg(a, &o[0], in->name) || !want_reg(a, &o[1], in->name))
            return;
        avr_rr(a->out, (enum avr_rr)in->aux, o[0].reg, o[1].reg);
        return;

    case F_TWIN:
        if (nop != 1) { aerr(a, "%.*s takes one register", mnlen, mn); return; }
        if (!want_reg(a, &o[0], in->name)) return;
        avr_rr(a->out, (enum avr_rr)in->aux, o[0].reg, o[0].reg);
        return;

    case F_MULS: case F_MULSU: {
        int lo = in->form == F_MULS ? 16 : 16;
        int hi = in->form == F_MULS ? 31 : 23;
        int dm, rm;
        if (nop != 2) { aerr(a, "%.*s takes two registers", mnlen, mn); return; }
        if (!want_reg(a, &o[0], in->name) || !want_reg(a, &o[1], in->name))
            return;
        if (o[0].reg < lo || o[0].reg > hi || o[1].reg < lo || o[1].reg > hi) {
            aerr(a, "%s reaches only r%d-r%d", in->name, lo, hi);
            return;
        }
        dm = (o[0].reg - 16) & (in->form == F_MULS ? 0xf : 0x7);
        rm = (o[1].reg - 16) & (in->form == F_MULS ? 0xf : 0x7);
        code_u16(a->out, in->aux | (unsigned)(dm << 4) | (unsigned)rm);
        return;
    }

    case F_RI:
        if (nop != 2) { aerr(a, "%.*s takes a register and a constant",
                             mnlen, mn); return; }
        if (!want_reg(a, &o[0], in->name)) return;
        if (o[0].reg < 16) {
            aerr(a, "%s reaches only r16-r31, and r%d is below it -- this is "
                    "the restriction that makes the register allocator's pool "
                    "matter on this machine", in->name, o[0].reg);
            return;
        }
        /* -1..-128 is how a source writes a byte's top half. */
        if (o[1].k == O_IMM && o[1].imm < 0 && o[1].imm >= -128)
            o[1].imm &= 0xff;
        if (!want_imm(a, &o[1], in->name, 0, 255, &v)) return;
        avr_ri(a->out, (enum avr_ri)in->aux, o[0].reg, (int)v);
        return;

    case F_SER:
        if (nop != 1) { aerr(a, "ser takes one register"); return; }
        if (!want_reg(a, &o[0], "ser")) return;
        if (o[0].reg < 16) { aerr(a, "ser reaches only r16-r31"); return; }
        avr_ri(a->out, AVR_LDI, o[0].reg, 0xff);
        return;

    case F_R1:
        if (nop != 1) { aerr(a, "%.*s takes one register", mnlen, mn); return; }
        if (!want_reg(a, &o[0], in->name)) return;
        avr_r1(a->out, (enum avr_r1)in->aux, o[0].reg);
        return;

    case F_ADIW:
        if (nop != 2) { aerr(a, "%.*s takes a register pair and a constant",
                             mnlen, mn); return; }
        if (!want_reg(a, &o[0], in->name)) return;
        if (o[0].reg != 24 && o[0].reg != 26 && o[0].reg != 28 &&
            o[0].reg != 30) {
            aerr(a, "%s reaches only r24, r26, r28 and r30", in->name);
            return;
        }
        if (!want_imm(a, &o[1], in->name, 0, 63, &v)) return;
        if (in->aux) avr_sbiw(a->out, o[0].reg, (int)v);
        else         avr_adiw(a->out, o[0].reg, (int)v);
        return;

    case F_MOVW:
        if (nop != 2) { aerr(a, "movw takes two register pairs"); return; }
        if (!want_reg(a, &o[0], "movw") || !want_reg(a, &o[1], "movw")) return;
        if ((o[0].reg | o[1].reg) & 1) {
            aerr(a, "movw moves a PAIR and both registers must be even");
            return;
        }
        avr_movw(a->out, o[0].reg, o[1].reg);
        return;

    case F_LD: case F_ST: {
        const struct oper *r = in->form == F_LD ? &o[0] : &o[1];
        const struct oper *m = in->form == F_LD ? &o[1] : &o[0];
        if (nop != 2) { aerr(a, "%.*s takes a register and a pointer",
                             mnlen, mn); return; }
        if (!want_reg(a, r, in->name)) return;
        if (m->k != O_PTR) {
            aerr(a, "%s addresses memory through X, Y or Z; '%s' is not one "
                    "of them", in->name,
                 m->k == O_REG ? "that register" : "that operand");
            return;
        }
        if (m->has_disp) {
            if (m->reg == AVR_X) {
                aerr(a, "X takes no displacement -- only Y and Z do, which is "
                        "why a frame pointer on this machine is Y");
                return;
            }
            if (m->disp < 0 || m->disp > 63) {
                aerr(a, "a displacement of %ld is outside 0..63", m->disp);
                return;
            }
            if (in->form == F_LD) avr_ldd(a->out, r->reg, m->reg, (int)m->disp);
            else                  avr_std(a->out, m->reg, (int)m->disp, r->reg);
            return;
        }
        if (in->form == F_LD) avr_ld(a->out, r->reg, m->reg, m->mode);
        else                  avr_st(a->out, m->reg, r->reg, m->mode);
        return;
    }

    case F_LDS:
        if (nop != 2) { aerr(a, "lds takes a register and an address"); return; }
        if (!want_reg(a, &o[0], "lds")) return;
        if (!want_imm(a, &o[1], "lds", 0, 0xffff, &v)) return;
        avr_lds(a->out, o[0].reg, (int)v);
        return;

    case F_STS:
        if (nop != 2) { aerr(a, "sts takes an address and a register"); return; }
        if (!want_imm(a, &o[0], "sts", 0, 0xffff, &v)) return;
        if (!want_reg(a, &o[1], "sts")) return;
        avr_sts(a->out, (int)v, o[1].reg);
        return;

    case F_LPM:
        /* Three forms: implicit (r0 from Z), and two explicit ones. elpm
         * is the same shape one bit along. */
        if (nop == 0) {
            code_u16(a->out, in->aux ? 0x95D8u : 0x95C8u);
            return;
        }
        if (nop != 2) {
            aerr(a, "%s takes no operands, or a register and Z", in->name);
            return;
        }
        if (!want_reg(a, &o[0], in->name)) return;
        if (o[1].k != O_PTR || o[1].reg != AVR_Z || o[1].has_disp) {
            aerr(a, "%s reads program space through Z only", in->name);
            return;
        }
        if (o[1].mode == AVR_PTR_PRE_DEC) {
            aerr(a, "%s has no pre-decrement form", in->name);
            return;
        }
        if (in->aux)
            code_u16(a->out, 0x9006u | (unsigned)(o[0].reg << 4) |
                             (o[1].mode == AVR_PTR_POST_INC ? 1u : 0u));
        else
            avr_lpm(a->out, o[0].reg, o[1].mode == AVR_PTR_POST_INC);
        return;

    case F_IN:
        if (nop != 2) { aerr(a, "in takes a register and an I/O address");
                        return; }
        if (!want_reg(a, &o[0], "in")) return;
        if (!want_imm(a, &o[1], "in", 0, 63, &v)) return;
        avr_in(a->out, o[0].reg, (int)v);
        return;

    case F_OUT:
        if (nop != 2) { aerr(a, "out takes an I/O address and a register");
                        return; }
        if (!want_imm(a, &o[0], "out", 0, 63, &v)) return;
        if (!want_reg(a, &o[1], "out")) return;
        avr_out(a->out, (int)v, o[1].reg);
        return;

    case F_BIT_IO: {
        long b;
        if (nop != 2) { aerr(a, "%.*s takes an I/O address and a bit",
                             mnlen, mn); return; }
        /* The single-bit I/O instructions reach only the low 32 ports,
         * where `in` and `out` reach 64. That is the restriction that
         * makes a register at 0x20-0x3f reachable one way and not the
         * other. */
        if (!want_imm(a, &o[0], in->name, 0, 31, &v)) return;
        if (!want_imm(a, &o[1], in->name, 0, 7, &b)) return;
        if (in->aux == 0x9800u) avr_cbi(a->out, (int)v, (int)b);
        else if (in->aux == 0x9A00u) avr_sbi(a->out, (int)v, (int)b);
        else code_u16(a->out, in->aux | (unsigned)(v << 3) | (unsigned)b);
        return;
    }

    case F_BIT_REG: {
        long b;
        if (nop != 2) { aerr(a, "%.*s takes a register and a bit",
                             mnlen, mn); return; }
        if (!want_reg(a, &o[0], in->name)) return;
        if (!want_imm(a, &o[1], in->name, 0, 7, &b)) return;
        code_u16(a->out, in->aux | (unsigned)(o[0].reg << 4) | (unsigned)b);
        return;
    }

    case F_PUSH: case F_POP:
        if (nop != 1) { aerr(a, "%.*s takes one register", mnlen, mn); return; }
        if (!want_reg(a, &o[0], in->name)) return;
        if (in->form == F_PUSH) avr_push(a->out, o[0].reg);
        else                    avr_pop(a->out, o[0].reg);
        return;

    case F_BR: {
        int d;
        if (nop != 1) { aerr(a, "%.*s takes one target", mnlen, mn); return; }
        if (!want_disp(a, &o[0], in->name, -64, 63, &d)) return;
        avr_br(a->out, (enum avr_cond)in->aux, d);
        return;
    }

    case F_BRB: {
        int d;
        long b;
        if (nop != 2) { aerr(a, "%.*s takes a bit and a target", mnlen, mn);
                        return; }
        if (!want_imm(a, &o[0], in->name, 0, 7, &b)) return;
        if (!want_disp(a, &o[1], in->name, -64, 63, &d)) return;
        avr_br(a->out, (enum avr_cond)((b << 1) | in->aux), d);
        return;
    }

    case F_REL: {
        int d;
        if (nop != 1) { aerr(a, "%.*s takes one target", mnlen, mn); return; }
        if (!want_disp(a, &o[0], in->name, -2048, 2047, &d)) return;
        if (in->aux == 0xC000u) avr_rjmp(a->out, d);
        else                    avr_rcall(a->out, d);
        return;
    }

    case F_ABS:
        /* jmp and call carry an ABSOLUTE word address. A `.±N` here came
         * from a label in this file, and a relocatable object does not know
         * where its own section will land -- so the SYMBOL form is the one
         * that works, and src/as/gas.c routes it through avrasm_symform
         * before this ever sees it. A bare displacement is refused rather
         * than turned into an address that happens to assemble. */
        if (nop != 1) { aerr(a, "%.*s takes one target", mnlen, mn); return; }
        if (o[0].k != O_IMM) { aerr(a, "%.*s takes an address", mnlen, mn);
                               return; }
        if (o[0].reldot) {
            aerr(a, "%.*s carries an absolute address, so a PC-relative "
                    "target cannot be encoded here; use rjmp/rcall, which "
                    "reach +-4KB, or name the label so it becomes a "
                    "relocation", mnlen, mn);
            return;
        }
        if (o[0].imm & 1) {
            aerr(a, "%.*s to an odd address: instructions are halfword "
                    "aligned and the address is halved to a word number",
                 mnlen, mn);
            return;
        }
        if (in->aux) avr_call(a->out, o[0].imm);
        else         avr_jmp(a->out, o[0].imm);
        return;

    case F_BARE:
        if (nop != 0) { aerr(a, "%.*s takes no operands", mnlen, mn); return; }
        code_u16(a->out, in->aux);
        return;

    case F_SREG:
        if (nop != 0) { aerr(a, "%.*s takes no operands", mnlen, mn); return; }
        if (in->aux & 1) avr_bclr(a->out, (enum avr_sreg_bit)(in->aux >> 1));
        else             avr_bset(a->out, (enum avr_sreg_bit)(in->aux >> 1));
        return;

    case F_BSET:
        if (nop != 1) { aerr(a, "%.*s takes a bit number", mnlen, mn); return; }
        if (!want_imm(a, &o[0], in->name, 0, 7, &v)) return;
        if (in->aux) avr_bclr(a->out, (enum avr_sreg_bit)v);
        else         avr_bset(a->out, (enum avr_sreg_bit)v);
        return;

    case F_REFUSE:
        aerr(a, "%s is an AVR instruction this assembler does not implement "
                "yet (it is XMEGA-only or a cryptographic accelerator); it is "
                "named here so this says \"not implemented\" rather than "
                "\"unknown\"", in->name);
        return;
    }
}

/* ---- the template ----------------------------------------------------- */

int avrasm_assemble(const char *text, struct code *out, char *err, int errlen)
{
    struct as a;
    const char *p = text;
    char buf[512];

    a.out = out; a.err = err; a.errlen = errlen; a.failed = 0; a.pc = 0;
    if (errlen > 0)
        err[0] = '\0';

    while (*p) {
        int n = 0;
        /* One statement: up to ';' or a newline. A ';' at the start of a
         * line is a comment in AVR sources as well as a separator, but by
         * the time a statement is split there is nothing before it, so an
         * empty one is simply skipped. */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;
        /* `;` starts a COMMENT on AVR -- GNU as sets it per port and this
         * port's is the semicolon, not the statement separator it is on
         * some other machines. An inline asm template for this target
         * therefore separates with newlines, which is what GCC's own AVR
         * documentation says to write. */
        if (*p == '#' || *p == ';' || (p[0] == '/' && p[1] == '/')) {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        while (*p && *p != '\n' && *p != '\r') {
            if (*p == ';') break;
            if (p[0] == '/' && p[1] == '/')
                break;
            if (n < (int)sizeof buf - 1)
                buf[n++] = *p;
            p++;
        }
        while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t'))
            n--;
        buf[n] = '\0';
        if (n)
            one(&a, buf);
        if (a.failed)
            return -1;
        if (*p == ';' || (p[0] == '/' && p[1] == '/'))
            while (*p && *p != '\n')
                p++;
    }
    return 0;
}

/* ---- the forms that name a symbol ------------------------------------ */

/* Every one of these is an instruction whose operand is an ADDRESS, which
 * a relocatable object cannot know. The statement is rewritten with a zero
 * in that operand's place and the site records which relocation fills it.
 *
 * AVR has more of them than any other target here for two reasons a
 * sixteen-bit machine with two address spaces cannot avoid: an address is
 * loaded a byte at a time, so `ldi rd, lo8(sym)` and `ldi rd, hi8(sym)`
 * are separate instructions with separate relocations; and program space is
 * addressed in WORDS, so a function's address needs the gs()/pm_ forms,
 * whose relocation halves it. Using a data form for a function produces a
 * pointer to twice as far into flash, which lands on a real instruction.
 */
int avrasm_symform(const char *stmt, struct asm_symform *f)
{
    const char *p = skipws(stmt);
    const char *mn = p;
    int mnlen = 0;
    int i;

    memset(f, 0, sizeof *f);
    while (idc((unsigned char)mn[mnlen]))
        mnlen++;
    if (!mnlen)
        return 0;
    p = skipws(mn + mnlen);

    /* jmp / call sym -- one relocation on the first halfword, covering
     * both: R_AVR_CALL patches the 22-bit word address across the pair. */
    if ((mnlen == 3 && strncmp(mn, "jmp", 3) == 0) ||
        (mnlen == 4 && strncmp(mn, "call", 4) == 0)) {
        int n = 0;
        if (!sym_start((unsigned char)p[0])) return 0;
        while (idc((unsigned char)p[n]))
            n++;
        if (!n) return 0;
        f->sym_at = (int)(p - stmt); f->sym_len = n;
        snprintf(f->encode, sizeof f->encode, "%.*s 0", mnlen, mn);
        f->site[0].off = 0; f->site[0].reloc = R_AVR_CALL;
        f->nsites = 1;
        return 1;
    }
    /* rjmp / rcall sym -- PC-relative, 12 bits of words. */
    if ((mnlen == 4 && strncmp(mn, "rjmp", 4) == 0) ||
        (mnlen == 5 && strncmp(mn, "rcall", 5) == 0)) {
        int n = 0;
        if (!sym_start((unsigned char)p[0])) return 0;
        while (idc((unsigned char)p[n]))
            n++;
        if (!n) return 0;
        f->sym_at = (int)(p - stmt); f->sym_len = n;
        snprintf(f->encode, sizeof f->encode, "%.*s .+2", mnlen, mn);
        f->site[0].off = 0; f->site[0].reloc = R_AVR_13_PCREL;
        f->nsites = 1;
        return 1;
    }
    /* A conditional branch to a symbol -- 7 bits of words. */
    if (mnlen == 4 && mn[0] == 'b' && mn[1] == 'r' &&
        strncmp(mn, "brbs", 4) != 0 && strncmp(mn, "brbc", 4) != 0) {
        int n = 0;
        while (idc((unsigned char)p[n]))
            n++;
        /* A bit number is not a symbol, which is why brbs/brbc are excluded
         * above: their FIRST operand is the flag. */
        if (n && sym_start((unsigned char)p[0]) && find_insn(mn, mnlen)) {
            f->sym_at = (int)(p - stmt); f->sym_len = n;
            snprintf(f->encode, sizeof f->encode, "%.*s .+2", mnlen, mn);
            f->site[0].off = 0; f->site[0].reloc = R_AVR_7_PCREL;
            f->nsites = 1;
            return 1;
        }
    }
    /* lds rd, sym  and  sts sym, rr -- the address is the SECOND halfword
     * of a 32-bit instruction, so the site is at offset 2. */
    if (mnlen == 3 && (strncmp(mn, "lds", 3) == 0 || strncmp(mn, "sts", 3) == 0)) {
        int is_lds = mn[0] == 'l';
        const char *sp = p;
        char reg[16];
        int rn = 0, n;
        if (is_lds) {
            while (idc((unsigned char)sp[rn]) && rn < (int)sizeof reg - 1)
                reg[rn] = sp[rn], rn++;
            reg[rn] = '\0';
            if (avrasm_gpr(reg, rn) < 0) return 0;
            sp = skipws(sp + rn);
            if (*sp != ',') return 0;
            sp = skipws(sp + 1);
        }
        n = 0;
        if (!sym_start((unsigned char)sp[0])) return 0;
        while (idc((unsigned char)sp[n]))
            n++;
        if (!n) return 0;
        if (is_lds) {
            f->sym_at = (int)(sp - stmt); f->sym_len = n;
            snprintf(f->encode, sizeof f->encode, "lds %s, 0", reg);
        } else {
            const char *rest = skipws(sp + n);
            if (*rest != ',') return 0;
            rest = skipws(rest + 1);
            f->sym_at = (int)(sp - stmt); f->sym_len = n;
            snprintf(f->encode, sizeof f->encode, "sts 0, %s", rest);
        }
        f->site[0].off = 2; f->site[0].reloc = R_AVR_16;
        f->nsites = 1;
        return 1;
    }
    /* ldi rd, lo8(sym) and its family. */
    if (mnlen == 3 && strncmp(mn, "ldi", 3) == 0) {
        static const struct { const char *fn; int data; int pm; } wrap[] = {
            { "pm_lo8", 0, 1 }, { "pm_hi8", 0, 2 },
            { "lo8", 1, 0 },    { "hi8", 2, 0 },
            { "hlo8", 3, 0 },   { "hh8", 3, 0 }
        };
        char reg[16];
        int rn = 0;
        const char *sp = p;
        while (idc((unsigned char)sp[rn]) && rn < (int)sizeof reg - 1)
            reg[rn] = sp[rn], rn++;
        reg[rn] = '\0';
        if (avrasm_gpr(reg, rn) < 0)
            return 0;
        sp = skipws(sp + rn);
        if (*sp != ',')
            return 0;
        sp = skipws(sp + 1);
        for (i = 0; i < (int)(sizeof wrap / sizeof wrap[0]); i++) {
            size_t wl = strlen(wrap[i].fn);
            const char *r;
            int n, gs = 0, reloc;
            if (strncmp(sp, wrap[i].fn, wl) != 0)
                continue;
            r = skipws(sp + wl);
            if (*r != '(')
                continue;
            r = skipws(r + 1);
            /* lo8(gs(sym)): the assembler spelling for a function's word
             * address, which is what avr-gcc emits and what `icall` needs. */
            if (strncmp(r, "gs", 2) == 0) {
                const char *g2 = skipws(r + 2);
                if (*g2 == '(') { gs = 1; r = skipws(g2 + 1); }
            }
            n = 0;
            if (!sym_start((unsigned char)r[0]))
                return 0;          /* lo8(64): a constant, not a symbol */
            while (idc((unsigned char)r[n]))
                n++;
            if (!n)
                return 0;
            if (gs || wrap[i].pm)
                reloc = (wrap[i].data == 2 || wrap[i].pm == 2)
                        ? R_AVR_HI8_LDI_GS : R_AVR_LO8_LDI_GS;
            else if (wrap[i].data == 2)
                reloc = R_AVR_HI8_LDI;
            else if (wrap[i].data == 1)
                reloc = R_AVR_LO8_LDI;
            else
                return 0;               /* hlo8/hh8: a 3-byte address space */
            f->sym_at = (int)(r - stmt); f->sym_len = n;
            snprintf(f->encode, sizeof f->encode, "ldi %s, 0", reg);
            f->site[0].off = 0; f->site[0].reloc = reloc;
            f->nsites = 1;
            return 1;
        }
        return 0;
    }
    return 0;
}

/* ---- the vocabulary, for the referee ---------------------------------
 *
 * One line per mnemonic in the table, with operands that exercise the
 * restriction each form has. Generated FROM the table, so a mnemonic added
 * above cannot escape the referee -- which is the discipline the branch
 * condition bug bought: the encoder's own vocabulary had never been shown
 * a single conditional branch, and three of the four condition pairs named
 * the wrong SREG flag.
 */
void avrasm_vocabulary(FILE *f)
{
    int i;

    for (i = 0; i < (int)(sizeof TAB / sizeof TAB[0]); i++) {
        const struct insn *in = &TAB[i];
        switch (in->form) {
        case F_RR:      fprintf(f, "%s r3, r20\n", in->name); break;
        case F_TWIN:    fprintf(f, "%s r20\n", in->name); break;
        case F_MULS:    fprintf(f, "%s r17, r18\n", in->name); break;
        case F_MULSU:   fprintf(f, "%s r17, r18\n", in->name); break;
        case F_RI:      fprintf(f, "%s r16, 0xa5\n%s r31, 0\n",
                                in->name, in->name); break;
        case F_SER:     fprintf(f, "%s r20\n", in->name); break;
        case F_R1:      fprintf(f, "%s r0\n%s r31\n", in->name, in->name); break;
        case F_ADIW:    fprintf(f, "%s r24, 3\n%s r30, 63\n",
                                in->name, in->name); break;
        case F_MOVW:    fprintf(f, "%s r30, r2\n", in->name); break;
        case F_LD:
            if (in->aux)
                fprintf(f, "%s r5, Y+0\n%s r5, Y+63\n%s r5, Z+17\n",
                        in->name, in->name, in->name);
            else
                fprintf(f, "%s r5, X\n%s r5, X+\n%s r5, -X\n"
                           "%s r5, Y\n%s r5, Y+\n%s r5, -Y\n"
                           "%s r5, Z\n%s r5, Z+\n%s r5, -Z\n",
                        in->name, in->name, in->name, in->name, in->name,
                        in->name, in->name, in->name, in->name);
            break;
        case F_ST:
            if (in->aux)
                fprintf(f, "%s Y+0, r5\n%s Y+63, r5\n%s Z+17, r5\n",
                        in->name, in->name, in->name);
            else
                fprintf(f, "%s X, r5\n%s X+, r5\n%s -X, r5\n"
                           "%s Y, r5\n%s Y+, r5\n%s -Y, r5\n"
                           "%s Z, r5\n%s Z+, r5\n%s -Z, r5\n",
                        in->name, in->name, in->name, in->name, in->name,
                        in->name, in->name, in->name, in->name);
            break;
        case F_LDS:     fprintf(f, "%s r5, 0x1234\n", in->name); break;
        case F_STS:     fprintf(f, "%s 0x1234, r5\n", in->name); break;
        case F_LPM:     fprintf(f, "%s\n%s r5, Z\n%s r5, Z+\n",
                                in->name, in->name, in->name); break;
        case F_IN:      fprintf(f, "%s r5, 0x3f\n%s r31, 0\n",
                                in->name, in->name); break;
        case F_OUT:     fprintf(f, "%s 0x3f, r5\n%s 0, r31\n",
                                in->name, in->name); break;
        case F_BIT_IO:  fprintf(f, "%s 5, 3\n%s 31, 7\n%s 0, 0\n",
                                in->name, in->name, in->name); break;
        case F_BIT_REG: fprintf(f, "%s r20, 5\n%s r0, 0\n%s r31, 7\n",
                                in->name, in->name, in->name); break;
        case F_PUSH: case F_POP:
                        fprintf(f, "%s r0\n%s r31\n", in->name, in->name); break;
        /* `.` is the START of the instruction here, and the machine
         * measures from the one after -- so `.+2` is a displacement of
         * zero words and `.+128` is the top of the seven-bit field. */
        /* The PC-relative forms are NOT here: llvm-mc leaves a relocation
         * on a branch even to a label in its own section, so its bytes are
         * a placeholder and comparing them would grade nothing. They are
         * refereed by avrasm_pcrel_vocabulary below instead. */
        case F_BR: case F_BRB: case F_REL: break;
        case F_ABS:     fprintf(f, "%s 0x1234\n%s 0\n", in->name, in->name);
                        break;
        case F_BARE: case F_SREG:
                        fprintf(f, "%s\n", in->name); break;
        case F_BSET:    fprintf(f, "%s 0\n%s 7\n", in->name, in->name); break;
        case F_REFUSE:  break;        /* nothing to encode */
        }
    }
}

/* The PC-relative forms, refereed the other way round: llvm-mc relocates a
 * branch even to a label in its own section, so these are assembled here
 * and DISASSEMBLED, and the text is compared against what each was meant to
 * be. That is the same arrangement src/arch/avr/emit.c uses for the same
 * reason, and for the same reason it exists -- a condition that encodes
 * cleanly and means the wrong thing is invisible to any other check.
 *
 * `f` prints the expected disassembly, `out` assembles the source. llvm's
 * AVR printer measures from the END of the instruction, where `.` in a
 * source measures from its START, so a source `.+2` prints as `.+0`.
 */
static void pcrel_walk(FILE *f, struct code *out, char *err, int errlen)
{
    static const int ds[5] = { 2, 4, 0, 128, -126 };   /* source bytes */
    int i, j;

    for (i = 0; i < (int)(sizeof TAB / sizeof TAB[0]); i++) {
        const struct insn *in = &TAB[i];
        int lo, hi;
        if (in->form == F_BR)       { lo = -64;   hi = 63; }
        else if (in->form == F_REL) { lo = -2048; hi = 2047; }
        else if (in->form == F_BRB) { lo = -64;   hi = 63; }
        else continue;
        for (j = 0; j < 5; j++) {
            long d = ds[j];
            long words = (d - 2) / 2;
            if ((d - 2) & 1) continue;
            if (words < lo || words > hi) continue;
            if (f) {
                /* What llvm PRINTS, which is not always what a source
                 * writes: brcs and brlo are one instruction under two
                 * names, and the disassembler picks the unsigned-comparison
                 * spelling. The vocabulary offers both spellings on purpose
                 * -- an assembler has to take either -- so the expected
                 * text maps them onto llvm's choice. */
                const char *shown = in->name;
                if (strcmp(shown, "brcs") == 0) shown = "brlo";
                else if (strcmp(shown, "brcc") == 0) shown = "brsh";
                if (in->form == F_BRB)
                    shown = in->aux ? "brsh" : "brlo";   /* bit 0, both senses */
                fprintf(f, "%s\t.%+ld\n", shown, 2 * words);
            } else {
                char buf[64];
                if (in->form == F_BRB)
                    snprintf(buf, sizeof buf, "%s 0, .%+ld", in->name, d);
                else
                    snprintf(buf, sizeof buf, "%s .%+ld", in->name, d);
                if (avrasm_assemble(buf, out, err, errlen) != 0)
                    return;
            }
        }
    }
}

void avrasm_pcrel_vocabulary(FILE *f) { pcrel_walk(f, NULL, NULL, 0); }

int avrasm_pcrel_encode(struct code *out, char *err, int errlen)
{
    if (errlen > 0) err[0] = 0;
    pcrel_walk(NULL, out, err, errlen);
    return err && err[0] ? -1 : 0;
}
