/* Intel syntax -- NASM's, and GNU as's .intel_syntax -- read into the AT&T
 * statement att.c encodes: one x86 encoder, two spellings of its input.
 *
 * The operands are reversed (but enter's), a size keyword becomes the
 * mnemonic's suffix where no register gives the size, [base + index*scale
 * + disp] becomes disp(base,index,scale), an immediate gets its `$`, a
 * register its `%`, a far pointer sel:off becomes ljmp/lcall's two
 * immediates, and the mnemonics the two syntaxes spell apart are renamed
 * (cdqe/cltq, movzx/movzbl, retf/lret, fld dword/flds ...). Symbols and
 * expressions are left as they are written: the file assembler (gas.c)
 * places them. */
#ifndef EMBCC_X86_INTEL_H
#define EMBCC_X86_INTEL_H

enum x86intel_flavor {
    X86I_NASM,      /* a bare symbol is its address; [rel x], default rel */
    X86I_GNU,       /* a bare symbol is memory; OFFSET x, DWORD PTR */
};

struct x86intel_opts {
    enum x86intel_flavor flavor;
    int mode;               /* 16, 32 or 64: NASM's bits */
    int default_rel;        /* NASM's `default rel` (64-bit code) */
    int prefix_regs;        /* GNU's .intel_syntax prefix: %eax */
};

/* `stmt` (prefixes, the mnemonic, the operands; no label, no comment) as
 * an AT&T statement in out[cap]: 0, or -1 with the reason in err. */
int x86intel_to_att(const char *stmt, const struct x86intel_opts *o,
                    char *out, int cap, char *err, int errlen);

#endif
