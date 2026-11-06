/* EmbCC's NASM-syntax assembler: `embas`, and `embcc -c file.asm`.
 *
 * NASM's language -- its preprocessor (%define, %assign, %macro, %rep,
 * %if, %include), its directives (bits, org, section, global, times,
 * equ, struc, align, incbin, db...resb) and Intel-syntax instructions --
 * read into the GNU-syntax text src/as/gas.c assembles, the instructions
 * through src/arch/x86_64/intel.c into the AT&T statements att.c
 * encodes. One x86 encoder, one object writer, one relaxation: what NASM
 * input adds is the reading. -f elf64 writes an object, -f bin a flat
 * image (org, the sections placed, every reference resolved). */
#ifndef EMBCC_AS_NASM_H
#define EMBCC_AS_NASM_H

enum nasm_format { NASM_ELF64, NASM_BIN };

struct nasm_opts {
    enum nasm_format format;
    const char **incdirs;       /* -I, searched by %include and incbin */
    int nincdirs;
    const char **defines;       /* -D NAME[=VALUE] */
    int ndefines;
    const char **undefs;        /* -U NAME */
    int nundefs;
    int pp_only;                /* -E: the preprocessed source to out */
};

int nasm_assemble(const char *in_path, const char *out_path,
                  const struct nasm_opts *o);

/* embas's command line (main() runs it when called as embas): a .asm
 * (or -f) is NASM's, anything else GNU syntax for the target --target
 * names, which `driver` (the compiler's own command line) assembles */
int embas_main(int argc, char **argv, int (*driver)(int, char **));

#endif
