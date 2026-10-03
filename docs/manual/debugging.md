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

`-g` can be combined with any `-O` level. See
[Optimized code](#optimized-code) for what a debugger can rely on above
`-O0`.

### `-ggdb`, `-g1`, `-g2`, `-g3`, `-gdwarf`, `-gdwarf-2`, `-gdwarf-3`, `-gdwarf-4`

Each of these is the same as `-g`. EmbCC emits one kind of debug
information, DWARF 4, at one level of detail, so a level or a version
that asks for no more than that is accepted. In particular `-g1` does
not limit the output to line tables, and `-g3` does not add macro
information.

### `-gdwarf-5`, `-gsplit-dwarf`, `-gz`

Refused, because EmbCC cannot produce what they ask for:

```text
embcc: error: -gdwarf-5 is not supported; EmbCC emits DWARF 4, uncompressed and in one piece
```

Any other `-gdwarf-N` gets the same message.

### Options that are not accepted

`-g0`, `-gline-tables-only`, `-gcolumn-info`, `-gno-...`, `-gz=...`
and `-fdebug-prefix-map=OLD=NEW` are not recognized:

```text
embcc: error: unknown argument '-g0'
```

To turn debug information off, leave out `-g`. No prefix map is needed
for reproducible builds, because no directory is recorded.

## What the debug information describes

### Sections

An object compiled with `-g` has three DWARF sections, each with its
relocation section:

| Section | Contents |
|---|---|
| `.debug_abbrev` | Abbreviations for `.debug_info` |
| `.debug_info` | One compile unit: functions, parameters, local variables, types |
| `.debug_line` | The line-number table |

They are not allocated: they occupy no memory in the running program.
EmbCC emits no `.debug_str`, `.debug_aranges`, `.debug_ranges`,
`.debug_loc`, `.debug_frame` or `.debug_macro`. Call-frame information,
when there is any, is in `.eh_frame`; see
[Call frames and unwinding](#call-frames-and-unwinding).

The 32-bit DWARF format is used. The address size is the target's
pointer size: 8 on x86-64, AArch64 and RV64, 4 on Thumb and RV32.

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

The line table maps each instruction address to a source line. A new
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
at `-O2` is part of its caller.

The frame base, against which every variable's location is given:

| Target | `DW_AT_frame_base` |
|---|---|
| x86-64 | `DW_OP_reg6` (`rbp`) |
| AArch64 | `DW_OP_reg29` (`x29`) |
| Thumb | `DW_OP_breg13 0` (`sp`); `DW_OP_breg7 0` (`r7`) in a function with `alloca`, a variable-length array or an over-aligned local, which addresses its frame from `r7` |
| RISC-V | `DW_OP_breg2 0` (`sp`); `DW_OP_breg8 0` (`s0`) in such a function |

### Variables

Every parameter and every local variable of a function is described,
with its name, its type and a location that is a single offset from the
frame base (`DW_OP_fbreg`). There are no location lists and no
register locations.

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

Global variables and `static` local variables are not described. A
debugger can still find a global by its symbol, but does not know its
type.

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
| AVR | Accepted, not usable | No line table rows and an unreadable `.debug_info`. |
| Any Darwin target (Mach-O) | Refused | |
| Any Windows target (COFF) | Refused | |

For Darwin and Windows targets the compilation stops:

```text
embcc: d.c: error: -g is not supported for a Darwin target yet: its DWARF goes in a __DWARF segment this does not write, and emitting the ELF layout under a Mach-O name would be worse than refusing
embcc: d.c: error: -g is not supported for a Windows target yet: its debug information goes in CodeView records this does not write, and emitting DWARF under a COFF name would be worse than refusing
```

`-g` is also refused for a unit that places a function in a section of
its own with `__attribute__((section("NAME")))`:

```text
embcc: sec.c: error: -g with a function in a section of its own ('.fast') is not supported yet: the compile unit's address range would span two sections
```

With `-S`, the assembly output contains no debug information, with or
without `-g`.

## Call frames and unwinding

EmbCC emits no `.debug_frame`. A debugger finds a function's caller in
one of three ways, depending on the target:

- **x86-64 and AArch64.** With `-g`, every function keeps a frame record
  (`rbp`, or `x29` and `x30`), so the chain of frame pointers leads from
  each frame to its caller.
- **`.eh_frame`.** When unwind tables are enabled, each function has an
  `.eh_frame` entry describing its frame, which a debugger also uses.
  Unwind tables are on for C++, with `-funwind-tables`,
  `-fasynchronous-unwind-tables` or `-fexceptions`, and by default for a
  Linux target. `-fno-unwind-tables` and
  `-fno-asynchronous-unwind-tables` turn them off. They describe frames
  correctly only on x86-64 and AArch64 (see
  [Known problems](#known-problems)).
- **Thumb and RISC-V.** No frame pointer and no correct `.eh_frame`; the
  debugger analyzes the function's prologue. gdb does this for both.

## Frame pointers

`-fomit-frame-pointer` and `-fno-omit-frame-pointer` are accepted and
have no effect.

| Target | Without `-g` | With `-g` |
|---|---|---|
| x86-64 | `rbp` frame in every function at `-O0` and `-O1`; at `-O2`, frameless leaf functions and push-only frames have none | `rbp` frame in every function |
| AArch64 | `x29`/`x30` frame record at `-O0` and `-O1`; at `-O2`, frameless leaf functions have none | Frame record in every function |
| Thumb | None (`r7` only in a function with `alloca`, a variable-length array, or a local aligned beyond 8 bytes) | Same |
| RISC-V | None (`s0` only in a function with `alloca`, a variable-length array, or a local aligned beyond 16 bytes) | Same |

See [Optimization](optimization.md#frame-pointer) for AVR.

## How `-g` changes the generated code

EmbCC does not generate the same code with and without `-g`. To keep
each variable at the location its DWARF names, `-g` turns off the
following:

| Target | Turned off by `-g` |
|---|---|
| x86-64 | Sharing one stack slot between variables that are not live at the same time (all levels); frameless and push-only functions; moving parameters straight into allocated registers (`-O2`). |
| AArch64 | Frameless leaf functions and tail calls (`-O2`). |
| Thumb | Choosing between register-pair and no-pair allocation, folding constant offsets into loads and stores, and tail calls (`-O2`). Variables the optimizer leaves in memory are not given registers. |
| RISC-V | Tail calls (`-O2`). Variables the optimizer leaves in memory are not given registers. |
| AVR | Register allocation, entirely (`-O2`). |

On every target, a variable that is never referenced still gets a stack
slot under `-g`.

The difference is largest on AVR, where `-O2 -g` code can be about twice
the size of `-O2` code, and on x86-64 at `-O0`, where every variable gets
its own slot. Measure size and speed without `-g`.

## Optimized code

At `-O0`, every variable lives in its stack slot for its whole lifetime,
each statement is a separate sequence of instructions, and the debug
information is accurate. Debug at `-O0` when you can. EmbCC has no
`-Og`.

At `-O1` and above, the debug information still describes every
variable at its stack slot, but the optimizer no longer keeps every
value there:

- At `-O2` and `-Os`, `mem2reg` moves local variables into registers.
  The variable's slot is then never written, and the debugger shows
  whatever the slot holds: usually a stale or uninitialized value.
  There is no "optimized out" marker.
- Parameters are stored to their slots on entry to the function, so the
  debugger shows each parameter's value at entry, even after the
  function has changed it.
- At `-O1`, a value stored to a local variable and read back in the same
  basic block is forwarded without the reload, and the store is then
  usually removed, so the slot can hold an old value.
- A `volatile` local is the exception at every level: it is never
  promoted, forwarded or given a register, so its slot always holds its
  current value.
- Inlined functions have no frame of their own: a backtrace shows the
  caller, and the line table moves between the caller's lines and the
  inlined function's lines.
- Tail calls (x86-64 at `-O2`, even with `-g`) replace the caller's frame
  with the callee's, so the caller is missing from a backtrace.
- Loops may be rotated, unrolled or vectorized, so stepping visits the
  loop's lines in an order that does not match the source.

Line-table breakpoints and backtraces remain usable in optimized code;
variable values do not. Use [`embcc inspect ir`](optimization.md#seeing-what-the-optimizer-did)
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
x86-64, AArch64 and RISC-V.

### A program on the host

For a Linux target, compile and link with `-g` and run the debugger on
the executable:

```sh
embcc --target=x86_64-linux-gnu -g -O0 prog.c -o prog
gdb ./prog
```

Darwin and Windows targets do not support `-g`.

### A board under QEMU

QEMU's gdb stub lets a debugger control a bare-metal image. Start QEMU
halted (`-S`) with the stub listening on a TCP port (`-gdb tcp::PORT`),
then connect:

```sh
embcc --target=riscv32-unknown-elf -g -O0 -c prog.c -o prog.o
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

`remote` supports x86-64, AArch64, Thumb, RV32 and RV64, not AVR. See
[EmbDBG](tools/embdbg.md) for its commands.

## Known problems

These are defects in the current implementation, not intended behavior.

- **AVR.** `-g` is accepted, but no line-table rows are emitted, the
  frame base is wrong, and `.debug_info` declares a 2-byte address size
  while writing 8-byte addresses, so debuggers cannot read past the
  compile unit. `-g` also turns off register allocation at `-O2`.
- **Unwind tables on Thumb, RISC-V and AVR.** `-funwind-tables` (and C++
  at RV64, the one of these targets that compiles C++) produce an
  `.eh_frame` in the x86-64 layout on these targets, which does not
  describe their frames. Do not rely on it for unwinding.
- **Header files.** Code from a header is attributed to the main source
  file, as described in [Line table](#line-table).

