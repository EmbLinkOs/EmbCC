#include "attrs.h"

#include "../target.h"
#include "../../driver/util.h"

#include <stdlib.h>
#include <string.h>

/* The tag numbers used below, from the ABI's table. Named rather than
 * spelled as numbers at the call sites, because a wrong number here is
 * not a compile error and would be read as a different property. */
enum {
    Tag_CPU_name             = 5,
    Tag_CPU_arch             = 6,
    Tag_CPU_arch_profile     = 7,
    Tag_ARM_ISA_use          = 8,
    Tag_THUMB_ISA_use        = 9,
    Tag_ABI_PCS_R9_use       = 14,
    Tag_ABI_PCS_GOT_use      = 17,
    Tag_FP_arch              = 10,
    Tag_ABI_PCS_wchar_t      = 18,
    Tag_ABI_FP_denormal      = 20,
    Tag_ABI_FP_exceptions    = 21,
    Tag_ABI_FP_number_model  = 23,
    Tag_ABI_align_needed     = 24,
    Tag_ABI_align_preserved  = 25,
    Tag_ABI_enum_size        = 26,
    Tag_ABI_HardFP_use       = 27,
    Tag_ABI_VFP_args         = 28,
    Tag_CPU_unaligned_access = 34,
    Tag_conformance          = 67
};

/* Tag_CPU_arch's values, again from the ABI's table. */
/* Read off clang's own output (llvm-readobj -A on an object built for each
 * triple), not from recollection: v8-M Mainline is 17 and its THUMB_ISA_use
 * is 3 where v7-M's is 2. */
/* ARMv6-M is written as v6S-M, 12, as clang writes it for thumbv6m: every
 * Cortex-M0/M0+/M1 has the OS extension (SVC and the process stack). */
enum { CPU_ARCH_V6S_M = 12, CPU_ARCH_V7 = 10, CPU_ARCH_V7E_M = 13,
       CPU_ARCH_V8M_MAIN = 17 };

struct buf {
    unsigned char *p;
    size_t n, cap;
};

static void bput(struct buf *b, unsigned v)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->p = xrealloc(b->p, b->cap);
    }
    b->p[b->n++] = (unsigned char)v;
}

static void bstr(struct buf *b, const char *s)
{
    while (*s)
        bput(b, (unsigned char)*s++);
    bput(b, 0);
}

/* Every tag and every value is ULEB128. All of the ones here fit in a
 * single byte, but writing them through the real encoding means a value
 * that ever exceeds 127 does not silently truncate. */
static void buleb(struct buf *b, unsigned long v)
{
    do {
        unsigned byte = v & 0x7f;
        v >>= 7;
        bput(b, v ? (byte | 0x80) : byte);
    } while (v);
}

static void btag(struct buf *b, unsigned tag, unsigned long val)
{
    buleb(b, tag);
    buleb(b, val);
}

static void bu32le(struct buf *b, unsigned long v)
{
    bput(b, v & 0xff);
    bput(b, (v >> 8) & 0xff);
    bput(b, (v >> 16) & 0xff);
    bput(b, (v >> 24) & 0xff);
}

unsigned char *arm_build_attributes(size_t *len)
{
    struct buf attrs = { NULL, 0, 0 };
    int em = target_thumb_em();
    int v8 = target_thumb_arch() >= 8;
    int v6 = target_thumb_arch() == 6;

    /* ---- the File sub-subsection's tag/value pairs ---- */
    buleb(&attrs, Tag_conformance);
    bstr(&attrs, "2.09");
    buleb(&attrs, Tag_CPU_name);
    /* The ARCHITECTURE's name, not a part number: EmbCC emits the same
     * code for every part of a profile, and naming one it was not told
     * about would be a claim it cannot support. */
    bstr(&attrs, v8 ? "8-M.MAIN" : v6 ? "6S-M" : em ? "7E-M" : "7-M");
    btag(&attrs, Tag_CPU_arch, v8 ? CPU_ARCH_V8M_MAIN
                             : v6 ? CPU_ARCH_V6S_M
                             : em ? CPU_ARCH_V7E_M : CPU_ARCH_V7);
    btag(&attrs, Tag_CPU_arch_profile, 'M');
    /* No ARM instruction set at all -- a Cortex-M is Thumb from end to
     * end, which is also why the object carries a `$t` mapping symbol
     * and no `$a`. */
    btag(&attrs, Tag_ARM_ISA_use, 0);
    /* 2 is Thumb-2; 3 is "the v8-M Mainline Thumb set", which is a superset.
     * A linker uses this to refuse an object built for a wider set than the
     * image's other objects, so claiming 3 on a v7-M build would let an
     * object into an image whose parts cannot all run there. */
    btag(&attrs, Tag_THUMB_ISA_use, v8 ? 3 : v6 ? 1 : 2);   /* 1: Thumb-1 */
    /* The FPU, when this object's code uses one: -mfpu= with softfp or
     * hard. FPv4-SP-D16 on v7E-M and FPv5-SP-D16 (FP-ARMv8, D16) on v8-M,
     * clang's values for those units. It used to be absent even when the
     * EMBCC_T_FPU hook had emitted VFP instructions -- an object claiming no
     * FPU while containing FPU code. The Cortex-M7's FPv5-D16 is the same
     * architecture as the M33's unit, 8, with double precision too -- which
     * is Tag_ABI_HardFP_use's to say, below. */
    if (target_thumb_fpu())
        btag(&attrs, Tag_FP_arch, v8 || target_thumb_fpu_dp() ? 8 : 6);
    btag(&attrs, Tag_ABI_PCS_R9_use, 0);         /* r9 is an ordinary reg */
    btag(&attrs, Tag_ABI_PCS_GOT_use, 1);        /* direct: no GOT, no PIC */
    btag(&attrs, Tag_ABI_PCS_wchar_t, 4);
    btag(&attrs, Tag_ABI_FP_denormal, 1);
    btag(&attrs, Tag_ABI_FP_exceptions, 0);
    btag(&attrs, Tag_ABI_FP_number_model, 3);    /* full IEEE 754 */
    btag(&attrs, Tag_ABI_align_needed, 1);       /* 8-byte */
    btag(&attrs, Tag_ABI_align_preserved, 1);
    /* An enum is `int` here and -fshort-enums is refused, so this says
     * 2. An object built the other way disagrees on every struct that
     * holds an enum, and this tag is what makes the linker say so. */
    btag(&attrs, Tag_ABI_enum_size, 2);
    /* Single precision only on the two -SP- units: a double still goes
     * through __adddf3, and this is what says so. On FPv5-D16 the code uses
     * both precisions, which is the tag's default -- "as Tag_FP_arch
     * says" -- so it is left out, as clang leaves it out for that unit. */
    if (target_thumb_fpu() && !target_thumb_fpu_dp())
        btag(&attrs, Tag_ABI_HardFP_use, 1);
    /* THE one that matters: 0 is the base standard -- floating point
     * travels in the CORE registers. Emitted explicitly rather than left
     * out, so the object states its ABI positively instead of by
     * absence. When hardware floating point lands this becomes 1, and
     * the two will then refuse to link, which is the point. */
    btag(&attrs, Tag_ABI_VFP_args, target_thumb_hard_abi() ? 1 : 0);
    /* ARMv6-M faults on an unaligned access, and its code never makes one;
     * the other levels permit them for LDR, STR and LDRH. */
    btag(&attrs, Tag_CPU_unaligned_access, v6 ? 0 : 1);

    /* ---- wrap it: File sub-subsection, vendor subsection, version ----
     *
     * Both lengths COUNT THEMSELVES, which is the detail worth stating:
     * the File length covers its own tag byte and its four length bytes
     * as well as the pairs, and the vendor length covers its four bytes
     * and the vendor string as well as everything after. Getting either
     * off by four produces a section every reader rejects. */
    struct buf file = { NULL, 0, 0 };
    bput(&file, 1);                              /* Tag_File */
    bu32le(&file, (unsigned long)(attrs.n + 1 + 4));
    for (size_t i = 0; i < attrs.n; i++)
        bput(&file, attrs.p[i]);

    struct buf out = { NULL, 0, 0 };
    bput(&out, 'A');                             /* format version */
    bu32le(&out, (unsigned long)(file.n + 4 + sizeof "aeabi"));
    bstr(&out, "aeabi");
    for (size_t i = 0; i < file.n; i++)
        bput(&out, file.p[i]);

    free(attrs.p);
    free(file.p);
    *len = out.n;
    return out.p;
}
