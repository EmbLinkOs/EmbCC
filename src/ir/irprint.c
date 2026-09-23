/* EmbIR's textual form — `embcc inspect ir` (vision §18).
 *
 * The IR is the boundary between language semantics and machine code, and
 * until now nothing could look at it. A pass that goes wrong was debugged by
 * reading the machine code it eventually produced, which is the wrong place
 * to find out that mem2reg dropped a phi.
 *
 * ---- what this is not, yet ----
 *
 * §9.1 asks for a ROUND-TRIP textual form: print -> parse -> identical IR,
 * so every pass is testable text-in/text-out. This is the print half. The
 * IR is now self-contained enough for the other half to be possible -- it
 * carries its own function facts, its own symbol table, and the ABI answers
 * that used to be derived from `struct type` at codegen time -- but the
 * parser is not written.
 *
 * What a parsed IR still could not do is emit DWARF *types*: `add_dbgvar`
 * and the DWARF emitter want the type GRAPH (member names, nested types,
 * array bounds), which EmbIR does not carry and which carrying would be a
 * separate design decision. Code generation needs none of it.
 *
 * What this DOES give: every pass's effect is now visible
 * (`--inspect-at=irgen` vs `--inspect-at=opt`), which is what made the
 * optimizer's remaining questions answerable at all.
 */
#include "ir.h"

#include <stdio.h>
#include <string.h>

#include "../driver/util.h"
#include "../parse/ast.h"
#include "../sema/type.h"

/* An operation's mnemonic.
 *
 * Indexed BY OPCODE rather than written in order. The list used to be
 * positional, with the comment that a new opcode without a name here
 * would print as `op<N>` and so be visibly wrong -- which is true only
 * of one appended at the END. IR_SQRT was inserted in the middle and its
 * name appended at the bottom, so every opcode from IR_SQRT on took its
 * neighbour's name: `__builtin_sqrt` printed as `fence`, a real fence
 * printed as `ud2`, and ir_op_from_name -- which irparse.c reads --
 * mapped all of them one place wrong, so the IR text format did not
 * round-trip. Nothing caught it because both directions used the one
 * wrong table and agreed with each other.
 *
 * A designated initializer cannot drift: the name is written against the
 * opcode, so inserting an opcode anywhere leaves the rest in place, and
 * one with no name here is a NULL that reads as `op?`. */
const char *ir_opname(enum ir_op op)
{
    static const char *const n[IR_OPCOUNT] = {
        [IR_CONST] = "const",   [IR_MOV] = "mov",       [IR_ADD] = "add",
        [IR_SUB] = "sub",       [IR_MUL] = "mul",       [IR_DIV] = "div",
        [IR_MOD] = "mod",       [IR_AND] = "and",       [IR_OR] = "or",
        [IR_XOR] = "xor",       [IR_SHL] = "shl",       [IR_SHR] = "shr",
        [IR_NEG] = "neg",       [IR_BNOT] = "bnot",     [IR_CMP] = "cmp",
        [IR_LDVAR] = "ldvar",   [IR_STVAR] = "stvar",   [IR_ADDR] = "addr",
        [IR_STRADDR] = "straddr", [IR_GADDR] = "gaddr", [IR_FADDR] = "faddr",
        [IR_LOAD] = "load",     [IR_STORE] = "store",   [IR_EXT] = "ext",
        [IR_I2F] = "i2f",       [IR_F2I] = "f2i",       [IR_F2F] = "f2f",
        [IR_CALL] = "call",     [IR_RET] = "ret",       [IR_LABEL] = "label",
        [IR_JMP] = "jmp",       [IR_MEMCPY] = "memcpy", [IR_MEMZERO] = "memzero",
        [IR_BRZ] = "brz",       [IR_BRNZ] = "brnz",     [IR_VA_START] = "va_start",
        [IR_BSWAP] = "bswap",   [IR_SQRT] = "sqrt",     [IR_FENCE] = "fence",
        [IR_UD2] = "ud2",       [IR_XCHG] = "xchg",     [IR_XADD] = "xadd",
        [IR_CMPXCHG] = "cmpxchg", [IR_ASM] = "asm",
        [IR_LABELADDR] = "labeladdr", [IR_IGOTO] = "igoto",
        [IR_ARMW] = "armw",     [IR_CAS] = "cas",       [IR_CAS16] = "cas16",
        [IR_FRAMEADDR] = "frameaddr", [IR_ALLOCA] = "alloca",
        [IR_SPSAVE] = "spsave", [IR_SPRESTORE] = "sprestore",
        [IR_LANDING] = "landing",
        [IR_VLOAD] = "vload",   [IR_VSTORE] = "vstore", [IR_VBIN] = "vbin",
        [IR_VSPLAT] = "vsplat", [IR_VREDADD] = "vredadd",
        [IR_VWIDEN] = "vwiden", [IR_SELECT] = "select",
    };
    if ((int)op < 0 || (int)op >= IR_OPCOUNT || !n[op])
        return "op?";
    return n[op];
}

/* The inverse, for the parser: a mnemonic back to its opcode, or -1. One
 * table serves both directions, so print and parse cannot drift apart. */
int ir_op_from_name(const char *n)
{
    for (int op = 0; op < IR_OPCOUNT; op++)
        if (!strcmp(ir_opname((enum ir_op)op), n))
            return op;
    return -1;
}

int ir_pred_from_name(const char *n)
{
    static const char *const p[] = { "eq","ne","lt","le","gt","ge" };
    static const enum binop v[] = { B_EQ,B_NE,B_LT,B_LE,B_GT,B_GE };
    for (size_t i = 0; i < sizeof p / sizeof p[0]; i++)
        if (!strcmp(p[i], n))
            return v[i];
    return -1;
}

static const char *predname(enum binop p)
{
    switch (p) {
    case B_EQ: return "eq";
    case B_NE: return "ne";
    case B_LT: return "lt";
    case B_LE: return "le";
    case B_GT: return "gt";
    case B_GE: return "ge";
    default:   return "?";
    }
}

/* The width/flag suffix: `.4`, `.8s` (signed), `.8f` (float), `v`
 * (volatile). Absent where the op has no width, so `jmp L3` stays `jmp L3`. */
static void suffix(struct outbuf *b, const struct ir_ins *i, int with_w)
{
    if (with_w && i->w)
        ob_fmt(b, ".%d", i->w);
    if (i->sign)
        ob_ch(b, 's');
    if (i->flt)
        ob_ch(b, 'f');
    if (i->vol)
        ob_ch(b, 'v');
}

/* The same, for an op that also names a MEMORY width: the widths read
 * left to right as `.<result>:<memory>` before the flags, so a sign-extending
 * 4-byte load into 8 bytes is `ldvar.8:4s` and not `ldvars.8:4`. */
static void memsuffix(struct outbuf *b, const struct ir_ins *i, int with_w)
{
    if (with_w && i->w)
        ob_fmt(b, ".%d", i->w);
    ob_fmt(b, ":%d", i->size);
    if (i->sign) ob_ch(b, 's');
    if (i->flt)  ob_ch(b, 'f');
    if (i->vol)  ob_ch(b, 'v');
}

static void operand_b(struct outbuf *b, const struct ir_ins *i)
{
    if (i->imm_b)
        ob_fmt(b, "#%ld", i->imm);     /* the optimizer folded it */
    else
        ob_fmt(b, "%%%d", i->b);
}

/* A symbol's name, from the unit's own table rather than from the AST --
 * which is the point of interning it (§9.1). The pointer is the fallback
 * while both exist, so a regression in the table shows up as a NAME that
 * differs, not as a crash. */
static const char *sym_name(const struct ir_unit *u, int idx,
                            const char *fallback)
{
    if (u && idx >= 0 && idx < u->nsyms)
        return u->syms[idx].name;
    return fallback ? fallback : "?";
}

static void print_ins(struct outbuf *b, const struct ir_unit *u,
                      const struct ir_ins *i)
{
    /* A label is the only thing that starts at column 0: it is a position in
     * the instruction stream, not an operation on values. */
    if (i->op == IR_LABEL) {
        ob_fmt(b, "L%d:\n", i->label);
        return;
    }

    ob_str(b, "  ");
    switch (i->op) {
    case IR_CONST:
        ob_fmt(b, "%%%d = const", i->dst); suffix(b, i, 1);
        ob_fmt(b, " %ld", i->imm);
        break;
    case IR_MOV:
        ob_fmt(b, "%%%d = mov", i->dst); suffix(b, i, 1);
        ob_fmt(b, " %%%d", i->a);
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
        ob_fmt(b, "%%%d = %s", i->dst, ir_opname(i->op)); suffix(b, i, 1);
        ob_fmt(b, " %%%d, ", i->a); operand_b(b, i);
        break;
    case IR_NEG: case IR_BNOT: case IR_BSWAP: case IR_SQRT:
        ob_fmt(b, "%%%d = %s", i->dst, ir_opname(i->op)); suffix(b, i, 1);
        ob_fmt(b, " %%%d", i->a);
        break;
    case IR_CMP:
        ob_fmt(b, "%%%d = cmp", i->dst); suffix(b, i, 1);
        ob_fmt(b, " %s %%%d, ", predname(i->pred), i->a); operand_b(b, i);
        break;
    case IR_LDVAR:
        ob_fmt(b, "%%%d = ldvar", i->dst); memsuffix(b, i, 1);
        ob_fmt(b, " v%d", i->a);
        break;
    case IR_STVAR:
        ob_str(b, "stvar"); memsuffix(b, i, 0);
        ob_fmt(b, " v%d, %%%d", i->dst, i->a);
        break;
    case IR_LOAD:
        ob_fmt(b, "%%%d = load", i->dst); memsuffix(b, i, 1);
        ob_fmt(b, " [%%%d]", i->a);
        break;
    case IR_STORE:
        ob_str(b, "store"); memsuffix(b, i, 0);
        ob_fmt(b, " [%%%d], %%%d", i->a, i->b);
        break;
    case IR_ADDR:
        ob_fmt(b, "%%%d = addr v%d", i->dst, i->a);
        break;
    case IR_STRADDR:
        ob_fmt(b, "%%%d = straddr str%d", i->dst, i->label);
        break;
    case IR_GADDR:
        ob_fmt(b, "%%%d = gaddr @%s", i->dst,
               sym_name(u, i->glob_sym, i->glob ? i->glob->name : NULL));
        break;
    case IR_FADDR:
        ob_fmt(b, "%%%d = faddr @%s", i->dst,
               sym_name(u, i->callee_sym, i->callee ? i->callee->name : NULL));
        break;
    case IR_EXT:
        ob_fmt(b, "%%%d = ext", i->dst); memsuffix(b, i, 1);
        ob_fmt(b, " %%%d", i->a);
        break;
    case IR_I2F: case IR_F2I: case IR_F2F:
        ob_fmt(b, "%%%d = %s", i->dst, ir_opname(i->op)); memsuffix(b, i, 1);
        ob_fmt(b, " %%%d", i->a);
        break;
    case IR_CALL:
        if (i->dst >= 0)
            ob_fmt(b, "%%%d = ", i->dst);
        if (i->indirect)
            ob_fmt(b, "call [%%%d](", i->a);
        else
            ob_fmt(b, "call @%s(",
                   sym_name(u, i->callee_sym,
                            i->callee ? i->callee->name : NULL));
        for (int k = 0; k < i->nargs; k++)
            ob_fmt(b, "%s%%%d", k ? ", " : "", i->argv[k].vreg);
        ob_ch(b, ')');
        if (i->call_varargs)
            ob_fmt(b, " varargs(%d)", i->call_nfixed);
        if (i->sret_first)
            ob_str(b, " sret");
        break;
    case IR_RET:
        ob_str(b, "ret");
        if (i->a >= 0)
            ob_fmt(b, " %%%d", i->a);
        break;
    case IR_JMP:
        ob_fmt(b, "jmp L%d", i->label);
        break;
    case IR_BRZ: case IR_BRNZ:
        ob_fmt(b, "%s", ir_opname(i->op)); suffix(b, i, 1);
        ob_fmt(b, " %%%d -> L%d", i->a, i->label);
        break;
    case IR_LABELADDR:
        ob_fmt(b, "%%%d = labeladdr L%d", i->dst, i->label);
        break;
    case IR_IGOTO:
        ob_fmt(b, "igoto [%%%d]", i->a);
        break;
    case IR_MEMCPY:
        ob_fmt(b, "memcpy:%d [%%%d], [%%%d]", i->size, i->a, i->b);
        break;
    case IR_MEMZERO:
        ob_fmt(b, "memzero:%d [%%%d]", i->size, i->a);
        break;
    case IR_XCHG: case IR_XADD: case IR_CAS: case IR_CAS16: case IR_ARMW:
        ob_fmt(b, "%%%d = %s", i->dst, ir_opname(i->op)); memsuffix(b, i, 1);
        ob_fmt(b, " [%%%d], %%%d", i->a, i->b);
        if (i->op == IR_CAS || i->op == IR_CAS16)
            ob_fmt(b, ", %%%d", i->c);
        if (i->op == IR_ARMW)
            ob_fmt(b, " op '%c'", (int)i->imm);
        break;
    case IR_CMPXCHG:
        ob_fmt(b, "%%%d = cmpxchg", i->dst); memsuffix(b, i, 1);
        ob_fmt(b, " [%%%d], [%%%d], %%%d", i->a, i->b, i->c);
        break;
    case IR_ALLOCA:
        ob_fmt(b, "%%%d = alloca %%%d", i->dst, i->a);
        break;
    case IR_SPSAVE:
        ob_fmt(b, "%%%d = spsave", i->dst);
        break;
    case IR_SPRESTORE:
        ob_fmt(b, "sprestore %%%d", i->a);
        break;
    case IR_FRAMEADDR:
        ob_fmt(b, "%%%d = frameaddr", i->dst);
        break;
    case IR_LANDING:
        ob_fmt(b, "%%%d, %%%d = landing", i->dst, i->b);
        break;
    /* Vectors print on the ordinary `.w:size` form, with the element
     * width as `size` -- the lane count is 16/size and adding a second
     * spelling only gave the parser something else to learn. §9.1 wants
     * print and parse to give back identical IR, so every opcode has to
     * be taught to both, and the vector ones were taught to neither
     * until tests/golden/ir-roundtrip.sh was pointed at a file that
     * actually vectorizes. */
    case IR_VLOAD:
        ob_fmt(b, "%%%d = vload", i->dst); memsuffix(b, i, 1);
        ob_fmt(b, " [%%%d]", i->a);
        break;
    case IR_VSTORE:
        ob_str(b, "vstore"); memsuffix(b, i, 1);
        ob_fmt(b, " [%%%d], %%%d", i->a, i->b);
        break;
    case IR_VBIN:
        ob_fmt(b, "%%%d = vbin", i->dst); memsuffix(b, i, 1);
        ob_fmt(b, " %c %%%d, ", (char)i->imm, i->a);
        if (i->imm == '<' || i->imm == '>')
            ob_fmt(b, "#%d", i->c);        /* a shift count, not a vector */
        else
            ob_fmt(b, "%%%d", i->b);
        break;
    case IR_VSPLAT:
        ob_fmt(b, "%%%d = vsplat", i->dst); memsuffix(b, i, 1);
        ob_fmt(b, " %%%d", i->a);
        break;
    case IR_VREDADD:
        ob_fmt(b, "%%%d = vredadd", i->dst); memsuffix(b, i, 1);
        ob_fmt(b, " %%%d", i->a);
        break;
    case IR_VWIDEN:
        ob_fmt(b, "%%%d = vwiden", i->dst); memsuffix(b, i, 1);
        ob_fmt(b, " %s %%%d", i->c ? "hi" : "lo", i->a);
        break;
    case IR_SELECT:
        ob_fmt(b, "%%%d = select", i->dst); suffix(b, i, 1);
        ob_fmt(b, " %%%d ? %%%d : %%%d", i->a, i->b, i->c);
        break;
    case IR_VA_START:
        ob_fmt(b, "va_start [%%%d]", i->a);
        break;
    case IR_ASM:
        /* The template is already assembled to bytes by this point, so
         * what there is to show is its shape, not its text. */
        ob_str(b, "asm");
        if (i->asm_ir)
            ob_fmt(b, " %d bytes, %d in, %d out", i->asm_ir->codelen,
                   i->asm_ir->nin, i->asm_ir->nout);
        break;
    case IR_FENCE: case IR_UD2:
        ob_str(b, ir_opname(i->op));
        break;
    default:
        ob_fmt(b, "%s dst=%%%d a=%%%d b=%%%d", ir_opname(i->op),
               i->dst, i->a, i->b);
        break;
    }
    /* Provenance (R3): line and, since it is now carried, the column. */
    if (i->line) {
        if (i->col)
            ob_fmt(b, "\t; %d:%d", i->line, i->col);
        else
            ob_fmt(b, "\t; line %d", i->line);
    } else if (i->synth) {
        ob_str(b, "\t; compiler-synthesized");
    }
    ob_ch(b, '\n');
}

static void print_func(struct outbuf *b, const struct ir_unit *u,
                       const struct ir_func *f)
{
    ob_fmt(b, "func @%s", f->name ? f->name : "?");
    if (f->is_static)  ob_str(b, " static");
    if (f->is_varargs) ob_str(b, " varargs");
    if (f->has_alloca) ob_str(b, " alloca");
    if (f->has_i128)   ob_str(b, " i128");
    ob_fmt(b, " nparams=%d nvars=%d vregs=%d labels=%d",
           f->nparams, f->nvars, f->nvregs, f->nlabels);
    if (f->scratch_bytes)
        ob_fmt(b, " scratch=%d", f->scratch_bytes);
    if (f->outgoing_bytes)
        ob_fmt(b, " outgoing=%d", f->outgoing_bytes);
    ob_str(b, " {\n");
    /* The frame slots, with what irgen decided about each type. A reader
     * rebuilding this IR needs them, and a reader UNDERSTANDING it wanted
     * them anyway: `v3 is 8 bytes, aligned 8, a pointer` is the question a
     * stack-layout bug always comes down to. */
    for (int i = 0; i < f->nvars && f->locals; i++) {
        const struct ir_local *L = &f->locals[i];
        ob_fmt(b, "  local v%d size=%d align=%d", i, L->size, L->align);
        if (L->user_align)          ob_fmt(b, " user_align=%d", L->user_align);
        if (L->is_volatile)         ob_str(b, " volatile");
        if (L->is_ldouble)          ob_str(b, " ldouble");
        if (L->is_int128)           ob_str(b, " int128");
        if (L->is_int_or_ptr)       ob_str(b, " intptr");
        if (L->is_scalar_int_or_ptr) ob_str(b, " scalar");
        ob_ch(b, '\n');
    }
    for (int i = 0; i < f->nins; i++)
        print_ins(b, u, &f->ins[i]);
    ob_str(b, "}\n");
}

void ir_print_func(struct outbuf *b, const struct ir_func *f)
{
    print_func(b, NULL, f);
}

void ir_print_unit(struct outbuf *b, const struct ir_unit *u)
{
    ob_str(b, "; EmbIR\n");
    for (int i = 0; i < u->nstrs; i++) {
        ob_fmt(b, "str%d = \"", i);
        const char *sp = u->strs[i].bytes;
        for (int k = 0; sp && k + 1 < u->strs[i].len; k++) {  /* not the NUL */
            char ch = sp[k];
            if (ch == '\n')      ob_str(b, "\\n");
            else if (ch == '\t') ob_str(b, "\\t");
            else if (ch == '"')  ob_str(b, "\\\"");
            else if (ch == '\\') ob_str(b, "\\\\");
            else                 ob_ch(b, ch);
        }
        ob_str(b, "\"\n");
    }
    if (u->nstrs)
        ob_ch(b, '\n');
    /* The symbols the unit refers to, with what the backends need of them.
     * Printed because a self-contained IR has to CARRY this: a reader of
     * the text must be able to resolve `@memcpy` without a parse tree. */
    for (int i = 0; i < u->nsyms; i++) {
        const struct ir_sym *y = &u->syms[i];
        ob_fmt(b, "%s @%s", y->is_func ? "func" : "data", y->name);
        if (y->defined)    ob_str(b, " defined");
        if (y->is_weak)    ob_str(b, " weak");
        if (y->is_varargs) ob_str(b, " varargs");
        if (y->sret_first) ob_str(b, " sret");
        if (y->is_nothrow) ob_str(b, " nothrow");
        ob_str(b, " ;decl\n");
    }
    if (u->nsyms)
        ob_ch(b, '\n');
    for (int i = 0; i < u->nfuncs; i++) {
        if (i)
            ob_ch(b, '\n');
        print_func(b, u, &u->funcs[i]);
    }
}
