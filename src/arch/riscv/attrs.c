/* RISC-V's .riscv.attributes, out of the backend so that what writes
 * them into an -S file (src/driver/asmout.c) needs no code generator:
 * tools/embls links the one and not the other. */
#include "../backend.h"
#include "../target.h"
#include "../../driver/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- .riscv.attributes ------------------------------------------------
 *
 * The RISC-V psABI's build attributes, in the same container as ARM's:
 * format 'A', a vendor subsection ("riscv") and a File sub-subsection of
 * tag/value pairs, both lengths counting themselves. Read back off
 * clang's object for the same triple rather than transcribed. */
enum { Tag_RISCV_stack_align = 4, Tag_RISCV_arch = 5 };

static void ab_put(unsigned char **p, size_t *n, size_t *cap, unsigned v)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *p = xrealloc(*p, *cap);
    }
    (*p)[(*n)++] = (unsigned char)v;
}

static void ab_u32(unsigned char **p, size_t *n, size_t *cap, unsigned long v)
{
    for (int k = 0; k < 4; k++)
        ab_put(p, n, cap, (unsigned)(v >> (8 * k)) & 0xff);
}

static void ab_str(unsigned char **p, size_t *n, size_t *cap, const char *s)
{
    while (*s)
        ab_put(p, n, cap, (unsigned char)*s++);
    ab_put(p, n, cap, 0);
}

unsigned char *riscv_build_attributes(size_t *len)
{
    /* I, M and A -- mul/div and the lr/sc atomics are emitted -- but A
     * only where -march= has it (rv32imc's objects claimed it, and a
     * linker merging attributes then marked the image as needing it),
     * plus F and D as -march= says (with the Zicsr they imply), C when
     * target_riscv_rvc says so, and Zifencei when -march= named it: in
     * clang's order. A disassembler reads this to know which
     * instructions to decode. */
    char arch[96];
    int flen = target_riscv_flen();
    snprintf(arch, sizeof arch, "rv%di2p1_m2p0%s%s%s%s%s%s",
             target_xlen(), target_riscv_atomics() ? "_a2p1" : "",
             flen ? "_f2p2" : "", flen == 64 ? "_d2p2" : "",
             target_riscv_rvc() ? "_c2p0" : "", flen ? "_zicsr2p0" : "",
             target_riscv_zifencei() ? "_zifencei2p0" : "");
    unsigned char *a = NULL, *o = NULL;
    size_t na = 0, ca = 0, no = 0, co = 0;
    ab_put(&a, &na, &ca, Tag_RISCV_stack_align);
    ab_put(&a, &na, &ca, 16);                   /* uleb128 16 */
    ab_put(&a, &na, &ca, Tag_RISCV_arch);
    ab_str(&a, &na, &ca, arch);

    ab_put(&o, &no, &co, 'A');                  /* format version */
    ab_u32(&o, &no, &co, (unsigned long)(4 + sizeof "riscv" + 1 + 4 + na));
    ab_str(&o, &no, &co, "riscv");
    ab_put(&o, &no, &co, 1);                    /* Tag_File */
    ab_u32(&o, &no, &co, (unsigned long)(1 + 4 + na));
    for (size_t k = 0; k < na; k++)
        ab_put(&o, &no, &co, a[k]);
    free(a);
    *len = no;
    return o;
}
