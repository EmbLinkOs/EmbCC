/* The ARM build attributes an ELF object must carry (.ARM.attributes).
 *
 * This section is how a consumer learns what the object was built FOR,
 * and the linker reads it to refuse combinations that cannot work. The
 * one that matters most is Tag_ABI_VFP_args: 0 says floating-point
 * arguments travel in the core registers (the base standard, what
 * -mfloat-abi=soft means) and 1 says they travel in s0-s15. GNU ld
 * compares that tag across inputs and refuses to mix them.
 *
 * EmbCC emitted no attributes at all, which sounds harmless and is the
 * opposite: with nothing to compare, a soft-float EmbCC object links
 * against a hard-float GCC one without complaint and the callee reads
 * its arguments from registers the caller never wrote. That is a silent
 * miscompilation produced at LINK time, past every check the compiler
 * makes -- so the fix is not a nicety, it is restoring the only place
 * downstream that could have caught it.
 *
 * Tag_ABI_enum_size is the second one worth knowing about. EmbCC's
 * enums are `int` and it refuses -fshort-enums; the tag says so, which
 * is what stops an object built the other way from linking silently.
 *
 * The format is from the ARM ABI's "Addenda to, and Errata in, the ABI
 * for the ARM Architecture", section 2.3. It was read back off a real
 * object rather than transcribed from the document, so the encoding is
 * checked against a toolchain that already gets it right.
 */
#ifndef EMBCC_ARCH_THUMB_ATTRS_H
#define EMBCC_ARCH_THUMB_ATTRS_H

#include <stddef.h>

/* Builds the .ARM.attributes payload for the target as currently
 * configured. Returns a malloc'd buffer the caller frees, with its
 * length in *len. */
unsigned char *arm_build_attributes(size_t *len);

/* An assembly file's `.fpu NAME`: the unit its hand-written instructions
 * are for, which is what its object then claims (Tag_FP_arch, and
 * Tag_Advanced_SIMD_arch for NEON -- the compiler never claims NEON, since
 * it emits none). fp_arch is the tag's value, 0 for none; simd 0 for no
 * NEON; hp 1 for the half-precision extension. arm_attrs_asm_fpu_clear()
 * goes back to what the target says. */
void arm_attrs_asm_fpu(int fp_arch, int simd, int hp);
void arm_attrs_asm_fpu_clear(void);

#endif
