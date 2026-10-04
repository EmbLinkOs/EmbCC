# Object files

This page describes how EmbCC writes relocatable object files: the
hand-off from a backend to the driver, the machine-neutral relocation
kinds, and the three writers for ELF (`src/elf`), Mach-O (`src/macho`)
and COFF (`src/coff`). It also covers what the driver places in each
object: sections, symbols, debug information and unwind tables. It is
for people who change object emission or add a target. The linker that
consumes these objects is described in [EmbLD](linker.md).

## From code to an object

A backend's `codegen_unit*` function (see [Backends](backends.md))
returns:

- the whole unit's machine code as one `struct code` buffer
  (`src/arch/code.h`), with each function's `code_off` and `code_len`
  filled in on its `struct func`, and the ranges of the buffer that hold
  data (jump tables) recorded with `code_mark_data`;
- four lists of *sites*, each a field in the code buffer that a
  relocation must fill (`src/arch/backend.h`):

| List | Structure | A site is |
|---|---|---|
| `ext` | `struct extcall` | A call to a function not defined in the unit. `tail` marks a tail call (a branch). |
| `strs` | `struct strsite` | A reference to a string literal in `.rodata` (`str_off` is its offset there). |
| `gs` | `struct gsite` | A reference to a global object's address. |
| `fs` | `struct fsite` | A reference to a function's address, with an optional `addend` for `RK_ABS64` jump-table entries. |

Each site records its offset in the code buffer (`patch_off`) and a
relocation *kind*. Calls between functions of the same unit are
resolved by the backend and leave no site.

The driver (`compile_unit` in `src/driver/main.c`) then:

1. lays out the defined globals: initialized objects in `.data`,
   zero-initialized ones in `.bss`, thread-locals in `.tdata` and
   `.tbss`, objects with a `section` attribute in their named section,
   and, for ELF output, `const` objects that are not `volatile` in
   `.rodata` after the string literals;
2. builds the `.rodata` image from the IR string pool (`ir_unit::strs`)
   and the `const` objects;
3. appends each file-scope `asm` block's bytes to the code buffer:
   on x86-64 and AArch64 16-byte aligned and padded with `0x90`; on the
   embedded targets aligned as the block asks (at least 4 bytes, 2 on
   AVR), padded with zeros, with the block's data ranges marked for
   `$d` mapping symbols;
4. runs the DWARF emitter (`dwarf_emit`) for `-g` and the unwind-table
   emitter (`eh_emit`) when unwind tables are wanted;
5. hands everything to the writer for the target's object format
   (`target_fmt_get()`), or, for `-S`, to `asm_emit_unit`
   (`src/driver/asmout.c`), which prints the same bytes as `.byte`
   directives with the relocations written out.

Unwind tables are emitted when asked for (`-funwind-tables`,
`-fasynchronous-unwind-tables`, `-fexceptions`), for every C++ unit
unless `-fno-exceptions` and `-fno-unwind-tables` are both given, and by
default for a hosted ELF target (`*-linux-gnu`).

### Functions in their own section

A function with `__attribute__((section("NAME")))` -- or, with
`-ffunction-sections`, every function, in `.text.NAME` -- is placed after
every other function in the code buffer, grouped by section name in
first-seen order. The ELF writer gives each group its own executable
section; `text_at` maps a code-buffer offset to its section and offset
there, and every reference into code (a symbol, a call's relocation, an
unwind table's, DWARF's) goes through it. DWARF's END addresses -- a
`high_pc`, the end of a range -- are mapped as the byte before, plus one,
since the byte after a function's last one is where the next section
begins in the buffer (`dwarf_reloc.end`). A unit whose code is split
gets `DW_AT_ranges` (`dwarf_emit_split`, `.debug_ranges`). ELF output
only.

The writer (`src/elf/write.c`) grows its section and relocation-group
tables as needed; a unit with thousands of functions gets thousands of
sections. The one limit left is ELF's: a section index at or above
`SHN_LORESERVE` (0xff00) needs extended numbering, which it refuses by
name.

A data object may not share a section name with a function.

## Relocation kinds

Codegen records a machine-neutral `enum reloc_kind` at each site, and
`src/arch/target.c` maps the kind to the object format's relocation
type: `target_reloc_type` for ELF, `target_macho_reloc` for Mach-O and
`target_coff_reloc` for COFF. The indirection exists because one act
costs a different number of relocations on each machine: taking an
address is one RIP-relative `lea` on x86-64, an `adrp`/`add` pair on
AArch64, a `movw`/`movt` pair on ARMv7-M, an `auipc`/`addi` pair on
RISC-V and two `ldi` on AVR.

ELF relocation types by kind:

| Kind | x86-64 | AArch64 | ARMv7-M | RISC-V | AVR |
|---|---|---|---|---|---|
| `RK_CALL` | `R_X86_64_PLT32` | `R_AARCH64_CALL26` | `R_ARM_THM_CALL` | `R_RISCV_CALL_PLT` | `R_AVR_CALL` |
| `RK_TAIL` | as `RK_CALL` | `R_AARCH64_JUMP26` | `R_ARM_THM_JUMP24` | as `RK_CALL` | as `RK_CALL` |
| `RK_PCREL32` | `R_X86_64_PC32` | | | | |
| `RK_ADR_HI21`, `RK_ADD_LO12` | | `R_AARCH64_ADR_PREL_PG_HI21`, `R_AARCH64_ADD_ABS_LO12_NC` | | | |
| `RK_GOT_PAGE`, `RK_GOT_LO12` | | `R_AARCH64_ADR_GOT_PAGE`, `R_AARCH64_LD64_GOT_LO12_NC` | | | |
| `RK_THM_MOVW`, `RK_THM_MOVT` | | | `R_ARM_THM_MOVW_ABS_NC`, `R_ARM_THM_MOVT_ABS` | | |
| `RK_RISCV_PCREL_HI20`, `RK_RISCV_PCREL_LO12_I` | | | | `R_RISCV_PCREL_HI20`, `R_RISCV_PCREL_LO12_I` | |
| `RK_AVR_LO8_LDI`, `RK_AVR_HI8_LDI` | | | | | `R_AVR_LO8_LDI`, `R_AVR_HI8_LDI` |
| `RK_AVR_LO8_LDI_GS`, `RK_AVR_HI8_LDI_GS` | | | | | `R_AVR_LO8_LDI_GS`, `R_AVR_HI8_LDI_GS` |
| `RK_AVR_TEXT_CALL` | | | | | `R_AVR_CALL` |
| `RK_ABS64` | `R_X86_64_64` | `R_AARCH64_ABS64` | | `R_RISCV_64` (RV64) | |
| `RK_ABS32` | `R_X86_64_32` | `R_AARCH64_ABS32` | `R_ARM_ABS32` | `R_RISCV_32` | `R_AVR_32` |
| `RK_AVR_ABS16`, `RK_AVR_ABS16_PM` | | | | | `R_AVR_16`, `R_AVR_16_PM` |
| `RK_DATA_PREL32` | `R_X86_64_PC32` | `R_AARCH64_PREL32` | `R_ARM_REL32` | | |
| `RK_TPOFF32` | `R_X86_64_TPOFF32` | | | | |
| `RK_TPREL_HI12`, `RK_TPREL_LO12` | | `R_AARCH64_TLSLE_ADD_TPREL_HI12`, `R_AARCH64_TLSLE_ADD_TPREL_LO12_NC` | | | |

An empty cell is a kind the target does not use; `target_reloc_type`
returns -1 for it, which is a codegen bug rather than an input error.
`RK_RISCV_CALL` and `RK_AVR_CALL` are defined, but the backends record
`RK_CALL` for calls.

Notes on particular kinds:

- **Addends.** `target_reloc_addend(arch, kind, bias)` returns the
  addend. x86-64's PC-relative fields (`RK_CALL`, `RK_PCREL32`) are
  measured from the end of the instruction, so their addend is
  `bias - 4`; every other target's are measured from the instruction
  and use `bias` unchanged.
- **RISC-V's low half.** `R_RISCV_PCREL_LO12_I` names the `auipc` that
  computed the high half, not the target. The driver emits it against
  the section symbol of the function's section, with the `auipc`'s
  offset (the site's offset minus 4) as the addend.
- **AVR's long jump.** A jump to a label in the same `.text` that is
  out of `rjmp` range uses `RK_AVR_TEXT_CALL`: a `jmp` with an absolute
  word address, relocated against the `.text` section symbol with the
  label's offset as the addend.
- **AVR's two address spaces.** A function's address is a word address
  in program space: `_GS` and `_PM` kinds halve the byte address, the
  plain kinds do not.
- **Thumb function symbols.** A Thumb function's symbol value has bit 0
  set (`fn_sym_value`), so `bx`/`blx` stay in Thumb state.

Data words that hold an address (pointer initializers, vtable entries,
`.init_array` slots) use the pointer-width kind: `RK_ABS64` on the LP64
targets, `RK_ABS32` on ARMv7-M and RV32, `RK_AVR_ABS16` on AVR, and
`RK_AVR_ABS16_PM` for a function pointer on AVR.

## ELF

`src/elf/elf.h` defines the ELF structures and constants by hand (EmbCC
compiles its own source, so it includes no system `<elf.h>`).
`src/elf/write.c` is the relocatable-object writer. It is shared by the
compiler, `embas` (`src/arch/x86_64/as.c`) and the GNU-syntax assembler
(`src/as/gas.c`).

### Writer interface

| Function | Purpose |
|---|---|
| `elfw_new(machine)` | A writer for `e_machine`. The class is ELFCLASS32 when the target's pointer is 4 bytes or fewer, ELFCLASS64 otherwise. ARM's `e_flags` is set to `EF_ARM_EABI_VER5`. |
| `elfw_set_flags(w, flags)` | `e_flags` from `target_elf_flags`: `EF_RISCV_RVC` on RISC-V, `EF_AVR_ARCH_AVR5` on AVR. |
| `elfw_add_section(w, name, type, flags, data, size, align)` | Appends a section; returns its index. The data is copied. |
| `elfw_add_symbol(w, name, value, size, info, shndx)` | Appends a symbol; returns its index. Local symbols must all come before global ones: a local added after a global is an internal error. |
| `elfw_symbol_visibility(w, sym, stv)` | Sets `st_other`. |
| `elfw_add_rela(w, target, offset, sym, type, addend)` | Adds a relocation to the group for section `target`. |
| `elfw_write(w, path)` | Writes the object through `plat_write_file`. |

The writer builds 64-bit structures and converts them field by field to
the 32-bit layout when writing an ELFCLASS32 object. Relocations are
always `SHT_RELA`, one `.rela.NAME` section per relocated section, on
every target. The file layout is the ELF header, the section payloads
in order, the `.rela.*` sections, `.symtab`, `.strtab`, `.shstrtab`, and
the section header table. The image is built in memory, zero-filled,
and written once.

Limits: 32 sections (`ELFW_MAX_SECTIONS`) and 6 relocated sections
(`ELFW_MAX_RELA`) per object. Exceeding either is an error naming the
limit.

### Sections in a compiled object

| Section | Type | Flags | Alignment | Present when |
|---|---|---|---|---|
| `.text` | `PROGBITS` | `AX` | 16 | always |
| a function section `NAME` | `PROGBITS` | `AX` | 16 | a function has `section("NAME")` |
| `.rodata` | `PROGBITS` | `A` | at least 16 | string literals or `const` objects |
| `.data` | `PROGBITS` | `WA` | the strictest object's | initialized writable objects |
| `.bss` | `NOBITS` | `WA` | the strictest object's | zero-initialized objects |
| `.tdata`, `.tbss` | `PROGBITS`, `NOBITS` | `WAT` | the strictest object's | thread-local objects |
| a data section `NAME` | `NOBITS` for `.bss` and `.bss.*`, otherwise `PROGBITS` | `A`, plus `X` for `.text*`, plus `W` unless `.rodata*` | the strictest object's | an object has `section("NAME")` |
| `.init_array`, `.fini_array` | `INIT_ARRAY`, `FINI_ARRAY` | `WA` | pointer size | `constructor`, `destructor` functions |
| `.debug_abbrev`, `.debug_info`, `.debug_line` | `PROGBITS` | none | 1 | `-g` |
| `.eh_frame` | `PROGBITS` on AArch64, `X86_64_UNWIND` otherwise | `A` | 8 | unwind tables |
| `.gcc_except_table` | `PROGBITS` | `A` | 4 | a function has exception regions |
| `.ARM.attributes` | `ARM_ATTRIBUTES` | none | 1 | ARMv7-M and ARMv8-M |
| `.riscv.attributes` | `RISCV_ATTRIBUTES` | none | 1 | RISC-V |

An object with a `section` attribute and an initializer cannot be
placed in a `NOBITS` section; a thread-local object cannot have a
`section` attribute. The `.init_array` slots are zero in the object and
filled by relocations against the functions, in source order; a
constructor priority is refused by the parser.

`.ARM.attributes` (`src/arch/thumb/attrs.c`) records the architecture
and the floating-point ABI (`Tag_CPU_arch`, `Tag_FP_arch`,
`Tag_ABI_VFP_args`, `Tag_ABI_enum_size` and others), which a linker
compares across objects. `.riscv.attributes` (`riscv_build_attributes`)
records the ISA string and the stack alignment.

### Symbols

The driver adds symbols in this order, which keeps every local before
every global:

1. an `STT_FILE` symbol whose name is the source file's basename, so
   the object does not depend on the directory the build ran in;
2. `STT_SECTION` symbols for `.gcc_except_table`, `.text` and each
   function section;
3. on ARMv7-M and AArch64, the mapping symbols: `$t` (Thumb) or `$x`
   (A64) at offset 0 of each code section, and `$d` at the start of each
   data range in code (a jump table), with the code symbol again where
   it ends;
4. `STT_SECTION` symbols for `.rodata` and the debug sections;
5. local functions (`static` and used) and their aliases as `STT_FUNC`,
   then `static` objects as `STT_OBJECT` or `STT_TLS`;
6. global functions and aliases (`STB_GLOBAL`, or `STB_WEAK` for a weak
   definition), then global objects;
7. the `.global` labels of file-scope `asm` blocks, as global `STT_FUNC`
   (typed and sized by `.type` and `.size`); a block label that is not
   global but names a function or object the C code declares and does
   not define (a `static` naked function) is a local symbol, among the
   locals of step 5;
8. undefined symbols (`SHN_UNDEF`, `STT_NOTYPE`) for referenced external
   objects and, as their sites are processed, for called or
   address-taken external functions; a weak declaration gives a weak
   undefined symbol.

Every defined function and object has its size in `st_size`. An
`alias` attribute adds a symbol at its target's value and size.
`visibility("...")` sets `st_other` after the symbols exist.

### Relocations

| Source | Section | Symbol | Kind |
|---|---|---|---|
| `ext` sites | the code section | the callee's symbol | `RK_CALL`, or `RK_TAIL` for a tail call |
| constructor and destructor slots | `.init_array`, `.fini_array` | the function | pointer width |
| file-scope `asm` | `.text` | the named function, or a new undefined symbol (one per name); `.text`'s section symbol, addend the label's offset, for a field naming a local `.L` label | the ELF type the block's assembler chose (embedded targets), else `RK_CALL` for `call sym`, `RK_ABS64` for `.quad sym` |
| `strs` sites | the code section | `.rodata`'s section symbol, addend the string's offset | the site's kind |
| `gs` sites | the code section | the object's symbol | the site's kind |
| pointer initializers | `.data`, `.rodata` or the named section | the target's symbol, or `.rodata`'s section symbol for a string | pointer width |
| `fs` sites | the code section | the function's symbol | the site's kind |
| DWARF fields | the debug section | `.text`, `.debug_abbrev` or `.debug_line` section symbol | `RK_ABS64` for 8-byte fields, `RK_ABS32` otherwise |
| unwind and exception tables | `.eh_frame`, `.gcc_except_table` | `.text`, `.gcc_except_table`, `__gxx_personality_v0` or a typeinfo object | `RK_DATA_PREL32` |

Relocations whose site is in a function section are emitted against that
section (`code_rela`).

### Thread-local storage

Thread-local objects go in `.tdata` and `.tbss` with `SHF_TLS` and
`STT_TLS` symbols. On x86-64 an access uses the local-exec model:
`R_X86_64_TPOFF32` (`RK_TPOFF32`). On AArch64 it uses the
`R_AARCH64_TLSLE_ADD_TPREL_HI12`/`_LO12_NC` pair. ARMv7-M, RISC-V and
AVR address a thread-local object with the same relocations as an
ordinary object.

## Debug information

`dwarf_emit(iu, filename, &out)` in `src/debug/dwarf.c` builds three
sections from the IR unit after code generation:

- `.debug_abbrev`: the abbreviations;
- `.debug_info`: one DWARF 4 compile unit (32-bit format) with
  `DW_AT_producer` `"EmbCC"`, `DW_AT_language` `DW_LANG_C99`,
  `DW_AT_name` the source path as given, `DW_AT_comp_dir` `"."`, the
  unit's `.text` range and `DW_AT_stmt_list`; type DIEs for every type a
  function returns or a variable has; and one `DW_TAG_subprogram` per
  function with its range, frame base, parameters
  (`DW_TAG_formal_parameter`) and locals (`DW_TAG_variable`), each
  located with `DW_OP_fbreg`;
- `.debug_line`: a DWARF 4 line program, one row where the line changes,
  bracketed per function by `DW_LNE_set_address` and
  `DW_LNE_end_sequence`, from the `(offset, line)` pairs code generation
  recorded in `ir_func::lines`.

The frame base is `DW_OP_reg6` (`rbp`) on x86-64, `DW_OP_reg29` (`x29`)
on AArch64, and the stack pointer plus 0 (`DW_OP_breg`) on ARMv7-M and
RISC-V, whose frames are addressed from `sp`. A backend that moves `sp`
in the body (an `alloca` or a VLA) refuses `-g` for that function.

The output is deterministic: no timestamps, and a fixed `comp_dir`.
Every field that holds a `.text` address or a debug-section offset is
written as zero and returned as a `struct dwarf_reloc`, which the driver
turns into an ELF relocation; the emitter itself has no ELF dependency.
Address fields are 4 bytes on ARMv7-M and RV32 and 8 bytes on the 64-bit
targets.

Known defect: on AVR the compile unit header gives an address size of
2, but address fields are written 8 bytes wide with `R_AVR_32`
relocations, so the `.debug_info` it produces does not parse.

`-g` is refused for Mach-O and COFF output (see below).

## Unwind and exception tables

`eh_emit(iu, arm64, &out)` in `src/debug/eh.c` writes `.eh_frame` (a CIE,
a second CIE naming `__gxx_personality_v0` when any function has
exception regions, and one FDE per function) and, for functions with
exception regions, their LSDAs in `.gcc_except_table`. The CFI is
derived from the prologue facts the backend recorded on each
`struct func` (`cfi_push`, `cfi_frame`, `cfi_saved_at`, `cfi_reg`,
`cfi_off`, `cfi_frameless`, `cfi_pushonly`). Pointers are written as
zero and returned as `struct eh_reloc` entries (32-bit PC-relative,
`RK_DATA_PREL32`). The emitter also records each function as a
`struct eh_func` for formats that describe functions rather than
instructions (Darwin's compact unwind).

The tables describe x86-64 when `arm64` is 0 and AArch64 when it is 1;
the driver passes 1 only for AArch64. Known defect: on ARMv7-M and
RISC-V, `-funwind-tables` produces an `.eh_frame` in the x86-64 layout
with section type `SHT_X86_64_UNWIND`, and on RISC-V its relocation
type is invalid (-1), because `RK_DATA_PREL32` has no RISC-V mapping.

## Mach-O

`src/macho/write.c` writes `MH_OBJECT` files for `x86_64-apple-darwin`
and `aarch64-apple-darwin` (`CPU_TYPE_X86_64`, `CPU_TYPE_ARM64`). Its
interface mirrors the ELF writer's.

| Function | Purpose |
|---|---|
| `machow_new(cputype, cpusubtype)` | A writer. |
| `machow_add_section(w, seg, sect, flags, data, size, align)` | Appends a section with alignment given as a power of two; returns its 1-based index. The section's address in the object is fixed here, consecutively from 0. |
| `machow_add_symbol`, `machow_add_symbol_weak` | A symbol with the platform's leading `_` added. `value` is an offset in the section; the writer stores the address. Returns a 1-based handle. |
| `machow_add_symbol_raw` | A symbol with the name as given, for the assembler-temporary anchors `ltmp_const`, `ltmp_lsda` and `ltmp_text`. |
| `machow_add_reloc` | A relocation against a symbol. |
| `machow_add_reloc_sub` | "target minus here": a `SUBTRACTOR` naming an anchor at the start of the section, followed by an `UNSIGNED` naming the target, at one address. |
| `machow_add_reloc_sect` | A relocation against a section rather than a symbol (`r_extern` 0). |
| `machow_section_addr` | A section's address in the object. |
| `machow_write` | Writes the file. |

Mach-O orders the symbol table as locals, then defined externals, then
undefined symbols. The writer refuses a symbol added out of that order
(`internal error: a local Mach-O symbol (...) after a ... one`), and the
driver adds all symbols, including every undefined one, before the first
relocation.

A Mach-O relocation has no addend field. The driver writes the value
into the field before handing the section to the writer (sections are
copied when added). On AArch64, an instruction field that needs an
addend gets an `ARM64_RELOC_ADDEND` entry in front of its relocation.

The file has three load commands: `LC_SEGMENT_64` with one unnamed
segment containing every section, `LC_BUILD_VERSION` (macOS, minimum
11.0), and `LC_SYMTAB`. The header sets `MH_SUBSECTIONS_VIA_SYMBOLS`.
Section data follows the load commands, then the relocations, the
symbol table and the string table.

| Section | Contents |
|---|---|
| `__TEXT,__text` | Code, aligned to 16. |
| `__TEXT,__const` | The string literals. `const` objects stay in `__data`. |
| `__DATA,__data`, `__DATA,__bss` | Initialized and zero-initialized objects. |
| `__DATA,NAME` | An object with `section("NAME")`. |
| `__TEXT,__gcc_except_table` | LSDAs. |
| `__LD,__compact_unwind` | One 32-byte entry per function: address, length, encoding, personality, LSDA. The encoding is `0x04000000` on arm64 and `0x01000000` on x86-64, with `0x40000000` when the function has an LSDA. |

Darwin does not use `.eh_frame`: unwind information is the
`__compact_unwind` entries, which the system linker folds into
`__unwind_info`. A typeinfo in an LSDA is named indirectly through a GOT
entry (`ARM64_RELOC_POINTER_TO_GOT`, `X86_64_RELOC_GOT`).

Mach-O relocation types by kind (`target_macho_reloc`):

| Kind | x86-64 | arm64 |
|---|---|---|
| `RK_CALL` | `X86_64_RELOC_BRANCH` | `ARM64_RELOC_BRANCH26` |
| `RK_PCREL32` | `X86_64_RELOC_SIGNED` | |
| `RK_ADR_HI21`, `RK_ADD_LO12` | | `ARM64_RELOC_PAGE21`, `ARM64_RELOC_PAGEOFF12` |
| `RK_GOT_PAGE`, `RK_GOT_LO12` | | `ARM64_RELOC_GOT_LOAD_PAGE21`, `ARM64_RELOC_GOT_LOAD_PAGEOFF12` |
| `RK_ABS64`, `RK_ABS32` | `X86_64_RELOC_UNSIGNED` | `ARM64_RELOC_UNSIGNED` |

A kind with no Mach-O spelling is refused, for example
`no Mach-O relocation for a global reference here`.

Refused for Mach-O output, each with an error naming the feature:
`-g`; a file-scope `asm` block with labels or symbol references;
`__thread`; `constructor` and `destructor`; the `alias` attribute; a
function with a `section` attribute.

## COFF

`src/coff/write.c` writes COFF objects for `x86_64-windows-gnu`
(`IMAGE_FILE_MACHINE_AMD64`). Every record is written field by field,
little-endian, because COFF's records (18-byte symbols, 10-byte
relocations) are not C structure sizes.

| Function | Purpose |
|---|---|
| `coffw_new(machine)` | A writer. |
| `coffw_add_section(w, name, flags, data, size, align)` | Appends a section; `align` is a byte count up to 8192, encoded in the characteristics. `data` `NULL` means no file bytes (`.bss`). Returns the 1-based section number. |
| `coffw_add_symbol(w, name, value, section, type, class)` | Appends a symbol; returns its index. An undefined symbol has section 0 and value 0. |
| `coffw_add_reloc(w, section, off, sym, type)` | A relocation; there is no addend. |
| `coffw_write(w, path)` | Writes the file. |

The layout is the file header (timestamp 0), the section headers, the
section data, each section's relocations, the symbol table, and the
string table, which is always present. A name longer than eight bytes
goes in the string table: a section header holds `/OFFSET`, a symbol
holds four zero bytes and the offset. A section may have at most 65535
relocations.

The driver emits `.text` (aligned to 16), `.rdata` (the string literals,
aligned to 16), `.data` and `.bss`, a static section symbol for
`.rdata`, the defined functions (`IMAGE_SYM_DTYPE_FUNCTION`) and
objects, and undefined symbols. Because there is no addend, the driver
writes each addend into the field first: address words in `.data`, and
each string's `.rdata` offset into its `rel32` field. COFF's `REL32`
is measured from the end of the field, so no `-4` is applied.

| Kind | COFF type |
|---|---|
| `RK_CALL`, `RK_PCREL32` | `IMAGE_REL_AMD64_REL32` |
| `RK_ABS64` | `IMAGE_REL_AMD64_ADDR64` |
| `RK_ABS32` | `IMAGE_REL_AMD64_ADDR32` |

Refused for COFF output: `-g`; a file-scope `asm` block with labels or
symbol references; `constructor` and `destructor`; `__thread`; unwind
tables (and therefore C++ exceptions); the `alias` attribute; a
function with a `section` attribute. Every compile for a Windows target
also prints the `-Wwindows-abi` warning, which states where the
generated code differs from the Microsoft x64 ABI.

Known defect: the COFF writer has no named data sections. An object
with `section("NAME")` is given a symbol in `.data` or `.bss` at its
offset within the missing section, and its initializer is not written.

## Other producers of objects

- `embas` and `embcc -c FILE.asm` assemble NASM-syntax x86-64 source
  (`src/arch/x86_64/as.c`) into ELF64 objects with the same writer.
  `embas -f bin` is not implemented and says so.
- `embcc -c FILE.s` and `FILE.S` assemble GNU-syntax source for
  AArch64, ARMv7-M, RISC-V and AVR (`src/as/gas.c`), encoding each
  statement with the same per-target assembler the compiler uses for
  inline `asm`. `.S` is preprocessed first. x86-64 is refused:
  `no assembly-file support for x86_64-elf yet`.
- `embcc --emit-empty-object FILE` writes an empty ELF object for the
  selected target, with a `.text` section and two local symbols.

## Related pages

- [EmbLD](linker.md): the linker that reads these objects.
- [Architecture](architecture.md): where object emission sits in the
  pipeline.
- [Targets](../manual/targets.md): which object format each triple
  uses.
