/* A GNU-syntax file assembler: `.S` and `.s` inputs, for every target
 * whose instructions EmbCC can already encode.
 *
 * WHY THIS EXISTS. Firmware starts in assembly -- a reset vector, the
 * stack set up before C can run, a context switch -- and EmbCC could
 * not compile a single line of it for any embedded target. `embas`
 * assembles NASM syntax for x86-64 only, and the per-target asm.c
 * files assemble one STATEMENT at a time for inline `__asm__`, with
 * the operands already substituted and no idea what a label is.
 *
 * WHAT IS SHARED AND WHAT IS NOT. Everything about a FILE is here and
 * is target-independent: sections, labels, the directives, the symbol
 * table, relocations, and writing the object. Only instruction
 * encoding is delegated, to the same asm.c the compiler's inline
 * assembly already goes through -- so a mnemonic cannot assemble one
 * way in a .S file and another inside __asm__, which is the bug a
 * second assembler per target would eventually have.
 *
 * HOW A LABEL REACHES AN INSTRUCTION. The statement assemblers take a
 * PC-relative displacement, spelled `.+N`, and know nothing else. This
 * driver runs two passes: the first places every label by encoding
 * each instruction with a zero displacement to learn its size, the
 * second substitutes the real displacement and encodes for good. A
 * reference to a symbol this file does not define becomes a
 * relocation instead, and the forms that can carry one are named
 * explicitly rather than guessed at.
 */
#ifndef EMBCC_AS_GAS_H
#define EMBCC_AS_GAS_H

#include "../arch/code.h"

/* What one target contributes: how to encode a statement, and what to
 * call itself in an ELF header. `encode` is the target's existing
 * inline-asm entry point. */
struct gas_target {
    int machine;                    /* ELF e_machine */
    int is32;                       /* ELFCLASS32 */
    int (*encode)(const char *text, struct code *out, char *err, int errlen);
    /* Is this identifier one of the target's register names? Without
     * it the driver cannot tell `a0` from a label, and would try to
     * relocate against a register. The targets already answer this for
     * inline asm's clobber lists. */
    int (*is_reg)(const char *name, int len);
    /* The relocation types this target uses for the two forms that can
     * name an external symbol. 0 means "this target has no such form",
     * and the driver then refuses by name. */
    int r_call;                     /* `call sym`  -- a two-instruction pair */
    int r_pcrel_hi, r_pcrel_lo;     /* `la rd, sym` -- likewise */
    int r_abs32, r_abs64;           /* `.word sym` / `.quad sym` */
    int r_abs16;                    /* AVR: a pointer is TWO bytes, so a
                                     * symbol's address fits in a `.word`
                                     * there and nowhere else. 0 elsewhere. */
    /* How wide `.word` is. GNU as makes it the machine's word: TWO bytes on
     * AVR and four everywhere else here. 0 means four. Getting it wrong
     * silently doubles every table in an AVR source. */
    int word_bytes;
    /* Statements whose operand is a SYMBOL rather than a number, for a
     * target whose forms are not RISC-V's `call`/`la` or ARM's `bl`.
     *
     * AVR needs eight of them where RISC-V needs two, and that is a
     * property of the machine rather than of its assembler: a sixteen-bit
     * address is loaded a BYTE AT A TIME, so `ldi rd, lo8(sym)` and
     * `ldi rd, hi8(sym)` are separate instructions carrying separate
     * relocations, and program space is addressed in WORDS, so a
     * function's address needs the gs() forms whose relocation halves it.
     *
     * The target rewrites the statement into one it can encode with a zero
     * operand and reports where each relocation goes. NULL when the target
     * has only the generic forms above. */
    int (*symform)(const char *stmt, struct asm_symform *f);
    /* An extra line-comment character, beyond `#` and `//`. GNU as sets
     * this per port and AVR's is `;` -- so an AVR source's comments reach
     * the parser as statements without it, and the first apostrophe in one
     * ("the harness's startup") opens a quote that swallows the rest of the
     * line. 0 when the target has none. */
    char comment_char;
    /* `#` is an immediate's prefix (`mov r0, #1`) on ARM and aarch64, and
     * starts a comment there only at the beginning of a line -- where the
     * preprocessor's own `# 12 "file"` markers sit. Everywhere else `#` is
     * a comment, as GNU as has it for RISC-V and AVR. Treating it as one
     * on ARM cut `tst lr, #0x10` down to `tst lr,`. */
    int hash_is_imm;
    /* Is `w`, in this statement, an operand word -- a special register, a
     * condition, an interrupt mask -- rather than a symbol? NULL when the
     * registers (is_reg) are the only such words. */
    int (*is_word)(const char *stmt, const char *w, int len);
    /* State a statement leaves for the next (ARM's IT block): forgotten at
     * the start of each pass, and refused if still owed at its end. */
    void (*reset)(void);
    int (*open)(void);
    /* Where the statement about to be encoded starts in its section, for
     * a target whose PC-relative forms depend on the instruction's own
     * address and not only on the distance (Xtensa's call and l32r round
     * it to a word). NULL elsewhere. */
    void (*at)(long pc);
    /* A statement separator beyond the newline (RX's `!`), 0 for none. */
    char line_sep;
    /* Relaxation by levels, for a target whose branch has more lengths
     * than two (RX: 1, 2, 3 or 4 bytes, and a pair beyond): the least form
     * index the statement may take -- -1 in the first pass, the optimistic
     * guess -- and, after it, the index it took. A statement's level only
     * rises, so the passes settle. NULL elsewhere. */
    void (*set_level)(int level);
    int (*took_level)(void);
    /* Code alignment's padding, GNU as's for this target; NULL for the
     * single nops do_align writes. */
    void (*fill)(struct code *c, long gap);
};

/* Assembles `in_path` into an ET_REL object at `out_path`, for the
 * currently selected target. Returns 0, or 1 with diagnostics on
 * stderr. `preprocess` runs the C preprocessor first, which is what
 * distinguishes `.S` from `.s`. */
/* `incdirs` are the -I directories (and EmbCC's own), searched by a
 * `.S` file's #include as by C's; NULL with 0 for none. */
int gas_assemble(const char *in_path, const char *out_path, int preprocess,
                 const char **incdirs, int nincdirs);

/* Assembles one block of GNU-syntax text -- a file-scope __asm__, a naked
 * function's body -- from ta->tmpl (diagnostics at ta->file, ta->line
 * onwards) into ta->code, syms, rels and drange, for the driver to place
 * in the unit's .text. Returns 0, or 1 with diagnostics. A block that
 * switches sections is refused. */
struct topasm;
int gas_assemble_block(struct topasm *ta);

#endif
