#include "emit.h"

#include <stdio.h>
#include <stdlib.h>

#include "../../driver/util.h"
#include "../target.h"

/* REX.W when the 64-bit form is wanted */
static void rexw(struct code *c, int w)
{
    if (w == 8)
        code_byte(c, 0x48);
}

/* ModRM for [rbp+disp]: rm=101 with mod=01 (disp8) or mod=10 (disp32). */
/* A slot nothing should touch: a local the allocator put in a register,
 * a temp that appears nowhere, a value with an FP home. layout_frame
 * gives each this displacement, and every rbp-relative access goes
 * through here -- so a lowering path that does not know the value has
 * moved fails loudly instead of reading whatever the frame holds. */
#define X86_DEAD_SLOT (-0x40000000)
static void no_dead_slot(int disp)
{
    if (disp == X86_DEAD_SLOT)
        internal_error("a value was read from a stack slot it does not have "
                       "-- it lives in a register, and some lowering path "
                       "does not know that");
}

static void modrm_rbp(struct code *c, int reg, int disp)
{
    no_dead_slot(disp);
    if (disp >= -128 && disp <= 127) {
        code_byte(c, 0x45 | (reg << 3));
        code_byte(c, disp & 0xff);
    } else {
        code_byte(c, 0x85 | (reg << 3));
        code_u32(c, (unsigned long)(unsigned int)disp);
    }
}

/* ModRM for [base+disp] with an arbitrary base register. rsp needs a
 * SIB byte; rbp cannot use the disp-less form (that encoding means
 * RIP-relative), so both take an explicit displacement. */
static void modrm_base(struct code *c, int reg, int base, int disp)
{
    if (base == 5) no_dead_slot(disp);        /* rbp: a frame access */
    int rm = base & 7;
    int mod;

    if (disp >= -128 && disp <= 127)
        mod = 1;
    else
        mod = 2;
    code_byte(c, (mod << 6) | ((reg & 7) << 3) | rm);
    if (rm == 4)
        code_byte(c, 0x24); /* SIB: base=rsp, no index */
    if (mod == 1)
        code_byte(c, disp & 0xff);
    else
        code_u32(c, (unsigned long)(unsigned int)disp);
}

/* REX for a (reg, base) pair; emitted when 64-bit or when either
 * register is r8..r15 (none are used here, but the bits are correct). */
static void rex_rb(struct code *c, int w64, int reg, int base)
{
    int rex = 0x40 | (w64 ? 8 : 0) | ((reg & 8) ? 4 : 0) |
              ((base & 8) ? 1 : 0);
    if (rex != 0x40)
        code_byte(c, rex);
}

/* REX for a form that names an 8-BIT REGISTER.
 *
 * Register numbers 4..7 in a byte operand mean %ah %ch %dh %bh unless a
 * REX prefix is present, and %spl %bpl %sil %dil when one is -- so a
 * byte form with such an operand needs REX even when it carries no
 * bits. It never came up while the integer pool was {r8..r15, rbx,
 * rdx}: all of those are either REX-extended already or directly
 * addressable as al/bl/cl/dl. It came up the moment an ABI hint put a
 * `char` PARAMETER in rsi -- `movzbl %sil,%ebx` assembled as
 * `movzbl %dh,%ebx`, and lib/libcxx's read_encoded then dispatched
 * every DWARF encoding as absptr, so a throw never found its handler
 * (tests/golden/unwind.sh, libcxx.sh, libcxx-std.sh).
 *
 * `byte_reg` and `byte_rm` say which of the two operands is the
 * 8-bit one. */
static void rex_rb8(struct code *c, int w64, int reg, int base,
                    int byte_reg, int byte_rm)
{
    int rex = 0x40 | (w64 ? 8 : 0) | ((reg & 8) ? 4 : 0) |
              ((base & 8) ? 1 : 0);
    if (rex != 0x40 || (byte_reg && reg >= 4) || (byte_rm && base >= 4))
        code_byte(c, rex);
}

/* An x87 memory instruction: `opcode /ext` against [base+disp] — fld/fstp
 * of a tword (DB /5, /7), qword (DD /0, /3) or dword (D9 /0, /3), fild
 * qword (DF /5), fistp qword (DF /7), fnstcw/fldcw (D9 /7, /5). */
void x86_x87_mem(struct code *c, int opcode, int ext, int base, int disp)
{
    rex_rb(c, 0, 0, base);
    code_byte(c, opcode);
    modrm_base(c, ext, base, disp);
}

/* A two-byte instruction with no operand encoding to do (faddp st1,
 * fchs, fucomip, ...). */
void x86_op2(struct code *c, int b1, int b2)
{
    code_byte(c, b1);
    code_byte(c, b2);
}

void x86_load_reg_mem(struct code *c, int dst, int base, int disp,
                      int size)
{
    switch (size) {
    case 1:
        rex_rb(c, 0, dst, base);
        code_byte(c, 0x0f);
        code_byte(c, 0xb6); /* movzx r32, r/m8 */
        break;
    case 2:
        rex_rb(c, 0, dst, base);
        code_byte(c, 0x0f);
        code_byte(c, 0xb7); /* movzx r32, r/m16 */
        break;
    case 4:
        rex_rb(c, 0, dst, base);
        code_byte(c, 0x8b);
        break;
    case 8:
        rex_rb(c, 1, dst, base);
        code_byte(c, 0x8b);
        break;
    default:
        internal_error("bad load size %d", size);
    }
    modrm_base(c, dst, base, disp);
}

void x86_store_mem_reg(struct code *c, int base, int disp, int src,
                       int size)
{
    switch (size) {
    case 1:
        rex_rb8(c, 0, src, base, 1, 0);
        code_byte(c, 0x88);
        break;
    case 2:
        code_byte(c, 0x66);
        rex_rb(c, 0, src, base);
        code_byte(c, 0x89);
        break;
    case 4:
        rex_rb(c, 0, src, base);
        code_byte(c, 0x89);
        break;
    case 8:
        rex_rb(c, 1, src, base);
        code_byte(c, 0x89);
        break;
    default:
        internal_error("bad store size %d", size);
    }
    modrm_base(c, src, base, disp);
}

void x86_movs_load_base(struct code *c, int xmm, int base, int disp,
                        int w)
{
    code_byte(c, w == 4 ? 0xf3 : 0xf2);
    /* modrm_base masks both fields to three bits and leaves the fourth
     * to REX, which nothing here used to need: every caller named an
     * xmm below 8 and a base below 8. The FP pool is xmm8-15. */
    if (xmm >= 8 || base >= 8)
        code_byte(c, 0x40 | ((xmm >= 8) << 2) | (base >= 8));
    code_byte(c, 0x0f);
    code_byte(c, 0x10);
    modrm_base(c, xmm, base, disp);
}

void x86_movs_store_base(struct code *c, int base, int disp, int xmm,
                         int w)
{
    code_byte(c, w == 4 ? 0xf3 : 0xf2);
    if (xmm >= 8 || base >= 8)
        code_byte(c, 0x40 | ((xmm >= 8) << 2) | (base >= 8));
    code_byte(c, 0x0f);
    code_byte(c, 0x11);
    modrm_base(c, xmm, base, disp);
}

void x86_lea_reg_slot(struct code *c, int dst, int disp)
{
    rex_rb(c, 1, dst, REG_RBP);
    code_byte(c, 0x8d);
    modrm_base(c, dst, REG_RBP, disp);
}

void x86_mov_reg_reg(struct code *c, int dst, int src)
{
    rex_rb(c, 1, src, dst);
    code_byte(c, 0x89); /* mov r/m64, r64 */
    code_byte(c, 0xc0 | ((src & 7) << 3) | (dst & 7));
}

/* dst = src, w-bit. w==4 leaves a 32-bit mov (zeroing the upper half — the
 * register allocator's "narrow value is zero-extended" invariant); w==8 is a
 * full 64-bit copy. */
void x86_mov_rr_w(struct code *c, int dst, int src, int w)
{
    rex_rb(c, w == 8, src, dst);
    code_byte(c, 0x89); /* mov r/m, r : reg=src, rm=dst */
    code_byte(c, 0xc0 | ((src & 7) << 3) | (dst & 7));
}

/* dst64 = sign-extend(src's low 32 bits) — movsxd. */
void x86_movsxd_rr(struct code *c, int dst, int src)
{
    rex_rb(c, 1, dst, src);
    code_byte(c, 0x63); /* movsxd reg, r/m32 : reg=dst, rm=src */
    code_byte(c, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/* dst = extend(the low `size` (1 or 2) bytes of src) to width w — movsx/movzx,
 * the reg-reg twin of x86_load_slot's narrow cases. */
void x86_movx_rr(struct code *c, int dst, int src, int size, int sign, int w)
{
    rex_rb8(c, w == 8, dst, src, 0, size == 1);
    code_byte(c, 0x0f);
    if (size == 1)
        code_byte(c, sign ? 0xbe : 0xb6); /* movsx/movzx r, r/m8 */
    else
        code_byte(c, sign ? 0xbf : 0xb7); /* movsx/movzx r, r/m16 */
    code_byte(c, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/* dst op= src (+ - * & | ^), w-bit — the reg-reg twin of x86_alu_eax_mem, same
 * "r, r/m" opcodes with reg=dst, rm=src. */
void x86_alu_rr(struct code *c, int op, int dst, int src, int w)
{
    rex_rb(c, w == 8, dst, src);
    switch (op) {
    case '+': code_byte(c, 0x03); break;
    case '-': code_byte(c, 0x2b); break;
    case '*': code_byte(c, 0x0f); code_byte(c, 0xaf); break;
    case '&': code_byte(c, 0x23); break;
    case '|': code_byte(c, 0x0b); break;
    case '^': code_byte(c, 0x33); break;
    default:
        internal_error("no reg-reg encoding for '%c'", op);
    }
    code_byte(c, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/* group-1 ALU `reg OP= imm` (add/sub/and/or/xor, and cmp via op 'c'): the imm8
 * form (83 /ext ib, sign-extended) when the value fits, else imm32 (81 /ext id).
 * Works for any register including rax — shorter than materialising the constant
 * in a scratch register first. */
void x86_alu_reg_imm(struct code *c, int op, int reg, long imm, int w)
{
    int ext;
    switch (op) {
    case '+': ext = 0; break;
    case '|': ext = 1; break;
    case '&': ext = 4; break;
    case '-': ext = 5; break;
    case '^': ext = 6; break;
    case 'c': ext = 7; break;   /* cmp */
    default:
        internal_error("no reg-imm encoding for '%c'", op);
    }
    rex_rb(c, w == 8, 0, reg);   /* reg is the r/m operand -> REX.B */
    if (imm >= -128 && imm <= 127) {
        code_byte(c, 0x83);
        code_byte(c, 0xc0 | (ext << 3) | (reg & 7));
        code_byte(c, (int)(imm & 0xff));
    } else {
        code_byte(c, 0x81);
        code_byte(c, 0xc0 | (ext << 3) | (reg & 7));
        code_u32(c, (unsigned long)imm);
    }
}

/* imul dst, src, imm — the three-operand form: dst = src * imm, so dst need not
 * equal src and nothing routes through rax. imm8 (6b) when it fits, else imm32
 * (69). */
void x86_imul_reg_imm(struct code *c, int dst, int src, long imm, int w)
{
    rex_rb(c, w == 8, dst, src);   /* dst -> REX.R, src -> REX.B */
    if (imm >= -128 && imm <= 127) {
        code_byte(c, 0x6b);
        code_byte(c, 0xc0 | ((dst & 7) << 3) | (src & 7));
        code_byte(c, (int)(imm & 0xff));
    } else {
        code_byte(c, 0x69);
        code_byte(c, 0xc0 | ((dst & 7) << 3) | (src & 7));
        code_u32(c, (unsigned long)imm);
    }
}

/* test reg, reg — ZF/SF from the value itself, the compact `cmp reg, 0`. */
void x86_test_reg(struct code *c, int reg, int w)
{
    rex_rb(c, w == 8, reg, reg);
    code_byte(c, 0x85);
    code_byte(c, 0xc0 | ((reg & 7) << 3) | (reg & 7));
}

/* cmp a, b (computes a - b, sets flags) — reg-reg twin of x86_cmp_eax_mem. */
void x86_cmp_rr(struct code *c, int a, int b, int w)
{
    rex_rb(c, w == 8, a, b);
    code_byte(c, 0x3b); /* cmp r, r/m : reg=a, rm=b */
    code_byte(c, 0xc0 | ((a & 7) << 3) | (b & 7));
}

/* [rdx:rax] / src -> quotient rax, remainder rdx (idiv /7, div /6). */
void x86_div_rr(struct code *c, int src, int sign, int w)
{
    rex_rb(c, w == 8, 0, src);
    code_byte(c, 0xf7);
    code_byte(c, 0xc0 | ((sign ? 7 : 6) << 3) | (src & 7));
}

/* The integer argument registers, in order, for whichever convention
 * this target uses. Microsoft x64 has FOUR and starts with rcx;
 * System V has six and starts with rdi. x86_nargregs() is the limit
 * that goes with the table, so a caller cannot use one without the
 * other. */
int x86_argreg(int index)
{
    static const int sysv[6] = { REG_RDI, REG_RSI, REG_RDX, REG_RCX,
                                 8 /* r8 */, 9 /* r9 */ };
    static const int win64[4] = { REG_RCX, REG_RDX, 8 /* r8 */, 9 /* r9 */ };
    if (target_win64_abi())
        return index >= 0 && index < 4 ? win64[index] : win64[3];
    return index >= 0 && index < 6 ? sysv[index] : sysv[5];
}

int x86_nargregs(void)
{
    return target_win64_abi() ? 4 : 6;
}

/* Where the first argument that did not fit a register sits, measured
 * from rbp after the standard prologue. System V puts it straight
 * above the return address; Microsoft x64 leaves 32 bytes of shadow
 * space there for the callee to spill its four register arguments
 * into, so the first stack argument is four words further up. */
int x86_stack_arg_base(void)
{
    return target_win64_abi() ? 48 : 16;
}

void x86_prologue(struct code *c, int framesize, int frameless)
{
    if (framesize % 16 != 0) {
        internal_error("frame size %d not 16-aligned", framesize);
    }
    if (frameless) {
        /* A leaf with nothing in its frame: no record to push, and rsp
         * never moves, so the caller's CFA rule holds throughout. */
        if (framesize != 0)
            internal_error("frameless function wants a %d-byte frame",
                           framesize);
        return;
    }
    code_byte(c, 0x55);                     /* push rbp */
    code_byte(c, 0x48); code_byte(c, 0x89); /* mov rbp, rsp */
    code_byte(c, 0xe5);
    if (framesize > 0) {
        code_byte(c, 0x48); code_byte(c, 0x81); /* sub rsp, imm32 */
        code_byte(c, 0xec);
        code_u32(c, (unsigned long)framesize);
    }
}

void x86_epilogue(struct code *c, int frameless)
{
    if (!frameless)
        code_byte(c, 0xc9); /* leave */
    code_byte(c, 0xc3); /* ret */
}

/* The frame teardown without the return: what a SIBLING call needs.
 * After this, rsp points at the return address the caller pushed, so a
 * plain `jmp` to the callee makes it return straight to that caller. */
void x86_leave(struct code *c)
{
    code_byte(c, 0xc9); /* leave */
}

/* These two used to carry a SECOND copy of the argument register
 * order, as a table of {reg-field, needs REX.R}. That was one table
 * too many the moment a target existed with a different order: changing
 * x86_argreg alone left every call site still loading rdi and rsi.
 * They derive both from x86_argreg now, so there is one answer. */
void x86_store_arg(struct code *c, int argno, int disp)
{
    int r = x86_argreg(argno);
    code_byte(c, (r & 8) ? 0x4c : 0x48);   /* REX.W (+REX.R) */
    code_byte(c, 0x89); /* mov r/m64, r64 */
    modrm_rbp(c, r & 7, disp);
}

void x86_load_arg(struct code *c, int argno, int disp)
{
    int r = x86_argreg(argno);
    code_byte(c, (r & 8) ? 0x4c : 0x48);
    code_byte(c, 0x8b); /* mov r64, r/m64 */
    modrm_rbp(c, r & 7, disp);
}

void x86_mov_eax_imm(struct code *c, long imm, int w)
{
    if (w == 4) {
        code_byte(c, 0xb8); /* mov eax, imm32 */
        code_u32(c, (unsigned long)imm);
        return;
    }
    if (imm >= -2147483647L - 1 && imm <= 2147483647L) {
        code_byte(c, 0x48); /* mov rax, imm32 (sign-extended) */
        code_byte(c, 0xc7);
        code_byte(c, 0xc0);
        code_u32(c, (unsigned long)imm);
        return;
    }
    code_byte(c, 0x48); /* movabs rax, imm64 */
    code_byte(c, 0xb8);
    code_u32(c, (unsigned long)imm & 0xffffffffUL);
    code_u32(c, ((unsigned long)imm >> 32) & 0xffffffffUL);
}

void x86_load_slot(struct code *c, int disp, int size, int sign, int w)
{
    switch (size) {
    case 1:
        rexw(c, w);
        code_byte(c, 0x0f); /* movsx/movzx r, r/m8 */
        code_byte(c, sign ? 0xbe : 0xb6);
        modrm_rbp(c, 0, disp);
        break;
    case 2:
        rexw(c, w);
        code_byte(c, 0x0f); /* movsx/movzx r, r/m16 */
        code_byte(c, sign ? 0xbf : 0xb7);
        modrm_rbp(c, 0, disp);
        break;
    case 4:
        if (w == 8 && sign) {
            code_byte(c, 0x48); /* movsxd rax, r/m32 */
            code_byte(c, 0x63);
        } else {
            /* 32-bit mov zeroes the upper half — the unsigned extend */
            code_byte(c, 0x8b);
        }
        modrm_rbp(c, 0, disp);
        break;
    case 8:
        code_byte(c, 0x48);
        code_byte(c, 0x8b); /* mov rax, r/m64 */
        modrm_rbp(c, 0, disp);
        break;
    default:
        internal_error("bad load size %d", size);
    }
}

void x86_store_slot(struct code *c, int disp, int size)
{
    switch (size) {
    case 1:
        code_byte(c, 0x88); /* mov r/m8, al */
        break;
    case 2:
        code_byte(c, 0x66); /* mov r/m16, ax */
        code_byte(c, 0x89);
        break;
    case 4:
        code_byte(c, 0x89); /* mov r/m32, eax */
        break;
    case 8:
        code_byte(c, 0x48); /* mov r/m64, rax */
        code_byte(c, 0x89);
        break;
    default:
        internal_error("bad store size %d", size);
    }
    modrm_rbp(c, 0, disp);
}

void x86_load_mem_rax(struct code *c, int size, int sign, int w)
{
    /* same matrix as x86_load_slot with ModRM 00 = [rax] */
    switch (size) {
    case 1:
        rexw(c, w);
        code_byte(c, 0x0f);
        code_byte(c, sign ? 0xbe : 0xb6);
        code_byte(c, 0x00);
        break;
    case 2:
        rexw(c, w);
        code_byte(c, 0x0f);
        code_byte(c, sign ? 0xbf : 0xb7);
        code_byte(c, 0x00);
        break;
    case 4:
        if (w == 8 && sign) {
            code_byte(c, 0x48);
            code_byte(c, 0x63); /* movsxd rax, [rax] */
        } else {
            code_byte(c, 0x8b);
        }
        code_byte(c, 0x00);
        break;
    case 8:
        code_byte(c, 0x48);
        code_byte(c, 0x8b);
        code_byte(c, 0x00);
        break;
    default:
        internal_error("bad load size %d", size);
    }
}

/* ModRM for [base] (disp 0) with register field `reg`, in the shortest correct
 * form. The r/m encoding has two traps: base whose low 3 bits are 100 (rsp/r12)
 * needs a SIB byte, and 101 (rbp/r13) collides with RIP-relative at mod=00 so it
 * takes a mod=01 disp8 of zero. */
static void modrm_base0(struct code *c, int reg, int base)
{
    int lo = base & 7;
    if (lo == 5) {          /* rbp / r13 */
        code_byte(c, (1 << 6) | ((reg & 7) << 3) | 5);
        code_byte(c, 0x00);
    } else if (lo == 4) {   /* rsp / r12 */
        code_byte(c, ((reg & 7) << 3) | 4);
        code_byte(c, 0x24); /* SIB: base=r/sp/r12, no index */
    } else {
        code_byte(c, ((reg & 7) << 3) | lo);
    }
}

/* ModRM+SIB for [base + index*scale], disp 0, register field `reg`. Always uses
 * a SIB byte (rm=100). A base whose low 3 bits are 101 (rbp/r13) still needs a
 * mod=01 disp8=0. The index is an allocated register, never rsp, so rm-index 100
 * (= "no index") never collides. */
static void modrm_baseindex0(struct code *c, int reg, int base, int index,
                             int scale)
{
    int ss = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
    int mod = (base & 7) == 5 ? 1 : 0;
    code_byte(c, (mod << 6) | ((reg & 7) << 3) | 4);          /* rm=100 -> SIB */
    code_byte(c, (ss << 6) | ((index & 7) << 3) | (base & 7));
    if (mod == 1)
        code_byte(c, 0x00);
}

/* Load into rax from [base + index*scale] (scale 1/2/4/8), same extension matrix
 * as x86_load_mem_rax — folds an address computation into the load. REX.X/REX.B
 * carry high index/base registers. */
void x86_load_reg_baseindex(struct code *c, int dst, int base, int index,
                            int scale, int size, int sign, int w)
{
    int rexRXB = ((dst & 8) ? 4 : 0) | ((index & 8) ? 2 : 0) |
                 ((base & 8) ? 1 : 0);
    switch (size) {
    case 1:
    case 2: {
        int rex = 0x40 | (w == 8 ? 8 : 0) | rexRXB;
        if (rex != 0x40) code_byte(c, rex);
        code_byte(c, 0x0f);
        code_byte(c, size == 1 ? (sign ? 0xbe : 0xb6) : (sign ? 0xbf : 0xb7));
        modrm_baseindex0(c, dst, base, index, scale);
        break;
    }
    case 4:
        if (w == 8 && sign) {
            code_byte(c, 0x48 | rexRXB);
            code_byte(c, 0x63);
        } else {
            int rex = 0x40 | rexRXB;
            if (rex != 0x40) code_byte(c, rex);
            code_byte(c, 0x8b);
        }
        modrm_baseindex0(c, dst, base, index, scale);
        break;
    case 8:
        code_byte(c, 0x48 | rexRXB);
        code_byte(c, 0x8b);
        modrm_baseindex0(c, dst, base, index, scale);
        break;
    default:
        internal_error("bad load size %d", size);
    }
}

void x86_load_baseindex_rax(struct code *c, int base, int index, int scale,
                            int size, int sign, int w)
{
    x86_load_reg_baseindex(c, 0, base, index, scale, size, sign, w);
}

/* Load into rax from [base + disp], same extension matrix as x86_load_mem_rax —
 * folds a constant-offset address (a struct field, `p->m`) into the load.
 * modrm_base carries the disp and the rsp/r12 SIB case; REX.B a high base. */
/* rax = *base into an ARBITRARY register: the register-targeted form of
 * x86_load_base_rax (identical bytes when dst == rax). Zero/sign-extends narrow
 * loads to `w` exactly as that routine does. */
void x86_load_base_reg(struct code *c, int dst, int base,
                       int size, int sign, int w)
{
    int rexRB = ((dst & 8) ? 4 : 0) | ((base & 8) ? 1 : 0);
    switch (size) {
    case 1:
    case 2: {
        int rex = 0x40 | (w == 8 ? 8 : 0) | rexRB;
        if (rex != 0x40) code_byte(c, rex);
        code_byte(c, 0x0f);
        code_byte(c, size == 1 ? (sign ? 0xbe : 0xb6) : (sign ? 0xbf : 0xb7));
        modrm_base0(c, dst, base);
        break;
    }
    case 4:
        if (w == 8 && sign) {
            code_byte(c, 0x48 | rexRB);
            code_byte(c, 0x63);      /* movsxd dst, [base] */
        } else {
            int rex = 0x40 | rexRB;
            if (rex != 0x40) code_byte(c, rex);
            code_byte(c, 0x8b);
        }
        modrm_base0(c, dst, base);
        break;
    case 8:
        code_byte(c, 0x48 | rexRB);
        code_byte(c, 0x8b);
        modrm_base0(c, dst, base);
        break;
    default:
        internal_error("bad load size %d", size);
    }
}

/* dst = *(base+disp) with size/sign extension into an ARBITRARY register — the
 * register-targeted form of x86_load_basedisp_rax (identical bytes when
 * dst == rax). Used to extend a memory local/temp straight into its home reg. */
void x86_load_reg_basedisp(struct code *c, int dst, int base, int disp,
                           int size, int sign, int w)
{
    int rexRB = ((dst & 8) ? 4 : 0) | ((base & 8) ? 1 : 0);
    switch (size) {
    case 1:
    case 2: {
        int rex = 0x40 | (w == 8 ? 8 : 0) | rexRB;
        if (rex != 0x40) code_byte(c, rex);
        code_byte(c, 0x0f);
        code_byte(c, size == 1 ? (sign ? 0xbe : 0xb6) : (sign ? 0xbf : 0xb7));
        modrm_base(c, dst, base, disp);
        break;
    }
    case 4:
        if (w == 8 && sign) {
            code_byte(c, 0x48 | rexRB);
            code_byte(c, 0x63);      /* movsxd dst, [base+disp] */
        } else {
            int rex = 0x40 | rexRB;
            if (rex != 0x40) code_byte(c, rex);
            code_byte(c, 0x8b);
        }
        modrm_base(c, dst, base, disp);
        break;
    case 8:
        code_byte(c, 0x48 | rexRB);
        code_byte(c, 0x8b);
        modrm_base(c, dst, base, disp);
        break;
    default:
        internal_error("bad load size %d", size);
    }
}

void x86_load_basedisp_rax(struct code *c, int base, int disp,
                           int size, int sign, int w)
{
    int rexb = (base & 8) ? 1 : 0;
    switch (size) {
    case 1:
    case 2: {
        int rex = 0x40 | (w == 8 ? 8 : 0) | rexb;
        if (rex != 0x40) code_byte(c, rex);
        code_byte(c, 0x0f);
        code_byte(c, size == 1 ? (sign ? 0xbe : 0xb6) : (sign ? 0xbf : 0xb7));
        modrm_base(c, 0, base, disp);
        break;
    }
    case 4:
        if (w == 8 && sign) {
            code_byte(c, 0x48 | rexb);
            code_byte(c, 0x63);
        } else {
            int rex = 0x40 | rexb;
            if (rex != 0x40) code_byte(c, rex);
            code_byte(c, 0x8b);
        }
        modrm_base(c, 0, base, disp);
        break;
    case 8:
        code_byte(c, 0x48 | rexb);
        code_byte(c, 0x8b);
        modrm_base(c, 0, base, disp);
        break;
    default:
        internal_error("bad load size %d", size);
    }
}

/* Load into rax straight from [base], with the same extension matrix as
 * x86_load_mem_rax — no `mov base,rax` first. REX.B carries a high base
 * (r8..r15); modrm_base0 handles the rsp/rbp/r12/r13 addressing traps. */
void x86_load_base_rax(struct code *c, int base, int size, int sign, int w)
{
    int rexb = (base & 8) ? 1 : 0;
    switch (size) {
    case 1:
    case 2: {
        int rex = 0x40 | (w == 8 ? 8 : 0) | rexb;
        if (rex != 0x40) code_byte(c, rex);
        code_byte(c, 0x0f);
        code_byte(c, size == 1 ? (sign ? 0xbe : 0xb6) : (sign ? 0xbf : 0xb7));
        modrm_base0(c, 0, base);
        break;
    }
    case 4:
        if (w == 8 && sign) {
            code_byte(c, 0x48 | rexb);
            code_byte(c, 0x63);      /* movsxd rax, [base] */
        } else {
            if (rexb) code_byte(c, 0x41);
            code_byte(c, 0x8b);
        }
        modrm_base0(c, 0, base);
        break;
    case 8:
        code_byte(c, 0x48 | rexb);
        code_byte(c, 0x8b);
        modrm_base0(c, 0, base);
        break;
    default:
        internal_error("bad load size %d", size);
    }
}

void x86_store_mem_rcx(struct code *c, int size)
{
    switch (size) {
    case 1:
        code_byte(c, 0x88); /* mov [rcx], al */
        break;
    case 2:
        code_byte(c, 0x66);
        code_byte(c, 0x89);
        break;
    case 4:
        code_byte(c, 0x89);
        break;
    case 8:
        code_byte(c, 0x48);
        code_byte(c, 0x89);
        break;
    default:
        internal_error("bad store size %d", size);
    }
    code_byte(c, 0x01); /* ModRM: [rcx], eax/rax */
}

/* Store rax to [base + disp] (a struct-field store, `p->m = x`), sized. reg
 * field is rax(0); modrm_base carries disp and the rsp/r12 SIB case. */
/* A byte store names the SOURCE in the reg field, and without a REX
 * prefix registers 4..7 there mean %ah..%bh rather than %spl..%dil. So
 * the byte forms emit REX whenever the source is one of those, even when
 * it carries no bits. */
void x86_store_basedisp_reg(struct code *c, int base, int disp, int src,
                            int size)
{
    int rexRB = ((src & 8) ? 4 : 0) | ((base & 8) ? 1 : 0);
    switch (size) {
    case 1:
        if (rexRB || src >= 4) code_byte(c, 0x40 | rexRB);
        code_byte(c, 0x88);
        break;
    case 2:
        code_byte(c, 0x66);
        if (rexRB) code_byte(c, 0x40 | rexRB);
        code_byte(c, 0x89);
        break;
    case 4:
        if (rexRB) code_byte(c, 0x40 | rexRB);
        code_byte(c, 0x89);
        break;
    case 8:
        code_byte(c, 0x48 | rexRB);
        code_byte(c, 0x89);
        break;
    default: internal_error("bad store size %d", size);
    }
    modrm_base(c, src, base, disp);
}

void x86_store_basedisp_rax(struct code *c, int base, int disp, int size)
{
    x86_store_basedisp_reg(c, base, disp, 0, size);
}

/* Store rax to [base + index*scale] (an array-element store, `p[i] = x`). */
void x86_store_baseindex_reg(struct code *c, int base, int index, int scale,
                             int src, int size)
{
    int rexRXB = ((src & 8) ? 4 : 0) | ((index & 8) ? 2 : 0) |
                 ((base & 8) ? 1 : 0);
    switch (size) {
    case 1:
        if (rexRXB || src >= 4) code_byte(c, 0x40 | rexRXB);
        code_byte(c, 0x88);
        break;
    case 2:
        code_byte(c, 0x66);
        if (rexRXB) code_byte(c, 0x40 | rexRXB);
        code_byte(c, 0x89);
        break;
    case 4:
        if (rexRXB) code_byte(c, 0x40 | rexRXB);
        code_byte(c, 0x89);
        break;
    case 8:
        code_byte(c, 0x48 | rexRXB);
        code_byte(c, 0x89);
        break;
    default: internal_error("bad store size %d", size);
    }
    modrm_baseindex0(c, src, base, index, scale);
}

void x86_store_baseindex_rax(struct code *c, int base, int index, int scale,
                             int size)
{
    x86_store_baseindex_reg(c, base, index, scale, 0, size);
}

void x86_mov_rcx_slot(struct code *c, int disp)
{
    code_byte(c, 0x48);
    code_byte(c, 0x8b); /* mov rcx, [rbp+disp] */
    modrm_rbp(c, 1, disp);
}

void x86_lea_rax_slot(struct code *c, int disp)
{
    code_byte(c, 0x48);
    code_byte(c, 0x8d); /* lea rax, [rbp+disp] */
    modrm_rbp(c, 0, disp);
}

int x86_lea_rax_rip(struct code *c)
{
    code_byte(c, 0x48);
    code_byte(c, 0x8d); /* lea rax, [rip+rel32] */
    code_byte(c, 0x05); /* ModRM: mod=00 rm=101 = RIP-relative */
    int off = c->len;
    code_u32(c, 0);
    return off;
}

/* lea reg, [rip+rel32] into an arbitrary register (byte-identical to the rax
 * form when reg == rax). Returns the rel32 patch offset, as x86_lea_rax_rip. */
int x86_lea_reg_rip(struct code *c, int reg)
{
    code_byte(c, 0x48 | ((reg & 8) ? 4 : 0));    /* REX.W (+REX.R) */
    code_byte(c, 0x8d);
    code_byte(c, 0x05 | ((reg & 7) << 3));        /* mod=00 reg rm=101 = RIP */
    int off = c->len;
    code_u32(c, 0);
    return off;
}

/* ---- local-exec thread-local storage -------------------------------------
 *
 * Two instructions. The first asks the CPU where THIS thread's block
 * is; the second adds the object's offset within it, which only the
 * linker can know (it depends on how much thread-local data the whole
 * program turned out to have) and so is left to a relocation.
 *
 * `mov %fs:0, reg` reads the word at offset 0 of the FS segment, which
 * by the x86-64 TLS ABI is a self-pointer: the thread pointer holds its
 * own address there precisely so a program can load it, since the FS
 * BASE itself is not readable from user space without a syscall.
 *
 *   64 48 8b 04 25 00000000   mov %fs:0x0, %rax
 *   ^  ^  ^  ^  ^  ^
 *   |  |  |  |  |  the absolute address inside the segment: 0
 *   |  |  |  |  SIB: no base, no index -> disp32 is the whole address
 *   |  |  |  mod=00 reg=dst rm=100 (SIB follows)
 *   |  |  opcode: mov r64, r/m64
 *   |  REX.W (+REX.R for r8..r15)
 *   the FS segment-override prefix
 */
void x86_mov_reg_fsbase(struct code *c, int reg)
{
    code_byte(c, 0x64);                          /* FS prefix */
    code_byte(c, 0x48 | ((reg & 8) ? 4 : 0));    /* REX.W (+REX.R) */
    code_byte(c, 0x8b);
    code_byte(c, 0x04 | ((reg & 7) << 3));       /* mod=00 rm=100 (SIB) */
    code_byte(c, 0x25);                          /* base=101 idx=100: disp32 */
    code_u32(c, 0);
}

/* `add $imm32, reg`, with the immediate left for a relocation to fill.
 * Returns the offset of that field. The offset is negative on x86-64 --
 * the thread block sits BELOW the thread pointer -- which is why the
 * field is a signed 32-bit add rather than anything narrower. */
int x86_add_reg_imm32_reloc(struct code *c, int reg)
{
    code_byte(c, 0x48 | ((reg & 8) ? 1 : 0));    /* REX.W (+REX.B) */
    code_byte(c, 0x81);
    code_byte(c, 0xc0 | (reg & 7));              /* /0 = ADD */
    int off = c->len;
    code_u32(c, 0);
    return off;
}

/* mov reg, imm into an arbitrary register — the register-targeted form of
 * x86_mov_eax_imm (identical bytes when reg == rax): 32-bit immediate,
 * sign-extended 32-bit into a 64-bit register, or a full movabs imm64. */
void x86_mov_reg_imm(struct code *c, int reg, long imm, int w)
{
    if (w == 4) {
        if (reg & 8) code_byte(c, 0x41);          /* REX.B */
        code_byte(c, 0xb8 | (reg & 7));           /* mov r32, imm32 */
        code_u32(c, (unsigned long)imm);
        return;
    }
    if (imm >= -2147483647L - 1 && imm <= 2147483647L) {
        code_byte(c, 0x48 | ((reg & 8) ? 1 : 0)); /* REX.W (+REX.B) */
        code_byte(c, 0xc7);
        code_byte(c, 0xc0 | (reg & 7));           /* mov r/m64, imm32 (sext) */
        code_u32(c, (unsigned long)imm);
        return;
    }
    code_byte(c, 0x48 | ((reg & 8) ? 1 : 0));     /* REX.W (+REX.B) movabs */
    code_byte(c, 0xb8 | (reg & 7));
    code_u32(c, (unsigned long)imm & 0xffffffffUL);
    code_u32(c, ((unsigned long)imm >> 32) & 0xffffffffUL);
}

void x86_zero_eax(struct code *c)
{
    code_byte(c, 0x31); /* xor eax, eax */
    code_byte(c, 0xc0);
}

void x86_alu_eax_mem(struct code *c, int op, int disp, int w)
{
    rexw(c, w);
    switch (op) {
    case '+':
        code_byte(c, 0x03); /* add r, r/m */
        break;
    case '-':
        code_byte(c, 0x2b); /* sub r, r/m */
        break;
    case '*':
        code_byte(c, 0x0f); /* imul r, r/m */
        code_byte(c, 0xaf);
        break;
    case '&':
        code_byte(c, 0x23); /* and r, r/m */
        break;
    case '|':
        code_byte(c, 0x0b); /* or r, r/m */
        break;
    case '^':
        code_byte(c, 0x33); /* xor r, r/m */
        break;
    default:
        internal_error("no encoding for op '%c'", op);
    }
    modrm_rbp(c, 0, disp);
}

void x86_cdq(struct code *c, int w)
{
    rexw(c, w); /* cqo when 64-bit */
    code_byte(c, 0x99);
}

void x86_zero_edx(struct code *c)
{
    code_byte(c, 0x31); /* xor edx, edx (also clears upper rdx) */
    code_byte(c, 0xd2);
}

void x86_div_mem(struct code *c, int disp, int sign, int w)
{
    rexw(c, w);
    code_byte(c, 0xf7); /* idiv is /7, div is /6 */
    modrm_rbp(c, sign ? 7 : 6, disp);
}

void x86_mov_eax_edx(struct code *c, int w)
{
    rexw(c, w);
    code_byte(c, 0x89); /* mov eax/rax, edx/rdx */
    code_byte(c, 0xd0);
}

void x86_mov_ecx_mem(struct code *c, int disp, int w)
{
    rexw(c, w);
    code_byte(c, 0x8b); /* mov ecx/rcx, r/m */
    modrm_rbp(c, 1, disp);
}

void x86_shift_eax_cl(struct code *c, int kind, int w)
{
    rexw(c, w);
    code_byte(c, 0xd3); /* group 2, count in cl */
    switch (kind) {
    case '<':
        code_byte(c, 0xe0); /* shl: /4 */
        break;
    case '>':
        code_byte(c, 0xf8); /* sar: /7 */
        break;
    case 'u':
        code_byte(c, 0xe8); /* shr: /5 */
        break;
    default:
        internal_error("bad shift kind");
    }
}

/* shift `reg` by a constant: the 1-count short form (D1 /ext) or the imm8 form
 * (C1 /ext ib). kind: '<' shl, '>' sar, 'u' shr — the twin of x86_shift_eax_cl. */
void x86_shift_reg_imm(struct code *c, int reg, int kind, int count, int w)
{
    int ext = kind == '<' ? 4 : kind == 'u' ? 5 : 7;   /* shl:/4 shr:/5 sar:/7 */
    rex_rb(c, w == 8, 0, reg);
    if (count == 1) {
        code_byte(c, 0xd1);
        code_byte(c, 0xc0 | (ext << 3) | (reg & 7));
    } else {
        code_byte(c, 0xc1);
        code_byte(c, 0xc0 | (ext << 3) | (reg & 7));
        code_byte(c, count & 0xff);
    }
}

void x86_neg_eax(struct code *c, int w)
{
    rexw(c, w);
    code_byte(c, 0xf7); /* neg: /3 */
    code_byte(c, 0xd8);
}

/* Byte-swap the low `size` bytes of rax/eax/ax in place. bswap has no
 * 16-bit form, so a 2-byte swap is `rol $8, %ax`. */
void x86_bswap(struct code *c, int size)
{
    if (size == 2) {
        code_byte(c, 0x66);   /* operand-size prefix: 16-bit */
        code_byte(c, 0xc1);   /* rol r/m16, imm8 */
        code_byte(c, 0xc0);   /* mod=11 /0 reg=ax */
        code_byte(c, 0x08);
        return;
    }
    if (size == 8)
        code_byte(c, 0x48);   /* REX.W: bswap %rax */
    code_byte(c, 0x0f);
    code_byte(c, 0xc8);       /* bswap eax/rax */
}

/* mfence — a full memory barrier (__sync_synchronize). */
void x86_mfence(struct code *c)
{
    code_byte(c, 0x0f);
    code_byte(c, 0xae);
    code_byte(c, 0xf0);
}

/* ud2 — the guaranteed-undefined instruction (__builtin_unreachable). */
void x86_ud2(struct code *c)
{
    code_byte(c, 0x0f);
    code_byte(c, 0x0b);
}

/* xchg rax/eax/ax/al with [rcx] — the memory operand makes it implicitly
 * LOCKed, i.e. atomic. RAX ends holding the old value at [rcx]. */
void x86_xchg_rax_mem_rcx(struct code *c, int size)
{
    switch (size) {
    case 1: code_byte(c, 0x86); break;
    case 2: code_byte(c, 0x66); code_byte(c, 0x87); break;
    case 4: code_byte(c, 0x87); break;
    case 8: code_byte(c, 0x48); code_byte(c, 0x87); break;
    default:
        internal_error("bad xchg size %d", size);
    }
    code_byte(c, 0x01); /* ModRM: [rcx] <-> eax/rax */
}

/* lock xadd %rax/eax/ax/al, (%rcx): atomically *rcx += rax, rax = old *rcx. */
void x86_lock_xadd_rcx(struct code *c, int size)
{
    code_byte(c, 0xf0);                       /* LOCK */
    if (size == 2) code_byte(c, 0x66);
    if (size == 8) code_byte(c, 0x48);        /* REX.W */
    code_byte(c, 0x0f);
    code_byte(c, size == 1 ? 0xc0 : 0xc1);
    code_byte(c, 0x01);                       /* ModRM: reg=rax, [rcx] */
}

/* lock cmpxchg %rdx/edx/dx/dl, (%rcx): compare RAX with *rcx; if equal set
 * *rcx = RDX and ZF, else load *rcx into RAX and clear ZF. Atomic. */
void x86_lock_cmpxchg_rcx(struct code *c, int size)
{
    code_byte(c, 0xf0);                       /* LOCK */
    if (size == 2) code_byte(c, 0x66);
    if (size == 8) code_byte(c, 0x48);        /* REX.W */
    code_byte(c, 0x0f);
    code_byte(c, size == 1 ? 0xb0 : 0xb1);
    code_byte(c, 0x11);                       /* ModRM: reg=rdx, [rcx] */
}

void x86_not_eax(struct code *c, int w)
{
    rexw(c, w);
    code_byte(c, 0xf7); /* not: /2 */
    code_byte(c, 0xd0);
}
/* neg/not on an arbitrary register (reg in the r/m field, /3 and /2). Lets a
 * register-resident unary op stay in place, no RAX round-trip. */
void x86_neg_reg(struct code *c, int reg, int w)
{
    rex_rb(c, w == 8 ? 1 : 0, 0, reg);
    code_byte(c, 0xf7);
    code_byte(c, 0xd8 + (reg & 7)); /* mod=11 /3 rm=reg */
}
void x86_not_reg(struct code *c, int reg, int w)
{
    rex_rb(c, w == 8 ? 1 : 0, 0, reg);
    code_byte(c, 0xf7);
    code_byte(c, 0xd0 + (reg & 7)); /* mod=11 /2 rm=reg */
}

/* The SSE2 prefix that selects scalar single vs scalar double. */
static void sse_prefix(struct code *c, int w)
{
    code_byte(c, w == 4 ? 0xf3 : 0xf2);
}

/* REX.R for an xmm register above 7. The SSE prefix is a legacy one and
 * comes first; REX has to sit immediately before the 0x0F escape. Until
 * the FP pool existed nothing here named a register above xmm7, so these
 * emitters simply did not write one. */
static void sse_rex_r(struct code *c, int xmm)
{
    if (xmm >= 8)
        code_byte(c, 0x44);
}

void x86_movs_load(struct code *c, int xmm, int disp, int w)
{
    sse_prefix(c, w);
    sse_rex_r(c, xmm);
    code_byte(c, 0x0f);
    code_byte(c, 0x10); /* movss/movsd xmm, m */
    modrm_rbp(c, xmm & 7, disp);
}

/* movaps xmm, xmm -- a whole-register copy, which is what a scalar move
 * between registers costs anyway and has no false dependency on the
 * destination's upper half the way movss/movsd does. */
/* movq xmm, r64 / movd xmm, r32 -- the only way a bit pattern in a
 * general register becomes a floating-point value without going through
 * memory. A float CONSTANT is exactly that: the IR holds its bits. */
void x86_movq_xmm_gpr(struct code *c, int xmm, int gpr, int w)
{
    code_byte(c, 0x66);
    if (w == 8 || xmm >= 8 || gpr >= 8)
        code_byte(c, 0x40 | ((w == 8) << 3) | ((xmm >= 8) << 2) | (gpr >= 8));
    code_byte(c, 0x0f);
    code_byte(c, 0x6e);
    code_byte(c, 0xc0 | ((xmm & 7) << 3) | (gpr & 7));
}

void x86_movs_reg(struct code *c, int dst, int src)
{
    if (dst >= 8 || src >= 8)
        code_byte(c, 0x40 | ((dst >= 8) << 2) | (src >= 8));
    code_byte(c, 0x0f);
    code_byte(c, 0x28);
    code_byte(c, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/* The register form of the scalar ALU ops: `addsd dst, src`. */
void x86_sse_alu_reg(struct code *c, int op, int dst, int src, int w)
{
    sse_prefix(c, w);
    if (dst >= 8 || src >= 8)
        code_byte(c, 0x40 | ((dst >= 8) << 2) | (src >= 8));
    code_byte(c, 0x0f);
    switch (op) {
    case '+': code_byte(c, 0x58); break;
    case '-': code_byte(c, 0x5c); break;
    case '*': code_byte(c, 0x59); break;
    case '/': code_byte(c, 0x5e); break;
    case 'q': code_byte(c, 0x51); break;
    default:
        internal_error("no SSE encoding for '%c'", op);
    }
    code_byte(c, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

void x86_movs_store(struct code *c, int xmm, int disp, int w)
{
    sse_prefix(c, w);
    sse_rex_r(c, xmm);
    code_byte(c, 0x0f);
    code_byte(c, 0x11); /* movss/movsd m, xmm */
    modrm_rbp(c, xmm & 7, disp);
}

void x86_sse_alu_mem(struct code *c, int op, int dst, int disp, int w)
{
    sse_prefix(c, w);
    sse_rex_r(c, dst);
    code_byte(c, 0x0f);
    switch (op) {
    case '+': code_byte(c, 0x58); break; /* addss/addsd */
    case '-': code_byte(c, 0x5c); break; /* subss/subsd */
    case '*': code_byte(c, 0x59); break; /* mulss/mulsd */
    case '/': code_byte(c, 0x5e); break; /* divss/divsd */
    case 'q': code_byte(c, 0x51); break; /* sqrtss/sqrtsd: reads memory
                                          * straight into xmm0, so no
                                          * separate load is needed */
    default:
        internal_error("no SSE encoding for '%c'", op);
    }
    modrm_rbp(c, dst & 7, disp);
}

void x86_ucomis_mem(struct code *c, int disp, int w)
{
    if (w == 8)
        code_byte(c, 0x66); /* ucomisd */
    code_byte(c, 0x0f);
    code_byte(c, 0x2e);
    modrm_rbp(c, 0, disp);
}

/* ucomis xmm, xmm -- the register form, for a second operand that has an
 * FP home rather than a slot. */
void x86_ucomis_reg(struct code *c, int a, int b, int w)
{
    if (w == 8)
        code_byte(c, 0x66);
    if (a >= 8 || b >= 8)
        code_byte(c, 0x40 | ((a >= 8) << 2) | (b >= 8));
    code_byte(c, 0x0f);
    code_byte(c, 0x2e);
    code_byte(c, 0xc0 | ((a & 7) << 3) | (b & 7));
}

/* setcc + zero-extend into an ARBITRARY register (register-targeted
 * x86_setcc_eax; identical bytes when reg == rax). Writes reg's low byte then
 * movzx-widens it in place -- RAX is never touched.
 *
 * Both halves name reg as an 8-bit operand, so both go through rex_rb8:
 * the note there is what this used to say could never happen. */
void x86_setcc_reg(struct code *c, int cc, int reg)
{
    rex_rb8(c, 0, 0, reg, 0, 1);               /* setcc r/m8 */
    code_byte(c, 0x0f);
    code_byte(c, cc);
    code_byte(c, 0xc0 | (reg & 7));
    rex_rb8(c, 0, reg, reg, 0, 1);             /* movzx reg32, reg8 */
    code_byte(c, 0x0f);
    code_byte(c, 0xb6);
    code_byte(c, 0xc0 | ((reg & 7) << 3) | (reg & 7));
}

void x86_set_float_eq(struct code *c, int ne)
{
    /* ucomis sets ZF=PF=CF=1 for unordered. == must be false for NaN,
     * != must be true, so neither is a single setcc. */
    code_byte(c, 0x0f);
    code_byte(c, ne ? 0x95 : 0x94); /* setne/sete al */
    code_byte(c, 0xc0);
    code_byte(c, 0x0f);
    code_byte(c, ne ? 0x9a : 0x9b); /* setp/setnp cl */
    code_byte(c, 0xc1);
    code_byte(c, ne ? 0x08 : 0x20); /* or/and al, cl */
    code_byte(c, 0xc8);
    code_byte(c, 0x0f); /* movzx eax, al */
    code_byte(c, 0xb6);
    code_byte(c, 0xc0);
}

void x86_cvtsi2s(struct code *c, int disp, int srcw, int dstw)
{
    sse_prefix(c, dstw);
    if (srcw == 8)
        code_byte(c, 0x48); /* REX.W: 64-bit integer source */
    code_byte(c, 0x0f);
    code_byte(c, 0x2a); /* cvtsi2ss/cvtsi2sd xmm0, r/m */
    modrm_rbp(c, 0, disp);
}

void x86_cvtts2si(struct code *c, int disp, int srcw, int dstw)
{
    sse_prefix(c, srcw);
    if (dstw == 8)
        code_byte(c, 0x48); /* REX.W: 64-bit integer destination */
    code_byte(c, 0x0f);
    code_byte(c, 0x2c); /* cvttss2si/cvttsd2si rax, xmm/m (truncating) */
    modrm_rbp(c, 0, disp);
}

void x86_cvts2s(struct code *c, int disp, int srcw)
{
    sse_prefix(c, srcw);
    code_byte(c, 0x0f);
    code_byte(c, 0x5a); /* cvtss2sd / cvtsd2ss */
    modrm_rbp(c, 0, disp);
}

/* The register forms of the three conversions, for a value that has an
 * FP home rather than a slot. Each writes xmm0 or rax exactly as its
 * memory sibling does; only where the OPERAND comes from changes. */
void x86_cvtsi2s_reg(struct code *c, int src, int srcw, int dstw)
{
    sse_prefix(c, dstw);
    if (srcw == 8 || src >= 8)
        code_byte(c, 0x40 | ((srcw == 8) << 3) | (src >= 8));
    code_byte(c, 0x0f);
    code_byte(c, 0x2a);
    code_byte(c, 0xc0 | (src & 7));          /* reg = xmm0 */
}

void x86_cvtts2si_reg(struct code *c, int src, int srcw, int dstw)
{
    sse_prefix(c, srcw);
    if (dstw == 8 || src >= 8)
        code_byte(c, 0x40 | ((dstw == 8) << 3) | (src >= 8));
    code_byte(c, 0x0f);
    code_byte(c, 0x2c);
    code_byte(c, 0xc0 | (src & 7));          /* reg = rax */
}

void x86_cvts2s_reg(struct code *c, int src, int srcw)
{
    sse_prefix(c, srcw);
    if (src >= 8)
        code_byte(c, 0x41);
    code_byte(c, 0x0f);
    code_byte(c, 0x5a);
    code_byte(c, 0xc0 | (src & 7));          /* reg = xmm0 */
}

void x86_mov_al_imm(struct code *c, int v)
{
    code_byte(c, 0xb0); /* mov al, imm8 */
    code_byte(c, v & 0xff);
}

void x86_cmp_eax_mem(struct code *c, int disp, int w)
{
    rexw(c, w);
    code_byte(c, 0x3b); /* cmp r, r/m */
    modrm_rbp(c, 0, disp);
}

void x86_setcc_eax(struct code *c, int cc)
{
    code_byte(c, 0x0f); /* setcc al */
    code_byte(c, cc);
    code_byte(c, 0xc0);
    code_byte(c, 0x0f); /* movzx eax, al */
    code_byte(c, 0xb6);
    code_byte(c, 0xc0);
}

void x86_test_eax(struct code *c, int w)
{
    rexw(c, w);
    code_byte(c, 0x85); /* test r/m, r */
    code_byte(c, 0xc0);
}

int x86_jz_rel32(struct code *c)
{
    code_byte(c, 0x0f);
    code_byte(c, 0x84);
    int off = c->len;
    code_u32(c, 0);
    return off;
}

int x86_jnz_rel32(struct code *c)
{
    code_byte(c, 0x0f);
    code_byte(c, 0x85); /* jnz rel32 */
    int off = c->len;
    code_u32(c, 0);
    return off;
}

int x86_jmp_rel32(struct code *c)
{
    code_byte(c, 0xe9);
    int off = c->len;
    code_u32(c, 0);
    return off;
}

/* The two-byte forms. A displacement that fits in a signed byte reaches
 * most of what a function branches to, and costs four bytes less every
 * time -- `74 cb` against `0f 84 cd`. Which branches may use one is not
 * knowable while emitting, because it depends on where everything else
 * lands; codegen decides by emitting the function more than once (see
 * the relaxation loop in codegen.c) and these are what it emits when it
 * has decided yes. */
int x86_jz_rel8(struct code *c)
{
    code_byte(c, 0x74);
    int off = c->len;
    code_byte(c, 0);
    return off;
}

int x86_jnz_rel8(struct code *c)
{
    code_byte(c, 0x75);
    int off = c->len;
    code_byte(c, 0);
    return off;
}

int x86_jmp_rel8(struct code *c)
{
    code_byte(c, 0xeb);
    int off = c->len;
    code_byte(c, 0);
    return off;
}

/* jmp *reg  (FF /4) — the indirect jump a GNU computed goto lowers to. */
void x86_jmp_reg(struct code *c, int reg)
{
    if (reg >= 8) code_byte(c, 0x41);     /* REX.B for r8..r15 */
    code_byte(c, 0xff);
    code_byte(c, 0xe0 + (reg & 7));       /* ModRM mod=11 /4 rm=reg */
}

/* Conditional jump rel32 for a setcc condition byte (0x9x, as cc_for returns):
 * the Jcc opcode shares the condition's low nibble (0f 8x). Patch offset back. */
int x86_jcc_rel32(struct code *c, int setcc)
{
    code_byte(c, 0x0f);
    code_byte(c, 0x80 | (setcc & 0x0f));
    int off = c->len;
    code_u32(c, 0);
    return off;
}

/* The two-byte conditional: `7x cb` where the near form is `0f 8x cd`. */
int x86_jcc_rel8(struct code *c, int setcc)
{
    code_byte(c, 0x70 | (setcc & 0x0f));
    int off = c->len;
    code_byte(c, 0);
    return off;
}

int x86_call_rel32(struct code *c)
{
    code_byte(c, 0xe8);
    int off = c->len;
    code_u32(c, 0);
    return off;
}

void x86_mov_r11_slot(struct code *c, int disp)
{
    code_byte(c, 0x4c); /* REX.WR: mov r11, [rbp+disp] */
    code_byte(c, 0x8b);
    modrm_rbp(c, 3, disp);
}

void x86_call_r11(struct code *c)
{
    code_byte(c, 0x41); /* REX.B */
    code_byte(c, 0xff); /* call r/m64: /2 */
    code_byte(c, 0xd3);
}

/* ---- 128-bit vectors (SSE2) ----------------------------------------------
 *
 * SSE2 and nothing above it, because every x86-64 has SSE2 by
 * definition: a vector instruction here needs no feature test, no
 * run-time dispatch and no fallback path. That is also why there is no
 * packed 32-bit multiply below -- `pmulld` is SSE4.1, and the
 * vectorizer turns a multiply by a constant into shifts and adds rather
 * than emit an instruction an old machine would fault on.
 *
 * A vector temp lives in a 16-byte frame slot (the `wide` map, as a long
 * double does), and every operation works in xmm0 against a slot, which
 * is the same shape the scalar float path already uses. */

/* movdqu xmm, [base+disp] / movdqu [base+disp], xmm. Unaligned, because
 * the address comes from the program (&a[i] for any i), not from us. */
void x86_vload_base(struct code *c, int xmm, int base, int disp)
{
    code_byte(c, 0xf3);
    rex_rb(c, 0, xmm, base);
    code_byte(c, 0x0f); code_byte(c, 0x6f);
    modrm_base(c, xmm, base, disp);
}

void x86_vstore_base(struct code *c, int base, int disp, int xmm)
{
    code_byte(c, 0xf3);
    rex_rb(c, 0, xmm, base);
    code_byte(c, 0x0f); code_byte(c, 0x7f);
    modrm_base(c, xmm, base, disp);
}

/* The same against a frame slot, which we align to 16, so movdqa. */
/* movdqu xmm, [base+disp] / movdqu [base+disp], xmm -- all sixteen
 * bytes in one instruction, against any base register.
 *
 * This is what a 16-byte COPY is: a long double, an __int128 or a
 * vector moving from one place to another. Doing it as two loads and
 * two stores through rax was four instructions and about twenty-two
 * bytes; this is two and thirteen, and it leaves rax (and its residency
 * cache) alone.
 *
 * UNALIGNED, because only some of the addresses involved are known to
 * be 16-aligned: a 16-byte slot is, and a pointer a program handed us
 * is not. On every processor this compiler targets movdqu against an
 * aligned address costs what movdqa would. */
void x86_mov128_load(struct code *c, int xmm, int base, int disp)
{
    code_byte(c, 0xf3);
    { int rex = 0x40 | ((xmm & 8) ? 4 : 0) | ((base & 8) ? 1 : 0);
      if (rex != 0x40) code_byte(c, rex); }
    code_byte(c, 0x0f); code_byte(c, 0x6f);
    modrm_base(c, xmm, base, disp);
}

void x86_mov128_store(struct code *c, int base, int disp, int xmm)
{
    code_byte(c, 0xf3);
    { int rex = 0x40 | ((xmm & 8) ? 4 : 0) | ((base & 8) ? 1 : 0);
      if (rex != 0x40) code_byte(c, rex); }
    code_byte(c, 0x0f); code_byte(c, 0x7f);
    modrm_base(c, xmm, base, disp);
}

void x86_vload_slot(struct code *c, int xmm, int disp)
{
    code_byte(c, 0x66);
    code_byte(c, 0x0f); code_byte(c, 0x6f);
    modrm_rbp(c, xmm, disp);
}

void x86_vstore_slot(struct code *c, int disp, int xmm)
{
    code_byte(c, 0x66);
    code_byte(c, 0x0f); code_byte(c, 0x7f);
    modrm_rbp(c, xmm, disp);
}

/* xmm <op>= [rbp+disp], lane by lane at `esize` bytes. */
void x86_vbin_slot(struct code *c, int xmm, int op, int esize, int disp)
{
    int opcode;
    switch (op) {
    case '+':
        opcode = esize == 1 ? 0xfc : esize == 2 ? 0xfd
               : esize == 4 ? 0xfe : 0xd4;          /* paddb/w/d/q */
        break;
    case '-':
        opcode = esize == 1 ? 0xf8 : esize == 2 ? 0xf9
               : esize == 4 ? 0xfa : 0xfb;          /* psubb/w/d/q */
        break;
    case '&': opcode = 0xdb; break;                 /* pand */
    case '|': opcode = 0xeb; break;                 /* por */
    case '^': opcode = 0xef; break;                 /* pxor */
    default:
        internal_error("no SSE2 packed encoding for '%c'", op);
    }
    code_byte(c, 0x66);
    code_byte(c, 0x0f); code_byte(c, (unsigned)opcode);
    modrm_rbp(c, xmm, disp);
}

/* xmm <<= imm / >>= imm, lane by lane. `arith` picks psra over psrl;
 * SSE2 has no arithmetic 64-bit shift, so the vectorizer does not ask. */
void x86_vshift_imm(struct code *c, int xmm, int left, int arith, int esize,
                    int imm)
{
    int grp = esize == 2 ? 0x71 : esize == 4 ? 0x72 : 0x73;
    int ext = left ? 6 : arith ? 4 : 2;
    /* Only a RIGHT shift can be arithmetic; `arith` says nothing about a
     * left one, and reading it there refused a perfectly good psllq. */
    if (!left && arith && esize == 8)
        internal_error("SSE2 has no arithmetic 64-bit packed shift");
    if (esize != 2 && esize != 4 && esize != 8)
        internal_error("no packed shift for %d-byte lanes", esize);
    code_byte(c, 0x66);
    if (xmm & 8) code_byte(c, 0x41);
    code_byte(c, 0x0f); code_byte(c, (unsigned)grp);
    code_byte(c, (unsigned)(0xc0 | (ext << 3) | (xmm & 7)));
    code_byte(c, (unsigned)(imm & 0xff));
}

/* xmm = xmm register-to-register (movdqa xmm, xmm). */
void x86_vmov_rr(struct code *c, int dst, int src)
{
    code_byte(c, 0x66);
    if ((dst & 8) || (src & 8))
        code_byte(c, (unsigned)(0x40 | ((dst & 8) ? 4 : 0) | ((src & 8) ? 1 : 0)));
    code_byte(c, 0x0f); code_byte(c, 0x6f);
    code_byte(c, (unsigned)(0xc0 | ((dst & 7) << 3) | (src & 7)));
}

/* pshufd xmm_dst, xmm_src, imm8 -- the lane permute both the splat and
 * the reduction are built from. */
void x86_vshufd(struct code *c, int dst, int src, int imm)
{
    code_byte(c, 0x66);
    if ((dst & 8) || (src & 8))
        code_byte(c, (unsigned)(0x40 | ((dst & 8) ? 4 : 0) | ((src & 8) ? 1 : 0)));
    code_byte(c, 0x0f); code_byte(c, 0x70);
    code_byte(c, (unsigned)(0xc0 | ((dst & 7) << 3) | (src & 7)));
    code_byte(c, (unsigned)(imm & 0xff));
}

/* movd/movq xmm, r  and  movd/movq r, xmm. */
void x86_vmov_xmm_reg(struct code *c, int xmm, int reg, int w)
{
    code_byte(c, 0x66);
    rex_rb(c, w == 8 ? 1 : 0, xmm, reg);
    code_byte(c, 0x0f); code_byte(c, 0x6e);
    code_byte(c, (unsigned)(0xc0 | ((xmm & 7) << 3) | (reg & 7)));
}

void x86_vmov_reg_xmm(struct code *c, int reg, int xmm, int w)
{
    code_byte(c, 0x66);
    rex_rb(c, w == 8 ? 1 : 0, xmm, reg);
    code_byte(c, 0x0f); code_byte(c, 0x7e);
    code_byte(c, (unsigned)(0xc0 | ((xmm & 7) << 3) | (reg & 7)));
}

/* The register-to-register form of the packed ALU, which the horizontal
 * reduction needs: it folds a vector against a permuted copy of itself,
 * and the copy is in a register, not a slot. */
void x86_vbin_rr(struct code *c, int dst, int src, int op, int esize)
{
    int opcode;
    switch (op) {
    case '+':
        opcode = esize == 1 ? 0xfc : esize == 2 ? 0xfd
               : esize == 4 ? 0xfe : 0xd4;
        break;
    case '-':
        opcode = esize == 1 ? 0xf8 : esize == 2 ? 0xf9
               : esize == 4 ? 0xfa : 0xfb;
        break;
    case '&': opcode = 0xdb; break;
    case '|': opcode = 0xeb; break;
    case '^': opcode = 0xef; break;
    default:
        internal_error("no SSE2 packed encoding for '%c'", op);
    }
    code_byte(c, 0x66);
    if ((dst & 8) || (src & 8))
        code_byte(c, (unsigned)(0x40 | ((dst & 8) ? 4 : 0) | ((src & 8) ? 1 : 0)));
    code_byte(c, 0x0f); code_byte(c, (unsigned)opcode);
    code_byte(c, (unsigned)(0xc0 | ((dst & 7) << 3) | (src & 7)));
}

/* punpck{l,h}{bw,wd,dq}: interleave the low (or high) half of two
 * vectors' lanes. With the second operand holding each lane's extension
 * bits -- all sign bits, or all zero -- this is how SSE2 widens: four
 * int32 lanes become two int64 ones, a half at a time. SSE4.1's
 * pmovsxdq would do it in one instruction, and is not SSE2. */
void x86_vunpck(struct code *c, int dst, int src, int high, int esize)
{
    int opcode;
    switch (esize) {
    case 1: opcode = high ? 0x68 : 0x60; break;   /* punpck?bw */
    case 2: opcode = high ? 0x69 : 0x61; break;   /* punpck?wd */
    case 4: opcode = high ? 0x6a : 0x62; break;   /* punpck?dq */
    default:
        internal_error("no packed unpack for %d-byte lanes", esize);
    }
    code_byte(c, 0x66);
    if ((dst & 8) || (src & 8))
        code_byte(c, (unsigned)(0x40 | ((dst & 8) ? 4 : 0) | ((src & 8) ? 1 : 0)));
    code_byte(c, 0x0f); code_byte(c, (unsigned)opcode);
    code_byte(c, (unsigned)(0xc0 | ((dst & 7) << 3) | (src & 7)));
}

/* test reg, reg -- set ZF from a value without changing it. */
void x86_test_rr(struct code *c, int a, int b, int w)
{
    rex_rb(c, w == 8 ? 1 : 0, b, a);
    code_byte(c, 0x85);
    code_byte(c, (unsigned)(0xc0 | ((b & 7) << 3) | (a & 7)));
}

/* cmovne reg, [rbp+disp] -- take the slot's value only if ZF is clear.
 * The whole point of a select: no branch, so no misprediction, and both
 * sides were already computed. */
void x86_cmovne_slot(struct code *c, int reg, int disp, int w)
{
    rex_rb(c, w == 8 ? 1 : 0, reg, REG_RBP);
    code_byte(c, 0x0f); code_byte(c, 0x45);
    modrm_rbp(c, reg, disp);
}

void x86_cmovne_rr(struct code *c, int dst, int src, int w)
{
    rex_rb(c, w == 8 ? 1 : 0, dst, src);
    code_byte(c, 0x0f); code_byte(c, 0x45);
    code_byte(c, (unsigned)(0xc0 | ((dst & 7) << 3) | (src & 7)));
}
