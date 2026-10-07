# embld — the EmbCC linker

This page is the reference for `embld`, EmbCC's static linker. It is for
anyone who links objects by hand: firmware for the ARM, RISC-V, MIPS32
and AVR boards, programs with several objects or extra libraries, the EmbLinkOS
kernel, and EMBX application images. It covers every option, the image
layout `embld` produces, the symbols it defines, and its diagnostics.

## NAME

`embld` — link ELF relocatable objects and static archives into an
executable image

## SYNOPSIS

```text
embld [-o FILE] [-e SYMBOL] [-Ttext ADDR] [-Tdata ADDR] [-Tstack ADDR]
      [--lma-offset OFFSET] [--rom-limit BYTES]
      [--embx [--cap NAME]...]
      INPUT...

embld -T SCRIPT [-L DIR]... [-u SYMBOL]... [--orphan-handling=MODE]
      [-o FILE] [-e SYMBOL] INPUT...

embld ... [--gc-sections [--print-gc-sections]] [-Map FILE]
      [--print-memory-usage] [--cmse-implib [--out-implib=FILE]] INPUT...

embld --doctor INPUT...
```

Options and inputs may appear in any order. Run `embld` with no inputs to
print the usage summary.

## DESCRIPTION

`embld` reads ELF relocatable objects (`ET_REL`) and static archives,
resolves their symbols, lays the sections out into two segments, applies
the relocations, and writes a statically linked `ET_EXEC` ELF executable
(or, with `--embx`, an EMBX image). It is the same linker that `embcc`
runs in-process when it links a program itself; see
[Invoking EmbCC](../invoking.md#linking).

`embld` produces static executables only. It does not create or read
shared libraries, does not produce position-independent executables, and
emits no PLT and no dynamic sections. Without a linker script the layout
is fixed (see [Image layout](#image-layout)) and the options below adjust
it; with one (`-T`, ARM and RISC-V) the script lays the image out (see
[Linker scripts](#linker-scripts)).

### Supported machines

An input is recognized by its contents, not by its file name. Every
object in one link must be for the same machine; the first object read
decides which.

| Machine | ELF class | Typical targets | Output |
|---|---|---|---|
| x86-64 (`EM_X86_64`) | ELF64 | `x86_64-elf`, `x86_64-linux-gnu`, EmbLinkOS | ELF64 executable, or EMBX |
| RISC-V 64 (`EM_RISCV`) | ELF64 | `riscv64-unknown-elf` | ELF64 executable |
| RISC-V 32 (`EM_RISCV`) | ELF32 | `riscv32-unknown-elf` | ELF32 executable |
| ARM, Thumb (`EM_ARM`) | ELF32 | `thumbv7m-none-eabi`, `thumbv7em-*`, `thumbv8m.*` | ELF32 executable |
| MIPS32, little-endian o32 (`EM_MIPS`) | ELF32 | `mipsel-none-elf`; clang's `mipsel-unknown-elf` without `-fPIC` | ELF32 executable |
| AVR (`EM_AVR`) | ELF32 | `avr` | ELF32 executable |
| Renesas RX, little-endian (`EM_RX`) | ELF32 | `rx-none-elf`; rx-elf-gcc's objects (sections `P`, `D_1`, `B_1`, ...) | ELF32 executable |

AArch64 objects are not supported. Such an input is refused with:

```text
embld: FILE: a 64-bit object for machine 183; only x86-64 and RV64 (EM_RISCV) are supported
```

Mixing machines is refused with
`embld: FILE: an object for a different machine than the ones before it (N against M)`.

ARM inputs may carry their relocations in `SHT_REL` (implicit addend, as
other ARM toolchains emit) or `SHT_RELA` sections; both are applied. The
output `e_flags` are `EF_ARM_EABI_VER5` on ARM; on RISC-V, `EF_RISCV_RVC`
is set when any input has it; on AVR the architecture is taken from the
first input that names one; on MIPS the architecture and ABI bits are the
first input's.

MIPS inputs must be little-endian o32 (`embld: FILE: a MIPS object that
is not little-endian o32; this linker links mipsel o32 only`), with REL
or RELA relocations. Each input's `.MIPS.abiflags` must agree on the
floating-point ABI: a soft-float object and one built for an FPU are
refused together (`embld: FILE: its floating-point ABI (an FPU) is not
that of FIRST (soft float); ...`). `.MIPS.abiflags`, `.reginfo` and
`.pdr` are not copied to the image.

### Symbol resolution

Inputs are processed in command-line order.

- An object file is always linked.
- An archive contributes a member only when that member defines a symbol
  that is referenced and still undefined. Members are pulled repeatedly
  until a full pass pulls nothing, and the search is repeated once more
  after the last input. The order of archives on the command line
  therefore does not matter, and two archives that depend on each other
  need not be listed twice (the behavior of `--start-group` in other
  linkers, always on).
- A strong definition wins over a weak one. Two strong definitions of the
  same name are an error:
  `embld: multiple definition of 'NAME' (in FILE1 and FILE2)`.
- A common (tentative) definition yields to any real definition. Several
  common definitions of one name are merged, with the largest size and
  the strictest alignment, and placed at the end of `.bss`.
- An undefined weak reference resolves to address 0.
- An undefined strong reference is an error. `embld` stops at the first
  one, naming the symbol and the object that refers to it:

  ```text
  embld: undefined symbol 'helper' (referenced by call.o)
  ```

  For the names of compiler-runtime routines (the `__divti3` and
  `__floatuntitf` families, `__fix*`, `__trunc*`, `__extend*`) and of the
  stack unwinder (`_Unwind_*`, `__gxx_personality_v0`,
  `__register_frame_info`, `__deregister_frame_info`, `dl_iterate_phdr`),
  a `note:` line follows that says which library provides them: these
  live in EmbCC's `librt.a`, which a hand-written link line must name
  after `libc.a`.

  To see every undefined symbol at once, with a probable cause for each,
  run `embld --doctor` over the same inputs (see [`--doctor`](#--doctor)).

Archives must be in the System V/GNU `ar` format, which `llvm-ar` and GNU
`ar` write for ELF objects. The BSD format that macOS's `/usr/bin/ar`
writes is not read; such an archive fails with a message like
`embld: LIB.a(#1): too small to be an object`.

### Image layout

Input sections with the `SHF_ALLOC` flag are grouped into output sections
by name. A name matches a group when it equals the group's name or
continues it with a dot: `.text.main` joins `.text`, `.data.rel` joins
`.data`. Within a group, sections keep the order in which they were read,
each aligned to its own alignment.

The **text segment** (read and execute) starts at the text address
(`0x400000` unless `-Ttext` is given) and holds, in order:

1. `.vectors` and `.isr_vector` — an interrupt vector table, placed first
   because a Cortex-M reads its initial stack pointer and reset address
   from the start of the image. With `-Tstack`, the RISC-V or MIPS entry
   stub is placed here as well.
2. `.text`
3. `.rodata` (except on AVR; see below)
4. read-only orphan sections

The **data segment** (read and write) starts at the next 4 KiB boundary
after the text segment, or at the `-Tdata` address. It holds, in order:

1. `.tdata` and `.tbss` — the thread-local template. Sections are
   classified as thread-local by their `SHF_TLS` flag, not by name.
2. `.rodata`, on AVR only
3. `.init_array`, `.fini_array`, `.ctors`, `.dtors`
4. `.data`
5. writable orphan sections
6. `.bss`, then the common symbols. These occupy memory but no file
   space.

An **orphan section** is an allocated section whose name matches none of
the groups above, such as `.eh_frame` or a table placed with
`__attribute__((section("mytab")))`. Each distinct orphan name forms its
own group, in the order the names are first seen, and receives the
bracket symbols described below. At most 64 distinct orphan names are
allowed; one more is an error:
`embld: more than 64 distinct orphan sections (at 'NAME')`. An
unrecognized section of type `SHT_NOBITS` joins `.bss`.

On AVR, program memory and data memory are separate address spaces and no
data-space load can read program memory. `.rodata` is therefore placed in
the data segment, with a RAM address and a flash load address like
`.data`, and the startup code copies it to RAM.

The **program headers** are one `PT_LOAD` for each segment, plus a
`PT_TLS` describing the thread-local template when the image has one.
When there is a `PT_TLS`, the first `PT_LOAD` begins at file offset 0 so
that it also maps the ELF and program headers. In the file, each segment
starts at an offset congruent to its address modulo 4 KiB (modulo 4 with
`-Tdata`).

The **section headers** of the output describe the image rather than
its inputs: one `.text` section covering the text segment, one `.data`
section covering the file part of the data segment (omitted when it is
empty), the merged `.debug_*` sections, `.symtab`, `.strtab` and
`.shstrtab`.

The **symbol table** lists every defined global symbol, with the type and
size its input gave it, and the symbols `embld` defines. A symbol with no
type that lies in the text segment is given `STT_FUNC`. Local symbols of
the inputs are not copied. The symbol table, section headers and debug
sections lie outside every `PT_LOAD`, so they cost file size but no
memory on the target.

### Linker-defined symbols

`embld` defines the following symbols. Any definition of the same name
in an input object takes precedence: one in a section, an absolute one
(`.set _end, ADDR`), or a common one, which then gets its own storage.

| Symbol | Value |
|---|---|
| `__init_array_start`, `__init_array_end` | bounds of the `.init_array` group |
| `__fini_array_start`, `__fini_array_end` | bounds of the `.fini_array` group |
| `__ctors_start`, `__ctors_end` | bounds of the `.ctors` group |
| `__dtors_start`, `__dtors_end` | bounds of the `.dtors` group |
| `__start_NAME`, `__stop_NAME` | bounds of the orphan group `NAME`, when `NAME` is a C identifier |
| `__NAME_start`, `__NAME_end` | bounds of the orphan group `.NAME`, when `NAME` is a C identifier (for example `__eh_frame_start` for `.eh_frame`) |
| `__data_start` | address of the data segment |
| `__data_end`, `__bss_start` | end of the file-backed part of the data segment |
| `__data_load` | where the data segment's initial bytes are stored; equal to `__data_start` unless `-Tdata` is given |
| `__bss_end`, `_end`, `end`, `kernel_end`, `__kernel_end` | end of the image: past `.bss` and the common symbols |

An empty group has equal start and end symbols, so a loop over it runs
zero times.

With these symbols a firmware startup routine can be written in plain C:

```c
extern char __data_start[], __data_end[], __data_load[];
extern char __bss_start[], __bss_end[];

void reset(void)
{
    char *dst = __data_start, *src = __data_load;
    while (dst < __data_end)
        *dst++ = *src++;
    for (dst = __bss_start; dst < __bss_end; )
        *dst++ = 0;
    main();
}
```

### Firmware layout

A microcontroller stores its initial data in flash and uses it in RAM.
`-Tdata ADDR` describes this: the data segment is *addressed* at `ADDR`
(its virtual address, `p_vaddr`) and *stored* directly after the text
segment, aligned to 4 bytes (its load address, `p_paddr`, and the value of
`__data_load`). Segments are then aligned to 4 bytes instead of 4 KiB, so
that no flash is spent on padding.

`--rom-limit BYTES` gives the size of the part's flash. The bytes counted
run from the text address to the end of the text segment, or, with
`-Tdata`, to the end of the stored initial data. An image that does not
fit is refused:

```text
embld: the image needs 1504 bytes of flash and the part has 1000 (--rom-limit): 1498 of text, 4 of initial data
```

On RISC-V and MIPS, nothing sets the stack pointer at reset, so
something must before the first C function runs. `-Tstack ADDR` makes
`embld` place an entry stub in the vector group at the start of the text
segment; the stub loads `ADDR` into `sp` and jumps to the entry symbol
(on RISC-V `auipc t0` and `jalr zero` through `t0`, which reaches ±2 GB;
on MIPS `lui`/`ori` of the entry's absolute address into `t9` and `jr
t9`, which reaches anywhere). The ELF entry point becomes the stub's
address. The option is refused on every other machine:

```text
embld: -Tstack is a RISC-V and MIPS option: every other target here starts with a stack pointer already set (a Cortex-M reads its own from the vector table)
```

On MIPS, `__data_end`, `__bss_start` and `__bss_end` are kept on word
boundaries (the data segment's stored bytes and its size are padded to
a multiple of 4), because the startup's word loops would trap on a
misaligned address.

If the entry symbol is more than 2 GB from the stub, the link fails with
`embld: the entry symbol is more than 2GB from the image base`.

On ARM, the entry symbol's Thumb bit is kept in the ELF entry point.

### Linker scripts

`-T SCRIPT` lays the image out by a GNU ld linker script instead, for ARM,
RISC-V and AVR images (another machine is refused: `embld: -T: a linker
script is supported for ARM, RISC-V and AVR images only (this one is
machine 8)`): the script a CMSIS, STM32CubeMX, vendor SDK, avr-libc or
RTOS project already has. It replaces `-Ttext`, `-Tdata`, `-Tstack`,
`--rom-limit` and `--lma-offset`, which are refused with it, and the
linker defines no bracket symbols of its own except `__start_NAME` and
`__stop_NAME` for an output section whose name is a C identifier.

```sh
embld -T STM32F407VGTx_FLASH.ld startup.o main.o librt.a -o fw.elf
```

The layout follows ld's rules:

- An input section is claimed by the first input description that
  matches it, in script order. Within one description, inputs are taken
  in command-line order and each object's sections in their own order,
  unless `SORT`, `SORT_BY_NAME`, `SORT_BY_ALIGNMENT` or
  `SORT_BY_INIT_PRIORITY` says otherwise.
- An output section starts at its address if it gives one, else at the
  next free byte of its `> REGION`, else at `.`. Its load address is
  `AT(ADDR)`, or the next free byte of its `AT> REGION`, or its run
  address shifted by as much as the previous section in the same region,
  or its run address. One free-byte pointer per region serves both, which
  is how `.data` is stored right after the code in flash.
- A section that names a region must fit in it. Otherwise the link stops
  with `region RAM overflowed by N bytes`, as ld words it.
- An allocated input section that nothing claims is an orphan. It gets an
  output section of its own name after the last one of its kind (code,
  read-only data, data, zero-initialised).
  `--orphan-handling=warn` reports each one; `=error` refuses the link.
- A section with no bytes of its own, `.bss` or one made only of `.`
  moves such as `._user_heap_stack`, takes no file space and no flash.
- Symbol assignments are evaluated where they stand. A forward reference
  (`_sidata = LOADADDR(.data)` above `.data`) uses the previous pass, and
  the layout is repeated until nothing moves.

The output has one `PT_LOAD` per output section, whose physical address
is the section's load address, and a section header per output section
under the script's names. `llvm-objcopy -O binary`, a flash programmer and
QEMU's `-kernel` therefore put every byte where the script says.

Supported: `ENTRY`, `MEMORY`, `SECTIONS`, `REGION_ALIAS`, `INCLUDE`,
`INPUT`, `GROUP`, `STARTUP`, `SEARCH_DIR`, `EXTERN`, `ASSERT`, `PROVIDE`,
`PROVIDE_HIDDEN`, `HIDDEN`; output sections with an address, `(NOLOAD)`,
`AT()`, `ALIGN()`, `SUBALIGN()`, `> REGION`, `AT> REGION`, `=FILL` and
`/DISCARD/`; `KEEP`, `EXCLUDE_FILE`, `archive:member` patterns, `COMMON`,
`BYTE`/`SHORT`/`LONG`/`QUAD`/`SQUAD`, `FILL`, and assignments to `.`
(an absolute value inside an output section is an offset into it, as in
ld); expressions with C's operators and `ALIGN`, `ORIGIN`, `LENGTH`,
`ADDR`, `LOADADDR`, `SIZEOF`, `ALIGNOF`, `DEFINED`, `MIN`, `MAX`,
`ABSOLUTE`, `LOG2CEIL`, `CONSTANT` and `SIZEOF_HEADERS`.
`OUTPUT_FORMAT`, `OUTPUT_ARCH` and `TARGET` are read and ignored: the
machine comes from the objects. An output section whose inputs all have
one special type (`INIT_ARRAY`, `ARM_EXIDX`, a note) has that type in the
image, and an `.ARM.exidx` output section links to the section its
functions went to, as ld writes them, so `llvm-readelf -u` and a
debugger find the unwind index.

Refused by name: `PHDRS`, `OVERLAY`, `INSERT`, `NOCROSSREFS`,
`ONLY_IF_RO`/`ONLY_IF_RW`, `INPUT_SECTION_FLAGS`, output section types
other than `NOLOAD`, and `DATA_SEGMENT_*`. A script that uses `/DISCARD/`
on a section the program still refers to is refused with both sections
named. COMMON symbols (tentative definitions from an object built with
`-fcommon`; EmbCC emits none) are refused unless the script places
`*(COMMON)`, because anywhere else is outside the range the startup
zeroes.

#### AVR scripts

An AVR script is written as avr-libc's are. Program space and data space
are separate on AVR, and the script tells them apart by address: flash
from 0, and data space from `0x800000`, so the ATmega328P's SRAM begins
at `0x800100`. A relocation keeps the low 16 bits of a data address, as
avr-ld's does, so `0x800100` is the pointer `0x0100`.

```text
ENTRY(__vectors)
MEMORY
{
  text (rx)   : ORIGIN = 0, LENGTH = 32K
  data (rw!x) : ORIGIN = 0x800100, LENGTH = 2K
}
SECTIONS
{
  .text : { KEEP(*(.vectors)) *(.text .text.*) } > text
  .data : {
    __data_start = .;
    *(.rodata .rodata*) *(.data .data*)
    . = ALIGN(2);
    __data_end = .;
  } > data AT> text
  __data_load = LOADADDR(.data);
  .bss (NOLOAD) : { __bss_start = .; *(.bss .bss*) *(COMMON) __bss_end = .; } > data
}
```

EmbCC reads read-only data with data-space loads, as avr-gcc does without
`__flash` or `PROGMEM`. So `.rodata` belongs in a section the startup
copies to RAM, beside `.data`, which is where avr-libc's scripts put it.
A script that leaves `.rodata` in program space would link and then read
whatever RAM holds at those addresses. That script is refused, with the
input section and its object named:

```text
embld: m328p.ld:14: section .text keeps .rodata of main.o in program space at 0x1a4; EmbCC reads read-only data from RAM on AVR, so it has to be in an output section the startup copies there (> data AT> text, as avr-libc's scripts place it)
```

The linker defines no symbols for an AVR script either. The startup's
copy and zeroing loops need the brackets: `__data_load`, `__data_start`,
`__data_end`, `__bss_start` and `__bss_end` for EmbCC's AVR startup, or
avr-libc's `__data_load_start` names. `tests/golden/avr-ldscript.sh` links
the same program with a script and with `-Ttext`/`-Tdata` and runs both
on QEMU's ATmega328P.

### Garbage collection

With `--gc-sections`, an allocated input section that nothing kept
refers to is left out of the image, as GNU ld does it. It is meant for
objects built with `-ffunction-sections -fdata-sections`, where each
function and object is a section of its own; with one `.text` per object,
an object is all kept or all dropped.

The link marks from these roots, and then from every relocation of every
section marked, to the section its symbol is in:

- the entry symbol, each `-u` symbol, and a script's `EXTERN` names;
- a section a script claims with `KEEP()`;
- the constructor and destructor arrays (by name, `.init_array*`,
  `.fini_array*`, `.preinit_array*`, `.ctors*`, `.dtors*`, `.init`,
  `.fini`, or by section type), notes, and sections marked
  `SHF_GNU_RETAIN`;
- every section named `NAME` when `__start_NAME` or `__stop_NAME` is
  referred to;
- without a script, the vector table (`.vectors`, `.isr_vector`).

A section with `SHF_LINK_ORDER` -- ARM's `.ARM.exidx.text.F` -- is kept
exactly when the section it belongs to is. `.eh_frame` is kept, but its
FDEs do not keep the functions they describe: an FDE for a function that
was dropped is left with a range of 0, so it describes no address, and
its personality routine and LSDA are still reached through the CIE and
the FDEs that remain. DWARF is never collected; its references to a
dropped function resolve to 0. A dropped section's symbols are left out
of the image's symbol table.

`--print-gc-sections` names each dropped section on standard error, in
ld's words:

```text
embld: removing unused section '.text.unused_fn' in file 'extra.o'
```

### Map file and memory usage

`-Map FILE` writes GNU ld's map: the archive members pulled in and the
object and symbol each was pulled for, the input sections dropped, the
memory regions, and each output section with its address, size and load
address, the input sections in it, and the global symbols each defines.

```text
.text           0x00000040      0x2a4
 .text.Reset_Handler
                0x00000040       0x78 startup.o
                0x00000041                Reset_Handler
 .text.Default_Handler
                0x000000b8        0x4 startup.o
                0x000000b9                Default_Handler
 .text.init_first
                0x000000bc       0x10 prog.o
 .text.main     0x000000cc      0x198 prog.o
                0x000000cd                main
```

`--print-memory-usage` prints, after the layout, ld's table of how much
of each `MEMORY` region of the script the image uses -- its run
addresses and the load addresses stored there, so `FLASH` counts
`.data`'s initial values:

```text
Memory region         Used Size  Region Size  %age Used
             RAM:       1968 B        64 KB      3.00%
           FLASH:        844 B       256 KB      0.32%
```

Without a script there are no regions, and the table has only its
header.

### Debug information

The `.debug_*` sections of every input object, archive members included,
are concatenated per section name and relocated, so the executable
carries DWARF that `gdb`, `llvm-dwarfdump` and [`embdbg`](embdbg.md) can
read. A link of objects compiled without `-g` carries none and costs
nothing.

When at least one object named on the command line (not an archive
member) has a `.debug_line` section, `embld` also writes a native
`.embdbg` file beside the output, named `OUT.embdbg`, and reports it on
standard error:

```text
embld: wrote fw.elf.embdbg (debug info from 1 object)
```

The `.embdbg` file holds the functions, line table, variables and types of
those objects with their final addresses, and a build ID that is the
SHA-256 of the linked image. [`embdbg`](embdbg.md) reads it. A link
without debug information does not remove an `.embdbg` file left by an
earlier link. No `.embdbg` file is written with `--embx`.

### ARM build attributes

`embld` reads the `.ARM.attributes` section of every ARM input and refuses
a set of objects that disagree about how floating-point arguments are
passed, or about the size of an enumeration:

```text
embld: 'a.o' and 'b.o' disagree about where floating-point arguments go: one passes them in the core registers (-mfloat-abi=soft) and the other in s0-s15 (-mfloat-abi=hard). Linking them would leave every float argument read from a register the caller never wrote
embld: 'a.o' and 'b.o' disagree about the size of an enum, which changes the layout of every struct that holds one
```

An object without an attributes section takes no part in the comparison.

### ARMv8-M secure gateway veneers

An ARM input that defines a global `__acle_se_NAME` beside a global
`NAME` at the same address -- what `embcc -mcmse` writes for a
`cmse_nonsecure_entry` function -- gets a secure gateway veneer, as GNU
ld makes one:

```text
NAME:   sg                          e97f e97f
        b.w     __acle_se_NAME
```

The veneers are eight bytes each, in name order, in a section
`.gnu.sgstubs` that is 32-byte aligned and padded to 32 bytes (the SAU's
granule), and `NAME` comes to name the veneer, so a call from anywhere
enters through the gateway while `__acle_se_NAME` still names the code.
A linker script places the section by name (`KEEP(*(.gnu.sgstubs*))`) in
the region the image marks Non-secure Callable; without one it is an
orphan after `.rodata`. A `__acle_se_` symbol with no `NAME` at its
address, or one that is not Thumb code, stops the link by name.

`--cmse-implib --out-implib=FILE` also writes the import library: an ELF32
relocatable object holding, for each veneer, a global absolute
(`SHN_ABS`) Thumb function symbol `NAME` at the veneer's address. A
Non-secure image links against it to call the Secure entry functions.

A Thumb call (`R_ARM_THM_CALL`, `R_ARM_THM_JUMP24`) to an ABSOLUTE symbol
-- an import library's, chiefly -- that is out of a `bl`'s ±16 MiB goes
through a long-branch veneer the linker adds to `.text`: `movw ip, #lo;
movt ip, #hi; bx ip`, or on ARMv6-M, which has no `movw`, `push {r0,
r1}; ldr r0, [pc, #4]; str r0, [sp, #4]; pop {r0, pc}` and the address.
`ip` is the register the AAPCS gives a veneer. A call that reaches
branches directly. The Non-secure code of an ARMv8-M part is usually
that far from the Secure veneers (0x00200000 against 0x10000000 on the
mps2-an505), so a call through the import library needs it.

`tests/golden/thumbv8m-cmse.sh` links a Secure and a Non-secure image
this way and runs them on QEMU's mps2-an505.

### Relocations

| Machine | Relocation types applied |
|---|---|
| x86-64 | `R_X86_64_64`, `R_X86_64_32`, `R_X86_64_32S`, `R_X86_64_PC32`, `R_X86_64_PLT32` (as `PC32`; no PLT is created), `R_X86_64_PC64`, `R_X86_64_TPOFF32` (local-exec thread-local storage) |
| ARM | `R_ARM_NONE`, `R_ARM_ABS32`, `R_ARM_TARGET1` (as `R_ARM_ABS32`), `R_ARM_REL32`, `R_ARM_PREL31`, `R_ARM_THM_CALL`, `R_ARM_THM_JUMP24`, `R_ARM_THM_MOVW_ABS_NC`, `R_ARM_THM_MOVT_ABS` |
| RISC-V | `R_RISCV_32`, `R_RISCV_64`, `R_RISCV_HI20`, `R_RISCV_LO12_I`, `R_RISCV_LO12_S`, `R_RISCV_PCREL_HI20`, `R_RISCV_PCREL_LO12_I`, `R_RISCV_PCREL_LO12_S`, `R_RISCV_BRANCH`, `R_RISCV_JAL`, `R_RISCV_CALL`, `R_RISCV_CALL_PLT`; `R_RISCV_RELAX` and `R_RISCV_ALIGN` are accepted and ignored |
| MIPS | `R_MIPS_NONE`, `R_MIPS_32`, `R_MIPS_26`, `R_MIPS_HI16`, `R_MIPS_LO16`, `R_MIPS_PC16`; `R_MIPS_JALR` is accepted and ignored |
| AVR | `R_AVR_NONE`, `R_AVR_32`, `R_AVR_16`, `R_AVR_16_PM`, `R_AVR_LO8_LDI`, `R_AVR_HI8_LDI`, `R_AVR_LO8_LDI_GS`, `R_AVR_HI8_LDI_GS`, `R_AVR_CALL`, `R_AVR_13_PCREL`, `R_AVR_7_PCREL` |

Any other type stops the link with a message of this form (the machine
name is omitted for x86-64):

```text
embld: FILE: unsupported RISC-V relocation type N (this is the next linker increment, not a bug in your program)
```

In a MIPS REL object an `R_MIPS_HI16` takes its addend's low half from
the `R_MIPS_LO16` that follows it against the same symbol (the o32
rule), and one without such a `R_MIPS_LO16` is refused. The relocations
of small-data and position-independent code are refused by name:
`R_MIPS_GPREL16`, `R_MIPS_GPREL32` and `R_MIPS_LITERAL` (compile with
`-G0`), `R_MIPS_GOT16` and `R_MIPS_CALL16` (compile without `-fPIC`).

`embld` performs no linker relaxation and creates no veneers,
trampolines or stubs, except ARMv8-M's (above). A relocated value that its field cannot hold is an
error, not truncated. For these types the message names the relocation,
the symbol, the value and the range the field holds:

| Machine | Checked types |
|---|---|
| x86-64 | `R_X86_64_32` (0 to 2^32 - 1), `R_X86_64_32S`, `R_X86_64_PC32`, `R_X86_64_PLT32` and `R_X86_64_TPOFF32` (-2^31 to 2^31 - 1) |
| RISC-V | `R_RISCV_32` (-2^31 to 2^32 - 1), `R_RISCV_BRANCH` (±4 KiB), `R_RISCV_JAL` (±1 MiB); at RV64 only, `R_RISCV_HI20`, `R_RISCV_PCREL_HI20`, `R_RISCV_CALL` and `R_RISCV_CALL_PLT` (about ±2 GiB) |
| MIPS | `R_MIPS_32` (-2^31 to 2^32 - 1), `R_MIPS_PC16` (-131072 to 131068), `R_MIPS_26` (a multiple of 4, in the 256 MiB region of the instruction after the jump; refused with `a jal or j at ADDR to 'SYM' at ADDR, which is in another 256 MiB region; jal reaches only its own`) |

```text
embld: far.o: R_X86_64_PC32 against 'g' needs 12880707577 (0x2ffbffff9), and the field holds -2147483648 to 2147483647; the image is laid out beyond what this code can reach
```

At RV64, `lui` sign-extends its result, so an object built for the
`medlow` code model (which addresses data with `lui`) cannot address
data at `0x7ffff800` or above, and cannot be linked at the `0x80000000`
RAM base of QEMU's `virt` board. EmbCC itself uses `auipc` (the `medany`
model) at both widths.

```text
embld: lui64.o: R_RISCV_HI20 against 'g' needs 2147487744 (0x80001000), and the field holds -2147485696 to 2147481599; the image is laid out beyond what this code can reach
```

At RV32 the `lui`/`auipc` forms and `call` wrap around the 32-bit address
space and reach every address, so they are not checked. The other
relocation errors have messages of their own:

| Message | Cause |
|---|---|
| `a Thumb call is more than 16MB away; this linker mints no veneers` | ARM `bl`/`b.w` out of range, to a symbol that is not absolute |
| `an rjmp reaches +-4KB and this target is N bytes away; ...` | AVR `rjmp`/`rcall` out of range; use `call` and `jmp` |
| `a conditional branch reaches +-126 bytes and this target is N away; ...` | AVR conditional branch out of range |
| `a call to an odd address 0x...; ...` | AVR `call`/`jmp` to an odd byte address |
| `a thread-local relocation, but the image has no thread block ...` | a `TPOFF32` relocation in an image with no `.tdata`/`.tbss` |

The ARM `movw`/`movt` and word forms and the AVR data and `ldi` forms
keep the low bits of the value without a check.

### EMBX output

`--embx` writes an EMBX version 1 application image, EmbLinkOS's native
executable format, instead of an ELF file. It contains the same two
segments as the ELF output: a 128-byte header, two segment descriptors
(text `r-x` and data `rw-`, each aligned to 4 KiB, with file offsets
congruent to their addresses), a capability table, and the segment
payloads. Each payload carries a CRC32C checksum, the header carries a
CRC32C over its first 124 bytes, and the build ID is the SHA-256 of the
whole image. On success `embld` reports:

```text
embld: wrote app.embx (EMBX, 2 capabilities)
```

The capability table lists the capabilities the program requires, named
with `--cap`, in ascending order. The EMBX header always records the
machine as x86-64, so link only x86-64 objects with `--embx`. An EMBX
image has no symbol table, no section headers, no `PT_TLS` equivalent and
no `.embdbg` file. Check the result with [`embread`](embread.md).

## OPTIONS

Numeric arguments are read in C notation: decimal, hexadecimal with a
`0x` prefix, or octal with a leading `0`. They are not validated: text
that is not a number reads as 0.

### `-o FILE`

Write the output to `FILE`. The default is `a.out`. Without a file name,
`embld` prints `embld: -o needs a file` and exits with status 2.

### `-e SYMBOL`

Use `SYMBOL` as the entry point. The default is `_start`. If the symbol is
not defined after resolution, the link fails with
`embld: entry symbol 'SYMBOL' is undefined`. Firmware usually names the
reset handler (`-e reset`) or, on AVR, the vector table (`-e __vectors`),
because the processor starts at address 0.

### `-Ttext ADDR`, `-TtextADDR`

Place the text segment at `ADDR`. The default is `0x400000`. `0` is a
valid address and is honored. The address may be given as the next
argument or attached to the option. The spelling `-Ttext=ADDR` is not
recognized: the `=` makes the address read as 0.

### `-Tdata ADDR`, `-TdataADDR`

Use the firmware layout: address the data segment at `ADDR` and store its
initial contents immediately after the text segment. See
[Firmware layout](#firmware-layout). Without this option, the data
segment follows the text segment at the next 4 KiB boundary and is stored
where it is addressed. The same spelling rules as `-Ttext` apply; an
address of 0 is the same as not giving the option.

### `-Tstack ADDR`, `-TstackADDR`

RISC-V and MIPS only. Emit an entry stub that sets the stack pointer to
`ADDR` and jumps to the entry symbol, and make the stub the ELF entry point. See
[Firmware layout](#firmware-layout). On any other machine the link fails.

### `--lma-offset OFFSET`, `--lma-offset=OFFSET`

Set each segment's physical (load) address to its virtual address minus
`OFFSET`. This is for a higher-half kernel whose virtual addresses are
offset from where the boot loader places it. The default is 0
(`p_paddr` equal to `p_vaddr`). With `-Tdata`, the data segment's load
address is the one described under [Firmware layout](#firmware-layout)
and `OFFSET` does not apply to it.

### `--rom-limit BYTES`, `--rom-limit=BYTES`

Refuse an image that needs more than `BYTES` bytes of flash. The default
is no limit. See [Firmware layout](#firmware-layout).

### `-T SCRIPT`, `-TSCRIPT`, `--script=SCRIPT`

Lay the image out by the GNU ld linker script `SCRIPT`. See
[Linker scripts](#linker-scripts). ARM and RISC-V only; refused with
`-Ttext`, `-Tdata`, `-Tstack`, `--rom-limit`, `--lma-offset` and `--embx`.

### `-L DIR`, `-LDIR`

Look for the files a script names in `INPUT`, `GROUP`, `STARTUP` and
`INCLUDE` in `DIR`, after the script's own `SEARCH_DIR`s and before the
script's directory. `-lNAME` in a script is `libNAME.a` there.

### `-u SYMBOL`, `--undefined=SYMBOL`

Treat `SYMBOL` as referenced, so that the archive member defining it is
linked in. A script's `EXTERN` and `ENTRY` do the same.

### `--orphan-handling=place|warn|error`

What to do with an allocated input section no rule of the script places:
`place` it as ld does (the default), place it and `warn`, or refuse the
link with an `error`.

### `--gc-sections`, `--no-gc-sections`

Leave out the sections nothing kept refers to. See
[Garbage collection](#garbage-collection).

### `--print-gc-sections`, `--no-print-gc-sections`

Name each section `--gc-sections` leaves out, on standard error. Given
without `--gc-sections`, the link is refused, since there is nothing to
print.

### `-Map FILE`, `-Map=FILE`, `--Map FILE`, `--Map=FILE`

Write the map file. See [Map file and memory usage](#map-file-and-memory-usage).

### `--print-memory-usage`

Print the region usage table on standard output.

### `--cmse-implib`

ARMv8-M: with `--out-implib`, write the secure gateway import library.
The veneers themselves are made whether or not it is given (see
[ARMv8-M secure gateway veneers](#armv8-m-secure-gateway-veneers)).

### `--out-implib=FILE`, `--out-implib FILE`

Write the import library to `FILE`. Requires `--cmse-implib`:
`--out-implib needs --cmse-implib: the import library this linker writes
is ARMv8-M's secure gateway one`.

### `--in-implib=FILE`

Refused: `--in-implib is not supported: it keeps each secure gateway
veneer at the address an earlier import library gave it, and this linker
lays the veneers out in name order every link`. A Secure image whose
veneers must keep their addresses across releases cannot be built with
embld yet.

### `--embx`

Write an EMBX image instead of an ELF executable. See
[EMBX output](#embx-output).

### `--cap NAME`

Declare that the program requires the capability `NAME`. The option may
be repeated. It has an effect only with `--embx`. Names are matched
without regard to case:

| Name | Capability id |
|---|---|
| `filesystem` | 1 |
| `network` | 2 |
| `gpu` | 3 |
| `audio` | 4 |
| `camera` | 5 |
| `usb` | 6 |
| `serial` | 7 |
| `rawdisk` | 8 |
| `kernel_ext` | 9 |

Any other name is refused with `embld: unknown capability 'NAME'` and exit
status 2.

### `--doctor`

Link nothing. Read the inputs and report every symbol that is referenced
and not defined, each with what the inputs suggest about it. All other
options are accepted and ignored.

For each undefined symbol, `--doctor` prints the name, its demangled form
when it is a C++ name (`that is vtable for Shape`), the input that wants
it, and one of these explanations:

| What the inputs show | What `--doctor` says |
|---|---|
| another input defines the name, but as `static` | names that input and suggests dropping the `static` or moving the caller |
| the name is a C library function | names the header that declares it and says to link `libc.a` |
| `__cxa_*`, `_Unwind_*`, `__cxxabiv1`, `_ZSt*` | the C++ runtime: link libsupc++ and libstdc++ |
| `_ZTV...`, `_ZTI...` | the key-function rule: a vtable and typeinfo are emitted with the class's first non-inline virtual function |
| another C++ name | a member function declared and never defined |
| nothing | asks whether a source file or library is missing from the link |

```text
embld: undefined: helper
  wanted by call.o
  own.o does define it — but as `static`, which keeps it inside that
  unit. Drop the `static`, or move the caller into that file.
embld: doctor: 1 symbol undefined
```

The report goes to standard error and the exit status is 1. When nothing
is undefined, `--doctor` prints
`embld: doctor: every symbol referenced is defined (N definitions across M inputs)`
on standard output and exits with status 0.

`--doctor` answers a different question from a link, and its scope
differs accordingly:

- It considers every member of every archive, not only the members a link
  would pull in. A reference made by a member that the link would never
  use is reported.
- It does not know the symbols `embld` defines itself (see
  [Linker-defined symbols](#linker-defined-symbols)); references to
  them, such as `__init_array_start` from a start file, are reported.
- It reads 64-bit ELF objects only. A 32-bit object (ARM, RV32, AVR) is
  skipped without a message, so a link of such objects always reports
  that every symbol is defined.
- It reads archive members stored under a short name only. A member whose
  name is longer than 15 characters (stored in the GNU long-name table)
  is skipped without a message.
- An input that cannot be read is reported
  (`embld: doctor: cannot read FILE`) and skipped.

## INPUT FILES

Each `INPUT` is an ELF relocatable object or a System V/GNU `ar` archive;
the two are told apart by the archive signature `!<arch>`. At most 256
inputs may be given (`embld: too many inputs`, exit status 2). An input
that cannot be read stops the link with `embld: cannot read 'FILE'`. Other
refusals of an input:

```text
embld: FILE: not an ELF file
embld: FILE: not little-endian
embld: FILE: not a 32- or 64-bit ELF
embld: FILE: not a relocatable object (ET_REL)
embld: FILE: too small to be an object
embld: FILE: section headers run past end of file
embld: FILE: corrupt archive header at offset N
```

`embld` takes no `-l` or `-L` options: name each library archive by its
path. The libraries EmbCC builds for a target are in the target's library
directory, which `embcc --print-search-dirs` reports.

## OUTPUT

The output is an `ET_EXEC` ELF file of the inputs' class and machine, or
an EMBX image with `--embx`, written to `-o FILE` or `a.out`. With debug
information, `FILE.embdbg` is written as well. The output is written
without execute permission.

Messages go to standard error, each prefixed with `embld:`. A successful
ELF link prints nothing unless it writes an `.embdbg` file.

## EXIT STATUS

| Status | Meaning |
|---|---|
| 0 | the image was written (with `--doctor`: nothing is undefined) |
| 1 | the link failed (with `--doctor`: at least one symbol is undefined) |
| 2 | a usage error: unknown option, missing option argument, unknown capability, no inputs, too many inputs |

## ENVIRONMENT

`embld` reads no environment variables.

## EXAMPLES

Link a static Linux x86-64 program by hand, where `$LIB` is the
`x86_64-linux-gnu` library directory:

```sh
embcc --target=x86_64-linux-gnu -c main.c -o main.o
embcc --target=x86_64-linux-gnu -c util.c -o util.o
embld -o prog $LIB/crt1.o main.o util.o $LIB/libc.a $LIB/librt.a
chmod +x prog
```

Link a Cortex-M3 firmware image with flash at 0 and SRAM at
`0x20000000`:

```sh
embcc --target=thumbv7m-none-eabi -c boot.c -o boot.o
embcc --target=thumbv7m-none-eabi -c main.c -o main.o
embld -e reset -Ttext 0x0 -Tdata 0x20000000 boot.o main.o -o fw.elf
qemu-system-arm -M lm3s6965evb -cpu cortex-m3 -nographic -kernel fw.elf
```

Link an ATmega328P image (32 KiB of flash, SRAM from `0x100`) with the
compiler runtime:

```sh
embcc --target=avr -c boot.S -o boot.o
embcc --target=avr -c main.c -o main.o
embld -e __vectors -Ttext 0x0 -Tdata 0x100 --rom-limit 32768 \
      boot.o main.o $LIB/librt.a -o fw.elf
```

Link a RISC-V image for QEMU's `virt` board, which loads it into RAM at
`0x80000000`, with the stack at the top of 8 MiB:

```sh
embld -e _start -Ttext 0x80000000 -Tstack 0x80800000 boot.o main.o -o fw.elf
```

Link a higher-half kernel whose virtual base is `0xFFFFFFFF80000000`:

```sh
embld -e _start -Ttext 0xFFFFFFFF80100000 \
      --lma-offset 0xFFFFFFFF80000000 -o kernel.elf *.o
```

Link an EMBX application that needs the file system and the network, and
verify it:

```sh
embld --embx --cap filesystem --cap network -o app.embx crt0.o app.o libc.a
embread app.embx
```

Find out why a link fails:

```sh
embld --doctor main.o util.o
```

## SEE ALSO

[`embread`](embread.md), [`embdbg`](embdbg.md), [`embas`](embas.md),
[Invoking EmbCC](../invoking.md#linking),
[Embedded programming](../embedded.md), [Targets](../targets.md),
[Debugging](../debugging.md), [Libraries](../libraries.md)
