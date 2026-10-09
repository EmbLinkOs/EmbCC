/* ---- constant folding ---- */

#include "opt_int.h"

/* Normalize a computed result to the operation width: an int-class result
 * is the sign-extended low 32 bits, exactly what a 32-bit op leaves. */
long norm(long r, int w)
{
    return w == 8 ? r : (long)(int)r;
}

void to_const(struct ir_ins *i, long val)
{
    i->op = IR_CONST;
    i->imm = val;
    i->a = -1;
    i->b = -1;
}

void to_mov(struct ir_ins *i, int src)
{
    i->op = IR_MOV;
    i->a = src;
    i->b = -1;
}

/* Fold a binary integer op on two constants, honoring width and signedness. */
int fold_bin(enum ir_op op, long A, long B, int w, int sign,
             enum binop pred, long *out)
{
    unsigned long ua = w == 8 ? (unsigned long)A : (unsigned int)A;
    unsigned long ub = w == 8 ? (unsigned long)B : (unsigned int)B;
    long sa = w == 8 ? A : (int)A;
    long sb = w == 8 ? B : (int)B;
    int sh = (int)(ub & (w == 8 ? 63 : 31));
    unsigned long r;
    switch (op) {
    case IR_ADD: r = ua + ub; break;
    case IR_SUB: r = ua - ub; break;
    case IR_MUL: r = ua * ub; break;
    /* The operands are 32-bit values whatever the width: the low word
     * of each, extended by `sign`. The product of two of them fits in
     * 64 bits either way, so it is exact here. */
    case IR_MULH: case IR_MULW: {
        unsigned long p = sign
            ? (unsigned long)((long)(int)A * (long)(int)B)
            : (unsigned long)(unsigned int)A * (unsigned int)B;
        if (op == IR_MULW) {
            *out = (long)p;
            return 1;
        }
        r = sign ? (unsigned long)((long)p >> 32) : p >> 32;
        break;
    }
    /* A division of two constants, which `sizeof a / sizeof a[0]` is
     * wherever it is not an integer constant expression the front end
     * folds -- a udiv on every target, and a __udivsi3 call on AVR,
     * where there is no divide. C truncates toward zero, as this does.
     * Never by zero, and never the one signed quotient that overflows
     * (MIN / -1): both trap on some machines, and the program must see
     * that happen where it did. 32 and 64 bits only, the widths the
     * operands are read at here. */
    case IR_DIV: case IR_MOD:
        if ((w != 4 && w != 8) || ub == 0)
            return 0;
        if (sign) {
            long min = w == 8 ? (long)(1UL << 63) : (long)INT32_MIN;
            if (sa == min && sb == -1)
                return 0;
            r = (unsigned long)(op == IR_DIV ? sa / sb : sa % sb);
        } else {
            r = op == IR_DIV ? ua / ub : ua % ub;
        }
        break;
    case IR_AND: r = ua & ub; break;
    case IR_OR:  r = ua | ub; break;
    case IR_XOR: r = ua ^ ub; break;
    case IR_SHL: r = ua << sh; break;
    case IR_SHR: r = sign ? (unsigned long)(sa >> sh) : (ua >> sh); break;
    case IR_CMP: {
        int c;
        switch (pred) {
        case B_EQ: c = ua == ub; break;
        case B_NE: c = ua != ub; break;
        case B_LT: c = sign ? sa < sb  : ua < ub;  break;
        case B_LE: c = sign ? sa <= sb : ua <= ub; break;
        case B_GT: c = sign ? sa > sb  : ua > ub;  break;
        case B_GE: c = sign ? sa >= sb : ua >= ub; break;
        default: return 0;
        }
        r = c ? 1 : 0;
        break;
    }
    default:
        return 0;
    }
    *out = norm((long)r, w);
    return 1;
}

/* Combine `(x INNER c1) OUTER c2` into one `x OP c` -- the arithmetic of
 * reassociation, with no IR in it so the cases can be read at once.
 * Everything is computed unsigned and normalised to the width, because
 * a signed overflow here is undefined and the machine's answer is the
 * one the program will see either way. */

/* Fold an IR_EXT of a constant: keep `size` low bytes, then sign/zero-extend. */
long fold_ext(long A, int size, int sign, int w)
{
    int bits = size * 8;
    unsigned long m = bits >= 64 ? ~0UL : (((unsigned long)1 << bits) - 1);
    unsigned long low = (unsigned long)A & m;
    long r;
    if (sign && bits < 64 && (low & ((unsigned long)1 << (bits - 1))))
        r = (long)(low | ~m);
    else
        r = (long)low;
    return norm(r, w);
}

/* If b is 2^k (k>=1), return k; else -1. */
static int log2_pow2(long b)
{
    if (b <= 1 || (b & (b - 1)))
        return -1;
    int k = 0;
    while ((b >>= 1))
        k++;
    return k;
}

/* Retarget a single-use constant temp to a new value in place (used by strength
 * reduction: rewrite `x * 8` as `x << 3` by turning the `8` literal into `3`).
 * Safe only when the constant has exactly one use — CSE may have shared it. */
static int retarget_const(struct ir_func *fn, struct defs *d, const int *use,
                          int ctemp, long newval)
{
    if (ctemp < 0 || d->cnt[ctemp] != 1 || d->ins[ctemp] < 0 || use[ctemp] != 1)
        return 0;
    struct ir_ins *ci = &fn->ins[d->ins[ctemp]];
    if (ci->op != IR_CONST || ci->flt)
        return 0;
    ci->imm = newval;
    return 1;
}

/* A conversion of a constant, done now: `(double)1000000` was a call to
 * __floatsidf at run time on every soft-float target, and `float s = 0`
 * a vcvt on the FPU ones. `size` is the source's width and `w` the
 * result's, as the backends read them; a float constant is its bit
 * pattern in an IR_CONST, as irgen's emit_fconst makes it.
 *
 * The host computes it, with its IEEE round-to-nearest -- what every
 * target does by default. Declined where the answer is not that simple:
 * a float that is not finite, or out of the integer type's range (C
 * leaves that undefined and targets really do differ -- ARM saturates,
 * x86 gives the "integer indefinite"), a NaN through a float/double
 * conversion (a soft-float runtime need not keep its payload as the host
 * does), and anything 16 bytes wide. */
static double fc_bits_to(long bits, int size)
{
    if (size == 8) {
        double d;
        memcpy(&d, &bits, 8);
        return d;
    }
    {
        unsigned int u = (unsigned int)bits;
        float f;
        memcpy(&f, &u, 4);
        return (double)f;
    }
}
static long fc_to_bits(double d, int w)
{
    long bits = 0;
    if (w == 8) {
        memcpy(&bits, &d, 8);
    } else {
        float f = (float)d;
        unsigned int u;
        memcpy(&u, &f, 4);
        bits = (long)u;
    }
    return bits;
}
/* A float or double operation on constants, as the machine does it: in
 * the operation's own format, one rounding, the default rounding mode
 * (EmbCC does not honour FENV_ACCESS, and keeps no exception flags).
 * Constants arrive as bit patterns, the form irgen gives them.
 *
 * Not when an operand or the result is a NaN: WHICH NaN an invalid
 * operation produces is the machine's choice (x86's default NaN is
 * negative, ARM's positive) and so is how a payload propagates, so the
 * folded bits could differ from the ones the program would compute.
 * Long double (width 16) is never here. A comparison folds to 0 or 1;
 * with NaN excluded every predicate is the plain ordered one. Integer
 * division is not folded (a divide by zero must happen at run time);
 * a floating one by zero is infinity, which is what the machine gives.
 *
 * Every float constant expression used to be computed at run time:
 * `(struct color){ 251/255.0f, ... }` was a divss per component, and
 * EmbLinkOs's ui/theme/theme.c was five times gcc's size for it. */
static int fold_fp(const struct ir_ins *i, long A, long B, long *out)
{
    if (i->w == 4) {
        unsigned int ua = (unsigned int)A, ub = (unsigned int)B, ur;
        float a, b, r;
        memcpy(&a, &ua, 4);
        memcpy(&b, &ub, 4);
        if (a != a || (i->op != IR_NEG && b != b))
            return 0;
        switch (i->op) {
        case IR_ADD: r = a + b; break;
        case IR_SUB: r = a - b; break;
        case IR_MUL: r = a * b; break;
        case IR_DIV: r = a / b; break;
        case IR_NEG: r = -a; break;
        case IR_CMP:
            switch (i->pred) {
            case B_EQ: *out = a == b; return 1;
            case B_NE: *out = a != b; return 1;
            case B_LT: *out = a < b;  return 1;
            case B_LE: *out = a <= b; return 1;
            case B_GT: *out = a > b;  return 1;
            case B_GE: *out = a >= b; return 1;
            default: return 0;
            }
        default: return 0;
        }
        if (r != r)
            return 0;
        memcpy(&ur, &r, 4);
        *out = (long)ur;
        return 1;
    }
    if (i->w == 8) {
        double a, b, r;
        memcpy(&a, &A, 8);
        memcpy(&b, &B, 8);
        if (a != a || (i->op != IR_NEG && b != b))
            return 0;
        switch (i->op) {
        case IR_ADD: r = a + b; break;
        case IR_SUB: r = a - b; break;
        case IR_MUL: r = a * b; break;
        case IR_DIV: r = a / b; break;
        case IR_NEG: r = -a; break;
        case IR_CMP:
            switch (i->pred) {
            case B_EQ: *out = a == b; return 1;
            case B_NE: *out = a != b; return 1;
            case B_LT: *out = a < b;  return 1;
            case B_LE: *out = a <= b; return 1;
            case B_GT: *out = a > b;  return 1;
            case B_GE: *out = a >= b; return 1;
            default: return 0;
            }
        default: return 0;
        }
        if (r != r)
            return 0;
        memcpy(out, &r, 8);
        return 1;
    }
    return 0;
}

static int fold_cvt(const struct ir_ins *i, long A, long *out)
{
    if (i->w == 16 || i->size == 16 || i->w <= 0 || i->size <= 0)
        return 0;
    if (i->op == IR_I2F) {
        long v;
        /* Where an unsigned 32-bit source is widened to a 64-bit signed
         * conversion (target_widen_unsigned_fp_cvt), irgen relies on the
         * MACHINE having zero-extended it -- there is no extension in the
         * IR to fold, and the constant's own normalisation need not match.
         * Only a value that reads the same either way is folded. */
        if (i->size == 8 && target_widen_unsigned_fp_cvt() &&
            (A < 0 || A > 0x7fffffffL))
            return 0;
        v = fold_ext(A, i->size, i->sign, 8);
        double d;
        float f;
        if (i->w != 4 && i->w != 8)
            return 0;
        if (i->w == 4) {
            /* ONE rounding, straight to float: through double first
             * would round twice for a 64-bit value */
            f = i->sign ? (float)v : (float)(unsigned long)v;
            *out = fc_to_bits((double)f, 4);
            return 1;
        }
        d = i->sign ? (double)v : (double)(unsigned long)v;
        *out = fc_to_bits(d, 8);
        return 1;
    }
    if (i->size != 4 && i->size != 8)
        return 0;
    {
        double d = fc_bits_to(A, i->size);
        if (d != d)                            /* NaN */
            return 0;
        if (i->op == IR_F2F) {
            if (i->w != 4 && i->w != 8)
                return 0;
            if (i->w == 4 && i->size == 8) {
                /* the rounding of double to float, which may overflow
                 * to infinity -- as the conversion does at run time */
                float f = (float)d;
                *out = fc_to_bits((double)f, 4);
            } else {
                *out = fc_to_bits(d, i->w);
            }
            return 1;
        }
        /* IR_F2I: truncate toward zero, only when the result fits. C's
         * own conversion truncates, so the host does it -- after the
         * range check, which is what makes the conversion defined. (No
         * math builtins here: EmbCC compiles this file too.) */
        {
            int bits = i->w * 8;
            double two63 = (double)(1ULL << 63);
            if (bits > 64 || bits <= 0)
                return 0;
            if (i->sign) {
                double hi = bits == 64 ? two63 : (double)(1ULL << (bits - 1));
                if (!(d > -hi - 1.0 && d < hi))
                    return 0;
                *out = norm((long)(long long)d, i->w);
            } else {
                double hi = bits == 64 ? two63 * 2.0 : (double)(1ULL << bits);
                if (!(d > -1.0 && d < hi))
                    return 0;
                *out = norm((long)(unsigned long long)d, i->w);
            }
            return 1;
        }
    }
}

/* The bits of vreg v that are zero in EVERY value it can take, at width
 * w: a shift leaves the bits it shifted over, a multiply by a multiple
 * of 2^k the low k, a mask the bits it clears, a zero-extension
 * everything above the source, and an or/xor only what both sides
 * leave. Bits above the width are reported zero, which is what the
 * caller's own width mask discards. Conservative: a bit not proven
 * zero is not claimed, and the walk stops a few definitions up. */
static unsigned long known_zero_ins(struct ir_func *fn, struct defs *d,
                                    const struct ir_ins *i, int w, int depth);

static unsigned long known_zero(struct ir_func *fn, struct defs *d, int v,
                                int w, int depth)
{
    unsigned long wm = w == 8 ? ~0UL : 0xffffffffUL, hi = ~wm;
    if (v < 0 || v >= fn->nvregs || depth > 4)
        return hi;
    /* A temp with several definitions -- a join's, after phi
     * destruction: `b = (u8)x` on one path and `b = 0x7e` on the other
     * -- has the zeros all of them have. Only with the lists built, only
     * a temp (a variable has its parameter binding too), and only when
     * the lists hold every definition (not a landing pad's). */
    if (d->cnt[v] > 1) {
        if (!d->first || v < fn->nvars || depth > 2)
            return hi;
        unsigned long kz = ~0UL;
        int seen = 0;
        for (int n = d->first[v]; n >= 0; n = d->next[n], seen++)
            kz &= known_zero_ins(fn, d, &fn->ins[n], w, depth + 1);
        return seen == d->cnt[v] ? kz | hi : hi;
    }
    if (d->cnt[v] != 1 || d->ins[v] < 0)
        return hi;
    return known_zero_ins(fn, d, &fn->ins[d->ins[v]], w, depth);
}

/* known_zero of what one instruction writes. */
static unsigned long known_zero_ins(struct ir_func *fn, struct defs *d,
                                    const struct ir_ins *i, int w, int depth)
{
    unsigned long wm = w == 8 ? ~0UL : 0xffffffffUL, hi = ~wm;
    if (i->flt || i->w == 16 || (i->w != w && i->op != IR_EXT))
        return hi;
    long B = 0;
    int kb = i->imm_b ? (B = i->imm, 1) : get_const(fn, d, i->b, &B);
    unsigned long ub = (unsigned long)B & wm;
    switch (i->op) {
    case IR_CONST:
        return hi | (~(unsigned long)i->imm & wm);
    case IR_MOV:
        return hi | known_zero(fn, d, i->a, w, depth + 1);
    case IR_SHL:
        if (!kb || B < 0 || B >= 8 * w)
            return hi;
        return hi | ((known_zero(fn, d, i->a, w, depth + 1) << B) & wm) |
               ((1UL << B) - 1);
    case IR_SHR:
        if (!kb || B < 0 || B >= 8 * w || i->sign)
            return hi;                   /* an arithmetic shift copies the sign */
        return hi | ((known_zero(fn, d, i->a, w, depth + 1) & wm) >> B) |
               (~(wm >> B) & wm);
    case IR_AND:
        return hi | known_zero(fn, d, i->a, w, depth + 1) |
               (kb ? (~ub & wm) : known_zero(fn, d, i->b, w, depth + 1));
    case IR_OR: case IR_XOR:
        return hi | (known_zero(fn, d, i->a, w, depth + 1) &
                     (kb ? (~ub & wm) : known_zero(fn, d, i->b, w, depth + 1)));
    case IR_MUL: {
        if (!kb || ub == 0)
            return hi;
        int tz = 0;
        while (!(ub & 1)) { ub >>= 1; tz++; }
        return hi | ((1UL << tz) - 1);   /* a multiple of 2^tz ends in tz zeros */
    }
    case IR_EXT:
        if (i->sign || i->size <= 0 || i->size >= w)
            return hi;
        return hi | (~((1UL << (8 * i->size)) - 1) & wm);
    case IR_LOAD:               /* `load.4:1` zero-extends the byte it reads */
        if (i->sign || i->size <= 0 || i->size >= w)
            return hi;
        return hi | (~((1UL << (8 * i->size)) - 1) & wm);
    default:
        return hi;
    }
}

/* Is vreg v defined earlier in the same basic block as instruction n --
 * no label between its definition and n? */
static int defined_in_block(struct ir_func *fn, struct defs *d, int v, int n)
{
    if (v < 0 || d->cnt[v] != 1 || d->ins[v] < 0 || d->ins[v] >= n)
        return 0;
    for (int m = d->ins[v] + 1; m < n; m++)
        if (fn->ins[m].op == IR_LABEL)
            return 0;
    return 1;
}

/* ---- block-local constants ----
 *
 * get_const knows a temp only when it has ONE definition, and the temps a
 * loop carries have one per incoming edge: phi destruction assigns an
 * induction variable before the loop and again at the latch. Inside one
 * block that does not matter. After `i = const 0`, and until something
 * else writes i, i is 0. That is exactly where a rotated loop's guard
 * sits -- `i = 0; if (!(i < 24)) skip the loop` -- and the guard stayed
 * a compare and a branch, run on every entry to every counted loop.
 *
 * lk_gen[v] == gen says v is known to hold lk_val[v] at width lk_w[v].
 * A new generation starts at every label and after every control
 * transfer, so nothing is believed across a block boundary; a definition
 * of v by anything but a constant, or a copy of a known value, forgets
 * it; and an instruction that writes temps def_target does not report
 * (inline asm, a landing pad) starts a new generation too. */

void lk_note(struct lkconst *k, const struct ir_ins *i)
{
    switch (i->op) {
    case IR_LABEL: case IR_JMP: case IR_BRZ: case IR_BRNZ: case IR_SWITCH:
    case IR_RET: case IR_IGOTO: case IR_UD2: case IR_ASM: case IR_LANDING:
        k->gen++;
        return;
    default:
        break;
    }
    int t = def_target(i);
    if (t < 0 || t >= k->nv)
        return;
    if (i->op == IR_CONST && !i->flt && (i->w == 4 || i->w == 8)) {
        k->gen_of[t] = k->gen; k->val[t] = i->imm; k->w[t] = i->w;
    } else if (i->op == IR_MOV && !i->vol && !i->flt && i->a >= 0 &&
               i->a < k->nv && i->a != t && k->gen_of[i->a] == k->gen &&
               k->w[i->a] == i->w) {
        k->gen_of[t] = k->gen; k->val[t] = k->val[i->a]; k->w[t] = i->w;
    } else {
        k->gen_of[t] = 0;
    }
}

/* Is v known here, at width w? */
int lk_get(const struct lkconst *k, int v, int w, long *out)
{
    if (v < 0 || v >= k->nv || k->gen_of[v] != k->gen || k->w[v] != w)
        return 0;
    *out = k->val[v];
    return 1;
}

/* May `ext.8 x` become `mov.8 x` when x is known zero above the
 * extension's size? Where a register is 64 bits, yes: a 32-bit value sits
 * in it extended, as every 64-bit backend keeps it. Where an eight-byte
 * value is a register PAIR -- every target with pointers narrower than 8
 * -- only if x itself is eight bytes wide wherever it is defined: a
 * four-byte x has no high word, and the copy read whatever the pair's
 * other register held. That was fuzz seed 927: `l0 = (u64)a0` became an
 * eight-byte copy of a four-byte byte-extension, wrong on SPARC, RV32,
 * Cortex-M and MIPS at -O1 and up, right on x86-64 and AArch64. */
static int kz_copy_wide_ok(struct ir_func *fn, struct defs *d,
                           const struct ir_ins *i)
{
    if (i->w != 8 || target_ptr_size() >= 8)
        return 1;
    if (i->a < fn->nparams)
        return 0;                       /* its width is the ABI's question */
    int any = 0;
    for (int n = d->first[i->a]; n >= 0; n = d->next[n]) {
        const struct ir_ins *in = &fn->ins[n];
        if (in->op == IR_STVAR ? in->size != 8 : in->w != 8)
            return 0;
        any = 1;
    }
    return any;
}

/* ---- long double constants ----
 *
 * A long double is 16 bytes -- x87 extended on x86-64, IEEE binary128 on
 * AArch64, RV64, MIPS64, SPARC and the rest -- too wide for IR_CONST, so
 * irgen keeps each constant in .rodata and loads it (`load.16 [straddr
 * strN]`). fold_fp never saw one: `2.0L * 3.0L` was a multiply at run
 * time, an x87 fmul through three slots or a __multf3 call, and a loop
 * `for (64) b *= 2.0L` unrolled to 64 of them.
 *
 * The arithmetic is sema/ldfloat's: each operation exact, then rounded
 * once to the target's format, round-to-nearest-even -- what the target's
 * x87 (precision control at 64 bits, as every x86-64 ABI EmbCC targets
 * leaves it) or its binary128 library computes. NaN is never folded, as
 * fold_fp does not: which NaN an operation makes is the machine's choice.
 *
 * A value is known when its single definition is such a load, or one
 * this pass folded (ldf_val), or -- inside one block -- when it was just
 * read from a 16-byte local the function never takes the address of,
 * after a known value was stored there: an unrolled `b *= 2.0L` keeps b in
 * its slot, since mem2reg does not promote 16-byte locals.
 *
 * A folded result becomes `load.16 [p]` with p a new `straddr` of the
 * result's interned bytes; the straddrs are inserted when the pass ends
 * (ld_insert), and the operands' loads are left for dead-code removal. */
struct ir_unit *g_fold_unit;     /* where pass_fold interns */

struct ldfold {
    struct ldf **val;          /* per vreg: folded by this pass */
    struct ldf **kval;         /* per vreg: known in this block (kgen) */
    int *kgen;
    struct ldf **vval;         /* per local: holds this, in this block */
    int *vgen;
    char *vok;                 /* per local: 16 bytes, never addressed */
    int nv, nvars;
    /* the straddrs to insert: before instruction at[k], p[k] = str lab[k] */
    int *at, *p, *lab, n, cap;
};

/* The value a 16-byte .rodata constant holds -- only if its bytes are
 * exactly what ldf_encode makes of that value, so an encoding x87 would
 * load as something else (an unnormal, a pseudo-denormal) or a NaN is
 * not mistaken for a number. */
static struct ldf *ld_of_str(int label)
{
    if (!g_fold_unit || label < 0 || label >= g_fold_unit->nstrs)
        return NULL;
    const struct ir_str *s = &g_fold_unit->strs[label];
    if (s->len != 16 || !s->bytes)
        return NULL;
    enum ldf_fmt fmt = ldf_target_fmt();
    unsigned char b[16], back[16];
    int be = target_big_endian();
    for (int k = 0; k < 16; k++)
        b[k] = (unsigned char)s->bytes[be ? 15 - k : k];
    struct ldf *v = ldf_from_bytes(b, fmt);
    if (!v || ldf_cmp(v, v) == LDF_UNORDERED)
        return NULL;
    ldf_encode(v, fmt, back);
    if (memcmp(b, back, fmt == LDF_X87 ? 10 : 16) != 0)
        return NULL;
    return v;
}

/* Is v a long double constant here (instruction-order position n)? */
static struct ldf *ld_known(struct ir_func *fn, struct defs *d,
                            const struct ldfold *L, int gen, int v)
{
    if (v < 0 || v >= L->nv)
        return NULL;
    if (L->val[v] && d->cnt[v] == 1)
        return L->val[v];
    if (L->kgen[v] == gen && L->kval[v])
        return L->kval[v];
    if (d->cnt[v] != 1 || d->ins[v] < 0)
        return NULL;
    const struct ir_ins *ld = &fn->ins[d->ins[v]];
    if (ld->op != IR_LOAD || ld->size != 16 || ld->w != 16 || ld->vol ||
        ld->flash || ld->memoff || ld->a < 0 || ld->a >= L->nv ||
        d->cnt[ld->a] != 1 || d->ins[ld->a] < 0)
        return NULL;
    const struct ir_ins *sa = &fn->ins[d->ins[ld->a]];
    return sa->op == IR_STRADDR ? ld_of_str(sa->label) : NULL;
}

/* What instruction i, just executed, tells the block about 16-byte
 * locals and the values read from them. */
static void ld_note(struct ir_func *fn, struct defs *d, struct ldfold *L,
                    int gen, const struct ir_ins *i)
{
    if (i->op == IR_STVAR && i->dst >= 0 && i->dst < L->nvars) {
        struct ldf *x = L->vok[i->dst] && i->size == 16 && !i->vol
                        ? ld_known(fn, d, L, gen, i->a) : NULL;
        L->vval[i->dst] = x;
        L->vgen[i->dst] = x ? gen : 0;
        return;
    }
    int t = def_target(i);
    if (t < 0 || t >= L->nv)
        return;
    if (i->op == IR_LDVAR && i->a >= 0 && i->a < L->nvars && L->vok[i->a] &&
        i->size == 16 && i->w == 16 && !i->vol && L->vgen[i->a] == gen &&
        L->vval[i->a]) {
        L->kval[t] = L->vval[i->a];
        L->kgen[t] = gen;
    } else {
        L->kgen[t] = 0;
    }
}

/* Turn i into a load of the constant x, its straddr to come before
 * instruction n. */
static void ld_become(struct ir_func *fn, struct ldfold *L, struct ir_ins *i,
                      int n, struct ldf *x)
{
    unsigned char *b = xcalloc(1, 16);
    ldf_encode_target(x, ldf_target_fmt(), b);
    int lab = ir_intern_aligned(g_fold_unit, (const char *)b, 16, 16);
    int p = fn->nvregs++;
    if (L->n == L->cap) {
        L->cap = L->cap ? 2 * L->cap : 8;
        L->at = xrealloc(L->at, (size_t)L->cap * sizeof *L->at);
        L->p = xrealloc(L->p, (size_t)L->cap * sizeof *L->p);
        L->lab = xrealloc(L->lab, (size_t)L->cap * sizeof *L->lab);
    }
    L->at[L->n] = n; L->p[L->n] = p; L->lab[L->n] = lab; L->n++;
    int dst = i->dst, line = i->line, col = i->col, synth = i->synth;
    memset(i, 0, sizeof *i);
    i->op = IR_LOAD;
    i->dst = dst; i->a = p; i->b = i->c = -1;
    i->size = 16; i->w = 16; i->natural = 1;
    i->pred = B_ADD; i->label = -1; i->callee_sym = i->glob_sym = -1;
    i->line = line; i->col = col; i->synth = synth;
    if (dst >= 0 && dst < L->nv)
        L->val[dst] = x;
}

/* A float or double's bits as an exact ldf; NULL if it is a NaN. */
static struct ldf *ld_from_bits(long A, int size)
{
    double v = fc_bits_to(A, size);
    return v == v ? ldf_from_double(v) : NULL;
}

/* Fold i if it is long double arithmetic or a conversion to or from one
 * on constants. Returns 1 if it changed. */
static int ld_fold(struct ir_func *fn, struct defs *d, struct ldfold *L,
                   struct lkconst *lk, struct ir_ins *i, int n)
{
    enum ldf_fmt fmt = ldf_target_fmt();
    int gen = lk->gen;
    if (fmt != LDF_X87 && fmt != LDF_QUAD)
        return 0;
    if (i->flt && i->w == 16 &&
        (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL ||
         i->op == IR_DIV || i->op == IR_NEG)) {
        struct ldf *a = ld_known(fn, d, L, gen, i->a), *b = NULL, *r;
        if (!a || (i->op != IR_NEG && !(b = ld_known(fn, d, L, gen, i->b))))
            return 0;
        r = i->op == IR_NEG ? ldf_neg(a)
          : ldf_binop(i->op == IR_ADD ? '+' : i->op == IR_SUB ? '-'
                      : i->op == IR_MUL ? '*' : '/', a, b, fmt);
        if (!r || ldf_cmp(r, r) == LDF_UNORDERED)
            return 0;
        ld_become(fn, L, i, n, r);
        return 1;
    }
    /* long double -> float or double: one rounding, from the exact value */
    if (i->op == IR_F2F && i->size == 16 && (i->w == 4 || i->w == 8)) {
        struct ldf *a = ld_known(fn, d, L, gen, i->a);
        unsigned char b[8];
        if (!a)
            return 0;
        enum ldf_fmt to = i->w == 4 ? LDF_FLOAT : LDF_DOUBLE;
        ldf_encode(ldf_round(a, to), to, b);
        unsigned long bits = 0;
        for (int k = 0; k < i->w; k++)
            bits |= (unsigned long)b[k] << (8 * k);
        to_const(i, i->w == 4 ? (long)(unsigned int)bits : (long)bits);
        i->flt = 0;
        return 1;
    }
    /* A widening of a constant to binary128 is a library call of eight
     * bytes or so, and its folded form a sixteen-byte constant and the
     * load of it: at -Os the call is kept. (x87 loads either way.) */
    if (g_opt_size && fmt == LDF_QUAD &&
        (i->op == IR_I2F || i->op == IR_F2F) && i->w == 16)
        return 0;
    /* float or double -> long double: exact */
    if (i->op == IR_F2F && i->w == 16 && (i->size == 4 || i->size == 8)) {
        long A;
        struct ldf *x;
        if (!get_const(fn, d, i->a, &A) && !lk_get(lk, i->a, i->size, &A))
            return 0;
        if (!(x = ld_from_bits(A, i->size)))
            return 0;
        ld_become(fn, L, i, n, x);
        return 1;
    }
    /* an integer -> long double: rounded once (a 64-bit integer does not
     * fit binary64, and x87's 64-bit significand holds it exactly) */
    if (i->op == IR_I2F && i->w == 16 && (i->size == 4 || i->size == 8)) {
        long A;
        int wide = 0;                       /* known as an 8-byte value */
        if (get_const(fn, d, i->a, &A))
            wide = fn->ins[d->ins[i->a]].w == 8;
        else if (lk_get(lk, i->a, i->size, &A))
            wide = i->size == 8;
        else
            return 0;
        /* as fold_cvt (see there), unless the constant is 8 bytes itself */
        if (i->size == 8 && !wide && target_widen_unsigned_fp_cvt() &&
            (A < 0 || A > 0x7fffffffL))
            return 0;
        long v = fold_ext(A, i->size, i->sign, 8);
        ld_become(fn, L, i, n, ldf_round(ldf_from_int(v, !i->sign), fmt));
        return 1;
    }
    return 0;
}

static void ld_insert(struct ir_func *fn, struct ldfold *L);

int pass_fold(struct ir_func *fn)
{
    struct defs d;
    compute_defs(fn, &d);
    /* use counts, for the single-use check strength reduction needs */
    int *use = xcalloc((size_t)fn->nvregs, sizeof *use);
    struct ucount uc = { use, fn->nvregs };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, &uc);
    int changed = 0;
    struct lkconst lk = {
        xcalloc((size_t)fn->nvregs, sizeof(int)),
        xmalloc((size_t)fn->nvregs * sizeof(long)),
        xmalloc((size_t)fn->nvregs * sizeof(int)), 1, fn->nvregs
    };
    int nv0 = fn->nvregs, nl = fn->nvars > 0 ? fn->nvars : 1;
    struct ldfold L = {
        xcalloc((size_t)nv0, sizeof(struct ldf *)),
        xcalloc((size_t)nv0, sizeof(struct ldf *)),
        xcalloc((size_t)nv0, sizeof(int)),
        xcalloc((size_t)nl, sizeof(struct ldf *)),
        xcalloc((size_t)nl, sizeof(int)),
        xcalloc((size_t)nl, 1), nv0, fn->nvars, NULL, NULL, NULL, 0, 0
    };
    for (int v = 0; v < fn->nvars; v++)
        L.vok[v] = fn->locals[v].is_ldouble && !fn->locals[v].is_volatile;
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_ADDR && fn->ins[n].a >= 0 &&
            fn->ins[n].a < fn->nvars)
            L.vok[fn->ins[n].a] = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        /* What the previous instruction left, in its final form: one
         * this loop has just folded to a constant is a constant. */
        if (n > 0) {
            lk_note(&lk, &fn->ins[n - 1]);
            ld_note(fn, &d, &L, lk.gen, &fn->ins[n - 1]);
        }
        if (i->op == IR_LABEL)
            lk.gen++;
        if (ld_fold(fn, &d, &L, &lk, i, n)) {
            changed = 1;
            continue;
        }
        if (i->flt) {
            /* never as an integer -- as a float, from bit patterns. An
             * operand is a constant when it has one definition that is
             * one, or -- for the arithmetic, whose width is its operands'
             * -- when the block has just made it one (lk_note), as the
             * integer folds below already ask: an unrolled `b *= 2.0`
             * reuses b's name, so b has several definitions and the
             * chain of multiplies by constants stayed a chain. Not a
             * compare, whose w is its result's, not its operands'. */
            long A, B = 0, r;
            int arith = i->op == IR_ADD || i->op == IR_SUB ||
                        i->op == IR_MUL || i->op == IR_DIV || i->op == IR_NEG;
            if ((arith || i->op == IR_CMP) &&
                (get_const(fn, &d, i->a, &A) ||
                 (arith && lk_get(&lk, i->a, i->w, &A))) &&
                (i->op == IR_NEG || get_const(fn, &d, i->b, &B) ||
                 (arith && lk_get(&lk, i->b, i->w, &B))) &&
                fold_fp(i, A, B, &r)) {
                to_const(i, r);
                i->flt = 0;             /* a bit pattern, as irgen's are */
                changed = 1;
            }
            continue;
        }
        /* Nor an __int128 one as a long. `imm` is 64 bits and norm()
         * truncates anything that is not width 8, so a 128-bit constant
         * cannot even be held here, let alone folded. Skipping these is
         * what lets the REST of the pipeline run on a function that
         * computes with __int128, which used to be excluded whole. */
        if (i->w == 16)
            continue;
        long A, B;
        int ka = get_const(fn, &d, i->a, &A);
        int kb = get_const(fn, &d, i->b, &B);

        if (i->op == IR_NEG || i->op == IR_BNOT) {
            if (ka) {
                unsigned long r = i->op == IR_NEG ? -(unsigned long)A
                                                  : ~(unsigned long)A;
                to_const(i, norm((long)r, i->w));
                changed = 1;
            }
            continue;
        }
        if (i->op == IR_MOV && ka && !i->flt && (i->w == 4 || i->w == 8) &&
            target_get() != TARGET_AVR && !defined_in_block(fn, &d, i->a, n)) {
            /* A copy of a constant IS that constant. The copies are what
             * phi destruction leaves at a loop's entry and latch, and a
             * constant they all read stayed live across the whole loop
             * for their sake -- in a callee-saved register, or a slot:
             * `bounds` in tests/bench kept its zero on the stack and
             * reloaded it every outer iteration. As a constant of its own
             * each copy is one instruction and no live range. Only across
             * a block boundary, though: within one block value numbering
             * merges equal constants into exactly this copy, and the two
             * rules undid each other forever -- src/arch/x86_64/codegen.c
             * never finished compiling. Not on AVR:
             * a constant there is an ldi per byte (four `mov r,r1` for a
             * zero) where the copy was a movw per pair, and lib/libc's
             * math grew by 256 bytes. */
            to_const(i, norm(A, i->w));
            changed = 1;
            continue;
        }
        if ((i->op == IR_I2F || i->op == IR_F2I || i->op == IR_F2F) && ka) {
            long r;
            if (fold_cvt(i, A, &r)) {
                to_const(i, r);
                changed = 1;
            }
            continue;
        }
        if (i->op == IR_EXT) {
            if (ka) {
                to_const(i, fold_ext(A, i->size, i->sign, i->w));
                changed = 1;
            } else if (i->size >= i->w) {
                /* An extension to a width the value already fills does
                 * nothing: `ext.4:4s` asks for the low four bytes
                 * sign-extended to four bytes, which is what it was
                 * handed. irgen emits these for every `int` operand of
                 * an `int` operation, so they are not rare -- four in a
                 * two-parameter comparison -- and each one costs an
                 * instruction on aarch64 (`asr w, w, #0`) that the x86
                 * backend was already folding into its addressing. */
                to_mov(i, i->a);
                changed = 1;
            } else if (i->a >= 0 && i->a < fn->nvregs && d.cnt[i->a] == 1 &&
                       d.ins[i->a] >= 0 &&
                       fn->ins[d.ins[i->a]].op == IR_EXT &&
                       !fn->ins[d.ins[i->a]].flt &&
                       fn->ins[d.ins[i->a]].w >= i->size) {
                /* An extension of an extension, which promoting a narrow
                 * local makes of every read after a store: `x = ext.4:2
                 * (ext.4:2 y)`. Wider than the inner width, or the same
                 * width and kind, the outer changes nothing -- the bits
                 * it would write already hold that extension -- so it is
                 * a copy. At the same width with the other kind, or
                 * narrower, it only needs the inner's SOURCE, provided
                 * that has one definition and so cannot have changed. */
                const struct ir_ins *in = &fn->ins[d.ins[i->a]];
                int src_ok = in->a >= 0 && in->a < fn->nvregs &&
                             d.cnt[in->a] == 1 && i->a != in->a;
                /* Wider than the inner, the outer changes nothing when the
                 * inner ZERO-extended (the bits above are 0, which either
                 * kind of extension keeps) or both sign-extend -- but a
                 * zero-extension of a sign-extended value clears what the
                 * inner set: (unsigned short)(signed char)-1 is 65535, and
                 * calling that a copy returned -1 at -O2. */
                int keeps = (i->size > in->size && (i->sign || !in->sign)) ||
                            (i->size == in->size && i->sign == in->sign);
                if (keeps && i->w == in->w) {
                    to_mov(i, i->a);
                    changed = 1;
                } else if (keeps && src_ok) {
                    /* WIDER than the inner result (`ext.8:4` of an
                     * `ext.4:2`): not a copy -- the high bytes are the
                     * outer's to make -- but the inner's extension taken
                     * straight to the outer width. As a copy it handed a
                     * four-byte value to an eight-byte add. */
                    i->a = in->a;
                    i->size = in->size;
                    i->sign = in->sign;
                    changed = 1;
                } else if (!keeps && src_ok && i->size <= in->size) {
                    /* Narrower, or the same width and the other kind: the
                     * outer reads only bytes the inner left as they were,
                     * so it may read them from the inner's source. Wider,
                     * it would read bytes the inner made. */
                    i->a = in->a;
                    changed = 1;
                }
            } else if (i->a >= 0 && i->a < fn->nvregs && d.cnt[i->a] == 1 &&
                       d.ins[i->a] >= 0 &&
                       fn->ins[d.ins[i->a]].op == IR_LOAD &&
                       !fn->ins[d.ins[i->a]].flt &&
                       fn->ins[d.ins[i->a]].w == i->w) {
                /* An extension of a narrow LOAD, which already extended
                 * what it read -- `load.4:1` is a byte zero-extended to
                 * four. The same rule as above decides whether the outer
                 * one is a copy; 31 byte loads in the Thumb corpus were
                 * followed by an `ext.4:1` of their own result. */
                const struct ir_ins *in = &fn->ins[d.ins[i->a]];
                if ((i->size > in->size && (i->sign || !in->sign)) ||
                    (i->size == in->size && i->sign == in->sign)) {
                    to_mov(i, i->a);
                    changed = 1;
                }
            } else if (i->a >= 0 && i->a < fn->nvregs && i->size > 0 &&
                       (i->w == 4 || i->w == 8) &&
                       !getenv("EMBCC_NO_KZEXT")) {
                /* An extension of a value whose high bits are already
                 * zero (known_zero) -- `(u8)(x >> 24)`, `(u8)(x & 0x7f)`,
                 * and a join of such values, `b = c ? (u8)x : 0x7e` --
                 * is a copy: zero-extending changes nothing above the
                 * size, and sign-extending copies a sign bit that is 0.
                 * The state machine of tools/bench extended its input
                 * byte once where it was made and again in every case
                 * that read it. */
                defs_lists(fn, &d);
                unsigned long wm = i->w == 8 ? ~0UL : 0xffffffffUL;
                unsigned long keep =
                    (1UL << (8 * i->size - (i->sign ? 1 : 0))) - 1;
                if ((wm & ~keep & ~known_zero(fn, &d, i->a, i->w, 0)) == 0 &&
                    kz_copy_wide_ok(fn, &d, i)) {
                    to_mov(i, i->a);
                    changed = 1;
                }
            }
            continue;
        }

        long r;
        /* Two constants and nothing else: none of the multiply's
         * identities below holds for the high half. */
        if (i->op == IR_MULH || i->op == IR_MULW) {
            if (ka && kb &&
                fold_bin(i->op, A, B, i->w, i->sign, i->pred, &r)) {
                to_const(i, r);
                changed = 1;
            }
            continue;
        }
        switch (i->op) {
        case IR_ADD: case IR_SUB: case IR_MUL:
        case IR_AND: case IR_OR: case IR_XOR:
        case IR_SHL: case IR_SHR: case IR_CMP:
        case IR_DIV: case IR_MOD:   /* folded unless by zero (fold_bin) */
            break;
        default:
            continue;
        }
        /* A loop-carried temp read where the block has just assigned it
         * a constant: see lk_note. Only at the width the constant was
         * written at. A compare at first (a rotated loop's guard); then
         * the arithmetic too, because a FULLY unrolled loop is this shape
         * from end to end -- `b = 0`, then `b*8`, `(b+1)*8`, ... in one
         * block, with the latch's dead `b = b + 4` keeping b two
         * definitions -- and FNV's byte loop shifted by a register four
         * times where clang has uxtb, ubfx and lsr #24. Not a division:
         * see fold_bin. */
        if ((i->op == IR_CMP || i->op == IR_ADD || i->op == IR_SUB ||
             i->op == IR_MUL || i->op == IR_AND || i->op == IR_OR ||
             i->op == IR_XOR || i->op == IR_SHL || i->op == IR_SHR) &&
            !i->imm_b && (i->w == 4 || i->w == 8) &&
            !(ka && kb) && !getenv("EMBCC_NO_LKCONST")) {
            long la, lb;
            int ja = ka || lk_get(&lk, i->a, i->w, &la);
            int jb = kb || lk_get(&lk, i->b, i->w, &lb);
            if (ja && jb) {
                if (!ka) A = la;
                if (!kb) B = lb;
                if (fold_bin(i->op, A, B, i->w, i->sign, i->pred, &r)) {
                    to_const(i, r);
                    changed = 1;
                    continue;
                }
            }
        }
        if (ka && kb) {
            if (fold_bin(i->op, A, B, i->w, i->sign, i->pred, &r)) {
                to_const(i, r);
                changed = 1;
            }
            continue;
        }
        /* `(i * 2) & 1` is zero whatever i is: an AND whose constant
         * reads only bits the other operand can never set (known_zero)
         * is a constant zero, and the branch on it, and the arm behind
         * the branch, go with it -- tests/bench's dead_branch spent
         * eleven instructions an iteration on x86-64 where five do. Only
         * the all-zero answer is taken; trimming a mask would change
         * nothing that runs. */
        if (i->op == IR_AND && kb && !ka && i->a >= 0) {
            unsigned long wm = i->w == 8 ? ~0UL : 0xffffffffUL;
            if (((unsigned long)B & wm & ~known_zero(fn, &d, i->a, i->w, 0)) == 0) {
                to_const(i, 0);
                changed = 1;
                continue;
            }
        }
        /* A COMPARISON OF A COMPARISON. `!!x` is `(x == 0) == 0`, and
         * `(a == b) && (b == c)` re-tests its own result twice more --
         * and each of those retests is a `sete`, a `movzbl` and a
         * `test` on x86 before the one that matters. The inner result is
         * 0 or 1 by construction, so comparing it with zero is the inner
         * comparison itself, negated (`== 0`) or as it stands (`!= 0`).
         *
         * The outer width and signedness fall away with the outer
         * comparison: a 0/1 value compares the same at every width, and
         * the inner comparison keeps its own. 687 sites across lib/libc
         * and lib/libcxx, 417 of them the `sete movzbl test sete` that
         * `!!` and `&&` leave behind. */
        if (i->op == IR_CMP && !i->flt && kb && B == 0 &&
            (i->pred == B_EQ || i->pred == B_NE) && i->a >= 0 &&
            d.cnt[i->a] == 1 && d.ins[i->a] >= 0) {
            struct ir_ins *in = &fn->ins[d.ins[i->a]];
            if (in->op == IR_CMP && in->w != 16) {
                if (i->pred == B_NE) {
                    /* Any comparison's result is already 0 or 1, so
                     * `!= 0` is that value -- true of a FLOAT compare
                     * too, which is why this branch has no `flt` test. */
                    to_mov(i, i->a);
                    changed = 1;
                    continue;
                }
                /* Negating the sense is the part that is not always
                 * sound: over floats `!(a < b)` is NOT `a >= b`, because
                 * an unordered pair makes both false. tests/golden/rt.sh
                 * and complex.c in regalloc-O2.sh both said so. */
                if (!in->flt) {
                    static const enum binop neg[] = {
                        [B_EQ] = B_NE, [B_NE] = B_EQ, [B_LT] = B_GE,
                        [B_LE] = B_GT, [B_GT] = B_LE, [B_GE] = B_LT,
                    };
                    /* `i` IS fn->ins[n], so everything worth keeping
                     * comes off it BEFORE the copy overwrites it. The
                     * inner comparison's OWN width and signedness come
                     * with it: `w` on an IR_CMP is what its operands are
                     * compared at, not what its 0/1 result is read at,
                     * and carrying the outer's 4 into a compare of two
                     * longs truncated both of them. */
                    int dst = i->dst;
                    int line = i->line, col = i->col;
                    enum binop np = neg[in->pred];
                    *i = *in;                   /* the inner comparison... */
                    i->pred = np;               /* ...with the sense flipped */
                    i->dst = dst;
                    i->line = line; i->col = col;
                    changed = 1;
                    continue;
                }
            }
        }
        /* Both operands the SAME value. None of these needs to know
         * anything about what the value is, which is what makes them
         * safe at any width and either signedness -- and `i->flt` is
         * already excluded above, so no NaN can make `x == x` false. */
        if (!i->imm_b && i->a >= 0 && i->a == i->b) {
            switch (i->op) {
            case IR_SUB: case IR_XOR:
                to_const(i, 0); changed = 1; continue;
            case IR_AND: case IR_OR:
                to_mov(i, i->a); changed = 1; continue;
            case IR_CMP:
                to_const(i, i->pred == B_EQ || i->pred == B_LE ||
                            i->pred == B_GE);
                changed = 1; continue;
            default:
                break;
            }
        }
        /* The constant goes on the RIGHT, and otherwise the lower vreg
         * does. Value numbering keys an operation on its operands in
         * order, so `1 + x` and `x + 1` were two different values of the
         * same expression and neither ever matched the other. */
        if (!i->imm_b && i->a >= 0 && i->b >= 0 &&
            (i->op == IR_ADD || i->op == IR_MUL || i->op == IR_AND ||
             i->op == IR_OR  || i->op == IR_XOR) &&
            ((ka && !kb) || (ka == kb && i->a > i->b))) {
            int t = i->a; i->a = i->b; i->b = t;
            t = ka; ka = kb; kb = t;
            long v = A; A = B; B = v;
            changed = 1;
        }
        /* one-operand algebraic identities (valid for any width/signedness) */
        switch (i->op) {
        case IR_ADD:
            if (ka && A == 0) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_SUB:
            if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_OR:
            if (ka && A == 0) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_XOR:
            if (ka && A == 0) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_MUL: {
            int sh;
            if ((ka && A == 0) || (kb && B == 0)) { to_const(i, 0); changed = 1; }
            else if (ka && A == 1) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 1) { to_mov(i, i->a); changed = 1; }
            /* x * 2^k -> x << k (retarget the literal to k). Handle either
             * operand being the constant, since MUL is commutative. */
            else if (kb && (sh = log2_pow2(B)) >= 0 &&
                     retarget_const(fn, &d, use, i->b, sh)) {
                i->op = IR_SHL; changed = 1;   /* x << k, count already in b */
            } else if (ka && (sh = log2_pow2(A)) >= 0 &&
                       retarget_const(fn, &d, use, i->a, sh)) {
                /* A_const * x -> x << k: put x in a, the retargeted count in b */
                int c = i->a; i->a = i->b; i->b = c;
                i->op = IR_SHL; changed = 1;
            }
            break;
        }
        case IR_DIV:
            /* unsigned x / 2^k -> x >> k (logical) */
            if (kb && !i->sign) {
                int sh = log2_pow2(B);
                if (sh >= 0 && retarget_const(fn, &d, use, i->b, sh)) {
                    i->op = IR_SHR; changed = 1;
                } else if (B == 1) { to_mov(i, i->a); changed = 1; }
            }
            break;
        case IR_MOD:
            /* unsigned x % 2^k -> x & (2^k - 1) */
            if (kb && !i->sign && log2_pow2(B) >= 0 &&
                retarget_const(fn, &d, use, i->b, B - 1)) {
                i->op = IR_AND; changed = 1;
            }
            break;
        case IR_AND:
            if ((ka && A == 0) || (kb && B == 0)) { to_const(i, 0); changed = 1; }
            break;
        case IR_SHL:
        case IR_SHR:
            if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        default:
            break;
        }
    }
    free(lk.gen_of); free(lk.val); free(lk.w);
    free(use);
    free_defs(&d);
    if (L.n)
        ld_insert(fn, &L);
    free(L.val); free(L.kval); free(L.kgen); free(L.vval); free(L.vgen);
    free(L.vok); free(L.at); free(L.p); free(L.lab);
    return changed;
}

/* Insert the straddrs ld_become asked for, each before its instruction. */
static void ld_insert(struct ir_func *fn, struct ldfold *L)
{
    struct ibuf nb = { 0, 0, 0 };
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    int k = 0;
    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = nb.n;
        while (k < L->n && L->at[k] == n) {
            struct ir_ins *e = ib_push(&nb);
            memset(e, 0, sizeof *e);
            e->op = IR_STRADDR;
            e->dst = L->p[k]; e->label = L->lab[k];
            e->a = e->b = e->c = -1;
            e->w = 4; e->size = 4; e->sign = 1; e->pred = B_ADD;
            e->callee_sym = e->glob_sym = -1;
            e->line = fn->ins[n].line; e->col = fn->ins[n].col; e->synth = 1;
            k++;
        }
        *ib_push(&nb) = fn->ins[n];
    }
    if (newpos) {
        newpos[fn->nins] = nb.n;
        for (int v = 0; v < fn->nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
            if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
        }
        free(newpos);
    }
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;
}
