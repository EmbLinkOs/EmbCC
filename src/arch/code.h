/* A growable buffer of machine code — what every target's encoder
 * (x86_64/emit.c, aarch64/emit.c) and the assemblers write into. Target
 * neutral: bytes, little-endian 32-bit words, patching, alignment. */
#ifndef EMBCC_ARCH_CODE_H
#define EMBCC_ARCH_CODE_H

struct code {
    unsigned char *p;
    int len, cap;
    /* [start, end) ranges of DATA inside the code: a jump table a backend
     * placed in .text. The ELF writer puts ARM's mapping symbols around
     * them ($d at the start, $t or $x after), so a disassembler shows
     * them as words rather than as instructions that happen to decode
     * or not, and a linker never plants a veneer inside one. Pairs. */
    int *drange;
    int ndrange, capdrange;
    /* An inline asm template's ALIGNMENTS (`.p2align 2`): what padding
     * they need depends on where the template lands in its section, which
     * the template's assembler does not know -- so it records them here
     * and code_put_asm pads where the backend places the bytes. Four ints
     * each, in offset order: the offset, the boundary, the fill byte (-1:
     * the target's nops) and the most padding allowed (0: any). */
    int *arange;
    int narange, caparange;
};
void code_mark_data(struct code *c, int start, int end);

/* ---- a statement that names a symbol ---------------------------------
 *
 * The contract between an assembler front end (src/as/gas.c) and a
 * target's own assembler: the target recognises a statement whose operand
 * is a SYMBOL, rewrites it into one it can encode with a zero in that
 * place, and says where each relocation goes.
 *
 * It lives here, in the header both sides already include, because
 * defining it in each gave two structurally identical types and one
 * incompatible function pointer. A target with no such forms leaves the
 * hook NULL.
 */
struct asm_symsite {
    int off;                     /* bytes from the start of this statement */
    int reloc;                   /* the target's relocation type */
};

struct asm_symform {
    int sym_at, sym_len;         /* the symbol's extent within the statement */
    /* A constant offset written after the symbol: `lds r24, buf+5`. It goes
     * into the relocation's addend, never into the instruction. This field
     * did not exist, and the AVR assembler read `buf+5` as `buf` -- the
     * identifier stopped at the `+` and nothing looked at what followed --
     * so the load went to buf[0] and the object assembled and linked
     * cleanly. */
    long addend;
    char encode[128];            /* the statement with a zero operand */
    struct asm_symsite site[2];
    int nsites;
};

void code_byte(struct code *c, int b);
void code_u16(struct code *c, unsigned v);               /* little-endian */
void code_u32(struct code *c, unsigned long v);          /* little-endian */
void code_patch32(struct code *c, int off, unsigned long v);
void code_align(struct code *c, int align, int fill);

/* ---- an inline asm template's directives ------------------------------
 *
 * The directives a template may hold beside its instructions, shared by
 * the targets' inline assemblers (a .s file's are src/as/gas.c's):
 *   - strings: `.ascii "..."`, and `.asciz`/`.string`, which end each
 *     string with a NUL -- llvm-mc's escapes (\b \f \n \r \t \" \\, octal
 *     \NNN, \xHH);
 *   - alignment: `.p2align N`, `.balign N` and `.align N` -- N a power of
 *     two on x86 ELF and COFF, the exponent everywhere else, as GNU as
 *     reads it per target -- with an optional fill byte and most padding.
 *     The padding is not decided here (see struct code's arange);
 *   - data, for a target that gives a table: each comma-separated constant
 *     expression at its directive's size, little-endian unless said.
 * Everything emitted is DATA (code_mark_data): an ARM disassembler sees it
 * between mapping symbols, and the ARMv6-M check for Thumb-2 instructions
 * passes over it. Linux's asm-offsets and EmbLinkRTOS's layout probes are
 * the reason: `.ascii "->SYM %c0"` and `.p2align 2` in a function. */
struct asm_datadir { const char *name; int size; };
extern const struct asm_datadir asm_data_x86[], asm_data_a64[],
                                asm_data_avr[];
struct asm_dirs {
    const struct asm_datadir *data;  /* NULL: the target handles its own */
    int align_bytes;                 /* `.align N` is N bytes, not 2^N */
    int big_endian;
};
/* 0 when stmt[0..len) is none of these, 1 when done, -1 with err set. */
int code_asm_directive(const char *stmt, int len, struct code *out,
                       const struct asm_dirs *d, char *err, int errlen);

/* Where a template's statement ends: at a newline or any of `seps`, but
 * not inside a "..." string, so `.ascii "a;b"` is one statement. */
int asm_stmt_len(const char *p, const char *seps);
/* Its length without a comment -- from any of `marks`, or from `//` when
 * `slashes` -- found outside a string. */
int asm_cut_comment(const char *s, int len, const char *marks, int slashes);

/* What a target pads code with, as llvm-mc pads it (writeNopData). */
enum code_fill {
    CODE_FILL_ZERO,     /* AVR, whose nop is 0x0000 */
    CODE_FILL_X86,      /* the long nops, at most fifteen bytes each */
    CODE_FILL_A64,      /* zeros to a word, then nops */
    CODE_FILL_THUMB2,   /* bf00 nops, then a zero byte when the gap is odd */
    CODE_FILL_THUMB1,   /* the same with 46c0 (mov r8, r8): ARMv6-M */
    CODE_FILL_A32,      /* e320f000 nops, then 00 / 00 00 / 00 00 a0 */
    CODE_FILL_RISCV     /* a zero to even, a c.nop to four, then nops */
};
void code_fill(struct code *c, int kind, long gap);

/* A template's bytes into the function at c->len, padded at each of its
 * alignments for where they land now -- relative to the SECTION, which
 * is the buffer: .text is 16-aligned and a function in a section of its
 * own starts on its code_align, which the backend raised to
 * code_asm_align_max. Its data ranges come along, and padding that
 * follows data is data too, as llvm-mc maps it. */
void code_put_asm(struct code *c, const unsigned char *b, int len,
                  const int *dr, int ndr, const int *ar, int nar, int fill);
/* The largest boundary among a template's alignments, 0 for none; and
 * the most the template can grow by when the code before it moves -- a
 * branch measured across it on one pass must reach on the next. */
int code_asm_align_max(const int *ar, int nar);
int code_asm_align_slack(const int *ar, int nar);
/* At irgen, where the template does not know its place: an alignment no
 * larger than `insn`, the alignment every instruction (so the template's
 * start) has, pads the same wherever it lands, and is settled now. Data
 * that would leave the code after the template off an instruction
 * boundary -- `.byte 1` alone on Thumb -- is followed by zeros up to one,
 * as GNU as does for AArch64. Returns 0 when nothing is left open, or the
 * largest boundary still open; -1 with err set is reserved for a refusal. */
int code_asm_settle(struct code *c, int insn, int fill, char *err, int errlen);

#endif
