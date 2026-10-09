/* ==== attribute inference =================================================== *
 *
 * A call is the most pessimistic thing in the IR. Load elimination
 * drops every cached value at one, dead-store elimination cannot look
 * past one, and the alias analysis has to answer "could be anything".
 * That is right for a call that might write through a pointer it was
 * handed, and wrong for `abs`, `strlen`, or any of the small helpers a
 * program is mostly made of.
 *
 * So: work out from the body what a function actually does. Two
 * questions, both answered conservatively -- "not known to be" rather
 * than "known not to be":
 *
 *   Does it write memory the caller can see? A store through a pointer
 *   it was given, a store to a global, a call to something that does.
 *   Writing its OWN locals does not count: they die with the frame.
 *
 *   Does it read any? Same set, for loads.
 *
 * A function that does neither is `const` in gcc's sense -- its result
 * depends only on its arguments. One that only reads is `pure`.
 *
 * Recursion and indirect calls are where this has to be careful. The
 * fixpoint starts by assuming every function is clean and retreats;
 * starting from "clean" and only ever removing the property is what
 * makes a recursive cycle converge to the truth rather than to an
 * optimistic lie, because a cycle that does something dirty has that
 * fact introduced by whichever member does it. An indirect call could
 * go anywhere, so it is dirty outright. */

#include "opt_int.h"

void infer_attrs(struct ir_unit *iu)
{
    int nf = iu->nfuncs;
    if (nf <= 0)
        return;
    /* Start clean and retreat. Not a weak function: the body here is a
     * default the link may replace with one that does anything. */
    for (int k = 0; k < nf; k++) {
        struct func *f = iu->funcs[k].src;
        if (!f) continue;
        f->inf_no_write = f->inf_no_read = f->has_defn && !f->absorbed &&
                                           !f->is_weak;
    }
    for (int round = 0, changed = 1; changed && round < 32; round++) {
        changed = 0;
        for (int k = 0; k < nf; k++) {
            struct ir_func *fn = &iu->funcs[k];
            struct func *f = fn->src;
            if (!f || !f->inf_no_write) {
                if (f && f->inf_no_read) { f->inf_no_read = 0; changed = 1; }
                if (!f || !f->inf_no_read) continue;
            }
            int w = 1, r = 1;
            for (int n = 0; n < fn->nins && (w || r); n++) {
                struct ir_ins *i = &fn->ins[n];
                switch (i->op) {
                case IR_STORE: case IR_MEMCPY: case IR_MEMZERO:
                case IR_XCHG: case IR_XADD: case IR_CMPXCHG: case IR_ARMW:
                case IR_CAS: case IR_CAS16: case IR_ASM: case IR_VA_START:
                case IR_FENCE: case IR_VSTORE:
                    w = r = 0;          /* reaches memory the caller shares */
                    break;
                case IR_LOAD: case IR_VLOAD:
                    r = 0;
                    break;
                case IR_STVAR:
                    /* a local, unless its address escaped -- and if it
                     * did, IR_ADDR below has already said so */
                    break;
                case IR_LDVAR:
                    break;
                case IR_ADDR:
                    /* the address of a local leaving the function is the
                     * caller being handed a way in */
                    break;
                case IR_GADDR:
                    /* a global's address alone is not an access */
                    break;
                case IR_CALL: {
                    struct func *t = i->indirect ? NULL : i->callee;
                    if (!t || !t->has_defn) { w = r = 0; break; }
                    if (!t->inf_no_write) w = 0;
                    if (!t->inf_no_read)  r = 0;
                    break;
                }
                default:
                    break;
                }
            }
            if (!w && f->inf_no_write) { f->inf_no_write = 0; changed = 1; }
            if (!r && f->inf_no_read)  { f->inf_no_read = 0;  changed = 1; }
        }
    }
    if (!remarks_on())
        return;
    for (int k = 0; k < nf; k++) {
        struct func *f = iu->funcs[k].src;
        if (!f || !f->has_defn || f->absorbed)
            continue;
        if (f->inf_no_read)
            remark_add("opt", "inferred", f->name, "attr/const",
                       f->file, f->line, "reads and writes no memory");
        else if (f->inf_no_write)
            remark_add("opt", "inferred", f->name, "attr/pure",
                       f->file, f->line, "writes no memory");
    }
}
