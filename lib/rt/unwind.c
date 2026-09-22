/* The stack unwinder: DWARF CFI, and the Itanium ABI's level-1 API.
 *
 * What `throw` actually does. The compiler turns a throw into a call to
 * `__cxa_throw`, which calls `_Unwind_RaiseException` here, and from
 * that moment the C++ runtime is a passenger: this walks back up the
 * stack, asks each frame's personality routine whether it wants the
 * exception, and eventually puts the machine back the way that frame
 * left it and jumps into its landing pad.
 *
 * ---- why it is here rather than nowhere ----------------------------------
 *
 * Every other target EmbCC has gets this from the platform -- libgcc on
 * EmbLinkOS, libunwind inside libSystem on macOS. The static Linux
 * target links against nothing, so it had neither, and a C++ program
 * that threw compiled and then failed at the link with an undefined
 * `_Unwind_Resume`. D-014's second amendment called this the harder
 * half and said it lands only against a real differential oracle;
 * `tests/golden/unwind.sh` is that oracle, running the same programs
 * against libgcc's unwinder and against this one.
 *
 * ---- the two phases ------------------------------------------------------
 *
 * The ABI unwinds TWICE, and the reason is not efficiency. Phase one
 * walks up asking "does any frame handle this?" and changes nothing.
 * Only if some frame says yes does phase two walk up again, running
 * destructors as it goes and stopping at the frame phase one found. If
 * it were one pass, a throw with no handler would have destroyed half
 * the stack before discovering there was nowhere to land -- and
 * `std::terminate` is required to run with the throw point still on the
 * stack, which is what makes a core dump useful.
 *
 * ---- the tables ----------------------------------------------------------
 *
 * `.eh_frame` is a sequence of CIE and FDE records. An FDE covers a
 * range of code and holds a program -- DWARF call-frame instructions --
 * that says, for any PC in that range, where the caller's registers
 * went. Running that program to the PC in question yields a table of
 * rules, and applying the rules yields the caller's frame. That is the
 * whole idea; everything below is bookkeeping.
 *
 * The section is found by the bracket symbols `__eh_frame_start` and
 * `__eh_frame_end`. EmbLD defines them for any orphan section whose
 * name is an identifier, so a static image needs no `dl_iterate_phdr`
 * and no `.eh_frame_hdr` search table -- the FDEs are scanned linearly.
 * That is O(n) per frame and the honest starting point: correct for
 * every layout, including the ones a binary search over an unsorted
 * table gets wrong.
 */
#include "rt.h"

#if defined(__linux__) || defined(__EMBCC_LINUX__)

typedef unsigned long uw_word;
typedef long uw_sword;

/* ---- the register file ---------------------------------------------------
 *
 * Indexed by DWARF register number, which is the numbering the CFI
 * instructions use. Only the callee-saved ones and the stack pointer
 * are ever meaningful during a walk -- a caller's scratch registers are
 * gone and no rule refers to them -- but the array is the full width so
 * the interpreter never has to translate. */
#ifdef __x86_64__
# define UW_NREG   17
# define UW_RA     16          /* the return-address column */
# define UW_SP     7           /* rsp */
# define UW_LP0    0           /* rax: where the landing pad reads the
                                * exception pointer */
# define UW_LP1    1           /* rdx: and the selector */
static const unsigned char uw_captured[] = { 3, 6, 12, 13, 14, 15 };
#else
# define UW_NREG   32
# define UW_RA     30          /* x30, the link register */
# define UW_SP     31
# define UW_LP0    0           /* x0 */
# define UW_LP1    1           /* x1 */
static const unsigned char uw_captured[] = {
    19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
};
#endif

#define UW_NCAPT ((int)(sizeof uw_captured / sizeof uw_captured[0]))

/* What the capture stub writes: the callee-saved registers in the order
 * above, then the stack pointer as the CALLER will see it, then the
 * return address. The resume stub reads the same shape plus two more
 * words for the landing pad's arguments. */
struct uw_capture {
    uw_word saved[UW_NCAPT];
    uw_word sp;
    uw_word pc;
    uw_word lp0, lp1;
};

void __uw_capture(struct uw_capture *out);
void __uw_resume(struct uw_capture *regs) __attribute__((noreturn));

/* ---- the stubs -----------------------------------------------------------
 *
 * Bytes rather than mnemonics, assembled by a real assembler and pasted
 * here with the source beside them -- the same discipline
 * `lib/libc/os/linux/thread.c` uses for its clone entry, and for the
 * same reason: a hand-encoded instruction that is wrong assembles, links
 * and crashes somewhere else entirely.
 *
 * `__uw_capture` saves the state of ITS CALLER, which is where the walk
 * begins. `__uw_resume` is the opposite and never returns: it loads
 * every register from the buffer FIRST and only then switches the stack,
 * so the buffer may live on the stack that is about to be abandoned.
 */
#ifdef __x86_64__
__asm__(
".text\n"
".globl __uw_capture\n"
".type __uw_capture,@function\n"
"__uw_capture:\n"
/*  mov %rbx,0(%rdi)   */ ".byte 0x48,0x89,0x1f\n"
/*  mov %rbp,8(%rdi)   */ ".byte 0x48,0x89,0x6f,0x08\n"
/*  mov %r12,16(%rdi)  */ ".byte 0x4c,0x89,0x67,0x10\n"
/*  mov %r13,24(%rdi)  */ ".byte 0x4c,0x89,0x6f,0x18\n"
/*  mov %r14,32(%rdi)  */ ".byte 0x4c,0x89,0x77,0x20\n"
/*  mov %r15,40(%rdi)  */ ".byte 0x4c,0x89,0x7f,0x28\n"
/*  lea 8(%rsp),%rax   */ ".byte 0x48,0x8d,0x44,0x24,0x08\n"
/*  mov %rax,48(%rdi)  */ ".byte 0x48,0x89,0x47,0x30\n"
/*  mov (%rsp),%rax    */ ".byte 0x48,0x8b,0x04,0x24\n"
/*  mov %rax,56(%rdi)  */ ".byte 0x48,0x89,0x47,0x38\n"
/*  ret                */ ".byte 0xc3\n"
".size __uw_capture,.-__uw_capture\n"
".globl __uw_resume\n"
".type __uw_resume,@function\n"
"__uw_resume:\n"
/*  mov 0(%rdi),%rbx   */ ".byte 0x48,0x8b,0x1f\n"
/*  mov 8(%rdi),%rbp   */ ".byte 0x48,0x8b,0x6f,0x08\n"
/*  mov 16(%rdi),%r12  */ ".byte 0x4c,0x8b,0x67,0x10\n"
/*  mov 24(%rdi),%r13  */ ".byte 0x4c,0x8b,0x6f,0x18\n"
/*  mov 32(%rdi),%r14  */ ".byte 0x4c,0x8b,0x77,0x20\n"
/*  mov 40(%rdi),%r15  */ ".byte 0x4c,0x8b,0x7f,0x28\n"
/*  mov 64(%rdi),%rax  */ ".byte 0x48,0x8b,0x47,0x40\n"
/*  mov 72(%rdi),%rdx  */ ".byte 0x48,0x8b,0x57,0x48\n"
/*  mov 56(%rdi),%rcx  */ ".byte 0x48,0x8b,0x4f,0x38\n"
/*  mov 48(%rdi),%rsi  */ ".byte 0x48,0x8b,0x77,0x30\n"
/*  mov %rsi,%rsp      */ ".byte 0x48,0x89,0xf4\n"
/*  jmp *%rcx          */ ".byte 0xff,0xe1\n"
".size __uw_resume,.-__uw_resume\n"
);
#else
__asm__(
".text\n"
".globl __uw_capture\n"
".type __uw_capture,%function\n"
"__uw_capture:\n"
/*  stp x19,x20,[x0]      */ ".long 0xa9005013\n"
/*  stp x21,x22,[x0,#16]  */ ".long 0xa9015815\n"
/*  stp x23,x24,[x0,#32]  */ ".long 0xa9026017\n"
/*  stp x25,x26,[x0,#48]  */ ".long 0xa9036819\n"
/*  stp x27,x28,[x0,#64]  */ ".long 0xa904701b\n"
/*  stp x29,x30,[x0,#80]  */ ".long 0xa905781d\n"
/*  mov x1,sp             */ ".long 0x910003e1\n"
/*  str x1,[x0,#96]       */ ".long 0xf9003001\n"
/*  str x30,[x0,#104]     */ ".long 0xf900341e\n"
/*  ret                   */ ".long 0xd65f03c0\n"
".size __uw_capture,.-__uw_capture\n"
".globl __uw_resume\n"
".type __uw_resume,%function\n"
"__uw_resume:\n"
/*  ldp x19,x20,[x0]      */ ".long 0xa9405013\n"
/*  ldp x21,x22,[x0,#16]  */ ".long 0xa9415815\n"
/*  ldp x23,x24,[x0,#32]  */ ".long 0xa9426017\n"
/*  ldp x25,x26,[x0,#48]  */ ".long 0xa9436819\n"
/*  ldp x27,x28,[x0,#64]  */ ".long 0xa944701b\n"
/*  ldp x29,x30,[x0,#80]  */ ".long 0xa945781d\n"
/*  ldr x2,[x0,#96]       */ ".long 0xf9403002\n"
/*  ldr x3,[x0,#104]      */ ".long 0xf9403403\n"
/*  ldp x4,x5,[x0,#112]   */ ".long 0xa9471404\n"
/*  mov sp,x2             */ ".long 0x9100005f\n"
/*  mov x0,x4             */ ".long 0xaa0403e0\n"
/*  mov x1,x5             */ ".long 0xaa0503e1\n"
/*  br  x3                */ ".long 0xd61f0060\n"
".size __uw_resume,.-__uw_resume\n"
);
#endif

/* ---- the API's types, as unwind.h declares them ------------------------- */

typedef int _Unwind_Reason_Code;
#define _URC_NO_REASON                0
#define _URC_FOREIGN_EXCEPTION_CAUGHT 1
#define _URC_FATAL_PHASE2_ERROR       2
#define _URC_FATAL_PHASE1_ERROR       3
#define _URC_NORMAL_STOP              4
#define _URC_END_OF_STACK             5
#define _URC_HANDLER_FOUND            6
#define _URC_INSTALL_CONTEXT          7
#define _URC_CONTINUE_UNWIND          8

#define _UA_SEARCH_PHASE  1
#define _UA_CLEANUP_PHASE 2
#define _UA_HANDLER_FRAME 4
#define _UA_FORCE_UNWIND  8

struct _Unwind_Exception;
typedef void (*_Unwind_Exception_Cleanup_Fn)(int, struct _Unwind_Exception *);

struct _Unwind_Exception {
    unsigned long exception_class;
    _Unwind_Exception_Cleanup_Fn exception_cleanup;
    unsigned long private_1;
    unsigned long private_2;
} __attribute__((aligned(16)));

/* The context handed to a personality routine. It is this unwinder's
 * frame state: the registers as the frame left them, where its CFA is,
 * and the two things the personality has to be able to write -- the
 * landing pad's arguments and the address to resume at. */
struct _Unwind_Context {
    uw_word reg[UW_NREG];
    uw_word cfa;
    uw_word pc;                    /* the return address into this frame */
    uw_word func_start;
    const unsigned char *lsda;
    uw_word lp0, lp1;              /* what _Unwind_SetGR(0/1) stored */
    uw_word landing_pad;           /* what _Unwind_SetIP stored */
    int have_landing_pad;
    int signal_frame;
};

typedef _Unwind_Reason_Code (*personality_fn)(int, int, unsigned long,
                                              struct _Unwind_Exception *,
                                              struct _Unwind_Context *);

/* ---- reading the tables --------------------------------------------------
 *
 * Every field below is little-endian and may be unaligned, so each read
 * is byte by byte. An unaligned load is fine on both machines EmbCC
 * targets, but .eh_frame is a format and not a struct, and reading it
 * as one is how a port to a third machine breaks silently. */
static uw_word rd_u8(const unsigned char **p)  { return *(*p)++; }

static uw_word rd_u16(const unsigned char **p)
{
    uw_word v = (uw_word)(*p)[0] | ((uw_word)(*p)[1] << 8);
    *p += 2;
    return v;
}

static uw_word rd_u32(const unsigned char **p)
{
    uw_word v = 0;
    int i;
    for (i = 0; i < 4; i++) v |= (uw_word)(*p)[i] << (8 * i);
    *p += 4;
    return v;
}

static uw_word rd_u64(const unsigned char **p)
{
    uw_word v = 0;
    int i;
    for (i = 0; i < 8; i++) v |= (uw_word)(*p)[i] << (8 * i);
    *p += 8;
    return v;
}

static uw_word rd_uleb(const unsigned char **p)
{
    uw_word v = 0;
    int shift = 0;
    unsigned char b;
    do {
        b = *(*p)++;
        v |= (uw_word)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    return v;
}

static uw_sword rd_sleb(const unsigned char **p)
{
    uw_word v = 0;
    int shift = 0;
    unsigned char b;
    do {
        b = *(*p)++;
        v |= (uw_word)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40))
        v |= ~(uw_word)0 << shift;     /* sign-extend */
    return (uw_sword)v;
}

/* The DW_EH_PE_* pointer encodings. The low nibble is the format, the
 * high nibble says what it is relative to -- and `pcrel` means relative
 * to the address of the ENCODED FIELD, not to the record, which is the
 * detail that makes every pointer read need to know where it started. */
#define DW_EH_PE_omit     0xFF
#define DW_EH_PE_uleb128  0x01
#define DW_EH_PE_udata2   0x02
#define DW_EH_PE_udata4   0x03
#define DW_EH_PE_udata8   0x04
#define DW_EH_PE_sleb128  0x09
#define DW_EH_PE_sdata2   0x0A
#define DW_EH_PE_sdata4   0x0B
#define DW_EH_PE_sdata8   0x0C
#define DW_EH_PE_pcrel    0x10
#define DW_EH_PE_textrel  0x20
#define DW_EH_PE_datarel  0x30
#define DW_EH_PE_funcrel  0x40
#define DW_EH_PE_aligned  0x50
#define DW_EH_PE_indirect 0x80

static uw_word rd_encoded(const unsigned char **p, unsigned char enc)
{
    const unsigned char *base = *p;
    uw_word v;

    if (enc == DW_EH_PE_omit)
        return 0;
    if ((enc & 0x0F) == DW_EH_PE_aligned) {
        uw_word a = ((uw_word)*p + 7) & ~(uw_word)7;
        *p = (const unsigned char *)a;
        v = rd_u64(p);
        return v;
    }
    switch (enc & 0x0F) {
    case DW_EH_PE_uleb128: v = rd_uleb(p); break;
    case DW_EH_PE_udata2:  v = rd_u16(p); break;
    case DW_EH_PE_udata4:  v = rd_u32(p); break;
    case DW_EH_PE_udata8:  v = rd_u64(p); break;
    case DW_EH_PE_sleb128: v = (uw_word)rd_sleb(p); break;
    case DW_EH_PE_sdata2:  v = (uw_word)(long)(short)rd_u16(p); break;
    case DW_EH_PE_sdata4:  v = (uw_word)(long)(int)rd_u32(p); break;
    case DW_EH_PE_sdata8:  v = rd_u64(p); break;
    default:               v = rd_u64(p); break;   /* absptr */
    }
    if (v == 0)
        return 0;                      /* a null pointer stays null */
    switch (enc & 0x70) {
    case DW_EH_PE_pcrel: v += (uw_word)base; break;
    case 0: default: break;            /* absolute; the others do not
                                        * occur in a static image, and
                                        * are left alone rather than
                                        * guessed at */
    }
    if (enc & DW_EH_PE_indirect)
        v = *(const uw_word *)v;
    return v;
}

/* ---- CIE and FDE --------------------------------------------------------- */

struct cie {
    uw_word code_align;
    uw_sword data_align;
    unsigned ra_reg;
    unsigned char fde_enc;
    unsigned char lsda_enc;
    personality_fn personality;
    const unsigned char *insns;
    const unsigned char *insns_end;
    int has_z;
};

struct fde {
    uw_word start, range;
    const unsigned char *lsda;
    const unsigned char *insns;
    const unsigned char *insns_end;
    struct cie cie;
};

static int parse_cie(const unsigned char *p, const unsigned char *end,
                     struct cie *out)
{
    uw_word len;
    const unsigned char *rec_end;
    unsigned char version;
    const char *aug;

    if (end - p < 4) return 0;
    len = rd_u32(&p);
    if (len == 0xFFFFFFFFUL) {
        if (end - p < 8) return 0;
        len = rd_u64(&p);
    }
    rec_end = p + len;
    if (rec_end > end) return 0;
    if ((end - p) < 4) return 0;
    rd_u32(&p);                        /* CIE id, zero */
    version = (unsigned char)rd_u8(&p);
    if (version != 1 && version != 3 && version != 4)
        return 0;
    aug = (const char *)p;
    while (*p) p++;
    p++;

    if (version == 4) {
        rd_u8(&p);                     /* address size */
        rd_u8(&p);                     /* segment size */
    }
    out->code_align = rd_uleb(&p);
    out->data_align = rd_sleb(&p);
    out->ra_reg = version == 1 ? (unsigned)rd_u8(&p) : (unsigned)rd_uleb(&p);
    out->fde_enc = 0;                  /* absptr, the default */
    out->lsda_enc = DW_EH_PE_omit;
    out->personality = 0;
    out->has_z = aug[0] == 'z';

    if (out->has_z) {
        uw_word alen = rd_uleb(&p);
        const unsigned char *aend = p + alen;
        const char *a;
        for (a = aug + 1; *a; a++) {
            if (*a == 'P') {
                unsigned char penc = (unsigned char)rd_u8(&p);
                out->personality = (personality_fn)rd_encoded(&p, penc);
            } else if (*a == 'L') {
                out->lsda_enc = (unsigned char)rd_u8(&p);
            } else if (*a == 'R') {
                out->fde_enc = (unsigned char)rd_u8(&p);
            } else if (*a == 'S') {
                /* a signal frame: the return address IS the interrupted
                 * PC rather than one past a call, so the lookup below
                 * must not subtract one */
            } else {
                break;                 /* an augmentation we do not know */
            }
        }
        p = aend;
    }
    out->insns = p;
    out->insns_end = rec_end;
    return 1;
}

/* Find the FDE covering `pc`, scanning .eh_frame from the front.
 *
 * Linear, and deliberately so for now: a binary search needs
 * .eh_frame_hdr's sorted table, which nothing in this toolchain emits
 * yet, and searching an unsorted table is how an unwinder finds the
 * wrong frame and unwinds into it. */
extern const unsigned char __eh_frame_start[] __attribute__((weak));
extern const unsigned char __eh_frame_end[] __attribute__((weak));

static int find_fde(uw_word pc, struct fde *out)
{
    const unsigned char *p = __eh_frame_start, *end = __eh_frame_end;

    if (!p || !end)
        return 0;
    while (p < end) {
        const unsigned char *rec = p, *rec_end;
        uw_word len;
        uw_word cie_off;
        const unsigned char *lenfield;

        if (end - p < 4) break;
        lenfield = p;
        len = rd_u32(&p);
        if (len == 0) break;               /* the terminator */
        if (len == 0xFFFFFFFFUL) {
            if (end - p < 8) break;
            len = rd_u64(&p);
        }
        rec_end = p + len;
        if (rec_end > end) break;

        if (end - p < 4) break;
        cie_off = rd_u32(&p);
        if (cie_off == 0) {                /* a CIE, not an FDE */
            p = rec_end;
            continue;
        }
        /* In .eh_frame the pointer is a BACKWARD offset from the field
         * itself -- not a section offset, which is what .debug_frame
         * uses, and mixing the two reads a CIE from the middle of an
         * FDE. */
        {
            const unsigned char *cie_at = (p - 4) - cie_off;
            struct cie cie;
            uw_word start, range;
            const unsigned char *q = p;

            if (cie_at < __eh_frame_start || cie_at >= end) { p = rec_end;
                                                             continue; }
            if (!parse_cie(cie_at, end, &cie)) { p = rec_end; continue; }

            start = rd_encoded(&q, cie.fde_enc);
            /* The range uses the same format but is never relative to
             * anything: it is a length. */
            range = rd_encoded(&q, (unsigned char)(cie.fde_enc & 0x0F));
            if (pc >= start && pc < start + range) {
                out->start = start;
                out->range = range;
                out->cie = cie;
                out->lsda = 0;
                if (cie.has_z) {
                    uw_word alen = rd_uleb(&q);
                    const unsigned char *aend = q + alen;
                    if (cie.lsda_enc != DW_EH_PE_omit) {
                        uw_word l = rd_encoded(&q, cie.lsda_enc);
                        out->lsda = (const unsigned char *)l;
                    }
                    q = aend;
                }
                out->insns = q;
                out->insns_end = rec_end;
                return 1;
            }
            (void)lenfield; (void)rec;
        }
        p = rec_end;
    }
    return 0;
}

/* ---- the rule table ------------------------------------------------------ */

enum rule_kind {
    RULE_UNDEFINED = 0,   /* the caller's copy is gone */
    RULE_SAME,            /* unchanged: it is still in the register */
    RULE_OFFSET,          /* saved at CFA + off */
    RULE_VAL_OFFSET,      /* the VALUE is CFA + off */
    RULE_REGISTER         /* moved to another register */
};

struct rule { unsigned char kind; uw_sword off; };

struct cfi_state {
    struct rule reg[UW_NREG];
    int cfa_reg;
    uw_sword cfa_off;
    int cfa_is_expr;                   /* refused, not guessed at */
};

/* The DWARF call-frame instructions, DWARF 5 §6.4.2. */
#define DW_CFA_advance_loc        0x40
#define DW_CFA_offset             0x80
#define DW_CFA_restore            0xC0
#define DW_CFA_nop                0x00
#define DW_CFA_set_loc            0x01
#define DW_CFA_advance_loc1       0x02
#define DW_CFA_advance_loc2       0x03
#define DW_CFA_advance_loc4       0x04
#define DW_CFA_offset_extended    0x05
#define DW_CFA_restore_extended   0x06
#define DW_CFA_undefined          0x07
#define DW_CFA_same_value         0x08
#define DW_CFA_register           0x09
#define DW_CFA_remember_state     0x0A
#define DW_CFA_restore_state      0x0B
#define DW_CFA_def_cfa            0x0C
#define DW_CFA_def_cfa_register   0x0D
#define DW_CFA_def_cfa_offset     0x0E
#define DW_CFA_def_cfa_expression 0x0F
#define DW_CFA_expression         0x10
#define DW_CFA_offset_extended_sf 0x11
#define DW_CFA_def_cfa_sf         0x12
#define DW_CFA_def_cfa_offset_sf  0x13
#define DW_CFA_val_offset         0x14
#define DW_CFA_val_offset_sf      0x15
#define DW_CFA_val_expression     0x16
#define DW_CFA_GNU_args_size      0x2E
#define DW_CFA_GNU_negative_offset_extended 0x2F

/* Run one instruction stream up to `target`, updating `st`. `loc` is the
 * address the stream has advanced to so far. Returns 0 if something in
 * it cannot be honoured, which is refused rather than approximated --
 * an unwinder that guesses at a rule it does not understand jumps to an
 * address it invented. */
static int run_cfi(const unsigned char *p, const unsigned char *end,
                   const struct cie *cie, uw_word *loc, uw_word target,
                   struct cfi_state *st, const struct cfi_state *initial)
{
    struct cfi_state stack[8];
    int depth = 0;

    while (p < end && *loc <= target) {
        unsigned char op = *p++;
        unsigned char hi = op & 0xC0, lo = op & 0x3F;

        if (hi == DW_CFA_advance_loc) {
            *loc += (uw_word)lo * cie->code_align;
            continue;
        }
        if (hi == DW_CFA_offset) {
            uw_word o = rd_uleb(&p);
            if (lo < UW_NREG) {
                st->reg[lo].kind = RULE_OFFSET;
                st->reg[lo].off = (uw_sword)o * cie->data_align;
            }
            continue;
        }
        if (hi == DW_CFA_restore) {
            if (lo < UW_NREG) st->reg[lo] = initial->reg[lo];
            continue;
        }
        switch (op) {
        case DW_CFA_nop:
            break;
        case DW_CFA_set_loc:
            *loc = rd_encoded(&p, cie->fde_enc);
            break;
        case DW_CFA_advance_loc1:
            *loc += rd_u8(&p) * cie->code_align;
            break;
        case DW_CFA_advance_loc2:
            *loc += rd_u16(&p) * cie->code_align;
            break;
        case DW_CFA_advance_loc4:
            *loc += rd_u32(&p) * cie->code_align;
            break;
        case DW_CFA_offset_extended: {
            uw_word r = rd_uleb(&p), o = rd_uleb(&p);
            if (r < UW_NREG) { st->reg[r].kind = RULE_OFFSET;
                               st->reg[r].off = (uw_sword)o * cie->data_align; }
            break; }
        case DW_CFA_GNU_negative_offset_extended: {
            uw_word r = rd_uleb(&p), o = rd_uleb(&p);
            if (r < UW_NREG) { st->reg[r].kind = RULE_OFFSET;
                               st->reg[r].off = -(uw_sword)o * cie->data_align; }
            break; }
        case DW_CFA_offset_extended_sf: {
            uw_word r = rd_uleb(&p); uw_sword o = rd_sleb(&p);
            if (r < UW_NREG) { st->reg[r].kind = RULE_OFFSET;
                               st->reg[r].off = o * cie->data_align; }
            break; }
        case DW_CFA_val_offset: {
            uw_word r = rd_uleb(&p), o = rd_uleb(&p);
            if (r < UW_NREG) { st->reg[r].kind = RULE_VAL_OFFSET;
                               st->reg[r].off = (uw_sword)o * cie->data_align; }
            break; }
        case DW_CFA_val_offset_sf: {
            uw_word r = rd_uleb(&p); uw_sword o = rd_sleb(&p);
            if (r < UW_NREG) { st->reg[r].kind = RULE_VAL_OFFSET;
                               st->reg[r].off = o * cie->data_align; }
            break; }
        case DW_CFA_restore_extended: {
            uw_word r = rd_uleb(&p);
            if (r < UW_NREG) st->reg[r] = initial->reg[r];
            break; }
        case DW_CFA_undefined: {
            uw_word r = rd_uleb(&p);
            if (r < UW_NREG) st->reg[r].kind = RULE_UNDEFINED;
            break; }
        case DW_CFA_same_value: {
            uw_word r = rd_uleb(&p);
            if (r < UW_NREG) st->reg[r].kind = RULE_SAME;
            break; }
        case DW_CFA_register: {
            uw_word r = rd_uleb(&p), r2 = rd_uleb(&p);
            if (r < UW_NREG) { st->reg[r].kind = RULE_REGISTER;
                               st->reg[r].off = (uw_sword)r2; }
            break; }
        case DW_CFA_remember_state:
            if (depth < (int)(sizeof stack / sizeof stack[0]))
                stack[depth++] = *st;
            else
                return 0;              /* deeper than any real function */
            break;
        case DW_CFA_restore_state:
            if (depth > 0) {
                struct cfi_state saved = stack[--depth];
                /* The location counter is NOT part of the state: only
                 * the rules are. Restoring it would rewind the walk. */
                uw_word keep_loc = *loc;
                *st = saved;
                *loc = keep_loc;
            } else {
                return 0;
            }
            break;
        case DW_CFA_def_cfa:
            st->cfa_reg = (int)rd_uleb(&p);
            st->cfa_off = (uw_sword)rd_uleb(&p);
            st->cfa_is_expr = 0;
            break;
        case DW_CFA_def_cfa_sf:
            st->cfa_reg = (int)rd_uleb(&p);
            st->cfa_off = rd_sleb(&p) * cie->data_align;
            st->cfa_is_expr = 0;
            break;
        case DW_CFA_def_cfa_register:
            st->cfa_reg = (int)rd_uleb(&p);
            break;
        case DW_CFA_def_cfa_offset:
            st->cfa_off = (uw_sword)rd_uleb(&p);
            break;
        case DW_CFA_def_cfa_offset_sf:
            st->cfa_off = rd_sleb(&p) * cie->data_align;
            break;
        case DW_CFA_GNU_args_size:
            rd_uleb(&p);               /* stack space for outgoing args;
                                        * only a forced unwind needs it */
            break;
        case DW_CFA_def_cfa_expression:
        case DW_CFA_expression:
        case DW_CFA_val_expression:
            /* A DWARF expression, which this does not evaluate. gcc
             * emits them for frames whose CFA is not a register plus a
             * constant -- a function using alloca, or a signal
             * trampoline. Refusing is the honest answer: the
             * alternative is to invent a CFA and jump through it. */
            return 0;
        default:
            return 0;                  /* an opcode from a later DWARF */
        }
    }
    return 1;
}

/* Build the rule table for `pc` and apply it: `ctx` goes from
 * describing a frame to describing its CALLER. Returns 0 at the end of
 * the stack or on anything it cannot honour. */
static int step(struct _Unwind_Context *ctx, const struct fde *f)
{
    struct cfi_state st, initial;
    uw_word loc = f->start, cfa;
    uw_word newreg[UW_NREG];
    int i;

    for (i = 0; i < UW_NREG; i++) {
        st.reg[i].kind = RULE_SAME;
        st.reg[i].off = 0;
    }
    st.cfa_reg = -1;
    st.cfa_off = 0;
    st.cfa_is_expr = 0;

    if (!run_cfi(f->cie.insns, f->cie.insns_end, &f->cie, &loc,
                 (uw_word)-1, &st, &st))
        return 0;
    initial = st;
    loc = f->start;
    if (!run_cfi(f->insns, f->insns_end, &f->cie, &loc, ctx->pc, &st,
                 &initial))
        return 0;
    if (st.cfa_reg < 0 || st.cfa_reg >= UW_NREG || st.cfa_is_expr)
        return 0;

    cfa = ctx->reg[st.cfa_reg] + (uw_word)st.cfa_off;

    for (i = 0; i < UW_NREG; i++) {
        switch (st.reg[i].kind) {
        case RULE_OFFSET:
            newreg[i] = *(const uw_word *)(cfa + (uw_word)st.reg[i].off);
            break;
        case RULE_VAL_OFFSET:
            newreg[i] = cfa + (uw_word)st.reg[i].off;
            break;
        case RULE_REGISTER:
            newreg[i] = st.reg[i].off >= 0 && st.reg[i].off < UW_NREG
                      ? ctx->reg[st.reg[i].off] : 0;
            break;
        case RULE_UNDEFINED:
            newreg[i] = 0;
            break;
        default:
            newreg[i] = ctx->reg[i];
            break;
        }
    }

    /* The return address column names where the caller's PC went. A rule
     * of UNDEFINED there is how the outermost frame says "stop" -- it is
     * what crt1 marks its entry point with, and following it would walk
     * off the bottom of the stack into whatever the kernel left. */
    {
        unsigned ra = f->cie.ra_reg < UW_NREG ? f->cie.ra_reg : UW_RA;
        uw_word ret = newreg[ra];
        if (st.reg[ra].kind == RULE_UNDEFINED || ret == 0)
            return 0;
        for (i = 0; i < UW_NREG; i++)
            ctx->reg[i] = newreg[i];
        ctx->reg[UW_SP] = cfa;         /* the caller's stack pointer IS
                                        * the CFA, by definition */
        ctx->cfa = cfa;
        ctx->pc = ret;
    }
    return 1;
}

/* ---- the accessors a personality routine uses --------------------------- */

unsigned long _Unwind_GetGR(struct _Unwind_Context *c, int i)
{
    return i >= 0 && i < UW_NREG ? c->reg[i] : 0;
}

void _Unwind_SetGR(struct _Unwind_Context *c, int i, unsigned long v)
{
    if (i == UW_LP0) c->lp0 = v;
    else if (i == UW_LP1) c->lp1 = v;
    else if (i >= 0 && i < UW_NREG) c->reg[i] = v;
}

unsigned long _Unwind_GetIP(struct _Unwind_Context *c) { return c->pc; }

unsigned long _Unwind_GetIPInfo(struct _Unwind_Context *c, int *before)
{
    /* "Before" means the PC is the address of the faulting instruction
     * rather than the one after a call. Only a signal frame is, and the
     * difference decides whether the personality subtracts one before
     * looking the PC up in the call-site table. */
    *before = c->signal_frame;
    return c->pc;
}

void _Unwind_SetIP(struct _Unwind_Context *c, unsigned long v)
{
    c->landing_pad = v;
    c->have_landing_pad = 1;
}

unsigned long _Unwind_GetCFA(struct _Unwind_Context *c) { return c->cfa; }

unsigned long _Unwind_GetRegionStart(struct _Unwind_Context *c)
{
    return c->func_start;
}

void *_Unwind_GetLanguageSpecificData(struct _Unwind_Context *c)
{
    return (void *)c->lsda;
}

/* Both are zero in a static image: there is one object, loaded where it
 * was linked, so nothing is relative to anything. */
unsigned long _Unwind_GetTextRelBase(struct _Unwind_Context *c)
{ (void)c; return 0; }
unsigned long _Unwind_GetDataRelBase(struct _Unwind_Context *c)
{ (void)c; return 0; }

/* ---- the walk ------------------------------------------------------------ */

/* One frame of the walk: fill in what the personality needs, and say
 * whether there is a personality at all. */
static int frame_at(struct _Unwind_Context *ctx, struct fde *f,
                    personality_fn *pers)
{
    /* The PC is a RETURN address, so it is one past the call -- and if
     * the call was the last instruction of a function, one past the
     * function too. Looking up pc - 1 finds the frame that made the
     * call rather than the one after it. A signal frame is the
     * exception: there the PC is the interrupted instruction itself. */
    uw_word lookup = ctx->signal_frame ? ctx->pc : ctx->pc - 1;

    if (!find_fde(lookup, f))
        return 0;
    ctx->func_start = f->start;
    ctx->lsda = f->lsda;
    *pers = f->cie.personality;
    return 1;
}

static _Unwind_Reason_Code unwind_phase1(struct _Unwind_Context *ctx,
                                         struct _Unwind_Exception *exc)
{
    struct _Unwind_Context w = *ctx;
    int depth = 0;

    for (;;) {
        struct fde f;
        personality_fn pers;

        if (!frame_at(&w, &f, &pers))
            return _URC_END_OF_STACK;
        if (pers) {
            _Unwind_Reason_Code r =
                pers(1, _UA_SEARCH_PHASE, exc->exception_class, exc, &w);
            if (r == _URC_HANDLER_FOUND) {
                /* Remember WHICH frame, by its CFA: phase two walks the
                 * same stack again and has to stop at the same place,
                 * and a CFA is the one thing that identifies a frame
                 * uniquely -- two frames of the same function at
                 * different depths have the same PC and different
                 * CFAs. */
                exc->private_2 = w.cfa;
                return _URC_HANDLER_FOUND;
            }
            if (r != _URC_CONTINUE_UNWIND)
                return _URC_FATAL_PHASE1_ERROR;
        }
        if (!step(&w, &f))
            return _URC_END_OF_STACK;
        if (++depth > 10000)
            return _URC_FATAL_PHASE1_ERROR;   /* a cycle, not a stack */
    }
}

static _Unwind_Reason_Code unwind_phase2(struct _Unwind_Context *ctx,
                                         struct _Unwind_Exception *exc,
                                         int force)
{
    struct _Unwind_Context w = *ctx;
    int depth = 0;

    for (;;) {
        struct fde f;
        personality_fn pers;
        int is_handler;

        if (!frame_at(&w, &f, &pers))
            return _URC_FATAL_PHASE2_ERROR;
        is_handler = !force && w.cfa == exc->private_2;
        if (pers) {
            int actions = _UA_CLEANUP_PHASE | (is_handler ? _UA_HANDLER_FRAME : 0)
                        | (force ? _UA_FORCE_UNWIND : 0);
            _Unwind_Reason_Code r;
            w.have_landing_pad = 0;
            w.lp0 = w.lp1 = 0;
            r = pers(1, actions, exc->exception_class, exc, &w);
            if (r == _URC_INSTALL_CONTEXT) {
                /* The personality chose a landing pad. Put the machine
                 * back the way this frame left it, hand over the two
                 * values it expects, and go. */
                struct uw_capture regs;
                int i;
                for (i = 0; i < UW_NCAPT; i++)
                    regs.saved[i] = w.reg[uw_captured[i]];
                regs.sp = w.reg[UW_SP];
                regs.pc = w.landing_pad;
                regs.lp0 = w.lp0;
                regs.lp1 = w.lp1;
                __uw_resume(&regs);
            }
            if (r != _URC_CONTINUE_UNWIND)
                return _URC_FATAL_PHASE2_ERROR;
        }
        if (is_handler)
            return _URC_FATAL_PHASE2_ERROR;   /* phase one lied */
        if (!step(&w, &f))
            return _URC_END_OF_STACK;
        if (++depth > 10000)
            return _URC_FATAL_PHASE2_ERROR;
    }
}

/* Capture the caller's state and take one step, so the walk starts at
 * whoever called _Unwind_RaiseException rather than at this function. */
static int start_context(struct _Unwind_Context *ctx,
                         const struct uw_capture *cap)
{
    int i;

    for (i = 0; i < UW_NREG; i++)
        ctx->reg[i] = 0;
    for (i = 0; i < UW_NCAPT; i++)
        ctx->reg[uw_captured[i]] = cap->saved[i];
    ctx->reg[UW_SP] = cap->sp;
    ctx->pc = cap->pc;
    ctx->cfa = cap->sp;
    ctx->func_start = 0;
    ctx->lsda = 0;
    ctx->lp0 = ctx->lp1 = 0;
    ctx->landing_pad = 0;
    ctx->have_landing_pad = 0;
    ctx->signal_frame = 0;
    return 1;
}

_Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception *exc)
{
    struct uw_capture cap;
    struct _Unwind_Context ctx;
    _Unwind_Reason_Code r;

    __uw_capture(&cap);
    start_context(&ctx, &cap);

    r = unwind_phase1(&ctx, exc);
    if (r != _URC_HANDLER_FOUND)
        return r;

    /* Phase two starts from the same place phase one did. Re-capturing
     * rather than reusing the walked context is deliberate: phase one's
     * context has been stepped all the way up. */
    __uw_capture(&cap);
    start_context(&ctx, &cap);
    return unwind_phase2(&ctx, exc, 0);
}

/* A landing pad that only ran destructors calls this to carry on. It
 * does not return, and it must NOT redo phase one: the handler frame
 * was already chosen and is still recorded in the exception. */
void _Unwind_Resume(struct _Unwind_Exception *exc)
{
    struct uw_capture cap;
    struct _Unwind_Context ctx;

    __uw_capture(&cap);
    start_context(&ctx, &cap);
    unwind_phase2(&ctx, exc, 0);
    /* Phase two only returns when it failed, and there is nowhere to
     * return TO -- the frames below have been unwound. */
    __builtin_trap();
}

/* A rethrow from inside a catch: phase one has to run again, because
 * the handler that will take it may be different. */
_Unwind_Reason_Code _Unwind_Resume_or_Rethrow(struct _Unwind_Exception *exc)
{
    return _Unwind_RaiseException(exc);
}

void _Unwind_DeleteException(struct _Unwind_Exception *exc)
{
    if (exc && exc->exception_cleanup)
        exc->exception_cleanup(_URC_FOREIGN_EXCEPTION_CAUGHT, exc);
}

/* ---- the frame-registration entry points -------------------------------
 *
 * A bare-metal image with no dynamic loader registers its tables by
 * hand, and `tests/harness/crt.c` calls these. Here they are no-ops
 * that succeed: the tables are found through the linker's bracket
 * symbols, which is the same answer for every image, so there is
 * nothing to register. Defining them rather than leaving them undefined
 * keeps such a crt linkable. */
void __register_frame_info(const void *b, void *o) { (void)b; (void)o; }
void *__deregister_frame_info(const void *b) { (void)b; return 0; }
void __register_frame_info_bases(const void *b, void *o, void *t, void *d)
{ (void)b; (void)o; (void)t; (void)d; }

#endif  /* Linux targets */
