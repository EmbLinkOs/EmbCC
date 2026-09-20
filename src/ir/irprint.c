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

/* An operation's mnemonic. Table-driven so the printer cannot drift from the
 * enum: a new opcode without a name here prints as `op<N>`, visibly wrong,
 * rather than silently as its neighbour. */
const char *ir_opname(enum ir_op op)
{
    static const char *const n[] = {
        "const", "mov", "add", "sub", "mul", "div", "mod", "and", "or",
        "xor", "shl", "shr", "neg", "bnot", "cmp", "ldvar", "stvar", "addr",
        "straddr", "gaddr", "faddr", "load", "store", "ext", "i2f", "f2i",
        "f2f", "call", "ret", "label", "jmp", "memcpy", "memzero", "brz",
        "brnz", "va_start", "bswap", "fence", "ud2", "xchg", "xadd",
        "cmpxchg", "asm", "labeladdr", "igoto", "armw", "cas", "cas16",
        "frameaddr", "alloca", "spsave", "sprestore", "landing",
    };
    if ((int)op < 0 || (size_t)op >= sizeof n / sizeof n[0])
        return "op?";
    return n[op];
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
    case IR_NEG: case IR_BNOT: case IR_BSWAP:
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
            ob_str(b, " varargs");
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
    if (f->is_static)
        ob_str(b, " static");
    ob_str(b, " {\n");
    ob_fmt(b, "  ; vregs %d, labels %d", f->nvregs, f->nlabels);
    if (f->scratch_bytes)
        ob_fmt(b, ", scratch %d", f->scratch_bytes);
    if (f->outgoing_bytes)
        ob_fmt(b, ", outgoing %d", f->outgoing_bytes);
    if (f->has_alloca)
        ob_str(b, ", alloca");
    if (f->has_i128)
        ob_str(b, ", i128");
    ob_ch(b, '\n');
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
