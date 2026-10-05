# The linker

This page describes how EmbLD, EmbCC's static linker, works inside: the
data structures in `src/link/link.c`, the order in which it reads,
resolves, lays out, relocates and writes, how each target's image is
shaped, and how each relocation type is computed. It is for people who
change `src/link`, `tools/embld` or the objects the backends emit. The
user-facing reference (synopsis, every option, the image layout as a user
sees it, the list of linker-defined symbols, examples) is
[embld](../manual/tools/embld.md); this page does not repeat it. The
objects EmbLD reads are described in [Object files](object-formats.md).

## Source files

| File | Contents |
|---|---|
| `src/link/link.h` | `struct link_opts` and `embld_link`, the whole public interface |
| `src/link/link.c` | the linker: input parsing, archives, symbol resolution, layout, relocation, ELF and EMBX output, the `.embdbg` sidecar |
| `tools/embld/embld.c` | the `embld` command: argument parsing into `struct link_opts`, then `embld_link` or `doctor_run` |
| `tools/embld/doctor.c`, `doctor.h` | `embld --doctor`; independent of `link.c` |
| `src/elf/elf.h` | ELF32 and ELF64 structures and the relocation constants, written by hand (no system `<elf.h>`) |
| `src/embx/embx.c`, `embx.h` | the EMBX header, segment and capability structures, CRC32C, capability names |
| `src/arch/riscv/emit.c` | `rv_li`, `rv_auipc`, `rv_jalr`, `rv_enc_b`, `rv_enc_j`: the encoders the RISC-V relocations and the entry stub use |
| `src/arch/avr/emit.c` | `avr_patch_ldi_at`, `avr_patch_call_at`, `avr_patch_rjmp_at`, `avr_patch_br_at`: the field patchers the AVR relocations use |
| `tools/embdbg/embdbg.c` | `embdbg_emit_objects` (the `.embdbg` writer) and `embdbg_sha256` (the EMBX build ID), compiled with `-DEMBDBG_NO_MAIN` |
| `src/driver/explain.c` | `header_declaring`, the C-library name table `--doctor` consults |
| `src/driver/util.c`, `diag.c` | `xmalloc` and friends; `fatal_unwind`, which every linker error ends in |

The linker reuses the backends' encoders instead of restating instruction
layouts: a split immediate is written by the same function that writes it
when the compiler emits the instruction. Keep it that way when adding a
relocation (see [Changing the linker](#changing-the-linker)).

### How it is built

The `embld:` rule in the `Makefile` compiles the tool from source in one
command, with `-DEMBDBG_NO_MAIN -Wno-unused-function`:

```text
tools/embld/embld.c tools/embld/doctor.c src/link/link.c
src/driver/util.c src/driver/diag.c src/driver/explain.c
src/embx/embx.c tools/embdbg/embdbg.c $(PLATFORM_SRCS)
src/arch/x86_64/disasm.c src/arch/riscv/emit.c src/arch/avr/emit.c src/arch/code.c
```

A new file that `link.c` depends on must be added to both the
prerequisites and the recipe of that rule. `src/link/link.c` is also part
of `embcc` itself (it is in `SRCS`, and the driver links in-process), so
it must stay inside the C subset EmbCC compiles.

`tools/os-build-embld.sh`, which cross-builds `embld.elf` for EmbLinkOS,
keeps its own list (`SRCS=`) and does not read the `Makefile`'s.

## Design constraints

- **A library, not a subprocess.** EmbLinkOS has no `fork`/`exec`, so a
  compiler that links on the target must link in its own process.
  `embld_link` is a function; the `embld` command is a thin `main` over
  it.
- **Static output only.** The output is always `ET_EXEC`. There is no PLT,
  no GOT, no dynamic section and no position-independent output.
  `R_X86_64_PLT32` is applied as a plain PC-relative reference.
- **A fixed layout, or a GNU ld script.** Without `-T` the layout is fixed
  in code (`enum osec` and `layout`), and the only inputs to it are the
  options in `struct link_opts`. With `-T` (ARM and RISC-V), the script
  decides it: see [Linker scripts](#linker-scripts).
- **One machine per link**, decided by the first object added.
- **One 64-bit representation.** ELF32 inputs are converted into the
  `Elf64_*` structures at parse time. Only the relocation reader (whose
  entries have a different size) and the writer (which narrows the
  headers back) know the class.
- **Linear searches.** The symbol table, the orphan list, the merged-debug
  list and the RISC-V `PCREL_HI20` record are arrays scanned linearly.

## Interface

```c
int embld_link(const char **inputs, int ninputs, const char *out,
               const struct link_opts *opts);
```

`inputs` are object and archive paths in command-line order. The function
returns 0 after writing `out`. Every failure goes through `die()`, which
prints `embld: ` and the message on standard error and calls
`fatal_unwind()`. No fatal boundary is set around a link, so the process
exits with status 1; `embld_link` never returns 1.

The fields of `struct link_opts`:

| Field | Set by | Meaning when zero | Read in |
|---|---|---|---|
| `entry` | `-e` | `"_start"` | `embld_link` (entry lookup), the stub, `e_entry` |
| `base`, `have_base` | `-Ttext` | text at `DEFAULT_BASE` (`0x400000`) | `layout` |
| `data_base` | `-Tdata` | data follows text at the next 4 KiB boundary | `layout`, `write_exec` |
| `stack_top`, `have_stack` | `-Tstack` | no entry stub | `add_entry_stub`, `fill_entry_stub` |
| `lma_offset` | `--lma-offset` | `p_paddr == p_vaddr` | `write_exec` |
| `rom_limit` | `--rom-limit` | no flash limit | `layout` |
| `emit_embx` | `--embx` | write ELF | `embld_link` |
| `caps` | `--cap` (one bit per capability ID) | no capabilities | `emit_embx` |

`have_base` exists because 0 is a real text address on a microcontroller.
`data_base` has no such flag: `-Tdata 0` is indistinguishable from no
`-Tdata`, and gives the contiguous layout.

## Data structures

| Structure | One per | Holds |
|---|---|---|
| `struct object` | input object or pulled archive member | the file buffer; the section headers and symbol table (converted to ELF64 for ELF32 input); `sec_out[i]`, the `insecs` index of input section `i` or -1; `dbg_sec[i]`/`dbg_off[i]`, where a `.debug_*` section landed in its merged section; the machine, `e_flags` and the three ARM attribute values |
| `struct insec` | allocated input section placed in the output | owning object and section index, name, data (NULL for `SHT_NOBITS`), size, alignment, `osec` (output group), `seg` (`SEG_TEXT` or `SEG_DATA`), final `vaddr` |
| `struct symbol` | global name | defining object, `insec` index (-1 for absolute), value (section-relative until `finalize_symbols`, then absolute), `defined`, `weak`, `common`, type, size, alignment (for common) |
| `struct archive`, `struct member` | archive, member | member name as `LIB.a(MEMBER)`, its bytes, whether it was pulled, its parsed `struct object` (parsed on first inspection) |
| `struct dbgsec` | distinct `.debug_*` name | the concatenated bytes of every input section of that name |
| `struct orphan` | distinct orphan section name | name, and whether the first section seen with it was writable |
| `struct linker` | link | all of the above, the options, the output class, machine and `e_flags`, the Harvard flag, the thread-block start and size, the RISC-V `PCREL_HI20` record and the entry stub |

Local symbols never enter the global table. A relocation against a local
symbol is resolved inside its own object (`reloc_symval`).

## The link, step by step

`embld_link` runs these steps in order:

1. **Read the inputs**, left to right (`read_file`).
   - An object is parsed (`parse_object`) and added (`add_object`): the
     machine is checked against the first object's, its allocated
     sections are appended to `insecs` (`collect_sections`), its
     `.debug_*` sections are appended to the merged debug buffers, and
     its global symbols are merged into the table (`add_symbols`).
   - An archive is split into members (`parse_archive`), and then
     `pull_archives` runs over every archive read so far.
2. **Pull archives once more** after the last input, so a reference made
   by a later object can be satisfied from an earlier archive.
3. **Add the entry stub** if `-Tstack` was given (RISC-V only).
4. **Lay out** every output group (`layout`), place common symbols at the
   end of `.bss`, and check `--rom-limit`.
5. **Finalize symbols**: add each defined symbol's section address to its
   value (`finalize_symbols`).
6. **Define the linker symbols**: the bracket symbols of the fixed and
   orphan groups, the end-of-image symbols and the firmware symbols.
7. **Check the ARM build attributes** across all loaded objects
   (`arm_attrs_check`).
8. **Look up the entry symbol**, and fill in the entry stub if there is
   one.
9. **Apply relocations**, object by object in the order they were added,
   relocation section by relocation section, entry by entry
   (`apply_relocs`).
10. **Write** the ELF executable (`write_exec`) and then the `.embdbg`
    sidecar (`emit_embdbg`), or the EMBX image (`emit_embx`).

Every check stops the link, so the order above decides which error a
broken link reports. In particular:

- a format error or a multiple definition in an input is reported while
  that input is read, before anything about later inputs;
- `--rom-limit` is checked during layout, before any symbol is known to be
  undefined;
- an undefined entry symbol is reported before any undefined symbol a
  relocation refers to;
- undefined symbols are found only while relocating, so the error names
  the first undefined symbol referenced by a relocation, in link order. A
  strong undefined symbol that no relocation refers to is not an error.

## Input formats

### Objects

`parse_object` accepts a little-endian ELF relocatable object (`ET_REL`)
of these classes and machines:

| Class | `e_machine` | Targets |
|---|---|---|
| ELF64 | `EM_X86_64` (62) | x86-64 ELF targets, EmbLinkOS |
| ELF64 | `EM_RISCV` (243) | RV64 |
| ELF32 | `EM_ARM` (40) | ARMv7-M, ARMv8-M Mainline |
| ELF32 | `EM_RISCV` (243) | RV32 |
| ELF32 | `EM_MIPS` (8) | MIPS32, little-endian o32 only (checked from `EI_DATA` and the ABI bits of `e_flags`) |
| ELF32 | `EM_AVR` (83) | AVR |

From an object it reads the section headers, the section-name string
table, the first `SHT_SYMTAB` and its string table (`sh_info` gives the
number of local symbols), every `SHT_REL` and `SHT_RELA` section, and
`.ARM.attributes`. ELF32 section headers and symbols are copied field by
field into `Elf64_Shdr` and `Elf64_Sym`, because the ELF32 structures
order their members differently; section contents are read in place.

The following are not interpreted:

- **Section groups.** `SHT_GROUP` is not allocated and is dropped; every
  member of every group is linked. Duplicate COMDAT code is therefore
  kept, and duplicate weak definitions resolve to the last one read (see
  [Merging a definition](#merging-a-definition)).
- **Mergeable sections.** `SHF_MERGE` strings are not merged.
- **Non-allocated sections** other than `.debug_*` (`.comment`,
  `.note.*`, `.ARM.attributes`, `.symtab` and so on) are not copied to
  the output.

Shared objects, executables and linker scripts are not inputs. An
`ET_DYN` or `ET_EXEC` file fails the `ET_REL` check.

### ARM build attributes

`arm_attrs_scan` reads the `aeabi` vendor subsection of `.ARM.attributes`
and, inside it, only the file-scope (`Tag_File`) attributes. A string tag
(4, 5, 32, 65, 67) is skipped; every other tag's value is a ULEB128.
Three values are kept, each stored as the tag value plus one so that 0
means "not stated":

| Tag | Number | Stored in | Compared |
|---|---|---|---|
| `Tag_CPU_arch` | 6 | `arm_arch` | no |
| `Tag_ABI_enum_size` | 26 | `arm_enum` | yes |
| `Tag_ABI_VFP_args` | 28 | `arm_vfp` | yes |

`arm_attrs_check` runs once all objects are loaded. The first object that
states either compared tag is the reference; every later object that
states a tag the reference also states must agree with it. An object
with no attributes section takes no part. The two refusals are listed in
[Error messages](#error-messages).

### Archives

`parse_archive` reads the System V/GNU `ar` format. Each member has a
60-byte ASCII header (`name[16]`, `mtime[12]`, `uid[6]`, `gid[6]`,
`mode[8]`, `size[10]`, and the terminator `` `\n ``); members start at even
offsets.

| Header name | Treated as |
|---|---|
| `/` (followed by space or NUL) | the symbol index: skipped |
| `//` | the long-name string table: remembered |
| `/N` | a member whose name is at offset `N` in the long-name table, up to `/` or newline |
| `name/` | a member with a short name |

A member is recorded as `LIB.a(NAME)`, the form every diagnostic uses,
and is not parsed until `pull_archives` first inspects it. The symbol
index is never used; members are inspected directly. The BSD format
(`#1/N` names, `__.SYMDEF`) is not recognized: its first member is taken
as an object named `LIB.a(#1)` and fails to parse.

## Symbol resolution

### Merging a definition

`add_symbols` walks the object's global symbols (index `sh_info` onward)
and interns each name, so an undefined reference is recorded too. A
definition is merged by these rules, where the existing entry is the one
already in the table:

| Existing entry | Incoming symbol | Result |
|---|---|---|
| any | undefined (`SHN_UNDEF`) | nothing changes; the name is now known |
| none, undefined, or common | common (`SHN_COMMON`) | common; size is the largest seen, alignment (`st_value`) the largest seen |
| weak or strong definition | common | the definition stays |
| none or undefined | weak or strong definition | the incoming definition |
| common | strong definition | the incoming definition |
| common | weak definition | the common symbol stays |
| weak definition | strong definition | the incoming definition |
| weak definition | weak definition | the incoming definition: the last weak definition read wins |
| strong definition | weak definition | the strong one stays |
| strong definition | strong definition | error: `multiple definition` |

A definition records the object, the `insecs` index of its section (-1
for `SHN_ABS`), its section-relative value, its size and its `STT_*`
type. The size and type are copied to the output symbol table.

### Choosing archive members

A member is pulled when it defines, with a non-local binding, a name that
is in the table and not defined (`defines_needed`). A name defined only
weakly, or only as a common symbol, counts as defined: a strong
definition in an archive does not replace a weak or common definition
from an object, and the member is not pulled for it.

`pull_archives` is a fixed-point loop:

```text
repeat
    for each archive, in the order read
        for each member not yet pulled, in archive order
            parse it if not yet parsed
            if it defines a needed name: pull it (add_object)
until a full pass pulls nothing
```

It runs after each archive is read and once after the last input. A
consequence of running over every archive read so far is that archive
order does not matter, and mutually dependent archives need not be
repeated. What order does decide:

- When two members define the same needed name, the first in the scan
  order (archive order on the command line, then member order) is pulled,
  and the other is pulled only if it is needed for another name, in which
  case two strong definitions are an error.
- An object that follows an archive and defines a name a member already
  supplied is a multiple definition:

  ```text
  embld: multiple definition of 'helper' (in libh.a(t_helper.o) and t_helper.o)
  ```

Pulled members are added at the point they are pulled, so their sections
follow the sections of the inputs read before them.

### Resolving a reference

`reloc_symval` gives the value `S` for a relocation's symbol:

- a local symbol in an allocated section: the section's address plus
  `st_value`;
- a local `SHN_ABS` symbol: `st_value`;
- a local section symbol of a merged `.debug_*` section: the offset of
  that object's contribution in the merged section plus `st_value`;
- a local symbol in any other non-allocated section: an error;
- a defined global: its final value;
- an undefined weak global: 0, with no error. The relocation is then
  applied with `S = 0` like any other, so a PC-relative reference to it
  must be able to reach address 0: an x86-64 `PC32` or an RV64 `call`
  placed more than about 2 GB above it fails the range check (see
  [Relocations out of range](#relocations-out-of-range)). At RV32 a
  `call` reaches every address;
- an undefined strong global: an error, with a note when the name
  belongs to the compiler runtime or the unwinder.

`missing_runtime_note` chooses the note:

| Name | Note family |
|---|---|
| `_Unwind_*`, `__gxx_personality_v0`, `__register_frame_info`, `__deregister_frame_info`, `dl_iterate_phdr` | the stack unwinder |
| `__fix*`, `__float*`, `__trunc*`, `__extend*` | a floating-point conversion |
| `__*` ending in `ti3`, `ti2`, `di3`, `di2`, `si3`, `si2`, `sf2`, `df2`, `xf2`, `tf2`, `sc3`, `dc3`, `xc3`, `tc3`, `sf3`, `df3`, `xf3` or `tf3` | a compiler-runtime helper |

The texts are in [Error messages](#error-messages).

### Linker-defined symbols

`define_linker_symbol(name, value)` interns `name` and defines it as an
absolute symbol unless an input already defines it **in a section**
(`insec >= 0`). The list of names and values is in the user reference
([Linker-defined symbols](../manual/tools/embld.md#linker-defined-symbols)).
Two properties matter when changing this code:

- The names are interned whether or not anything refers to them, so every
  linker-defined symbol is defined in every link and appears in the
  output symbol table.
- An input's absolute (`SHN_ABS`) definition, and a common definition
  (which has `insec == -1` once placed), do not take precedence: they are
  overwritten. A weak definition in a section does take precedence.

## Sections and layout

### Output groups

Each allocated input section joins an output group, `enum osec`, in this
layout order:

| Group | Input sections | Segment |
|---|---|---|
| `OSEC_VECTORS` | `.vectors`, `.isr_vector`, and the RISC-V entry stub | text |
| `OSEC_TEXT` | `.text` | text |
| `OSEC_RODATA` | `.rodata` | text; data on AVR |
| `OSEC_TDATA` | any `SHF_TLS` section not of type `SHT_NOBITS` | data |
| `OSEC_TBSS` | any `SHF_TLS` section of type `SHT_NOBITS` | data |
| `OSEC_INIT_ARRAY` | `.init_array` | data |
| `OSEC_FINI_ARRAY` | `.fini_array` | data |
| `OSEC_CTORS` | `.ctors` | data |
| `OSEC_DTORS` | `.dtors` | data |
| `OSEC_DATA` | `.data` | data |
| `OSEC_BSS` | `.bss`, and any `SHT_NOBITS` section whose name matches no group | data |
| `OSEC_COUNT + i` | orphan `i` | text if read-only, data if writable |

`classify_osec` matches a name when it equals the group name or continues
it with a dot: `.text.main` joins `.text`, `.data.rel.ro` joins `.data`,
`.init_array.00100` joins `.init_array`; `.textual` does not join
`.text`. `SHF_TLS` is tested before the name. Sections within a group are
not sorted: a `.init_array.NNNNN` priority suffix has no effect on order.

A section that matches no group is an **orphan**. Each distinct orphan
name is its own group, numbered from `OSEC_COUNT` in first-seen order, at
most `MAX_ORPHANS` (64). Whether an orphan group is read-only or writable
is decided by the `SHF_WRITE` flag of the first section seen with that
name. `.eh_frame`, `.ARM.exidx`, `.gcc_except_table`, a RISC-V `.sdata`
and a `section("mytab")` table are all orphans.

### Segments

Every input section belongs to one of two segments:

- **text** (`PF_R | PF_X`): the vectors, text and read-only data groups
  and the read-only orphans;
- **data** (`PF_R | PF_W`): everything else.

The one exception is the Harvard rule: on AVR (`l->harvard`, set when the
first object is `EM_AVR`), `.rodata` is assigned to the data segment in
`collect_sections` and placed in it by `layout`. The two must agree; a
group placed in neither is silently dropped and a group placed in both
advances the location counter twice.

### Placing a group

`place_osec` walks `insecs` in index order (the order sections were
collected, which is the order objects were added) and places each section
of the group at the location counter aligned to the section's own
alignment. The group's start is the first member's aligned address, so
padding before the first member is not part of the group and the bracket
symbols do not include it. The group's `[start, end)` is recorded in
`bounds[]` for the bracket symbols.

### The layout

`layout` assigns addresses in this order:

```text
va = base                                  -Ttext, or 0x400000
text segment:   .vectors  .text  [.rodata]  read-only orphans
                                          (.rodata here unless Harvard)
if -Tdata:      data_lma = align(va, 4);  va = data_base
else:           va = align(va, 0x1000);    data_lma = va
data segment:   align(va, TLS alignment)
                .tdata                     tls_filesz ends here
                .tbss                      tls_memsz ends here
                va = end of .tdata         (.tbss takes no address space)
                [.rodata]                  (Harvard only)
                .init_array .fini_array .ctors .dtors .data
                writable orphans           data_filesz ends here
                --rom-limit check
                .bss, then each common symbol in symbol-table order
                                          data_memsz ends here
```

The thread-local template is first in the data segment and contiguous,
because each thread copies it as one block. `.tbss` is laid out to give
the template its size and its symbols their addresses, and then the
location counter returns to the end of `.tdata`: the sections after it
are addressed as if `.tbss` were not there, exactly as the file holds no
bytes for it. The aligned template size (`align(tls_memsz, tls_align)`)
is what `R_X86_64_TPOFF32` is measured from.

The `--rom-limit` check counts the bytes from `base` to the end of the
text segment, or, with `-Tdata`, to `data_lma + data_filesz` (the
initial data stored after the text). `.bss` is not stored and is not
counted.

### What the linker does not do

- **Garbage collection only when asked.** Without `--gc-sections` every
  allocated section of every object in the link reaches the image, and
  the unit of exclusion is the archive member. With it, see
  [Garbage collection](#garbage-collection).
- **No relaxation.** `R_RISCV_RELAX` and `R_RISCV_ALIGN` are skipped, and
  no instruction sequence is shortened. Alignment padding the assembler
  inserted stays as it is, which is correct because nothing moves.
- **No veneers, stubs or trampolines.** A branch that cannot reach its
  target is an error naming the limit.
- **No section sorting** by name, priority or alignment, except where a
  script's `SORT` asks for it.

## Garbage collection

`gc_sections` runs after every input is loaded and every archive member
pulled, and before layout -- in a script link, after `ls_claim`, because
that is what knows which sections a `KEEP()` claimed (`insec.keep`). It
is a mark from roots over a worklist:

1. Each object gets `relsec[]`: the REL/RELA section for each of its
   sections.
2. Roots are marked `live`: the entry symbol's section, `-u` and
   `EXTERN` symbols' sections, `keep` sections, the constructor and
   destructor arrays by name and by type, notes, `SHF_GNU_RETAIN`,
   `.eh_frame`, sections named by a referenced `__start_`/`__stop_`
   symbol, synthetic sections (the RISC-V entry stub), and without a
   script the vector table group.
3. A live section's relocations mark the section each symbol is in
   (`gc_target_insec`: a local symbol's own section, a global's
   definition). In `.eh_frame`, a relocation at an FDE's `pc_begin`
   (`eh_is_fde_pc`) is not followed.
4. When the worklist empties, a section with `SHF_LINK_ORDER` whose
   `sh_link` section is live is marked, and the loop continues.
5. Every section not live is `discarded` with `gc` set; in a script
   link it is taken out of every rule's list. Then each live
   `.eh_frame` has the FDEs of dropped functions neutralized
   (`eh_neutralize`): the CIE's FDE pointer encoding is read to find
   `pc_range`, which is zeroed, and `apply_relocs` skips their
   `pc_begin` relocation.

Everything downstream already skipped `discarded` sections for
`/DISCARD/`: layout (`place_osec`, `ls_layout_osec`), the copy into the
image, the symbol tables (`sym_in_image`), and `apply_relocs`, which
resolves a DWARF reference into one to 0. A relocation from a live
section into a collected one cannot happen by construction, and is an
internal error if it does.

The map file (`write_map`) and `--print-memory-usage` read the final
layout: for a script, each output section's `vma`, `lma` and claimed
inputs, and each region's `cur - origin`, which advances for load
addresses as well as run addresses; without one, the fixed groups'
bounds. An archive member records the symbol it was pulled for
(`pulled_for`) and the first object that referred to that symbol
(`symbol.ref`).

The symbol table is hashed (`sym_find`: FNV-1a, open addressing over
indices into `syms[]`). It was a linear scan, called per relocation and
per member symbol on every archive pass.

## Linker scripts

`src/link/ldscript.h`, included once by `link.c`, parses a script into a
tree (`struct ls_script`): the top-level commands in order, the output
sections (`struct ls_osec`) with their bodies, the memory regions and
their aliases, and the `INPUT`/`GROUP`/`EXTERN` names. A header rather
than a unit of its own, so that the hand-maintained source lists in
`Makefile`, `tools/os-build-embld.sh` and `build.ebm` did not change
beyond the header closure. The layout code is in `link.c`, under
"linker scripts: the layout":

1. `ls_claim`: each `LS_INPUT` description, in script order, claims the
   unclaimed allocated input sections it matches (`ls_file_match`,
   `ls_glob`), recording their `insecs` indices in its `list`, sorted if
   it says `SORT*`. `/DISCARD/` marks what it claims `discarded`.
2. `ls_orphans`: an unclaimed input gets an `orphan` output section of its
   own name, inserted after the last command whose section is of the same
   kind (`ls_class`: code, read-only, data, zero-initialised).
3. `ls_layout_pass`, repeated until `ls_signature` (every section's
   addresses and size, every script symbol, every input's address) stops
   changing, then once more with `final` set, which is the pass that
   reports region overflow and `ASSERT` failures. A forward reference reads
   the previous pass. Eight passes without settling is an error.
4. `finalize_symbols`, `__start_`/`__stop_` for C-identifier sections,
   relocation, and `write_exec_script`: one `PT_LOAD` per output section
   with `p_paddr` its load address (its run address if it is NOBITS,
   because a loader zeroes `p_memsz` there), one section header each.

An expression value carries whether it is relative to a section (derived
from `.` or from a symbol in a section). Inside an output section, `. =`
an absolute value is an offset into the section, as in ld. A script
symbol (`struct symbol.scripted`) remembers the output section it was
assigned in, which becomes its `st_shndx`; otherwise it is `SHN_ABS`. A
reference from kept code to a symbol in a discarded section is refused in
`apply_relocs`; DWARF that describes discarded code gets address 0.

## Per-target images

The table shows the options each in-tree caller passes. The driver passes
only what `-Wl,` and `-Xlinker` give it: see
[The driver's link](#the-drivers-link).

| Caller | Options | Image |
|---|---|---|
| `embcc FILE -o OUT` | none, unless given with `-Wl,` | x86-64 program at `0x400000` |
| `tools/gen-kernel-manifest.sh` | `-e _start -Ttext 0xFFFFFFFF80100000 --lma-offset 0xFFFFFFFF80000000` | the higher-half EmbLinkOS kernel |
| `tests/harness/thumb/link.sh`, `thumb-m4f/link.sh` | `-e reset -Ttext 0x0 -Tdata 0x20000000` | ARMv7-M firmware: flash at 0, SRAM at `0x20000000` |
| `tests/harness/thumb-m33/link.sh` | `-e reset -Ttext 0x10000000 -Tdata 0x10100000` | ARMv8-M firmware in the secure alias of the MPS2-AN505's SSRAM |
| `tests/harness/riscv/link.sh` | `-e _start -Ttext 0x80000000 -Tstack 0x80800000` | RISC-V image loaded into RAM on QEMU `virt` |
| `tests/harness/avr/link.sh` | `-e __vectors -Ttext 0x0 -Tdata 0x100 --rom-limit 32768` | ATmega328P firmware |

### x86-64 and EmbLinkOS

Without `-Ttext` the text segment starts at `0x400000`, and the data
segment at the next 4 KiB boundary after it. In the file each segment
starts at an offset congruent to its address modulo 4 KiB, so the
EmbLinkOS loader (or Linux) can map file pages directly; `p_align` is
`0x1000`.

When the image has a thread-local template, the first `PT_LOAD` starts at
file offset 0 so that it maps the ELF and program headers. A loader can
only pass a program `AT_PHDR` when a loadable segment covers the program
headers, and the runtime finds `PT_TLS` through it. An image without a
template keeps the first `PT_LOAD` at the text's own offset. A static
Linux program with a `__thread` variable, linked by the driver:

```text
  Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  LOAD           0x000000 0x00000000003ff000 0x00000000003ff000 0x012ff8 0x012ff8 R E 0x1000
  LOAD           0x013000 0x0000000000412000 0x0000000000412000 0x000594 0x004508 RW  0x1000
  TLS            0x013000 0x0000000000412000 0x0000000000412000 0x000004 0x000008 R   0x4
```

The kernel is linked at its virtual base with `--lma-offset`, which sets
`p_paddr = p_vaddr - OFFSET` for every program header, so a boot loader
that loads by physical address places it low while every symbol has its
higher-half address.

`--embx` writes the same two segments as an EMBX image instead; see
[Output formats](#output-formats).

### ARMv7-M and ARMv8-M firmware

A Cortex-M fetches its initial stack pointer from the first word of the
image and its reset address from the second, so `.vectors` (or CMSIS's
`.isr_vector`) is the first group of the text segment. The ARM harness's
`boot.c` defines the table in C; no assembly is needed.

`-Tdata` separates the data segment's two addresses:

- its **VMA**, `p_vaddr`, is the `-Tdata` address in SRAM, and every
  symbol in the data segment has an address there;
- its **LMA**, `p_paddr`, is `data_lma`, the end of the text segment
  aligned to 4, where the initial bytes are stored in flash. The same
  value is `__data_load`.

The startup code copies `__data_load` to `[__data_start, __data_end)` and
zeroes `[__bss_start, __bss_end)`. With `-Tdata`, segments are aligned to
4 bytes in the file instead of 4 KiB, because the image is copied into
flash and padding would be flash the part does not have. A small program
linked with the ARMv7-M harness options:

```text
  Type           Offset   VirtAddr   PhysAddr   FileSiz MemSiz  Flg Align
  LOAD           0x000074 0x00000000 0x00000000 0x00276 0x00276 R E 0x4
  LOAD           0x0002ec 0x20000000 0x00000278 0x00004 0x0000c RW  0x4
```

A Thumb function symbol's value has bit 0 set (the compiler and assembler
put it in `st_value`), and the linker neither adds nor removes it: an
`R_ARM_ABS32` vector-table entry and the ELF `e_entry` both carry it. The
output `e_flags` is `EF_ARM_EABI_VER5` (`0x05000000`).

### RISC-V

The `virt` harness loads the whole image into RAM at `0x80000000`, so
there is no flash-to-RAM copy and no `-Tdata`; the data segment follows
the text at the next 4 KiB boundary and `__data_load` equals
`__data_start`, which makes the startup's copy loop move nothing.

RISC-V starts with every register zero, including `sp`. `-Tstack ADDR`
makes the linker prepend an entry stub:

- `add_entry_stub` (before layout) adds a synthetic section `.start` to
  the `OSEC_VECTORS` group, `rv_li_len(ADDR) + 8` bytes long and 4-byte
  aligned. It has no object, so `apply_relocs` never sees it. It is
  added after every input, so an input `.vectors` section would precede
  it.
- `fill_entry_stub` (after symbols are final) writes `rv_li sp, ADDR`,
  then `auipc t0, hi20(d)` and `jalr zero, lo12(d)(t0)`, where `d` is the
  distance from the `auipc` to the entry symbol. `rv_li` builds the
  constant correctly at RV64, where `lui` sign-extends bit 31.
- `e_entry` is the stub's address instead of the entry symbol's.

The stub is 12 bytes at RV32 and 20 at RV64 for the harness's
`0x80800000`:

```text
RV32                                      RV64
80000000: 80800137  lui   sp, 0x80800     80000000: 00081137  lui   sp, 0x81
80000004: 00000297  auipc t0, 0x0         80000004: 80010113  addi  sp, sp, -0x800
80000008: 00c28067  jr    0xc(t0)         80000008: 00c11113  slli  sp, sp, 0xc
                                          8000000c: 00000297  auipc t0, 0x0
                                          80000010: 01428067  jr    0x14(t0)
```

The output `e_flags` is the OR of the inputs' `EF_RISCV_RVC` bits and
nothing else, so the float-ABI field is 0 (soft float).

### MIPS32

The `malta` harness loads the whole image into RAM at `0x80100000`
(KSEG0), so as on RISC-V there is no `-Tdata` and the copy loop moves
nothing.

`-Tstack ADDR` adds the same `.start` section, `mips_li_len(ADDR) + 16`
bytes long, which `fill_entry_stub` fills with `li sp, ADDR`, then `lui
t9, hi(entry)`, `ori t9, t9, lo(entry)`, `jr t9` and the delay slot's
`nop`. The jump is absolute, so it reaches the entry from anywhere; the
stub is 20 bytes for the harness's `0x80800000`.

The layout keeps the data segment's stored size and the end of `.bss` on
word boundaries on MIPS, so `__data_end`, `__bss_start` and `__bss_end`
are multiples of 4: a startup's word loops store with `sw`, which traps
at a misaligned address. (A `.data` 14 bytes long once sent the
harness's `.bss` loop to the boot ROM's exception vector.)

Each input's `.MIPS.abiflags` (`mips_abiflags_check`) must name the same
floating-point ABI as the first input's, so soft-float and FPU objects
are never linked together; `.MIPS.abiflags` and `.reginfo` are then
dropped, and the output `e_flags` keeps the first input's architecture
and ABI bits.

### AVR

The ATmega328P has separate program and data address spaces: `ld` and
`lds` reach only data space, so data left in flash cannot be read by the
code EmbCC emits for `*p`. The linker therefore places `.rodata` in the
data segment, after the thread-local template and before `.init_array`
and `.data`: string literals get RAM addresses and flash load addresses,
and the startup copies them with `.data`.

Addresses in the ELF file are byte addresses in both spaces. The text
segment starts at 0 (flash) and the data segment's VMA is the `-Tdata`
address itself (`0x100`, the first SRAM byte above the register file and
I/O space); no `0x800000` data-space offset is applied. Instructions that
reach program space use word addresses, and the relocations that need
one halve the byte address (see [AVR relocations](#avr-relocations)).

The entry is `__vectors`, not `reset`, because QEMU requires an AVR
image's entry point to be 0 and the processor starts there. A small
program with the harness options:

```text
  Type           Offset   VirtAddr   PhysAddr   FileSiz MemSiz  Flg Align
  LOAD           0x000074 0x00000000 0x00000000 0x000ac 0x000ac R E 0x4
  LOAD           0x000120 0x00000100 0x000000ac 0x0000a 0x0000c RW  0x4
```

The output `e_flags` carries the architecture (`EF_AVR_ARCH_MASK` bits)
of the first object that names one.

### AArch64, Mach-O and COFF

EmbLD does not link AArch64. An AArch64 object is refused by
`parse_object`:

```text
embld: a64.o: a 64-bit object for machine 183; only x86-64 and RV64 (EM_RISCV) are supported
```

The AArch64 QEMU harness (`tests/harness/aarch64/link.sh`) links with
`aarch64-elf-ld` and the script `tests/harness/aarch64/link.ld`. Mach-O
and COFF objects are not ELF and are refused as `not an ELF file`.

## Relocation processing

### The loop

`apply_relocs` visits every `SHT_REL` and `SHT_RELA` section of an
object. The section it relocates is `sh_info`. It is skipped when that
section was neither collected nor merged as debug information, and when
it is `SHT_NOBITS`.

For each entry it computes:

- `r_offset`, the symbol index and the type. ELF32 packs `r_info` as
  24 bits of symbol and 8 of type; ELF64 as 32 and 32.
- `A`, the addend: `r_addend` for `SHT_RELA`. For an ELF32 `SHT_REL`
  section the addend is 0, except on ARM, where it is read back out of
  the field (below).
- `S`, from `reloc_symval`.
- `P`, the place: the target section's address plus `r_offset`. In a
  debug section, which has no address, `P` is 0, so only absolute
  relocations are meaningful there.

The field is patched in the object's own buffer (or in the merged debug
buffer), and the patched bytes are copied into the image when it is
written. `R_RISCV_RELAX` and `R_RISCV_ALIGN` are recognized and skipped
before the symbol is looked up, because their symbol index is 0.

In the tables below, `V` is `S + A`.

### x86-64 relocations

| Type | Value written | Width | Range checked |
|---|---|---|---|
| `R_X86_64_64` | `V` | 8 | no |
| `R_X86_64_32` | `V` | 4 | 0 to 2^32 - 1 |
| `R_X86_64_32S` | `V` | 4 | -2^31 to 2^31 - 1 |
| `R_X86_64_PC32`, `R_X86_64_PLT32` | `V - P` | 4 | -2^31 to 2^31 - 1 |
| `R_X86_64_PC64` | `V - P` | 8 | no |
| `R_X86_64_TPOFF32` | `V - tls_start - align(tls_memsz, tls_align)` | 4 | -2^31 to 2^31 - 1 |

`R_X86_64_PLT32` is a direct PC-relative reference: no PLT slot is
created. `R_X86_64_TPOFF32` is the local-exec thread-local model. x86-64
places the thread block below the thread pointer, so the offset is
negative; the runtime (`lib/libc/os/linux/tls.c`) must round the block
to the same alignment. A 4-byte value outside its range is refused by
`need_range` (see [Relocations out of range](#relocations-out-of-range)),
not truncated. `R_X86_64_GOTPCREL`, `R_X86_64_GOTPCRELX` and
`R_X86_64_REX_GOTPCRELX` are defined in `src/elf/elf.h` but not applied;
an object that uses them fails with the unsupported-type error.

### ARM relocations

| Type | Value written | Field |
|---|---|---|
| `R_ARM_NONE` | nothing: a dependency (`.ARM.exidx` on its personality routine) | none |
| `R_ARM_ABS32`, `R_ARM_TARGET1` | `V` (the Thumb bit comes with `S`) | word |
| `R_ARM_REL32` | `V - P` | word |
| `R_ARM_PREL31` | `(V - P) & 0x7fffffff` | word, bit 31 kept |
| `R_ARM_THM_CALL`, `R_ARM_THM_JUMP24` | `(S & ~1) + A - (P + 4)` | `bl`/`b.w`, ±16 MiB |
| `R_ARM_THM_MOVW_ABS_NC` | `V & 0xffff` | `movw` immediate |
| `R_ARM_THM_MOVT_ABS` | `V >> 16` | `movt` immediate |

A 32-bit Thumb instruction is two little-endian halfwords, not a
little-endian word; every ARM patcher reads and writes halfwords.

- **`bl`/`b.w`** (`patch_thm_b24`). The displacement is halved and split
  as `S:I1:I2:imm10:imm11`, with `I1` and `I2` stored as
  `J1 = ~(I1 ^ S)` and `J2 = ~(I2 ^ S)`. Bits 15, 14 and 12 of the
  second halfword, which distinguish `bl` from `b.w`, are kept. The
  Thumb bit is removed from `S` first, because the displacement is
  between addresses.
- **`movw`/`movt`** (`patch_thm_mov`). The 16-bit immediate is split as
  `imm4` (first halfword bits 3:0), `i` (bit 10), `imm3` (second
  halfword bits 14:12) and `imm8` (bits 7:0).
- **`PREL31`** is the self-relative pointer in `.ARM.exidx`. Bit 31 of
  the existing word is kept because it says whether the entry is inline
  unwind data.

EmbCC writes `SHT_RELA` for ARM. Objects from other ARM toolchains (clang,
`llvm-mc`) use `SHT_REL`, where the addend is the current content of the
field. `apply_relocs` reads it back in the shape
of the field:

| Type | Addend |
|---|---|
| `R_ARM_ABS32`, `R_ARM_TARGET1`, `R_ARM_REL32` | the word, as a signed 32-bit value |
| `R_ARM_PREL31` | bits 30:0 of the word, sign-extended |
| `R_ARM_THM_CALL`, `R_ARM_THM_JUMP24` | the decoded displacement plus 4 (`read_thm_b24`) |
| `R_ARM_THM_MOVW_ABS_NC`, `R_ARM_THM_MOVT_ABS` | the 16-bit immediate, sign-extended (`read_thm_mov`) |
| any other | 0 |

The `+ 4` is because the field encodes a displacement from `P + 4` while
the ABI's addend is measured from `P`: an assembler with no target writes
`bl .` (`f7ff fffe`, displacement -4) to mean an addend of 0.

### RISC-V relocations

`apply_riscv` handles both RV32 and RV64. Two helpers split a value into
the halves of a `lui`/`auipc` pair:

```c
hi20(v) = ((v + 0x800) >> 12) & 0xfffff
lo12(v) = ((v & 0xfff) ^ 0x800) - 0x800     /* sign-extended */
```

The `+ 0x800` is required because the low half is sign-extended: when
bit 11 of `v` is set, the low half contributes a negative value and the
high half must be one larger.

| Type | Value written | Field | Range checked |
|---|---|---|---|
| `R_RISCV_32` | `V` | word | -2^31 to 2^32 - 1 |
| `R_RISCV_64` | `V` | doubleword | no |
| `R_RISCV_HI20` | `hi20(V)` | U-type immediate | RV64: -2^31 - 0x800 to 2^31 - 1 - 0x800; RV32: no |
| `R_RISCV_LO12_I` | `lo12(V)` | I-type immediate | no |
| `R_RISCV_LO12_S` | `lo12(V)` | S-type immediate (bits 11:5 at 31:25, 4:0 at 11:7) | no |
| `R_RISCV_PCREL_HI20` | `hi20(V - P)`; `V - P` is recorded under `P` | U-type immediate | `V - P` as `HI20`'s `V`, at RV64 only |
| `R_RISCV_PCREL_LO12_I`, `_S` | `lo12(d)`, where `d` is the value recorded for the `auipc` at `V` | I- or S-type immediate | no |
| `R_RISCV_BRANCH` | `V - P`, re-encoded by `rv_enc_b` | B-type | -4096 to 4094 |
| `R_RISCV_JAL` | `V - P`, re-encoded by `rv_enc_j` | J-type | -2^20 to 2^20 - 2 |
| `R_RISCV_CALL`, `R_RISCV_CALL_PLT` | `hi20(V - P)` at `P`, `lo12(V - P)` at `P + 4` | `auipc` + `jalr` | `V - P` as `HI20`'s `V`, at RV64 only |
| `R_RISCV_RELAX`, `R_RISCV_ALIGN` | nothing | | |

The upper-half ranges are those a `lui` or `auipc` pair can produce at
RV64, where the 20-bit immediate is sign-extended to 64 bits; the
`+ 0x800` rounding shifts the reachable range down by 2 KiB. At RV32 the
pair wraps with the 32-bit address space, so every value is in reach and
none is checked; in particular, a `call` at RV32 reaches any address.
The low halves need no check: they are the low 12 bits of the value
whose high half the paired relocation carries.

Only the immediate bits are rewritten; the opcode and registers stay as
the compiler wrote them.

**The `PCREL` pair.** A `PCREL_LO12` relocation does not name the target:
its `S + A` is the address of the `auipc` that computed the high half
(EmbCC emits it against the function's section symbol with the `auipc`'s
offset as addend). The low half must use the low 12 bits of the same
displacement the high half saw, not of the target address, or the two
disagree about the rounding. So `PCREL_HI20` records `(P, V - P)` with
`note_pcrel_hi`, and `PCREL_LO12_*` looks up its `V` with
`find_pcrel_hi`, searching newest first. The high half must therefore be
relocated before its low half: in the same object, earlier in the
relocation order. EmbCC lists each pair high half first.

**Branches and jumps** are re-encoded by the compiler's own B-type and
J-type encoders. `apply_riscv` checks the displacement against the
field first, so one out of range is refused by the linker, naming the
relocation and the symbol, and never reaches the encoder's own check.
EmbCC's own branches are resolved inside the function and never reach
the linker; these relocations come from assembler or clang objects.

**Not handled:** the compressed-instruction relocations
(`R_RISCV_RVC_BRANCH`, `R_RISCV_RVC_JUMP`), the `ADD`/`SUB`/`SET` family,
`R_RISCV_32_PCREL`, and the GOT and TLS relocations. With linker
relaxation enabled (its default), clang expresses distances inside
`.debug_*` and `.eh_frame` with the `ADD`/`SUB`/`SET` family, so, for
example, a clang object built with
`-g -fasynchronous-unwind-tables` and without `-mno-relax` fails:

```text
embld: c_main.o: unsupported RISC-V relocation type 35 (this is the next linker increment, not a bug in your program)
```

### MIPS relocations

`apply_mips` reads both REL and RELA sections (`struct mips_relctx`
says which and carries the section's entries). In a REL section the
addend is the field's own bits, read per type as the o32 ABI defines:

| Type | Addend from the field (REL) | Value written |
|---|---|---|
| `R_MIPS_NONE`, `R_MIPS_JALR` | | nothing |
| `R_MIPS_32` | the word | `V`, range-checked to 32 bits |
| `R_MIPS_HI16` | AHL: `(field << 16)` plus the sign-extended field of the next `R_MIPS_LO16` against the same symbol (`mips_lo_of`) | `(V + 0x8000) >> 16` |
| `R_MIPS_LO16` | the sign-extended field | `V & 0xffff` |
| `R_MIPS_26` | `field << 2`, sign-extended from 28 bits for an external symbol | `V >> 2` in the low 26 bits; `V` must be a multiple of 4 and in the 256 MiB region of `P + 4` |
| `R_MIPS_PC16` | `field << 2`, sign-extended | `(V - P) >> 2`, -131072..131068 |

The `+ 0x8000` in the HI16 is the same rounding as RISC-V's `+ 0x800`:
the `addiu` that adds the low half sign-extends it. Several HI16s may
share one LO16; a HI16 with none after it is refused, because its
addend cannot be known. `R_MIPS_GPREL16`, `R_MIPS_GPREL32`,
`R_MIPS_LITERAL`, `R_MIPS_GOT16` and `R_MIPS_CALL16` are refused by name
(small data and PIC), and anything else as an unsupported MIPS type.

EmbCC's own objects use `R_MIPS_32`, `R_MIPS_26` (every call, and a long
branch's `j` against its function's section) and the HI16/LO16 pair.
`tests/golden/mips-link.sh` links llvm-mc objects whose addends straddle
every carry of the AHL rule.

### AVR relocations

`apply_avr` patches every split field through the AVR encoder's patchers
in `src/arch/avr/emit.c`.

| Type | Value written | Field |
|---|---|---|
| `R_AVR_NONE` | nothing | |
| `R_AVR_32` | `V` | 4 bytes |
| `R_AVR_16` | `V & 0xffff` | 2 bytes: a data-space pointer |
| `R_AVR_16_PM` | `(V >> 1) & 0xffff` | 2 bytes: a function pointer (word address) |
| `R_AVR_LO8_LDI` | `V & 0xff` | `ldi` K (`avr_patch_ldi_at`) |
| `R_AVR_HI8_LDI` | `(V >> 8) & 0xff` | `ldi` K |
| `R_AVR_LO8_LDI_GS` | `(V >> 1) & 0xff` | `ldi` K: low byte of a word address |
| `R_AVR_HI8_LDI_GS` | `(V >> 9) & 0xff` | `ldi` K: high byte of a word address |
| `R_AVR_CALL` | `V >> 1` | 22-bit word address of `call`/`jmp` (`avr_patch_call_at`); odd `V` is an error |
| `R_AVR_13_PCREL` | `(V - P - 2) / 2` | `rjmp`/`rcall`, -2048..2047 words (`avr_patch_rjmp_at`) |
| `R_AVR_7_PCREL` | `(V - P - 2) / 2` | conditional branch, -64..63 words (`avr_patch_br_at`) |

The `_GS` and `_PM` forms halve the byte address; the plain forms do not.
Using the wrong one produces a pointer twice as far into flash, which
lands on a real instruction and does not fault. The `ldi` immediate is
split around the register field (bits 11:8 and 3:0). PC-relative
displacements are in words from the instruction after the branch, which
is the `- 2`. The `_GS` forms take the word address modulo 2^16 and
create no stub, which is sufficient for parts with at most 128 KiB of
flash.

### Unsupported types

Any type not listed above stops the link. The message names the machine
for ARM, RISC-V, MIPS and AVR, and omits it for x86-64:

```text
embld: FILE: unsupported relocation type N (this is the next linker increment, not a bug in your program)
embld: FILE: unsupported ARM relocation type N (this is the next linker increment, not a bug in your program)
embld: FILE: unsupported RISC-V relocation type N (this is the next linker increment, not a bug in your program)
embld: FILE: unsupported AVR relocation type N (this is the next linker increment, not a bug in your program)
embld: FILE: unsupported MIPS relocation type N (this is the next linker increment, not a bug in your program)
```

## Debug information

`l.keep_debug` is always set. In `collect_sections`, each non-allocated
`SHT_PROGBITS` section whose name starts with `.debug_` is appended to
the merged section of that name (`dbgsec_for`, first-seen order), and the
object records where its piece landed (`dbg_sec`, `dbg_off`). Archive
members' debug sections are merged too.

No DWARF-aware processing is needed, because EmbCC's DWARF writer
expresses every cross-reference as a relocation: a unit's
`DW_AT_stmt_list` and abbreviation offset are absolute relocations
against the `.debug_line` and `.debug_abbrev` section symbols, and its
`low_pc` against `.text`. `apply_relocs` patches those relocations inside
the merged buffer, and `reloc_symval` resolves a debug section symbol to
the object's offset within the merged section, which rebases every
reference.

After an ELF image is written, `emit_embdbg` writes the native
`OUT.embdbg` sidecar through `embdbg_emit_objects`. It takes every object
named on the command line (an object whose name contains `(` is an archive
member and is skipped) that has a `.debug_line` section, biased by the
final address of that object's `.text` section, and reads the written
image back for its build ID. With `--embx` no sidecar is written. The
format belongs to [embdbg](../manual/tools/embdbg.md).

## Output formats

### ELF executable

`write_exec` builds the headers as `Elf64_*` structures and narrows them
to ELF32 when the inputs were ELF32. The file is laid out as:

```text
ELF header
program headers         PT_LOAD text, PT_LOAD data, [PT_TLS]
padding                 text offset ≡ text address (mod page)
text segment bytes
padding                 data offset ≡ data address (mod page)
data segment bytes      file-backed part only; .bss has none
merged .debug_* sections   (8-byte aligned start)
.symtab  .strtab  .shstrtab
section headers         (8-byte aligned)
```

The page is `0x1000`, or 4 when `-Tdata` is given.

| Field | Value |
|---|---|
| `e_type` | `ET_EXEC` |
| `e_machine` | the inputs' machine |
| `e_entry` | the entry stub's address, or the entry symbol's value (with the Thumb bit on ARM) |
| `e_flags` | ARM: `EF_ARM_EABI_VER5`; RISC-V: the OR of the inputs' `EF_RISCV_RVC`; AVR: the first input's architecture; x86-64: 0 |
| `PT_LOAD` text | `PF_R \| PF_X`; `p_paddr = p_vaddr - lma_offset`; starts at file offset 0 when there is a `PT_TLS` |
| `PT_LOAD` data | `PF_R \| PF_W`; `p_paddr` is `data_lma` with `-Tdata`, else `p_vaddr - lma_offset`; `p_memsz - p_filesz` is the `.bss` tail |
| `PT_TLS` | the template inside the data segment; `p_memsz - p_filesz` is `.tbss`; `p_align` is the largest TLS input alignment |

The section headers describe the image, not the inputs: one `.text`
covering the whole text segment, one `.data` covering the file part of
the data segment (omitted when it is empty), the merged `.debug_*`
sections, `.symtab`, `.strtab` and `.shstrtab`. `.text` and `.data` have
`sh_addralign` 4.

The symbol table holds the null entry, then the inputs' local functions
and objects (`STB_LOCAL`, `STT_FUNC` or `STT_OBJECT`, with a name, in a
section that was laid out), then every defined global with `STB_GLOBAL`
binding (a weak definition is written as global). `sh_info` is one past
the last local, as ELF requires. A local keeps its input type and size,
and its value is where its section landed, as a relocation against it
would resolve; section symbols, file symbols and unnamed labels are not
copied. A global's type is the input's `STT_*` type; a symbol with none
is `STT_FUNC` if it lies in the text segment and `STT_OBJECT`
otherwise. The section index is `.text` for a symbol in the text segment
and `.data` (or `.text`, when there is no `.data`) for everything else,
including absolute and linker-defined symbols. The locals are what let
a debugger or a profiler name a static function in an image. None of
this lies inside a `PT_LOAD`, so it costs file size and no target
memory.

The file is written with `plat_write_file`.

### EMBX

`emit_embx` writes the same two segments in EmbLinkOS's EMBX container
([D-003](decisions.md#d-003)). Its byte layout matches
`tools/embx/mkembx.py`:

```text
header (128 bytes)
segment descriptors (2 x 64 bytes): text R+X, data R+W
capability table (16 bytes per capability, ascending ID)
segment payloads, each at a file offset ≡ its address (mod 4 KiB)
```

Each segment descriptor carries a CRC32C of its payload and `align`
`0x1000`. The build ID is the SHA-256 of the whole image with the build
ID and header checksum still zero; the header checksum (CRC32C over the
header body) is computed last. The header's `machine` is always
`EMBX_MACHINE_X86_64`, whatever the inputs' machine. Each segment's
`paddr` is 0, so neither `--lma-offset` nor the `-Tdata` load address is
represented, and there is no `PT_TLS` equivalent. The format is verified
by [embread](../manual/tools/embread.md).

## The driver's link

`embcc [FILE] [OBJECTS...] -o OUT` links in-process through
`compile_and_link` in `src/driver/main.c`. The driver links x86-64 ELF programs and, for the firmware targets
(ARMv7-M, ARMv8-M, RV32, RV64, MIPS32, AVR), images whose memory map the build
gives: a linker script (`-T`, ARM and RISC-V) or `-Wl,-Ttext`/`-Tdata`.
A firmware link without one stops with `embcc: error: linking a TRIPLE
image needs its memory map`. Every other target (AArch64 ELF, Mach-O,
COFF) stops with `embcc: error: cannot link for TRIPLE`, because embld
does not read those objects.

The driver compiles to `OUT.embcc-tmp.o` beside the output and calls
`embld_link` with a `struct link_opts` that is zero except for what
`apply_wl` sets from the `-Wl,` and `-Xlinker` words: `-e`/`--entry`,
`-Ttext` (and `-Ttext-segment`), `-Tdata`, `-Tstack`, `--rom-limit`,
`--lma-offset`, `-T`, `-L`, `-u`, `--orphan-handling`, `--gc-sections`,
`--print-gc-sections`, `-Map` and `--print-memory-usage`. Without them the entry is `_start` and the text starts
at `0x400000`. `apply_wl` accepts the options that change nothing about
an image EmbLD makes (`-s`, `--build-id`, `-z now` and the others
listed in
[Invoking EmbCC](../manual/invoking.md#-wlargs--xlinker-arg)) and refuses
any other (`embcc: error: linker option 'OPT' is not one EmbLD has ...`);
the driver then removes the temporary object and links nothing.

The inputs are, in order: `crt1.o` (if the target has one), the
temporary object, `libcxx.a` (C++ only), `libc.a` (if the target has
one) and `librt.a` (if the target has one). Because of the fixed-point
pull, the order of the archives in this list does not change which
symbols resolve; it changes only the order of the pulled members'
sections.

A link error ends the process from inside `embld_link`. The message
names the temporary object (`referenced by OUT.embcc-tmp.o`), and the
temporary object is not removed.

## `--doctor`

`doctor_run` in `tools/embld/doctor.c` does not call the linker. It
builds two tables from the inputs' symbol tables and reports every name
in the second that the first does not define publicly. Its user-facing
description is in the manual
([`--doctor`](../manual/tools/embld.md#--doctor) and
[diagnostics](../manual/diagnostics.md#embld-doctor), section T6).

### Scanning

- `scan_object` reads an object's `SHT_SYMTAB`. A named `SHN_UNDEF`
  symbol with a non-local binding goes into `refs`, recorded with the
  first input that refers to it. A named symbol in any section (local,
  weak or global) goes into `defs`, recorded with the first input that
  defines it and whether that definition is local or weak. Only the first
  definition of a name is kept.
- `scan_archive` scans every member whose header name does not start
  with `/` and is not `__.SYMDEF`, labelled `LIB.a(NAME)`. Every member is
  scanned, not only those a link would pull.

### Classifying

A reference is reported unless `defs` holds a non-local definition of the
name. The causes are tried in this order, and the first that applies is
printed:

1. a local definition exists: the name is `static` in another unit;
2. `header_declaring(name)` finds it in the C-library table in
   `src/driver/explain.c`;
3. the name starts with `__cxa_`, `_Unwind_` or `_ZSt`, or contains
   `__cxxabiv1`: the C++ runtime;
4. the name starts with `_ZTV` or `_ZTI`: the key-function rule;
5. the name demangles: a member function declared and never defined;
6. otherwise: nothing on the command line defines it.

Rule 3 precedes rule 4 because libsupc++'s own vtables, such as
`_ZTVN10__cxxabiv117__class_type_infoE`, start with `_ZTV`, and the
key-function explanation is wrong for them.

The demangler (`demangle`) reads `_Z` names made of length-prefixed
source names, optionally nested in `N...E` with `K`, `V` and `r`
qualifiers, and the `TV`, `TI` and `TS` special names. A parameter list
is printed as `()` for `v` and as `(...)` otherwise; `const` and
`volatile` from the nested-name qualifiers are appended. A name it cannot
read gets no `that is` line.

### Output

Each undefined name prints, on standard error:

```text
embld: undefined: NAME
  that is DEMANGLED
  wanted by FILE
```

The `that is` line appears only for a name the demangler reads. One of
these explanations follows, exactly as written:

```text
  FILE does define it — but as `static`, which keeps it inside that
  unit. Drop the `static`, or move the caller into that file.

  it is the C library's NAME (declared in HEADER): link the library that
  defines it (libc.a), which a freestanding link does not add for you.

  it belongs to the C++ runtime: link libsupc++ (and libstdc++ for the
  library itself). A freestanding link adds neither for you.

  a class's vtable and typeinfo are emitted with its KEY FUNCTION —
  the first virtual function that is not inline. If every virtual
  function of that class is inline or pure, no unit emits them; if the
  key function is merely declared, define it.

  nothing here defines it. A member function that is declared in the
  class and never defined looks exactly like this.

  nothing on this command line defines it — is a source file missing
  from the link, or a library?
```

The run ends with `embld: doctor: N symbol(s) undefined` on standard
error and status 1, or with
`embld: doctor: every symbol referenced is defined (N definitions across M input(s))`
on standard output and status 0. An unreadable input prints
`embld: doctor: cannot read FILE` and is skipped.

### Limits

These follow from the code and differ from what a link does:

- Only ELF64 objects are read (`e_shentsize` must be
  `sizeof(Elf64_Shdr)`); an ELF32 object is skipped silently.
- An archive member stored under a long name (`/N`) is skipped.
- An undefined weak reference is reported, although a link binds it to 0.
- A linker-defined symbol (such as `__init_array_start`) is reported as
  undefined, because the doctor does not know them.
- Because only the first definition of a name is kept, a `static`
  definition read before a public one makes the name be reported with
  the `static` explanation.

## Error messages

Every linker error is printed as `embld: ` followed by the text below,
and ends the process with status 1. `FILE` is an input path or
`LIB.a(MEMBER)`.

### Reading inputs

| Message | Raised in |
|---|---|
| `cannot read 'FILE'` | `read_file` |
| `FILE: too small to be an object` | `parse_object` |
| `FILE: not an ELF file` | `parse_object` |
| `FILE: not little-endian` | `parse_object` |
| `FILE: not a 32- or 64-bit ELF` | `parse_object` |
| `FILE: not a relocatable object (ET_REL)` | `parse_object` |
| `FILE: a 32-bit object for machine N; only ARM (EM_ARM), RV32 (EM_RISCV) and AVR (EM_AVR) are supported` | `parse_object` |
| `FILE: a 64-bit object for machine N; only x86-64 and RV64 (EM_RISCV) are supported` | `parse_object` |
| `FILE: section headers run past end of file` | `parse_object` |
| `FILE: corrupt archive header at offset N` | `parse_archive` |
| `FILE: an object for a different machine than the ones before it (N against M)` | `add_object` |
| `more than 64 distinct orphan sections (at 'NAME')` | `orphan_osec` |

### Symbols

```text
embld: multiple definition of 'NAME' (in FILE1 and FILE2)
embld: entry symbol 'NAME' is undefined
embld: undefined symbol 'NAME' (referenced by FILE)
embld: FILE: local symbol 'NAME' is in section N (SECTION), which is not allocated and so has no address to relocate against
```

`FILE1` is the object that defined the name first. The undefined-symbol
error is raised by `reloc_symval` for the first relocation that refers to
an undefined strong symbol. For a runtime name it is followed by a line
`  note: ` and one of these texts:

```text
this is the stack unwinder, which C++ exceptions need. EmbCC has one (lib/rt/unwind.c, in librt.a), so this usually means librt.a is not on the link line -- the driver puts it there by itself, and a hand-written link has to name it after libc.a

this is a compiler-runtime conversion between a floating-point type and an integer one, or between two floating-point widths — including every aarch64 `long double` operation, which is IEEE binary128 in software there. EmbCC ships these in librt.a (lib/rt); a link that reaches this note is usually one that left it out

this is a compiler-runtime helper (libgcc's __muldi3 family) — the routine a backend calls for an operation the machine has no instruction for, such as 128-bit multiply or divide. EmbCC ships these in librt.a (lib/rt); the driver puts it on the link line by itself, and a hand-written link has to name it after libc.a
```

For example:

```text
embld: undefined symbol '__divti3' (referenced by x_rt.o)
  note: this is a compiler-runtime helper (libgcc's __muldi3 family) — ...
```

### Layout and memory regions

```text
embld: the image needs N bytes of flash and the part has M (--rom-limit): T of text, D of initial data
embld: -Tstack is a RISC-V option: every other target here starts with a stack pointer already set (a Cortex-M reads its own from the vector table)
embld: the entry symbol is more than 2GB from the image base
```

`D` is 0 without `-Tdata`. The flash limit is the only memory region
checked: nothing checks the data segment against the size of RAM, or the
text segment against the address space.

### ARM build attributes

```text
embld: 'A' and 'B' disagree about where floating-point arguments go: one passes them in the core registers (-mfloat-abi=soft) and the other in s0-s15 (-mfloat-abi=hard). Linking them would leave every float argument read from a register the caller never wrote
embld: 'A' and 'B' disagree about the size of an enum, which changes the layout of every struct that holds one
```

`A` is the reference object (the first that stated a compared tag).

### Relocations out of range

The x86-64 4-byte forms and the RISC-V forms marked as checked in the
tables above are refused by `need_range` with one message:

```text
embld: FILE: R_TYPE against 'SYM' needs V (0xHEX), and the field holds LO to HI; the image is laid out beyond what this code can reach
```

`SYM` is the symbol the relocation names, or the section's name for a
section symbol. `V` is the value the field would have to hold (for a
PC-relative form, the displacement), printed in decimal and then in
hexadecimal as a 64-bit pattern. `R_RISCV_CALL_PLT` is reported as `R_RISCV_CALL`. For
example, data placed 12 GB from RIP-relative code:

```text
embld: far.o: R_X86_64_PC32 against 'g' needs 12880707577 (0x2ffbffff9), and the field holds -2147483648 to 2147483647; the image is laid out beyond what this code can reach
```

The other range errors have messages of their own:

| Message | Limit |
|---|---|
| `FILE: a Thumb call is more than 16MB away; this linker mints no veneers` | `bl`/`b.w`: -2^24 to 2^24 - 1 bytes |
| `` FILE: an rjmp reaches +-4KB and this target is N bytes away; this linker mints no trampolines, so the call has to be a `call` rather than an `rcall` `` | `rjmp`/`rcall`: -2048 to 2047 words |
| `FILE: a conditional branch reaches +-126 bytes and this target is N away; it has to be an inverted branch over an rjmp` | AVR branch: -64 to 63 words |
| `FILE: a call to an odd address 0xADDR; AVR instructions are halfword-aligned and the address is halved to a word number, so an odd one cannot be encoded` | AVR `call`/`jmp` |

The ARM `movw`/`movt`, `ABS32`, `REL32` and `PREL31` forms are not
range-checked. Neither are the AVR data and `ldi` forms, by design: they
keep the low bits of the value, so a `0x800000` data-space offset in an
input is dropped (see [AVR](#avr)).

### Other relocation errors

```text
embld: FILE: a RISC-V PCREL_LO12 relocation names 0xADDR, where no PCREL_HI20 was relocated; the two halves of an address must be emitted as a pair
embld: FILE: a thread-local relocation, but the image has no thread block -- was a __thread object linked without its .tdata/.tbss?
```

and the unsupported-type messages in
[Unsupported types](#unsupported-types).

### Output

```text
embld: cannot write 'FILE'
```

On success an ELF link prints nothing unless it writes a sidecar
(`embld: wrote OUT.embdbg (debug info from N object(s))`), and an EMBX
link prints `embld: wrote OUT (EMBX, N capabilit(y|ies))`.

## Changing the linker

### Adding a relocation type

1. Add the constant to `src/elf/elf.h` if it is not there.
2. Add the case to the x86-64 switch or the ARM switch inside
   `apply_relocs`, or to `apply_riscv` or `apply_avr`. Patch the field
   with the backend's encoder or patcher, so that the layout of a split
   immediate is written in one place; add one to `src/arch/*/emit.c` if
   it is missing.
3. On ARM, if the type can arrive in `SHT_REL`, teach the addend switch in
   `apply_relocs` to read the field back.
4. Check the range the field can hold with `need_range` (or, for a
   machine-specific limit, a `die` that names it in the style of the
   existing ones). Writing the low bits of a value that does not fit
   produces a working-looking image that jumps or reads elsewhere.
5. Test it with an object from another assembler (clang or `llvm-mc`),
   because EmbCC's own output usually does not exercise the new type.
6. Add the type to the table in [embld](../manual/tools/embld.md#relocations).

### Adding a machine

The machine is checked in `parse_object` (accepted class and
`e_machine`), merged in `add_object` (`e_flags`, the Harvard flag),
dispatched in `apply_relocs`, and written in `write_exec` (`e_flags`).
`emit_embx` writes x86-64 unconditionally. `--doctor` reads ELF64 only.
The driver links only x86-64 ELF (`compile_and_link`). Update the
machine table in the manual page.

### Adding an output group

Add the enumerator to `enum osec` at its layout position, the name to the
`classify_osec` map, the segment rule in `collect_sections`, the
`place_osec` call in `layout`, and, if a program needs its bounds, the
bracket symbols in `define_brackets`. `bounds[]` is sized by
`OSEC_COUNT + MAX_ORPHANS`.

## Tests

| Test | What it covers |
|---|---|
| `tests/golden/x86_64/embld-link.sh` | freestanding x86-64 programs linked and run on a Linux host: cross-object references, `.data`/`.bss`, an archive pulled to a fixed point with a back-reference and a dead member, `.init_array` brackets, common symbols, two `PT_LOAD` segments with W^X |
| `tests/golden/x86_64/embld-sections.sh` | orphan sections gathered contiguously, both bracket spellings, relocations inside orphan sections |
| `tests/golden/x86_64/embld-b1.sh` | a link against EmbLinkOS's `crt0.o`, `syscalls.o` and newlib's `libc.a`, compared with the cross `ld`; skips without the OS tree |
| `tests/golden/x86_64/embld-embdbg.sh`, `embld-embdbg-multi.sh` | the `.embdbg` sidecar, for one object and merged across several |
| `tests/golden/embld-doctor.sh` | every `--doctor` cause, demangling, and that all undefined names are reported in one run |
| `tests/golden/embld-reloc-range.sh` | the range refusals: x86-64 data 8 GB from RIP-relative code, and a clang RV64 `lui` (medlow) object linked at `0x80000000` |
| `tests/golden/link-dwarf.sh` | DWARF carried through a link, checked by a `gdb` session against a QEMU guest |
| `tests/golden/arm-abi-tags.sh` | the build-attribute refusals, against clang objects with hard-float arguments and short enums |
| `tests/golden/mips-link.sh` | the o32 AHL rule over addends on both sides of every carry, a string literal more than 32 KB into `.rodata`, and the MIPS refusals (gp-relative relocations, a hard-float object, a HI16 without its LO16) |
| `tests/golden/mips-access.sh` | the MIPS word-aligned `__data_end`/`__bss_start`/`__bss_end`, by a board run whose `.data` ends mid-word |
| `tests/golden/thumb-exec.sh`, `riscv-exec.sh`, `mips-exec.sh`, `avr-exec.sh` and the other embedded execution tests | every program linked with the harness `link.sh` scripts and run on QEMU, which exercises the firmware layout, the entry stub, a passing `--rom-limit` and the relocations EmbCC emits for those targets |

See [Testing](testing.md) for running them. A test that changes the
linker should also be run with an object from another toolchain, as
`arm-abi-tags.sh` does.

## Related pages

- [embld](../manual/tools/embld.md): the command, its options and the
  image layout as a user sees it.
- [Object files](object-formats.md): the objects and relocations the
  backends emit.
- [Embedded programming](../manual/embedded.md): startup code that uses
  the linker's symbols.
- [Backends](backends.md): the encoders the linker reuses.
- [Design decisions](decisions.md): D-003 (EMBX) and D-015 (the first
  32-bit target).
