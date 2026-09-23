/* EmbIR: a linear three-address form over virtual registers, all of
 * type int (ARCHITECTURE §3 — "the simplest thing that lets codegen be
 * written without lying"). Control flow is labels and conditional
 * branches; short-circuit && and || are lowered to branches here, so
 * codegen never sees them. Still no SSA and no passes — that revision
 * comes with the optimizer (VISION_LONGTERM), not before.
 *
 * vreg numbering: [0, nparams) are the parameters, then locals in
 * declaration order, then expression temporaries.
 */
#ifndef EMBCC_IR_IR_H
#define EMBCC_IR_IR_H

#include "../parse/ast.h"
#include "../sema/type.h"

/* Width/representation model (see sema/type.h): temporaries hold
 * promoted values — `w` is 4 (int class) or 8 (long/pointer class) and
 * selects 32- vs 64-bit operations. Variables live in memory at their
 * true `size` (1/2/4/8); LDVAR/LOAD extend on the way in (per `sign`),
 * STVAR/STORE truncate on the way out. `sign` also picks signed vs
 * unsigned division, shift, and comparison. */
enum ir_op {
    IR_CONST, /* dst = imm            (w) */
    IR_MOV,   /* dst = a              (full temp-to-temp copy) */
    IR_ADD,   /* dst = a + b          (w) */
    IR_SUB,   /* dst = a - b          (w) */
    IR_MUL,   /* dst = a * b          (w) */
    IR_DIV,   /* dst = a / b          (w, sign: idiv/div) */
    IR_MOD,   /* dst = a % b          (w, sign) */
    IR_AND,   /* dst = a & b          (w) */
    IR_OR,    /* dst = a | b          (w) */
    IR_XOR,   /* dst = a ^ b          (w) */
    IR_SHL,   /* dst = a << b         (w) */
    IR_SHR,   /* dst = a >> b         (w, sign: sar/shr) */
    IR_NEG,   /* dst = -a             (w) */
    IR_BNOT,  /* dst = ~a             (w) */
    IR_CMP,   /* dst = (a pred b) 0/1 (w, sign; pred is B_EQ..B_GE) */
    IR_LDVAR, /* dst = var a          (size, sign, w: extend) */
    IR_STVAR, /* var dst = a          (size: truncating store) */
    IR_ADDR,  /* dst = &var a         (always w=8) */
    IR_STRADDR, /* dst = &.rodata string (label = string index) */
    IR_GADDR, /* dst = &global (glob) */
    IR_FADDR, /* dst = &function (callee) */
    IR_LOAD,  /* dst = *(temp a)      (size, sign, w: extend) */
    IR_STORE, /* *(temp a) = b        (size) */
    IR_EXT,   /* dst = a re-extended  (size, sign: from; w: to) */
    IR_I2F,   /* dst = (float)a       (size,sign: int src; w: float dst) */
    IR_F2I,   /* dst = (int)a         (size: float src; w,sign: int dst) */
    IR_F2F,   /* dst = (float)a       (size: src width; w: dst width) */
    IR_CALL,  /* dst = callee(args...); indirect: target fp in a */
    IR_RET,   /* return a (a == -1: void return) */
    IR_LABEL, /* label: (id in `label`) */
    IR_JMP,   /* goto label */
    IR_MEMCPY,/* copy `size` bytes: *(addr a) <- *(addr b) */
    IR_MEMZERO,/* zero `size` bytes at (addr a) */
    IR_BRZ,   /* if (a == 0) goto label  (w) */
    IR_BRNZ,  /* if (a != 0) goto label  (w) */
    IR_VA_START, /* init the va_list whose ADDRESS is in temp a (SysV:
                  * fill a __va_list_tag on the frame, point *a at it) */
    IR_BSWAP, /* dst = byteswap(a)   (size: 2/4/8; __builtin_bswapN) */
    IR_SQRT,  /* dst = sqrt(a)       (flt; w: 4 float / 8 double)
               * One instruction on both targets -- sqrtss/sqrtsd and
               * fsqrt -- and correctly rounded by the hardware, which no
               * software core matches. A C library that had to call out
               * to software here would be slower AND less accurate. */
    IR_FENCE, /* a full memory barrier (mfence; __sync_synchronize) */
    IR_UD2,   /* the undefined instruction (ud2; __builtin_unreachable) */
    IR_XCHG,  /* dst = *(temp a); *(temp a) = b   (atomic; size) */
    IR_XADD,  /* dst = *(temp a); *(temp a) += b  (lock xadd; size) */
    IR_CMPXCHG, /* CAS at *(a): compare against *(b), set to c on match;
                 * dst = matched?1:0, and *(b) updated to the seen value.
                 * (lock cmpxchg; size) */
    IR_ASM,   /* extended asm: load inputs to fixed registers, assemble the
               * template, store outputs. Detail in ir_ins.asm_ir */
    IR_LABELADDR, /* dst = &&label  (GNU label address; id in `label`) */
    IR_IGOTO, /* goto *a  (GNU computed goto: jump to the address in temp a) */
    IR_ARMW,  /* dst = *(temp a); *(temp a) = dst OP b   (atomic; size, w).
               * OP is in `imm`: '&' '|' '^', or 'n' for nand = ~(dst & b).
               * Add and subtract stay IR_XADD, which x86 does in one
               * locked instruction; these need a compare-and-swap loop. */
    IR_CAS,   /* dst = *(temp a); if dst == b then *(temp a) = c
               * (atomic compare-and-swap by VALUE; size, w). The result is
               * the value seen, whether or not the swap happened — the
               * __sync_*_compare_and_swap shape, where IR_CMPXCHG is the
               * __atomic one (expected passed by address, a bool back). */
    IR_CAS16, /* IR_CAS of 16 bytes (an __int128): b, c and dst wide
               * (x86-64's lock cmpxchg16b, aarch64's exclusive pair;
               * a full barrier both) — irgen builds the other atomics of
               * an __int128 as loops of it */
    IR_FRAMEADDR, /* dst = this function's frame pointer (rbp / x29), which
                   * on both targets points at [saved fp][return address] —
                   * the base of __builtin_frame_address/_return_address */
    IR_ALLOCA,    /* dst = a fresh 16-aligned block of `a` bytes on the
                   * stack, above the outgoing-argument area (a VLA) */
    IR_SPSAVE,    /* dst = the stack pointer */
    IR_SPRESTORE, /* stack pointer = a (releases every IR_ALLOCA since the
                   * IR_SPSAVE that produced a) */
    IR_LANDING,   /* a landing pad's entry (exception regions): dst = the
                   * exception pointer, b = the selector — what the unwinder
                   * left in rax/rdx (x0/x1) */

    /* ---- vectors ----------------------------------------------------
     *
     * One width, 128 bits, because that is what SSE2 and NEON both have
     * without asking: every x86-64 has SSE2 and every aarch64 has
     * Advanced SIMD, so a vector op needs no feature test and no
     * run-time dispatch. Wider (AVX) would.
     *
     * `size` is the ELEMENT width in bytes, so the lane count is
     * 16/size — a vector of four ints has size 4. A vector temp is 16
     * bytes and lives in a 16-byte slot, marked in the backend's `wide`
     * map exactly as a long double is, which also keeps it away from the
     * integer register allocator. */
    IR_VLOAD,  /* dst = the 16 bytes at *(temp a)      (size: element) */
    IR_VSTORE, /* the 16 bytes at *(temp a) = b        (size: element) */
    IR_VBIN,   /* dst = a OP b, lane by lane           (size: element;
                * OP in `imm`: '+' '-' '&' '|' '^', or '<' shl and '>'
                * shr, whose count is a CONSTANT in `c` and whose `sign`
                * picks arithmetic over logical. There is no '*': a
                * packed 32-bit multiply is SSE4.1, so the vectorizer
                * turns a multiply by a constant into shifts and adds
                * and refuses the rest.) */
    IR_VSPLAT, /* dst = every lane set to scalar a     (size: element) */
    IR_VREDADD,/* dst = the sum of a's lanes           (size: element;
                * w: the scalar result's width) */
    IR_VWIDEN, /* dst = half of a's lanes, each widened to twice its
                * size   (size: the SOURCE element width; `c`: 0 the low
                * half, 1 the high; `sign`: sign- rather than
                * zero-extend). Four int32 lanes become two int64 ones,
                * which is why it takes a half at a time -- and why a
                * widening sum needs two accumulators. */

    IR_OPCOUNT    /* not an opcode: the table size, so print and parse can
                   * agree on how many there are */
};

/* One resolved asm operand: an input carries the temp holding its VALUE, an
 * output the temp holding its lvalue ADDRESS; reg is the fixed register
 * (0-15 on x86-64, 0-30 on aarch64) the constraint pins it to. */
struct ir_asm_op {
    int temp;
    int reg;
    int size;
    int inout;   /* a "+" output: the register must hold the lvalue's
                  * CURRENT value when the asm starts, not just receive its
                  * new one. Set on aarch64; the x86 path leaves it 0. */
    int mem;     /* an "m" operand: the register holds the lvalue's ADDRESS
                  * and the template reads or writes through it. Nothing is
                  * loaded into it beforehand and nothing is stored out of
                  * it afterwards -- the asm IS the access. Without this an
                  * "=m" output had the register stored over what the
                  * template had just written there. */
};

struct ir_asm {
    const unsigned char *code;   /* assembled template bytes */
    int codelen;
    struct ir_asm_op *in;
    int nin;
    struct ir_asm_op *out;
    int nout;
};

struct ir_ins {
    enum ir_op op;
    /* Where this instruction came from (R3). `line` is the statement or
     * expression it lowers from; `col` is the column within it, 0 when
     * unknown. `synth` marks an instruction the compiler invented that
     * corresponds to no source construct at all -- a prologue store, a
     * landing pad's entry -- which is the exception §9.1 allows to the
     * verifier's "every instruction has a location" rule. */
    int line;
    int col;
    int synth;
    int dst, a, b;
    int c;                   /* IR_CMPXCHG: the third operand (desired value) */
    int w;                   /* 4 or 8: operation width class */
    int size;                /* 1/2/4/8: memory width for LD/ST/EXT */
    int sign;                /* signed variant of the op */
    int flt;                 /* operate in xmm at width w (SSE scalar) */
    int vol;                 /* LOAD/STORE/LDVAR/STVAR: a `volatile` access —
                              * the optimizer must never CSE or remove it (MMIO) */
    long imm;                /* IR_CONST; also the folded value when imm_b */
    int imm_b;               /* ADD/SUB/AND/OR/XOR/CMP: operand b is the constant
                              * in `imm` (an immediate), not vreg b — set by the
                              * optimizer's immediate-fold pass, read by codegen */
    enum binop pred;         /* IR_CMP */
    int label;               /* IR_LABEL/IR_JMP/IR_BRZ */
    struct func *callee;     /* IR_CALL (direct), IR_FADDR */
    /* The same target as an index into ir_unit::syms -- what a self-contained
     * IR refers to, and what its textual form prints (§9.1). The pointers
     * above remain while the type side is still being interned. */
    int callee_sym;
    int glob_sym;
    int indirect;            /* IR_CALL through a function pointer */
    int sret_first;          /* IR_CALL: argument 0 is the indirect-result
                              * pointer (type.h sret_first) */
    int call_varargs;        /* al = 0 needed at the call */
    int call_nfixed;         /* IR_CALL: how many NAMED parameters the
                              * callee has. Needed because Darwin's
                              * arm64 passes every argument past them on
                              * the stack, where AAPCS64 puts them in
                              * registers like any other -- so the split
                              * point, not merely the fact of variadicity,
                              * decides where an argument goes. */
    struct global *glob;     /* IR_GADDR */
    /* IR_CALL arguments. SysV splits the argument REGISTERS by class —
     * integers walk rdi..r9, floats walk xmm0..7, independently — and
     * an aggregate is either taken apart into eightbytes or copied to
     * the stack. Each argument therefore carries its own classification,
     * decided in irgen where the types still exist. */
    struct ir_arg {
        int vreg;            /* value, or the ADDRESS when is_struct */
        int is_struct;
        int size;            /* struct size, or the scalar's width */
        int nclass;          /* eightbyte count; 0 = MEMORY (stack) */
        enum arg_class cls[2];
        int on_stack;        /* no registers left (or MEMORY class) */
        int stk_off;         /* offset in the outgoing area */
        /* Everything the AAPCS64 placer needs about this argument's type,
         * computed at irgen where the type still exists (§9.1). The backend
         * reads these instead of walking `ty`. */
        int align;
        int is_float;
        int is_int128;
        int hfa_n, hfa_size;
        int byref;
        /* Microsoft x64: where the CALLER's private copy of a byref
         * aggregate lives, as an offset into the caller's scratch area.
         * The convention says the caller makes that copy because the
         * callee may write to it, so the copy is the caller's frame's
         * business and only its ADDRESS travels in the argument slot. */
        int copy_off;
        const struct type *ty; /* the argument's type: AAPCS64 decides an
                                * aggregate's placement from its MEMBERS
                                * (a Homogeneous Floating-point Aggregate
                                * travels in v registers), which the SysV
                                * classes above cannot express */
    } argv[MAX_PARAMS];
    int nargs;
    /* IR_CALL returning a struct: its size, classification, and the
     * caller-side scratch the result lands in. nclass 0 means MEMORY,
     * i.e. the hidden-pointer (sret) convention. */
    int retsize;
    /* The same, for the value a call returns. */
    int ret_hfa_n, ret_hfa_size;
    int ret_byref;
    const struct type *rety; /* IR_CALL returning a struct: its type (AAPCS64
                              * returns an HFA in v0..v3) */
    int retnclass;
    enum arg_class retcls[2];
    int ret_x87;             /* x86-64: the struct comes back in x87 registers
                              * (type.h ty_x87_ret): 1 one long double in st0,
                              * 2 a long double _Complex in st0/st1 */
    int scratch;             /* frame offset of the returned struct */
    struct ir_asm *asm_ir;   /* IR_ASM */
    int eh_region;           /* IR_CALL: 1 + the innermost exception region
                              * it is in (ir_func.eh), 0 if none */
};

/* An exception region (STMT_EHREGION): instructions [lo, hi) are its
 * body; a call among them that throws lands at lp_label. */
struct ir_eh {
    int parent;              /* the enclosing region, or -1 */
    int lo, hi;
    int lp_label;
    struct eh_act *acts;
    int nacts;
    int lp_off;              /* codegen: the landing pad's offset in the
                              * function's code */
};

/* codegen: a call's code in a function with exception regions — its
 * offsets in the function's code and its region (ir_ins.eh_region). */
struct ir_csite {
    int start, end;
    int region;
};

/* One line-table row: a .text offset (within this function) maps to a
 * source line. Collected by codegen only under -g; consumed by the DWARF
 * emitter, which brackets each function's rows with set_address/end_sequence
 * using the function's code_off/code_len (on struct func). */
struct ir_line { int off; int line; };

/* -g: a source-level variable (parameter or local). Its storage is the frame
 * slot of vreg `vreg`; irgen records name/vreg/type, codegen fills the slot's
 * rbp-relative offset into ir_func.var_off[vreg], and the DWARF emitter turns
 * the pair into DW_AT_location = DW_OP_fbreg(offset). Statics are excluded —
 * they are globals, not frame storage. */
/* `line`/`col` are where the variable was DECLARED (R3). A diagnostic or a
 * remark about a variable has to point at the variable, not at the function
 * that happens to contain it. */
struct ir_dbgvar {
    const char *name;
    int vreg;
    int is_param;
    struct type *ty;
    int line, col;
};

/* What EmbIR needs to know about one frame slot's type, decided at irgen
 * (§9.1). Everything the backends and the optimizer actually asked `struct
 * type` -- a size, an alignment, and four yes/no questions -- and nothing
 * more, so a textual form can carry it. */
struct ir_local {
    int size, align;
    int user_align;          /* __attribute__((aligned(N))); 0 = natural */
    int is_volatile;
    int is_ldouble;          /* x86-64: lives in x87, not an SSE register */
    int is_int128;
    int is_int_or_ptr;       /* an integer or a pointer, any width */
    int is_scalar_int_or_ptr; /* ... and 4 or 8 bytes: mem2reg's test */
    int is_scalar_float;     /* a float or double (NOT long double, which
                              * lives in x87 and is 16 bytes here) */
};

struct ir_func {
    /* EmbIR is meant to be a module, not a view over the AST (§9.1): what a
     * pass or a backend needs about the function is HERE, copied at irgen
     * time, so nothing downstream has to follow a pointer back into the
     * parse tree. `src` remains for what genuinely still lives there --
     * code_off/code_len, which the linker writes back, and the types the
     * ABI classification has not yet absorbed -- and every use of it is a
     * remaining step toward a self-contained IR. */
    const char *name;
    const char *file;
    int line;
    int is_static;
    int is_varargs;
    int nparams;
    int nvars;

    /* This function's OWN parameters, classified at irgen exactly as a
     * call's arguments are, so the prologue places them without consulting
     * the AST either (§9.1). Length nparams; the vreg field is the
     * parameter's slot. */
    struct ir_arg *param_abi;

    /* One per frame slot, length nvars. The inliner extends this alongside
     * the AST's var_tys -- both must grow together, which is the failure
     * that made `nvars` a split brain the first time. */
    struct ir_local *locals;
    /* The function's own return type, classified as a call's is. */
    struct ir_arg ret_abi;

    struct func *src;        /* code_off/len; the types not yet interned */
    int nvregs;
    int nlabels;
    int scratch_bytes;       /* struct-return temporaries */
    int outgoing_bytes;      /* widest stack-argument area of any call */
    int has_i128;            /* computes with __int128 (w 16, not float):
                              * the optimizer and inliner leave it alone */
    int has_alloca;          /* an IR_ALLOCA moves the stack pointer at run
                              * time, so the frame must not be addressed
                              * from it (aarch64 then uses x19) */
    struct ir_ins *ins;
    int nins, cap;
    struct ir_line *lines;   /* -g: (offset, line) rows in .text order */
    int nlines, linecap;
    struct ir_dbgvar *dbgvars; /* -g: params + locals (irgen) */
    int ndbgvars, dbgvarcap;
    int *var_off;            /* -g: rbp-relative slot offset per vreg (codegen) */
    /* Per-LOCAL lexical scope, as a half-open instruction range [lo, hi) (irgen).
     * Two locals whose scopes are disjoint never coexist — a stack pointer used
     * past its scope is UB — so codegen may give them one stack slot. Params and
     * function-level locals span the whole function; only nested-block locals get
     * a narrower range. Length nvars; unused (NULL) when there are no locals. */
    int *var_scope_lo, *var_scope_hi;
    /* exception regions (C++'s lowering), the catch types their landing
     * pads' selectors number (1-based; NULL: catch-all), and — codegen —
     * every call's code */
    struct ir_eh *eh;
    int neh;
    struct global **eh_types;
    int neh_types;
    struct ir_csite *csites;
    int ncsites, capcsites;
};

/* One .rodata string; offsets are assigned sequentially at collection
 * time and become section offsets verbatim in the driver. */
struct ir_str {
    const char *bytes;
    int len;                 /* including the terminating NUL */
    int off;                 /* offset inside .rodata */
};

/* A symbol EmbIR refers to, by NAME rather than by a pointer into the AST
 * (§9.1). The fields are exactly what the backends read off a callee or a
 * global -- nothing speculative -- so the table stays small and a textual
 * form can carry all of it. */
struct ir_sym {
    const char *name;
    int is_func;
    int defined;       /* a definition exists in this unit */
    int is_weak;
    int is_varargs;    /* functions */
    int sret_first;    /* functions: argument 0 is the indirect result */
    int is_nothrow;    /* functions: no exception leaves it */
};

struct ir_unit {
    struct unit *src;
    struct ir_sym *syms;     /* every name the IR mentions */
    int nsyms, capsyms;
    struct ir_func *funcs;   /* array, same order as src->funcs */
    int nfuncs;
    struct ir_str *strs;
    int nstrs, capstrs;
    int rodata_len;
};

/* Intern a symbol, returning its index. Interning by name means the same
 * function referred to from two instructions is one entry, which is what
 * lets a parsed IR resolve a name without a frontend symbol table. */
void ir_locals_fill(struct ir_func *fn, struct func *f, int nvars);
int ir_sym_func(struct ir_unit *u, struct func *f);
int ir_sym_global(struct ir_unit *u, struct global *g);

struct ir_unit *irgen(struct unit *u);

/* EmbIR's textual form (src/ir/irprint.c, vision §18) — what
 * `embcc --inspect=ir` prints. Print only: see that file's head for why the
 * round-trip half of §9.1 is a structural change, not a printer feature. */
struct outbuf;
void ir_print_unit(struct outbuf *b, const struct ir_unit *u);
void ir_print_func(struct outbuf *b, const struct ir_func *f);
/* An opcode's mnemonic, so a diagnostic can name the instruction. */
const char *ir_opname(enum ir_op op);
/* The inverse, for the parser (src/ir/irparse.c): -1 when unknown. */
int ir_op_from_name(const char *n);
/* Read EmbIR back from its textual form (src/ir/irparse.c). `text` is
 * modified in place. The unit's `src` is NULL: a parsed IR can be printed,
 * analysed and transformed, but not handed to the DWARF emitter. */
struct ir_unit *ir_parse(const char *file, char *text);
int ir_pred_from_name(const char *n);

/* codegen: record a call's code (offsets in the function's code) in a
 * function with exception regions. */
void ir_add_csite(struct ir_func *fn, int start, int end, int region);

/* Intern a string into the unit's .rodata pool (used by the driver to
 * place a global initializer's string targets). Returns its index; the
 * offset is iu->strs[index].off. */
int ir_intern_string(struct ir_unit *iu, const char *bytes, int len);

#endif
