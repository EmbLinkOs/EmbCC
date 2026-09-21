/* Mach-O, the object format macOS uses (D-014).
 *
 * Only what a RELOCATABLE object needs — MH_OBJECT — because that is
 * what the compiler writes; the system linker makes the executable, as
 * D-014 decided. Structures are laid out exactly as the file does, in
 * declaration order with no padding on either target, so a struct can
 * be written straight out.
 *
 * ---- how it differs from ELF, where it matters -------------------------
 *
 * Three differences shape the writer, and none of them is cosmetic.
 *
 * SECTIONS LIVE INSIDE SEGMENTS. An object file has exactly one
 * LC_SEGMENT_64 whose name is the empty string, holding every section;
 * the segment each section *claims* to be in (`segname`, __TEXT/__DATA)
 * is a label the linker groups by, not a container. ELF has a flat
 * section list and puts the grouping in the flags.
 *
 * RELOCATIONS CARRY NO ADDEND. ELF's RELA has an explicit one. Mach-O
 * stores it in the instruction or the data word being patched, and
 * aarch64 — where a 21-bit page field has nowhere to put it — instead
 * emits an ARM64_RELOC_ADDEND entry immediately BEFORE the real one.
 * Two relocations for one address, which is the same shape D-011's
 * adrp/add pair already forced on the ELF side for a different reason.
 *
 * SYMBOL NAMES ARE PREFIXED. A C function `main` is the symbol `_main`.
 * The prefix belongs to the platform, not to the compiler, so it is
 * applied here rather than in codegen — everything above this file
 * names symbols the way C does.
 */
#ifndef EMBCC_MACHO_H
#define EMBCC_MACHO_H

#include <stdint.h>

#define MH_MAGIC_64 0xfeedfacfu
#define MH_OBJECT   0x1u
/* The linker may split sections at symbol boundaries and drop what
 * nothing reaches. Every compiler sets it; without it, dead-stripping
 * cannot work at all. */
#define MH_SUBSECTIONS_VIA_SYMBOLS 0x2000u

#define CPU_ARCH_ABI64        0x01000000
#define CPU_TYPE_X86_64       (0x00000007 | CPU_ARCH_ABI64)
#define CPU_TYPE_ARM64        (0x0000000c | CPU_ARCH_ABI64)
#define CPU_SUBTYPE_X86_64_ALL 3
#define CPU_SUBTYPE_ARM64_ALL  0

#define LC_SEGMENT_64    0x19u
#define LC_SYMTAB        0x02u
#define LC_BUILD_VERSION 0x32u

#define PLATFORM_MACOS 1

#define VM_PROT_READ    0x1
#define VM_PROT_WRITE   0x2
#define VM_PROT_EXECUTE 0x4

/* Section type (low 8 bits) and attributes (high 24). */
#define S_REGULAR                0x0u
#define S_ZEROFILL               0x1u
#define S_CSTRING_LITERALS       0x2u
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400u

/* nlist n_type: the type field, plus the two bits above it. */
#define N_STAB 0xe0
#define N_PEXT 0x10
#define N_TYPE 0x0e
#define N_EXT  0x01
#define N_UNDF 0x0
#define N_ABS  0x2
#define N_SECT 0xe

/* aarch64 relocation types. */
#define ARM64_RELOC_UNSIGNED           0
#define ARM64_RELOC_SUBTRACTOR         1
#define ARM64_RELOC_BRANCH26           2
#define ARM64_RELOC_PAGE21             3
#define ARM64_RELOC_PAGEOFF12          4
#define ARM64_RELOC_GOT_LOAD_PAGE21    5
#define ARM64_RELOC_GOT_LOAD_PAGEOFF12 6
#define ARM64_RELOC_POINTER_TO_GOT     7
#define ARM64_RELOC_ADDEND            10

/* x86-64 relocation types. */
#define X86_64_RELOC_UNSIGNED   0
#define X86_64_RELOC_SIGNED     1
#define X86_64_RELOC_BRANCH     2
#define X86_64_RELOC_GOT_LOAD   3
#define X86_64_RELOC_GOT        4
#define X86_64_RELOC_SUBTRACTOR 5

struct mach_header_64 {
    uint32_t magic;
    int32_t  cputype;
    int32_t  cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
};

struct segment_command_64 {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    int32_t  maxprot;
    int32_t  initprot;
    uint32_t nsects;
    uint32_t flags;
};

struct section_64 {
    char     sectname[16];
    char     segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;          /* a power of two, as the EXPONENT */
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
};

struct symtab_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
};

/* Modern ld refuses an object with no platform recorded; this is how a
 * compiler says which one it built for. */
struct build_version_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t platform;
    uint32_t minos;          /* xxxx.yy.zz packed as xxxx<<16 | yy<<8 | zz */
    uint32_t sdk;
    uint32_t ntools;
};

struct nlist_64 {
    uint32_t n_strx;
    uint8_t  n_type;
    uint8_t  n_sect;         /* 1-based across ALL sections, or 0 */
    uint16_t n_desc;
    uint64_t n_value;
};

/* The packed second word is written by hand rather than as a bit-field:
 * bit-field ordering within a word is implementation-defined, and this
 * has to be the same on every host that builds the compiler (R4). */
struct relocation_info {
    int32_t  r_address;
    uint32_t r_packed;
};

/* symbolnum:24, pcrel:1, length:2, extern:1, type:4 — from the low bit
 * up, which is what every Mach-O producer and consumer agrees on for a
 * little-endian file. `length` is the log2 of the field's width. */
static inline uint32_t macho_reloc_pack(uint32_t symbolnum, int pcrel,
                                        int length, int is_extern, int type)
{
    return (symbolnum & 0x00ffffffu) |
           ((uint32_t)(pcrel & 1) << 24) |
           ((uint32_t)(length & 3) << 25) |
           ((uint32_t)(is_extern & 1) << 27) |
           ((uint32_t)(type & 0xf) << 28);
}

#endif
