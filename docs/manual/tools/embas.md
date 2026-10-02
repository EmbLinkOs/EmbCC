# embas — the EmbCC NASM-syntax assembler

This page is the reference for `embas`, EmbCC's standalone assembler for
x86-64 source in NASM (Intel) syntax. It is for people who assemble
hand-written `.asm` files, such as an operating-system kernel's entry and
interrupt code, without NASM. It lists the syntax, directives and
instructions `embas` accepts and what it does with anything else. Assembly
in GNU syntax (`.s`, `.S`) is handled by `embcc` itself; see
[GNU-syntax assembly](#gnu-syntax-assembly) at the end of this page.

## NAME

`embas` — assemble NASM-syntax x86-64 source into an ELF64 object

## SYNOPSIS

```text
embas [-f elf64|bin] [-o FILE] INPUT.asm
```

## DESCRIPTION

`embas` reads one source file in NASM syntax, expands its macros,
assembles it for 64-bit mode, and writes an x86-64 ELF64 relocatable
object. Short jumps are relaxed as NASM relaxes them: a jump to a label
in the same section uses an 8-bit displacement when the target is in
range and a 32-bit one otherwise. A local label (one whose name begins
with `.`) belongs to the most recent non-local label, and is written to
the symbol table as `GLOBAL.local`, as NASM does.

`embas` accepts a subset of NASM: the directives and instructions listed
below. A mnemonic it does not know is an error, not a guess. The same
assembler runs when `embcc` is given a `.asm` file:

```sh
embcc -c entry.asm -o entry.o
```

With `embcc`, the output name defaults to the input name with `.asm`
replaced by `.o`, and `-E` is refused with
`embcc: error: -E does not apply to assembly`.

## OPTIONS

### `-f FORMAT`

Select the output format. `FORMAT` is `elf64` (the default) or `bin`.

`-f bin` (a flat binary) is accepted on the command line but not
implemented: the file is assembled and then refused with
`embas: -f bin not yet implemented`, exit status 1.

Any other format is refused with `embas: unknown format 'FORMAT'`, exit
status 2. Without an argument: `embas: -f needs a format`, exit status 2.

### `-o FILE`

Write the object to `FILE`. The default is `a.out`. Without an argument:
`embas: -o needs a file`, exit status 2.

Any other argument beginning with `-` is refused with
`embas: unknown option 'ARG'`, exit status 2. There is no `--help`.

## INPUT

One source file. If several file names are given, only the last is
assembled.

### Lines, comments and labels

- A `;` begins a comment that runs to the end of the line, except inside
  a quoted string.
- A label is a name followed by a colon: `entry:`. The colon is required;
  a name alone on a line is read as an instruction. Names consist of
  letters, digits, `_`, `.` and `$`. Several labels may precede one
  statement.
- A label beginning with `.` is local to the preceding non-local label.
  References to it (`jne .loop`) are resolved in the same scope.
- Mnemonics and directive names are case-insensitive; register names and
  symbol names are case-sensitive and registers must be written in lower
  case.

### Operands

| Form | Meaning |
|---|---|
| `rax` ... `r15`, `eax` ... `r15d` | a 64-bit or 32-bit general register; 16-bit and 8-bit registers are not supported |
| `123`, `0x7f`, `-8`, `~0xFF` | an integer: decimal, `0x` hexadecimal, or octal with a leading `0`, optionally with unary `-`, `+` or `~` |
| `[reg]`, `[reg+disp]`, `[reg-disp]` | memory at a base register plus a constant displacement |
| `[gs:disp]`, `[fs:disp]`, `[gs:reg+disp]` | memory with a segment override; a bare displacement is an absolute address |
| `[rel symbol]` | RIP-relative memory; supported by `mov` only (see below) |
| `symbol` | a label or external symbol |
| `byte`, `word`, `dword`, `qword`, `oword`, `tword`, `yword` | a size keyword before an operand; accepted and ignored |

Arithmetic between symbols or constants (`label+4`, `2*8`) is not
supported. NASM's `0FFh` and `$`-prefixed hexadecimal forms are not
recognized.

## DIRECTIVES

| Directive | Effect |
|---|---|
| `section NAME`, `segment NAME` | switch to section `NAME`, one of `.text`, `.rodata`, `.data`, `.bss`; any other name is an error (`unknown section NAME`) |
| `global NAME`, `globl NAME` | export `NAME`; one name per directive |
| `extern NAME` | declare an external symbol; one name per directive |
| `db`, `dw`, `dd`, `dq` | emit bytes, words, doublewords or quadwords: comma-separated integers, quoted strings (bytes, no escape sequences), or symbol names |
| `resb N`, `resw N`, `resd N`, `resq N` | reserve `N` units of uninitialized space (in `.bss`) |
| `align N` | pad the current section with zero bytes to a multiple of `N` |
| `incbin "PATH"` | insert the contents of a file; `PATH` is relative to the current working directory |
| `[BITS 64]`, `bits 64`, `default ...`, `cpu ...` | accepted and ignored; `embas` always assembles 64-bit code. Any other line beginning with `[` is ignored too |
| `%macro NAME N` ... `%endmacro` | define a macro; `%1` through `%9` in its body are replaced by the arguments of an invocation |

A macro is invoked by writing its name as the first word of a line,
followed by comma-separated arguments. The expanded lines are scanned
again, so a macro may invoke another. Expansion more than 32 levels deep
stops with `embas: macro 'NAME' nests more than 32 deep (does it invoke
itself?)`.

A symbol name in `dd` or `dq` produces an absolute relocation
(`R_X86_64_32` or `R_X86_64_64`). This is correct only in `.text`: in any
other section the relocation is recorded against `.text` and the object is
wrong. In `db` and `dw`, symbol names are not supported.

Not supported: `equ`, `times`, `%define`, `%include`, `%if` and the rest
of the NASM preprocessor except `%macro`, `struc`, and section attributes.
They are reported as unknown instructions, for example
`unsupported instruction times`.

## INSTRUCTIONS

| Mnemonics | Operand forms |
|---|---|
| `cli`, `sti`, `hlt`, `nop`, `ret`, `leave`, `pushfq`, `popfq`, `cpuid`, `rdtsc`, `rdmsr`, `wrmsr`, `syscall`, `sysret`, `swapgs`, `iretd` | none |
| `iret`, `iretq` | none; both assemble as `iretq` (`48 cf`), as NASM does in 64-bit mode |
| `o64 sysret` | none; the 64-bit `sysret` (`48 0f 07`) |
| `push` | register, immediate (8-bit or 32-bit form chosen by value), memory |
| `pop` | register |
| `mov` | register to register; memory to register; register to memory; immediate to register; `symbol` to register (as `movabs` with `R_X86_64_64`); `[rel symbol]` to register and register to `[rel symbol]` (with `R_X86_64_PC32`) |
| `lea` | memory to register |
| `add`, `or`, `and`, `sub`, `xor`, `cmp` | register and register; register and immediate |
| `test` | register and register |
| `imul` | register and register (two-operand form) |
| `shl`, `sal`, `shr`, `sar` | register and immediate (`D1` form for a count of 1) |
| `call`, `jmp` | register (indirect) or symbol |
| `jo`, `jno`, `jb`, `jae`, `je`, `jz`, `jne`, `jnz`, `jbe`, `ja`, `js`, `jns`, `jl`, `jge`, `jle`, `jg` | symbol |
| `fxsave`, `fxrstor`, `lgdt`, `lidt` | memory |

A move of an immediate to a 64-bit register uses the shortest form NASM
uses: a 32-bit `mov` when the value fits in 32 bits unsigned, a
sign-extended 32-bit immediate when it fits signed, and a 64-bit
immediate otherwise.

A form not in this table is refused, for example
`assembler error: bad ALU operand add` for `add rax, [rbx]` or
`assembler error: bad mov` for `mov al, 1`. A mnemonic not in the table
gives `assembler error: unsupported instruction MNEMONIC`, with the
mnemonic in lower case.

Some invalid forms are not detected and produce wrong code. Do not use
them:

- an index register or scale in a memory operand: `[rax+rbx*8]`
  assembles as `[rax]`;
- `[rel symbol]` with any instruction other than `mov`: `lea rax,
  [rel sym]` assembles as `lea rax, [rbp]`;
- a reference to a symbol that is neither defined in the file nor
  declared with `extern`: the assembler terminates abnormally instead of
  reporting it. Declare every external symbol with `extern`.

## OUTPUT

An x86-64 ELF64 relocatable object. It has the sections `.text`,
`.rodata`, `.data` and `.bss` that received content, in the order in
which the source first used them. Its symbol table holds an `STT_FILE`
symbol naming the input, a section symbol per section, the labels
(non-exported labels as local symbols), and the `extern` symbols as
undefined globals. A reference to a symbol defined in the same object is
relocated against its section symbol plus an offset; a reference to an
external symbol is relocated against the symbol.

| Construct | Relocation |
|---|---|
| `call symbol` (external or in another section), `mov reg, [rel symbol]`, `mov [rel symbol], reg` | `R_X86_64_PC32`, addend −4 |
| `mov reg, symbol`, `dq symbol` | `R_X86_64_64` |
| `dd symbol` | `R_X86_64_32` |

A `call` or jump to a label in the same section is resolved in the
object and needs no relocation.

## DIAGNOSTICS

Errors have the form:

```text
FILE:LINE: assembler error: MESSAGE
```

`embas` stops at the first error. The line number counts the lines
after comments and blank lines are removed and macros are expanded, so it
can be smaller than the line's number in the source file.

## EXIT STATUS

| Status | Meaning |
|---|---|
| 0 | the object was written |
| 1 | the source could not be read (`embas: cannot open FILE`) or did not assemble, or `-f bin` was requested |
| 2 | a usage error: no input, unknown option or format, missing option argument |

If the output file cannot be written, `embas` prints
`embcc: cannot write 'FILE'` and exits with a nonzero status.

## ENVIRONMENT

`embas` reads no environment variables.

## EXAMPLES

A system-call stub:

```asm
; exit.asm -- void sys_exit(int code)
[BITS 64]
section .text
global sys_exit
sys_exit:
    mov rax, 60
    syscall
.hang:
    hlt
    jmp .hang
```

```sh
embas -f elf64 exit.asm -o exit.o
```

An interrupt stub built from a macro:

```asm
%macro STUB 1
global isr%1
isr%1:
    push qword 0
    push %1
    jmp isr_common
%endmacro

section .text
extern isr_dispatch
STUB 0
STUB 1
isr_common:
    call isr_dispatch
    iretq
```

## GNU-SYNTAX ASSEMBLY

`embas` reads NASM syntax for x86-64 only. Assembly in GNU syntax is
assembled by `embcc` itself, for the ARM (Thumb), AArch64, RISC-V and AVR
targets:

```sh
embcc --target=thumbv7m-none-eabi -c startup.S -o startup.o
```

A `.S` file is preprocessed first; a `.s` file is not. GNU-syntax files
for x86-64 are refused. Instructions are encoded by the same per-target
assembler that inline `__asm__` uses, so the accepted instructions are the
inline-assembly vocabulary listed in [Inline assembly](../inline-asm.md);
driver behavior and output naming are described in
[Invoking EmbCC](../invoking.md#assembly-input).

The directives this assembler accepts:

| Directive | Effect |
|---|---|
| `.text`, `.data`, `.rodata`, `.bss`, `.section NAME` | switch section; `NAME` must be one of those four, otherwise: `.section "NAME" is not one of .text/.data/.rodata/.bss` |
| `.global NAME`, `.globl NAME` | export a symbol |
| `.weak NAME`, `.weakref NAME` | declare a weak symbol; an undefined weak reference resolves to 0 at link time |
| `.type NAME, %function` (or `@function`) | mark a symbol as a function |
| `.byte`, `.short`, `.half`, `.word`, `.long`, `.quad`, `.dword` | data of 1, 2, 2, 4 (2 on AVR), 4, 8 and 8 bytes; a symbol name becomes an absolute relocation |
| `.ascii`, `.asciz`, `.string` | string data; `.asciz` and `.string` add a terminating zero; escapes `\n \t \r \0 \b \f \\ \"` |
| `.space N`, `.zero N`, `.skip N` | `N` zero bytes, or reserved space in `.bss` |
| `.align N`, `.balign N` | align to `N` bytes |
| `.p2align N` | align to 2^`N` bytes |
| `.size`, `.file`, `.ident`, `.cfi_startproc`, `.cfi_endproc`, `.syntax`, `.thumb`, `.thumb_func`, `.code`, `.arch`, `.fpu`, `.eabi_attribute`, `.attribute`, `.option` | accepted and ignored |

Any other directive is refused with
`directive ".NAME" is not one this assembler knows` (this includes `.equ`
and `.set`). Comments begin with `#` or `//`, and also with `;` on AVR.
Because `#` begins a comment anywhere on a line, write ARM and AArch64
immediates without it: `movs r0, 5`, not `movs r0, #5`. The latter loses
its operand and is reported as an unknown instruction, for example
`asm instruction "movs" is not in the ARMv7-M vocabulary`. Numeric local
labels (`1:`, referenced as `1b` and `1f`) are supported,
for the digits 0 to 9.

A symbol that the file does not define may be named only in the
instruction forms that carry a relocation:

| Target | Forms |
|---|---|
| RISC-V | `call SYMBOL`, `la REG, SYMBOL` |
| ARM, AArch64 | `bl SYMBOL` |
| AVR | `call`, `jmp`, `rcall`, `rjmp` and conditional branches to a symbol; `lds`/`sts` with a symbol address; `ldi REG, lo8(SYMBOL)`, `hi8(...)`, `pm_lo8(...)`, `pm_hi8(...)`, and `lo8(gs(SYMBOL))`, `hi8(gs(SYMBOL))` |

Any other use is refused with
`"STATEMENT" names the undefined symbol 'NAME' in a form this target's assembler cannot relocate`.
Errors have the form `FILE:LINE: error: MESSAGE`.

## SEE ALSO

[`embld`](embld.md), [Inline assembly](../inline-asm.md),
[Invoking EmbCC](../invoking.md#assembly-input), [Targets](../targets.md)
