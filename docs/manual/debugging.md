# Debugging

This page describes the debug information EmbCC produces: the `-g`
options, the DWARF it emits and what that DWARF describes, which targets
and object formats support it, how `-g` and optimization interact, how
EmbLD carries debug information into a linked image, and how to debug a
program with gdb, lldb, a QEMU gdb stub or EmbDBG. It is written for
people who compile code with EmbCC and want to debug it.

## Producing debug information

### `-g`

Emit DWARF version 4 debug information into the object file. Off by
default.

The output is deterministic. The compilation directory is recorded as
`.`, not as the directory the compiler ran in, and the source file name
exactly as it was given on the command line. Compiling the same file
under the same relative name, with the same options, gives the same
bytes in any directory; an absolute file name is recorded as such.

`-g` can be combined with any `-O` level, and it never changes the
generated code (see [`-g` does not change the generated
code](#-g-does-not-change-the-generated-code)). See
[Optimized code](#optimized-code) for what a debugger can rely on above
`--opt=none` and `--opt=debug`.

### `-ggdb`, `-g1`, `-g2`, `-g3`, `-gdwarf`

Each of these is the same as `-g`. EmbCC emits debug information at one
level of detail. In particular `-g1` does not limit the output to line
tables, and `-g3` does not add macro information.

### `-gdwarf-5`, `-gdwarf-4`

`-g` writes DWARF 5, as GCC 11 and Clang 14 do: version 5 unit and line
table headers, `.debug_rnglists` for a unit whose code is in several
sections, and `.debug_loclists` for location lists. `-gdwarf-4` writes
DWARF 4 instead, with `.debug_ranges` and `.debug_loc`, for an older
debugger. `-gdwarf-2` and `-gdwarf-3` also write DWARF 4. Any other
version is refused:

```text
embcc: error: -gdwarf-6: EmbCC writes DWARF 4 or 5
```

### `-gsplit-dwarf`

Writes most of the debug information to a second file, `FILE.dwo`
beside `FILE.o`, as GCC names it. The object keeps a skeleton unit
(`DW_TAG_skeleton_unit`, with `DW_AT_dwo_name` and `DW_AT_addr_base`),
the line table, the frame descriptions and `.debug_addr`. The `.dwo`
file holds the types, functions and variables in `.debug_info.dwo`,
`.debug_abbrev.dwo` and `.debug_loclists.dwo`, and it is never
relocated: every address in it is an index into the object's
`.debug_addr` (`DW_FORM_addrx`, `DW_OP_addrx`, `DW_LLE_base_addressx`).
The two units carry the same `DW_AT_dwo_id`. Split DWARF is DWARF 5, so
`-gsplit-dwarf` writes version 5 whatever `-gdwarf-N` says. The linker
does not read `.dwo` files. A debugger finds each one by its name in the
skeleton, which is the path given to `-o` with `.dwo` in place of `.o`.

### `-gz`, `-gz=zlib`, `-gz=none`

`-gz` compresses every debug section with zlib and marks it
`SHF_COMPRESSED`, behind an `Elf32_Chdr` or `Elf64_Chdr` header
(`ELFCOMPRESS_ZLIB`). Relocations still address the uncompressed bytes,
as the ELF specification says. EmbLD inflates a compressed debug section
from any compiler before merging it, and `llvm-objcopy
--decompress-debug-sections` gives back the bytes `-g` alone writes.
`-gz=none` turns compression off again. Other formats are refused:

```text
embcc: error: -gz=zstd: EmbCC compresses debug sections with zlib (-gz=zlib) or not at all (-gz=none)
```

### Options that are not accepted

`-g0`, `-gline-tables-only`, `-gcolumn-info`, `-gno-...` and
`-fdebug-prefix-map=OLD=NEW` are not recognized:

```text
embcc: error: unknown argument '-g0'
```

To turn debug information off, leave out `-g`. No prefix map is needed
for reproducible builds, because no directory is recorded.

## What the debug information describes

### Sections

An object compiled with `-g` has these DWARF sections, each with its
relocation section:

| Section | Contents |
|---|---|
| `.debug_abbrev` | Abbreviations for `.debug_info` |
| `.debug_info` | One compile unit: functions, parameters, local variables, types |
| `.debug_line` | The line-number table |
| `.debug_frame` | Call frame information, on Thumb and RISC-V (see [Call frames and unwinding](#call-frames-and-unwinding)) |

`.debug_ranges` is added when the unit's code is in several sections.
They are not allocated: they occupy no memory in the running program.
EmbCC emits no `.debug_str`, `.debug_aranges`, `.debug_loc` or
`.debug_macro`.

The 32-bit DWARF format is used. The address size is the target's
pointer size: 8 on x86-64, AArch64 and RV64, 4 on Thumb and RV32. AVR's
is 4, as avr-gcc's is, although its pointers are 2 bytes: a code address
there is a byte address in flash, and a data address is `0x800000` plus
the SRAM address (see [Targets](targets.md#debugging)).

### Compile unit

| Attribute | Value |
|---|---|
| `DW_AT_producer` | `EmbCC` |
| `DW_AT_language` | `DW_LANG_C99`, for C and for C++ (C++ is compiled through C) |
| `DW_AT_name` | The input file name as given on the command line |
| `DW_AT_comp_dir` | `.` |
| `DW_AT_low_pc`, `DW_AT_high_pc` | The unit's code |
| `DW_AT_stmt_list` | The line table |

### Line table

The line table maps each instruction address to a source line. Each
function's first row is at its entry address, with the line the
function is declared on; the first row of its body carries
`prologue_end`, which is where a debugger puts `break FUNCTION`. A new
row starts wherever the source line changes. Every row is a statement
boundary (`is_stmt`). Columns are not recorded.

The table has one file entry, the main source file. Code that comes from
a header, such as a `static inline` function, is attributed to the main
file with the header's line numbers, so a debugger shows the wrong
source text for it.

### Functions

Each function defined in the unit is a `DW_TAG_subprogram` with its
name, return type and address range. There is no
`DW_AT_external`, `DW_AT_decl_file` or `DW_AT_decl_line`, and no
description of inlined calls (`DW_TAG_inlined_subroutine`): code inlined
at `--opt=speed` is part of its caller.

The frame base, against which every variable's location is given:

| Target | `DW_AT_frame_base` |
|---|---|
| x86-64 | `DW_OP_reg6` (`rbp`) |
| AArch64 | `DW_OP_reg29` (`x29`) |
| Thumb | `DW_OP_breg13 0` (`sp`); `DW_OP_breg7 0` (`r7`) in a function with `alloca`, a variable-length array or an over-aligned local, which addresses its frame from `r7` |
| RISC-V | `DW_OP_breg2 0` (`sp`); `DW_OP_breg8 0` (`s0`) in such a function |
| AVR | `DW_OP_breg28 0` (`Y`, the pair `r28:r29`), which the prologue points one byte below the frame: a variable at `DW_OP_fbreg 5` is the one the code addresses as `Y+5` |

### Variables

Every parameter and every local variable of a function is described,
with its name, its type and a location. The location is a single offset
from the frame base (`DW_OP_fbreg`) when the variable lives in its
stack slot. When the optimized code keeps it in registers, the location
is a location list: the register or slot holding it over each range of
code (see [Optimized code](#optimized-code)).

An array or structure aligned beyond what the stack guarantees (for
example `char buf[64] __attribute__((aligned(64)))`) is stored in a
block carved from the stack at function entry, and its slot holds the
block's address. Its DWARF entry describes that slot, so its type is a
pointer to the declared type (`unsigned char (*)[64]`), and the
debugger shows the address; dereference it to see the contents.

Variables declared in inner blocks are listed directly under the
function, with no `DW_TAG_lexical_block`: the debugger sees all of a
function's variables at once, including ones whose block has not been
entered or has ended.

Each global variable the unit defines is a `DW_TAG_variable` child of
the compile unit, with its name, its type and a `DW_OP_addr` location
relocated against its own symbol. Thread-local variables, `static` local
variables and variables only declared (`extern`) are not described; a
`static` local is in the symbol table as `FUNCTION.NAME` (`f.cnt`), so
a debugger can find its address but not its type.

### Types

| Type | Described as |
|---|---|
| Integer and character types, `float`, `double`, `long double` | Base type with name, size and encoding |
| `_Bool` | Unsigned base type of size 1 |
| `_Complex` types | Complex-float base type |
| Pointers | Pointer to the described type |
| Arrays | Array with its upper bound; an array of unknown size or a variable-length array has none |
| `struct`, `union` | Name, size, and each member with its offset; bit-fields with bit offset and width |
| `enum` | Its underlying type: `int`, or the wider integer type an enumeration takes when a value does not fit in `int`; enumerators are not described |
| `typedef` names | The type they name; the typedef name itself is not described |
| `const`, `volatile` | Not described; the unqualified type is used |
| Pointers to functions | Pointer to `void` |

## Support by target and object format

| Target | `-g` | Notes |
|---|---|---|
| x86-64 (ELF) | Yes | Tested with gdb under QEMU. |
| AArch64 (ELF) | Yes | Tested with gdb under QEMU. |
| Thumb (Cortex-M) | Yes | |
| RISC-V, RV32 and RV64 | Yes | Tested with gdb under QEMU (RV32). See [Known problems](#known-problems). |
| AVR | Yes | Tested with gdb and with EmbDBG under QEMU (`-M uno`). |
| Any Darwin target (Mach-O) | No | Compiled without debug information, with a warning. |
| Any Windows target (COFF) | Refused | |

For a Darwin target, `-g` gives a warning and the object has no debug
information; the code is the same either way. Its DWARF belongs in a
`__DWARF` segment that EmbCC does not write yet:

```text
embcc: warning: -g: no debug information for aarch64-apple-darwin yet; d.c is compiled without it
```

For a Windows target the compilation stops:

```text
embcc: d.c: error: -g is not supported for a Windows target yet: its debug information goes in CodeView records this does not write, and emitting DWARF under a COFF name would be worse than refusing
```

A unit whose code is in more than one section, by
`--sections=functions` or `__attribute__((section("NAME")))`, gives its
compile unit a `DW_AT_ranges` list (in `.debug_ranges`) instead of one
`DW_AT_low_pc`..`DW_AT_high_pc` span, and each function's addresses are
relocated in its own section, so the debug information stays right
wherever the linker puts each one -- and when `--gc-sections` drops one,
its addresses resolve to 0, as with GNU ld.

With `-S`, the assembly output contains no debug information, with or
without `-g`.

## Call frames and unwinding

A debugger finds a function's caller in one of these ways, depending on
the target:

- **Thumb and RISC-V: `.debug_frame`.** With `-g`, every function has
  call frame information in `.debug_frame`: where the caller's frame is,
  and where the return address and each saved register went, step by
  step through the prologue. On Thumb that is `push`, `vpush`, the
  frame's `sub sp`, and `r7` as the frame base in a function with
  `alloca` or a variable-length array (`r5` on ARMv6-M). On RISC-V it is the `addi sp`, the
  stores of `ra` and the saved `s` and `fs` registers, and `s0` as the
  frame base. A debugger unwinds by it at every level, through
  floating-point frames too, and reads a caller's saved registers back,
  float registers included. `tests/golden/debug-frame.sh` backtraces
  through such frames with gdb. A RISC-V interrupt handler has no
  entry.
- **x86-64 and AArch64: frame records.** At `--opt=none` and `--opt=debug` every
  function keeps a frame record (`rbp`, or `x29` and `x30`), so the chain
  of frame pointers leads from each frame to its caller. Optimized, a
  leaf may have no frame record; the debugger then analyzes the
  function's prologue.
- **`.eh_frame`.** When unwind tables are enabled, each function has an
  `.eh_frame` entry describing its frame, which a debugger also uses.
  Unwind tables are on for C++, with `--unwind-tables`,
  `-fasynchronous-unwind-tables` or `--exceptions`, and by default for a
  Linux target. `--no-unwind-tables` and
  `-fno-asynchronous-unwind-tables` turn them off. They describe frames
  correctly only on x86-64 and AArch64 (see
  [Known problems](#known-problems)).
- **The other embedded targets.** No `.debug_frame` yet; the debugger
  analyzes the function's prologue.
- **AVR.** `Y` is a frame pointer, but nothing records where the return
  address is above it; gdb analyzes the prologue, which follows
  avr-gcc's.

## Frame pointers

`--no-frame-pointer` and `--frame-pointer` are accepted and
have no effect.

| Target | Without `-g` | With `-g` |
|---|---|---|
| x86-64 | `rbp` frame in every function at `--opt=none`; from `--opt=basic`, frameless leaf functions and push-only frames have none | `rbp` frame in every function |
| AArch64 | `x29`/`x30` frame record at `--opt=none`; from `--opt=basic`, frameless leaf functions have none | Frame record in every function |
| Thumb | None (`r7` only in a function with `alloca`, a variable-length array, or a local aligned beyond 8 bytes) | Same |
| RISC-V | None (`s0` only in a function with `alloca`, a variable-length array, or a local aligned beyond 16 bytes) | Same |

See [Optimization](optimization.md#frame-pointer) for AVR.

## `-g` does not change the generated code

The code, data, symbols and relocations of an object are the same with
and without `-g`, at every optimization level and on every target. `-g`
only adds the debug sections. So the image you debug is the image you
ship: build once with `-g`, flash the stripped copy, and debug it with
the unstripped ELF.

`tests/golden/g-same-code.sh` checks this. It compiles programs for
every target at `--opt=none`, `--opt=basic`, `--opt=speed`, `--opt=size` and `--opt=debug`, with and without
`-g`, and requires the two objects to be identical once their debug
sections are stripped.

## Optimized code

At `--opt=none` and `--opt=debug`, every variable lives in its stack slot for its whole
lifetime, and the debug information names that slot. `--opt=debug` is `--opt=basic`
with this one difference: the optimizer still removes work, but every
source variable stays in memory, where a debugger reads it.

At `--opt=basic` and above, the optimizer takes most variables out of
memory. Each value such a variable takes is followed into the temporary
that holds it, and the temporary into its register or slot, so the
variable's location is a location list. Each range of code where the
value is live gives one entry: `DW_OP_reg5 RDI` over the loop,
`DW_OP_fbreg -24` where it was spilled. A value in two registers (a
64-bit value on a 32-bit machine) is `DW_OP_piece`s of both, and on AVR
each byte is its own register. Outside those ranges the debugger prints
`<optimized out>`. In particular, that is the case after the variable's
last use, where its register may already hold something else.

Variables that remain in their slot have that slot's location, which is
always right:

- A local the code still reads or writes in memory is in its slot.
- A parameter that has a slot is stored there on entry, so the slot
  holds the value it arrived with. If the optimizer takes it out of
  memory and the function then assigns it, it gets a location list as
  any other variable.
- A `volatile` local is the exception at every level: it is never
  promoted, forwarded or given a register, so its slot always holds its
  current value.

Where two of a variable's values are live at once, because the old one
is still read after the new one is made, the list names the newer one.
A value the optimizer computes without keeping it (folded into an
address, or into a vector register) is not followed there: the list
names the register that last held it as a scalar, or nothing.

Also, at `--opt=basic` and above:

- Inlined functions have no frame of their own: a backtrace shows the
  caller, and the line table moves between the caller's lines and the
  inlined function's lines.
- Tail calls replace the caller's frame with the callee's, so the caller
  is missing from a backtrace.
- Loops may be rotated, unrolled or vectorized, so stepping visits the
  loop's lines in an order that does not match the source.

Line-table breakpoints and backtraces remain usable in optimized code.
For variable values, debug at `--opt=none` or `--opt=debug`, or use
[`embcc inspect ir`](optimization.md#seeing-what-the-optimizer-did)
to see what became of a variable.

## Debug information in a linked image

EmbLD merges the debug sections of its input objects into the output:

- Each `.debug_*` section of every input object, including archive
  members, is appended to the output section of the same name, and the
  relocations into it are resolved. The result is one `.debug_info`,
  one `.debug_abbrev` and one `.debug_line`, containing one compile unit
  per object that had debug information.
- The debug sections are not allocated and lie outside every loadable
  segment, so they take no flash and no RAM. The symbol table is kept in
  the image in the same way.
- EmbLD also writes a native debug file, `OUTPUT.embdbg`, built from the
  objects named on its command line that have a line table (archive
  members are not included). It reports this as:

  ```text
  embld: wrote fw.elf.embdbg (debug info from 1 object)
  ```

  No `.embdbg` file is written when no input has debug information. This
  is the split recorded in design decision D-010: the compiler emits
  relocatable DWARF, and the linker, which knows the final addresses,
  produces the absolute-addressed native form that EmbDBG reads.

EmbLD has no option to strip debug information (`-s`, `-S` and
`--strip-debug` are refused as unknown options). Given to the driver as
`-Wl,-s` or `-Wl,--strip-debug`, they are accepted and change nothing:
the debug sections stay in the image. Strip a finished image
with a separate tool if needed, for example
`llvm-objcopy --strip-debug fw.elf`. An image converted to a raw binary
for flashing does not contain the debug sections in any case.

## Using a debugger

The DWARF EmbCC emits is read by gdb and lldb. The test suite checks it
with `llvm-dwarfdump --verify` and with gdb sessions against QEMU on
x86-64, AArch64, RISC-V and AVR.

### A program on the host

For a Linux target, compile and link with `-g` and run the debugger on
the executable:

```sh
embcc --target=x86_64-linux-gnu -g --opt=none prog.c -o prog
gdb ./prog
```

Darwin and Windows targets do not support `-g`.

### A board under QEMU

QEMU's gdb stub lets a debugger control a bare-metal image. Start QEMU
halted (`-S`) with the stub listening on a TCP port (`-gdb tcp::PORT`),
then connect:

```sh
embcc --target=riscv32-unknown-elf -g --opt=none -c prog.c -o prog.o
# link prog.o with startup code into prog.elf (see Embedded programming)
qemu-system-riscv32 -M virt -bios none -nographic -kernel prog.elf -S -gdb tcp::3333 &
gdb prog.elf -ex 'set architecture riscv:rv32' -ex 'target remote :3333'
```

Then use gdb as usual: `break scale`, `continue`, `info args`, `next`,
`info locals`, `bt`.

<!-- UNVERIFIED: this session (gdb 17.2, QEMU 11.1.1, macOS) could load the
     image, resolve `break scale` to img2.c line 3 and connect, but after
     `continue` gdb answered "Cannot execute this command while the target
     is running" to the next command. The commands above are the ones
     tests/golden/link-dwarf.sh and debug-live.sh use; the lead may want
     to confirm that those tests pass with this gdb/QEMU pair. -->

| Target | QEMU used by the test suite | gdb `set architecture` |
|---|---|---|
| x86-64 | `qemu-system-x86_64` | `i386:x86-64` |
| AArch64 | `qemu-system-aarch64 -M virt -cpu cortex-a72` | `aarch64` |
| Thumb, Cortex-M3 | `qemu-system-arm -M lm3s6965evb -cpu cortex-m3` | `arm` |
| Thumb, Cortex-M4F | `qemu-system-arm -M mps2-an386` | `arm` |
| Thumb, Cortex-M33 | `qemu-system-arm -M mps2-an505` | `arm` |
| RISC-V | `qemu-system-riscv32` / `qemu-system-riscv64 -M virt -bios none` | `riscv:rv32` / `riscv:rv64` |
| AVR | `qemu-system-avr -M uno -bios IMAGE` | `avr` |

On x86-64, the test suite uses hardware breakpoints (`hbreak`), because
the boot loader writes the code after the debugger has attached and
would overwrite a software breakpoint.

For a real board, an OpenOCD or probe gdb server takes QEMU's place:
connect to its port with `target remote`.

How to build and run images for each board is described in
[Embedded programming](embedded.md).

### EmbDBG

EmbDBG is EmbCC's own debugger. It reads the DWARF in an image, or the
`.embdbg` file EmbLD writes, without gdb: it lists functions and line
tables, symbolizes addresses, prints backtraces and source context with
the variables in scope, analyzes crash dumps, and converts DWARF to the
`.embdbg` form. Its `remote` command speaks the gdb remote protocol, so
it can drive QEMU's gdb stub or OpenOCD directly:

```sh
qemu-system-riscv64 -M virt -bios none -nographic -m 8 -kernel fw.elf -S -gdb tcp::3333 &
embdbg fw.elf remote :3333
```

`remote` supports x86-64, AArch64, Thumb, RV32, RV64 and AVR. See
[EmbDBG](tools/embdbg.md) for its commands.

## Known problems

These are defects in the current implementation, not intended behavior.

- **Unwind tables on Thumb, RISC-V and AVR.** `--unwind-tables` (and C++
  at RV64, the one of these targets that compiles C++) produce an
  `.eh_frame` in the x86-64 layout on these targets, which does not
  describe their frames. Do not rely on it for unwinding. On MIPS32 the
  options are refused by name instead.
- **Header files.** Code from a header is attributed to the main source
  file, as described in [Line table](#line-table).

