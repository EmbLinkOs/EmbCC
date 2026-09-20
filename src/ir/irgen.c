#include "ir.h"
#include "irgen_int.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../arch/aarch64/asm.h"
#include "../driver/util.h"
#include "../sema/ldfloat.h"
#include "../sema/sema.h"
#include "../sema/type.h"
#include "../arch/target.h"

/* The source line currently being lowered. gen_stmt updates it as it walks
 * the statement list, and gen_func resets it per function; emit() stamps it
 * onto every instruction so the -g line table in codegen can map .text
 * offsets back to source lines. irgen runs one function at a time, single
 * threaded, so a file-scope cursor is sound. Off (-g absent) it is simply
 * ignored — nothing reads ir_ins.line. */
static int gen_expr_inner(struct ir_func *fn, struct expr *e);
static int g_cur_line;
/* The column within that line, from the expression being lowered (R3). A
 * statement-granular location is enough for a line table; a diagnostic or a
 * remark about one operand inside a long expression is not. */
static int g_cur_col;
/* Set while lowering something the compiler invented rather than something
 * the programmer wrote: the prologue, a landing pad, a temporary's cleanup.
 * The verifier allows these to carry no location. */
static int g_synth;

struct ir_ins *emit(struct ir_func *fn)
{
    if (fn->nins == fn->cap) {
        fn->cap = fn->cap ? fn->cap * 2 : 16;
        fn->ins = xrealloc(fn->ins, (size_t)fn->cap * sizeof *fn->ins);
    }
    struct ir_ins *i = &fn->ins[fn->nins++];
    /* Zero first: the array is grown with xrealloc, so a reused slot
     * carries a previous instruction's bytes. Fields an instruction does
     * not set (retsize/retnclass on a non-struct call, the whole va/arg
     * machinery on a plain op) must read as 0, not stale garbage — a
     * garbage retnclass once walked retcls[] off the end and crashed. */
    memset(i, 0, sizeof *i);
    i->op = IR_CONST;
    i->line = g_cur_line;
    i->col = g_cur_col;
    i->synth = g_synth;
    i->dst = i->a = i->b = -1;
    i->w = 4;
    i->size = 4;
    i->sign = 1;
    i->pred = B_ADD;
    i->label = -1;
    return i;
}

int new_temp(struct ir_func *fn) { return fn->nvregs++; }
int new_label(struct ir_func *fn) { return fn->nlabels++; }

/* A value's width class: 4 or 8, and 16 for a long double — the only value
 * wider than a register. Its temps get 16-byte slots, and every op that
 * produces one says w = 16 (codegen sizes the slot from that). */
static int ty_w(const struct type *t)
{
    if (t->kind == TY_LDOUBLE || t->kind == TY_INT128) return 16;
    return ty_wide(t) ? 8 : 4;
}

void emit_label(struct ir_func *fn, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_LABEL;
    i->label = label;
}

void emit_jmp(struct ir_func *fn, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_JMP;
    i->label = label;
}

/* goto/label resolution. Labels are function-scoped and forward-referable, so a
 * name gets its IR label the first time EITHER a `goto` or the label itself is
 * seen. gen_func resets this table; after the body it errors on any label that
 * was referenced by a goto but never defined. */
#define IRGEN_MAX_LABELS 256
static struct { const char *name; int label; int defined; int line; }
    g_labels[IRGEN_MAX_LABELS];
static int g_nlabels_used;

static int label_idx(struct ir_func *fn, const char *name, int line)
{
    for (int i = 0; i < g_nlabels_used; i++)
        if (strcmp(g_labels[i].name, name) == 0)
            return i;
    if (g_nlabels_used >= IRGEN_MAX_LABELS)
        diag_fatal(fn->src->file, line, "too many labels in one function");
    g_labels[g_nlabels_used].name = name;
    g_labels[g_nlabels_used].label = new_label(fn);
    g_labels[g_nlabels_used].defined = 0;
    g_labels[g_nlabels_used].line = line;
    return g_nlabels_used++;
}

void emit_brz(struct ir_func *fn, int v, int w, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_BRZ;
    i->a = v;
    i->w = w;
    i->label = label;
}

void emit_brnz(struct ir_func *fn, int v, int w, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_BRNZ;
    i->a = v;
    i->w = w;
    i->label = label;
}

/* A floating constant is just its BIT PATTERN moved into the slot: a
 * float temp's slot holds raw bits, so no xmm and no constant pool are
 * involved. Same reason loads and stores need no float path. */
static int emit_ldconst(struct ir_func *fn, const struct ldf *v);

static int emit_fconst(struct ir_func *fn, double d, int w)
{
    if (w == 16)                 /* long double: exact from the double */
        return emit_ldconst(fn, ldf_from_double(d));
    long bits = 0;
    if (w == 8) {
        double v = d;
        memcpy(&bits, &v, 8);
    } else {
        float v = (float)d;
        unsigned int u = 0;
        memcpy(&u, &v, 4);
        bits = (long)u;
    }
    struct ir_ins *i = emit(fn);
    i->op = IR_CONST;
    i->imm = bits;
    i->w = w;
    i->dst = new_temp(fn);
    return i->dst;
}

int emit_const(struct ir_func *fn, long imm, int w)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_CONST;
    i->imm = imm;
    i->w = w;
    i->dst = new_temp(fn);
    return i->dst;
}

/* dst = a op b at width w; returns dst */
int emit_bin(struct ir_func *fn, enum ir_op op, int a, int b,
                    int w, int sign)
{
    struct ir_ins *i = emit(fn);
    i->op = op;
    i->a = a;
    i->b = b;
    i->w = w;
    i->sign = sign;
    i->dst = new_temp(fn);
    return i->dst;
}

static int emit_fbin(struct ir_func *fn, enum ir_op op, int a, int b, int w)
{
    struct ir_ins *i = emit(fn);
    i->op = op;
    i->a = a;
    i->b = b;
    i->w = w;
    i->flt = 1;
    i->dst = new_temp(fn);
    return i->dst;
}

/* Load variable v (type t) into a fresh promoted temp. */
static int emit_ldvar(struct ir_func *fn, int v, const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_LDVAR;
    i->a = v;
    i->size = ty_size(t);
    i->sign = ty_signed_int(t);
    i->w = ty_w(t);
    i->vol = t->is_volatile;
    i->dst = new_temp(fn);
    return i->dst;
}

static void emit_stvar(struct ir_func *fn, int v, int val,
                       const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_STVAR;
    i->dst = v;
    i->a = val;
    i->size = ty_size(t);
    i->vol = t->is_volatile;
}

static int emit_gaddr(struct ir_func *fn, struct global *g)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_GADDR;
    i->glob = g;
    i->dst = new_temp(fn);
    return i->dst;
}

/* Typed load/store through an address temp. */
int emit_load(struct ir_func *fn, int addr, const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_LOAD;
    i->a = addr;
    i->size = ty_size(t);
    i->sign = ty_signed_int(t);
    i->w = ty_w(t);
    i->vol = t->is_volatile;
    i->dst = new_temp(fn);
    return i->dst;
}

void emit_store(struct ir_func *fn, int addr, int val,
                       const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_STORE;
    i->a = addr;
    i->b = val;
    i->size = ty_size(t);
    i->vol = t->is_volatile;
}

void emit_mov(struct ir_func *fn, int dst, int src)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_MOV;
    i->dst = dst;
    i->a = src;
}

/* ---- bitfield access (little-endian, gcc-compatible) ----
 * A bitfield occupies bits [bit_off, bit_off+width) of the storage unit at
 * `addr` (a load/store of the field's declared type). Reading shifts the
 * field to the top of the value class then back down — arithmetic for a
 * signed field so its sign bit fills — the classic two-shift extraction,
 * immune to neighbouring fields packed into the same unit. */
/* a 32-bit value class zero-extended to 64 */
static int zext64(struct ir_func *fn, int v)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_EXT;
    i->a = v;
    i->size = 4;
    i->sign = 0;
    i->w = 8;
    i->dst = new_temp(fn);
    return i->dst;
}

/* A packed struct's field across its unit (m->bf_bytes): its bytes read
 * one at a time, as a 64-bit value with byte k at bits 8k. */
static int bf_bytes_load(struct ir_func *fn, int addr, const struct member *m)
{
    struct type *u8 = ty_base(TY_CHAR, 1);
    int raw = emit_const(fn, 0, 8);
    for (int k = 0; k < m->bf_bytes && k < 8; k++) {
        int a = k ? emit_bin(fn, IR_ADD, addr, emit_const(fn, k, 8), 8, 0)
                  : addr;
        int b = zext64(fn, emit_load(fn, a, u8));
        if (k)
            b = emit_bin(fn, IR_SHL, b, emit_const(fn, 8 * k, 4), 8, 0);
        raw = emit_bin(fn, IR_OR, raw, b, 8, 0);
    }
    return raw;
}

/* A packed field across its unit that 8 bytes do not hold (a long long's
 * at bit 1..7 of its first byte takes 9, an __int128's 16 or 17): worked
 * in 128 bits — bytes [from, n) as one value, byte k at bit 8(k - from).
 * A 17th byte, past what 128 bits hold, is read and merged apart. */
static int bf_wide_bytes(struct ir_func *fn, int addr, int from, int n)
{
    struct type *u8 = ty_base(TY_CHAR, 1), *u128 = ty_base(TY_INT128, 1);
    int raw = emit_const(fn, 0, 16);
    for (int k = from; k < n; k++) {
        int a = k ? emit_bin(fn, IR_ADD, addr, emit_const(fn, k, 8), 8, 0)
                  : addr;
        int b = gen_convert(fn, emit_load(fn, a, u8), u8, u128);
        if (k > from)
            b = emit_bin(fn, IR_SHL, b, emit_const(fn, 8 * (k - from), 4), 16,
                         0);
        raw = emit_bin(fn, IR_OR, raw, b, 16, 0);
    }
    return raw;
}

static int bf_wide_load(struct ir_func *fn, int addr, const struct member *m)
{
    const struct type *bt = m->ty;
    int n = m->bf_bytes, off = m->bit_off, wd = m->bit_width;
    int v = bf_wide_bytes(fn, addr, 0, n < 16 ? n : 16);
    if (off)
        v = emit_bin(fn, IR_SHR, v, emit_const(fn, off, 4), 16, 0);
    if (n == 17) {                        /* (then off >= 1) */
        int hi = bf_wide_bytes(fn, addr, 16, 17);
        v = emit_bin(fn, IR_OR, v, emit_bin(fn, IR_SHL, hi,
                                           emit_const(fn, 128 - off, 4), 16, 0),
                     16, 0);
    }
    if (wd < 128) {
        v = emit_bin(fn, IR_SHL, v, emit_const(fn, 128 - wd, 4), 16, 0);
        v = emit_bin(fn, IR_SHR, v, emit_const(fn, 128 - wd, 4), 16,
                     ty_signed_int(bt));
    }
    if (bt->kind == TY_INT128)
        return v;
    return gen_convert(fn, v, ty_base(TY_INT128, !ty_signed_int(bt)), bt);
}

static int bf_wide_store(struct ir_func *fn, int addr, const struct member *m,
                         int val)
{
    const struct type *bt = m->ty;
    int n = m->bf_bytes, off = m->bit_off, wd = m->bit_width;
    struct type *u8 = ty_base(TY_CHAR, 1), *u128 = ty_base(TY_INT128, 1);
    int v = bt->kind == TY_INT128 ? val : gen_convert(fn, val, bt, u128);
    int ones = emit_const(fn, -1, 16);
    int fm = wd < 128 ? emit_bin(fn, IR_SHR, ones,
                                 emit_const(fn, 128 - wd, 4), 16, 0)
                      : ones;
    v = emit_bin(fn, IR_AND, v, fm, 16, 0);
    int lo = bf_wide_bytes(fn, addr, 0, n < 16 ? n : 16);
    int pl = off ? emit_bin(fn, IR_SHL, fm, emit_const(fn, off, 4), 16, 0)
                 : fm;
    int nv = off ? emit_bin(fn, IR_SHL, v, emit_const(fn, off, 4), 16, 0) : v;
    lo = emit_bin(fn, IR_OR, emit_bin(fn, IR_AND, lo,
                                      emit_bin(fn, IR_XOR, pl, ones, 16, 0),
                                      16, 0),
                  nv, 16, 0);
    for (int k = 0; k < n && k < 16; k++) {
        int a = k ? emit_bin(fn, IR_ADD, addr, emit_const(fn, k, 8), 8, 0)
                  : addr;
        int b = k ? emit_bin(fn, IR_SHR, lo, emit_const(fn, 8 * k, 4), 16, 0)
                  : lo;
        emit_store(fn, a, gen_convert(fn, b, u128, u8), u8);
    }
    if (n == 17) {
        int hi = bf_wide_bytes(fn, addr, 16, 17);
        int sh = emit_const(fn, 128 - off, 4);
        int hm = emit_bin(fn, IR_SHR, fm, sh, 16, 0);
        hi = emit_bin(fn, IR_OR, emit_bin(fn, IR_AND, hi,
                                          emit_bin(fn, IR_XOR, hm, ones, 16, 0),
                                          16, 0),
                      emit_bin(fn, IR_SHR, v, sh, 16, 0), 16, 0);
        emit_store(fn, emit_bin(fn, IR_ADD, addr, emit_const(fn, 16, 8), 8, 0),
                   gen_convert(fn, hi, u128, u8), u8);
    }
    return bf_wide_load(fn, addr, m);
}

static int bf_load(struct ir_func *fn, int addr, const struct member *m)
{
    const struct type *bt = m->ty;
    int w = bt->kind == TY_INT128 ? 16 : ty_wide(bt) ? 8 : 4; /* class, bytes */
    int vb = w * 8;
    if (m->bf_bytes > 8 || (m->bf_bytes && bt->kind == TY_INT128))
        return bf_wide_load(fn, addr, m);
    if (m->bf_bytes) {
        /* the bytes, then the same two shifts in 64 bits */
        int v = bf_bytes_load(fn, addr, m);
        int lsh = 64 - m->bit_off - m->bit_width;
        if (lsh)
            v = emit_bin(fn, IR_SHL, v, emit_const(fn, lsh, 4), 8, 0);
        v = emit_bin(fn, IR_SHR, v, emit_const(fn, 64 - m->bit_width, 4), 8,
                     ty_signed_int(bt));
        return v;                 /* (a 32-bit class reads the low half) */
    }
    /* load the raw storage unit UNSIGNED, so no stray sign extension */
    int v = emit_load(fn, addr, ty_base(bt->kind, 1));
    int lsh = vb - m->bit_off - m->bit_width;
    if (lsh)
        v = emit_bin(fn, IR_SHL, v, emit_const(fn, lsh, 4), w, 0);
    int rsh = vb - m->bit_width;
    if (rsh)
        v = emit_bin(fn, IR_SHR, v, emit_const(fn, rsh, 4), w,
                     ty_signed_int(bt));
    return v;
}

/* Store `val` into a bitfield: read the storage unit, clear the field's
 * bits, OR in the low `width` bits of the value, write it back. Returns the
 * field re-read, which is the assignment expression's (truncated) value. */
static int bf_store(struct ir_func *fn, int addr, const struct member *m,
                    int val)
{
    const struct type *bt = m->ty;
    int w = ty_wide(bt) ? 8 : 4;
    if (m->bf_bytes > 8 || (m->bf_bytes && bt->kind == TY_INT128))
        return bf_wide_store(fn, addr, m, val);
    if (m->bf_bytes) {
        /* merge in 64 bits, then write each byte back */
        unsigned long fmask = m->bit_width >= 64
                            ? ~0UL : (((unsigned long)1 << m->bit_width) - 1);
        unsigned long placed = fmask << m->bit_off;
        int old = bf_bytes_load(fn, addr, m);
        int v64 = w == 8 ? val : zext64(fn, val);
        int cleared = emit_bin(fn, IR_AND, old,
                               emit_const(fn, (long)~placed, 8), 8, 0);
        int low = emit_bin(fn, IR_AND, v64, emit_const(fn, (long)fmask, 8),
                           8, 0);
        if (m->bit_off)
            low = emit_bin(fn, IR_SHL, low, emit_const(fn, m->bit_off, 4), 8,
                           0);
        int merged = emit_bin(fn, IR_OR, cleared, low, 8, 0);
        struct type *u8 = ty_base(TY_CHAR, 1);
        for (int k = 0; k < m->bf_bytes && k < 8; k++) {
            int a = k ? emit_bin(fn, IR_ADD, addr, emit_const(fn, k, 8), 8, 0)
                      : addr;
            int b = k ? emit_bin(fn, IR_SHR, merged,
                                 emit_const(fn, 8 * k, 4), 8, 0)
                      : merged;
            emit_store(fn, a, b, u8);
        }
        return bf_load(fn, addr, m);
    }
    struct type *ut = ty_base(bt->kind, 1);
    if (bt->kind == TY_INT128) {
        /* the masks in 128 bits, from all ones: fmask = ~0 >> (128 - width) */
        int ones = emit_const(fn, -1, 16);
        int fm = m->bit_width < 128
                 ? emit_bin(fn, IR_SHR, ones,
                            emit_const(fn, 128 - m->bit_width, 4), 16, 0)
                 : ones;
        int pl = m->bit_off ? emit_bin(fn, IR_SHL, fm,
                                       emit_const(fn, m->bit_off, 4), 16, 0)
                            : fm;
        int old = emit_load(fn, addr, ut);
        int cleared = emit_bin(fn, IR_AND, old,
                               emit_bin(fn, IR_XOR, pl, ones, 16, 0), 16, 0);
        int low = emit_bin(fn, IR_AND, val, fm, 16, 0);
        if (m->bit_off)
            low = emit_bin(fn, IR_SHL, low, emit_const(fn, m->bit_off, 4), 16,
                           0);
        emit_store(fn, addr, emit_bin(fn, IR_OR, cleared, low, 16, 0), ut);
        return bf_load(fn, addr, m);
    }
    unsigned long fmask = m->bit_width >= 64
                        ? ~0UL : (((unsigned long)1 << m->bit_width) - 1);
    unsigned long placed = fmask << m->bit_off;
    int old = emit_load(fn, addr, ut);
    int cleared = emit_bin(fn, IR_AND, old,
                           emit_const(fn, (long)~placed, w), w, 0);
    int low = emit_bin(fn, IR_AND, val,
                       emit_const(fn, (long)fmask, w), w, 0);
    if (m->bit_off)
        low = emit_bin(fn, IR_SHL, low, emit_const(fn, m->bit_off, 4), w, 0);
    int merged = emit_bin(fn, IR_OR, cleared, low, w, 0);
    emit_store(fn, addr, merged, ut);
    return bf_load(fn, addr, m);
}

/* Place one flattened initializer leaf `ie` (value already in `v`) at address
 * `at`: a bitfield merges into its storage unit, a struct is a byte copy, any
 * other scalar a plain truncating store. Shared by declaration and compound-
 * literal initialization. */
static void store_init_leaf(struct ir_func *fn, int at,
                            const struct initelem *ie, int v)
{
    if (ie->bit_width) {
        struct member m;
        memset(&m, 0, sizeof m);
        m.name = NULL; m.ty = ie->ty; m.off = 0;
        m.is_bitfield = 1; m.bit_off = ie->bit_off; m.bit_width = ie->bit_width;
        m.bf_bytes = ie->bf_bytes;
        bf_store(fn, at, &m, v);
    } else if (ie->ty->kind == TY_STRUCT) {
        struct ir_ins *mm = emit(fn);
        mm->op = IR_MEMCPY;
        mm->a = at;
        mm->b = v;
        mm->size = ty_size(ie->ty);
    } else {
        emit_store(fn, at, v, ie->ty);
    }
}

static int expr_is_bitfield(const struct expr *e)
{
    return e->kind == EXPR_MEMBER && e->memb && e->memb->is_bitfield;
}

int emit_cmp(struct ir_func *fn, enum binop pred, int a, int b,
                    int w, int sign)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_CMP;
    i->pred = pred;
    i->a = a;
    i->b = b;
    i->w = w;
    i->sign = sign;
    i->dst = new_temp(fn);
    return i->dst;
}

int gen_expr(struct ir_func *fn, struct expr *e);
static int gen_complit(struct ir_func *fn, struct expr *e);
int gen_convert(struct ir_func *fn, int v, const struct type *from,
                       const struct type *to);
struct loopctx;
static void gen_stmt(struct ir_func *fn, struct stmt *s,
                     const struct loopctx *loop);
static int gen_stmtexpr(struct ir_func *fn, struct expr *e);
int gen_expr(struct ir_func *fn, struct expr *e);

/* ---- variable length arrays ---- */

/* sizeof t as a value: a VLA's from its size slot, anything else a constant */
static int type_size_val(struct ir_func *fn, const struct type *t)
{
    if (ty_is_vla(t))
        return emit_ldvar(fn, t->vla_size, ty_base(TY_LONG, 1));
    return emit_const(fn, ty_size(t), 8);
}

/* Compute the byte size of every VLA in a variably modified type, innermost
 * first, into its slot — where the declaration (or type name) is reached, so
 * a declaration in a loop re-reads its lengths each time round. */
static void vla_eval(struct ir_func *fn, struct type *t)
{
    if (!t || (t->kind != TY_PTR && t->kind != TY_ARRAY))
        return;
    vla_eval(fn, t->pointee);
    if (!ty_is_vla(t))
        return;
    int n = gen_expr(fn, t->vla_len);
    int sz = emit_bin(fn, IR_MUL, n, type_size_val(fn, t->pointee), 8, 1);
    emit_stvar(fn, t->vla_size, sz, ty_base(TY_LONG, 1));
}

/* The VLA declarations whose scope encloses the statement being generated,
 * outermost first: the slot the stack pointer was saved in just before each
 * one's allocation, and the statements after it (its scope, for goto). */
struct vla_scope { int sp_slot; struct stmt *decl; };
static struct vla_scope g_vla[64];
static int g_nvla;

/* Leaving the scopes [depth, g_nvla): restore the stack pointer saved before
 * the outermost of them, which releases all of them. Emits nothing when
 * none is open. The textual scopes stay open — the caller pops them. */
static void vla_release(struct ir_func *fn, int depth)
{
    if (g_nvla <= depth)
        return;
    int sp = emit_ldvar(fn, g_vla[depth].sp_slot, ty_base(TY_LONG, 1));
    struct ir_ins *i = emit(fn);   /* operands first: emit() appends */
    i->op = IR_SPRESTORE;
    i->a = sp;
}

/* Is label `name` defined anywhere in statement list s (nested included)? */
static int stmts_define_label(const struct stmt *s, const char *name)
{
    for (; s; s = s->next) {
        if (s->kind == STMT_LABEL && strcmp(s->name, name) == 0)
            return 1;
        if (stmts_define_label(s->body, name) ||
            stmts_define_label(s->thn, name) ||
            stmts_define_label(s->els, name))
            return 1;
    }
    return 0;
}

/* The address of an lvalue (or of a struct-typed expression — struct
 * "values" are represented by their address, since sema bars them from
 * every value context). */
int gen_addr(struct ir_func *fn, struct expr *e)
{
    switch (e->kind) {
    case EXPR_VAR:
        if (e->gref)
            return emit_gaddr(fn, e->gref);
        if (ty_is_vla(e->undecayed ? e->undecayed : e->ty))
            /* a VLA's slot holds the address of its storage */
            return emit_ldvar(fn, e->var_index,
                              ty_ptr(e->undecayed ? e->undecayed->pointee
                                                  : e->ty->pointee));
        {
            struct ir_ins *i = emit(fn);
            i->op = IR_ADDR;
            i->a = e->var_index;
            i->dst = new_temp(fn);
            return i->dst;
        }
    case EXPR_DEREF:
        return gen_expr(fn, e->rhs);
    case EXPR_MEMBER: {
        int base = e->is_arrow ? gen_expr(fn, e->lhs)
                               : gen_addr(fn, e->lhs);
        if (e->memb->off == 0)
            return base;
        int off = emit_const(fn, e->memb->off, 8);
        return emit_bin(fn, IR_ADD, base, off, 8, 1);
    }
    case EXPR_COMPLIT:
        return gen_complit(fn, e);
    default:
        /* a struct value that is no lvalue (a call's result, `?:`, a
         * comma or an assignment): gen_expr's value is its address —
         * `f().m` reads a member of it */
        if (e->ty && e->ty->kind == TY_STRUCT)
            return gen_expr(fn, e);
        internal_error("address of a non-lvalue");
    }
}

int gen_expr(struct ir_func *fn, struct expr *e);

/* A compound literal `(type){ init }`: clear its synthesized slot, place the
 * flattened initializer leaves (zero-fill + last-write-wins, like a declared
 * aggregate), and return the object's address. */
static int gen_complit(struct ir_func *fn, struct expr *e)
{
    struct ir_ins *ad = emit(fn);
    ad->op = IR_ADDR;
    ad->a = e->var_index;
    ad->dst = new_temp(fn);
    int base = ad->dst;
    struct ir_ins *z = emit(fn);
    z->op = IR_MEMZERO;
    z->a = base;
    /* e->ty is the decayed pointer for an array literal; the OBJECT's size
     * is the undecayed array (or the type itself for struct/scalar). */
    z->size = ty_size(e->undecayed ? e->undecayed : e->ty);
    for (int k = 0; k < e->ninits; k++) {
        int v = gen_expr(fn, e->inits[k].e);
        int at = base;
        if (e->inits[k].off) {
            int o = emit_const(fn, e->inits[k].off, 8);
            at = emit_bin(fn, IR_ADD, base, o, 8, 1);
        }
        store_init_leaf(fn, at, &e->inits[k], v);
    }
    return base;
}

/* The unit being generated — for the string table. One compilation per
 * process, so a file-scope current-unit pointer is honest. */
static struct ir_unit *cur_unit;

/* Intern a string into the unit's .rodata pool, returning its index.
 * Deduping means a literal shared by code and a global initializer lands
 * once. */
int ir_intern_string(struct ir_unit *iu, const char *bytes, int len)
{
    for (int i = 0; i < iu->nstrs; i++)
        if (iu->strs[i].len == len &&
            memcmp(iu->strs[i].bytes, bytes, (size_t)len) == 0)
            return i;
    if (iu->nstrs == iu->capstrs) {
        iu->capstrs = iu->capstrs ? iu->capstrs * 2 : 8;
        iu->strs = xrealloc(iu->strs,
                            (size_t)iu->capstrs * sizeof *iu->strs);
    }
    struct ir_str *s = &iu->strs[iu->nstrs];
    s->bytes = bytes;
    s->len = len;
    s->off = iu->rodata_len;
    iu->rodata_len += len;
    return iu->nstrs++;
}

static int intern_str(const char *bytes, int len)
{
    return ir_intern_string(cur_unit, bytes, len);
}

/* A long double constant: 16 bytes in the target's format (x87 extended or
 * binary128), too wide for IR_CONST's 64-bit immediate — so it lives in
 * .rodata like a string and is loaded from there. */
static int emit_ldconst(struct ir_func *fn, const struct ldf *v)
{
    char *b = xmalloc(16);
    ldf_encode(v, ldf_target_fmt(), (unsigned char *)b);
    struct ir_ins *i = emit(fn);
    i->op = IR_STRADDR;
    i->label = intern_str(b, 16);
    i->dst = new_temp(fn);
    return emit_load(fn, i->dst, ty_base(TY_LDOUBLE, 0));
}

/* !x and conditions want "is zero" — comparison against a zero of the
 * operand's width. */
static int emit_isz(struct ir_func *fn, int v, int w)
{
    int zero = emit_const(fn, 0, w);
    struct ir_ins *i = emit(fn);
    i->op = IR_CMP;
    i->pred = B_EQ;
    i->a = v;
    i->b = zero;
    i->w = w;
    i->sign = 1;
    i->dst = new_temp(fn);
    return i->dst;
}

/* Convert any scalar to _Bool: the result is (v != 0), a 0/1 int. C says a
 * store to _Bool normalizes this way, and a float 0.5 must become 1 (so it
 * compares the float directly, not a truncation). */
static int emit_tobool(struct ir_func *fn, int v, const struct type *from)
{
    int flt = ty_is_float(from);
    /* the zero operand MUST be emitted before the compare that reads it —
     * the IR is lowered in order, so an operand emitted after would be
     * materialized after the cmp already ran (a real bug, once). */
    int zero = flt ? emit_fconst(fn, 0.0, ty_size(from))
                   : emit_const(fn, 0, ty_w(from));
    struct ir_ins *i = emit(fn);
    i->op = IR_CMP;
    i->pred = B_NE;
    i->a = v;
    i->b = zero;
    i->sign = 0;
    i->flt = flt;
    i->w = flt ? ty_size(from) : ty_w(from);
    i->dst = new_temp(fn);
    return i->dst;
}

/* A value about to be TESTED (a condition, !, && or ||): a floating one is
 * compared with 0.0 as a float — -0.0 is false, which its bit pattern is
 * not, and a long double is too wide to test as bits at all. Returns the
 * temp to branch on, and its width in *w. */
static int truth(struct ir_func *fn, int v, const struct type *t, int *w)
{
    if (ty_is_float(t) || t->kind == TY_INT128) {   /* (no 16-byte branch) */
        *w = 4;
        return emit_tobool(fn, v, t);
    }
    *w = ty_w(t);
    return v;
}

/* An I2F/F2I instruction, spelled out because unsigned-64 conversions build
 * several by hand. `a` is the source vreg. */
static int emit_i2f(struct ir_func *fn, int a, int srcw, int dstw)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_I2F; i->a = a; i->size = srcw; i->sign = 1; i->w = dstw;
    i->dst = new_temp(fn);
    return i->dst;
}
static int emit_f2i(struct ir_func *fn, int a, int srcw, int dstw)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_F2I; i->a = a; i->size = srcw; i->sign = 1; i->w = dstw;
    i->dst = new_temp(fn);
    return i->dst;
}

/* unsigned-64 -> floating. SSE2's cvtsi2sd is SIGNED, so a u64 with its top bit
 * set would convert as a huge negative. Split into two 32-bit halves -- each is
 * positive and < 2^32, so cvtsi2sd is exact -- then hi*2^32 + lo. Both partials
 * are exact doubles, so the single add rounds the true u64 once (correctly
 * rounded). For a float target do it in double first (exact) then narrow, which
 * avoids a double rounding. */
static int gen_u64_to_float(struct ir_func *fn, int v, int tsize)
{
    /* In long double the halves and the sum are all exact (64-bit or wider
     * significand), so compute there directly. */
    int cw = tsize == 16 ? 16 : 8;
    int hi = emit_bin(fn, IR_SHR, v, emit_const(fn, 32, 4), 8, 0);      /* v >> 32 */
    int lo = emit_bin(fn, IR_AND, v, emit_const(fn, 0xffffffffL, 8), 8, 1);
    int hd = emit_i2f(fn, hi, 8, cw);
    int ld = emit_i2f(fn, lo, 8, cw);
    int hs = emit_fbin(fn, IR_MUL, hd, emit_fconst(fn, 4294967296.0, cw), cw);
    int res = emit_fbin(fn, IR_ADD, hs, ld, cw);
    if (tsize == 4) {   /* narrow the exact double to float: one rounding */
        struct ir_ins *nf = emit(fn);
        nf->op = IR_F2F; nf->a = res; nf->size = 8; nf->w = 4;
        nf->dst = new_temp(fn);
        return nf->dst;
    }
    return res;
}

/* floating -> unsigned-64. cvttsd2si is SIGNED: exact for v < 2^63, but v in
 * [2^63, 2^64) overflows it. For those, convert (v - 2^63) and set the top bit
 * back. Branch on v >= 2^63. */
static int gen_float_to_u64(struct ir_func *fn, int v, int fsize)
{
    int two63 = emit_fconst(fn, 9223372036854775808.0, fsize);   /* 2^63 */
    struct ir_ins *cmp = emit(fn);                               /* c = v >= 2^63 */
    cmp->op = IR_CMP; cmp->pred = B_GE; cmp->a = v; cmp->b = two63;
    cmp->w = fsize; cmp->flt = 1; cmp->dst = new_temp(fn);
    int res = new_temp(fn);
    int l_small = new_label(fn), l_done = new_label(fn);
    emit_brz(fn, cmp->dst, 4, l_small);                          /* v < 2^63 -> direct */
    /* v >= 2^63: (u64)(v - 2^63) with the sign bit flipped back on */
    int vm = emit_fbin(fn, IR_SUB, v, two63, fsize);
    int big = emit_bin(fn, IR_XOR, emit_f2i(fn, vm, fsize, 8),
                       emit_const(fn, (long)1 << 63, 8), 8, 0);
    emit_mov(fn, res, big);
    emit_jmp(fn, l_done);
    emit_label(fn, l_small);
    emit_mov(fn, res, emit_f2i(fn, v, fsize, 8));
    emit_label(fn, l_done);
    return res;
}

/* Change a temp's representation between type classes: truncating to a
 * narrow type re-extends from its low bytes; widening extends per the
 * SOURCE's signedness. Free conversions return the same temp. */
int gen_convert(struct ir_func *fn, int v, const struct type *from,
                       const struct type *to)
{
    int fsize = ty_size(from), tsize = ty_size(to);
    int fw = ty_w(from), tw = ty_w(to);

    /* To _Bool is a normalize-to-0/1, not a truncation. */
    if (to->kind == TY_BOOL && from->kind != TY_BOOL)
        return emit_tobool(fn, v, from);

    /* __int128 (w 16): to or from it, an IR_EXT at width 16 (extend) or
     * from a 16-byte value (truncate), or an I2F/F2I of size/width 16 */
    if (from->kind == TY_INT128 || to->kind == TY_INT128) {
        struct ir_ins *i;
        if (from->kind == TY_INT128 && to->kind == TY_INT128)
            return v;
        if (ty_is_float(to) || ty_is_float(from)) {
            i = emit(fn);
            i->op = ty_is_float(to) ? IR_I2F : IR_F2I;
            i->a = v;
            i->size = ty_is_float(to) ? 16 : fsize;
            i->w = ty_is_float(to) ? tsize : 16;
            i->sign = ty_is_float(to) ? !from->is_unsigned : !to->is_unsigned;
            i->dst = new_temp(fn);
            return i->dst;
        }
        i = emit(fn);
        i->op = IR_EXT;
        i->a = v;
        if (to->kind == TY_INT128) {
            /* extend: the source as its class holds it */
            i->size = fw == 8 ? 8 : 4;
            i->sign = fsize <= 2 ? 1 : ty_signed_int(from);
            i->w = 16;
        } else {
            /* truncate: the low bytes, re-extended as the target */
            i->size = tsize;
            i->sign = ty_signed_int(to);
            i->w = tw;
        }
        i->dst = new_temp(fn);
        return i->dst;
    }

    /* Floating conversions are real instructions, not reinterpretations
     * — the bit patterns have nothing in common. */
    if (ty_is_float(from) || ty_is_float(to)) {
        struct ir_ins *i;
        if (ty_is_float(from) && ty_is_float(to)) {
            if (fsize == tsize)
                return v;
            i = emit(fn);
            i->op = IR_F2F;
            i->a = v;
            i->size = fsize;
            i->w = tsize;
            i->dst = new_temp(fn);
            return i->dst;
        }
        if (ty_is_float(to)) {
            /* unsigned 64-bit -> float needs the split-and-add fixup; every
             * other integer source goes straight through signed cvtsi2sd. */
            if (from->is_unsigned && ty_size(from) == 8)
                return gen_u64_to_float(fn, v, tsize);
            /* int -> float, and the source WIDTH matters: a 32-bit
             * operation zero-extends its result into the 8-byte slot
             * regardless of signedness, so a negative int read back as
             * 64 bits is 2^32 too large. Read a signed 32-bit source as
             * 32 bits and let cvtsi2sd interpret the sign; read an
             * unsigned int as 64, where the zero extension IS the value
             * (which is what makes it exact). */
            int srcw = 4;
            if (ty_wide(from) ||
                (from->is_unsigned && ty_size(from) == 4))
                srcw = 8;
            i = emit(fn);
            i->op = IR_I2F;
            i->a = v;
            i->size = srcw;
            i->sign = 1;
            i->w = tsize;
            i->dst = new_temp(fn);
            return i->dst;
        }
        /* float -> unsigned 64-bit needs the 2^63 bias fixup (cvttsd2si is
         * signed); other targets use the signed convert-then-narrow below. */
        if (to->is_unsigned && ty_size(to) == 8)
            return gen_float_to_u64(fn, v, fsize);
        /* float -> int: truncates toward zero, as C requires. Convert
         * to the 64-bit form then narrow, so unsigned int lands right. */
        i = emit(fn);
        i->op = IR_F2I;
        i->a = v;
        i->size = fsize;
        i->w = 8;
        i->sign = 1;
        i->dst = new_temp(fn);
        int iv = i->dst;
        if (tsize <= 2) {
            struct ir_ins *x = emit(fn);
            x->op = IR_EXT;
            x->a = iv;
            x->size = tsize;
            x->sign = ty_signed_int(to);
            x->w = 4;
            x->dst = new_temp(fn);
            return x->dst;
        }
        return iv;
    }

    if (tsize <= 2) {
        /* to char/short: truncate + extend per TARGET's signedness */
        struct ir_ins *i = emit(fn);
        i->op = IR_EXT;
        i->a = v;
        i->size = tsize;
        i->sign = ty_signed_int(to);
        i->w = 4;
        i->dst = new_temp(fn);
        return i->dst;
    }
    if (tw == 8 && fw == 4) {
        /* int class -> long/pointer: extend per SOURCE signedness;
         * narrow sources were already promoted, so extend from 32. */
        struct ir_ins *i = emit(fn);
        i->op = IR_EXT;
        i->a = v;
        i->size = 4;
        i->sign = fsize <= 2 ? 1 : ty_signed_int(from);
        i->w = 8;
        i->dst = new_temp(fn);
        return i->dst;
    }
    /* long->int (read low 32), ptr<->long, same class: free */
    return v;
}

/* ---- GCC atomic builtins -------------------------------------------------
 *
 * One lowering for both targets, with the ordering differences made
 * explicit. Every builtin is treated as seq_cst whatever its memory-order
 * argument says — always correct, since a stronger order is always allowed —
 * and the order arguments are not evaluated (they are constants in practice).
 *
 * x86-64 is TSO: an aligned load or store is already atomic and acquire /
 * release, a locked read-modify-write is a full barrier, and only a seq_cst
 * store needs a trailing mfence. aarch64 orders nothing by default, so it
 * uses the fence-based mapping: a barrier after an atomic load (acquire),
 * before and after an atomic store (release, then seq_cst), and codegen
 * brackets each exclusive-access loop with barriers of its own.
 */

static int atomic_arm(void) { return target_get() == TARGET_AARCH64; }

/* The machine exchange leaves a narrow result zero-extended; re-extend it as
 * its type says — a signed char object holding -1 must read back as -1 — and
 * for an OP_fetch result also truncate it to the object's width. */
static int atomic_result(struct ir_func *fn, int v, const struct type *t)
{
    if (ty_size(t) >= 4)
        return v;
    struct ir_ins *x = emit(fn);
    x->op = IR_EXT;
    x->a = v;
    x->size = ty_size(t);
    x->sign = ty_signed_int(t);
    x->w = ty_w(t);
    x->dst = new_temp(fn);
    return x->dst;
}

/* An atomic access is never merged with another or removed: vol says so to
 * the optimizer (and changes nothing codegen emits). */
static int atomic_load(struct ir_func *fn, int addr, const struct type *t)
{
    int v = emit_load(fn, addr, t);
    fn->ins[fn->nins - 1].vol = 1;
    if (atomic_arm())
        emit(fn)->op = IR_FENCE;          /* acquire */
    return v;
}

static void atomic_store(struct ir_func *fn, int addr, int val,
                         const struct type *t)
{
    if (atomic_arm())
        emit(fn)->op = IR_FENCE;          /* release */
    emit_store(fn, addr, val, t);
    fn->ins[fn->nins - 1].vol = 1;
    emit(fn)->op = IR_FENCE;              /* seq_cst: published before what follows */
}

static int atomic_rmw(struct ir_func *fn, enum ir_op op, int opc, int addr,
                      int val, const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = op;
    i->a = addr;
    i->b = val;
    i->imm = opc;
    i->size = ty_size(t);
    i->w = ty_w(t);
    i->dst = new_temp(fn);
    return i->dst;
}

/* The value argument, converted to the atomic object's type first — so
 * __atomic_fetch_add(&some_long, -1, ...) adds -1, not 4294967295. */
static int atomic_value(struct ir_func *fn, struct expr *arg,
                        const struct type *t)
{
    return gen_convert(fn, gen_expr(fn, arg), arg->ty, t);
}

/* dst = the value *addr held, *addr = des if it was exp (IR_CAS16) */
static int cas16(struct ir_func *fn, int addr, int exp, int des)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_CAS16;
    i->a = addr;
    i->b = exp;
    i->c = des;
    i->size = 16;
    i->w = 16;
    i->dst = new_temp(fn);
    return i->dst;
}

/* *addr = f(old), atomically, by compare-and-swap until no one else wrote
 * in between: f is `op` of old and val ('=' for val itself). Returns the
 * old value; *nvp the one stored. */
static int cas16_loop(struct ir_func *fn, int addr, int op, int val,
                      const struct type *t, int *nvp)
{
    int cur = new_temp(fn);
    emit_mov(fn, cur, emit_load(fn, addr, t));   /* a guess: the CAS checks */
    int l_loop = new_label(fn), l_done = new_label(fn);
    emit_label(fn, l_loop);
    int nv;
    switch (op) {
    case '=': nv = val; break;
    case '+': nv = emit_bin(fn, IR_ADD, cur, val, 16, 1); break;
    case '-': nv = emit_bin(fn, IR_SUB, cur, val, 16, 1); break;
    case '&': nv = emit_bin(fn, IR_AND, cur, val, 16, 0); break;
    case '|': nv = emit_bin(fn, IR_OR, cur, val, 16, 0); break;
    case '^': nv = emit_bin(fn, IR_XOR, cur, val, 16, 0); break;
    default: {                                        /* nand */
        struct ir_ins *n = emit(fn);
        n->op = IR_BNOT;
        n->a = emit_bin(fn, IR_AND, cur, val, 16, 0);
        n->w = 16;
        n->dst = new_temp(fn);
        nv = n->dst;
        break;
    }
    }
    int seen = cas16(fn, addr, cur, nv);
    emit_brnz(fn, emit_cmp(fn, B_EQ, seen, cur, 16, 0), 4, l_done);
    emit_mov(fn, cur, seen);
    emit_jmp(fn, l_loop);
    emit_label(fn, l_done);
    if (nvp)
        *nvp = nv;
    return cur;
}

/* The atomics of an __int128: a load is a compare-and-swap of 0 with 0
 * (the value seen, whatever it is, and nothing changed); the rest loops */
static int gen_atomic16(struct ir_func *fn, struct expr *e,
                        enum atomic_kind ak, int op, const struct type *obj,
                        int addr)
{
    int zero = emit_const(fn, 0, 16);
    switch (ak) {
    case AK_LOAD_N:
        return cas16(fn, addr, zero, zero);
    case AK_LOAD:
        emit_store(fn, gen_expr(fn, e->args[1]), cas16(fn, addr, zero, zero),
                   obj);
        return -1;
    case AK_STORE_N:
        cas16_loop(fn, addr, '=', atomic_value(fn, e->args[1], obj), obj,
                   NULL);
        return -1;
    case AK_STORE:
        cas16_loop(fn, addr, '=', emit_load(fn, gen_expr(fn, e->args[1]), obj),
                   obj, NULL);
        return -1;
    case AK_SYNC_LOCK_RELEASE:
        cas16_loop(fn, addr, '=', zero, obj, NULL);
        return -1;
    case AK_EXCHANGE_N: case AK_SYNC_LOCK_TAS:
        return cas16_loop(fn, addr, '=', atomic_value(fn, e->args[1], obj),
                          obj, NULL);
    case AK_EXCHANGE: {
        int vp = gen_expr(fn, e->args[1]);
        int rp = gen_expr(fn, e->args[2]);
        emit_store(fn, rp, cas16_loop(fn, addr, '=', emit_load(fn, vp, obj),
                                      obj, NULL), obj);
        return -1;
    }
    case AK_FETCH_OP: case AK_OP_FETCH: {
        int nv;
        int old = cas16_loop(fn, addr, op, atomic_value(fn, e->args[1], obj),
                             obj, &nv);
        return ak == AK_FETCH_OP ? old : nv;
    }
    case AK_CMPXCHG_N: case AK_CMPXCHG: {
        int ep = gen_expr(fn, e->args[1]);           /* &expected */
        int des = ak == AK_CMPXCHG_N
            ? atomic_value(fn, e->args[2], obj)
            : emit_load(fn, gen_expr(fn, e->args[2]), obj);
        int exp = emit_load(fn, ep, obj);
        int seen = cas16(fn, addr, exp, des);
        int ok = emit_cmp(fn, B_EQ, seen, exp, 16, 0);
        int l_ok = new_label(fn);
        emit_brnz(fn, ok, 4, l_ok);
        emit_store(fn, ep, seen, obj);               /* *expected = seen */
        emit_label(fn, l_ok);
        return ok;
    }
    case AK_SYNC_VAL_CAS: case AK_SYNC_BOOL_CAS: {
        int expv = atomic_value(fn, e->args[1], obj);
        int old = cas16(fn, addr, expv, atomic_value(fn, e->args[2], obj));
        return ak == AK_SYNC_VAL_CAS ? old
                                     : emit_cmp(fn, B_EQ, old, expv, 16, 0);
    }
    default:
        diag_fatal(fn->src->file, e->line, "internal: atomic kind %d", (int)ak);
        return -1;
    }
}

static int gen_atomic(struct ir_func *fn, struct expr *e, enum atomic_kind ak,
                      int op)
{
    if (ak == AK_THREAD_FENCE || ak == AK_SIGNAL_FENCE) {
        /* A signal fence need only stop the compiler; a real barrier is
         * stronger and so also correct. */
        emit(fn)->op = IR_FENCE;
        return -1;
    }
    const struct type *obj = e->args[0]->ty->pointee;
    if (ak == AK_TEST_AND_SET || ak == AK_CLEAR)
        obj = ty_base(TY_CHAR, 1);                    /* one byte, as gcc does */
    int w = ty_w(obj), sign = ty_signed_int(obj);
    int addr = gen_expr(fn, e->args[0]);
    if (ty_size(obj) == 16)
        return gen_atomic16(fn, e, ak, op, obj, addr);

    switch (ak) {
    case AK_LOAD_N:
        return atomic_load(fn, addr, obj);
    case AK_STORE_N:
        atomic_store(fn, addr, atomic_value(fn, e->args[1], obj), obj);
        return -1;
    case AK_SYNC_LOCK_RELEASE: case AK_CLEAR:
        atomic_store(fn, addr, emit_const(fn, 0, w), obj);
        return -1;
    case AK_EXCHANGE_N: case AK_SYNC_LOCK_TAS: {
        int val = atomic_value(fn, e->args[1], obj);
        return atomic_result(fn, atomic_rmw(fn, IR_XCHG, 0, addr, val, obj),
                             obj);
    }
    case AK_TEST_AND_SET: {
        int old = atomic_rmw(fn, IR_XCHG, 0, addr, emit_const(fn, 1, 4), obj);
        return emit_cmp(fn, B_NE, old, emit_const(fn, 0, 4), 4, 0);
    }
    case AK_FETCH_OP: case AK_OP_FETCH: {
        int val = atomic_value(fn, e->args[1], obj);
        int old;
        if (op == '+' || op == '-') {
            /* x86 has lock xadd for these; subtraction adds the negation */
            int addend = op == '-'
                ? emit_bin(fn, IR_SUB, emit_const(fn, 0, w), val, w, 1)
                : val;
            old = atomic_rmw(fn, IR_XADD, 0, addr, addend, obj);
        } else {
            old = atomic_rmw(fn, IR_ARMW, op, addr, val, obj);
        }
        if (ak == AK_FETCH_OP)
            return atomic_result(fn, old, obj);
        /* OP_fetch: the new value is the old one with the operation applied
         * again — the same computation the atomic did, just not atomically,
         * which is fine: it only rebuilds what was stored. */
        int nv;
        switch (op) {
        case '+': nv = emit_bin(fn, IR_ADD, old, val, w, sign); break;
        case '-': nv = emit_bin(fn, IR_SUB, old, val, w, sign); break;
        case '&': nv = emit_bin(fn, IR_AND, old, val, w, sign); break;
        case '|': nv = emit_bin(fn, IR_OR, old, val, w, sign); break;
        case '^': nv = emit_bin(fn, IR_XOR, old, val, w, sign); break;
        default: {                                   /* nand: ~(old & val) */
            int a = emit_bin(fn, IR_AND, old, val, w, sign);
            struct ir_ins *n = emit(fn);
            n->op = IR_BNOT;
            n->a = a;
            n->w = w;
            n->dst = new_temp(fn);
            nv = n->dst;
            break;
        }
        }
        return atomic_result(fn, nv, obj);
    }
    case AK_CMPXCHG_N: case AK_CMPXCHG: {
        int exp = gen_expr(fn, e->args[1]);          /* &expected */
        int des = ak == AK_CMPXCHG_N
            ? atomic_value(fn, e->args[2], obj)
            : emit_load(fn, gen_expr(fn, e->args[2]), obj);  /* by pointer */
        struct ir_ins *i = emit(fn);
        i->op = IR_CMPXCHG;
        i->a = addr;
        i->b = exp;
        i->c = des;
        i->size = ty_size(obj);
        i->w = w;
        i->dst = new_temp(fn);
        return i->dst;
    }
    case AK_LOAD: {                                   /* *ret = atomic *p */
        int ret = gen_expr(fn, e->args[1]);
        emit_store(fn, ret, atomic_load(fn, addr, obj), obj);
        return -1;
    }
    case AK_STORE: {                                  /* atomic *p = *val */
        int vp = gen_expr(fn, e->args[1]);
        atomic_store(fn, addr, emit_load(fn, vp, obj), obj);
        return -1;
    }
    case AK_EXCHANGE: {                               /* *ret = xchg(p, *val) */
        int vp = gen_expr(fn, e->args[1]);
        int rp = gen_expr(fn, e->args[2]);
        int old = atomic_rmw(fn, IR_XCHG, 0, addr, emit_load(fn, vp, obj), obj);
        emit_store(fn, rp, old, obj);
        return -1;
    }
    case AK_SYNC_VAL_CAS: case AK_SYNC_BOOL_CAS: {
        int expv = atomic_value(fn, e->args[1], obj);
        int newv = atomic_value(fn, e->args[2], obj);
        struct ir_ins *i = emit(fn);
        i->op = IR_CAS;
        i->a = addr;
        i->b = expv;
        i->c = newv;
        i->size = ty_size(obj);
        i->w = w;
        i->dst = new_temp(fn);
        int old = atomic_result(fn, i->dst, obj);
        if (ak == AK_SYNC_VAL_CAS)
            return old;
        return emit_cmp(fn, B_EQ, old, expv, w, sign);
    }
    default:
        diag_fatal(fn->src->file, e->line, "internal: atomic kind %d", (int)ak);
        return -1;
    }
}

/* ---- the GCC bit builtins ------------------------------------------------
 *
 * Built from ordinary integer ops rather than a target instruction: x86-64's
 * popcnt is not baseline, a freestanding kernel links no libgcc to call
 * __popcountdi2 in, and one lowering for both targets is one lowering to get
 * right. Every form reduces to a population count:
 *   popcount  the SWAR sequence (pairs, nibbles, bytes, then a multiply)
 *   ctz(x)    popcount((x & -x) - 1)     — the bits below the lowest set one
 *   clz(x)    bits - popcount(x smeared right)
 *   ffs(x)    (x != 0) * (ctz(x) + 1)    — 0 for 0, as C's ffs defines it
 *   parity    popcount & 1
 *   clrsb(x)  clz(x ^ (x >> (bits-1))) - 1, the shift arithmetic
 * ctz and clz of 0 are undefined in gcc; these give the bit width.
 */
static int bk(struct ir_func *fn, unsigned long v, int w)
{
    return emit_const(fn, (long)v, w);
}

static int bpopcount(struct ir_func *fn, int x, int w)
{
    unsigned long m1 = w == 8 ? 0x5555555555555555UL : 0x55555555UL;
    unsigned long m2 = w == 8 ? 0x3333333333333333UL : 0x33333333UL;
    unsigned long m4 = w == 8 ? 0x0F0F0F0F0F0F0F0FUL : 0x0F0F0F0FUL;
    unsigned long h1 = w == 8 ? 0x0101010101010101UL : 0x01010101UL;
    int t = emit_bin(fn, IR_AND, emit_bin(fn, IR_SHR, x, bk(fn, 1, w), w, 0),
                     bk(fn, m1, w), w, 0);
    x = emit_bin(fn, IR_SUB, x, t, w, 0);
    t = emit_bin(fn, IR_AND, emit_bin(fn, IR_SHR, x, bk(fn, 2, w), w, 0),
                 bk(fn, m2, w), w, 0);
    x = emit_bin(fn, IR_ADD, emit_bin(fn, IR_AND, x, bk(fn, m2, w), w, 0), t,
                 w, 0);
    x = emit_bin(fn, IR_AND,
                 emit_bin(fn, IR_ADD, x,
                          emit_bin(fn, IR_SHR, x, bk(fn, 4, w), w, 0), w, 0),
                 bk(fn, m4, w), w, 0);
    x = emit_bin(fn, IR_MUL, x, bk(fn, h1, w), w, 0);
    return emit_bin(fn, IR_SHR, x, bk(fn, (unsigned long)(w * 8 - 8), w), w, 0);
}

static int bctz(struct ir_func *fn, int x, int w)
{
    struct ir_ins *n = emit(fn);
    n->op = IR_NEG;
    n->a = x;
    n->w = w;
    n->dst = new_temp(fn);
    int low = emit_bin(fn, IR_AND, x, n->dst, w, 0);           /* x & -x */
    return bpopcount(fn, emit_bin(fn, IR_SUB, low, bk(fn, 1, w), w, 0), w);
}

static int bclz(struct ir_func *fn, int x, int w)
{
    for (int sh = 1; sh < w * 8; sh <<= 1)
        x = emit_bin(fn, IR_OR, x, emit_bin(fn, IR_SHR, x, bk(fn, (unsigned long)sh, w),
                                             w, 0), w, 0);
    return emit_bin(fn, IR_SUB, bk(fn, (unsigned long)(w * 8), w),
                    bpopcount(fn, x, w), w, 0);
}

static int gen_bitop(struct ir_func *fn, struct expr *e, int kind, int w)
{
    /* the operand as the unsigned int / unsigned long the builtin takes (ffs
     * and clrsb take a signed one, which is the same bits) */
    const struct type *ut = ty_base(w == 8 ? TY_LONG : TY_INT, 1);
    int x = gen_convert(fn, gen_expr(fn, e->args[0]), e->args[0]->ty, ut);
    int r;
    switch (kind) {
    case 1: r = bctz(fn, x, w); break;
    case 2: r = bclz(fn, x, w); break;
    case 3: r = bpopcount(fn, x, w); break;
    case 4: {
        int nz = emit_cmp(fn, B_NE, x, bk(fn, 0, w), w, 0);
        nz = gen_convert(fn, nz, ty_base(TY_INT, 0), ut);
        r = emit_bin(fn, IR_MUL,
                     emit_bin(fn, IR_ADD, bctz(fn, x, w), bk(fn, 1, w), w, 0),
                     nz, w, 0);
        break;
    }
    case 5: r = emit_bin(fn, IR_AND, bpopcount(fn, x, w), bk(fn, 1, w), w, 0); break;
    default: {
        int sign = emit_bin(fn, IR_SHR, x, bk(fn, (unsigned long)(w * 8 - 1), w), w, 1);
        r = emit_bin(fn, IR_SUB, bclz(fn, emit_bin(fn, IR_XOR, x, sign, w, 0), w),
                     bk(fn, 1, w), w, 0);
        break;
    }
    }
    return gen_convert(fn, r, ut, ty_base(TY_INT, 0));    /* the result is int */
}

static int eh_type_index(struct ir_func *fn, struct global *ti);

int gen_expr(struct ir_func *fn, struct expr *e)
{
    /* Every instruction this expression lowers to is attributed to the
     * expression, not to the statement containing it -- so `a[i] + b[j]`
     * blames the right subscript. Saved and restored, because lowering
     * recurses and the caller's position must survive it. */
    int save_line = g_cur_line, save_col = g_cur_col;
    if (e->line) {
        g_cur_line = e->line;
        g_cur_col = e->col;
    }
    int r = gen_expr_inner(fn, e);
    g_cur_line = save_line;
    g_cur_col = save_col;
    return r;
}

static int gen_expr_inner(struct ir_func *fn, struct expr *e)
{
    switch (e->kind) {
    case EXPR_NUM:
        return emit_const(fn, e->num, ty_w(e->ty));
    case EXPR_EHTYPEID:          /* a catch type's selector: a constant */
        return emit_const(fn, eh_type_index(fn, e->gref), 8);
    case EXPR_FNUM:
        if (e->ty->kind == TY_LDOUBLE)
            return emit_ldconst(fn, e->ldv ? e->ldv : ldf_from_double(e->fnum));
        return emit_fconst(fn, e->fnum, ty_size(e->ty));
    case EXPR_LABELADDR: {   /* &&label -> a void* to the label's code location */
        struct ir_ins *i = emit(fn);
        i->op = IR_LABELADDR;
        i->label = g_labels[label_idx(fn, e->name, e->line)].label;
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_STR: {
        /* The literal arrives encoded at its real width (lit_encode): num
         * elements of str_width bytes each, NUL included. */
        int w = e->str_width ? e->str_width : 1;
        e->str_index = intern_str(e->name, (int)e->num * w);
        struct ir_ins *i = emit(fn);
        i->op = IR_STRADDR;
        i->label = e->str_index;
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_VAR:
        if (e->fref) { /* function designator: its address */
            struct ir_ins *i = emit(fn);
            i->op = IR_FADDR;
            i->callee = e->fref;
            i->dst = new_temp(fn);
            return i->dst;
        }
        /* arrays and structs are represented by their address */
        if (e->undecayed || e->ty->kind == TY_STRUCT)
            return gen_addr(fn, e);
        if (e->gref)
            return emit_load(fn, emit_gaddr(fn, e->gref), e->ty);
        return emit_ldvar(fn, e->var_index, e->ty);
    case EXPR_MEMBER: {
        int addr = gen_addr(fn, e);
        if (e->memb->is_bitfield)
            return bf_load(fn, addr, e->memb);
        if (e->undecayed || e->ty->kind == TY_STRUCT)
            return addr; /* array member decays; nested struct is addr */
        return emit_load(fn, addr, e->ty);
    }
    case EXPR_COMPLIT: {
        int addr = gen_complit(fn, e);
        if (e->undecayed || e->ty->kind == TY_STRUCT)
            return addr; /* an array decays; a struct is carried by address */
        return emit_load(fn, addr, e->ty);
    }
    case EXPR_ASSIGN: {
        if (e->ty->kind == TY_STRUCT) {
            /* a struct assignment is a copy of its bytes */
            int dst = gen_addr(fn, e->lhs);
            int src = gen_expr(fn, e->rhs); /* structs ARE addresses */
            struct ir_ins *i = emit(fn);
            i->op = IR_MEMCPY;
            i->a = dst;
            i->b = src;
            i->size = ty_size(e->ty);
            return dst;
        }
        if (e->lhs->kind == EXPR_VAR && !e->lhs->gref) {
            int v = gen_expr(fn, e->rhs);
            emit_stvar(fn, e->lhs->var_index, v, e->ty);
            return v;
        }
        /* global, *p, or member: a store through an address */
        int addr = gen_addr(fn, e->lhs);
        int v = gen_expr(fn, e->rhs);
        if (expr_is_bitfield(e->lhs))
            return bf_store(fn, addr, e->lhs->memb, v);
        emit_store(fn, addr, v, e->ty);
        return v;
    }
    case EXPR_INCDEC: {
        struct type *t = e->ty;
        int scale = t->kind == TY_PTR ? ty_size(t->pointee) : 1;
        int vla_step = t->kind == TY_PTR && ty_is_vla(t->pointee);
        int w = ty_w(t);
        int local = e->lhs->kind == EXPR_VAR && !e->lhs->gref;
        int is_bf = expr_is_bitfield(e->lhs);
        int addr = local ? -1 : gen_addr(fn, e->lhs);
        int cur = local ? emit_ldvar(fn, e->lhs->var_index, t)
                : is_bf ? bf_load(fn, addr, e->lhs->memb)
                        : emit_load(fn, addr, t);
        int old = -1;
        if (e->is_post) {
            struct ir_ins *save = emit(fn);
            save->op = IR_MOV;
            save->a = cur;
            save->dst = old = new_temp(fn);
        }
        int sum;
        if (ty_is_float(t))   /* x++ adds 1.0 — not 1 to the bit pattern */
            sum = emit_fbin(fn, e->delta > 0 ? IR_ADD : IR_SUB, cur,
                            emit_fconst(fn, 1.0, ty_size(t)), ty_size(t));
        else if (vla_step)   /* ++p over rows of a run-time size */
            sum = emit_bin(fn, e->delta > 0 ? IR_ADD : IR_SUB, cur,
                           type_size_val(fn, t->pointee), w, 1);
        else
            sum = emit_bin(fn, IR_ADD, cur,
                           emit_const(fn, (long)e->delta * scale, w), w, 1);
        if (ty_size(t) <= 2 && !is_bf) {
            /* ++c on a char must wrap like a char, in the value too */
            struct ir_ins *i = emit(fn);
            i->op = IR_EXT;
            i->a = sum;
            i->size = ty_size(t);
            i->sign = ty_signed_int(t);
            i->w = 4;
            i->dst = new_temp(fn);
            sum = i->dst;
        }
        if (local)
            emit_stvar(fn, e->lhs->var_index, sum, t);
        else if (is_bf)
            sum = bf_store(fn, addr, e->lhs->memb, sum);
        else
            emit_store(fn, addr, sum, t);
        return e->is_post ? old : sum;
    }
    case EXPR_NOT: {
        int w;
        int v = truth(fn, gen_expr(fn, e->rhs), e->rhs->ty, &w);
        return emit_isz(fn, v, w);
    }
    case EXPR_NEG:
    case EXPR_BNOT: {
        int v = gen_expr(fn, e->rhs);
        if (e->kind == EXPR_NEG && e->ty->kind == TY_LDOUBLE) {
            /* the sign bit of a 16-byte value: a float IR_NEG (fchs, or
             * flipping bit 127) — exact for -0.0 and NaN like the XOR below */
            struct ir_ins *i = emit(fn);
            i->op = IR_NEG;
            i->a = v;
            i->w = 16;
            i->flt = 1;
            i->dst = new_temp(fn);
            return i->dst;
        }
        if (e->kind == EXPR_NEG && ty_is_float(e->ty)) {
            /* -x on a float flips the sign BIT: exact for -0.0 and for
             * NaN, which 0.0-x is not, and it needs no new instruction
             * because the slot already holds the pattern. */
            int sz = ty_size(e->ty);
            int mask = emit_const(fn,
                                  sz == 8 ? (long)0x8000000000000000LL
                                          : (long)0x80000000L, sz);
            return emit_bin(fn, IR_XOR, v, mask, sz, 0);
        }
        struct ir_ins *i = emit(fn);
        i->op = e->kind == EXPR_NEG ? IR_NEG : IR_BNOT;
        i->a = v;
        i->w = ty_w(e->ty);
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_DEREF: {
        int addr = gen_expr(fn, e->rhs);
        /* *fp is fp (the OPERAND points at a function — nothing to
         * load); rows of 2-D arrays and structs are addresses too */
        if (e->rhs->ty->pointee->kind == TY_FUNC)
            return addr;
        if (e->undecayed || e->ty->kind == TY_STRUCT)
            return addr;
        struct ir_ins *i = emit(fn);
        i->op = IR_LOAD;
        i->a = addr;
        i->size = ty_size(e->ty);
        i->sign = ty_signed_int(e->ty);
        i->w = ty_w(e->ty);
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_ADDR:
        return gen_addr(fn, e->rhs);
    case EXPR_CAST: {
        int v = gen_expr(fn, e->rhs);
        if (e->ty->kind == TY_VOID)
            return v; /* evaluated for its effect; the value is dropped */
        return gen_convert(fn, v, e->rhs->ty, e->ty);
    }
    case EXPR_REAL:
    case EXPR_IMAG:
        break; /* sema lowered them to the parts; unreachable */
    case EXPR_SIZEOF:
        /* only a VLA's survives sema: read its size slot, computing it
         * first for a type name (sizeof(int[n])) */
        if (e->cast_ty) {
            vla_eval(fn, e->cast_ty);
            return type_size_val(fn, e->cast_ty);
        }
        return type_size_val(fn, e->rhs->undecayed ? e->rhs->undecayed
                                                   : e->rhs->ty);
    case EXPR_ALIGNOF:
        break; /* folded to EXPR_NUM by sema; unreachable */
    case EXPR_STMTEXPR:
        return gen_stmtexpr(fn, e);
    case EXPR_VA_ARG:
        return target_get() == TARGET_AARCH64 ? irg_va_arg_aapcs(fn, e)
                                              : irg_va_arg_sysv(fn, e);
    case EXPR_BINOP: {
        struct type *lt = e->lhs->ty, *rt = e->rhs->ty;

        switch (e->op) {
        case B_LAND:
        case B_LOR: {
            /* Short-circuit: the right side must not run when the left
             * decides — observable through calls. */
            int dst = new_temp(fn);
            int l_short = new_label(fn);
            int l_end = new_label(fn);
            int aw;
            int a = truth(fn, gen_expr(fn, e->lhs), lt, &aw);
            if (e->op == B_LAND) {
                emit_brz(fn, a, aw, l_short);
                int bw;
                int b = truth(fn, gen_expr(fn, e->rhs), rt, &bw);
                int nz = emit_isz(fn, b, bw);
                int one = emit_isz(fn, nz, 4); /* !!b */
                struct ir_ins *m = emit(fn);
                m->op = IR_MOV;
                m->a = one;
                m->dst = dst;
                emit_jmp(fn, l_end);
                emit_label(fn, l_short);
                struct ir_ins *z = emit(fn);
                z->op = IR_CONST;
                z->imm = 0;
                z->w = 4;
                z->dst = dst;
            } else {
                int l_rhs = new_label(fn);
                emit_brz(fn, a, aw, l_rhs);
                struct ir_ins *o = emit(fn);
                o->op = IR_CONST;
                o->imm = 1;
                o->w = 4;
                o->dst = dst;
                emit_jmp(fn, l_end);
                emit_label(fn, l_rhs);
                int bw;
                int b = truth(fn, gen_expr(fn, e->rhs), rt, &bw);
                int nz = emit_isz(fn, b, bw);
                int one = emit_isz(fn, nz, 4); /* !!b */
                struct ir_ins *m = emit(fn);
                m->op = IR_MOV;
                m->a = one;
                m->dst = dst;
            }
            emit_label(fn, l_end);
            return dst;
        }
        case B_ADD:
        case B_SUB: {
            int lp = lt->kind == TY_PTR, rp = rt->kind == TY_PTR;
            if (lp && rp) {
                /* ptr - ptr: signed byte difference divided by the element
                 * size. Division (not a shift) so any element size works,
                 * including non-powers-of-two like a 24-byte struct — the
                 * mirror of the IR_MUL scaling on the ptr+int path. */
                int a = gen_expr(fn, e->lhs);
                int b = gen_expr(fn, e->rhs);
                int diff = emit_bin(fn, IR_SUB, a, b, 8, 1);
                if (ty_is_vla(lt->pointee))
                    return emit_bin(fn, IR_DIV, diff,
                                    type_size_val(fn, lt->pointee), 8, 1);
                int size = ty_size(lt->pointee);
                if (size <= 1)
                    return diff;
                int c = emit_const(fn, size, 8);
                return emit_bin(fn, IR_DIV, diff, c, 8, 1);
            }
            if (lp || rp) {
                /* ptr +/- int: scale the (already long) index */
                struct expr *pe = lp ? e->lhs : e->rhs;
                struct expr *ie = lp ? e->rhs : e->lhs;
                int p = gen_expr(fn, lp ? pe : ie);
                int idx = gen_expr(fn, lp ? ie : pe);
                if (!lp) {
                    int t = p;
                    p = idx;
                    idx = t;
                }
                struct type *pt = (lp ? lt : rt)->pointee;
                int size = ty_size(pt);
                if (ty_is_vla(pt))
                    idx = emit_bin(fn, IR_MUL, idx, type_size_val(fn, pt),
                                   8, 1);
                else if (size > 1) {
                    int c = emit_const(fn, size, 8);
                    idx = emit_bin(fn, IR_MUL, idx, c, 8, 1);
                }
                return emit_bin(fn, e->op == B_ADD ? IR_ADD : IR_SUB,
                                p, idx, 8, 1);
            }
            /* plain arithmetic */
            int a = gen_expr(fn, e->lhs);
            int b = gen_expr(fn, e->rhs);
            enum ir_op o = e->op == B_ADD ? IR_ADD : IR_SUB;
            if (ty_is_float(e->ty))
                return emit_fbin(fn, o, a, b, ty_size(e->ty));
            return emit_bin(fn, o, a, b, ty_w(e->ty),
                            ty_signed_int(e->ty));
        }
        case B_EQ:
        case B_NE:
        case B_LT:
        case B_LE:
        case B_GT:
        case B_GE: {
            int a = gen_expr(fn, e->lhs);
            int b = gen_expr(fn, e->rhs);
            struct ir_ins *i = emit(fn);
            i->op = IR_CMP;
            i->pred = e->op;
            i->a = a;
            i->b = b;
            if (ty_is_float(lt)) {
                i->flt = 1;
                i->w = ty_size(lt);
            } else {
                i->w = ty_w(lt);
            }
            /* pointers compare unsigned, as C requires */
            i->sign = ty_signed_int(lt);
            i->dst = new_temp(fn);
            return i->dst;
        }
        default: {
            static const enum ir_op map[] = {
                IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
                IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR,
            };
            int a = gen_expr(fn, e->lhs);
            int b = gen_expr(fn, e->rhs);
            if (ty_is_float(e->ty))
                return emit_fbin(fn, map[e->op - B_ADD], a, b,
                                 ty_size(e->ty));
            return emit_bin(fn, map[e->op - B_ADD], a, b,
                            ty_w(e->ty), ty_signed_int(e->ty));
        }
        }
    }
    case EXPR_COMMA:
        gen_expr(fn, e->lhs); /* for side effects */
        return gen_expr(fn, e->rhs);
    case EXPR_INITLIST:
        break; /* consumed by sema's flattening; never evaluated */
    case EXPR_GENERIC:
        break; /* sema replaced it with the selected expression */
    case EXPR_COMPOUND: {
        /* the address is computed ONCE — the whole reason this is not
         * desugared to `x = x op y` */
        struct type *lt = e->lhs->ty;
        int local = e->lhs->kind == EXPR_VAR && !e->lhs->gref;
        int is_bf = expr_is_bitfield(e->lhs);
        int addr = local ? -1 : gen_addr(fn, e->lhs);
        int cur = local ? emit_ldvar(fn, e->lhs->var_index, lt)
                : is_bf ? bf_load(fn, addr, e->lhs->memb)
                        : emit_load(fn, addr, lt);
        int rv = gen_expr(fn, e->rhs);
        int res;
        if (lt->kind == TY_PTR) {
            int esz = ty_size(lt->pointee);
            if (ty_is_vla(lt->pointee))
                rv = emit_bin(fn, IR_MUL, rv, type_size_val(fn, lt->pointee),
                              8, 1);
            else if (esz > 1) {
                int k = emit_const(fn, esz, 8);
                rv = emit_bin(fn, IR_MUL, rv, k, 8, 1);
            }
            res = emit_bin(fn, e->op == B_ADD ? IR_ADD : IR_SUB, cur, rv,
                           8, 1);
        } else {
            struct type *ct = e->cast_ty;
            int cv = gen_convert(fn, cur, lt, ct);
            static const enum ir_op map[] = {
                IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
                IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR,
            };
            enum ir_op o = map[e->op - B_ADD];
            if (ty_is_float(ct))
                res = emit_fbin(fn, o, cv, rv, ty_size(ct));
            else
                res = emit_bin(fn, o, cv, rv, ty_w(ct),
                               ty_signed_int(ct));
            res = gen_convert(fn, res, ct, lt);
        }
        if (local)
            emit_stvar(fn, e->lhs->var_index, res, lt);
        else if (is_bf)
            return bf_store(fn, addr, e->lhs->memb, res);
        else
            emit_store(fn, addr, res, lt);
        return res;
    }
    case EXPR_COND: {
        int dst = new_temp(fn);
        int l_else = new_label(fn);
        int l_end = new_label(fn);
        int cw;
        int c = truth(fn, gen_expr(fn, e->args[0]), e->args[0]->ty, &cw);
        emit_brz(fn, c, cw, l_else);
        int a = gen_expr(fn, e->lhs);
        struct ir_ins *m1 = emit(fn);
        m1->op = IR_MOV;
        m1->a = a;
        m1->dst = dst;
        emit_jmp(fn, l_end);
        emit_label(fn, l_else);
        int b = gen_expr(fn, e->rhs);
        struct ir_ins *m2 = emit(fn);
        m2->op = IR_MOV;
        m2->a = b;
        m2->dst = dst;
        emit_label(fn, l_end);
        return dst;
    }
    case EXPR_CALL: {
        /* stdarg builtins: va_start records where its va_list lives so
         * codegen can point it at a freshly built __va_list_tag; va_end
         * is a no-op. Neither is a real call. */
        if (e->name && strcmp(e->name, "__builtin_va_start") == 0) {
            int ap = gen_addr(fn, e->args[0]);
            struct ir_ins *i = emit(fn);
            i->op = IR_VA_START;
            i->a = ap;
            return -1;
        }
        if (e->name && strcmp(e->name, "__builtin_va_end") == 0)
            return -1;
        /* va_copy(dst, src): copy the tag src points at into this call's
         * hidden slot, then point dst at the copy — so each list advances
         * independently, as C99 7.15.1.2 requires. */
        if (e->name && strcmp(e->name, "__builtin_va_copy") == 0) {
            struct ir_ins *ad = emit(fn);
            ad->op = IR_ADDR;
            ad->a = e->var_index;
            ad->dst = new_temp(fn);
            int tag = ad->dst;
            int src = gen_expr(fn, e->args[1]);
            struct ir_ins *c = emit(fn);
            c->op = IR_MEMCPY;
            c->a = tag;
            c->b = src;
            c->size = target_get() == TARGET_AARCH64 ? 32 : 24;
            struct expr *d = e->args[0];
            if (d->kind == EXPR_VAR && !d->gref)
                emit_stvar(fn, d->var_index, tag, d->ty);
            else
                emit_store(fn, gen_addr(fn, d), tag, d->ty);
            return -1;
        }
        if (e->name && strncmp(e->name, "__builtin_bswap", 15) == 0) {
            int v = gen_expr(fn, e->args[0]);
            struct ir_ins *i = emit(fn);
            i->op = IR_BSWAP;
            i->a = v;
            i->size = ty_size(e->ty);
            i->w = ty_w(e->ty);
            i->dst = new_temp(fn);
            return i->dst;
        }
        if (e->name && strcmp(e->name, "__sync_synchronize") == 0) {
            emit(fn)->op = IR_FENCE;
            return -1;
        }
        if (e->name && strcmp(e->name, "__builtin_prefetch") == 0) {
            for (int k = 0; k < e->nargs; k++)
                gen_expr(fn, e->args[k]);          /* for side effects only */
            return -1;
        }
        if (e->name && strncmp(e->name, "__builtin_", 10) == 0) {
            int bw, bk = builtin_bitop(e->name + 10, &bw);
            if (bk)
                return gen_bitop(fn, e, bk, bw);
        }
        if (e->name && (strcmp(e->name, "__builtin_unreachable") == 0 ||
                        strcmp(e->name, "__builtin_trap") == 0)) {
            emit(fn)->op = IR_UD2;
            return -1;
        }
        if (e->name) {
            int aop;
            enum atomic_kind ak = atomic_builtin(e->name, &aop);
            if (ak != AK_NONE)
                return gen_atomic(fn, e, ak, aop);
        }
        if (e->name && strcmp(e->name, "__builtin_alloca") == 0) {
            int size = gen_expr(fn, e->args[0]);
            struct ir_ins *al = emit(fn);
            al->op = IR_ALLOCA;
            al->a = size;
            al->dst = new_temp(fn);
            fn->has_alloca = 1;
            return al->dst;
        }
        if (e->name && (strcmp(e->name, "__builtin_return_address") == 0 ||
                        strcmp(e->name, "__builtin_frame_address") == 0)) {
            /* Walk e->num saved frame pointers up the chain, then either
             * stop (the frame address) or read the return address beside
             * it — [fp] is the caller's fp and [fp+8] the return address,
             * on both targets. */
            struct ir_ins *fa = emit(fn);
            fa->op = IR_FRAMEADDR;
            fa->w = 8;
            fa->dst = new_temp(fn);
            int fp = fa->dst;
            for (long k = 0; k < e->num; k++)
                fp = emit_load(fn, fp, e->ty);
            if (strcmp(e->name, "__builtin_frame_address") == 0)
                return fp;
            int at = emit_bin(fn, IR_ADD, fp, emit_const(fn, 8, 8), 8, 0);
            return emit_load(fn, at, e->ty);
        }
        int args[MAX_PARAMS];
        int fptemp = -1;
        if (!e->callee) /* through a pointer: evaluate the callee */
            fptemp = gen_expr(fn, e->lhs);
        for (int k = 0; k < e->nargs; k++)
            args[k] = gen_expr(fn, e->args[k]);
        struct ir_ins *i = emit(fn);
        i->op = IR_CALL;
        i->callee = e->callee;
        i->indirect = !e->callee;
        i->a = fptemp;
        i->call_varargs = e->callee ? e->callee->is_varargs
                                    : e->lhs->ty->pointee->is_varargs;
        i->sret_first = e->callee ? e->callee->sret_first
                                  : e->lhs->ty->pointee->sret_first;
        i->nargs = e->nargs;

        /* Classify every argument here, where the types still exist;
         * codegen only places what it is told. MEMORY-class arguments
         * are assigned offsets in this call's outgoing area, and the
         * function's frame reserves the widest such area. */
        int stk = 0;
        int ireg = 0, freg = 0;
        /* a hidden return pointer consumes rdi before anything else —
         * unless the struct comes back in x87 registers instead */
        if (e->ty->kind == TY_STRUCT) {
            enum arg_class rc[2];
            if (ty_classify(e->ty, rc) == 0 &&
                (target_get() == TARGET_AARCH64 || !ty_x87_ret(e->ty)))
                ireg = 1;
        }
        for (int k = 0; k < e->nargs; k++) {
            struct type *at = e->args[k]->ty;
            struct ir_arg *ar = &i->argv[k];
            ar->vreg = args[k];
            ar->ty = at;
            ar->is_struct = at->kind == TY_STRUCT;
            ar->size = ty_size(at);
            ar->nclass = ty_classify(at, ar->cls);
            ar->stk_off = 0;
            ar->on_stack = 0;

            /* SysV: an argument goes on the stack when its class has no
             * registers left for ALL of its eightbytes — the decision is
             * made here so codegen only follows it, and the two cannot
             * drift apart. */
            int ni = 0, nf = 0;
            for (int q = 0; q < ar->nclass; q++) {
                if (ar->cls[q] == CLASS_SSE)
                    nf++;
                else
                    ni++;
            }
            if (ar->nclass == 0 || ireg + ni > 6 || freg + nf > 8) {
                ar->on_stack = 1;
                /* a slot is aligned to the argument's own alignment, at
                 * least 8 — 16 for a long double (SysV 3.2.3) */
                stk = ty_align(at) > 8 ? (stk + 15) & ~15 : (stk + 7) & ~7;
                ar->stk_off = stk;
                stk += (ar->size + 7) & ~7;
            } else {
                ireg += ni;
                freg += nf;
            }
        }
        if (stk > fn->outgoing_bytes)
            fn->outgoing_bytes = stk;

        if (e->ty->kind == TY_STRUCT) {
            i->ret_x87 = target_get() == TARGET_AARCH64 ? 0
                                                        : ty_x87_ret(e->ty);
            i->retsize = ty_size(e->ty);
            i->rety = e->ty;
            i->retnclass = ty_classify(e->ty, i->retcls);
            fn->scratch_bytes = (fn->scratch_bytes + 7) & ~7;
            i->scratch = fn->scratch_bytes;
            fn->scratch_bytes += (i->retsize + 7) & ~7;
        }
        i->flt = ty_is_float(e->ty);
        i->w = i->flt ? ty_size(e->ty) : e->ty->kind == TY_INT128 ? 16 : 8;
        i->dst = new_temp(fn);
        return i->dst;
    }
    }
    return -1; /* unreachable; every kind returns above */
}

struct loopctx {
    int brk, cont;
    int brk_vla, cont_vla;   /* g_nvla at each target: the VLA scopes a
                              * break/continue leaves are those above it */
};

/* The exception region being generated (ir_func.eh), or -1. */
static int g_eh_cur = -1;

/* The selector a landing pad sees for catch type ti (NULL: catch-all):
 * its 1-based place in the function's type table. */
static int eh_type_index(struct ir_func *fn, struct global *ti)
{
    for (int i = 0; i < fn->neh_types; i++)
        if (fn->eh_types[i] == ti)
            return i + 1;
    fn->eh_types = xrealloc(fn->eh_types, (size_t)(fn->neh_types + 1) *
                                          sizeof *fn->eh_types);
    fn->eh_types[fn->neh_types++] = ti;
    return fn->neh_types;
}

static void gen_eh_region(struct ir_func *fn, struct stmt *s,
                          const struct loopctx *loop)
{
    int r = fn->neh++;
    fn->eh = xrealloc(fn->eh, (size_t)fn->neh * sizeof *fn->eh);
    memset(&fn->eh[r], 0, sizeof fn->eh[r]);
    fn->eh[r].parent = g_eh_cur;
    fn->eh[r].acts = s->eh_acts;
    fn->eh[r].nacts = s->neh_acts;
    int lp = fn->eh[r].lp_label = new_label(fn);
    int end = new_label(fn);
    for (int i = 0; i < s->neh_acts; i++)
        if (!s->eh_acts[i].cleanup)
            eh_type_index(fn, s->eh_acts[i].ti);
    int saved = g_eh_cur;
    g_eh_cur = r;
    fn->eh[r].lo = fn->nins;
    gen_stmt(fn, s->body, loop);
    fn->eh[r].hi = fn->nins;
    g_eh_cur = saved;
    emit_jmp(fn, end);
    /* the landing pad: its calls are the enclosing region's */
    emit_label(fn, lp);
    struct ir_ins *i = emit(fn);
    i->op = IR_LANDING;
    i->dst = new_temp(fn);
    i->b = new_temp(fn);
    i->w = 8;
    int exc = i->dst, sel = i->b;
    emit_store(fn, gen_addr(fn, s->expr), exc, s->expr->ty);
    emit_store(fn, gen_addr(fn, s->cond), sel, s->cond->ty);
    gen_stmt(fn, s->thn, loop);
    emit_label(fn, end);
}

void ir_add_csite(struct ir_func *fn, int start, int end, int region)
{
    if (fn->ncsites == fn->capcsites) {
        fn->capcsites = fn->capcsites ? fn->capcsites * 2 : 16;
        fn->csites = xrealloc(fn->csites,
                              (size_t)fn->capcsites * sizeof *fn->csites);
    }
    fn->csites[fn->ncsites].start = start;
    fn->csites[fn->ncsites].end = end;
    fn->csites[fn->ncsites].region = region;
    fn->ncsites++;
}

/* Each call's innermost region (regions are numbered outermost first) —
 * a call that can throw: not of a nothrow function. When no call in any
 * region can, the function has no landing pads after all (the pads'
 * code is unreachable), and is compiled as any other. */
static void mark_eh_calls(struct ir_func *fn)
{
    int any = 0;
    for (int r = 0; r < fn->neh; r++)
        for (int n = fn->eh[r].lo; n < fn->eh[r].hi; n++) {
            struct ir_ins *i = &fn->ins[n];
            if (i->op != IR_CALL ||
                (!i->indirect && i->callee && i->callee->is_nothrow))
                continue;
            i->eh_region = r + 1;
            any = 1;
        }
    if (!any)
        fn->neh = 0;
}

static void gen_stmt(struct ir_func *fn, struct stmt *s,
                     const struct loopctx *loop)
{
    for (; s; s = s->next) {
        if (s->line) {
            g_cur_line = s->line;   /* -g: rows key off statement lines */
            g_cur_col = s->col;
        }
        switch (s->kind) {
        case STMT_BREAK:
            vla_release(fn, loop->brk_vla);
            emit_jmp(fn, loop->brk);
            break;
        case STMT_CONTINUE:
            vla_release(fn, loop->cont_vla);
            emit_jmp(fn, loop->cont);
            break;
        case STMT_EHREGION:
            gen_eh_region(fn, s, loop);
            break;
        case STMT_LABEL: {
            int ix = label_idx(fn, s->name, s->line);
            if (g_labels[ix].defined)
                diag_fatal(fn->src->file, s->line, "duplicate label '%s'",
                           s->name);
            g_labels[ix].defined = 1;
            emit_label(fn, g_labels[ix].label);
            gen_stmt(fn, s->body, loop);   /* the labeled statement */
            break;
        }
        case STMT_GOTO:
            if (s->expr) {   /* computed goto: goto *expr (GNU) */
                int v = gen_expr(fn, s->expr);
                struct ir_ins *i = emit(fn);
                i->op = IR_IGOTO;
                i->a = v;
            } else {
                /* leaving the scope of every VLA whose remaining statements
                 * do not define the label: release from the outermost */
                for (int k = 0; k < g_nvla; k++)
                    if (!stmts_define_label(g_vla[k].decl->next, s->name)) {
                        vla_release(fn, k);
                        break;
                    }
                emit_jmp(fn, g_labels[label_idx(fn, s->name, s->line)].label);
            }
            break;
        case STMT_DECL:
            if (s->is_extern)
                break; /* block-scope extern: a declaration, emits no code */
            if (s->sglob)
                break; /* a static local IS its global; no code here */
            if (ty_is_vm(s->dty))
                vla_eval(fn, s->dty);
            if (ty_is_vla(s->dty)) {
                /* save sp, carve the array off the stack, keep its address
                 * in the variable's slot */
                struct type *ul = ty_base(TY_LONG, 1);
                struct ir_ins *sv = emit(fn);
                sv->op = IR_SPSAVE;
                sv->dst = new_temp(fn);
                emit_stvar(fn, s->vla_sp, sv->dst, ul);
                int size = type_size_val(fn, s->dty);
                struct ir_ins *al = emit(fn);
                al->op = IR_ALLOCA;
                al->a = size;
                al->dst = new_temp(fn);
                emit_stvar(fn, s->var_index, al->dst, ty_ptr(s->dty->pointee));
                fn->has_alloca = 1;
                if (g_nvla == (int)(sizeof g_vla / sizeof g_vla[0]))
                    diag_fatal(fn->src->file, s->line,
                               "more than %d nested variable length arrays",
                               (int)(sizeof g_vla / sizeof g_vla[0]));
                g_vla[g_nvla].sp_slot = s->vla_sp;
                g_vla[g_nvla].decl = s;
                g_nvla++;
                break;
            }
            if (s->ninits) {
                /* C zero-fills whatever the initializer does not
                 * mention, so clear the object first and then place
                 * the listed values. */
                struct ir_ins *ad = emit(fn);
                ad->op = IR_ADDR;
                ad->a = s->var_index;
                ad->dst = new_temp(fn);
                int base = ad->dst;
                struct ir_ins *z = emit(fn);
                z->op = IR_MEMZERO;
                z->a = base;
                z->size = ty_size(s->dty);
                for (int k = 0; k < s->ninits; k++) {
                    int v = gen_expr(fn, s->inits[k].e);
                    int at = base;
                    if (s->inits[k].off) {
                        int o = emit_const(fn, s->inits[k].off, 8);
                        at = emit_bin(fn, IR_ADD, base, o, 8, 1);
                    }
                    store_init_leaf(fn, at, &s->inits[k], v);
                }
                break;
            }
            if (s->expr) {
                int v = gen_expr(fn, s->expr);
                if (s->dty->kind == TY_STRUCT) {
                    /* initializing a struct is the same byte copy an
                     * assignment is */
                    struct ir_ins *a = emit(fn);
                    a->op = IR_ADDR;
                    a->a = s->var_index;
                    a->dst = new_temp(fn);
                    struct ir_ins *i = emit(fn);
                    i->op = IR_MEMCPY;
                    i->a = a->dst;
                    i->b = v;
                    i->size = ty_size(s->dty);
                } else {
                    emit_stvar(fn, s->var_index, v, s->dty);
                }
            }
            break;
        case STMT_EXPR:
            gen_expr(fn, s->expr); /* value discarded */
            break;
        case STMT_ASM:
            /* the template is assembled by the target's own vocabulary */
            if (target_get() == TARGET_AARCH64)
                irg_asm_arm64(fn, s);
            else
                irg_asm_x86(fn, s);
            break;
        case STMT_RETURN: {
            struct ir_ins *i;
            int v = s->expr ? gen_expr(fn, s->expr) : -1;
            i = emit(fn);
            i->op = IR_RET;
            i->a = v;
            if (s->expr && ty_is_float(s->expr->ty)) {
                i->flt = 1; /* the value goes home in xmm0, not rax */
                i->w = ty_size(s->expr->ty);
            } else if (s->expr && s->expr->ty->kind == TY_STRUCT) {
                /* `a` is the ADDRESS of the value; how it travels home
                 * is the callee's classification, computed in codegen
                 * from the function's own return type. */
                i->size = ty_size(s->expr->ty);
            }
            break;
        }
        case STMT_IF: {
            int l_else = new_label(fn);
            int cw;
            int c = truth(fn, gen_expr(fn, s->cond), s->cond->ty, &cw);
            emit_brz(fn, c, cw, l_else);
            gen_stmt(fn, s->thn, loop);
            if (s->els) {
                int l_end = new_label(fn);
                emit_jmp(fn, l_end);
                emit_label(fn, l_else);
                gen_stmt(fn, s->els, loop);
                emit_label(fn, l_end);
            } else {
                emit_label(fn, l_else);
            }
            break;
        }
        case STMT_DO: {
            /* body first, THEN the test — the whole point of do-while;
             * continue re-tests, so it targets the condition. */
            struct loopctx lc;
            int l_top = new_label(fn);
            lc.cont = new_label(fn);
            lc.brk = new_label(fn);
            lc.brk_vla = lc.cont_vla = g_nvla;
            emit_label(fn, l_top);
            gen_stmt(fn, s->body, &lc);
            emit_label(fn, lc.cont);
            int cw;
            int c = truth(fn, gen_expr(fn, s->cond), s->cond->ty, &cw);
            emit_brnz(fn, c, cw, l_top);
            emit_label(fn, lc.brk);
            break;
        }
        case STMT_CASE:
        case STMT_DEFAULT:
            emit_label(fn, s->label);
            break;
        case STMT_SWITCH: {
            /* A compare-and-branch chain: correct and slow, the house
             * rule (ARCHITECTURE §3). A jump table is an OPTIMIZATION
             * and belongs to the optimizer era, not here. */
            struct loopctx lc;
            lc.brk = new_label(fn);
            /* continue inside a switch belongs to the enclosing LOOP;
             * sema has already refused it when there is none. */
            lc.cont = loop ? loop->cont : -1;
            lc.brk_vla = g_nvla;
            lc.cont_vla = loop ? loop->cont_vla : 0;

            int v = gen_expr(fn, s->cond);
            int w = ty_w(s->cond->ty);
            int sign = ty_signed_int(s->cond->ty);
            struct stmt *list = switch_stmts(s->body);
            int dflt = -1;

            for (struct stmt *c = list; c; c = c->next) {
                if (c->kind == STMT_DEFAULT) {
                    c->label = new_label(fn);
                    dflt = c->label;
                    continue;
                }
                if (c->kind != STMT_CASE)
                    continue;
                c->label = new_label(fn);
                int k = emit_const(fn, c->cval, w);
                struct ir_ins *i = emit(fn);
                i->op = IR_CMP;
                i->pred = B_EQ;
                i->a = v;
                i->b = k;
                i->w = w;
                i->sign = sign;
                i->dst = new_temp(fn);
                emit_brnz(fn, i->dst, 4, c->label);
            }
            emit_jmp(fn, dflt >= 0 ? dflt : lc.brk);
            gen_stmt(fn, list, &lc); /* fallthrough is just: no jumps */
            emit_label(fn, lc.brk);
            break;
        }
        case STMT_WHILE: {
            struct loopctx lc;
            lc.cont = new_label(fn); /* while: continue re-tests */
            lc.brk = new_label(fn);
            lc.brk_vla = lc.cont_vla = g_nvla;
            emit_label(fn, lc.cont);
            int cw;
            int c = truth(fn, gen_expr(fn, s->cond), s->cond->ty, &cw);
            emit_brz(fn, c, cw, lc.brk);
            gen_stmt(fn, s->body, &lc);
            emit_jmp(fn, lc.cont);
            emit_label(fn, lc.brk);
            break;
        }
        case STMT_FOR: {
            /* for: continue jumps to the STEP, not the condition. */
            int l_cond = new_label(fn);
            struct loopctx lc;
            lc.cont = new_label(fn);
            lc.brk = new_label(fn);
            int for_vla = g_nvla;
            lc.brk_vla = lc.cont_vla = g_nvla;
            if (s->initdecl)
                gen_stmt(fn, s->initdecl, &lc);
            /* a VLA in the init-declaration lives across iterations: a
             * break/continue does not release it; leaving the loop does */
            lc.brk_vla = lc.cont_vla = g_nvla;
            if (s->init)
                gen_expr(fn, s->init);
            emit_label(fn, l_cond);
            if (s->cond) { /* NULL = forever, left by break */
                int cw;
                int c = truth(fn, gen_expr(fn, s->cond), s->cond->ty, &cw);
                emit_brz(fn, c, cw, lc.brk);
            }
            gen_stmt(fn, s->body, &lc);
            emit_label(fn, lc.cont);
            if (s->step)
                gen_expr(fn, s->step);
            emit_jmp(fn, l_cond);
            emit_label(fn, lc.brk);
            vla_release(fn, for_vla);
            g_nvla = for_vla;
            break;
        }
        case STMT_BLOCK: {
            /* Record the block's instruction span, then stamp every local
             * declared DIRECTLY in it with that scope range [lo, hi). Locals in
             * disjoint sibling blocks get disjoint ranges and may share a slot
             * (codegen). Static/extern decls have no frame slot — skip them. */
            int lo = fn->nins;
            int depth = g_nvla;
            gen_stmt(fn, s->body, loop);
            vla_release(fn, depth);   /* the block's VLAs end with it */
            g_nvla = depth;
            int hi = fn->nins;
            for (struct stmt *c = s->body; c; c = c->next)
                if (c->kind == STMT_DECL && !c->is_extern && !c->sglob &&
                    c->var_index >= 0 && c->var_index < fn->src->nvars) {
                    fn->var_scope_lo[c->var_index] = lo;
                    fn->var_scope_hi[c->var_index] = hi;
                }
            break;
        }
        }
    }
}

/* A GNU statement expression `({ s1; s2; ...; last })`: emit every statement,
 * and if the last is an expression statement yield its VALUE (a plain
 * STMT_EXPR would discard it). break/continue at the block's own level were
 * rejected by sema, so a dummy loop context suffices for the prefix. */
static int gen_stmtexpr(struct ir_func *fn, struct expr *e)
{
    static const struct loopctx none = { -1, -1, 0, 0 };
    struct stmt *body = e->body->body;   /* the block's statement list */
    struct stmt *last = NULL, *prev = NULL;
    for (struct stmt *s = body; s; s = s->next) {
        if (s->next)
            prev = s;
        last = s;
    }
    int depth = g_nvla;
    if (last && last != body) {          /* emit all but the last statement */
        prev->next = NULL;
        gen_stmt(fn, body, &none);
        prev->next = last;
    }
    int v = -1;
    if (last && last->kind == STMT_EXPR && last->expr)
        v = gen_expr(fn, last->expr);    /* the block's value */
    else if (last)
        gen_stmt(fn, last, &none);       /* a non-value last statement */
    vla_release(fn, depth);              /* its VLAs end with it */
    g_nvla = depth;
    return v;
}

/* -g: record one source variable. Skips the unnamed (prototype params never
 * reach a definition, but be defensive) so the DWARF DIE always has a name. */
static void add_dbgvar(struct ir_func *fn, const char *name, int vreg,
                       int is_param, struct type *ty, int line, int col)
{
    if (!name) return;
    if (fn->ndbgvars == fn->dbgvarcap) {
        fn->dbgvarcap = fn->dbgvarcap ? fn->dbgvarcap * 2 : 8;
        fn->dbgvars = xrealloc(fn->dbgvars,
                               (size_t)fn->dbgvarcap * sizeof *fn->dbgvars);
    }
    struct ir_dbgvar *v = &fn->dbgvars[fn->ndbgvars++];
    v->name = name;
    v->vreg = vreg;
    v->is_param = is_param;
    v->ty = ty;
    v->line = line;
    v->col = col;
}

/* -g: walk the body for block-scope locals. Each STMT_DECL owns a var slot
 * (var_index); a static local became a global (sglob) and has no frame slot,
 * so it is skipped. Flattened into the subprogram — lexical-block scoping is a
 * later refinement, not needed to print a local by name. */
static void collect_locals(struct ir_func *fn, struct stmt *s)
{
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_DECL:
            if (s->is_extern)     /* block-scope extern: no local slot at all */
                break;
            if (!s->sglob)
                add_dbgvar(fn, s->name, s->var_index, 0,
                           fn->src->var_tys[s->var_index], s->line, s->col);
            break;
        case STMT_IF:
            collect_locals(fn, s->thn);
            collect_locals(fn, s->els);
            break;
        case STMT_EHREGION:
            collect_locals(fn, s->body);
            collect_locals(fn, s->thn);
            break;
        case STMT_WHILE:
        case STMT_DO:
            collect_locals(fn, s->body);
            break;
        case STMT_FOR:
            collect_locals(fn, s->initdecl);
            collect_locals(fn, s->body);
            break;
        case STMT_BLOCK:
        case STMT_SWITCH:
            collect_locals(fn, s->body);
            break;
        default:
            break;
        }
    }
}

static void gen_func(struct ir_func *fn, struct func *f)
{
    fn->src = f;
    fn->nvregs = f->nvars; /* params + locals occupy [0, nvars) */
    g_cur_line = f->line;  /* prologue rows attribute to the definition */
    /* -g bookkeeping (harmless when -g is off — only the DWARF pass reads it):
     * parameters are vregs [0, nparams); locals come from the body. */
    for (int i = 0; i < f->nparams; i++)
        add_dbgvar(fn, f->params[i], i, 1, f->param_tys[i],
                   f->param_lines[i], f->param_cols[i]);
    collect_locals(fn, f->body);
    /* Local scope ranges: default to the whole function ([0, +inf), narrowed to
     * the real end below) so any local not inside a nested block never coalesces
     * — the safe default. gen_stmt's STMT_BLOCK case narrows nested-block locals. */
    if (f->nvars > 0) {
        fn->var_scope_lo = xmalloc((size_t)f->nvars * sizeof *fn->var_scope_lo);
        fn->var_scope_hi = xmalloc((size_t)f->nvars * sizeof *fn->var_scope_hi);
        for (int i = 0; i < f->nvars; i++) {
            fn->var_scope_lo[i] = 0;
            fn->var_scope_hi[i] = 0x7fffffff;   /* whole function until stamped */
        }
    }
    g_nlabels_used = 0;                 /* labels are per-function */
    g_nvla = 0;
    if (f->has_vm_params)               /* `int a[n][m]`: its row size */
        for (int i = 0; i < f->nparams; i++)
            vla_eval(fn, f->param_tys[i]);
    g_eh_cur = -1;
    gen_stmt(fn, f->body, NULL);
    if (fn->neh)
        mark_eh_calls(fn);
    for (int i = 0; i < f->nvars; i++)  /* clamp the un-narrowed default */
        if (fn->var_scope_hi[i] == 0x7fffffff)
            fn->var_scope_hi[i] = fn->nins;
    for (int i = 0; i < g_nlabels_used; i++)
        if (!g_labels[i].defined)
            diag_fatal(fn->src->file, g_labels[i].line,
                       "label '%s' used but not defined", g_labels[i].name);
    /* __int128 anywhere: a local, a parameter, the result, or an op */
    fn->has_i128 = f->ret_ty && f->ret_ty->kind == TY_INT128;
    for (int i = 0; i < f->nvars && !fn->has_i128; i++)
        fn->has_i128 = f->var_tys && f->var_tys[i] &&
                       f->var_tys[i]->kind == TY_INT128;
    for (int n = 0; n < fn->nins && !fn->has_i128; n++) {
        const struct ir_ins *i = &fn->ins[n];
        fn->has_i128 = (!i->flt && i->w == 16 && i->op != IR_LOAD &&
                        i->op != IR_LDVAR && i->op != IR_I2F &&
                        i->op != IR_F2F) ||
                       (i->op == IR_I2F && i->size == 16) ||
                       (i->op == IR_F2I && i->w == 16);
    }
}

struct ir_unit *irgen(struct unit *u)
{
    struct ir_unit *iu = xcalloc(1, sizeof *iu);
    iu->src = u;
    cur_unit = iu;

    /* Only canonical, defined functions produce code; prototypes of
     * externals produce symbols and relocations instead (driver).
     * UNUSED static functions are skipped entirely — headers define
     * static inline helpers wholesale (newlib stdio does), and
     * emitting the unused ones would drag their callees into every
     * link. Internal linkage makes this invisible to other objects. */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn &&
            (!f->is_static || f->used || strcmp(f->name, "main") == 0))
            iu->nfuncs++;
    iu->funcs = xcalloc((size_t)iu->nfuncs, sizeof *iu->funcs);

    int n = 0;
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn &&
            (!f->is_static || f->used || strcmp(f->name, "main") == 0))
            gen_func(&iu->funcs[n++], f);
    return iu;
}
