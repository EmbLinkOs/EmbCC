/* COFF object structures and constants, for the Windows targets (D-014).
 *
 * Written down here rather than pulled from a Windows SDK, for the same
 * reason src/elf/elf.h is: EmbCC must eventually compile itself, so its
 * source cannot lean on headers the target does not ship -- and on the
 * machines this is developed on there is no SDK to lean on anyway. Only
 * what the writer actually emits is defined; this file grows with it.
 *
 * ---- the one thing that bites ------------------------------------------
 *
 * COFF's structures are PACKED. A symbol table entry is eighteen bytes,
 * not the twenty a C compiler would lay out for the same fields, and a
 * section header's name is eight bytes with no terminator when it fits.
 * So nothing here is written by copying a struct: src/coff/write.c puts
 * every field with an explicit little-endian store, and the sizes below
 * are what the format says rather than what sizeof would say.
 *
 * ---- and the one that differs from ELF ---------------------------------
 *
 * A COFF relocation has NO ADDEND FIELD. Where ELF's RELA carries the
 * addend beside the relocation, COFF leaves it in the bytes being
 * relocated, exactly as Mach-O does -- so the writer's callers patch the
 * target field before handing the section over, and a forgotten patch
 * is a silent zero rather than a link error.
 *
 * REL32 is also measured differently and more conveniently: it is
 * relative to the address of the NEXT instruction, which is what an x86
 * rel32 field means anyway. ELF's PC32 is relative to the field itself
 * and needs an addend of -4 to say the same thing; here that bias is
 * simply absent, and passing it on would displace every call by four
 * bytes.
 */
#ifndef EMBCC_COFF_COFF_H
#define EMBCC_COFF_COFF_H

/* Sizes as the format defines them, not as a compiler would pad them. */
#define COFF_FILE_HEADER_SIZE    20
#define COFF_SECTION_HEADER_SIZE 40
#define COFF_RELOC_SIZE          10
#define COFF_SYMBOL_SIZE         18

/* Machine. Only AMD64 is emitted today: D-014 puts MinGW x86-64 first,
 * because it is the target that reuses the Itanium C++ ABI this tree
 * already implements. ARM64 is named so the refusal can say so. */
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_MACHINE_ARM64 0xAA64

/* Section characteristics. The alignment lives in these bits rather than
 * in a field of its own, which is why the writer converts a byte count
 * into one of them. */
#define IMAGE_SCN_CNT_CODE               0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA   0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_LNK_INFO               0x00000200
#define IMAGE_SCN_LNK_REMOVE             0x00000800
#define IMAGE_SCN_ALIGN_1BYTES           0x00100000
#define IMAGE_SCN_ALIGN_2BYTES           0x00200000
#define IMAGE_SCN_ALIGN_4BYTES           0x00300000
#define IMAGE_SCN_ALIGN_8BYTES           0x00400000
#define IMAGE_SCN_ALIGN_16BYTES          0x00500000
#define IMAGE_SCN_ALIGN_32BYTES          0x00600000
#define IMAGE_SCN_ALIGN_64BYTES          0x00700000
#define IMAGE_SCN_ALIGN_128BYTES         0x00800000
#define IMAGE_SCN_ALIGN_256BYTES         0x00900000
#define IMAGE_SCN_ALIGN_512BYTES         0x00A00000
#define IMAGE_SCN_ALIGN_1024BYTES        0x00B00000
#define IMAGE_SCN_ALIGN_2048BYTES        0x00C00000
#define IMAGE_SCN_ALIGN_4096BYTES        0x00D00000
#define IMAGE_SCN_ALIGN_8192BYTES        0x00E00000
#define IMAGE_SCN_ALIGN_MASK             0x00F00000
#define IMAGE_SCN_MEM_DISCARDABLE        0x02000000
#define IMAGE_SCN_MEM_EXECUTE            0x20000000
#define IMAGE_SCN_MEM_READ               0x40000000
#define IMAGE_SCN_MEM_WRITE              0x80000000

/* Storage class: what kind of name this is. EXTERNAL covers both a
 * definition the linker may export and an undefined reference -- the
 * SECTION NUMBER tells those apart, which is the opposite of ELF, where
 * the binding does. */
#define IMAGE_SYM_CLASS_EXTERNAL 2
#define IMAGE_SYM_CLASS_STATIC   3
#define IMAGE_SYM_CLASS_LABEL    6
#define IMAGE_SYM_CLASS_FILE   103
#define IMAGE_SYM_CLASS_SECTION 104

/* Section number. Sections are numbered from ONE, and zero means
 * undefined -- so a symbol whose section is 0 with a nonzero value is a
 * COMMON, and with a zero value is an external reference. */
#define IMAGE_SYM_UNDEFINED   0
#define IMAGE_SYM_ABSOLUTE  (-1)
#define IMAGE_SYM_DEBUG     (-2)

/* Symbol type. The only distinction the format really carries is
 * "function or not", in the high nibble; the low one is a base type
 * nothing reads. */
#define IMAGE_SYM_TYPE_NULL      0x0000
#define IMAGE_SYM_DTYPE_FUNCTION 0x0020

/* Relocations, AMD64. REL32_1..5 exist because an x86 instruction may
 * have immediates after the displacement, and the field is still
 * measured to the end of the instruction -- the suffix is how many
 * bytes follow. EmbCC emits none of them yet. */
#define IMAGE_REL_AMD64_ABSOLUTE 0x0000
#define IMAGE_REL_AMD64_ADDR64   0x0001
#define IMAGE_REL_AMD64_ADDR32   0x0002
#define IMAGE_REL_AMD64_ADDR32NB 0x0003
#define IMAGE_REL_AMD64_REL32    0x0004
#define IMAGE_REL_AMD64_SECREL   0x000B

#endif
