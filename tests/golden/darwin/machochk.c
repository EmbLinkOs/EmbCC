/* Generated: our Mach-O header against the SDK's own. */
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/reloc.h>
#include <mach-o/arm64/reloc.h>
#include <mach-o/x86_64/reloc.h>
#include <stdio.h>
#include <stddef.h>
static const unsigned long sys_MH_MAGIC_64 = (unsigned long)(MH_MAGIC_64);
static const unsigned long sys_MH_OBJECT = (unsigned long)(MH_OBJECT);
static const unsigned long sys_MH_SUBSECTIONS_VIA_SYMBOLS = (unsigned long)(MH_SUBSECTIONS_VIA_SYMBOLS);
static const unsigned long sys_CPU_TYPE_X86_64 = (unsigned long)(CPU_TYPE_X86_64);
static const unsigned long sys_CPU_TYPE_ARM64 = (unsigned long)(CPU_TYPE_ARM64);
static const unsigned long sys_CPU_SUBTYPE_X86_64_ALL = (unsigned long)(CPU_SUBTYPE_X86_64_ALL);
static const unsigned long sys_CPU_SUBTYPE_ARM64_ALL = (unsigned long)(CPU_SUBTYPE_ARM64_ALL);
static const unsigned long sys_LC_SEGMENT_64 = (unsigned long)(LC_SEGMENT_64);
static const unsigned long sys_LC_SYMTAB = (unsigned long)(LC_SYMTAB);
static const unsigned long sys_LC_BUILD_VERSION = (unsigned long)(LC_BUILD_VERSION);
static const unsigned long sys_PLATFORM_MACOS = (unsigned long)(PLATFORM_MACOS);
static const unsigned long sys_VM_PROT_READ = (unsigned long)(VM_PROT_READ);
static const unsigned long sys_VM_PROT_WRITE = (unsigned long)(VM_PROT_WRITE);
static const unsigned long sys_VM_PROT_EXECUTE = (unsigned long)(VM_PROT_EXECUTE);
static const unsigned long sys_S_REGULAR = (unsigned long)(S_REGULAR);
static const unsigned long sys_S_ZEROFILL = (unsigned long)(S_ZEROFILL);
static const unsigned long sys_S_CSTRING_LITERALS = (unsigned long)(S_CSTRING_LITERALS);
static const unsigned long sys_S_ATTR_PURE_INSTRUCTIONS = (unsigned long)(S_ATTR_PURE_INSTRUCTIONS);
static const unsigned long sys_S_ATTR_SOME_INSTRUCTIONS = (unsigned long)(S_ATTR_SOME_INSTRUCTIONS);
static const unsigned long sys_N_STAB = (unsigned long)(N_STAB);
static const unsigned long sys_N_PEXT = (unsigned long)(N_PEXT);
static const unsigned long sys_N_TYPE = (unsigned long)(N_TYPE);
static const unsigned long sys_N_EXT = (unsigned long)(N_EXT);
static const unsigned long sys_N_UNDF = (unsigned long)(N_UNDF);
static const unsigned long sys_N_ABS = (unsigned long)(N_ABS);
static const unsigned long sys_N_SECT = (unsigned long)(N_SECT);
static const unsigned long sys_ARM64_RELOC_UNSIGNED = (unsigned long)(ARM64_RELOC_UNSIGNED);
static const unsigned long sys_ARM64_RELOC_SUBTRACTOR = (unsigned long)(ARM64_RELOC_SUBTRACTOR);
static const unsigned long sys_ARM64_RELOC_BRANCH26 = (unsigned long)(ARM64_RELOC_BRANCH26);
static const unsigned long sys_ARM64_RELOC_PAGE21 = (unsigned long)(ARM64_RELOC_PAGE21);
static const unsigned long sys_ARM64_RELOC_PAGEOFF12 = (unsigned long)(ARM64_RELOC_PAGEOFF12);
static const unsigned long sys_ARM64_RELOC_GOT_LOAD_PAGE21 = (unsigned long)(ARM64_RELOC_GOT_LOAD_PAGE21);
static const unsigned long sys_ARM64_RELOC_GOT_LOAD_PAGEOFF12 = (unsigned long)(ARM64_RELOC_GOT_LOAD_PAGEOFF12);
static const unsigned long sys_ARM64_RELOC_POINTER_TO_GOT = (unsigned long)(ARM64_RELOC_POINTER_TO_GOT);
static const unsigned long sys_ARM64_RELOC_ADDEND = (unsigned long)(ARM64_RELOC_ADDEND);
static const unsigned long sys_X86_64_RELOC_UNSIGNED = (unsigned long)(X86_64_RELOC_UNSIGNED);
static const unsigned long sys_X86_64_RELOC_SIGNED = (unsigned long)(X86_64_RELOC_SIGNED);
static const unsigned long sys_X86_64_RELOC_BRANCH = (unsigned long)(X86_64_RELOC_BRANCH);
static const unsigned long sys_X86_64_RELOC_GOT_LOAD = (unsigned long)(X86_64_RELOC_GOT_LOAD);
static const unsigned long sys_X86_64_RELOC_GOT = (unsigned long)(X86_64_RELOC_GOT);
static const unsigned long sys_X86_64_RELOC_SUBTRACTOR = (unsigned long)(X86_64_RELOC_SUBTRACTOR);
static const size_t syssz_mach_header_64 = sizeof(struct mach_header_64);
static const size_t syssz_segment_command_64 = sizeof(struct segment_command_64);
static const size_t syssz_section_64 = sizeof(struct section_64);
static const size_t syssz_symtab_command = sizeof(struct symtab_command);
static const size_t syssz_build_version_command = sizeof(struct build_version_command);
static const size_t syssz_nlist_64 = sizeof(struct nlist_64);
static const size_t syssz_relocation_info = sizeof(struct relocation_info);
static const size_t sysoff_mach_header_64_flags = offsetof(struct mach_header_64,flags);
static const size_t sysoff_mach_header_64_reserved = offsetof(struct mach_header_64,reserved);
static const size_t sysoff_segment_command_64_nsects = offsetof(struct segment_command_64,nsects);
static const size_t sysoff_segment_command_64_initprot = offsetof(struct segment_command_64,initprot);
static const size_t sysoff_section_64_flags = offsetof(struct section_64,flags);
static const size_t sysoff_section_64_reloff = offsetof(struct section_64,reloff);
static const size_t sysoff_section_64_align = offsetof(struct section_64,align);
static const size_t sysoff_section_64_offset = offsetof(struct section_64,offset);
static const size_t sysoff_nlist_64_n_value = offsetof(struct nlist_64,n_value);
static const size_t sysoff_nlist_64_n_sect = offsetof(struct nlist_64,n_sect);
static const size_t sysoff_nlist_64_n_desc = offsetof(struct nlist_64,n_desc);
static const size_t sysoff_symtab_command_strsize = offsetof(struct symtab_command,strsize);
static const size_t sysoff_build_version_command_sdk = offsetof(struct build_version_command,sdk);
static const size_t sysoff_relocation_info_r_address = offsetof(struct relocation_info,r_address);
#undef MH_MAGIC_64
#undef MH_OBJECT
#undef MH_SUBSECTIONS_VIA_SYMBOLS
#undef CPU_TYPE_X86_64
#undef CPU_TYPE_ARM64
#undef CPU_SUBTYPE_X86_64_ALL
#undef CPU_SUBTYPE_ARM64_ALL
#undef LC_SEGMENT_64
#undef LC_SYMTAB
#undef LC_BUILD_VERSION
#undef PLATFORM_MACOS
#undef VM_PROT_READ
#undef VM_PROT_WRITE
#undef VM_PROT_EXECUTE
#undef S_REGULAR
#undef S_ZEROFILL
#undef S_CSTRING_LITERALS
#undef S_ATTR_PURE_INSTRUCTIONS
#undef S_ATTR_SOME_INSTRUCTIONS
#undef N_STAB
#undef N_PEXT
#undef N_TYPE
#undef N_EXT
#undef N_UNDF
#undef N_ABS
#undef N_SECT
#undef ARM64_RELOC_UNSIGNED
#undef ARM64_RELOC_SUBTRACTOR
#undef ARM64_RELOC_BRANCH26
#undef ARM64_RELOC_PAGE21
#undef ARM64_RELOC_PAGEOFF12
#undef ARM64_RELOC_GOT_LOAD_PAGE21
#undef ARM64_RELOC_GOT_LOAD_PAGEOFF12
#undef ARM64_RELOC_POINTER_TO_GOT
#undef ARM64_RELOC_ADDEND
#undef X86_64_RELOC_UNSIGNED
#undef X86_64_RELOC_SIGNED
#undef X86_64_RELOC_BRANCH
#undef X86_64_RELOC_GOT_LOAD
#undef X86_64_RELOC_GOT
#undef X86_64_RELOC_SUBTRACTOR
#define mach_header_64 my_mach_header_64
#define segment_command_64 my_segment_command_64
#define section_64 my_section_64
#define symtab_command my_symtab_command
#define build_version_command my_build_version_command
#define nlist_64 my_nlist_64
#define relocation_info my_relocation_info
#include "macho.h"
static int bad;
#define VAL(n) do { if ((unsigned long)(n) != sys_##n) { printf("VAL  %-32s ours %#lx sdk %#lx\n", #n, (unsigned long)(n), sys_##n); bad++; } } while (0)
#define SZ(s) do { if (sizeof(struct my_##s) != syssz_##s) { printf("SIZE %-32s ours %zu sdk %zu\n", #s, sizeof(struct my_##s), syssz_##s); bad++; } } while (0)
#define OFF(s,f) do { if (offsetof(struct my_##s,f) != sysoff_##s##_##f) { printf("OFF  %-24s.%-10s ours %zu sdk %zu\n", #s, #f, offsetof(struct my_##s,f), sysoff_##s##_##f); bad++; } } while (0)
int main(void) {
    int n = 62;
    VAL(MH_MAGIC_64);
    VAL(MH_OBJECT);
    VAL(MH_SUBSECTIONS_VIA_SYMBOLS);
    VAL(CPU_TYPE_X86_64);
    VAL(CPU_TYPE_ARM64);
    VAL(CPU_SUBTYPE_X86_64_ALL);
    VAL(CPU_SUBTYPE_ARM64_ALL);
    VAL(LC_SEGMENT_64);
    VAL(LC_SYMTAB);
    VAL(LC_BUILD_VERSION);
    VAL(PLATFORM_MACOS);
    VAL(VM_PROT_READ);
    VAL(VM_PROT_WRITE);
    VAL(VM_PROT_EXECUTE);
    VAL(S_REGULAR);
    VAL(S_ZEROFILL);
    VAL(S_CSTRING_LITERALS);
    VAL(S_ATTR_PURE_INSTRUCTIONS);
    VAL(S_ATTR_SOME_INSTRUCTIONS);
    VAL(N_STAB);
    VAL(N_PEXT);
    VAL(N_TYPE);
    VAL(N_EXT);
    VAL(N_UNDF);
    VAL(N_ABS);
    VAL(N_SECT);
    VAL(ARM64_RELOC_UNSIGNED);
    VAL(ARM64_RELOC_SUBTRACTOR);
    VAL(ARM64_RELOC_BRANCH26);
    VAL(ARM64_RELOC_PAGE21);
    VAL(ARM64_RELOC_PAGEOFF12);
    VAL(ARM64_RELOC_GOT_LOAD_PAGE21);
    VAL(ARM64_RELOC_GOT_LOAD_PAGEOFF12);
    VAL(ARM64_RELOC_POINTER_TO_GOT);
    VAL(ARM64_RELOC_ADDEND);
    VAL(X86_64_RELOC_UNSIGNED);
    VAL(X86_64_RELOC_SIGNED);
    VAL(X86_64_RELOC_BRANCH);
    VAL(X86_64_RELOC_GOT_LOAD);
    VAL(X86_64_RELOC_GOT);
    VAL(X86_64_RELOC_SUBTRACTOR);
    SZ(mach_header_64);
    SZ(segment_command_64);
    SZ(section_64);
    SZ(symtab_command);
    SZ(build_version_command);
    SZ(nlist_64);
    SZ(relocation_info);
    OFF(mach_header_64,flags);
    OFF(mach_header_64,reserved);
    OFF(segment_command_64,nsects);
    OFF(segment_command_64,initprot);
    OFF(section_64,flags);
    OFF(section_64,reloff);
    OFF(section_64,align);
    OFF(section_64,offset);
    OFF(nlist_64,n_value);
    OFF(nlist_64,n_sect);
    OFF(nlist_64,n_desc);
    OFF(symtab_command,strsize);
    OFF(build_version_command,sdk);
    OFF(relocation_info,r_address);
    printf(bad ? "%d MISMATCH(ES)\n" : "all %d checks agree with the macOS SDK\n", bad ? bad : n);
    return bad != 0; }
