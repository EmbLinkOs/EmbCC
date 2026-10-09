/* ==== alias analysis ======================================================== *
 *
 * "Can these two memory references be the same bytes?" Every pass that
 * moves, reuses or deletes a memory operation needs the answer, and
 * until now there was none: pass_loadcse's kill model is "a store to a
 * non-address-taken local kills that local's LDVARs; anything else
 * kills every LOAD and every address-taken local's LDVAR". One store
 * through one pointer invalidated every cached load in the function.
 *
 * What makes a cheap answer possible is C's object model. A pointer
 * derived from one object does not point into another, so if two
 * references are based on DIFFERENT objects they cannot overlap --
 * whatever indexing happened in between. So the question becomes "what
 * object is this address based on", which is a walk back through the
 * address arithmetic:
 *
 *      %5 = gaddr @arr          <- the object
 *      %9 = shl %i, #2
 *      %10 = add %5, %9         <- still @arr
 *      load [%10]
 *
 * Three answers are possible: a named global, a numbered frame slot, or
 * unknown. The useful part is the last rule below -- an unknown pointer
 * cannot reach a slot whose address was never taken, because there is
 * no way for the program to have obtained one.
 *
 * What this deliberately does NOT do: type-based aliasing (EmbIR does
 * not carry the type of a load), `restrict`, and field sensitivity.
 * Those want metadata on the reference, which is the next step rather
 * than this one. */

#include "opt_int.h"

/* The object an address is based on, or MEM_UNKNOWN. Walks back through
 * the arithmetic irgen builds for `a[i]` and `p->f`, which by C's rules
 * cannot leave the object it started from. */
static struct memref mem_base(struct ir_func *fn, struct defs *d, int addr)
{
    struct memref r = { MEM_UNKNOWN, -1 };
    for (int hop = 0; hop < 8; hop++) {         /* a chain, not a cycle */
        if (addr < 0 || addr >= fn->nvregs || d->cnt[addr] != 1)
            return r;
        int n = d->ins[addr];
        if (n < 0)
            return r;                           /* a parameter: unknown */
        struct ir_ins *i = &fn->ins[n];
        switch (i->op) {
        case IR_GADDR:
            r.kind = MEM_GLOBAL; r.id = i->glob_sym; return r;
        case IR_ADDR:
            r.kind = MEM_SLOT; r.id = i->a; return r;
        case IR_MOV:
            addr = i->a; continue;
        case IR_ADD: case IR_SUB: {
            /* base + offset, either way round. Only one side can be a
             * base; if both resolve, the answer is not decidable here. */
            struct memref A = { MEM_UNKNOWN, -1 };
            if (i->a >= 0) {
                int an = i->a < fn->nvregs && d->cnt[i->a] == 1 ? d->ins[i->a] : -1;
                if (an >= 0 && (fn->ins[an].op == IR_GADDR ||
                                fn->ins[an].op == IR_ADDR ||
                                fn->ins[an].op == IR_MOV ||
                                fn->ins[an].op == IR_ADD))
                    A = mem_base(fn, d, i->a);
            }
            if (A.kind != MEM_UNKNOWN)
                return A;
            if (i->op == IR_SUB || i->imm_b)
                return r;                       /* offset - base is not a base */
            addr = i->b; continue;
        }
        default:
            return r;
        }
    }
    return r;
}

/* Can a reference based on `a` and one based on `b` be the same bytes? */
static int may_alias(struct memref a, struct memref b, const char *taken,
                     int nvars)
{
    if (a.kind == MEM_GLOBAL && b.kind == MEM_GLOBAL)
        return a.id == b.id;            /* two globals do not overlap */
    if (a.kind == MEM_SLOT && b.kind == MEM_SLOT)
        return a.id == b.id;            /* nor two frame slots */
    if ((a.kind == MEM_GLOBAL && b.kind == MEM_SLOT) ||
        (a.kind == MEM_SLOT && b.kind == MEM_GLOBAL))
        return 0;                       /* nor a global and a slot */
    /* One of them is unknown. It can be anything the program could have
     * taken the address of -- which a slot whose address was never
     * taken is not. */
    struct memref k = a.kind == MEM_UNKNOWN ? b : a;
    if (k.kind == MEM_SLOT && k.id >= 0 && k.id < nvars && !taken[k.id])
        return 0;
    return 1;
}

/* ==== which bytes of the object ============================================
 *
 * may_alias answers by OBJECT, and one object is where most of the
 * traffic is. A function handed `Queue_t *q` reads and writes q's fields
 * through that one pointer, so every store is to the same unknown object
 * as every load, and each store to one field made the optimizer forget
 * every other field and load it again: FreeRTOS's xQueueGenericReset
 * read pxQueue->pcHead a second time after storing pxQueue->pcWriteTo,
 * which clang does not.
 *
 * The finer question is which BYTES. An address that is a base value
 * plus a constant names a range of bytes from that value:
 *
 *      %8 = add %0, #60          (%0, 60, 4 bytes)
 *      %9 = load.4 [%8]
 *      %3 = add %0, #4           (%0, 4, 4 bytes)
 *      store:4 [%3], %15
 *
 * and two ranges from the SAME value that do not overlap are different
 * bytes, whatever the value is. Nothing else is assumed. Not the type of
 * either access: EmbCC does not do type-based aliasing, because the
 * kernels it compiles pun through casts. And not that pointer arithmetic
 * stays inside its object: the offsets are kept modulo the address width,
 * exactly as the machine adds them, so a negative offset, or one that
 * wraps, is compared as the bytes it really reaches (-fwrapv).
 *
 * "The same value" is what needs care, because a vreg is a name, not a
 * value. The base must be a temp written once, or a parameter never
 * reassigned -- and such a temp's definition dominates its uses, which
 * makes the name good enough: on any path, the address temps of a load
 * and of a later store were both computed after the base's LAST
 * definition, so they were computed from the same value of it. A load
 * whose address predates that definition is not remembered there at all:
 * load CSE's meet forgets it at the top of the block that defines the
 * base (the path from the entry into that block never loaded it), and
 * value numbering never carries anything across a block.
 *
 * The address of a global or of a frame slot is the same value wherever
 * it is computed, so the walk keys `gaddr @g` by the symbol and `addr v3`
 * by the slot, whichever temps hold them. Anything the walk cannot see
 * through -- a load, a call's result, an add of a variable -- is a base of
 * its own. Two different bases say nothing about each other, and the
 * question goes back to may_alias. */

/* The modulus addresses are computed in: the target's pointer width,
 * or the host's unsigned long when that is narrower. Narrower is still
 * sound for disjointness -- two byte addresses equal modulo 2^64 are
 * equal modulo 2^32 too, so ranges disjoint at 32 bits are disjoint at
 * 64 -- but not for containment, which pass_dse asks only when the
 * modulus is exact. */
static unsigned long addr_mask(int *exact)
{
    int bits = 8 * PTRW;
    if (bits <= 0 || bits >= (int)(8 * sizeof(unsigned long))) {
        *exact = bits == (int)(8 * sizeof(unsigned long));
        return ~0UL;
    }
    *exact = 1;
    return (1UL << bits) - 1;
}

/* A vreg that names one value for the whole function: a temp written
 * once, or a parameter nothing reassigns. A local's slot is not one --
 * its vreg number is read through IR_LDVAR, not as a value. */
int one_value(const struct ir_func *fn, const struct defs *d, int v)
{
    return v >= 0 && v < fn->nvregs && d->cnt[v] == 1 &&
           (v < fn->nparams || v >= fn->nvars);
}

/* The value of `v` when it is a constant of width `w`, the width of the
 * add that uses it: a constant of another width leaves the bits the add
 * reads above it to the backend, and they are not guessed here. */
static int addr_const(const struct ir_func *fn, const struct defs *d, int v,
                      int w, long *out)
{
    if (!one_value(fn, d, v) || d->ins[v] < 0)
        return 0;
    const struct ir_ins *c = &fn->ins[d->ins[v]];
    if (c->op != IR_CONST || c->flt || c->w != w)
        return 0;
    *out = c->imm;
    return 1;
}

/* Base, offset and extent of the `size` bytes at address temp `addr`.
 * The walk goes back through copies and through adds and subtracts of a
 * constant at the address width, while each step is one value; where it
 * stops is the base. The walk from any temp is the same walk, so two
 * addresses built on one base -- at whatever depth -- end at that base
 * with their offsets from it, unless the hop limit cuts one short, which
 * only makes the bases differ and the answer conservative. */
struct maccess mem_access(struct ir_func *fn, struct defs *d,
                          int addr, int size)
{
    struct maccess m;
    m.obj = mem_base(fn, d, addr);
    m.bkind = BASE_NONE; m.bid = -1; m.off = 0;
    m.size = size > 0 ? size : 0;
    int exact;
    unsigned long mask = addr_mask(&exact), off = 0;
    if (!one_value(fn, d, addr))
        return m;
    for (int hop = 0; hop < 16; hop++) {
        int n = d->ins[addr], next = -1;
        if (n >= 0) {
            const struct ir_ins *i = &fn->ins[n];
            long c = 0;
            if (i->op == IR_GADDR && i->glob_sym >= 0) {
                m.bkind = BASE_GLOBAL; m.bid = i->glob_sym;
                m.off = off & mask;
                return m;
            }
            if (i->op == IR_ADDR) {
                m.bkind = BASE_SLOT; m.bid = i->a;
                m.off = off & mask;
                return m;
            }
            if (i->op == IR_MOV) {
                next = i->a;
            } else if ((i->op == IR_ADD || i->op == IR_SUB) && !i->flt &&
                       i->w == PTRW) {
                int other = -1;
                if (i->imm_b) {
                    c = i->imm; other = i->a;
                } else if (addr_const(fn, d, i->b, i->w, &c)) {
                    other = i->a;
                } else if (i->op == IR_ADD &&
                           addr_const(fn, d, i->a, i->w, &c)) {
                    other = i->b;
                }
                if (other >= 0 && one_value(fn, d, other)) {
                    off = i->op == IR_SUB ? off - (unsigned long)c
                                          : off + (unsigned long)c;
                    next = other;
                }
            }
        }
        if (next < 0 || !one_value(fn, d, next))
            break;                      /* `addr` is the base */
        addr = next;
    }
    m.bkind = BASE_VREG; m.bid = addr;
    m.off = off & mask;
    return m;
}

/* A frame slot as a whole, as IR_LDVAR and IR_STVAR reach it: the object
 * is known, the bytes are not described. */
struct maccess slot_access(int var)
{
    struct maccess m;
    m.obj.kind = MEM_SLOT; m.obj.id = var;
    m.bkind = BASE_NONE; m.bid = -1; m.off = 0; m.size = 0;
    return m;
}

/* The object alone, for an access whose bytes are not described: a
 * memcpy, a memzero, a volatile store (whose existing rules stay as
 * they were). */
struct maccess obj_access(struct ir_func *fn, struct defs *d, int addr)
{
    struct maccess m;
    m.obj = mem_base(fn, d, addr);
    m.bkind = BASE_NONE; m.bid = -1; m.off = 0; m.size = 0;
    return m;
}

int same_base(struct maccess a, struct maccess b)
{
    return a.bkind != BASE_NONE && a.bkind == b.bkind && a.bid == b.bid &&
           a.size > 0 && b.size > 0;
}

/* Can these two accesses touch a common byte? From one base: only when
 * the ranges overlap on the ring of addresses -- b starts fewer than
 * a.size bytes past a, or a fewer than b.size bytes past b, modulo the
 * address width. Otherwise by object. */
int acc_overlap(struct maccess a, struct maccess b, const char *taken,
                int nvars)
{
    if (same_base(a, b)) {
        int exact;
        unsigned long mask = addr_mask(&exact);
        unsigned long ab = (b.off - a.off) & mask;   /* b past a */
        unsigned long ba = (a.off - b.off) & mask;   /* a past b */
        return ab < (unsigned long)a.size || ba < (unsigned long)b.size;
    }
    return may_alias(a.obj, b.obj, taken, nvars);
}

/* Does `later` write every byte `early` does? Only from one base, and
 * only when the modulus is the machine's own: this is the must-alias
 * answer dead-store elimination deletes a write on. */
int acc_covers(struct maccess later, struct maccess early)
{
    if (!same_base(later, early))
        return 0;
    int exact;
    unsigned long mask = addr_mask(&exact);
    if (!exact)
        return 0;
    unsigned long d = (early.off - later.off) & mask;  /* early past later */
    return d < (unsigned long)later.size &&
           (unsigned long)later.size - d >= (unsigned long)early.size;
}

/* The slots whose address is taken anywhere in the function: the only
 * locals a pointer can reach (may_alias). Freed by the caller. */
char *slots_taken(struct ir_func *fn)
{
    int nvars = fn->nvars;
    char *taken = xcalloc((size_t)(nvars ? nvars : 1), 1);
    for (int i = 0; i < fn->nins; i++)
        if (fn->ins[i].op == IR_ADDR && fn->ins[i].a >= 0 &&
            fn->ins[i].a < nvars)
            taken[fn->ins[i].a] = 1;
    return taken;
}

/* pass_lvn's memory kill. A store need not forget every remembered
 * read, only those it could reach: drop exactly those from the table and
 * return 1. Anything else that writes memory -- a call, a memcpy, an
 * atomic, a fence, asm, a vector store, any volatile write -- returns 0,
 * and the caller forgets every read as before (memver). Entries made
 * under an older memver can never match again and are dropped here
 * too. */
int lvn_mem_kill(struct ir_func *fn, struct defs *d, const char *taken,
                 struct vntab *tb, const struct ir_ins *ins)
{
    struct maccess w;
    if (ins->vol)
        return 0;
    if (ins->op == IR_STORE)
        w = mem_access(fn, d, ins->a, ins->size);
    else if (ins->op == IR_STVAR && ins->dst >= 0 && ins->dst < fn->nvars)
        w = slot_access(ins->dst);
    else
        return 0;
    /* Only the loads keyed at the current version can be reached: an
     * older one's key will never be asked for again. */
    int j = 0;
    for (int x = 0; x < tb->nld; x++) {
        int ei = tb->ld[x];
        struct vnent *e = &tb->e[ei];
        if (!e->live)
            continue;
        struct maccess r = e->k.op == IR_LOAD
            ? mem_access(fn, d, e->k.a, e->k.size) : slot_access(e->k.a);
        if (acc_overlap(w, r, taken, fn->nvars)) {
            vntab_unlink(tb, ei);
            continue;
        }
        tb->ld[j++] = ei;
    }
    tb->nld = j;
    return 1;
}
