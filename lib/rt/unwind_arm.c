/* The stack unwinder for 32-bit ARM: the Exception Handling ABI for the
 * ARM Architecture (EHABI, IHI 0038), as GCC's libgcc implements it.
 *
 * ARM does not describe frames with DWARF CFI. Each function has an
 * entry in .ARM.exidx -- sorted by address, bracketed by __exidx_start
 * and __exidx_end -- whose second word is either the frame's unwinding
 * inline (personality routine 0, three opcode bytes), a pointer to an
 * .ARM.extab entry (more opcodes, a personality routine and its data:
 * C++'s LSDA), or EXIDX_CANTUNWIND. The opcodes undo the prologue on a
 * virtual register set -- "vsp += 16; pop {r4-r7, lr}" -- and a
 * personality routine runs them for its frame (__gnu_unwind_frame)
 * when it does not stop there.
 *
 * The C++ runtime's personality (lib/libcxx/src/eh.cc) reads its LSDA
 * and calls back into this for the frame; the compact routines pr0, pr1
 * and pr2 here only unwind. _Unwind_RaiseException and _Unwind_Resume
 * are entered through assembly stubs that capture the caller's core
 * registers (and d8-d15 with an FPU), so the walk starts at the throw.
 */
#include "rt.h"

#if defined(__ARM_EABI__) && !defined(__ARM_DWARF_EH__)

#include <unwind.h>

typedef unsigned int uw32;

/* The virtual register set: r0-r15, and the callee-saved VFP registers
 * d8-d15 (as eight bytes each) when there is an FPU. */
struct _Unwind_Context {
    uw32 r[16];
#if defined(__ARM_FP)
    unsigned long long d[16];
#endif
};
#define R_SP 13
#define R_LR 14
#define R_PC 15

/* the stubs: capture the caller's registers, then the C below */
void __uw_arm_raise(_Unwind_Control_Block *, struct _Unwind_Context *);
void __uw_arm_resume(_Unwind_Control_Block *, struct _Unwind_Context *);
void __uw_arm_restore(const struct _Unwind_Context *) __attribute__((noreturn));

/* Thumb-1 mnemonics, so one text serves ARMv6-M to ARMv8-M: r0 is the
 * control block, the registers go to a context on the stack, which is
 * r1 for the C function the stub calls.  The context's offsets: r[k] at
 * 4k, d8-d15 (d[8..15]) from 128. */
#if defined(__ARM_FP)
# define UW_SAVE_D "add r2, r1, #128\nvstmia r2, {s16-s31}\n"
# define UW_LOAD_D "add r1, r0, #128\nvldmia r1, {s16-s31}\n"
# define UW_CTX "192"
#else
# define UW_SAVE_D ""
# define UW_LOAD_D ""
# define UW_CTX "64"
#endif
#if defined(__thumb__)
#define UW_STUB(name, c_fn) \
    ".globl " name "\n" \
    ".type " name ", %function\n" \
    ".thumb_func\n" \
    name ":\n" \
    "push {r4, lr}\n" \
    "sub sp, #" UW_CTX "\n" \
    "mov r1, sp\n" \
    "str r4, [r1, #16]\n" \
    "str r5, [r1, #20]\n" \
    "str r6, [r1, #24]\n" \
    "str r7, [r1, #28]\n" \
    "mov r2, r8\n" "str r2, [r1, #32]\n" \
    "mov r2, r9\n" "str r2, [r1, #36]\n" \
    "mov r2, r10\n" "str r2, [r1, #40]\n" \
    "mov r2, r11\n" "str r2, [r1, #44]\n" \
    /* sp as the caller had it: above the context and the push */ \
    "add r2, sp, #" UW_CTX "\n" "adds r2, #8\n" "str r2, [r1, #52]\n" \
    "ldr r2, [sp, #" UW_CTX "]\n" "str r2, [r1, #16]\n"   /* r4, pushed */ \
    "mov r2, lr\n" "str r2, [r1, #56]\n" "str r2, [r1, #60]\n" \
    UW_SAVE_D \
    "bl " c_fn "\n" \
    "add sp, #" UW_CTX "\n" \
    "pop {r4, pc}\n"
__asm__(
    ".text\n"
    ".syntax unified\n"
    ".thumb\n"
    UW_STUB("_Unwind_RaiseException", "__uw_arm_raise")
    UW_STUB("_Unwind_Resume_or_Rethrow", "__uw_arm_raise")
    UW_STUB("_Unwind_Resume", "__uw_arm_resume")
    ".globl __uw_arm_restore\n"
    ".type __uw_arm_restore, %function\n"
    ".thumb_func\n"
    "__uw_arm_restore:\n"
    UW_LOAD_D
    "ldr r1, [r0, #32]\n" "mov r8, r1\n"
    "ldr r1, [r0, #36]\n" "mov r9, r1\n"
    "ldr r1, [r0, #40]\n" "mov r10, r1\n"
    "ldr r1, [r0, #44]\n" "mov r11, r1\n"
    "ldr r1, [r0, #48]\n" "mov r12, r1\n"
    "ldr r1, [r0, #56]\n" "mov lr, r1\n"
    "ldr r1, [r0, #52]\n" "mov sp, r1\n"
    "ldr r4, [r0, #16]\n"
    "ldr r5, [r0, #20]\n"
    "ldr r6, [r0, #24]\n"
    "ldr r7, [r0, #28]\n"
    "ldr r2, [r0, #60]\n"
    "ldr r3, [r0, #12]\n"
    "ldr r1, [r0, #4]\n"
    "ldr r0, [r0, #0]\n"
    "bx r2\n"
);

#else
/* ARM state (armv7a-none-eabi): the same stubs in A32, whose three-operand
 * forms reach r8-r12, lr and sp directly */
#define UW_STUB(name, c_fn) \
    ".globl " name "\n" \
    ".type " name ", %function\n" \
    name ":\n" \
    "push {r4, lr}\n" \
    "sub sp, sp, #" UW_CTX "\n" \
    "mov r1, sp\n" \
    "str r4, [r1, #16]\n" "str r5, [r1, #20]\n" \
    "str r6, [r1, #24]\n" "str r7, [r1, #28]\n" \
    "str r8, [r1, #32]\n" "str r9, [r1, #36]\n" \
    "str r10, [r1, #40]\n" "str r11, [r1, #44]\n" \
    "add r2, sp, #" UW_CTX "\n" "add r2, r2, #8\n" "str r2, [r1, #52]\n" \
    "ldr r2, [sp, #" UW_CTX "]\n" "str r2, [r1, #16]\n" \
    "str lr, [r1, #56]\n" "str lr, [r1, #60]\n" \
    UW_SAVE_D \
    "bl " c_fn "\n" \
    "add sp, sp, #" UW_CTX "\n" \
    "pop {r4, pc}\n"
__asm__(
    ".text\n"
    ".syntax unified\n"
    ".arm\n"
    UW_STUB("_Unwind_RaiseException", "__uw_arm_raise")
    UW_STUB("_Unwind_Resume_or_Rethrow", "__uw_arm_raise")
    UW_STUB("_Unwind_Resume", "__uw_arm_resume")
    ".globl __uw_arm_restore\n"
    ".type __uw_arm_restore, %function\n"
    "__uw_arm_restore:\n"
    UW_LOAD_D
    "ldr r8, [r0, #32]\n" "ldr r9, [r0, #36]\n"
    "ldr r10, [r0, #40]\n" "ldr r11, [r0, #44]\n"
    "ldr r12, [r0, #48]\n" "ldr lr, [r0, #56]\n"
    "ldr sp, [r0, #52]\n"
    "ldr r4, [r0, #16]\n" "ldr r5, [r0, #20]\n"
    "ldr r6, [r0, #24]\n" "ldr r7, [r0, #28]\n"
    "ldr r2, [r0, #60]\n" "ldr r3, [r0, #12]\n"
    "ldr r1, [r0, #4]\n" "ldr r0, [r0, #0]\n"
    "bx r2\n"
);
#endif

/* ---- the virtual register set -------------------------------------------- */

_Unwind_VRS_Result _Unwind_VRS_Get(_Unwind_Context *c, _Unwind_VRS_RegClass cl,
                                   _uw reg, _Unwind_VRS_DataRepresentation rep,
                                   void *v)
{
    if (cl == _UVRSC_CORE && rep == _UVRSD_UINT32 && reg < 16) {
        *(uw32 *)v = c->r[reg];
        return _UVRSR_OK;
    }
#if defined(__ARM_FP)
    if (cl == _UVRSC_VFP && rep == _UVRSD_DOUBLE && reg >= 8 && reg < 16) {
        *(unsigned long long *)v = c->d[reg];
        return _UVRSR_OK;
    }
#endif
    return _UVRSR_FAILED;
}

_Unwind_VRS_Result _Unwind_VRS_Set(_Unwind_Context *c, _Unwind_VRS_RegClass cl,
                                   _uw reg, _Unwind_VRS_DataRepresentation rep,
                                   void *v)
{
    if (cl == _UVRSC_CORE && rep == _UVRSD_UINT32 && reg < 16) {
        c->r[reg] = *(uw32 *)v;
        return _UVRSR_OK;
    }
#if defined(__ARM_FP)
    if (cl == _UVRSC_VFP && rep == _UVRSD_DOUBLE && reg >= 8 && reg < 16) {
        c->d[reg] = *(unsigned long long *)v;
        return _UVRSR_OK;
    }
#endif
    return _UVRSR_FAILED;
}

/* Pop from vsp: core registers by mask (r0-r15), or VFP registers
 * d[start]..d[start+count-1], the discriminator (start << 16) | count. */
_Unwind_VRS_Result _Unwind_VRS_Pop(_Unwind_Context *c, _Unwind_VRS_RegClass cl,
                                   _uw disc, _Unwind_VRS_DataRepresentation rep)
{
    uw32 *vsp = (uw32 *)c->r[R_SP];
    if (cl == _UVRSC_CORE) {
        int sp_popped = 0;
        uw32 newsp = 0;
        for (int i = 0; i < 16; i++)
            if (disc & (1u << i)) {
                if (i == R_SP) {
                    newsp = *vsp++;
                    sp_popped = 1;
                } else {
                    c->r[i] = *vsp++;
                }
            }
        c->r[R_SP] = sp_popped ? newsp : (uw32)vsp;
        return _UVRSR_OK;
    }
    if (cl == _UVRSC_VFP) {
        unsigned start = disc >> 16, count = disc & 0xffff;
        for (unsigned k = 0; k < count; k++) {
            unsigned long long v;
            __builtin_memcpy(&v, vsp, 8);
            vsp += 2;
#if defined(__ARM_FP)
            if (start + k >= 8 && start + k < 16)
                c->d[start + k] = v;
#endif
        }
        if (rep == _UVRSD_VFPX)
            vsp++;                         /* FSTMFDX's pad word */
        c->r[R_SP] = (uw32)vsp;
        return _UVRSR_OK;
    }
    return _UVRSR_FAILED;
}

/* ---- the unwind opcodes -------------------------------------------------- */

struct uw_bytes {
    const uw32 *w;      /* the next word */
    uw32 cur;           /* the bytes left of the current word, MSB first */
    int left;           /* how many of them */
    int words;          /* words still to come */
};

static int next_byte(struct uw_bytes *b)
{
    if (!b->left) {
        if (!b->words)
            return 0xb0;                   /* finish */
        b->cur = *b->w++;
        b->words--;
        b->left = 4;
    }
    int v = (int)(b->cur >> 24);
    b->cur <<= 8;
    b->left--;
    return v;
}

/* Run the opcodes for one frame. pc comes from lr unless an opcode set
 * it (the pop of r15). */
static _Unwind_Reason_Code execute(_Unwind_Context *c, struct uw_bytes *b)
{
    int set_pc = 0;
    for (;;) {
        int op = next_byte(b);
        if (op == 0xb0) {                                  /* finish */
            if (!set_pc)
                c->r[R_PC] = c->r[R_LR];
            return _URC_OK;
        }
        if ((op & 0xc0) == 0x00) {                         /* vsp += */
            c->r[R_SP] += ((uw32)(op & 0x3f) << 2) + 4;
        } else if ((op & 0xc0) == 0x40) {                  /* vsp -= */
            c->r[R_SP] -= ((uw32)(op & 0x3f) << 2) + 4;
        } else if ((op & 0xf0) == 0x80) {                  /* pop r4-r15 */
            uw32 mask = ((uw32)(op & 0x0f) << 8) | (uw32)next_byte(b);
            if (!mask)
                return _URC_FAILURE;                       /* refuse */
            mask <<= 4;
            _Unwind_VRS_Pop(c, _UVRSC_CORE, mask, _UVRSD_UINT32);
            if (mask & (1u << R_PC))
                set_pc = 1;
        } else if ((op & 0xf0) == 0x90) {                  /* vsp = r[n] */
            int n = op & 0x0f;
            if (n == 13 || n == 15)
                return _URC_FAILURE;
            c->r[R_SP] = c->r[n];
        } else if ((op & 0xf0) == 0xa0) {                  /* pop r4-r[4+n] */
            uw32 mask = ((1u << ((op & 7) + 1)) - 1) << 4;
            if (op & 8)
                mask |= 1u << R_LR;
            _Unwind_VRS_Pop(c, _UVRSC_CORE, mask, _UVRSD_UINT32);
        } else if (op == 0xb1) {                           /* pop r0-r3 */
            uw32 mask = (uw32)next_byte(b);
            if (!mask || (mask & 0xf0))
                return _URC_FAILURE;
            _Unwind_VRS_Pop(c, _UVRSC_CORE, mask, _UVRSD_UINT32);
        } else if (op == 0xb2) {                           /* vsp += uleb */
            uw32 v = 0;
            int shift = 0, byte;
            do {
                byte = next_byte(b);
                v |= (uw32)(byte & 0x7f) << shift;
                shift += 7;
            } while (byte & 0x80);
            c->r[R_SP] += 0x204 + (v << 2);
        } else if (op == 0xb3) {                           /* FSTMFDX d[s]..*/
            int sc = next_byte(b);
            _Unwind_VRS_Pop(c, _UVRSC_VFP,
                            ((uw32)(sc >> 4) << 16) | ((uw32)(sc & 15) + 1),
                            _UVRSD_VFPX);
        } else if ((op & 0xf8) == 0xb8) {                  /* FSTMFDX d8.. */
            _Unwind_VRS_Pop(c, _UVRSC_VFP, (8u << 16) | ((uw32)(op & 7) + 1),
                            _UVRSD_VFPX);
        } else if (op == 0xc8 || op == 0xc9) {             /* VPUSH d[..] */
            int sc = next_byte(b);
            uw32 start = (uw32)(sc >> 4) + (op == 0xc8 ? 16 : 0);
            _Unwind_VRS_Pop(c, _UVRSC_VFP,
                            (start << 16) | ((uw32)(sc & 15) + 1),
                            _UVRSD_DOUBLE);
        } else if ((op & 0xf8) == 0xd0) {                  /* VPUSH d8.. */
            _Unwind_VRS_Pop(c, _UVRSC_VFP, (8u << 16) | ((uw32)(op & 7) + 1),
                            _UVRSD_DOUBLE);
        } else {
            return _URC_FAILURE;                           /* spare, iWMMX */
        }
    }
}

/* The frame the control block's pr_cache names, unwound: what a
 * personality routine calls when its frame is not where it stops. The
 * table (ehtp) is either the exidx word itself (additional 1: the short
 * form's three opcodes) or an extab entry: a personality pointer, then
 * a word whose top byte counts the opcode words after it -- GCC's own
 * format for its personality routines. */
_Unwind_Reason_Code __gnu_unwind_frame(_Unwind_Control_Block *ucbp,
                                       _Unwind_Context *c)
{
    struct uw_bytes b;
    const uw32 *p = ucbp->pr_cache.ehtp;
    b.w = p + 2;
    b.cur = p[1] << 8;
    b.left = 3;
    b.words = (int)(p[1] >> 24);
    return execute(c, &b) == _URC_OK ? _URC_OK : _URC_FAILURE;
}

/* The compact personality routines: only unwind. pr0's opcodes are the
 * word's three low bytes; pr1's and pr2's follow two bytes and a count
 * in the extab word. */
static _Unwind_Reason_Code compact(int idx, _Unwind_State st,
                                   _Unwind_Control_Block *ucbp,
                                   _Unwind_Context *c)
{
    (void)st;
    const uw32 *p = ucbp->pr_cache.ehtp;
    struct uw_bytes b;
    if (idx == 0) {
        b.cur = p[0] << 8;
        b.left = 3;
        b.words = 0;
        b.w = p + 1;
    } else {
        b.cur = p[0] << 16;
        b.left = 2;
        b.words = (int)((p[0] >> 16) & 0xff);
        b.w = p + 1;
    }
    return execute(c, &b) == _URC_OK ? _URC_CONTINUE_UNWIND : _URC_FAILURE;
}

_Unwind_Reason_Code __aeabi_unwind_cpp_pr0(_Unwind_State st,
                                           _Unwind_Control_Block *u,
                                           _Unwind_Context *c)
{
    return compact(0, st, u, c);
}

_Unwind_Reason_Code __aeabi_unwind_cpp_pr1(_Unwind_State st,
                                           _Unwind_Control_Block *u,
                                           _Unwind_Context *c)
{
    return compact(1, st, u, c);
}

_Unwind_Reason_Code __aeabi_unwind_cpp_pr2(_Unwind_State st,
                                           _Unwind_Control_Block *u,
                                           _Unwind_Context *c)
{
    return compact(2, st, u, c);
}

/* ---- finding a frame's entry --------------------------------------------- */

extern const uw32 __exidx_start[] __attribute__((weak));
extern const uw32 __exidx_end[] __attribute__((weak));

static uw32 prel31(const uw32 *p)
{
    uw32 off = *p & 0x7fffffffu;
    if (off & 0x40000000u)
        off |= 0x80000000u;               /* sign-extend the 31 bits */
    return (uw32)p + off;
}

typedef _Unwind_Reason_Code (*pr_fn)(_Unwind_State, _Unwind_Control_Block *,
                                     _Unwind_Context *);

/* The entry for pc (the address of a call's bytes, not its return):
 * fills the control block's pr_cache and answers the personality. NULL
 * at the end of what the tables describe (CANTUNWIND, or none). */
static pr_fn lookup(_Unwind_Control_Block *ucbp, uw32 pc)
{
    const uw32 *lo = __exidx_start, *hi = __exidx_end;
    if (!lo || hi <= lo)
        return 0;
    long n = (long)(hi - lo) / 2, a = 0, z = n - 1, at = -1;
    while (a <= z) {                       /* the last entry <= pc */
        long m = (a + z) / 2;
        if (prel31(&lo[2 * m]) <= pc) {
            at = m;
            a = m + 1;
        } else {
            z = m - 1;
        }
    }
    if (at < 0)
        return 0;
    const uw32 *e = &lo[2 * at];
    ucbp->pr_cache.fnstart = prel31(e);
    if (e[1] == 1)
        return 0;                          /* EXIDX_CANTUNWIND */
    const uw32 *ehtp;
    if (e[1] & 0x80000000u) {
        ehtp = &e[1];
        ucbp->pr_cache.additional = 1;
    } else {
        ehtp = (const uw32 *)prel31(&e[1]);
        ucbp->pr_cache.additional = 0;
    }
    ucbp->pr_cache.ehtp = (_uw *)ehtp;
    if (*ehtp & 0x80000000u) {
        switch ((*ehtp >> 24) & 0x0f) {
        case 0: return __aeabi_unwind_cpp_pr0;
        case 1: return __aeabi_unwind_cpp_pr1;
        case 2: return __aeabi_unwind_cpp_pr2;
        default: return 0;
        }
    }
    return (pr_fn)prel31(ehtp);
}

/* ---- the two phases ------------------------------------------------------ */

static _Unwind_Reason_Code phase2(_Unwind_Control_Block *ucbp,
                                  _Unwind_Context *c, _Unwind_State first)
{
    _Unwind_State st = first;
    for (int depth = 0; depth < 10000; depth++) {
        pr_fn pers = lookup(ucbp, (c->r[R_PC] & ~1u) - 2);
        if (!pers)
            return _URC_FAILURE;
        /* the frame is identified by vsp on entry to it, as the search
         * recorded the handler's (barrier_cache.sp) */
        _Unwind_Reason_Code r = pers(st, ucbp, c);
        st = _US_UNWIND_FRAME_STARTING;
        if (r == _URC_INSTALL_CONTEXT) {
#if defined(__thumb__)
            c->r[R_PC] |= 1;               /* Thumb: every function is */
#endif
            __uw_arm_restore(c);           /* (ARM state: as SetIP kept it) */
        }
        if (r != _URC_CONTINUE_UNWIND)
            return _URC_FAILURE;
    }
    return _URC_FAILURE;
}

void __uw_arm_raise(_Unwind_Control_Block *ucbp, _Unwind_Context *entry)
{
    _Unwind_Context c = *entry;
    /* the phase-one walk on a copy: it changes nothing */
    for (int depth = 0; depth < 10000; depth++) {
        pr_fn pers = lookup(ucbp, (c.r[R_PC] & ~1u) - 2);
        if (!pers)
            return;                        /* end of stack: no handler */
        _Unwind_Reason_Code r = pers(_US_VIRTUAL_UNWIND_FRAME, ucbp, &c);
        if (r == _URC_HANDLER_FOUND)
            break;
        if (r != _URC_CONTINUE_UNWIND)
            return;
    }
    c = *entry;
    phase2(ucbp, &c, _US_UNWIND_FRAME_STARTING);
}

/* A cleanup's landing pad has run and calls _Unwind_Resume: the frame it
 * is in carries on unwinding (its personality, told RESUME, does not run
 * the cleanup again) and phase two goes on to its callers. */
void __uw_arm_resume(_Unwind_Control_Block *ucbp, _Unwind_Context *entry)
{
    _Unwind_Context c = *entry;
    phase2(ucbp, &c, _US_UNWIND_FRAME_RESUME);
    __builtin_trap();
}

/* _Unwind_Resume_or_Rethrow is a stub of its own, above: a C function
 * calling _Unwind_RaiseException put a frame between the rethrowing code
 * and the walk -- on ARMv6-M, where it is no tail call, the walk began in
 * that frame, which no table describes, and popped garbage. */

void _Unwind_DeleteException(_Unwind_Control_Block *ucbp)
{
    if (ucbp->exception_cleanup)
        ucbp->exception_cleanup(_URC_FOREIGN_EXCEPTION_CAUGHT, ucbp);
}

void _Unwind_Complete(_Unwind_Control_Block *ucbp)
{
    (void)ucbp;
}

/* ---- the Itanium-shaped accessors EmbCC's C++ runtime uses ------------- */

_Unwind_Word _Unwind_GetGR(_Unwind_Context *c, int i)
{
    return i >= 0 && i < 16 ? c->r[i] : 0;
}

void _Unwind_SetGR(_Unwind_Context *c, int i, _Unwind_Word v)
{
    if (i >= 0 && i < 16)
        c->r[i] = (uw32)v;
}

_Unwind_Ptr _Unwind_GetIP(_Unwind_Context *c)
{
    return c->r[R_PC] & ~1u;
}

_Unwind_Ptr _Unwind_GetIPInfo(_Unwind_Context *c, int *before)
{
    *before = 0;
    return c->r[R_PC] & ~1u;
}

/* The frame's own instruction set stays: its pc's bit 0 (the return
 * address it was reached by) is set in Thumb code and clear in ARM code,
 * and the landing pad is in the same function. Setting it always sent
 * an ARM-state handler into Thumb state. */
void _Unwind_SetIP(_Unwind_Context *c, _Unwind_Ptr v)
{
    c->r[R_PC] = ((uw32)v & ~1u) | (c->r[R_PC] & 1u);
}

_Unwind_Word _Unwind_GetCFA(_Unwind_Context *c)
{
    return c->r[R_SP];
}

/* The LSDA and the function's start, from the control block the
 * personality left in r12: past the personality pointer and the opcode
 * words of its extab entry; the exidx entry's address. */
void *_Unwind_GetLanguageSpecificData(_Unwind_Context *c)
{
    const _Unwind_Control_Block *u = (const _Unwind_Control_Block *)c->r[12];
    const uw32 *p = (const uw32 *)u->pr_cache.ehtp;
    p++;
    p += ((*p >> 24) & 0xff) + 1;
    return (void *)p;
}

_Unwind_Ptr _Unwind_GetRegionStart(_Unwind_Context *c)
{
    const _Unwind_Control_Block *u = (const _Unwind_Control_Block *)c->r[12];
    return u->pr_cache.fnstart;
}

_Unwind_Ptr _Unwind_GetDataRelBase(_Unwind_Context *c) { (void)c; return 0; }
_Unwind_Ptr _Unwind_GetTextRelBase(_Unwind_Context *c) { (void)c; return 0; }

#endif /* ARM EHABI */
