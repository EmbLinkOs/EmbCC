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
    /* dst = the BITS of a, reinterpreted at the same width.
     * size and w are both that width; sign is 1 when the destination is
     * an INTEGER (float bits out) and 0 when it is a float (bits in).
     *
     * This is one op rather than a builtin-per-operation because every
     * IEEE-754 predicate is integer arithmetic once the bits are in a
     * GPR: fabs is an AND, copysign an AND/OR pair, signbit a shift,
     * isnan and isinf comparisons against the exponent field. gcc and
     * clang each carry a separate optab for all of them; here they are
     * ordinary IR the existing optimizer already folds, and only the
     * move between the register files is target code. On the soft-float
     * targets (ARMv7-M, RISC-V) even that is a plain move, because the
     * float was already in a GPR. */
    IR_BITCAST,
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
               * template, store outputs. Detail in ir_ins.asm_ir; dst is
               * its `val` output's value, or -1 */
    IR_LABELADDR, /* dst = &&label  (GNU label address; id in `label`) */
    IR_IGOTO, /* goto *a  (GNU computed goto: jump to the address in temp a) */
    IR_SWITCH, /* a dense switch as ONE multi-way terminator: if (unsigned)a
                * < jt[jt].n then goto jt[jt].labels[a], else goto label.
                * `w` is a's width (4 or 8); a is the case value with the
                * lowest case subtracted, so the test is unsigned by
                * construction. Every target is named, which is what lets
                * the CFG, liveness and the allocator see through it where
                * IR_IGOTO's unknown targets make them step aside. */
    IR_ARMW,  /* dst = *(temp a); *(temp a) = dst OP b   (atomic; size, w).
               * OP is in `imm`: '&' '|' '^', or 'n' for nand = ~(dst & b);
               * on AVR only, 'L' for an atomic load wider than a byte:
               * dst = *a, and nothing stored.
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
    IR_FRAMEADDR, /* imm 0: dst = this function's frame pointer (rbp /
                   * x29 / a6), which points at [saved fp][return address]
                   * — the base of __builtin_frame_address/_return_address.
                   * Where there is no such chain (target_has_frame_chain),
                   * level 0 only: imm 1, the frame address (sp at entry);
                   * imm 2, the return address the function was entered
                   * with */
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
    IR_SELECT, /* dst = a ? b : c      (w, sign: the arms; size: a's)
                * `size` is the CONDITION's width, 4 or 8, which is not
                * the arms': `long c; c ? i : j` picks between ints on a
                * test of all 64 bits, and `(int)l ? p : q` between
                * pointers on a test of 32. The branch the select replaced
                * carried it as its `w`; tested at the arms' width
                * instead, a condition of 0x100000000 was false.
                * Both targets have this without a branch -- cmov on
                * x86-64, csel on aarch64 -- and neither emitted it. The
                * arms are VALUES, already computed: a select evaluates
                * both, so the pass that builds one must refuse anything
                * that could fault or have an effect. */
    IR_VWIDEN, /* dst = half of a's lanes, each widened to twice its
                * size   (size: the SOURCE element width; `c`: 0 the low
                * half, 1 the high; `sign`: sign- rather than
                * zero-extend). Four int32 lanes become two int64 ones,
                * which is why it takes a half at a time -- and why a
                * widening sum needs two accumulators. */

    /* ---- the 32x32->64 multiply, for the 32-bit machines -----------
     *
     * Both read their operands as 32-bit values, whatever `w` says, and
     * `sign` says how: 1 signed by signed, 0 unsigned by unsigned. They
     * exist because the IR's only multiply keeps the low half at its
     * width, so a 32-bit machine that HAS a widening multiply (umull,
     * mulhu, multu, mulhwu, umul and %y...) had to be handed a 64-bit
     * multiply of two extended values -- four multiplies on RV32 -- or,
     * for a division by a constant, nothing at all. Emitted only where
     * target_has_mulh() says the backend lowers them; the folders take
     * them by the same definition (fold_bin). */
    IR_MULH,   /* dst = the HIGH 32 bits of the 64-bit product a * b
                * (w 4, sign) -- mulh/mulhu, smull/umull's high register */
    IR_MULW,   /* dst = the whole 64-bit product of 32-bit a and b
                * (w 8, sign) -- mul + mulh, smull/umull, mult/multu */

    IR_OPCOUNT   /* not an opcode: the table size, so print and parse can
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
    int val;     /* an output whose VALUE is the instruction's dst, as a
                  * call's result is: the asm leaves it in reg, and the
                  * code generator moves it to dst's home. temp is -1.
                  * irgen stores dst to the lvalue afterwards, so a local
                  * written by an asm is no longer address-taken and can
                  * live in a register. At most one per instruction; see
                  * `cont`. Thumb only (ra_target.asm_in_reg). */
};

struct ir_asm {
    const unsigned char *code;   /* assembled template bytes */
    int codelen;
    struct ir_asm_op *in;
    int nin;
    struct ir_asm_op *out;
    int nout;
    /* A continuation: no bytes, no inputs, and one `val` output -- a
     * further value output of the asm just before it, which can have
     * only one dst. It follows that asm immediately (the code generator
     * refuses one that does not), and the asm's own lowering writes it;
     * the continuation itself emits nothing. */
    int cont;
    /* What the asm may change, bit r for register r: its operands'
     * registers, the clobber list, the registers the template names, the
     * scratch its lowering uses (`scr`), and everything a call changes if
     * the template calls. A value live across the asm keeps out of these
     * and only these (regalloc.c, asm_in_reg); 0 means unknown, and the
     * asm is then treated as a call. */
    unsigned long clob;
    int scr;     /* the scratch the lowering stores outputs through, or -1 */
    /* The template calls -- or otherwise writes the link register -- so
     * the function holding it is not a leaf: PowerPC's LR must be saved
     * around it (ppc/irgen.c). 0 elsewhere. */
    int calls;
};

/* A jump table: the targets of one IR_SWITCH, for index values 0..n-1; a
 * value with no case names the default. Owned by the function and referred
 * to by index, so a copied switch shares its table -- which is what every
 * copy of one wants, since a pass that retargets labels rewrites the table
 * once for all of them. */
struct ir_jt { int n; int *labels; };

/* An atomic read-modify-write's memory order (ir_ins.mo). Consume is
 * acquire; seq_cst is 0, so an instruction built without one is the
 * strongest. RISC-V maps them to .aq and .rl as clang does. */
enum {
    IR_MO_SEQ_CST = 0, IR_MO_RELAXED, IR_MO_ACQUIRE, IR_MO_RELEASE,
    IR_MO_ACQ_REL
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
    int mo;                  /* IR_XCHG/XADD/ARMW/CAS/CMPXCHG: the memory
                              * order, IR_MO_*. 0 -- what every hand-built
                              * one has -- is seq_cst, the strongest; a
                              * backend may always treat any as seq_cst */
    int w;                   /* 4 or 8: operation width class */
    int size;                /* 1/2/4/8: memory width for LD/ST/EXT */
    int sign;                /* signed variant of the op */
    int flt;                 /* operate in xmm at width w (SSE scalar) */
    int vol;                 /* LOAD/STORE/LDVAR/STVAR: a `volatile` access —
                              * the optimizer must never CSE or remove it (MMIO) */
    int flash;               /* LOAD: from AVR program memory (__flash), read
                              * with LPM. Set with vol, so no pass folds it into
                              * an ordinary load or a memcpy (irgen emit_load) */
    long imm;                /* IR_CONST; also the folded value when imm_b */
    int imm_b;               /* ADD/SUB/AND/OR/XOR/CMP: operand b is the constant
                              * in `imm` (an immediate), not vreg b — set by the
                              * optimizer's immediate-fold pass, read by codegen */
    enum binop pred;         /* IR_CMP */
    int label;               /* IR_LABEL/IR_JMP/IR_BRZ; IR_SWITCH's default */
    int jt;                  /* IR_SWITCH: index into ir_func::jt */
    struct func *callee;     /* IR_CALL (direct), IR_FADDR */
    /* The same target as an index into ir_unit::syms -- what a self-contained
     * IR refers to, and what its textual form prints (§9.1). The pointers
     * above remain while the type side is still being interned. */
    int callee_sym;
    int glob_sym;
    int indirect;            /* IR_CALL through a function pointer */
    int sret_first;          /* IR_CALL: argument 0 is the indirect-result
                              * pointer (type.h sret_first) */
    int sret_size;           /* ...and the size of the object it points at
                              * (SPARC's unimp after the call), or 0 */
    int call_varargs;        /* al = 0 needed at the call */
    int memoff;              /* IR_LOAD/IR_STORE: a constant byte offset
                              * added to the address -- set only by a
                              * backend's own pre-codegen pass
                              * (ra_fold_memoff), never by irgen or the
                              * optimizer, so every other backend sees 0 */
    int natural;             /* IR_LOAD/IR_STORE: the address is aligned
                              * to the access, because C guarantees it
                              * there (a dereference, a global, a member
                              * at its natural offset). 0 is "not known":
                              * a packed struct's member, or anything
                              * irgen did not say -- a backend whose
                              * aligned-only instructions (vldr) fault on
                              * a misaligned address must not use them. */
    int call_pcs;            /* IR_CALL: the callee's pcs attribute (ARM;
                              * see target_pcs_vfp) */
    int call_cmse;           /* IR_CALL, indirect: through a pointer to a
                              * cmse_nonsecure_call function type (-mcmse):
                              * the registers and flags are cleared and the
                              * branch is a BLXNS (src/arch/thumb) */
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
        int nat_align;       /* ty_natural_align; 0 when not filled: align */
        /* A struct argument: its address (vreg) is aligned to its type,
         * because C promises it there -- 0 for a packed struct's member,
         * and for anything irgen did not say. A backend whose aligned-only
         * loads fault (Xtensa) reads a 0 one a byte at a time. */
        int natural;
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
    } *argv;                 /* IR_CALL: nargs of them, out of line. They
                              * were MAX_PARAMS inline in EVERY instruction
                              * -- 2.6 KB of an ir_ins's 2.8 -- and each pass
                              * that rebuilds a function copies all of it.
                              * A call's array is its own: an instruction
                              * copied while the original stays (the
                              * inliner) takes ir_args_copy, and the verifier
                              * refuses two live calls sharing one. */
    int nargs;
    /* IR_CALL returning a struct: its size, classification, and the
     * caller-side scratch the result lands in. nclass 0 means MEMORY,
     * i.e. the hidden-pointer (sret) convention. */
    int retsize;
    /* A SCALAR return's own type, which `w` above deliberately does not
     * describe: it reports the return REGISTER's width, because on the
     * four register-per-value machines that is what the callee leaves
     * behind. AVR's return value is a RUN of byte registers sized by the
     * type -- r24 alone for a char, r25:r24 for an int -- and the ABI
     * leaves everything above it undefined, so the CALLER must extend.
     * It cannot: `signed char sc(void)` yields `ext.4:2s` at the use,
     * which reads a second byte the callee never wrote.
     *
     * avr-gcc's callers extend for themselves at exactly this point, so
     * this is the convention and not a shortcoming to route around. The
     * size and signedness therefore travel with the call, from irgen
     * where the type still exists (§9.1). Zero size means "no scalar
     * result" -- a void call, or a struct, which retsize describes. */
    int ret_tybytes, ret_tysign;
    /* TriCore: the call's result is a POINTER, which comes back in the
     * address register A2 rather than in D2 (irgen sets it there only). */
    int ret_ptr;
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
/* -g: one step of a function's prologue, as call frame information
 * (src/debug/dwarf.c writes .debug_frame from these). `off` is the byte
 * offset from the function's start where the step has taken effect. */
enum {
    IR_CFI_CFA_OFFSET,   /* the CFA is now the CFA register + val */
    IR_CFI_CFA_REG,      /* the CFA is now DWARF register `reg` + val */
    IR_CFI_SAVED         /* DWARF register `reg` is saved at CFA + val */
};
struct ir_cfi { int off, kind, reg; long val; };

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
    /* mem2reg took this variable out of memory although something
     * assigned it: its slot no longer follows it. Its only reader is a
     * backend deciding whether the slot is a true DW_AT_location -- for a
     * parameter, whose slot the prologue still writes, that is the one
     * thing that says the value there went stale. */
    int moved;
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
    int pcs;                 /* its own pcs attribute (ARM) */
    int cmse_entry;          /* cmse_nonsecure_entry (-mcmse; struct func) */

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
    struct ir_jt *jt;        /* the jump tables IR_SWITCH refers to */
    int njt, jtcap;
    struct ir_line *lines;   /* -g: (offset, line) rows in .text order */
    int nlines, linecap;
    struct ir_cfi *cfi;      /* -g: the prologue's steps (ir_cfi_add) */
    int ncfi, cficap;
    struct ir_dbgvar *dbgvars; /* -g: params + locals (irgen) */
    int ndbgvars, dbgvarcap;
    int *var_off;            /* -g: rbp-relative slot offset per vreg (codegen) */
/* A var_off for a variable with NO location: its slot is never written in
 * the code the function became (the optimizer kept the value in a
 * temporary). The DIE then says so with an empty location -- a debugger
 * prints <optimized out> -- rather than naming a slot that holds whatever
 * was there before. */
#define IR_VAR_NO_LOC (-0x7fffffff)
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
    int align;               /* what `off` is a multiple of (0: nothing
                              * asked); .rodata itself is 16-aligned */
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

/* -fsanitize, in TRAP mode -- the only mode there can be here, because
 * a diagnosing sanitizer needs a runtime (__ubsan_handle_*) and a bare
 * metal target has nowhere to print. A failed check executes the
 * target's trap instruction, which IR_UD2 already lowers on all four
 * backends: `ud2` on x86-64, `udf #0` on aarch64 and ARMv7-M, and
 * `unimp` on RISC-V. Each is an illegal encoding, so the program takes
 * an exception at the offending operation -- a breakpoint under a
 * debugger, and a stop rather than a wrong value without one, which is
 * the point on a board. */
enum { SAN_OVERFLOW = 1, SAN_DIVIDE = 2, SAN_SHIFT = 4 };
void irgen_set_sanitize(unsigned mask);
/* -finstrument-functions, with GCC's two exclusion lists (comma-separated;
 * either may be NULL) */
void irgen_set_instrument(int on, const char *funcs, const char *files);
void irgen_set_opt_size(int on);       /* -Os: a switch table must be denser */
/* A new table of n entries (all -1) in fn; its index. */
int ir_jt_add(struct ir_func *fn, int n);
/* A copy of src's table `jt` in dst, every label moved up by lbase (the
 * inliner's renumbering); its index. dst and src may be one function. */
int ir_jt_clone(struct ir_func *dst, const struct ir_func *src, int jt, int lbase);
unsigned irgen_sanitize(void);

/* EmbIR's textual form (src/ir/irprint.c, vision §18) — what
 * `embcc --inspect=ir` prints. Print only: see that file's head for why the
 * round-trip half of §9.1 is a structural change, not a printer feature. */
struct outbuf;
void ir_print_unit(struct outbuf *b, const struct ir_unit *u);
void ir_print_func(struct outbuf *b, const struct ir_func *f);
/* An opcode's mnemonic, so a diagnostic can name the instruction. */
const char *ir_opname(enum ir_op op);
/* A call's argument array of its own, with n entries (at least one) */
struct ir_arg *ir_args_new(int n);
struct ir_arg *ir_args_copy(const struct ir_arg *a, int n);
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
/* ...at an offset that is a multiple of `align` (a power of two, <= 16). */
int ir_intern_aligned(struct ir_unit *iu, const char *bytes, int len,
                      int align);

/* Append one prologue step to fn's call frame information (-g). A
 * backend records its prologue once the function is final; ncfi = 0
 * starts it again. */
void ir_cfi_add(struct ir_func *fn, int off, int kind, int reg, long val);

#endif
