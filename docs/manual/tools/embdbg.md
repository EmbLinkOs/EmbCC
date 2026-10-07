# embdbg — the EmbCC debugger

This page is the reference for `embdbg`, EmbCC's debugger. It is for
people who need to answer "where is this address, and what was in scope
there" for code EmbCC compiled: symbolizing a crash address or a
backtrace, browsing functions and their variables, analyzing a kernel
fault dump, and debugging a running firmware image through the GDB remote
protocol (QEMU's `-gdb` stub, or OpenOCD on real hardware). It reads the
DWARF that `embcc -g` emits and EmbCC's native `.embdbg` format; it does
not need `gdb`. For compiling with debug information and for using other
debuggers, see [Debugging](../debugging.md).

## NAME

`embdbg` — symbolize addresses, inspect debug information, analyze crash
dumps, and debug a live target

## SYNOPSIS

```text
embdbg FILE funcs
embdbg FILE lines
embdbg FILE symbolize ADDR...
embdbg FILE backtrace ADDR...
embdbg FILE where ADDR
embdbg FILE list ADDR
embdbg FILE info FUNCTION
embdbg FILE disassemble FUNCTION|ADDR [LENGTH]
embdbg FILE crash REPORT
embdbg FILE tui [REPORT]
embdbg FILE remote [HOST:]PORT
embdbg FILE emit OUT.embdbg
embdbg FILE emit-kernel OUT.embdbg
embdbg FILE.embdbg verify
```

Run `embdbg` with fewer than two arguments to print the usage summary.

## DESCRIPTION

`FILE` is either an ELF file — a relocatable object or a linked
executable — or a native `.embdbg` file. The kind is recognized from the
file's first bytes.

From an ELF file, `embdbg` reads:

- the symbol table (`.symtab`): every `STT_FUNC` symbol with a nonzero
  size is a function, with the Thumb bit removed from ARM addresses;
- the line table (`.debug_line`);
- the functions, parameters, local variables, global variables and their
  types from `.debug_info` and `.debug_abbrev`, as EmbCC's DWARF 4 writer
  emits them: each local variable has a location relative to its
  function's frame base, each global an address, and base types and
  pointers are named. A linked image has one compile unit per object
  compiled with `-g`, and every one is read. Units in DWARF 5 (from
  another compiler) are skipped;
- `.text`, for disassembly.

In a relocatable object, code addresses are offsets into `.text`, and the
relocations in `.rela.debug_line` and `.rela.debug_info` are applied to
recover them. In a linked image, addresses are absolute. Addresses are
read at the size the unit's header gives: 8 bytes on the 64-bit targets,
4 on the 32-bit ones and on AVR.

A `.embdbg` file holds the same functions, line table, variables and
types, with absolute addresses, but no machine code: the commands that
disassemble need the ELF file. [`embld`](embld.md) writes one beside every
image it links from objects compiled with `-g`, and the `emit` command
below writes one from any ELF file.

Addresses on the command line are read in C notation: `0x` for
hexadecimal, otherwise decimal.

Source lines are shown by opening the file name recorded in the debug
information (the path given on the compiler's command line), relative to
the current directory. When the file cannot be opened, `embdbg` says
`(source 'PATH' not reachable from here)` and continues.

Variables are shown with their kind (`param` or `local`), their type,
their name, and their frame slot: an offset from the function's frame
base, labeled with the register the function's `DW_AT_frame_base` names:
`rbp` on x86-64, `x29` on AArch64, `sp` on ARM and RISC-V (`r7` or `s0`
in a function with `alloca`), and `Y` (`r28:r29`) on AVR. A `.embdbg`
file does not record the frame base, and its slots are labeled `rbp`.
A variable the compiler marks as optimized out (an empty location, which
EmbCC writes on AVR) is not listed.

### Target support

| Image | Symbol table (`funcs`, function in `symbolize`) | Lines and variables | `disassemble`, crash disassembly | `remote` |
|---|---|---|---|---|
| x86-64 (ELF64) | yes | yes | yes | yes |
| AArch64 (ELF64) | yes | yes | no | yes |
| RISC-V 64 (ELF64) | yes | yes | no | yes |
| RISC-V 32 (ELF32) | yes | yes | no | yes |
| ARM, Thumb (ELF32) | yes | yes | no | yes |
| AVR (ELF32) | yes | yes | no | yes |

The disassembler decodes x86-64 only.

## COMMANDS

### `funcs`

List the functions: name, start address and size in bytes. From an ELF
file these come from the symbol table; from a `.embdbg` file, from its
function table.

```text
FUNCTION                 ADDR       SIZE
compute                  0x0        133
main                     0x90       224
```

### `lines`

Print the decoded line table: address, file and line for each row. A row
marked `(end)` is the end of a sequence of addresses.

### `symbolize ADDR...`

For each address, print the function and offset it falls in and its
source file and line. `??` means no function contains the address, and
`(no line info)` that the line table does not cover it.

```text
0x4001c0  main+0x10  hello.c:6
```

### `backtrace ADDR...`

Symbolize a list of return addresses, numbering them as frames `#0`,
`#1`, and so on. Pass the addresses a fault handler collected, innermost
first.

### `where ADDR`

Symbolize `ADDR`, print the five source lines around it with the current
line marked `->`, and list the parameters and local variables of the
function that contains it. `(no scope info here)` means no function with
debug information contains the address.

```text
0x13  compute+0x13  fw.c:4
        2 | static int acc = 5;
        3 | int buf[16];
  ->    4 | int compute(int a, int b) { int t = a * b; acc += t; return t + 1; }
        5 | int main(void) { for (int i = 0; i < 3; i++) putn(compute(i, i + 2)); ...
  in compute — 3 variable(s) in scope:
    param int            a        @ rbp-8
    param int            b        @ rbp-16
    local int            t        @ rbp-24
```

### `list ADDR`

Symbolize `ADDR` and print the five source lines around it.

### `info FUNCTION`

Print a function's address range and its parameters and local variables.
If no function of that name has debug information, `embdbg` prints
`embdbg: no function 'NAME' with debug info`.

### `disassemble FUNCTION|ADDR [LENGTH]`

Disassemble x86-64 code in AT&T syntax, interleaved with the source line
each run of instructions belongs to. Given a function name, the whole
function is shown; given an address, `LENGTH` bytes (default 32) from
that address. Bytes that cannot be decoded are shown as `.byte`.

Addresses are taken as offsets into the file's `.text` section, which is
what they are in a relocatable object. Disassemble the `.o` file rather
than the linked image: for an image linked at a nonzero address, a
function name produces only its heading. A `.embdbg` file holds no code
and is refused with
`embdbg: no .text to disassemble (need the object/ELF, not the .embdbg)`.

### `crash REPORT`

Analyze an x86-64 fault dump without a live target. `REPORT` is a text
file with one item per line:

| Line | Meaning |
|---|---|
| `exception NAME` | the exception, for example `PAGE_FAULT` |
| `fault ADDR` | the faulting address (`cr2` for a page fault); optional |
| `reg NAME VALUE` | a register value, for example `reg rip 0x401a2c`; up to 40 |
| `mem ADDR VALUE` | a 64-bit stack word at `ADDR`; up to 256 |

Lines beginning with `#` are comments. Numbers are in C notation.

The analysis prints the exception and faulting address, `rip`
symbolized, the registers, a backtrace, the variables in scope in the
faulting function, and the instructions around `rip` with the faulting
one marked `->`. The backtrace walks the frame-pointer chain through the
`mem` words: the return address at `rbp+8` and the caller's `rbp` at
`rbp`. It stops when a word is missing, when a return address lies in no
known function, or when the chain does not move up the stack. For it to
reach the caller, the report must include the words of every frame
record the handler walked.

```text
=== EmbDBG Crash Analysis ===
Exception: PAGE_FAULT    faulting address 0x0
RIP: 0x13  compute+0x13  fw.c:4

Registers:
  rip  0x0000000000000013  rsp  0x0000000000007f00  rbp  0x0000000000007f40
  rax  0x0000000000000000

Backtrace:
  #0  0x13  compute+0x13  fw.c:4
  #1  0xb5  main+0x25  fw.c:5
...
```

(Output shortened; the variables and the instructions near `rip`
follow.)

### `tui [REPORT]`

Open an interactive browser in the terminal. The screen has three
columns: on the left, the list of functions (or, with a crash report, the
call stack); in the center, the source of the selected function above
its disassembly; on the right, registers above the selected function's
variables. Registers and variable values come from `REPORT`, a crash
report as described under `crash`; without one, the browser shows the
debug information only. Disassembly needs an ELF file.

| Key | Action |
|---|---|
| `j`, Down | select the next entry |
| `k`, Up | select the previous entry |
| `g`, `G` | select the first or the last entry |
| `/` | filter the function list by a substring; Enter or Esc ends the input |
| `:` | open the command line: a function name jumps to it, `0xADDR` jumps to the function containing that address, `q` quits; Esc cancels |
| Tab | switch between the call stack and the function list (with a crash report) |
| `q` | quit |

When standard input or standard output is not a terminal, `tui` prints a
plain report instead: for each function, its source and its variables.
This makes the command usable in scripts and tests.

### `remote [HOST:]PORT`

Connect to a GDB remote-protocol stub at `HOST:PORT` (or at `localhost`
when only a port is given) and debug the running target with the symbols
and debug information of `FILE`. The register layout is chosen from
`FILE`'s ELF header: x86-64, AArch64, ARM (M-profile), RV32, RV64 or
AVR. `FILE` must be the ELF image, not a `.embdbg` file. If
the connection fails, `embdbg` prints
`embdbg: cannot reach a gdb stub at HOST:PORT` and exits with status 1.

On connecting, `embdbg` prints `connected to HOST:PORT — ARCH target`,
asks the stub why the target stopped, and shows where it is. It then
reads commands from standard input, one per line, and runs until `quit`
or end of input. Blank lines and lines beginning with `#` are ignored, so
a session can be scripted with a here-document.

| Command | Action |
|---|---|
| `break LOCATION`, `b LOCATION` | set a software breakpoint. `LOCATION` is a function name, `FILE:LINE`, or `*ADDR`. A function name resolves to the first line-table row after the function's entry, past its prologue. `FILE` is matched against the end of the recorded file name, so `main.c:12` finds `src/main.c` |
| `delete` | remove every breakpoint |
| `continue`, `c` | resume until the target stops, then show where it is |
| `step`, `s` | single-step instructions until the source line changes (at most 20000 instructions), then show where the target is |
| `stepi`, `si` | execute one instruction, then show where the target is |
| `where`, `w` | show the stop address symbolized, the source around it, and the variables in scope |
| `bt` | show a backtrace (see below) |
| `regs` | show the registers |
| `set REG VALUE` | write a register, named as `regs` shows it; prints `REG = 0xVALUE` |
| `print NAME`, `p NAME` | read a variable and print `NAME = VALUE`: a parameter or local of the function the target is stopped in, else a global. An optimized-out variable prints `NAME = <optimized out>` |
| `locals`, `info locals` | print every parameter and local of the current function, as `print` does |
| `mem ADDR [LENGTH]` | dump `LENGTH` bytes of target memory (default 32, at most 512), 16 per line |
| `quit`, `q` | detach and exit; the target continues to run |

`where` lists variables with their frame slots; `print` and `locals`
read their values. A local is read at the frame base (the register
`DW_AT_frame_base` names, read from the stub, plus that attribute's
offset) plus the variable's offset; a global at its `DW_OP_addr`. The
value is printed by its type: a signed or unsigned integer, a character
as its number, a `float` or `double`, a pointer in hexadecimal, and a
structure, union or array as its bytes in hexadecimal.

The registers `regs` shows are: `rax` to `r15`, `rip` and
`eflags` on x86-64; `x0` to `x8`, `x19` to `x21`, `x29`, `x30`, `sp`, `pc`
and `cpsr` on AArch64; `r0` to `r12`, `sp`, `lr`, `pc` and `xpsr` on ARM;
the 32 integer registers by ABI name and `pc` on RISC-V; `r0` to `r31`,
`sreg`, `sp` and `pc` on AVR. `set` writes the whole register block back
with the protocol's `G` packet, so any register `regs` shows can be
written.

On AVR the layout is QEMU's (and GDB's): `r0` to `r31` one byte each,
`sreg` one byte, `sp` two bytes and `pc` four, 39 bytes in all; `pc` is a
byte address, the same number as an ELF symbol's value. The stub reads
data memory at `0x800000` plus the SRAM address, and flash below that:
`embdbg` adds `0x800000` to a frame address (`Y` plus an offset), and a
global's DWARF address already includes it. Give `mem` the `0x800000`
form too.

`bt` on x86-64 follows the frame-pointer chain in target memory. On the
other machines EmbCC's code keeps no frame pointer and `-g` emits no
`.debug_frame`, so `bt` shows frame `#0` and, as frame `#1`, the address
in the return-address register (`lr` or `ra`), which is the caller only
until the function's prologue has saved it. AVR has no return-address
register, and `bt` there shows frame `#0` only.

The breakpoint length given to the stub is 1 byte on x86-64, 2 on ARM
and AVR, and 4 on the other machines. When `continue` or `step` starts
with the target stopped on one of its breakpoints, `embdbg` removes that
breakpoint, steps one instruction and puts it back first: otherwise a
stub can stop at the same breakpoint again at once, and QEMU's AVR stub
does. Messages a session can print include
`embdbg: cannot resolve 'LOCATION'`,
`embdbg: no variable 'NAME' here`, `embdbg: could not write REG`,
`embdbg: this stub has no software breakpoints`,
`embdbg: the stub refused a breakpoint at 0xADDR`, `the target exited`
(the program ended), `the target has exited` (a later run command), and
`embdbg: lost the target` (the connection failed; the session ends).

For QEMU, start the machine halted with its stub listening, so that
breakpoints can be set before the first instruction runs:

```sh
qemu-system-riscv64 -M virt -bios none -nographic -m 8 \
    -kernel fw.elf -S -gdb tcp::1234 &
```

Do not probe the port with another client first: disconnecting detaches,
and the target then runs.

### `emit OUT.embdbg`

Write the debug information read from the ELF `FILE` as a `.embdbg` file.
Its build ID is the SHA-256 of `FILE`. The input must be an ELF file
(`embdbg: input is already .embdbg` otherwise).

### `emit-kernel OUT.embdbg`

Write a `.embdbg` file for an ELF image whose DWARF `embdbg` cannot read
itself, such as a kernel built by GCC with DWARF 5. Functions come from
the symbol table and the line table from the output of
`readelf --debug-dump=decodedline`; no variables or types are written.
The `readelf` program is taken from the `READELF` environment variable,
or is `readelf`. When that program is missing the file is written with no
line rows, so check the count `embdbg` reports on standard error:

```text
embdbg: kernel .embdbg — N funcs, M line rows
```

### `verify`

Check a `.embdbg` file: its magic number, the CRC32C of its header and of
each section. Prints one line per check, the build ID, and `verify OK`;
or `VERIFY FAILED` and exits with status 1. `FILE` must be a `.embdbg`
file (`embdbg: verify needs a .embdbg file` otherwise).

```text
magic            OK
header_checksum  OK
section kind 1   OK
...
section kind 7   OK
build_id         e7f9234aa6faa8d49d779954207b88f0b7c980e04196b578590ac2ca768b5f48
verify OK
```

## THE .embdbg FORMAT

A `.embdbg` file begins with the eight bytes `7f 45 4d 44 42 47 0a 1a`
(`\x7fEMDBG\n\x1a`), followed by a header with a SHA-256 build ID of the
image it describes, a CRC32C of the header, and a table of sections. The
sections are, by kind number: 1 strings, 2 files, 3 line table,
4 functions, 5 frame information, 6 variables, 7 types. Each section has
its own CRC32C. All values are little-endian.

## EXIT STATUS

| Status | Meaning |
|---|---|
| 0 | the command completed (including `info` for a function without debug information, and a `remote` session that ended) |
| 1 | a usage error, an unknown command (`embdbg: unknown command 'NAME'`), a file that cannot be opened (`embdbg: cannot open file`) or is not ELF (`embdbg: not an ELF file`), a missing argument, a `verify` failure, or a failed `remote` connection |

## ENVIRONMENT

`READELF`
: The `readelf` program `emit-kernel` runs. Default: `readelf`.

## EXAMPLES

Symbolize the addresses of a crash backtrace against the image:

```sh
embdbg kernel.elf backtrace 0xffffffff80104a2c 0xffffffff80101f10
```

Show the variables in scope at an address in an object file:

```sh
embcc -g -c fw.c -o fw.o
embdbg fw.o where 0x13
```

Break in a function of a RISC-V firmware image running in QEMU, then look
around:

```sh
qemu-system-riscv64 -M virt -bios none -nographic -m 8 \
    -kernel fw.elf -S -gdb tcp::1234 &
embdbg fw.elf remote 1234 <<'EOF'
break compute
continue
regs
bt
quit
EOF
```

```text
connected to localhost:1234 — riscv64 target
stopped at 0x1000  ??
breakpoint 1 at 0x80000326  compute+0x6  fw.c:4
stopped at 0x80000326  compute+0x6  fw.c:4
        3 | int buf[16];
  ->    4 | int compute(int a, int b) { int t = a * b; acc += t; return t + 1; }
        5 | int main(void) { for (int i = 0; i < 3; i++) putn(compute(i, i + 2)); ...
  in compute — 3 variable(s) in scope:
    param int            a        @ s0+32
    param int            b        @ s0+40
    local int            t        @ s0+48
zero  0000000000000000  ra    00000000800003c2  sp    00000000807ffec0  gp    0000000000000000
...
pc    0000000080000326
  #0  0x80000326  compute+0x6  fw.c:4
  #1  0x800003c2  main+0x46  fw.c:5   (from the return-address register)
  (no deeper: this backend keeps no frame pointer and -g
   emits no .debug_frame for it yet)
```

(Output shortened.) `stopped at 0x1000` is QEMU's reset code, before the
image runs.

Debug an ATmega328P image built with `-g` on QEMU's arduino-uno: stop at
a function's entry, change its first argument, then stop at a source line
and read a local and a global:

```sh
qemu-system-avr -M uno -nographic -bios fw.elf -S -gdb tcp::1234 &
embdbg fw.elf remote 1234 <<'END'
break *0xa80
break fw.c:6
continue
regs
set r24 7
continue
print t
print acc
step
print acc
quit
END
```

```text
connected to localhost:1234 — avr target
stopped at 0x0  ??
breakpoint 1 at 0xa80  compute+0x0  fw.c:3
breakpoint 2 at 0xafa  compute+0x7a  fw.c:6
stopped at 0xa80  compute+0x0  fw.c:3
...
r24   03  r25   00  r26   3e  r27   01  r28   e9  r29   08  r30   14  r31   0d
sreg  02  sp    08e7  pc    00000a80
r24 = 0x7
stopped at 0xafa  compute+0x7a  fw.c:6
...
t = 33
acc = 100
stopped at 0xb46  compute+0xc6  fw.c:7
...
acc = 133
```

(Output shortened.) `0xa80` is `compute`'s address from `funcs`. The
program is `int acc = 100;` and `compute(int a, int b)` with
`int t = a * b + 5; acc += t; return t + 1;` on lines 5 to 7, first
called as `compute(3, 4)`: with `r24`, its first argument, changed to 7,
`t` is 33.

Convert an object's DWARF to `.embdbg` and check the result:

```sh
embdbg fw.o emit fw.embdbg
embdbg fw.embdbg verify
```

## SEE ALSO

[Debugging](../debugging.md), [`embld`](embld.md),
[Embedded programming](../embedded.md), [Invoking EmbCC](../invoking.md)
