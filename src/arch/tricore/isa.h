/* The TriCore instruction set as tables (src/arch/tricore/isa.inc, which
 * tools/tricore-isa generates from what GNU binutils for TriCore do), and
 * what reads them: the assembler (asm.c) choosing and packing a form, and
 * EmbLD (src/link/link.c) finding the field a relocation fills in an
 * instruction and packing the value into it -- the same bits, the same
 * code, so the two cannot disagree about where a field lies. */
#ifndef EMBCC_ARCH_TRICORE_ISA_H
#define EMBCC_ARCH_TRICORE_ISA_H

/* One bit of a field: where it sits, and its weight 2^shift (negative for
 * a sign bit). */
struct tc_fbit { unsigned char pos, shift, neg; };
/* kind: D A E (a register), N (a number), L (a PC-relative target, in
 * bytes), S (a core special function register), M (another field's value
 * again: field v0's at bit fb -- updfl's register, twice). rule: 0 any
 * value, 1 even only, 2 only the nok values at tc_oks[ok]. A field holds
 * v0 plus the weights of its set bits. */
struct tc_field {
    char kind;
    unsigned char nb;
    unsigned short fb;
    int v0;
    unsigned char rule;
    unsigned short ok, nok;
};
/* A form: mnemonic, operand shape ("D,[A]N"), length, the word with every
 * field clear, its fields (f..f+nf). Sorted by mnemonic, shape, length,
 * then GNU's preference. */
struct tc_insn {
    const char *mnem, *shape;
    unsigned char len;
    unsigned int base;
    unsigned short f, nf;
};
/* A symbol in field `field` with operator `op` (0 sym, 1 hi:, 2 lo:, 3
 * sm:, 4 up:, 5 a label of another section): the form GNU takes (an
 * index into tc_insns; -1 the long branch; -2 refused), the relocation
 * and its offset; for the long branch `extra` is the sequence's length.
 * `map`: when GNU writes another form (`add d0, sym` as `add d0, d0,
 * sym`), the statement's operand each of that form's fields takes. */
struct tc_symrule {
    const char *mnem, *shape;
    int field, op, entry, rel, reloff, extra;
    const char *map;
};
/* 3-operand forms GNU writes as 2 when the destination is a source: a
 * the first, b the second (where the operation commutes). */
struct tc_narrow { const char *mnem, *shape; int a, b; };

extern const struct tc_fbit tc_fbits[];
extern const int tc_oks[];
extern const struct tc_field tc_fields[];
extern const struct tc_insn tc_insns[];
extern const struct tc_symrule tc_symrules[];
extern const struct tc_narrow tc_narrows[];
extern const int tc_ninsn, tc_nsymrules, tc_nnarrows;

/* Field f holding v: its bits ORed into *w, 1; or 0 when v does not fit
 * or is not one GNU takes there. */
int tc_fit_field(const struct tc_field *f, long long v, unsigned *w);
/* The values field f can hold: [*lo, *hi] in steps of *step. */
void tc_field_range(const struct tc_field *f, long long *lo, long long *hi,
                    long long *step);
/* The bits field f occupies. */
unsigned tc_field_mask(const struct tc_field *f);
/* The first form of mnemonic mn, or -1. */
int tc_first_of(const char *mn);

/* EmbLD: the field a relocation of `type` fills in the len-byte
 * instruction w -- the form whose opcode bits w has, and whose field GNU
 * as writes that relocation for. The form's index in *entry and the
 * field's in *field (0..nf-1); 0, or -1 when no form has one. */
int tc_reloc_site(unsigned long w, int len, int type, int *entry, int *field);

#endif
