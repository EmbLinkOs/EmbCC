# embld — the EmbCC linker

This page is the reference for `embld`, EmbCC's static linker. It is for
anyone who links objects by hand: firmware for the ARM, RISC-V and AVR
boards, programs with several objects or extra libraries, the EmbLinkOS
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
emits no PLT and no dynamic sections. It reads no linker script: the
layout is fixed (see [Image layout](#image-layout)), and the options below
are the only way to influence it.

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
| AVR (`EM_AVR`) | ELF32 | `avr` | ELF32 executable |

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
first input that names one.

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
   from the start of the image. With `-Tstack`, the RISC-V entry stub is
   placed here as well.
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

`embld` defines the following symbols. A definition of the same name in
a section of an input object takes precedence.

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

On RISC-V, every register is zero at reset, so something must set the
stack pointer before the first C function runs. `-Tstack ADDR` makes
`embld` place an entry stub in the vector group at the start of the text
segment; the stub loads `ADDR` into `sp` and jumps to the entry symbol (`auipc t0` and
`jalr zero` through `t0`, which reaches ±2 GB). The ELF entry point
becomes the stub's address. The option is refused on every other machine:

```text
embld: -Tstack is a RISC-V option: every other target here starts with a stack pointer already set (a Cortex-M reads its own from the vector table)
```

If the entry symbol is more than 2 GB from the stub, the link fails with
`embld: the entry symbol is more than 2GB from the image base`.

On ARM, the entry symbol's Thumb bit is kept in the ELF entry point.

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

### Relocations

| Machine | Relocation types applied |
|---|---|
| x86-64 | `R_X86_64_64`, `R_X86_64_32`, `R_X86_64_32S`, `R_X86_64_PC32`, `R_X86_64_PLT32` (as `PC32`; no PLT is created), `R_X86_64_PC64`, `R_X86_64_TPOFF32` (local-exec thread-local storage) |
| ARM | `R_ARM_ABS32`, `R_ARM_REL32`, `R_ARM_PREL31`, `R_ARM_THM_CALL`, `R_ARM_THM_JUMP24`, `R_ARM_THM_MOVW_ABS_NC`, `R_ARM_THM_MOVT_ABS` |
| RISC-V | `R_RISCV_32`, `R_RISCV_64`, `R_RISCV_HI20`, `R_RISCV_LO12_I`, `R_RISCV_LO12_S`, `R_RISCV_PCREL_HI20`, `R_RISCV_PCREL_LO12_I`, `R_RISCV_PCREL_LO12_S`, `R_RISCV_BRANCH`, `R_RISCV_JAL`, `R_RISCV_CALL`, `R_RISCV_CALL_PLT`; `R_RISCV_RELAX` and `R_RISCV_ALIGN` are accepted and ignored |
| AVR | `R_AVR_NONE`, `R_AVR_32`, `R_AVR_16`, `R_AVR_16_PM`, `R_AVR_LO8_LDI`, `R_AVR_HI8_LDI`, `R_AVR_LO8_LDI_GS`, `R_AVR_HI8_LDI_GS`, `R_AVR_CALL`, `R_AVR_13_PCREL`, `R_AVR_7_PCREL` |

Any other type stops the link with a message of this form (the machine
name is omitted for x86-64):

```text
embld: FILE: unsupported RISC-V relocation type N (this is the next linker increment, not a bug in your program)
```

`embld` performs no linker relaxation and creates no veneers or
trampolines. A branch whose target is out of range is an error that
names the limit:

| Message | Cause |
|---|---|
| `a Thumb call is more than 16MB away; this linker mints no veneers` | ARM `bl`/`b.w` out of range |
| `a RISC-V call is more than 2GB away; auipc/jalr cannot reach it and this linker mints no stubs` | RISC-V `call` out of range |
| `an rjmp reaches +-4KB and this target is N bytes away; ...` | AVR `rjmp`/`rcall` out of range; use `call` and `jmp` |
| `a conditional branch reaches +-126 bytes and this target is N away; ...` | AVR conditional branch out of range |
| `a call to an odd address 0x...; ...` | AVR `call`/`jmp` to an odd byte address |
| `a thread-local relocation, but the image has no thread block ...` | a `TPOFF32` relocation in an image with no `.tdata`/`.tbss` |

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

RISC-V only. Emit an entry stub that sets the stack pointer to `ADDR` and
jumps to the entry symbol, and make the stub the ELF entry point. See
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
embcc --target=x86_64-linux-gnu -c main.c util.c
embld -o prog $LIB/crt1.o main.o util.o $LIB/libc.a $LIB/librt.a
chmod +x prog
```

Link a Cortex-M3 firmware image with flash at 0 and SRAM at
`0x20000000`:

```sh
embcc --target=thumbv7m-none-eabi -c boot.c main.c
embld -e reset -Ttext 0x0 -Tdata 0x20000000 boot.o main.o -o fw.elf
qemu-system-arm -M lm3s6965evb -cpu cortex-m3 -nographic -kernel fw.elf
```

Link an ATmega328P image (32 KiB of flash, SRAM from `0x100`) with the
compiler runtime:

```sh
embcc --target=avr -c boot.S main.c
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
