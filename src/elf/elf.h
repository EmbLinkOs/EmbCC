/* ELF64 structures and constants shared by asm and link (ARCHITECTURE.md §2).
 *
 * Defined here rather than pulled from the host's <elf.h>: EmbCC must
 * eventually compile itself (ARCHITECTURE.md §7), so its own source cannot
 * lean on headers the target does not ship. Only what EmbCC actually emits
 * is defined — this file grows with the writer, it is not a mirror of the
 * spec.
 *
 * Layouts follow the System V gABI, ELF64, little-endian (the only target,
 * TARGET_ABI.md).
 */
#ifndef EMBCC_ELF_ELF_H
#define EMBCC_ELF_ELF_H

typedef unsigned char      Elf64_Uchar;
typedef unsigned short     Elf64_Half;
typedef unsigned int       Elf64_Word;
typedef unsigned long long Elf64_Xword;
typedef unsigned long long Elf64_Addr;
typedef unsigned long long Elf64_Off;

#define EI_NIDENT 16

typedef struct {
    Elf64_Uchar e_ident[EI_NIDENT];
    Elf64_Half  e_type;
    Elf64_Half  e_machine;
    Elf64_Word  e_version;
    Elf64_Addr  e_entry;
    Elf64_Off   e_phoff;
    Elf64_Off   e_shoff;
    Elf64_Word  e_flags;
    Elf64_Half  e_ehsize;
    Elf64_Half  e_phentsize;
    Elf64_Half  e_phnum;
    Elf64_Half  e_shentsize;
    Elf64_Half  e_shnum;
    Elf64_Half  e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  sh_name;
    Elf64_Word  sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link;
    Elf64_Word  sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;

typedef struct {
    Elf64_Word  st_name;
    Elf64_Uchar st_info;
    Elf64_Uchar st_other;
    Elf64_Half  st_shndx;
    Elf64_Addr  st_value;
    Elf64_Xword st_size;
} Elf64_Sym;

typedef struct {
    Elf64_Addr  r_offset;
    Elf64_Xword r_info;
    long long   r_addend;
} Elf64_Rela;

/* Program header — the linker (M3) writes these; the compiler's ET_REL
 * output has none. */
typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

/* The 32-bit forms. EmbCC builds every object in the 64-bit structures
 * above and converts at the one place that serialises them
 * (elf/write.c), because a second set of structures threaded through the
 * writer would be a second set of places to get a field order wrong. A
 * 32-bit target is ARMv7-M today (D-015); nothing else here is ILP32.
 *
 * The layouts are NOT the 64-bit ones with narrower fields: Elf32_Sym
 * puts st_value and st_size BEFORE st_info, where Elf64_Sym puts them
 * after. Copying field by field is the only safe way across. */
typedef unsigned char  Elf32_Uchar;
typedef unsigned short Elf32_Half;
typedef unsigned int   Elf32_Word;
typedef unsigned int   Elf32_Addr;
typedef unsigned int   Elf32_Off;

typedef struct {
    Elf32_Uchar e_ident[EI_NIDENT];
    Elf32_Half  e_type;
    Elf32_Half  e_machine;
    Elf32_Word  e_version;
    Elf32_Addr  e_entry;
    Elf32_Off   e_phoff;
    Elf32_Off   e_shoff;
    Elf32_Word  e_flags;
    Elf32_Half  e_ehsize;
    Elf32_Half  e_phentsize;
    Elf32_Half  e_phnum;
    Elf32_Half  e_shentsize;
    Elf32_Half  e_shnum;
    Elf32_Half  e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off  sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
} Elf32_Shdr;

typedef struct {
    Elf32_Word  st_name;
    Elf32_Addr  st_value;
    Elf32_Word  st_size;
    Elf32_Uchar st_info;
    Elf32_Uchar st_other;
    Elf32_Half  st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
    int        r_addend;
} Elf32_Rela;

/* The implicit-addend form, which the ARM EABI specifies and every ARM
 * toolchain therefore emits: the addend lives IN the field being
 * patched, in whatever shape that field has. */
typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} Elf32_Rel;

typedef struct {
    Elf64_Addr  r_offset;
    Elf64_Xword r_info;
} Elf64_Rel;

/* Not Elf64_Phdr with narrower members either: p_flags is the SECOND
 * field there and the LAST one here. */
typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

#define ELF32_R_INFO(sym, type) \
    (((Elf32_Word)(sym) << 8) | ((Elf32_Word)(type) & 0xff))

#define ELFCLASS32 1

/* EF_ARM_EABI_VER5 — the EABI version in e_flags, which every ARM
 * consumer checks and which is 0 on an object that forgot it. Read off
 * llvm-mc's own output for a thumbv7m object rather than remembered. */
#define EF_ARM_EABI_VER5 0x05000000
/* EF_RISCV_RVC: the object holds compressed (C extension) instructions,
 * which a disassembler reads as <unknown> and a core without the
 * extension traps on. Bits 2:1 are the float ABI, whose 0 means SOFT. */
#define EF_RISCV_RVC 0x0001
/* AVR's e_flags carry the architecture in the low seven bits; avr5 is the
 * ATmega328P's (__AVR_ARCH__ 5), and 0 reads as avr0 -- a core without
 * mul, movw or the 16-bit adiw/sbiw. */
#define EF_AVR_ARCH_AVR5 5
#define EF_AVR_ARCH_MASK 0x7f

#define ELF64_R_INFO(sym, type) \
    (((Elf64_Xword)(sym) << 32) | ((Elf64_Xword)(type) & 0xffffffff))
#define ELF64_R_SYM(info)  ((Elf64_Word)((info) >> 32))
#define ELF64_R_TYPE(info) ((Elf64_Word)((info) & 0xffffffff))
#define ELF64_ST_BIND(info) ((info) >> 4)
#define ELF64_ST_TYPE(info) ((info) & 0xf)

/* Relocation types. R_X86_64_PLT32 is what gas/gcc emit for a call to a
 * global; TARGET_ABI §4a records the expensive fact that a static link
 * must treat it as a plain PC32 — the EmbLinkOS linker (TCC patch 0001)
 * and the cross ld both do. GOTPCREL/its relaxable forms reach data
 * through the GOT (newlib's _impure_ptr — TCC patch 0003, the static
 * GOT must be built AND filled). */
#define R_X86_64_64            1
#define R_X86_64_PC32          2
#define R_X86_64_PLT32         4
#define R_X86_64_GOTPCREL      9
#define R_X86_64_32           10
#define R_X86_64_32S          11
#define R_X86_64_PC64         24
#define R_X86_64_GOTPCRELX    41
#define R_X86_64_REX_GOTPCRELX 42
/* Local-exec TLS: the 32-bit field holds the object's offset from the
 * thread pointer, which the LINKER computes once it knows how big the
 * whole thread block is. x86-64 puts the block below the thread
 * pointer, so the value is negative. */
#define R_X86_64_TPOFF32      23

/* aarch64 (ELF for the Arm 64-bit Architecture, §4.6.3). Only the four
 * EmbCC emits are named: a `bl`'s 26-bit branch, the adrp/add pair that
 * materialises a symbol's address, and an absolute 64-bit data slot. */
#define R_AARCH64_ABS64              257
#define R_AARCH64_ABS32              258
#define R_AARCH64_PREL32             261
#define R_AARCH64_ADR_PREL_PG_HI21   275
#define R_AARCH64_ADD_ABS_LO12_NC    277
#define R_AARCH64_JUMP26             282
#define R_AARCH64_CALL26             283
#define R_AARCH64_ADR_GOT_PAGE       311
#define R_AARCH64_LD64_GOT_LO12_NC   312
/* Local-exec TLS, the aarch64 half of the same idea, as an add pair
 * because the offset does not fit one instruction. aarch64 puts the
 * block ABOVE the thread pointer, so the value is positive. */
#define R_AARCH64_TLSLE_ADD_TPREL_HI12    549
/* _NC: no overflow check. 550 is the CHECKING variant of the same
 * field, and it is the wrong one here -- the low add legitimately
 * drops the bits the high add already carried, so a checked form would
 * reject every offset above 4095. The numbers are what
 * aarch64-elf-as emits for :tprel_hi12: and :tprel_lo12_nc:, read off
 * its own output rather than remembered. */
#define R_AARCH64_TLSLE_ADD_TPREL_LO12_NC 551

/* ARM 32-bit (ELF for the Arm Architecture, §4.6.1.2). Only the three
 * a Cortex-M object needs so far.
 *
 * THM_CALL rather than CALL is the one to get right: the Cortex-M
 * executes Thumb and only Thumb, so every call site is a 32-bit Thumb
 * `bl` whose displacement is split across two halfwords with the sign
 * bit reused twice (J1/J2). R_ARM_CALL names the ARM-state instruction
 * at the same address, and a linker handed it would rewrite four bytes
 * that mean something else entirely. */
#define R_ARM_ABS32      2
#define R_ARM_REL32      3
#define R_ARM_THM_CALL  10
/* The movw/movt pair that materialises a symbol's address in Thumb
 * state. _NC on the low half: it drops the bits movt carries, so the
 * checking variant would reject every address above 65535. */
#define R_ARM_THM_MOVW_ABS_NC 47
#define R_ARM_THM_MOVT_ABS    48
/* The same field as THM_CALL in a `b.w` rather than a `bl`; an
 * assembler emits it for a tail branch to another section. */
#define R_ARM_THM_JUMP24      30
/* The exception index table's self-relative pointer: 31 bits of signed
 * offset, the top bit reserved to say what the entry holds. */
#define R_ARM_PREL31          42

/* RISC-V relocations (psABI), the ones an object from this compiler
 * needs.
 *
 * R_RISCV_CALL patches a PAIR -- the `auipc` it sits on and the `jalr`
 * four bytes later -- which is unlike every other relocation here and is
 * the ABI's own shape. _PLT is the same thing through a PLT where one
 * exists; a static image has none, and the two are interchangeable for
 * a linker that resolves the symbol itself.
 *
 * HI20 and LO12 are a PAIR in arithmetic rather than in bits: the low
 * half is SIGN-EXTENDED when the instruction adds it, so the linker must
 * compute the high half as (V + 0x800) >> 12. LO12_I and LO12_S are the
 * same 12 bits in two instruction formats -- S-type splits the field
 * across bits 31:25 and 11:7 so rs1 and rs2 keep their places.
 *
 * The PCREL_ forms are the ones this compiler emits, because `lui`
 * sign-extends bit 31 and the absolute pair therefore cannot name an
 * RV64 address between 0x80000000 and 0xffffffff7fffffff -- which is
 * where a firmware image lives. PCREL_LO12's SYMBOL is the AUIPC, not
 * the target: the linker looks up the high half's relocation at that
 * address and takes the low twelve bits of what IT computed. The
 * absolute HI20/LO12 numbers are here because an object from another
 * toolchain may carry them and the linker reads those too.
 *
 * R_RISCV_RELAX carries no value: it marks a site the linker MAY shorten
 * (a call that turns out to be in range of a single `jal`). Relaxation
 * is optional, so a linker that ignores it is correct -- embld does. */
#define R_RISCV_32        1
#define R_RISCV_64        2
#define R_RISCV_BRANCH   16
#define R_RISCV_JAL      17
#define R_RISCV_CALL     18
#define R_RISCV_CALL_PLT 19
#define R_RISCV_PCREL_HI20   23
#define R_RISCV_PCREL_LO12_I 24
#define R_RISCV_PCREL_LO12_S 25
#define R_RISCV_HI20     26
#define R_RISCV_LO12_I   27
#define R_RISCV_LO12_S   28
#define R_RISCV_RELAX    51
/* Padding a relaxing linker may shrink. Like RELAX it carries no symbol,
 * and like RELAX it is safe to ignore: the assembler already emitted the
 * NOPs that make the alignment hold, and a linker that moves nothing
 * cannot invalidate it. */
#define R_RISCV_ALIGN    43

/* e_ident indices and values */
#define EI_MAG0       0
#define EI_MAG1       1
#define EI_MAG2       2
#define EI_MAG3       3
#define EI_CLASS      4
#define EI_DATA       5
#define EI_VERSION    6
#define ELFMAG0       0x7f
#define ELFMAG1       'E'
#define ELFMAG2       'L'
#define ELFMAG3       'F'
#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1

/* e_type — ET_REL until the integrated linker lands (ROADMAP M3);
 * ET_EXEC is listed because it is the only executable type the kernel
 * loader accepts (TARGET_ABI.md §4b: never ET_DYN/PIE). */
#define ET_REL        1
#define ET_EXEC       2

#define EM_X86_64     62
#define EM_AARCH64   183
#define EM_ARM        40
#define EM_RISCV     243

/* p_type */
#define PT_NULL       0
#define PT_LOAD       1
/* The thread-block template. Not loaded as itself -- its bytes are part
 * of a PT_LOAD -- but DESCRIBED, so a runtime can find them and give
 * each thread a private copy. */
#define PT_TLS        7

/* p_flags */
#define PF_X          0x1
#define PF_W          0x2
#define PF_R          0x4

/* sh_type */
#define SHT_NULL      0
/* AVR, from the ELF machine registry. */
#define EM_AVR 83

/* AVR relocation types. Read off llvm-mc's own output rather than a
 * table: `llvm-readobj -r` on an object assembled from call/ldi/.word
 * names each one, which is the same referee the encoder uses. */
#define R_AVR_NONE          0
#define R_AVR_32            1
#define R_AVR_7_PCREL       2
#define R_AVR_13_PCREL      3
#define R_AVR_16            4
#define R_AVR_16_PM         5
#define R_AVR_LO8_LDI       6
#define R_AVR_HI8_LDI       7
#define R_AVR_CALL          18
#define R_AVR_LO8_LDI_GS    24
#define R_AVR_HI8_LDI_GS    25

#define SHT_PROGBITS  1
/* ARM's build-attributes section. A processor-specific type, so it is
 * SHT_LOPROC+3 rather than a number in the generic range. */
#define SHT_ARM_ATTRIBUTES 0x70000003
#define SHT_RISCV_ATTRIBUTES 0x70000003   /* the same processor-specific number */
#define SHT_SYMTAB    2
#define SHT_STRTAB    3
#define SHT_RELA      4
#define SHT_NOBITS    8
/* The implicit-addend relocation section. Not a legacy form: the ARM
 * EABI specifies it, so every ARM object holds .rel.text rather than
 * .rela.text. */
#define SHT_REL       9
/* Arrays of function pointers the startup code walks before main and at
 * exit. Their TYPE is what says so -- a linker gathers sections by type
 * here, not by name -- so a .init_array emitted as SHT_PROGBITS would
 * be laid out as ordinary data and never run. */
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHT_X86_64_UNWIND 0x70000001   /* x86-64 psABI: .eh_frame's type */

/* sh_flags */
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define SHF_INFO_LINK 0x40
/* Thread-local storage. A section with this flag is not part of the
 * image every thread shares: it is the TEMPLATE from which each
 * thread's own block is made, and the linker gathers such sections
 * into PT_TLS rather than into a PT_LOAD the program addresses
 * directly. */
#define SHF_TLS       0x400

/* special section indices */
#define SHN_UNDEF     0
#define SHN_ABS       0xfff1
#define SHN_COMMON    0xfff2

/* symbol binding/type, packed into st_info */
#define STB_LOCAL     0
#define STB_GLOBAL    1
#define STB_WEAK      2
#define STT_NOTYPE    0
#define STT_OBJECT    1
#define STT_FUNC      2
#define STT_SECTION   3
#define STT_FILE      4
/* A symbol whose st_value is an offset within the thread block, not an
 * address. A TLS object marked STT_OBJECT would be relocated as though
 * it had one fixed location shared by every thread. */
#define STT_TLS       6
/* Symbol visibility, in st_other's low two bits. HIDDEN means the name
 * is not visible to other components once linked -- the linker turns it
 * into a local -- which is how a library keeps an interface private
 * without giving up being able to call it across its own units. */
#define STV_DEFAULT   0
#define STV_INTERNAL  1
#define STV_HIDDEN    2
#define STV_PROTECTED 3

#define ELF64_ST_INFO(bind, type) ((Elf64_Uchar)(((bind) << 4) | ((type) & 0xf)))

#endif
