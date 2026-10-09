# Inline Assembly

This page describes assembly language embedded in C source: `asm`
statements inside functions (basic and extended), local register
variables, assembler names on declarations, and file-scope `asm` blocks.
For each target it lists the constraint letters, the operand modifiers and
the complete set of instructions EmbCC's built-in assembler accepts in a
template. It is for people writing kernels, drivers, startup code and
other low-level C with EmbCC, and for people porting inline assembly
written for GCC or Clang. Target triples, register conventions and ABIs
are in [Targets](targets.md); the other GNU extensions are in
[Extensions](extensions.md).

## How EmbCC assembles a template

EmbCC does not hand an `asm` template to an external assembler. It
assembles the template itself, while it generates code for the
function, with a built-in assembler for the selected target. Each
target's assembler accepts a fixed set of instructions, listed on this
page under the target. An instruction outside that set is refused with a
diagnostic that names it. The few places where an assembler accepts an
operand form and encodes something other than what GNU `as` would are
stated under the target.

| Target | Diagnostic for an instruction the assembler does not know |
|---|---|
| [x86-64](#x86-64) | `asm instruction "vzeroall" not supported` |
| [AArch64](#aarch64) | `aarch64 inline asm: instruction 'ldxr' is not supported (in "...")` |
| [ARM Cortex-M](#arm-cortex-m) | `asm instruction "vldr" is not in the ARMv7-M vocabulary` |
| [RISC-V](#risc-v) | `asm instruction "amoswap.w" is not in the RISC-V vocabulary` |
| [MIPS32](#mips32) | `asm instruction "madd $t0, $t1" is not in the MIPS vocabulary` |
| [SPARC](#sparc) | `asm instruction "popc" is SPARC V9's, and the LEON3 is a V8`, or `... is not in the SPARC vocabulary` |
| [PowerPC](#powerpc) | `asm instruction "fadd" is a floating-point instruction: ...`, or `... is not in the PowerPC vocabulary` |
| [AVR](#avr) | `'frobnicate' is not an AVR instruction this assembler knows (assembling "...")` |

The full diagnostic carries the file and the line of the `asm` statement:

```text
embcc: probe.c:1: error: asm instruction "vzeroall" not supported
  void f(void){ __asm__("vzeroall"); }
```

On AArch64, ARM Cortex-M, RISC-V and AVR, EmbCC first substitutes the
operands into the template text and then assembles the text with the
same assembler that handles `.s` and `.S` files for that target (see
[Assembly input](invoking.md#assembly-input)). On x86-64 the template is
AT&T syntax and is read by a separate encoder that resolves each operand
reference as it encodes the instruction; it is unrelated to the
NASM-syntax assembler used for `.asm` files ([embas](tools/embas.md)).
On MIPS32 the operands are substituted the same way, and the text is
assembled by the same MIPS32 assembler that reads `.s` and `.S` files.

A template is assembled only when its function is emitted. An `asm`
statement in a function that is never emitted, such as an unused
`static inline` function in a header, is not assembled, and an unknown
instruction in it is not reported.

There is no `-masm=` option; x86-64 templates are AT&T syntax. The GCC
option `-fno-asm` is not accepted (`unknown argument '-fno-asm'`).

## Basic asm

```text
asm ( "TEMPLATE" ) ;
```

The keyword is spelled `asm`, `__asm__` or `__asm`. All three are
keywords in every `-std=` mode, so `asm` cannot be used as an identifier
even with `-std=c11`. Adjacent string literals in the template are
concatenated.

Inside a function, EmbCC treats basic asm exactly as an extended asm
statement with no operands. In particular:

- The template rules of extended asm apply. On x86-64 a hard register is
  written `%%rax` even in basic asm; a single `%` is read as an operand
  reference and refused with `asm: expected a %N operand in "..."`.
- Basic asm is a compiler barrier like any other `asm` statement (see
  [What the compiler assumes](#what-the-compiler-assumes)).

Basic asm at file scope is described in [File-scope asm](#file-scope-asm).

## Extended asm

### Syntax

```text
asm [volatile] ( TEMPLATE
                 [ : OUTPUTS
                 [ : INPUTS
                 [ : CLOBBERS ] ] ] ) ;
```

`OUTPUTS` and `INPUTS` are comma-separated lists of operands, each of the
form

```text
[ [NAME] ] "CONSTRAINT" ( EXPRESSION )
```

and `CLOBBERS` is a comma-separated list of string literals. Any list may
be empty, and trailing sections may be omitted. The template, every
constraint and every clobber may be written as adjacent string literals,
which are concatenated.

Operands are numbered from 0 in the order they appear, outputs first and
then inputs. A `+` operand is one operand and has one number.

```c
long add_named(long a, long b)
{
    long r;
    __asm__("movq %[x], %[r]\n\taddq %[y], %[r]"
            : [r] "=&r"(r)
            : [x] "r"(a), [y] "r"(b));
    return r;
}
```

### Qualifiers

| Qualifier | Status |
|---|---|
| `volatile` | Accepted, also spelled `__volatile__` and `__volatile`. It changes nothing: EmbCC treats every `asm` statement as volatile. |
| `inline` | Not accepted in C: `expected '(' after asm before 'inline'`. |
| `goto` | Not supported; refused with the diagnostic below. |

```text
`asm goto` is not supported: its template branches to a label, which needs a patchable placeholder in each backend's inline assembler and CFG edges the optimizer honours. Use a normal asm that sets a value and branch on that
```

### Output operands

An output constraint must begin with `=` or `+`; otherwise EmbCC reports
`an asm output constraint must start with '=' or '+' (got "r")`. The
expression must be an lvalue; otherwise it reports
`an asm output operand must be an lvalue`.

| Modifier | Meaning |
|---|---|
| `=` | The operand is written by the template. Its register holds an unspecified value when the template starts. |
| `+` | The operand is read and written. EmbCC loads the lvalue's current value into the register before the template starts. A `+` anywhere in the constraint has this effect. |
| `&` | Accepted and ignored. Every operand EmbCC allocates gets a register of its own, distinct from every other operand's, so an output never shares a register with an input unless both name the same fixed register. |

After the template, EmbCC stores the output register into the lvalue,
writing as many bytes as the lvalue's type has.

An output must fit in one general-purpose register; on AVR it may occupy
up to four consecutive registers. A wider output is refused on ARM
Cortex-M, RISC-V, MIPS32 and AVR with the diagnostics listed under each target;
on x86-64 and AArch64 it stops the compiler with an internal error
(`bad store size 16`, `aarch64 access size cannot encode 16`).

An output constraint that contains `m` (`=m`, `+m`) is accepted, but
EmbCC does not load the lvalue's address into the operand's register and
stores nothing afterwards, so the template has no usable operand. Pass
the address as an input in a register and add a `"memory"` clobber
instead.

### Input operands

EmbCC evaluates each input expression and loads its value into the
operand's register immediately before the template. On AVR an input
occupies as many consecutive registers as its type has bytes (1, 2 or 4).

An input whose constraint contains the letter `m` anywhere (`"m"`,
`"rm"`) is passed by **address**: the register holds the address of the
expression, and the template dereferences it. The expression must be an
lvalue; anything else stops the compiler with an internal error
(`address of a non-lvalue`). This differs from GCC, where `%1` of an `m`
operand prints a memory reference. In EmbCC the template writes the
dereference itself:

| Target | Template |
|---|---|
| x86-64 | `movq (%1), %0` |
| AArch64, ARM Cortex-M | `ldr %0, [%1]` |
| RISC-V, MIPS32 | `lw %0, 0(%1)` |
| AVR | do not use `m`; pass a pointer with `e`, `x` or `z` and write `%a1` (see [AVR](#avr)) |

On AArch64 a bare `m` is refused; `rm` is accepted and passes the
address.

The integer-constant constraints `i` and `n` behave differently per
target. On AArch64, MIPS32 and AVR the constant is substituted into the
template as a literal. On x86-64, ARM Cortex-M and RISC-V the constant is
computed into a register like any other input, so the template uses it
as a register operand:

```c
/* ARM Cortex-M and RISC-V: %2 is a register holding 5 */
__asm__("add %0, %1, %2" : "=r"(r) : "r"(a), "i"(5));
```

### Clobbers

A clobber names a register the template changes that is neither an
operand nor otherwise visible to EmbCC. EmbCC never places an operand in
a clobbered general-purpose register. List every register an instruction
writes implicitly, for example `rcx` and `r11` for x86-64 `syscall`, so
that no input is placed in it.

`"memory"` is accepted and has no additional effect, because EmbCC
already treats every `asm` statement as reading and writing memory.
`"cc"` is accepted and has no effect.

A clobber that does not name a general-purpose register of the target
(a floating-point or vector register, or an unknown name) is accepted and
ignored.

A callee-saved register in the clobber list is refused on AArch64, ARM
Cortex-M, RISC-V, MIPS32 and AVR, and accepted without being saved on x86-64; see
[Callee-saved registers](#callee-saved-registers).

### Referring to operands in the template

| Form | Meaning |
|---|---|
| `%N` | Operand number `N`. |
| `%[NAME]` | The operand declared with `[NAME]`. An unknown name is refused (x86-64: `asm: unknown operand %[zz] in "..."`; other targets: `asm template names an unknown operand 'zz'`). |
| `%%` | On x86-64, the prefix of a hard register name (`%%rax`, `%%cr3`). On the other targets, a literal `%`. |

A number past the last operand is refused (x86-64:
`asm operand %5 out of range in "..."`; other targets:
`asm template refers to operand %5, but there are only 2`).

The modifiers each target accepts between `%` and the operand are:

| Modifier | Meaning | Targets |
|---|---|---|
| `%cN`, `%c[NAME]` | the value of a constant (`"i"`, `"n"`) operand alone, with no `#` or `$`: `-5` | every target; refused for a register operand (`%c0 names a register operand`) |
| `%wN`, `%w[NAME]` | the 32-bit name of the register (`w9`) | AArch64 |
| `%xN`, `%x[NAME]` | the 64-bit name of the register (`x9`) | AArch64 |
| `%AN` .. `%DN` | byte 0 to 3 of a multi-register operand | AVR |
| `%aN` | the operand as a pointer register: `X`, `Y` or `Z` | AVR |

No other modifier is supported. On x86-64, `%c` is read only in a
directive (below); GCC's `%b`, `%h`, `%w`, `%k` and `%q` are refused
there with `asm: expected a %N operand in "..."`. The generic `%n`,
`%l`, `%P` and `%=` are refused on every target; outside x86-64 the
diagnostic is `asm template modifier '%n' is not supported for aarch64`
(with `ARMv7-M`, `RISC-V` or `AVR` in place of `aarch64`).

On x86-64, a constant operand that the template names only as `%c` is
text: no register is allocated or loaded for it.

A register that the template names directly (`%%rsi` on x86-64, `x0` on
AArch64, `r0` on ARM, `a0` on RISC-V, `r24` on AVR) is never given to an
operand.

### Directives in a template

Beside its instructions, a template may hold data and alignment, written
as GNU `as` writes them:

| Directive | Emits |
|---|---|
| `.ascii "S"[, "S"...]` | the bytes of each string |
| `.asciz "S"`, `.string "S"` | each string followed by a NUL byte |
| `.byte`, `.short`, `.word`, `.long`, `.quad` and the other data directives | each comma-separated constant expression at the directive's size |
| `.p2align N[, FILL[, MAX]]` | padding to a multiple of 2^N bytes |
| `.balign N[, FILL[, MAX]]` | padding to a multiple of N bytes (a power of two) |
| `.align N[, FILL[, MAX]]` | as `.balign N` on x86-64 in ELF and COFF objects, as `.p2align N` everywhere else, including AVR, where GNU `as` reads it that way |

Strings take the escapes `\b`, `\f`, `\n`, `\r`, `\t`, `\"`, `\\`, an
octal `\NNN` and a hex `\xHH`. A `;`, `#`, `@` or `//` inside a string is
part of the string. A data value must be a constant that fits the
directive's size, signed or unsigned; a symbol is refused, because an
inline template cannot carry a relocation.

The data directive's size follows the target's assembler: `.word` is two
bytes on x86-64 and AVR and four elsewhere; `.half` and `.dword` exist on
RISC-V; `.hword`, `.xword` and `.dword` on AArch64.

Padding is decided where the template lands in its section, not relative
to the template: `.p2align 2` after an 18-byte string pads to the next
multiple of four in `.text`. Without a fill value, the padding is the
target's nop, as llvm-mc pads code (x86-64's long nops, AArch64's `nop`
after zero bytes up to a word, Thumb's `nop`, RISC-V's `c.nop` and
`nop`, zero bytes on AVR). A function holding such a template starts on
that alignment, so the padding is also right when the function is in a
section of its own (`-ffunction-sections`). The largest alignment is 16
bytes, the alignment of `.text`.

Data that would leave the next instruction off an instruction boundary,
such as `.byte 7` alone on Arm or RISC-V, is followed by zero bytes up to
one, as GNU `as` does on AArch64.

On ARM and AArch64, the data and the padding after it are marked with
`$d` mapping symbols, so a disassembler shows them as data.

This is how Linux's `asm-offsets` and similar layout probes write a
structure's size into an object file:

```c
#define LAYOUT(name, size, align) \
    __asm__ volatile("\n.ascii \"->PROBE " #name " %c0 %c1\"\n.p2align 2\n" \
                     : : "i"(size), "i"(align))
```

These directives are read on x86-64, AArch64, ARM (Cortex-M and ARM
state), RISC-V, AVR, MIPS32 and LoongArch64. On MIPS32 and LoongArch64
an alignment larger than four bytes is refused (`the asm aligns to 8
bytes, which MIPS inline asm does not do yet`). On Xtensa, SPARC, PowerPC,
TriCore, RX and ColdFire a template takes no directive yet.

### Constraint strings

EmbCC reads a constraint as a set of letters: it looks for the letters it
knows, in a fixed order of precedence for the target, and ignores the
others. A GCC constraint with several alternatives therefore takes the
meaning of the first known letter by that order. The letters and their
precedence are listed under each target.

Matching constraints (a digit such as `"0"`) and flag-output constraints
(`"=@ccz"`) are not supported on any target. A digit constraint is
refused. Flag outputs are discussed under [x86-64](#x86-64) and
[AArch64](#aarch64), the two targets that define
`__GCC_ASM_FLAG_OUTPUTS__`.

Whether an unsupported constraint is reported depends on the target. On
x86-64, ARM Cortex-M and RISC-V it is reported when the function is
checked, even if the function is never emitted. On AArch64, MIPS32 and
AVR it is reported only when the `asm` statement is generated, so a GCC-style
header with another machine's constraints in an unused `static inline`
function compiles.

### What the compiler assumes

EmbCC makes the same assumptions about every `asm` statement, with or
without `volatile` and whatever its clobbers say:

- It is never deleted, even when its outputs are unused, and a loop that
  contains one is not unrolled.
- It may read and write any memory. Values that the code read from memory
  before the statement are read again after it.
- A function that contains an `asm` statement may be inlined like any
  other, so an `always_inline` intrinsic around one disappears into its
  caller.
- On ARM Cortex-M and RISC-V, a value live across the statement is kept
  out of the registers it may change: its operands' registers, the
  registers in its clobber list, the registers its template names, the
  one EmbCC stores outputs through, and every caller-saved register
  (r0 to r3, r12 and lr; ra, t0 to t6 and a0 to a7) when the template
  calls (`bl`, `blx` or `svc` on ARM; `call`, `tail`, `jal`, `jalr` or
  `ecall` on RISC-V). Any other register may hold a value across it, so
  the template must list every register it changes and does not name,
  as GCC requires. On x86-64, AArch64 and AVR no value is kept in a
  register across it: every value live across an `asm` statement stays
  in memory, so the template may change any caller-saved register
  without listing it, provided no operand is placed there (see
  [Clobbers](#clobbers)).

The operands are moved into their registers immediately before the
template and out of them immediately after it. On ARM Cortex-M and
RISC-V they are values like any other: an input is moved from wherever it is (a
register or memory), several at once as one parallel move, and an
`"=r"` output of an integer or pointer type is a value that its variable
receives afterwards, so a local written by an `asm` can live in a
register. Other outputs, and every operand on the other targets, go
through memory: inputs and `+` outputs are loaded immediately before the
template, and outputs are stored through their addresses immediately
after it.

### Callee-saved registers

EmbCC does not save callee-saved registers around an `asm` statement.
Each target handles a template that would change one as follows:

| Target | Callee-saved register named in the template or the clobbers | Operand placed in a callee-saved register |
|---|---|---|
| x86-64 | accepted; not saved | saved by the prologue at `-O2` and `-Os` only |
| AArch64 | refused (x19 to x30) | cannot happen: neither the allocator nor a register variable uses one |
| ARM Cortex-M | refused (r4 to r11) | never chosen by the allocator; possible through the letters `S` and `D` (see [ARM Cortex-M](#arm-cortex-m)), and then saved by the prologue and given no other value in that function |
| RISC-V | refused (s0 to s11) | never chosen by the allocator; see [Register variables](#register-variables) |
| MIPS32 | refused (s0 to s7, and gp, sp, fp, ra, k0, k1) | cannot happen: neither the allocator nor a register variable uses one |
| AVR | refused (r2 to r17, and r28 and r29) | refused |

The details and the diagnostics are under each target.

## Register variables

```text
register TYPE NAME __asm__("REGISTER") [= INITIALIZER];
```

A local variable declared with an `asm` register name is placed in that
register when, and only when, the variable itself appears as an operand
of an `asm` statement. Everywhere else it is an ordinary local variable.
An expression that merely contains the variable (`x + 0`) is not bound.
The `register` keyword is optional: `long x __asm__("r10");` binds the
same way.

| Target | Names that bind | Other names |
|---|---|---|
| x86-64 | `rax`, `rbx`, `rcx`, `rdx`, `rsi`, `rdi`, `r8` to `r15` | ignored without a diagnostic; the operand is placed as its constraint says |
| AArch64 | `x0` to `x11`, `x13` to `x15`, and the `w` names of the same registers | refused |
| ARM Cortex-M | not supported (see below) | |
| RISC-V | not supported (see below) | |
| MIPS32 | `v0`, `v1`, `a0` to `a3`, `t0` to `t9`, with or without `$`, and their numbers `$2` to `$15`, `$24`, `$25` | refused |
| AVR | `r0`, `r1`, `r18` to `r27`, `r30`, `r31`, `XL`, `XH`, `ZL`, `ZH`, `X`, `Z`, `__tmp_reg__`, `__zero_reg__` | refused |

On x86-64 a fixed-register constraint letter (`a`, `b`, `c`, `d`, `S`,
`D`) takes precedence over the variable's register. Only the 64-bit
names bind: `register int x __asm__("eax")` is ignored.

On AArch64 any other name is refused with
`register variable bound to 'x19' is not supported for aarch64 asm (use x0..x11 or x13..x15)`.
x12 is the code generator's address scratch, x16 to x18 are the
intra-procedure-call and platform registers, and x19 and above are
callee-saved.

On MIPS32 any other name is refused with
`register variable bound to '$s0' is not supported for MIPS asm (use v0-v1, a0-a3 or t0-t9)`.

On AVR a name that is not a register is refused with
`register variable bound to 'foo' is not an AVR register`, and a
callee-saved register or half of the frame pointer with
`an asm operand is pinned to 'r5', which is callee-saved, and EmbCC saves nothing around an asm`.

On ARM Cortex-M and RISC-V register variables are not supported, and
EmbCC does not diagnose them. It looks the name up among the x86-64
names above, so every RISC-V ABI name and the ARM names `r0` to `r7` are
ignored and the operand is allocated as its constraint says. The names
`r8` to `r15` match x86-64 names and bind to register number 8 to 15
with no callee-saved check: on Cortex-M that is r8 to r15. To put a
value in a particular register on these targets, move it there inside
the template and name the register; a register the template names is
never given to an operand:

```c
int semihost(int op, void *arg)            /* ARM Cortex-M */
{
    int r;
    __asm__ volatile("mov r0, %1\n\tmov r1, %2\n\tbkpt #0xab\n\tmov %0, r0"
                     : "=r"(r) : "r"(op), "r"(arg) : "r0", "r1", "memory");
    return r;
}
```

Register variables at file scope (global register variables) are not
supported: `'register' is not supported yet (see docs/manual/c-language.md)`.

## Assembler names on declarations

GCC's `asm` labels, which give a declaration a different symbol name,
are not supported in C:

| Declaration | Result |
|---|---|
| `int foo asm("bar");` at file scope | `expected ';' before 'asm'` |
| `int f(void) asm("g");` | `expected '{' or ';' before 'asm'` |
| `extern int y asm("zz");` in a block | `expected ';' before 'asm'` |
| `static int y asm("zz");` in a block | accepted; the name is ignored and the object keeps its usual symbol |

In C++ they are supported; see [C++](cxx.md).

## File-scope asm

```text
asm ( "TEMPLATE" ) ;
```

An `asm` block outside any function takes no qualifier and no operands.
Each line of the template is one statement, optionally preceded by a
label (`name:`).

How a block is assembled depends on the target:

- **ARM Cortex-M, RISC-V, MIPS32, LoongArch64, Xtensa, TriCore, SPARC, PowerPC, RX, ColdFire and AVR.** The block is read by the assembler
  that reads a `.s` file, so it holds the target's own instructions; see
  [On Cortex-M, RISC-V, MIPS32 and AVR](#on-cortex-m-risc-v-mips32-and-avr).
- **x86-64 and AArch64.** The block is read by a small fixed vocabulary
  of directives, data and (on x86-64) four instructions, described in the
  rest of this section. `#`, `;` and `/*` start a comment that runs to
  the end of the line. Registers are written with a single `%`.

The assembled bytes of every block are placed in `.text` after the
unit's functions. On x86-64 and AArch64 each block starts on a 16-byte
boundary.

### Directives

| Directive | Effect |
|---|---|
| `.global NAME`, `.globl NAME` | makes the label `NAME`, defined in the same block, a global symbol |
| `.weak NAME` | makes the label `NAME` a weak global symbol |
| `.type NAME, %function` (or `@function`) | makes `NAME`'s symbol a function; on Cortex-M its value has bit 0 set, as a Thumb function's must |
| `.type NAME, %object` (or `@object`) | makes `NAME`'s symbol an object |
| `.thumb_func` | Cortex-M only: the next label is a Thumb function, as with `.type NAME, %function` |
| `.byte V, ...` | 1-byte values |
| `.long V, ...` | 4-byte values; on a target with 4-byte addresses (Cortex-M, RV32), a symbol name instead of a number emits an absolute 32-bit relocation |
| `.quad V, ...` | 8-byte values; on a target with 8-byte addresses, a symbol name instead of a number emits an absolute 64-bit relocation |
| `.align N`, `.balign N`, `.p2align N` | pads to the boundary with the target's no-op instruction; `.align` counts bytes on x86-64 and is a power of two elsewhere, as in GNU as |
| `.text`, `.section .text...`, `.pushsection .text...`, `.popsection`, `.previous`, `.local`, `.size` | accepted; no effect beyond what is above |
| `.file`, `.loc`, `.cfi_startproc`, `.cfi_endproc`, `.cfi_def_cfa`, `.cfi_def_cfa_offset`, `.cfi_def_cfa_register`, `.cfi_offset`, `.cfi_restore`, `.cfi_sections` | accepted, no effect |

A global label without `.type` is a function symbol, except on Cortex-M,
where it is untyped and its value has no Thumb bit, as with GNU as. Values
are decimal, hexadecimal (`0x`) or octal (leading `0`). Any other
directive is refused:

```text
file-scope asm directive not supported: ".word". EmbCC's assembler emits data with .byte/.long/.quad; an unknown directive would contribute no bytes and leave the label pointing at whatever came next
```

These are refused because they would not do what they say:

| Refused | Diagnostic |
|---|---|
| Any section other than `.text` | `file-scope asm section ".data" is not supported: EmbCC places every byte of a block in .text, where this data would not be writable; define it in C` |
| Alignment beyond 16 bytes | `file-scope asm ".balign 32": the alignment must be a power of two of at most 16 bytes, which is what a block starts on` |
| `.hidden` | `file-scope asm .hidden is not supported: the symbol would be emitted with default visibility` |
| A symbol in data of another width | `asm data naming a symbol must be the size of an address, 8 bytes here (.quad): ".long main"` |
| `.type` other than function or object | `file-scope asm ".type f, %gnu_indirect_function": the type must be function or object` |

A `.global` or `.weak` that names no label in its block is refused with
`asm .global names "ghost", which has no label`.

Labels that no `.global` names are local to the block and produce no
symbol.

### Instructions

On x86-64 four instructions are accepted:

| Instruction | Encoding |
|---|---|
| `and $IMM, %REG` | 64-bit AND with a sign-extended 8-bit immediate (-128 to 127); `REG` is a 64-bit name |
| `call SYMBOL` | `call rel32` with a relocation; `SYMBOL` may be any function |
| `jmp LABEL` | `jmp rel32` to a label in the same block (`1b`, `1f`, or a name) |
| `ret` | `ret` |

Anything else is refused:

```text
file-scope asm instruction not supported: "push %rbx" (EmbCC assembles .global/labels/.byte/.long/.quad and and/call/jmp/ret)
```

A `jmp` to a name outside the block is refused with
`file-scope asm jump target "nowhere" is not a local label`. A function
that a block calls with `call` counts as used, so a `static` function
reached only from file-scope asm is still emitted.

```c
__asm__(".global _start\n"
        "_start:\n"
        "  and $-16, %rsp\n"
        "  call start_c\n"
        "1: jmp 1b\n");
```

### Per target

| Target | What a file-scope block may contain |
|---|---|
| x86-64 ELF (`x86_64-elf`, `x86_64-emblink`, `x86_64-linux-gnu`) | everything above |
| ARM Cortex-M, RISC-V, MIPS32, LoongArch64, Xtensa, TriCore, SPARC, PowerPC, RX, ColdFire, AVR | the target's instructions and the GNU assembler's directives; see [On Cortex-M, RISC-V, MIPS32 and AVR](#on-cortex-m-risc-v-mips32-and-avr) |
| AArch64 ELF | directives and data only; an instruction is refused (below) |
| `x86_64-apple-darwin` | a block with a label or a symbol reference is refused (below) |
| `aarch64-apple-darwin` | directives and data only, and a block with a label or a symbol reference is refused (below) |
| `x86_64-windows-gnu` | a block with a label or a symbol reference is refused (below) |
| C++ (any target) | refused: `file-scope asm in C++ is not supported yet` |

On AArch64, an instruction in a file-scope block is refused, whatever
its mnemonic:

```text
file-scope asm instruction "ret": EmbCC assembles instructions for x86-64 only. On this target write the block as .byte/.long data (see lib/libc/src/setjmp).
```

A block written as data (`.byte`, `.long`, `.quad`, labels and the
directives above) assembles there.

On Darwin and Windows targets the bytes of a block would be emitted
without its symbols and relocations, so a block with any label or symbol
reference is refused:

```text
a file-scope asm block with labels or symbol references is not supported for a Darwin target yet: its bytes would be emitted but its symbols and relocations dropped
a file-scope asm block with labels or symbol references is not supported for a Windows target yet
```

### On Cortex-M, RISC-V, MIPS32 and AVR

On these targets a block is assembled by EmbCC's GNU-syntax assembler,
the one that assembles a `.s` or `.S` file (see
[embas](tools/embas.md#gnu-syntax-assembly)). A block may hold the
target's instructions, labels (named, `.L` local and numeric `1:`/`1b`),
literal pools (`ldr rd, =sym` with `.ltorg` on ARM), the assembler's
expressions and macros, and references to C functions and objects:

```c
/* sum_table(n): the first n words of a table, on a Cortex-M */
__asm__(".text\n"
        ".global sum_table\n"
        ".type sum_table, %function\n"
        ".thumb_func\n"
        "sum_table:\n"
        "  ldr r1, .Ladr\n"
        "  movs r2, #0\n"
        "1: cbz r0, 2f\n"
        "  ldr r3, [r1], #4\n"
        "  add r2, r2, r3\n"
        "  subs r0, #1\n"
        "  b 1b\n"
        "2: mov r0, r2\n"
        "  bx lr\n"
        "  .p2align 2\n"
        ".Ladr: .word .Ldata\n"
        ".Ldata: .word 10, 20, 30\n");
```

The block's bytes go into the unit's `.text`, aligned as the block's own
alignment directives ask and at least to 4 bytes (2 on AVR). Its symbols:

- A label named by `.global` or `.weak` is a global symbol, typed by
  `.type` or `.thumb_func`, with the size `.size` gives. A Thumb function
  has bit 0 set.
- A label that is not global, with the name of a function or object the
  C code declares and does not define, is a local symbol, and the C
  code's calls and references go to it. That is how a `static` function
  written in assembly is called from C.
- Other labels produce no symbol.

A reference to a label the block itself defines and does not make global
reaches that label, whatever C or another block calls by the same name:
two naked functions may each have a `loop:`, and a block's own `cfunc:`
is not the C function `cfunc`.

A call or a data word naming a C function or object is a relocation
against it, and a function called only from a block is still emitted.
A data word naming a local label (`.word .Ldata`) is a relocation
against `.text`. The block's data (its literal pools and data words) is
marked with `$d` mapping symbols on ARM, so disassemblers show it as
data.

These are refused by name:

| Refused | Diagnostic |
|---|---|
| Bytes in any section but `.text` | `assembly in a C file that switches to section .data is not supported yet: a block's bytes go in .text` |
| A relocation outside `.text` | `a relocation outside .text in an asm block` |
| `.set` naming something the block does not define | `'a' is set to 'b', which this block does not define` |

Everything the assembler itself refuses (an instruction it does not
encode, an out-of-range branch) is refused with the file and line of the
block.

## Naked functions

```c
void xPortPendSVHandler(void) __attribute__((naked));
```

A naked function has no prologue and no epilogue: its body is the asm in
it, which must return by itself. It is how a Cortex-M RTOS writes its
context switch, which runs on a task's stack and saves registers the
code generator's frame would otherwise own. EmbCC supports `naked` on
ARM Cortex-M, RISC-V, MIPS32 and AVR, and does what GCC does: the function's
body is assembled where the function would be, starting with its label.
It is assembled as a [file-scope block](#on-cortex-m-risc-v-mips32-and-avr), so
it may hold labels, literal pools and calls into C.

The attribute may be on any declaration of the function; FreeRTOS's
ports put it on the prototype only.

```c
static void prvPortStartFirstTask(void) __attribute__((naked));

static void prvPortStartFirstTask(void)
{
    __asm volatile(" ldr r0, =0xE000ED08 \n"
                   " ldr r0, [r0]        \n"
                   " ldr r0, [r0]        \n"
                   " msr msp, r0         \n"
                   " cpsie i             \n"
                   " svc 0               \n"
                   " .ltorg              \n");
}
```

The body may contain:

- **asm statements.** Basic asm is assembled as written. Extended asm
  may have input operands that are constants (`"i"` or `"n"`), written
  into the template as numbers: `%0`, `%c0` and `%[name]` all give `80`
  for `"i"(80)`. `%%` is `%`.
- **calls with no arguments,** `vTaskSwitchContext();`, assembled as the
  target's call instruction (`bl` on ARM, `jal` on MIPS32, `call` on
  RISC-V and AVR).
  AVR's FreeRTOS port calls the scheduler this way from its naked yield.

A naked function's parameters arrive in the registers the calling
convention puts them in, for the asm to read. There is no warning for an
unused parameter, and no error for a non-`void` function without a
`return`: the asm leaves the value in the return register.

These are refused, because a naked function has no frame for them:

| Refused | Diagnostic |
|---|---|
| any other statement | `naked function 'f' holds a statement that is not an asm or a call with no arguments; with no prologue there is no frame for it to run in` |
| an operand that is not a constant | `operand 0 of the asm in naked function 'f' is not a constant ("i"); a naked function has no frame to load one from` |
| an output operand | `the asm in naked function 'f' has an output; a naked function has no frame to put it in` |
| a `section` attribute | `naked function 'f' in section '.ramfunc' is not supported yet: its body is assembled into .text` |
| x86-64 and AArch64 | `__attribute__((naked)) is not supported: on this target the body could only be assembled by the file-scope assembler's few instructions; it is supported on the ARM, RISC-V, MIPS and AVR targets` |

`tests/golden/freertos-cm3.sh` builds the FreeRTOS kernel and its GCC
ARM_CM3 port, unmodified, and runs three tasks, a queue, a mutex and a
software timer on QEMU's lm3s6965evb.

## x86-64

This section applies to every x86-64 triple, including
`x86_64-apple-darwin` and `x86_64-windows-gnu`.

### Constraints

| Letter | Meaning |
|---|---|
| `a` | `rax` |
| `b` | `rbx` |
| `c` | `rcx` |
| `d` | `rdx` |
| `S` | `rsi` |
| `D` | `rdi` |
| `r`, `q`, `g`, `R` | a general register chosen by EmbCC |
| `i`, `n` | the constant, computed into a general register chosen by EmbCC |
| `m` | a general register chosen by EmbCC, holding the address of the operand (inputs; see [Input operands](#input-operands)) |
| `x` | an SSE register, `xmm0` to `xmm7` |
| `=`, `+`, `&` | see [Output operands](#output-operands) |

Precedence: a fixed letter (`a`, `b`, `c`, `d`, `S`, `D`) anywhere in the
string wins; then a [register variable](#register-variables); then any of
`r`, `q`, `g`, `m`, `R`, `i`, `n`; then `x`. So `"Nd"` means `rdx` and
`"rm"` means a register holding an address.

`q`, `g` and `R` do not have their GCC meanings: each is any register of
the pool, including `r8` to `r11`. `g` never means memory or an
immediate.

A chosen register comes from `rax`, `rcx`, `rdx`, `rbx`, `rsi`, `rdi`,
`r8`, `r9`, `r10`, `r11`, in that order, skipping registers fixed by
another operand, listed as clobbers, or named anywhere in the template
(`%%rsi`). When none is left EmbCC reports
`asm: out of registers for the operands`, and for `x` operands
`asm: out of xmm registers for the operands`.

Any other letter is refused:

```text
asm constraint "=t" is not supported (EmbCC handles a/b/c/d/S/D, 'r'/'q'/'g'/'m', 'x', and a register-asm variable)
```

This includes `A`, `I`, `J`, `K`, `L`, `M`, `N` alone, `e`, `Z`, `t`,
`u`, `f`, `y`, `Y` and `0` to `9`.

**Flag outputs.** `__GCC_ASM_FLAG_OUTPUTS__` is defined, but flag-output
constraints are not implemented and are not refused: in `"=@ccc"`,
`"=@ccz"` and every other `=@cc` spelling, EmbCC sees the fixed letter
`c`, and the output receives the contents of `rcx`. Compute the condition
in the template instead, for example with `setc` into an `r` output.

### Operands

An operand reference names the whole register; there are no size
modifiers. The width of an operation comes from the mnemonic (`addl`
operates on 32 bits, `addq` and `add` on 64), and `setc` writes the low
byte of its operand's register. Hard registers are written `%%name`.

Memory operands have the form `DISP(BASE)`, where `BASE` is `%N`,
`%[NAME]` or a 64-bit `%%register`, and `DISP` is an optional signed
decimal or `0x` number. There is no index register, no scale, and no
symbol. `(%N)` with a base of `rsp` or `rbp` is refused by `invlpg` and
`movdqa` with `asm: memory base %rsp/%rbp unsupported`.

Statements are separated by newlines or `;`. A template has no comment
syntax: `#` is read as a mnemonic and refused. Mnemonics are lower case.

### Instructions

No operands (any operand text after the mnemonic is ignored):

`cli`, `sti`, `hlt`, `nop`, `pause`, `cpuid`, `rdtsc`, `rdmsr`,
`wrmsr`, `syscall`, `mfence`, `lfence`, `sfence`, `wbinvd`, `fninit`,
`pushfq`, `popfq`, `iretq`, `lretq`, `stac`, `clac`.

The registers these instructions read and write are fixed by the
hardware; bind them with the fixed-register constraints (`a`, `b`, `c`,
`d`, `S`, `D`) or register variables.

| Instruction | Operands |
|---|---|
| `int $N` | vector 0 to 255, written as a literal |
| `inb`, `inw`, `inl` | always `in %dx, %al` / `%ax` / `%eax`; the operand text is ignored |
| `outb`, `outw`, `outl` | always `out %al` / `%ax` / `%eax`, `%dx`; the operand text is ignored |
| `rdrand %N`, `rdseed %N` | 64-bit register; numbered form only |
| `setc %N` | low byte of the register; numbered form only |
| `sqrtsd %N, %M`, `sqrtss %N, %M` | both operands `x`; numbered forms only |
| `push`, `pushq` | `%N`, `%[NAME]`, a 64-bit `%%register`, or `$IMM` (32-bit, sign-extended) |
| `pop`, `popq` | `%N`, `%[NAME]` or a 64-bit `%%register` |
| `mov`, `movq`, `movabs` | see below |
| `add`, `or`, `and`, `sub`, `xor`, `cmp`, with suffix `q`, `l` or none | `SRC, DST` or `$IMM, DST`; registers only; any register name width is accepted, the suffix sets the operation size |
| `leaq Nf(%%rip), %%REG` | address of local label `N:` defined in the same template (`Nb` is accepted too); `REG` is a 64-bit name |
| `str %N`, `ltr %N` | task register |
| `lgdt %N`, `lidt %N` | the register holds the address of the descriptor-table pointer |
| `stmxcsr %N`, `ldmxcsr %N` | the register holds the address of the 4-byte MXCSR image |
| `invlpg (%N)` | the register holds the address |
| `movdqa (%N), %%xmm0`, `movdqa %%xmm0, (%N)` | `xmm0` only |
| `N:` | defines local label `N`, used only by `leaq` |

`mov`, `movq` and `movabs` always operate on 64 bits, except the two
16-bit segment forms:

| Form | Encoding |
|---|---|
| `mov SRC, DST`, both registers (`%N`, `%[NAME]`, 64-bit `%%register`) | 64-bit register move |
| `mov $IMM, DST` | sign-extended 32-bit immediate when it fits, else a 64-bit immediate; `movabs` always uses the 64-bit form |
| `mov DISP(BASE), DST` | 64-bit load |
| `mov SRC, DISP(BASE)` | 64-bit store; `$IMM` to memory is refused with `asm: mov $imm to memory unsupported` |
| `mov %%crN, DST`, `mov SRC, %%crN` | control register |
| `mov %%ax, %%SEG` | segment register (`es`, `cs`, `ss`, `ds`, `fs`, `gs`) |
| `mov $IMM, %%ax` | 16-bit immediate |

There is no `movl`, `movb`, `movw`, `lea`, `xchg`, `lock`, `test`,
`jmp`, `jcc`, `call`, `ret`, `ud2` or `int3`, and no memory operand for
the arithmetic instructions.

The port I/O instructions ignore their operands: `inb $0x60, %%al` is
encoded as `inb %dx, %al`. Use the `"a"` and `"Nd"` (or `"d"`)
constraints so that the value and the port are in `al` and `dx`:

```c
static inline unsigned char inb(unsigned short port)
{
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(unsigned short port, unsigned char v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
```

A system call with six arguments:

```c
static inline long syscall6(long n, long a1, long a2, long a3,
                            long a4, long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}
```

### Callee-saved registers on x86-64

From `-O1` the prologue saves `rbx` and `r12` to `r15` when an
operand is placed in one of them, whether by a constraint (`"=b"` for
`cpuid`), a register variable, or the allocator (the fourth `r` operand
gets `rbx` when `rax`, `rcx` and `rdx` are taken). At `-O0` nothing is
saved.

A callee-saved register that the template writes by name or that the
clobber list names is not saved at any level, and EmbCC gives no
diagnostic. On `x86_64-windows-gnu`, where `rsi` and `rdi` are also
callee-saved, an operand placed in either is not saved. In these cases
the template must preserve the register itself.

## AArch64

This section applies to every AArch64 triple, including
`aarch64-apple-darwin`.

### Constraints

| Letter | Meaning |
|---|---|
| `r`, `g` | a general register chosen by EmbCC |
| `i`, `n` | an integer constant expression, substituted into the template as a decimal literal |
| `=`, `+`, `&` | see [Output operands](#output-operands) |

Precedence: a [register variable](#register-variables) wins; then, if the
string contains `i` or `n` but neither `r` nor `g`, the operand is an
immediate; then `r` or `g` makes it a register. `"ri"` is therefore a
register. An `m` in a string that also contains `r` or `g` passes the
address (see [Input operands](#input-operands)).

An immediate whose expression is not a constant, and any constraint with
none of these letters (`m`, `w`, `x`, `Q`, `I`, `K`, `0`, ...), is
refused when the statement is generated:

```text
asm constraint "m" is not valid for aarch64
```

An output cannot be an immediate (`an asm output cannot be an immediate`).

A chosen register comes from `x9`, `x10`, `x11`, `x13`, `x14`, `x15`,
then `x0` to `x8`, skipping registers bound to another operand, listed
as clobbers, or named in the template. When none is left EmbCC reports
`no free register for an asm operand`.

**Flag outputs.** `__GCC_ASM_FLAG_OUTPUTS__` is defined, but flag-output
constraints are not implemented. Most spellings are refused (for example
`asm constraint "=@cceq" is not valid for aarch64`), but `"=@ccge"` and
`"=@ccgt"` contain `g` and are accepted as ordinary register outputs:
the output receives a register's contents, not the condition.

### Modifiers

`%N` prints a register operand as `wN` when its type is 4 bytes or
smaller and as `xN` otherwise. `%wN` and `%xN` (and `%w[NAME]`,
`%x[NAME]`) force the 32-bit or 64-bit name. An immediate prints as its
value; a modifier on an immediate is refused with
`a %w modifier on an immediate asm operand makes no sense`.

Instructions that need an X register reject a 4-byte operand printed as
`w`: write `%x0`, or give the operand a 64-bit type.

### Template syntax

GNU AArch64 syntax. Statements are separated by `;` or newlines; `//`
starts a comment. Mnemonics, system-register names and barrier options
are case-insensitive. Immediates may be written with or without `#`, in
decimal or `0x` hexadecimal. A branch target is a byte displacement from
the start of the instruction, written `.+N`, `.-N` or as a bare number;
labels are refused with
`labels in aarch64 inline asm are not supported yet (in "1: nop")`.

### Instructions

| Instruction | Operands |
|---|---|
| `nop`, `yield`, `wfe`, `wfi`, `sev`, `sevl` | none |
| `dsb OPT`, `dmb OPT` | `sy`, `st`, `ld`, `ish`, `ishst`, `ishld`, `nsh`, `nshst`, `nshld`, `osh`, `oshst`, `oshld` |
| `isb`, `isb sy` | none, or `sy` |
| `mrs Xt, SYSREG` | X register only |
| `msr SYSREG, Xt` | X register only |
| `msr daifset, #IMM`, `msr daifclr, #IMM` | immediate mask |
| `tlbi OP[, Xt]` | `vmalle1`, `vmalle1is` (no register); `vae1`, `vae1is`, `aside1`, `aside1is`, `vaae1`, `vaae1is`, `vale1`, `vale1is`, `vaale1`, `vaale1is` (with a register) |
| `dc OP, Xt` | `ivac`, `isw`, `csw`, `cisw`, `zva`, `cvac`, `cvau`, `cvap`, `civac` |
| `ic OP[, Xt]` | `ialluis`, `iallu` (no register); `ivau` (with a register) |
| `brk`, `hvc`, `smc`, `svc` `#IMM` | 0 to 0xffff |
| `ldr`, `str` `Rt, [Xn{, #OFF}]` | `Rt` a W, X or Q register; base an X register or `sp`; `OFF` unsigned and a multiple of the access size (4, 8, or 16 for Q) |
| `add`, `sub` `Rd, Rn, Rm` or `Rd, Rn, #IMM` | |
| `and`, `orr`, `eor` `Rd, Rn, Rm` | registers only |
| `mov Rd, Rm` or `mov Rd, #IMM` | any 64-bit immediate |
| `cmp Rn, Rm` or `cmp Rn, #IMM` | |
| `ret`, `br Xn`, `blr Xn` | |
| `b`, `bl`, `b.COND` `OFFSET` | `COND` one of `eq`, `ne`, `cs`, `hs`, `cc`, `lo`, `mi`, `pl`, `vs`, `vc`, `hi`, `ls`, `ge`, `lt`, `gt`, `le`; offset a multiple of 4 |
| `cbz`, `cbnz` `Rt, OFFSET` | offset a multiple of 4 |
| `.inst V[, V...]` | 32-bit words |

`SYSREG` is one of `nzcv`, `daif`, `fpcr`, `fpsr`, `currentel`, `pan`,
`spsel`, `sp_el0`, `spsr_el1`, `elr_el1`, `esr_el1`, `far_el1`,
`par_el1`, `midr_el1`, `mpidr_el1`, `id_aa64pfr0_el1`,
`id_aa64pfr1_el1`, `id_aa64isar0_el1`, `id_aa64isar1_el1`,
`id_aa64mmfr0_el1`, `id_aa64mmfr1_el1`, `id_aa64mmfr2_el1`,
`sctlr_el1`, `cpacr_el1`, `ttbr0_el1`, `ttbr1_el1`, `tcr_el1`,
`mair_el1`, `vbar_el1`, `contextidr_el1`, `tpidr_el1`, `tpidr_el0`,
`tpidrro_el0`, `cntfrq_el0`, `cntpct_el0`, `cntvct_el0`,
`cntp_tval_el0`, `cntp_ctl_el0`, `cntp_cval_el0`, `cntv_tval_el0`,
`cntv_ctl_el0`, `cntv_cval_el0`, `cntkctl_el1`, or the generic form
`S<op0>_<op1>_C<n>_C<m>_<op2>` (for example `s3_0_c12_c12_5`).

There are no exclusive or acquire/release loads and stores, no `ldrb`,
`ldrh`, `ldp` or `stp`, no pre- or post-indexed addressing, no
conditional select, no `eret`, and no floating-point or SIMD instruction
other than `ldr`/`str` of a Q register. Use `.inst` with the encoding for
anything else.

Other assembler diagnostics name the statement, for example
`'mrs' needs an X register, got 'w0' (in "mrs w0")` and
`offset must be a multiple of 8 in the unsigned range (in "ldr x0")`.

### Callee-saved registers on AArch64

A template that names x19 to x30 (including `x29` and `x30`) is refused:

```text
aarch64 asm names callee-saved register 'x19', which EmbCC does not save around an asm
```

and the same registers in the clobber list with
`aarch64 asm clobbers callee-saved register 'x20', which EmbCC does not save around an asm`.

### Example

```c
static inline unsigned long read_cntvct(void)
{
    unsigned long v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static long psci_version(void)
{
    register long x0 __asm__("x0") = 0x84000000;     /* PSCI_VERSION */
    register long x1 __asm__("x1") = 0;
    register long x2 __asm__("x2") = 0;
    register long x3 __asm__("x3") = 0;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return x0;
}

int second(const int *p)
{
    int w;
    __asm__("ldr %w0, [%1, #4]" : "=r"(w) : "r"(p));
    return w;
}
```

## ARM Cortex-M

This section applies to every Cortex-M triple: `thumbv6m-none-eabi`,
`thumbv8m.base-none-eabi`, `thumbv7m-none-eabi`, `thumbv7em-none-eabi`,
`thumbv7em-none-eabihf`, `thumbv8m.main-none-eabi` and
`thumbv8m.main-none-eabihf`. All of them use the same assembler; at the
Thumb-1 levels (ARMv6-M and ARMv8-M Baseline) it refuses what the core
does not have, and the ARMv8-M levels add the instructions in
[ARMv8-M security and acquire/release instructions](#armv8-m-security-and-acquirerelease-instructions).

### Constraints

| Letter | Meaning |
|---|---|
| `r`, `q`, `g`, `R` | a general register chosen by EmbCC |
| `i`, `n`, `I`, `J`, `K`, `L`, `M` | a constant, written into the template as a number (`ssat %0, %1, %2` with `"I"(8)` reads `ssat r0, 8, r1`); refused when the operand is not a constant |
| `m` | a general register chosen by EmbCC, holding the address of the operand (inputs) |
| `0`, `1`, ... (inputs) | the output of that number: the input must be the output's own lvalue, and the pair is taken as one `+r` operand, as CMSIS's `__SMLALD` writes its accumulator; another value is refused |
| `=`, `+`, `&` | see [Output operands](#output-operands) |

Constraints are read by the same rules as on x86-64. As a result the
x86-64 letters `a`, `b`, `c`, `d`, `S` and `D` are accepted and select
r0, r3, r1, r2, r6 and r7 respectively; r6 and r7 are callee-saved, so a
function that uses `S` or `D` saves them and keeps no other value there
(and `D` is refused in a function with a variable-length array, whose
frame r7 addresses). Do not use these letters. `x` is accepted and produces a
template that does not assemble. ARM letters such as `l`, `h`, `Q`, `t`
and `w`, alone, are refused:

```text
asm constraint "=l" is not supported (EmbCC handles a/b/c/d/S/D, 'r'/'q'/'g'/'m', 'x', and a register-asm variable)
```

A combination with `r`, such as `"Ir"`, is a register.

A chosen register comes from r0, r1, r2, r3 and r12, skipping registers
listed as clobbers or named in the template. When none is left EmbCC
reports `no free register for an asm operand`. One of these five
registers must also stay free for EmbCC to store the outputs; otherwise:

```text
the ARMv7-M backend cannot lower an asm with no scratch register left around it yet (function f)
```

An output wider than 4 bytes is refused:

```text
the ARMv7-M backend cannot lower an asm output wider than a register yet (function f)
```

### Modifiers

None. `%N` prints the register name (`r0` to `r12`, `sp`, `lr`, `pc`).
Any modifier is refused with
`asm template modifier '%w' is not supported for ARMv7-M`.

### Template syntax

GNU ARM unified syntax. Statements are separated by `;` or newlines; `@`
and `//` start a comment. Mnemonics are lower case and case-sensitive. A
`.w` or `.n` suffix is accepted and ignored: EmbCC chooses the 16-bit or
32-bit encoding itself. Registers are `r0` to `r15`, `sp`, `lr`, `pc`,
`ip` (r12) and `fp` (r11). A branch target is a byte displacement from
the start of the instruction, written `.+N`, `.-N` or as a bare number;
every branch uses the 32-bit encoding. Labels are not accepted (`1:` is
reported as an unknown instruction).

An immediate (`#...`), including a memory operand's offset, may be a
constant expression, as in GNU as: C's integer literals (decimal, `0x`,
`0b`, octal, with `u`/`l` suffixes) and operators (`~ * / % + - << >> &
^ |`) with parentheses. It runs to the next comma, so spaces are allowed:
FreeRTOS's Cortex-M4F port enables the FPU with
`orr r1, r1, #( 0xf << 20 )`. A symbol in an immediate is not evaluated
and is refused, as is a value the instruction cannot encode.

### Instructions

| Instruction | Operands |
|---|---|
| `nop`, `yield`, `wfe`, `wfi`, `sev` | none |
| `dsb`, `dmb`, `isb` | none, or `sy` |
| `cpsid`, `cpsie` | `i`, `f` or `if` |
| `mrs Rd, SPECREG`, `msr SPECREG, Rn` | `apsr`, `iapsr`, `eapsr`, `xpsr`, `ipsr`, `epsr`, `iepsr`, `msp`, `psp`, `primask`, `basepri`, `basepri_max`, `faultmask`, `control` (case-insensitive) |
| `bkpt #IMM` | 0 to 255 |
| `bx Rm`, `blx Rm` | |
| `mov`, `movs` `Rd, Rm` or `Rd, #IMM` | any 32-bit immediate |
| `movw`, `movt` `Rd, #IMM` | 0 to 0xffff |
| `mvn`, `mvns` `Rd, Rm` | |
| `clz`, `rbit`, `rev`, `rev16`, `revsh` `Rd, Rm` | `rev16`/`revsh` swap the bytes of each halfword / of the low one, sign-extended (CMSIS's `__REV16`, `__REVSH`) |
| `rrx`, `rrxs` `Rd, Rm` | rotate right one bit through the carry |
| `cmp`, `tst` `Rn, Rm` or `Rn, #IMM` | |
| `add`, `adds`, `sub`, `subs`, `and`, `ands`, `orr`, `orrs`, `eor`, `eors`, `bic`, `bics`, `adc`, `adcs`, `sbc`, `sbcs`, `rsb`, `rsbs` | `Rd, Rn, Rm` or `Rd, Rn, #IMM` (three operands) |
| `lsl`, `lsls`, `lsr`, `lsrs`, `asr`, `asrs`, `ror`, `rors` | `Rd, Rn, Rm` or `Rd, Rn, #0..31` |
| `mul Rd, Rn, Rm`, `udiv`, `sdiv` | |
| `mla`, `mls` `Rd, Rn, Rm, Ra` | `Ra + Rn*Rm`, `Ra - Rn*Rm` |
| `smull`, `umull`, `smlal`, `umlal` `RdLo, RdHi, Rn, Rm` | the 64-bit product (accumulated); `RdLo` not `RdHi`. `umaal` is the DSP extension's ([below](#the-dsp-extension)) |
| `bfi Rd, Rn, #LSB, #WIDTH`, `bfc Rd, #LSB, #WIDTH` | insert `Rn`'s low bits into / clear a field of `Rd`; `LSB` 0 to 31, `WIDTH` 1 to 32-`LSB` |
| `ubfx`, `sbfx` `Rd, Rn, #LSB, #WIDTH` | extract a field, zero- or sign-extended |
| `ldrd`, `strd` `Rt, Rt2, [Rn{, #OFF}]`, `[Rn, #OFF]!` or `[Rn], #OFF` | `OFF` a multiple of 4, -1020 to 1020; `Rt` and `Rt2` not `sp` or `pc` (and different for a load); with writeback `Rn` neither `pc` nor `Rt` nor `Rt2`; `ldrd Rt, Rt2, [pc, #OFF]` is a literal |
| `ldr`, `ldrb`, `ldrsb`, `ldrh`, `ldrsh`, `str`, `strb`, `strh` | `Rt, [Rn]` or `Rt, [Rn, #OFF]`; with writeback, `Rt, [Rn, #OFF]!` (pre-indexed) or `Rt, [Rn], #OFF` (post-indexed), `OFF` -255 to 255 and `Rn` neither `pc` nor `Rt` |
| `ldr Rt, [pc, #OFF]` | a literal, `OFF` from the word-aligned pc, -4095 to 4095 |
| `ldr Rt, =IMM` | any 32-bit constant, assembled as `movw` and `movt` |
| `ldrex Rt, [Rn{, #OFF}]` | `OFF` a multiple of 4, 0 to 1020 |
| `strex Rd, Rt, [Rn{, #OFF}]` | `OFF` a multiple of 4, 0 to 1020 |
| `ldrexb`, `ldrexh` `Rt, [Rn]`; `strexb`, `strexh` `Rd, Rt, [Rn]` | `Rd` neither `Rt` nor `Rn` |
| `clrex` | none |
| `msr apsr_nzcvq, Rn`, `msr apsr_nzcvqg, Rn` | the flags; `apsr_nzcvqg` also the DSP extension's GE bits (not on ARMv6-M or ARMv8-M Baseline) |
| `ssat Rd, #1..32, Rn`, `usat Rd, #0..31, Rn` | an optional `lsl #0..31` or `asr #1..31` (not on ARMv6-M or ARMv8-M Baseline) |
| `sxtb`, `sxth`, `uxtb`, `uxth` `Rd, Rm` | an optional `ror #8`, `#16` or `#24` (not on ARMv6-M or ARMv8-M Baseline, where only the 16-bit form with r0-r7 exists) |
| The DSP extension's | see [The DSP extension](#the-dsp-extension) |
| ARMv8-M only: `lda`, `ldab`, `ldah`, `ldaex`, `ldaexb`, `ldaexh`, `stl`, `stlb`, `stlh`, `stlex`, `stlexb`, `stlexh`, `tt`, `ttt`, `tta`, `ttat`, `sg`, `bxns`, `blxns`, and on Mainline `vlstm`, `vlldm` | see [ARMv8-M security and acquire/release instructions](#armv8-m-security-and-acquirerelease-instructions) |
| `b`, `bl`, `beq`, `bne`, `bcs`, `bhs`, `bcc`, `blo`, `bmi`, `bpl`, `bvs`, `bvc`, `bhi`, `bls`, `bge`, `blt`, `bgt`, `ble` | `OFFSET` (even) |
| `cbz`, `cbnz` | `Rn, OFFSET`: `Rn` r0 to r7, `OFFSET` 4 to 130, forward |
| `push`, `pop` | a register list, `{r4-r7, lr}` or without braces, `r4, lr` |
| `ldm`, `ldmia`, `ldmfd`, `ldmdb`, `ldmea`, `stm`, `stmia`, `stmea`, `stmdb`, `stmfd` | `Rn{!}, {LIST}`: two or more registers, never `sp`; no `pc` in a store; not `pc` and `lr` together in a load; not `Rn` with writeback |
| `svc #IMM` | 0 to 255 |
| `it`, `itt`, `ite`, ... (up to four instructions) | a condition; each instruction in the block carries its condition suffix (`moveq`) |
| `vmov Sn, Rt`, `vmov Rt, Sn` | |
| `vldm`, `vldmia`, `vldmdb`, `vstm`, `vstmia`, `vstmdb` | `Rn{!}, {Sm-Sn}`: consecutive single-precision registers; `db` needs writeback |
| `vpush`, `vpop` | `{Sm-Sn}` |

Mnemonics, register names and conditions are case-insensitive, as in
GNU as (`MRS %0, primask`). The two-operand forms (`adds r0, #1`,
`lsls r2, #2`) are the three-operand ones with the destination repeated.
Loads and stores take `[Rn, Rm]` and `[Rn, Rm, lsl #N]` (N 0 to 3) too.
The barriers take `sy` or its number `0xF`; on ARMv8-M, `mrs`/`msr` also
name `msplim`, `psplim` and the TrustZone `_ns` registers (see
[ARMv8-M stack limits](#armv8-m-stack-limits)).

Points that differ from the GNU assembler (in inline asm; a `.s`/`.S`
file is assembled as GNU as does it, see [embas](tools/embas.md#gnu-syntax-assembly)):

- `ldm`, `stm`, the branches and `ldr Rt, [pc, #OFF]` always use the
  32-bit encoding, and `ldr Rt, =IMM` is `movw`/`movt` rather than a load
  from a literal pool.
- Inside an IT block, a flag-setting instruction (`adds`, `movs`, ...)
  is refused, because its 16-bit encoding sets no flags there, and so is
  a branch that is not the block's last instruction. A block left open at
  the end of an asm statement is refused, since it would make the
  compiler's next instructions conditional.
- A barrier option other than `sy` is refused with
  ``only the `sy` barrier option is supported; "ish" is not``.

There is no floating-point instruction beyond `vmov`, `vldm`/`vstm` and
`vpush`/`vpop` (`vmrs`, `vmsr`, `vldr`, ...), and no doubleword exclusive
(`ldrexd`/`strexd`), which no Cortex-M has: they are refused by name
(`ldrexd is not an M-profile instruction: no Cortex-M core has a
doubleword exclusive`).

On ARMv6-M (`thumbv6m-none-eabi`) and ARMv8-M Baseline
(`thumbv8m.base-none-eabi`), every 32-bit encoding a statement produces is
checked against what the core has, in inline asm and in a `.s` file
alike: an instruction outside the set -- `ldr.w`, an `add` whose
immediate needs the 32-bit form, `it` -- is refused by name rather than
assembled into a HardFault, and so are the Main Extension's special
registers (`basepri`, `basepri_max`, `faultmask` and their `_ns` forms):

```text
"add.w r0, r1, #4096" is not an ARMv8-M Baseline instruction: it encodes as a 32-bit Thumb-2 form that core does not have
"basepri" is not a special register of ARMv8-M Baseline: it is the Main Extension's
```

So `mla`, the long multiplies, `rrx`, the bit fields, `ldrd`/`strd`, and
`rev16`/`revsh` of a high register are refused there, as llvm-mc refuses
them; `rev16` and `revsh` of `r0`-`r7` are the 16-bit forms every core
has. ARMv6-M has `bl`, `mrs`, `msr` and the barriers; ARMv8-M Baseline adds
`b.w`, `movw`, `movt`, `sdiv`, `udiv`, the exclusives (`ldrex`/`strex`
with an offset, the byte and halfword forms, `clrex`), the
acquire/release family, `tt`/`ttt`/`tta`/`ttat` and `sg`, and the 16-bit
`cbz`, `cbnz`, `bxns` and `blxns` (see
[Targets](targets.md#armv8-m-baseline)).

### The DSP extension

ARMv7E-M (`thumbv7em-none-eabi[hf]`, `-mcpu=cortex-m4` or `cortex-m7`) and
ARMv8-M Mainline with the extension (`-mcpu=cortex-m33`,
`-march=armv8-m.main+dsp`) have the DSP instructions, and so does EmbCC's
assembler for them, in inline asm and in `.s` files. They are what
CMSIS's `cmsis_gcc.h` (`__SADD16` ... `__SMLALD`, selected by
`__ARM_FEATURE_DSP`) and EmbCC's `<arm_acle.h>` (`__sadd16`, `__smlad`,
`__ssat`, ...: ACLE's DSP and SIMD32 intrinsics, under clang's names) are
made of:

| Instructions | Operands |
|---|---|
| `sadd16`, `sasx`, `ssax`, `ssub16`, `sadd8`, `ssub8`, and the same with `q`, `sh`, `u`, `uq` and `uh` in place of `s` (`qadd16`, `shsub8`, `uasx`, `uqsub16`, `uhadd8`, ...) | `Rd, Rn, Rm` |
| `qadd`, `qsub`, `qdadd`, `qdsub` | `Rd, Rm, Rn`: `qsub rd, rm, rn` is `rd = sat(rm - rn)` |
| `sel` | `Rd, Rn, Rm`, each byte by the GE bits |
| `usad8`; `usada8` | `Rd, Rn, Rm`; `Rd, Rn, Rm, Ra` |
| `smuad`, `smuadx`, `smusd`, `smusdx`, `smulbb`, `smulbt`, `smultb`, `smultt`, `smulwb`, `smulwt`, `smmul`, `smmulr` | `Rd, Rn, Rm` |
| `smlad`, `smladx`, `smlsd`, `smlsdx`, `smlabb`, `smlabt`, `smlatb`, `smlatt`, `smlawb`, `smlawt`, `smmla`, `smmlar`, `smmls`, `smmlsr` | `Rd, Rn, Rm, Ra` |
| `smlald`, `smlaldx`, `smlsld`, `smlsldx`, `smlalbb`, `smlalbt`, `smlaltb`, `smlaltt`, `umaal` | `RdLo, RdHi, Rn, Rm`, `RdLo` not `RdHi` (`umaal`: `RdHi:RdLo = Rn*Rm + RdLo + RdHi`) |
| `ssat16 Rd, #1..16, Rn`, `usat16 Rd, #0..15, Rn` | |
| `pkhbt Rd, Rn, Rm{, lsl #0..31}`, `pkhtb Rd, Rn, Rm{, asr #1..32}` | `pkhtb` with no shift is `pkhbt Rd, Rm, Rn`, as GNU as and llvm-mc encode it |
| `sxtb16`, `uxtb16` `Rd, Rm`; `sxtab16`, `uxtab16`, `sxtab`, `sxtah`, `uxtab`, `uxtah` `Rd, Rn, Rm` | an optional `ror #8`, `#16` or `#24` |

No operand may be `sp` or `pc` (in ARM state, `pc`). The rotation and shift keywords are
case-insensitive and the `#` is optional, so CMSIS's
`"sxtb16 %0, %1, ROR %2"` with an `"i"` operand assembles.

On a part without the extension -- ARMv7-M, `thumbv8m.main-none-eabi`
alone (as with clang), ARMv6-M, ARMv8-M Baseline -- each is refused by
name, in inline asm and in a `.s` file alike, as llvm-mc refuses it:

```text
"sadd16" is an instruction of the DSP extension, which ARMv7-M lacks: it is ARMv7E-M's (-mcpu=cortex-m4, cortex-m7, --target=thumbv7em-none-eabi) and ARMv8-M Mainline's with the extension (-mcpu=cortex-m33, -march=armv8-m.main+dsp)
```

Every ARMv7-A part has them, and so does the ARM-state assembler
([ARM state](#arm-state-armv7-a)), where `<arm_acle.h>` declares them too.
`tests/golden/thumb-dsp.sh` checks every form against llvm-mc, byte for
byte and by disassembly, compares the refusals core by core, and runs
each instruction on a Cortex-M4 under QEMU against a C model of it;
`tests/golden/arm-asm-more.sh` does the same for the multiplies, bit
fields and pairs above, and for all of them in ARM state on a
Cortex-A15.

### ARM state (ARMv7-A)

On `armv7a-none-eabi[hf]` inline asm and `.s` files take the vocabulary
above, A32-encoded, the DSP extension's instructions included, with these
differences, each as llvm-mc has it for `armv7a`:

- Any instruction may carry a condition, with no IT block: `moveq r0,
  #1`, `sadd16ne r0, r1, r2`, `ldrdlt r0, r1, [r2]`.
- `sp` may be an operand of the DSP instructions, the multiplies, the
  reversals, `rrx` and the bit fields; `pc` may not.
- `ldrd`/`strd` take an even register and the next one (`r0, r1` ...
  `r12, sp`), and an offset of -255 to 255 in every addressing form.
- `ssat`/`usat` take `asr #32`.
- `ldrexd Rt, Rt2, [Rn]` and `strexd Rd, Rt, Rt2, [Rn]` exist (`Rt` even,
  `Rt2` the next; `Rd` none of the others).
- `mrs`/`msr` name `cpsr` (and its fields), not the M-profile special
  registers; `mrc`/`mcr` reach the system control coprocessor; `svc`,
  `bkpt` and `udf` take A32's wider immediates; `cbz`, `cbnz`, `tbb` and
  `tbh` are refused.

A few UNPREDICTABLE forms llvm-mc assembles are refused, in either state:
`pc` as a multiply's accumulator or a reversal's register, an `ldrd`
writeback through `pc`, and a `strexd` status register that is one of
its other operands.

### ARMv8-M security and acquire/release instructions

On the ARMv8-M triples (`thumbv8m.main-none-eabi[hf]` and
`thumbv8m.base-none-eabi`), the security extension's instructions and the
load-acquire/store-release family assemble; on any other level each is
refused by name (`tt is an instruction of ARMv8-M's security extension,
and this is not an ARMv8-M target`, `lda is an ARMv8-M instruction
(load-acquire and store-release), and this is not an ARMv8-M target`).

| Instruction | Operands | Meaning |
|---|---|---|
| `tt`, `ttt`, `tta`, `ttat` | `Rd, Rn` (`Rd` not `sp` or `pc`) | the test target: the MPU, SAU and IDAU attributes of the address in `Rn`; `t` as unprivileged code would see them, `a` as the other security state would (Secure state only) |
| `sg` | none | the secure gateway: the first instruction of an entry the Non-secure state may branch to |
| `bxns`, `blxns` | `Rm` | `bx`/`blx` that may leave the Secure state, when `Rm`'s bit 0 is clear |
| `vlstm`, `vlldm` | `Rn` (or `Rn, {d0-d15}`) | Mainline only: the lazy save and restore of the floating-point context around a call to the Non-secure state |
| `lda`, `ldab`, `ldah` | `Rt, [Rn]` | load-acquire, word, byte, halfword |
| `ldaex`, `ldaexb`, `ldaexh` | `Rt, [Rn]` | load-acquire exclusive |
| `stl`, `stlb`, `stlh` | `Rt, [Rn]` | store-release |
| `stlex`, `stlexb`, `stlexh` | `Rd, Rt, [Rn]` | store-release exclusive; `Rd` 0 when it took |

None of these takes an offset, and `sp` and `pc` are refused as their
registers. `tasm_vocabulary_v8m` in `src/arch/thumb/asm.c` lists every
form across the registers, and `tests/golden/thumbv8mbase-encoding.sh`
checks each against llvm-mc (`-mattr=+8msecext`) byte for byte, at
Baseline and Mainline. A Secure program normally reaches `tt` through
`<arm_cmse.h>` ([Targets](targets.md#trustzone-m-cmse)):

```c
static inline unsigned test_target(void *p)
{
    unsigned v;
    __asm__ volatile("tt %0, %1" : "=r"(v) : "r"(p));
    return v;          /* bits 7:0 the MPU region, 16 MPU-region-valid, ... */
}
```

`sg`, `bxns` and `blxns` are what the compiler and the linker emit for
`cmse_nonsecure_entry` and `cmse_nonsecure_call`; written by hand, a
`bxns` to the Non-secure state must first clear every register and flag
that holds a secret, as those do.

### ARMv8-M stack limits

On the ARMv8-M triples, `mrs` and `msr` name the stack-limit
registers, and the core enforces them. A push or a stack-pointer
adjustment that would take the stack below its limit is not made, and
takes a UsageFault with CFSR.STKOF (bit 20) set instead. An RTOS sets
PSPLIM to the bottom of each task's stack, so an overflow faults rather
than overwriting whatever lies below.

| Name | Register |
|---|---|
| `msplim` | the main stack's limit |
| `psplim` | the process stack's limit |
| `msplim_ns`, `psplim_ns` | the Non-secure limits, from Secure code |

```c
static inline void set_psplim(unsigned limit)
{
    __asm__ volatile("msr psplim, %0" : : "r"(limit));
}

static inline unsigned get_msplim(void)
{
    unsigned v;
    __asm__ volatile("mrs %0, msplim" : "=r"(v));
    return v;
}
```

The encodings are clang's. `tests/golden/thumbv8m-splim.sh` runs a
thread past its PSPLIM on QEMU's Cortex-M33 and checks the UsageFault.
The ARMv7-M triples have no stack-limit registers, and refuse the names:
`"msplim" is not an ARMv7-M special register`.

### Callee-saved registers on ARM Cortex-M

A template that names r4 to r11 (or `fp`) is refused:

```text
ARMv7-M asm names callee-saved register 'r4', which EmbCC does not save around an asm
```

and the same registers in the clobber list with
`ARMv7-M asm clobbers callee-saved register 'r5', which EmbCC does not save around an asm`.

### Example

```c
static inline unsigned irq_save(void)
{
    unsigned primask;
    __asm__ volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask) : : "memory");
    return primask;
}

static inline void irq_restore(unsigned primask)
{
    __asm__ volatile("msr primask, %0" : : "r"(primask) : "memory");
}

static inline int clz32(unsigned x)
{
    int r;
    __asm__("clz %0, %1" : "=r"(r) : "r"(x));
    return r;
}
```

## RISC-V

This section applies to `riscv32-unknown-elf` and `riscv64-unknown-elf`.

### Constraints

| Letter | Meaning |
|---|---|
| `r`, `q`, `g`, `R` | a general register chosen by EmbCC |
| `i`, `n` | the constant, computed into a general register chosen by EmbCC |
| `m` | a general register chosen by EmbCC, holding the address of the operand (inputs) |
| `=`, `+`, `&` | see [Output operands](#output-operands) |

Constraints are read by the same rules as on x86-64. As a result the
x86-64 letters `a`, `b`, `c`, `d`, `S` and `D` are accepted and select
x0 (`zero`), x3 (`gp`), x1 (`ra`), x2 (`sp`), x6 (`t1`) and x7 (`t2`)
respectively. Do not use these letters. `x` is accepted and produces a
template that does not assemble. RISC-V letters such as `f`, `I`, `J`,
`K` and `A`, alone, are refused with
`asm constraint "=f" is not supported (EmbCC handles a/b/c/d/S/D, 'r'/'q'/'g'/'m', 'x', and a register-asm variable)`.

Because `i` gives a register, an instruction with an immediate field
cannot take an `i` operand: `addi %0, %1, %2` with `"i"(5)` is refused
with `addi wants an immediate`. Write `add %0, %1, %2`, or put the
constant in the template text.

A chosen register comes from `t0` to `t6`, then `a0` to `a7`, skipping
registers listed as clobbers or named in the template. When none is left
EmbCC reports `no free register for an asm operand`. An output wider
than a register (an 8-byte type on RV32) is refused:

```text
the RV32 backend cannot lower an asm output wider than a register yet (function f)
```

### Modifiers

None. `%N` prints the ABI name of the register (`t0`, `a0`, ...). Any
modifier is refused with
`asm template modifier '%z' is not supported for RISC-V`.

### Template syntax

GNU RISC-V syntax. Statements are separated by `;` or newlines; `#` and
`//` start a comment. Mnemonics are lower case and case-sensitive.
Registers are the ABI names (`zero`, `ra`, `sp`, `gp`, `tp`, `t0`-`t6`,
`s0`-`s11`, `a0`-`a7`), `fp` (s0), and `x0` to `x31`; the float
registers are `f0` to `f31` or `ft0`-`ft11`, `fs0`-`fs11`, `fa0`-`fa7`.
Memory operands are
`OFF(REG)` or `(REG)` with a 12-bit signed offset. A branch or jump
target is a byte displacement from the start of the instruction, written
`.+N`, `.-N` or as a bare number. Labels are not accepted.

### Instructions

| Instruction | Operands |
|---|---|
| `nop`, `ret`, `ebreak`, `unimp`, `ecall`, `mret`, `sret`, `wfi`, `fence.i` | none |
| `fence` | none (meaning `fence iorw, iorw`), or two sets of `i`, `o`, `r`, `w` |
| `csrr Rd, CSR` | |
| `csrw`, `csrs`, `csrc` `CSR, Rs` | |
| `csrrw`, `csrrs`, `csrrc` `Rd, CSR, Rs` | |
| `csrwi`, `csrsi`, `csrci` `CSR, IMM` | `IMM` 0 to 31 |
| `csrrwi`, `csrrsi`, `csrrci` `Rd, CSR, IMM` | `IMM` 0 to 31 |
| `add`, `sub`, `and`, `or`, `xor`, `slt`, `sltu` `Rd, Rs1, Rs2` | |
| `addi`, `andi`, `ori`, `xori`, `slti`, `sltiu` `Rd, Rs1, IMM` | 12-bit signed |
| `sll`, `srl`, `sra` `Rd, Rs1, Rs2`; `slli`, `srli`, `srai` `Rd, Rs1, SHAMT` | `SHAMT` 0 to XLEN-1 |
| `mul`, `mulh`, `mulhsu`, `mulhu`, `div`, `divu`, `rem`, `remu` | three registers |
| `lb`, `lbu`, `lh`, `lhu`, `lw`, `sb`, `sh`, `sw` | `Rt, OFF(Rs)` |
| `beq`, `bne`, `blt`, `bge`, `bltu`, `bgeu` `Rs1, Rs2, OFFSET` | even, within 4 KiB |
| `beqz`, `bnez`, `bltz`, `bgez`, `blez`, `bgtz` `Rs, OFFSET` | even, within 4 KiB |
| `j OFFSET`, `jal [Rd,] OFFSET` | even, within 1 MiB |
| `jr Rs`, `jalr Rs`, `jalr Rd, Rs`, `jalr Rd, OFF(Rs)` | |
| `lui`, `auipc` `Rd, IMM` | 20-bit |
| `li Rd, IMM` | any constant; expanded to the instruction sequence the code generator uses |
| `mv`, `not`, `neg`, `seqz`, `snez` `Rd, Rs` | |

With the F extension (`-march=` naming `f`, or `g`), the floating-point
instructions, in their single (`.s`) form; with D also their double
(`.d`) form. A rounding mode, where one is shown, is `rne`, `rtz`, `rdn`,
`rup`, `rmm` or `dyn`, and is `dyn` when left out (`rne` for the
conversions that are always exact, `fcvt.d.s` and `fcvt.d.w[u]`, as
llvm-mc writes them).

| Instruction | Operands |
|---|---|
| `flw`, `fsw`, `fld`, `fsd` | `Ft, OFF(Rs)` |
| `fadd`, `fsub`, `fmul`, `fdiv` | `Fd, Fs1, Fs2 [, RM]` |
| `fsqrt` | `Fd, Fs [, RM]` |
| `fmadd`, `fmsub`, `fnmsub`, `fnmadd` | `Fd, Fs1, Fs2, Fs3 [, RM]` |
| `fsgnj`, `fsgnjn`, `fsgnjx`, `fmin`, `fmax` | `Fd, Fs1, Fs2` |
| `fmv`, `fneg`, `fabs` | `Fd, Fs` |
| `feq`, `flt`, `fle` | `Rd, Fs1, Fs2` |
| `fclass` | `Rd, Fs` |
| `fcvt.w`, `.wu`, `.l`, `.lu` `.s`/`.d` | `Rd, Fs [, RM]` (`l`, `lu` RV64 only) |
| `fcvt.s`/`.d` `.w`, `.wu`, `.l`, `.lu` | `Fd, Rs [, RM]` |
| `fcvt.s.d`, `fcvt.d.s` | `Fd, Fs [, RM]` |
| `fmv.x.w`, `fmv.w.x` (and `fmv.x.s`, `fmv.s.x`); `fmv.x.d`, `fmv.d.x` (RV64) | `Rd, Fs` or `Fd, Rs` |
| `frcsr`, `frrm`, `frflags` | `Rd` |
| `fscsr`, `fsrm`, `fsflags` | `[Rd,] Rs` |
| `fsrmi`, `fsflagsi` | `[Rd,] IMM`, 0 to 31 |

An F instruction on a target without F, or a D one without D, is refused
with `fsd needs the D extension, which -march= does not name`. The same
instructions are accepted in `.s` and `.S` files. There is no `"f"`
constraint yet: name the float register in the template, and pass
values through memory or with `fmv.w.x`/`fmv.x.w`.

RV64 only: `addw`, `subw`, `addiw`, `sllw`, `srlw`, `sraw`, `slliw`,
`srliw`, `sraiw`, `mulw`, `divw`, `divuw`, `remw`, `remuw`, `ld`, `sd`,
`negw`, `sext.w`, `lwu`. At RV32 most of them are refused, for example
with `ld is an RV64 instruction and this is RV32`. Three are not: `negw`
and `sext.w` are encoded with their RV64 opcodes, which an RV32 core does
not implement, and `lwu` is encoded as `lw`.

`CSR` is a name from the table below, or a number from 0 to 0xfff.

| Group | CSRs |
|---|---|
| Floating point | `fflags`, `frm`, `fcsr` |
| Machine information | `mvendorid`, `marchid`, `mimpid`, `mhartid` |
| Machine trap setup | `mstatus`, `misa`, `medeleg`, `mideleg`, `mie`, `mtvec`, `mcounteren` |
| Machine trap handling | `mscratch`, `mepc`, `mcause`, `mtval`, `mip` |
| Memory protection | `pmpcfg0`, `pmpcfg1` (RV32 only), `pmpaddr0`, `pmpaddr1` |
| Supervisor | `sstatus`, `sie`, `stvec`, `scounteren`, `sscratch`, `sepc`, `scause`, `stval`, `sip`, `satp` |
| Counters | `cycle`, `time`, `instret`, and `cycleh`, `timeh`, `instreth` (RV32 only) |
| Machine counters | `mcycle`, `minstret`, `mcountinhibit`, and `mcycleh`, `minstreth` (RV32 only) |

An RV32-only CSR at RV64 is refused with `CSR "cycleh" exists only on RV32`,
and an unknown name or a number out of range with
`"mfoo" is not a CSR this assembler knows`.

There are no atomic instructions (`lr`, `sc`, `amo*`), no compressed
instructions written explicitly (`c.*`), no `la`, `call` or `tail`, no `rdcycle`/`rdtime`/`rdinstret` (use
`csrr`), and no two-register `bgt`, `ble`, `bgtu` or `bleu`.

### Callee-saved registers on RISC-V

A template that names s0 to s11 (or `fp`) is refused:

```text
RISC-V asm names callee-saved register 's1', which EmbCC does not save around an asm
```

and the same registers in the clobber list with
`RISC-V asm clobbers callee-saved register 's2', which EmbCC does not save around an asm`.

### Example

```c
static inline unsigned long hart_id(void)
{
    unsigned long id;
    __asm__ volatile("csrr %0, mhartid" : "=r"(id));
    return id;
}

static inline unsigned long irq_save(void)
{
    unsigned long m;
    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(m) : : "memory");
    return m;
}

static inline void irq_restore(unsigned long m)
{
    __asm__ volatile("csrw mstatus, %0" : : "r"(m) : "memory");
}

long sbi_call(long ext, long fid, long arg0)
{
    long r;
    __asm__ volatile("mv a7, %1\n\tmv a6, %2\n\tmv a0, %3\n\tecall\n\tmv %0, a0"
                     : "=r"(r) : "r"(ext), "r"(fid), "r"(arg0)
                     : "a0", "a1", "a6", "a7", "memory");
    return r;
}
```

## MIPS32

This section applies to `mipsel-none-elf`.

### Constraints

| Letter | Meaning |
|---|---|
| `r`, `d`, `g` | a general register chosen by EmbCC |
| `m` | a general register chosen by EmbCC, holding the address of the operand; for an `=m` output the template stores through it |
| `i`, `n`, `I` to `P` | the constant, written into the template as a decimal number |
| `=`, `+`, `&` | see [Output operands](#output-operands) |

A constraint with `r`, `d`, `g` or `m` anywhere gets a register; one with
only the constant letters must be given an integer constant expression.
Anything else, a non-constant `i` included, is refused with
`asm constraint "=a" is not valid for MIPS`.

A chosen register comes from `t0` to `t9`, then `v0`, `v1` and `a0` to
`a3`, skipping registers listed as clobbers or named in the template.
When none is left EmbCC reports `no free register for an asm operand`.
An input wider than a register passes its low word; an output wider than
a register is refused:

```text
the MIPS32 backend cannot lower an asm output wider than a register yet (function f) [asm w=4 size=4]
```

### Modifiers

None. `%N` prints the register with its `$` (`$t0`, `$a1`, ...) or the
constant. Any modifier is refused with
`asm template modifier '%z' is not supported for MIPS`.

### Template syntax

GNU MIPS syntax. A template starts in `.set reorder` mode, as GCC's and
clang's do: the assembler puts a `nop` in the delay slot of every branch
and jump, so the instruction written after a transfer runs after it, not
in its slot. A template that schedules its own delay slots says `.set
noreorder` (and may `.set push` and `.set pop` around it); a template
written for GCC with an explicit slot but without `.set noreorder` gets
an extra `nop`, as it would from GNU as. The other `.set` options that
name what EmbCC emits anyway (`at`, `noat`, `macro`, `nomacro`,
`mips32r2`, ...) are accepted; `.set mips16`, `micromips`, `mips32r6`
and the 64-bit ISAs are refused. Statements are separated by `;` or
newlines; `#` and `//` start a comment. Mnemonics are lower case. Registers are `$0` to `$31` and the
ABI names with `$` (`$zero`, `$at`, `$v0`, `$v1`, `$a0`-`$a3`,
`$t0`-`$t9`, `$s0`-`$s7`, `$k0`, `$k1`, `$gp`, `$sp`, `$fp` or `$s8`,
`$ra`). Memory operands are `OFF(REG)` or `(REG)` with a 16-bit signed
offset; an offset or immediate may be a constant expression (`(16 + 4 *
3)($sp)`), and `%hi(N)` and `%lo(N)` of a number give the halves a
`lui`/`addiu` pair adds up to. A branch target is a byte displacement
from the delay slot, a multiple of 4 written as a number (`b 8` skips the
slot and the instruction after it), or `.+N` / `.-N` from the branch
itself. Labels are not accepted in a template; they are in a
[file-scope block](#on-cortex-m-risc-v-mips32-and-avr) and a `.S` file,
where `jal sym`, `%hi(sym)`, `%lo(sym)` and `la` also take symbols.

### Instructions

| Instruction | Operands |
|---|---|
| `nop`, `ssnop`, `ehb`, `eret`, `wait`, `syscall` | none |
| `break [CODE]` | 0 to 1023 |
| `sync [STYPE]` | 0 to 31 |
| `di [Rt]`, `ei [Rt]` | |
| `addu`, `subu`, `and`, `or`, `xor`, `nor`, `slt`, `sltu`, `movn`, `movz`, `mul` `Rd, Rs, Rt` | |
| `sllv`, `srlv`, `srav`, `rotrv` `Rd, Rt, Rs` | the value, then the amount |
| `addiu`, `slti`, `sltiu` `Rt, Rs, IMM` | 16-bit signed |
| `andi`, `ori`, `xori` `Rt, Rs, IMM` | 0 to 0xffff |
| `sll`, `srl`, `sra`, `rotr` `Rd, Rt, SA` | 0 to 31 |
| `lui Rt, IMM` | 0 to 65535, or `%hi(N)` |
| `lb`, `lbu`, `lh`, `lhu`, `lw`, `sb`, `sh`, `sw`, `ll`, `sc`, `lwl`, `lwr`, `swl`, `swr` | `Rt, OFF(Rs)` |
| `mult`, `multu` `Rs, Rt` | |
| `div`, `divu` | `$zero, Rs, Rt` only (below) |
| `mfhi`, `mflo`, `mthi`, `mtlo` `Rd` | |
| `clz`, `clo`, `seb`, `seh`, `wsbh` `Rd, Rs` | |
| `ext`, `ins` `Rt, Rs, POS, SIZE` | within the word |
| `mfc0`, `mtc0` `Rt, $N[, SEL]` | `SEL` 0 to 7 |
| `teq Rs, Rt[, CODE]` | 0 to 1023 |
| `beq`, `bne` `Rs, Rt, OFFSET`; `blez`, `bgtz`, `bltz`, `bgez`, `beqz`, `bnez` `Rs, OFFSET`; `b`, `bal` `OFFSET` | a multiple of 4 from -131072 to 131068 |
| `jr Rs`, `jalr Rs`, `jalr Rd, Rs` | `Rd` and `Rs` different |
| `j TARGET`, `jal TARGET` | a register (`j $ra` is `jr`, `jal $t9` is `jalr`); `.+N` from the jump, encoded as `b`/`bal` within 128 KiB; or a number, the target's place in its 256 MiB region |
| `jal SYM`, `j SYM`, `lui Rt, %hi(SYM)`, `addiu Rt, Rs, %lo(SYM)`, `lw Rt, %lo(SYM)(Rs)` (and the other loads and stores), `la Rt, SYM` | a `.S` file or a file-scope block only: `R_MIPS_26`, `R_MIPS_HI16`, `R_MIPS_LO16`; `SYM` may have a `+K` or `-K` |
| `move Rd, Rs` | `or Rd, Rs, $zero` |
| `li Rd, IMM` | any 32-bit constant; the code generator's sequence (one or two instructions) |
| `not Rd, Rs`, `negu Rd, Rs` | `nor Rd, Rs, $zero`; `subu Rd, $zero, Rs` |

`div` and `divu` with two registers are assembler macros in GNU `as`,
which add a divide-by-zero trap, and are refused:
`this div is an assembler macro (it adds a divide-by-zero trap); write div $zero, rs, rt and read the quotient with mflo`.
`neg` is not accepted for the same reason (it is the trapping `sub`);
write `negu`. An immediate out of its field is refused with, for example,
`addiu immediate 70000 does not fit its signed 16-bit field`.

`%lo` is only an `addiu`'s or a load's or store's offset, and `%hi`
only a `lui`'s operand: `%hi` is rounded for the sign extension those
instructions give `%lo`, so `ori $t0, $t0, %lo(sym)` would compute a
wrong address for half of all symbols, and is refused (`%lo(symbol) is an
addiu's or a load's or store's offset: %hi is rounded for its sign
extension`). The operators of position-independent, small-data and TLS
code (`%got`, `%call16`, `%gp_rel`, ...) are refused, and so is a branch
to a symbol the block or file does not define.

There are no floating-point or coprocessor 1 instructions, no trapping
`add`, `addi` or `sub`, no DSP, MIPS16 or microMIPS instructions, no
`cache`, `pref` or `rdhwr`, and no `madd` or `msub`.

### Callee-saved registers on MIPS32

A template that names s0 to s7, gp, fp, ra, k0 or k1 is refused, and so
is the same register in the clobber list:

```text
MIPS asm names register '$s1', which EmbCC does not save around an asm
MIPS asm clobbers register 's2', which EmbCC does not save around an asm
```

`$sp` may be named, for a template that reads it. `$at` may be named and
changed: no value is kept in it across an `asm`.

### Example

```c
static inline unsigned read_status(void)     /* CP0 Status */
{
    unsigned s;
    __asm__ volatile("mfc0 %0, $12" : "=r"(s));
    return s;
}

static inline unsigned irq_save(void)
{
    unsigned s;
    __asm__ volatile("di %0; ehb" : "=r"(s) : : "memory");
    return s & 1;
}

static inline unsigned bswap32(unsigned x)
{
    unsigned r;
    __asm__("wsbh %0, %1; rotr %0, %0, 16" : "=r"(r) : "r"(x));
    return r;
}
```

## LoongArch64

This section applies to `loongarch64-unknown-elf`.

### Constraints

| Letter | Meaning |
|---|---|
| `r`, `g` | a general register chosen by EmbCC |
| `m` | a general register chosen by EmbCC, holding the address of the operand; it is written into the template as `$REG, 0`, the base and offset a load or store takes |
| `i`, `n`, `I`, `J`, `K` | the constant, written into the template as a decimal number |
| `=`, `+`, `&` | see [Output operands](#output-operands) |

Anything else, a non-constant `i` included, is refused with
`asm constraint "=a" is not valid for LoongArch`. A chosen register comes
from `t0` to `t8`, then `a0` to `a7`, skipping registers listed as
clobbers or named in the template. A register variable must be one of
those (`register long x __asm__("a0")`); another is refused with
`register variable bound to 's3' is not supported for LoongArch asm (use
a0-a7 or t0-t8)`.

### Modifiers

None. `%N` prints the register with its `$` (`$t0`, `$a1`, ...) or the
constant; any modifier is refused with `asm template modifier '%z' is
not supported for LoongArch`.

### Template syntax

GNU LoongArch syntax, as llvm-mc reads it. Registers are `$r0` to `$r31`
and the psABI names with `$` (`$zero`, `$ra`, `$tp`, `$sp`, `$a0`-`$a7`,
`$t0`-`$t8`, `$fp` or `$s9`, `$s0`-`$s8`). Every operand is written out:
a load or store is `ld.w $a0, $a1, 0`, and an offset or immediate may be
a constant expression. A branch target is a byte offset from the branch,
a multiple of 4, as a number or `.+N` / `.-N`. Labels and symbols are
accepted in a [file-scope block](#on-cortex-m-risc-v-mips32-and-avr) and
a `.S` file, where `b`/`bl sym`, `call36`, `tail36`, `la.pcrel`,
`la.local`, `la`, `la.global` and the `%pc_hi20`/`%pc_lo12`,
`%got_pc_hi20`/`%got_pc_lo12`, `%abs_*` and `%call36` operators take
them. Statements are separated by `;` or newlines; `#` and `//` start a
comment.

### Instructions

| Instruction | Operands |
|---|---|
| `nop`, `ret`, `ertn` | none |
| `move Rd, Rj`; `jr Rj`; `li.w`, `li.d` `Rd, IMM` | `li` builds any constant, as llvm-mc's does |
| `add.w/d`, `sub.w/d`, `slt`, `sltu`, `and`, `or`, `xor`, `nor`, `andn`, `orn`, `sll/srl/sra/rotr.w/d`, `maskeqz`, `masknez`, `mul.w/d`, `mulh.w/wu/d/du`, `mulw.d.w/wu`, `div/mod.w/wu/d/du` | `Rd, Rj, Rk` |
| `addi.w`, `addi.d`, `slti`, `sltui` | `Rd, Rj, IMM`, 12-bit signed |
| `andi`, `ori`, `xori` | `Rd, Rj, IMM`, 0 to 4095 |
| `lu52i.d Rd, Rj, IMM`; `lu12i.w`, `lu32i.d`, `pcaddi`, `pcalau12i`, `pcaddu12i`, `pcaddu18i` `Rd, IMM` | 12- and 20-bit signed |
| `slli`, `srli`, `srai`, `rotri` `.w` / `.d` | `Rd, Rj, SA`, 0-31 / 0-63 |
| `ext.w.b`, `ext.w.h`, `clo/clz/cto/ctz.w/d`, `revb.2h/4h/2w/d`, `bitrev.4b/8b/w/d`, `cpucfg`, `rdtimel.w`, `rdtimeh.w`, `rdtime.d`, `iocsrrd.b/h/w/d`, `iocsrwr.b/h/w/d` | `Rd, Rj` |
| `bstrpick.w/d`, `bstrins.w/d` | `Rd, Rj, MSB, LSB` |
| `alsl.w`, `alsl.d` | `Rd, Rj, Rk, SA`, 1 to 4 |
| `ld.b/bu/h/hu/w/wu/d`, `st.b/h/w/d` | `Rd, Rj, OFF`, 12-bit signed |
| `ldx.*`, `stx.*` | `Rd, Rj, Rk` |
| `ldptr.w/d`, `stptr.w/d`, `ll.w/d`, `sc.w/d` | `Rd, Rj, OFF`, a multiple of 4 in -32768..32764 |
| `amswap`, `amadd`, `amand`, `amor`, `amxor`, `ammax`, `ammin` (`.w`, `.d`, `.wu`, `.du`, with and without `_db`) | `Rd, Rk, Rj`; Rd may not be Rk or Rj |
| `beq`, `bne`, `blt`, `bge`, `bltu`, `bgeu`, `bgt`, `ble`, `bgtu`, `bleu` | `Rj, Rd, TARGET`, +-128 KiB |
| `beqz`, `bnez` | `Rj, TARGET`, +-4 MiB; `bltz`, `bgez`, `bgtz`, `blez` `Rj, TARGET` +-128 KiB |
| `b`, `bl` | `TARGET`, +-128 MiB |
| `jirl Rd, Rj, OFF` | |
| `dbar`, `ibar`, `break`, `syscall`, `idle` | 0 to 32767 |
| `csrrd`, `csrwr` `Rd, CSR`; `csrxchg Rd, Rj, CSR` | CSR 0 to 16383; csrxchg's Rj not `$r0` or `$r1` |

Floating-point and vector instructions are refused, as is anything else
outside the list: `asm instruction "fadd.d $fa0, $fa0, $fa0" is not in
the LoongArch vocabulary`.

### Callee-saved registers on LoongArch64

EmbCC saves nothing around an asm, so a template or clobber list naming
`$fp` or `$s0`-`$s8` is refused (`LoongArch asm names callee-saved
register '$s0', which EmbCC does not save around an asm`), and one naming
`$tp` or `$r21`, which the psABI reserves, likewise. A template that
calls (`bl`, `jirl`, `syscall`, ...) clobbers `ra`, `a0`-`a7` and
`t0`-`t8`, whatever its clobber list says.

### Example

```c
static inline void irq_disable(void)
{
    long ie = 0, mask = 4;                  /* CRMD.IE */
    __asm__ volatile("csrxchg %0, %1, 0x0" : "+r"(ie) : "r"(mask) : "memory");
}
```

## Xtensa

This section applies to `xtensa-none-elf` (the ESP32's LX6 and the
ESP32-S3's LX7, windowed ABI).

### Constraints

| Letter | Meaning |
|---|---|
| `r`, `a`, `g` | an address register chosen by EmbCC (`a` is GCC's Xtensa letter for them) |
| `m` | an address register chosen by EmbCC, holding the address of the operand; it is written into the template as `aN, 0`, the base and offset a load or store takes |
| `i`, `n`, `I`-`P` | the constant, written into the template as a decimal number |
| `=`, `+`, `&` | see [Output operands](#output-operands) |

Anything else, a non-constant `i` included, is refused with
`asm constraint "b" is not valid for Xtensa`. An operand is one 32-bit
register: a `long long` one is refused (`Xtensa asm operand 0 is 8
bytes`). A chosen register comes from `a10`-`a13`, then `a2`-`a6` and
`a8`-`a9`, skipping registers listed as clobbers, named in the template or
changed by a call in it; never `a0` or `a1` (the return address and the
stack pointer), `a7` (the frame base of a function that calls `alloca`) or
`a14`/`a15` (the code generator's scratch). A register variable must be
one of those (`register int x __asm__("a10")`); another is refused with
`register variable bound to 'a7' is not supported for Xtensa asm (use
a2-a6 or a8-a13)`.

### Modifiers

None. `%N` prints the register (`a10`) or the constant; any modifier is
refused with `asm template modifier '%x' is not supported for Xtensa`.

### Template syntax

GNU Xtensa syntax, as Espressif's GNU as reads it. Registers are `a0` to
`a15`, and `sp` is `a1`. Every operand is written out: a load or store is
`l32i a2, a3, 8`, and an offset or immediate may be a constant expression.
A branch, loop, `j`, `callN` or `l32r` target is `.+N` or `.-N` bytes from
the instruction, or a numeric label of the template (`1:` referred to as
`1b` or `1f`); a named label is refused, since one template may be emitted
more than once. A special register is named as GNU as names it (`rsr a2,
ps`, `rsr.ps a2`, `wsr a3, intenable`) or by number (`rsr a2, 230`).
Statements are separated by `;` or newlines; `#` and `//` start a
comment.

A template cannot name a symbol: its bytes carry no relocation, so
`call8 f` is refused (`"f" is a symbol, and inline asm cannot reach one`)
-- call through a register with `callx8`, or write the code in a `.S`
file or a [file-scope block](#on-cortex-m-risc-v-mips32-and-avr), where
symbols, `movi aN, sym` and `.literal` work. For the same reason `call`
and `l32r` to a `.+N` target are refused in a template: their encoding
depends on the instruction's own address, rounded to a word, which a
template does not know. `movi` takes a 12-bit signed constant; a larger
one is an `"r"` operand.

### Instructions

| Instruction | Operands |
|---|---|
| `nop`, `ill`, `isync`, `rsync`, `esync`, `dsync`, `memw`, `extw`, `rfe`, `rfde`, `rfwo`, `rfwu`, `syscall`, `simcall`, `ret`, `retw` | none |
| `add`, `sub`, `and`, `or`, `xor`, `addx2/4/8`, `subx2/4/8`, `mull`, `mul16u`, `mul16s`, `quos`, `quou`, `rems`, `remu`, `min`, `max`, `minu`, `maxu`, `moveqz`, `movnez`, `movltz`, `movgez`, `src` | `ar, as, at` |
| `mov ar, as`; `neg`, `abs ar, at`; `nsa`, `nsau at, as`; `sll ar, as`; `srl`, `sra ar, at`; `movsp at, as` | |
| `ssl`, `ssr`, `ssa8l`, `jx`, `callx0`, `callx4`, `callx8`, `callx12` | `as` |
| `movi at, IMM` | -2048 to 2047 |
| `addi at, as, IMM`; `addmi at, as, IMM` | -128 to 127; a multiple of 256 in -32768..32512 |
| `slli ar, as, SA`; `srli`, `srai ar, at, SA`; `ssai SA`; `extui ar, at, SHIFT, BITS` | 1-31; 0-15 and 0-31; 0-31; 0-31 and 1-16 |
| `sext`, `clamps ar, as, B` | 7 to 22 |
| `l8ui`, `l16ui`, `l16si`, `l32i`, `s8i`, `s16i`, `s32i`, `l32ai`, `s32ri`, `s32c1i` | `at, as, OFF`, scaled by the size: 0-255, even 0-510, a multiple of 4 in 0-1020 |
| `l32e`, `s32e` | `at, as, OFF`, a multiple of 4 in -64..-4 |
| `l32r at, TARGET` | 4 to 262144 bytes back (in a file or block only) |
| `beq`, `bne`, `blt`, `bge`, `bltu`, `bgeu`, `bany`, `bnone`, `ball`, `bnall`, `bbc`, `bbs` | `as, at, TARGET`, -124..131 from the branch |
| `beqz`, `bnez`, `bltz`, `bgez` | `as, TARGET`, -2044..2051 |
| `beqi`, `bnei`, `blti`, `bgei` / `bltui`, `bgeui` | `as, K, TARGET`, K one of -1, 1-8, 10, 12, 16, 32, 64, 128, 256 / 2-8, 10, 12, 16, 32, 64, 128, 256, 32768, 65536 |
| `bbci`, `bbsi` (and `bbci.l`, `bbsi.l`) | `as, BIT, TARGET` |
| `loop`, `loopnez`, `loopgtz` | `as, END`, 4..259 bytes past the loop instruction |
| `j TARGET`; `call0`, `call4`, `call8`, `call12 TARGET` | +-128 KiB; +-512 KiB to a word-aligned target (in a file or block only) |
| `entry as, FRAME` | a multiple of 8 in 0..32760 |
| `rotw N`; `rsil at, LEVEL`; `waiti LEVEL`; `rfi LEVEL`; `break S, T` | -8..7; 0-15; 0-15; 1-15; 0-15 each |
| `rsr`, `wsr`, `xsr at, SR` (or `rsr.SR at`) | the ESP32's special registers by name -- `ps`, `epc1`-`epc7`, `eps2`-`eps7`, `excsave1`-`excsave7`, `depc`, `exccause`, `excvaddr`, `intenable`, `interrupt`, `intset`, `intclear`, `ccount`, `ccompare0`-`2`, `vecbase`, `sar`, `lbeg`, `lend`, `lcount`, `scompare1`, `atomctl`, `windowbase`, `windowstart`, `prid`, `cpenable`, `br`, `acclo`, `acchi`, `m0`-`m3`, `memctl`, `ddr`, `ibreakenable`, `ibreaka0/1`, `dbreaka0/1`, `dbreakc0/1`, `icount`, `icountlevel`, `debugcause`, `configid0/1`, `misc0`-`3`, `mmid` -- with GNU's read/write/exchange rules (`intset` is write-only, `prid` read-only), or by number 0-255 |
| `rur`, `wur at, threadptr` (or `rur.threadptr at`) | |

A leading `_` (GNU's "do not transform") is accepted on any of them. The
density option's 16-bit forms are refused with `ret.n is a 16-bit
instruction of the density option, which this assembler does not emit:
write ret, its 24-bit form`; anything else outside the list with
`asm instruction "ssa8b a2" is not in the Xtensa vocabulary`.

### The register window

Under the windowed ABI `a0` holds the return address and `a1` the stack
pointer. A template may read or write them where it names them, but they
are never an operand's register and a clobber list naming either is
refused (`Xtensa asm clobbers 'a0', which holds this function's return
address under the windowed ABI; EmbCC does not save it around an asm`).
`a2`-`a15` belong to this function's window and nothing needs saving: a
value live across an asm keeps out of every register the asm changes --
its operands', its clobbers' and the template's.

A call in a template is a windowed call. `call8`/`callx8` hands its
callee the window from `a8`, which the callee may change, so `a8`-`a15`
are clobbered -- as `call4` clobbers `a4`-`a15` and `call12` `a12`-`a15` --
whether or not the clobber list says so, and no operand is put there.
`call0`/`callx0` would write `a0` and are refused (`callx0 in Xtensa asm
writes a0, which holds this function's return address under the windowed
ABI`).

### Example

```c
static inline unsigned irq_save(void)
{
    unsigned ps;
    __asm__ volatile("rsil %0, 3" : "=r"(ps) :: "memory");
    return ps;
}

static inline void irq_restore(unsigned ps)
{
    __asm__ volatile("wsr %0, ps\n rsync" :: "r"(ps) : "memory");
}

static inline unsigned cycles(void)
{
    unsigned c;
    __asm__ volatile("rsr %0, ccount" : "=r"(c));
    return c;
}
```

## SPARC

This section applies to `sparc-none-elf` (SPARC V8, the LEON3).

### Constraints

| Letter | Meaning |
|---|---|
| `r`, `g` | an integer register chosen by EmbCC |
| `m` | a register chosen by EmbCC, holding the address of the operand; it is written into the template as `[%o0]`, as GCC prints a SPARC memory operand |
| `i`, `n`, `I`-`P` | the constant, written into the template as a decimal number (`I` is GCC's signed 13-bit) |
| `0`-`9` | an input in the register of the output it names |
| `=`, `+`, `&` | see [Output operands](#output-operands) |

Anything else, a non-constant `i` included, is refused with
`asm constraint "a" is not valid for SPARC`. An operand is one 32-bit
register: a `long long` one is refused (`SPARC asm operand 0 is 8 bytes`).
A chosen register comes from `%o0`-`%o5`, then `%l0`-`%l5` and
`%i0`-`%i5`, skipping registers listed as clobbers, named in the template
or changed by a call in it; never a global (`%g1`-`%g4`, `%l6`, `%l7` and
`%o7` are the code generator's scratch, `%g5`-`%g7` the system's), `%sp`,
`%fp` or `%i7`. A register variable must be one of those (`register int x
__asm__("o3")`); another is refused with `register variable bound to 'g1'
is not supported for SPARC asm (use %o0-%o5, %l0-%l5 or %i0-%i5)`.

### Modifiers

`%cN` prints a constant operand bare, and `%rN` prints `%g0` for the
constant 0 (as GCC's `r`). Any other modifier is refused with `asm
template modifier '%H' is not supported for SPARC`.

### Template syntax

GNU SPARC syntax, as GNU as and llvm-mc read it: `op rs1, reg_or_imm,
rd`, registers `%g0`-`%g7`, `%o0`-`%o7`, `%l0`-`%l7`, `%i0`-`%i7`,
`%r0`-`%r31`, `%sp` and `%fp` (in an extended asm's text a literal
register is `%%o0`); an address `[%o0 + %o1]`, `[%fp - 8]`, `[%o0 +
%lo(0x1234)]`; a constant a C expression, or `%hi()`/`%lo()` of one. A
branch or call target is `.+N` or `.-N` bytes from the instruction, or a
numeric label of the template (`1:`, `1b`, `1f`). Delay slots are the
template's own: write the instruction that fills one after the branch,
call or `retl`; nothing is moved into or out of a template. Statements are
separated by `;` or newlines; `!` and `//` start a comment, and `#` does
at a line's start. A template cannot name a symbol (`"foo" names a symbol,
and inline asm cannot reach one`): call through a register (`call %o2`),
or write the code in a `.S` file or a file-scope block, where `call sym`,
`%hi(sym)`, `%lo(sym)` and `set sym, rd` are relocated.

### Instructions

| Instruction | Operands |
|---|---|
| `add`, `addcc`, `addx`, `addxcc`, `sub`, `subcc`, `subx`, `subxcc`, `and`, `andcc`, `andn`, `andncc`, `or`, `orcc`, `orn`, `orncc`, `xor`, `xorcc`, `xnor`, `xnorcc`, `umul`, `umulcc`, `smul`, `smulcc`, `udiv`, `udivcc`, `sdiv`, `sdivcc`, `taddcc`, `tsubcc`, `taddcctv`, `tsubcctv`, `mulscc`, `save`, `restore` | `rs1, reg_or_imm, rd`, the immediate -4096..4095; `save`/`restore` also with none |
| `sll`, `srl`, `sra` | `rs1, reg_or_imm, rd`, the count 0-31 |
| `ld`, `ldub`, `lduh`, `ldsb`, `ldsh`, `ldd`, `ldstub`, `swap` | `[address], rd` (`ldd`: an even rd) |
| `st`, `stb`, `sth`, `std` | `rd, [address]` |
| the same with `a` (`lda`, `stba`, `swapa`...) | `[rs1 + rs2] ASI` (0-255) |
| `casa` | `[rs1] ASI, rs2, rd` |
| `sethi` | a 22-bit constant or `%hi(...)`, `rd` |
| `ba`, `bn`, `bne`, `be`, `bg`, `ble`, `bge`, `bl`, `bgu`, `bleu`, `bcc`, `bcs`, `bpos`, `bneg`, `bvc`, `bvs`, `b`, `bnz`, `bz`, `bgeu`, `blu`, each also `,a` | `TARGET`, -8388608..8388604 bytes |
| `call` | `TARGET` (+-2 GiB), or an address in registers (`jmpl ..., %o7`) |
| `jmpl` | `address, rd` (no brackets) |
| `jmp`, `rett`, `flush`, `iflush` | `address` |
| `ret`, `retl`, `nop`, `stbar` | none |
| `ta`, `tn`, `tne`, `te`, `tg`, `tle`, `tge`, `tl`, `tgu`, `tleu`, `tcc`, `tcs`, `tpos`, `tneg`, `tvc`, `tvs`, `t`, `tnz`, `tz`, `tgeu`, `tlu` | `N` (0-127), `%rs1`, `%rs1 + N`, `%rs1 + %rs2` |
| `rd` | `%y`, `%psr`, `%wim`, `%tbr` or `%asr1`-`%asr31`, `rd` |
| `wr` | `rs1, reg_or_imm, %y` (or another state register), or `reg_or_imm, %y` |
| `unimp` | a 22-bit constant |
| `mov` | `reg_or_imm, rd`; `%y` (or another state register), `rd`; `reg_or_imm, %y` |
| `cmp`, `btst`, `bset`, `bclr`, `btog` | `rs1, reg_or_imm` / `reg_or_imm, rd` |
| `tst`; `not`, `neg` | `rs`; `rs, rd` or `rd` |
| `inc`, `inccc`, `dec`, `deccc` | `rd` or `imm, rd` |
| `clr`, `clrb`, `clrh` | `rd` (`clr`) or `[address]` |
| `set` | `value, rd`: `mov` when the value fits 13 bits, `sethi` alone when its low ten bits are 0, else `sethi` and `or` -- GNU's choice |

Floating-point and coprocessor instructions are refused with `asm
instruction "faddd" is a floating-point instruction: EmbCC compiles soft
float, and this assembler has no floating-point vocabulary`; SPARC V9's
(`ldx`, `membar`, `,pt`...) with `asm instruction "ldx" is SPARC V9's, and
the LEON3 is a V8`; anything else outside the list with `asm instruction
"foo" is not in the SPARC vocabulary`.

### The register window

`%sp`, `%fp` and `%i7` (the stack and frame pointers and the return
address) may be read or written where a template names them, but they
are never an operand's register and a clobber list naming one is refused
(`SPARC asm clobbers '%sp', which holds this function's stack pointer;
EmbCC does not save it around an asm`). Everything else in the window --
the locals and the ins -- is this function's own, so nothing needs
saving: a value live across an asm keeps out of every register the asm
changes, which is its operands', its clobbers' and every register its
text names, clobber list or not. A `call` (or a `jmpl` linking through
`%o7`) in a template also changes the outs and `%g1`-`%g4`, as any call
does, and no operand is put there.

### Example

```c
static inline unsigned irq_disable(void)
{
    unsigned psr;
    __asm__ volatile("rd %%psr, %0\n"
                     " or %0, 0xf00, %%g1\n"      /* PIL 15 */
                     " wr %%g1, %%psr\n nop\n nop\n nop"
                     : "=r"(psr) :: "g1", "memory");
    return psr;
}

static inline unsigned long long umul64(unsigned a, unsigned b)
{
    unsigned lo, hi;
    __asm__("umul %2, %3, %0\n rd %%y, %1" : "=r"(lo), "=r"(hi)
            : "r"(a), "r"(b));
    return (unsigned long long)hi << 32 | lo;
}
```

## PowerPC

This section applies to `powerpc-none-eabi` (32-bit PowerPC, the e500
and e200 and the classic cores' shared vocabulary).

### Constraints

| Letter | Meaning |
|---|---|
| `r`, `b`, `g` | a general register chosen by EmbCC (never r0, so `b` is `r`) |
| `m` | a register chosen by EmbCC, holding the address of the operand; it is written into the template as `0(rN)`, the D-form GCC prints |
| `i`, `n`, `I`-`P` | the constant, written into the template as a decimal number |
| `0`-`9` | an input in the register of the output it names |
| `=`, `+`, `&` | see [Output operands](#output-operands) |

Anything else is refused with `asm constraint "a" is not valid for
PowerPC`; a `long long` operand with `PowerPC asm operand 0 is 8 bytes`. A
chosen register comes from r3-r8, then r14-r30, skipping registers listed
as clobbers, named in the template or changed by a call in it; never r0,
r1 (the stack pointer), r2 or r13 (the EABI's small-data anchors), r9-r12
(the code generator's scratch) or r31 (the frame base under `alloca`). A
register variable must be one of those (`register int x
__asm__("r5")`); another is refused with `register variable bound to 'r11'
is not supported for PowerPC asm (use r3-r8 or r14-r30)`.

### Modifiers

`%UN` and `%XN` print nothing (an `"m"` operand here is always the plain
D-form, so `lwz%U1%X1 %0, %1` is `lwz`), and `%cN` prints a constant
bare. Any other modifier is refused with `asm template modifier '%L' is
not supported for PowerPC`.

### Template syntax

GNU PowerPC syntax, as GNU as and llvm-mc read it. A register is a bare
number in a register's place (`addi 3, 4, 1`, GCC's own spelling), `rN`
or `%rN`, or `sp`; a CR field `crN` or a number; a CR bit a number or an
expression of `crN`, `lt`, `gt`, `eq`, `so` and `un` (`4*cr7+eq`); a
constant a C expression, with `@ha`, `@h` or `@l` for its halves; a
memory operand `d(rA)`. A branch target is `.+N` or `.-N` bytes from the
instruction, or a numeric label of the template (`1:`, `1b`, `1f`).
Statements are separated by `;` or newlines; `#` and `//` start a
comment. A template cannot name a symbol (`"foo" names a symbol, and
inline asm cannot reach one`): call through CTR (`mtctr`, `bctrl`), or
write the code in a `.S` file or a file-scope block, where `bl sym`,
`b sym`, a conditional branch to a symbol and `sym@ha`/`@h`/`@l` are
relocated.

### Instructions

| Instruction | Operands |
|---|---|
| `add`, `addc`, `adde`, `subf`, `subfc`, `subfe`, `mullw`, `divw`, `divwu` (each also `o`, `.`, `o.`); `mulhw`, `mulhwu` (also `.`); `sub`, `subc` (also `.`) | `rt, ra, rb` |
| `addze`, `addme`, `subfze`, `subfme`, `neg` (also `o`, `.`, `o.`) | `rt, ra` |
| `and`, `andc`, `or`, `orc`, `xor`, `nand`, `nor`, `eqv`, `slw`, `srw`, `sraw` (also `.`) | `ra, rs, rb` |
| `cntlzw`, `extsb`, `extsh`, `mr`, `not` (also `.`); `srawi` (also `.`) | `ra, rs`; `ra, rs, SH` |
| `addi`, `addis`, `addic`, `addic.`, `mulli`, `subfic`; `li`, `lis`, `la`, `subi`, `subis`, `subic`, `subic.` | `rt, ra, SIMM` (`addis`/`lis` 16 bits either way); `rt, SIMM`; `rt, d(ra)` |
| `ori`, `oris`, `xori`, `xoris`, `andi.`, `andis.` | `ra, rs, UIMM` |
| `rlwinm`, `rlwimi`, `rlwnm` (also `.`) | `ra, rs, SH (rb), MB, ME` |
| `rotlwi`, `rotrwi`, `rotlw`, `slwi`, `srwi`, `clrlwi`, `clrrwi`, `extlwi`, `extrwi`, `inslwi`, `insrwi`, `clrlslwi` (also `.`) | as the ISA's table of extended mnemonics |
| `cmpw`, `cmplw`, `cmpwi`, `cmplwi` | `[crN,] ra, rb/SIMM/UIMM` |
| `cmp`, `cmpl`, `cmpi`, `cmpli` | `crN, 0, ra, rb/IMM` |
| `lwz`, `lbz`, `lhz`, `lha`, `stw`, `stb`, `sth` and their `u` forms, `lmw`, `stmw` | `rt, d(ra)` |
| `lwzx`, `lbzx`, `lhzx`, `lhax`, `stwx`, `stbx`, `sthx` and their `ux` forms, `lwbrx`, `lhbrx`, `stwbrx`, `sthbrx`, `lwarx`, `stwcx.` | `rt, ra, rb` |
| `dcbf`, `dcbst`, `dcbt`, `dcbtst`, `dcbz`, `dcbi`, `icbi`, `tlbsx`, `tlbivax` | `ra, rb` |
| `sync`, `msync`, `lwsync`, `eieio`, `isync`, `tlbwe`, `tlbre`, `tlbsync`, `rfi`, `rfci`, `rfmci`, `sc`, `trap`, `nop`; `mbar` | none; `[MO]` |
| `tw`, `twi`; `tweq`, `twlt`, `twgt`, `twne`, `twle`, `twge`, `twnl`, `twng`, `twllt`, `twlgt`, `twlle`, `twlge`, `twlnl`, `twlng`, `twu` and their `i` forms | `TO, ra, rb/SIMM`; `ra, rb/SIMM` |
| `crand`, `cror`, `crxor`, `crnand`, `crnor`, `creqv`, `crandc`, `crorc`; `crset`, `crclr`, `crmove`, `crnot`; `mcrf` | three CR bits; one or two; two CR fields |
| `mfcr`, `mtcr`; `mtcrf` | `rt`; `FXM, rs` |
| `isel`; `isellt`, `iselgt`, `iseleq` | `rt, ra, rb, BC`; `rt, ra, rb` |
| `mfmsr`, `mtmsr`, `wrtee`; `wrteei` | `rt`; `0` or `1` |
| `mfspr`, `mtspr` | `rt, SPR` / `SPR, rs`: a number 0-1023 or a name -- `xer`, `lr`, `ctr`, `dec`, `srr0`, `srr1`, `csrr0`, `csrr1`, `mcsrr0`, `mcsrr1`, `dear`, `esr`, `ivpr`, `ivor0`-`ivor15`, `ivor32`-`ivor35`, `pid`, `pid1`, `pid2`, `decar`, `tcr`, `tsr`, `tbl`, `tbu`, `tbwl`, `tbwu`, `sprg0`-`sprg7`, `usprg0`, `pvr`, `pir`, `svr`, `hid0`, `hid1`, `l1csr0`, `l1csr1`, `mmucsr0`, `mmucfg`, `bucsr`, `mas0`-`mas4`, `mas6`, `mas7`, `tlb0cfg`, `tlb1cfg`, `dbsr`, `dbcr0`-`dbcr2`, `iac1`, `iac2`, `dac1`, `dac2`, `mcsr`, `mcar`, `spefscr`, `ear`, `dsisr`, `dar`, `sdr1` |
| `mfNAME`, `mtNAME` for each name above; `mfsprg`, `mtsprg`; `mftb`, `mftbu` | `rt` / `rs`; `rt, N` / `N, rs` (N 0-7); `rt` |
| `b`, `bl`; `ba`, `bla` | `TARGET`, +-32 MiB; an absolute address |
| `bc`, `bcl`, `bca`, `bcla` | `BO, BI, TARGET` (-32768..32764) |
| `bclr`, `bclrl`, `bcctr`, `bcctrl`; `blr`, `blrl`, `bctr`, `bctrl` | `BO, BI [, BH]`; none |
| `b<cond>` for `lt`, `le`, `eq`, `ge`, `gt`, `nl`, `ne`, `ng`, `so`, `ns`, `un`, `nu`, each also `l`, `a`, `la`, `lr`, `lrl`, `ctr`, `ctrl` | `[crN,] TARGET`, or `[crN]` |
| `bt`, `bf` (also `l`, `a`, `la`, `lr`, `lrl`, `ctr`, `ctrl`); `bdnz`, `bdz` (also `l`, `a`, `la`, `lr`, `lrl`); `bdnzt`, `bdnzf`, `bdzt`, `bdzf` (likewise) | `BI, TARGET`; `TARGET`; `BI, TARGET` |

Writing LR (`mtlr`, `mtspr lr`) or a branch that links makes the function
save its own LR around the template; its operands and every value live
across it then keep out of r0 and r3-r12, which a call changes. Floating
point is refused with `asm instruction "fadd" is a floating-point
instruction: EmbCC compiles soft float, and this assembler has no
floating-point vocabulary`, as are AltiVec, SPE, 64-bit and string
instructions by name; a branch-prediction suffix with `beq+: a
branch-prediction hint (+/-) is not supported: write the branch without
it`; anything else outside the list with `asm instruction "foo" is not in
the PowerPC vocabulary`.

### Callee-saved registers on PowerPC

r14-r31 survive a call. When an asm changes one -- an operand in it, a
clobber naming it, or the template's text naming it (a bare `li 14, 0`
counts: EmbCC assembles the template once with its operands in
placeholders to learn which registers it names) -- the function's prologue
saves it and its epilogue restores it, at every optimization level. r1,
r2 and r13 may not be clobbered (`PowerPC asm clobbers 'r1', which holds
the stack pointer; EmbCC does not save it around an asm`), and r31 may
not be changed in a function that calls `alloca`, where it is the frame
base.

### Example

```c
static inline unsigned irq_save(void)
{
    unsigned msr;
    __asm__ volatile("mfmsr %0\n wrteei 0" : "=r"(msr) :: "memory");
    return msr;
}

static inline void irq_restore(unsigned msr)
{
    __asm__ volatile("wrtee %0" :: "r"(msr) : "memory");
}

static inline int cas(int *p, int old, int new_)
{
    int prev;
    __asm__ volatile("1: lwarx %0, 0, %1\n"
                     "   cmpw %0, %2\n"
                     "   bne 2f\n"
                     "   stwcx. %3, 0, %1\n"
                     "   bne 1b\n"
                     "2:"
                     : "=&r"(prev) : "r"(p), "r"(old), "r"(new_)
                     : "cc", "memory");
    return prev;
}
```

## Renesas RX

This section applies to `rx-none-elf` (RXv1: the RX600/RX610, RX100 and
RX200 cores).

### Constraints

| Letter | Meaning |
|---|---|
| `r`, `g` | a general register chosen by EmbCC |
| `m` | a general register chosen by EmbCC, holding the address of the operand; it is written into the template as `[rN]`, the memory operand an RX instruction takes |
| `i`, `n`, `I`-`P`, and GCC's `Int08`, `Sint08`, `Sint16`, `Sint24`, `Uint04` | the constant, written into the template as `#N`, as GCC's RX port prints it |
| `=`, `+`, `&` | see [Output operands](#output-operands) |

Anything else, a non-constant `i` included, is refused with
`asm constraint "b" is not valid for RX`. An operand is one 32-bit
register: a `long long` one is refused (`RX asm operand 0 is 8 bytes`).
A chosen register comes from `r1`-`r4`, then `r6`-`r12`, skipping
registers listed as clobbers or named in the template; never `r0` (the
stack pointer), `r5`, `r14` or `r15` (the code generator's scratch) or
`r13` (the frame base of a function that calls `alloca`). A register
variable must be one of those (`register int x __asm__("r1")`); another is
refused with `register variable bound to 'r5' is not supported for RX asm
(use r1-r4 or r6-r12)`.

### Modifiers

None. `%N` prints the register (`r3`), the constant (`#5`) or the memory
operand (`[r3]`); any modifier is refused with `asm template modifier
'%x' is not supported for RX`. In a [naked function](#naked-functions)
`%cN` prints a constant without its `#`.

### Template syntax

GNU RX syntax, as rx-elf-as reads it. Registers are `r0` to `r15` (GNU has
no `sp`), immediates `#expr`, memory `[rN]`, `dsp[rN]`, `[ri, rb]`,
`[rN+]` and `[-rN]`, with `.b`, `.w`, `.l`, `.ub` or `.uw` after a memex
source (`add 4[r1].w, r2`); a displacement is in bytes, a multiple of the
access size. A branch target is `.+N` or `.-N` bytes from the branch, or
a numeric label of the template (`1:` referred to as `1b` or `1f`); a
named label is refused, since one template may be emitted more than once.
A branch written without a size takes the shortest form that reaches, as
GNU as relaxes it: `bra` `.s`/`.b`/`.w`/`.a`, `bsr` `.w`/`.a`,
`beq`/`bne` `.s`/`.b`/`.w` and then the inverse condition over a
`bra.a`, any other condition `.b` and then its inverse over a `bra.w` or
`bra.a` (`bgt far` is `ble.b .+5; bra.w far`). Statements are separated by
newlines or `!`; `;` starts a comment (GNU's RX comment character), as
does `#` at the start of a line.

A template cannot name a symbol: its bytes carry no relocation, so
`bsr _f` and `mov.l #_x, r1` are refused (`"_f" is a symbol, and inline
asm cannot reach one`) -- pass the address as an `"r"` operand and `jsr`
through it, or write the code in a `.S` file or a
[file-scope block](#on-cortex-m-risc-v-mips32-and-avr), where symbols
relocate. There, as with GCC, a C name carries the RX underscore:
`bsr _c_function`, `.global _asm_function`.

### Instructions

| Instruction | Operands |
|---|---|
| `nop`, `rts`, `rte`, `rtfi`, `wait`, `brk`, `satr` | none |
| `mov.b`, `mov.w`, `mov.l` (`mov` is `mov.l`) | `rs, rd` (`.b`/`.w` sign-extend); `#imm, rd` (`.l`); `dsp[rs], rd`; `rs, dsp[rd]`; `#imm, dsp[rd]`; `[ri, rb], rd`; `rs, [ri, rb]`; `[rs+], rd`; `[-rs], rd`; `rs, [rd+]`; `rs, [-rd]` |
| `movu.b`, `movu.w` | `rs, rd`; `dsp[rs], rd`; `[ri, rb], rd`; `[rs+], rd`; `[-rs], rd` |
| `add`, `sub`, `and`, `or`, `mul` | `rs, rd`; `#imm, rd`; `src, rd` (memex); `rs, rs2, rd`; `add #imm, rs, rd` |
| `cmp`, `xor`, `tst`, `max`, `min`, `div`, `divu`, `emul`, `emulu` | `rs, rd`; `#imm, rd`; `src, rd` (memex) |
| `adc`, `sbb` | `rs, rd`; `#imm, rd`; `dsp[rs].l, rd` |
| `neg`, `not`, `abs` | `rd`; `rs, rd` |
| `xchg` | `rs, rd`; `src, rd` (memex) |
| `stz`, `stnz` | `#imm, rd` |
| `shll`, `shlr`, `shar` | `#0-31, rd`; `#0-31, rs, rd`; `rs, rd` |
| `rotl`, `rotr` | `#0-31, rd`; `rs, rd` |
| `rolc`, `rorc`, `sat` | `rd` |
| `revl`, `revw` | `rs, rd` |
| `push.b`, `push.w`, `push.l` (`push` is `push.l`); `pop` | `rs` or `dsp[rs]`; `rd` |
| `pushm`, `popm` | `rN-rM`, 1 <= N <= M |
| `pushc`, `popc`; `mvfc cr, rd`; `mvtc rs, cr`; `mvtc #imm, cr` | `psw`, `pc` (not written), `usp`, `fpsw`, `bpsw`, `bpc`, `isp`, `fintv`, `intb` |
| `setpsw`, `clrpsw` | `c`, `z`, `s`, `o`, `i`, `u` |
| `mvtipl #0-15`; `int #0-255` | |
| `bra` (`.s`, `.b`, `.w`, `.a`), `bsr` (`.w`, `.a`) | `TARGET`; `bra.l`/`bsr.l rs` (and `bra`/`bsr rs`) |
| `bCND` (`.s` and `.w` for `eq`/`ne`, `.b` for all) | `TARGET`; CND one of `eq`/`z`, `ne`/`nz`, `geu`/`c`, `ltu`/`nc`, `gtu`, `leu`, `pz`, `n`, `ge`, `lt`, `gt`, `le`, `o`, `no` |
| `jmp`, `jsr` | `rs` |
| `rtsd` | `#0-1020` (a multiple of 4); `#bytes, rN-rM` |
| `bset`, `bclr`, `btst`, `bnot` | `#0-31, rd`; `#0-7, dsp[rd].b`; `rs, rd`; `rs, dsp[rd].b` |
| `bmCND` | `#0-31, rd`; `#0-7, dsp[rd].b` |
| `scCND.l` | `rd` |
| `suntil`, `swhile`, `sstr`, `rmpa` (`.b`, `.w`, `.l`); `scmpu`, `smovu`, `smovb`, `smovf` | none |
| `mvfachi`, `mvfaclo`, `mvfacmi`; `mvtachi`, `mvtaclo` | `rd`; `rs` |
| `racw #1-2`; `mulhi`, `mullo`, `machi`, `maclo` | ; `rs, rs2` |

An immediate is any 32-bit value, signed or unsigned (`sub` and `sbb`,
which GNU as negates and inverts, take a signed one). A memex source
defaults to `.l`; its displacement is unsigned, scaled by the size and at
most 65535 units -- 32767 for `movu` and for `mov #imm, dsp[rd]`, whose
field QEMU reads signed. Each form is encoded as GNU as encodes it, the
shortest field that holds the value: tests/golden/rx-asm.sh assembles
every one of them with rx-elf-as and must get the same bytes. The FPU's
instructions (`fadd`, `itof`, ...) and RXv2's are refused with `fadd is an
FPU or RXv2 instruction, which is not in EmbCC's RX vocabulary`; a
memory-to-memory `mov` and the memory forms of `scCND` are refused by
name; anything else outside the list with `asm instruction "frob" is not
in the RX vocabulary`.

### Callee-saved registers

`r6`-`r13` are callee-saved. A template may change any register except
`r0` when it says so -- in its clobber list, or by naming the register in
the template, which counts as a clobber here even when the list does not
say it (beyond what GCC promises): a value live across the asm keeps out
of every register the asm changes, and a callee-saved one among them is
saved by the function's prologue (`pushm r6-rN`), as GCC saves it. A
clobber list naming `r0` is refused (`RX asm clobbers 'r0', the stack
pointer; EmbCC does not save it around an asm`). A call from a template
(`jsr %1`) must list the caller-saved registers it changes, `r1`-`r5`,
`r14` and `r15`, as with GCC.

### Example

```c
static inline unsigned irq_save(void)
{
    unsigned psw;
    __asm__ volatile("mvfc psw, %0\n\tclrpsw i" : "=r"(psw) :: "memory");
    return psw;
}

static inline void irq_restore(unsigned psw)
{
    __asm__ volatile("mvtc %0, psw" :: "r"(psw) : "memory");
}

static inline void set_usp(void *sp)
{
    __asm__ volatile("mvtc %0, usp" :: "r"(sp));
}
```

## ColdFire

This section applies to `m68k-none-elf` (ColdFire ISA_A+, the MCF5208).

### Constraints

| Letter | Meaning |
|---|---|
| `d`, `r`, `g` | a data register chosen by EmbCC |
| `a` | an address register chosen by EmbCC |
| `m` | an address register chosen by EmbCC, holding the address of the operand; it is written into the template as `(%aN)` |
| `i`, `n`, `I`-`P` | the constant, written into the template as `#N`, as GCC's m68k port prints it |
| `=`, `+`, `&` | see [Output operands](#output-operands) |

Anything else, a non-constant `i` included, is refused with
`asm constraint "b" is not valid for ColdFire`. An operand is one 32-bit
register: a `long long` one is refused (`ColdFire asm operand 0 is 8
bytes`), and so is an output narrower than a long in an address register,
which ColdFire moves only whole. A chosen data register comes from `d0`,
`d1`, then `d2`-`d7`, an address register from `a0`, then `a2`-`a5`,
skipping registers listed as clobbers or named in the template; never
`a1` (the lowering's own), `a6` (the frame pointer) or `a7` (the stack
pointer). A register variable must be one of those
(`register int x __asm__("d2")`); another is refused with `register
variable bound to 'a1' is not supported for ColdFire asm (use d0-d7, a0 or
a2-a5)`.

### Modifiers

None. `%N` prints the register (`%d2`), the constant (`#5`) or the memory
operand (`(%a2)`); `%%` is a `%`, so a register the template names is
written `%%d0` in extended asm (basic asm keeps its text: `%d0`). In a
[naked function](#naked-functions) `%cN` prints a constant without its
`#`.

### Template syntax

GNU as's Motorola syntax, as m68k-elf GCC writes it: registers `%d0`-`%d7`,
`%a0`-`%a7`, `%fp` (`%a6`) and `%sp` (`%a7`) -- the `%` optional and either
case, as GNU as takes them (`move.w SR,D7`) -- immediates `#expr`, and the
effective addresses `(An)`, `(An)+`, `-(An)`, `(d,An)` or `d(An)`,
`(d,An,Xi.l*s)`, `(d,%pc)`, `(xxx).w`, `(xxx).l` or a bare address, and
MIT's `An@`, `An@+`, `An@-`, `An@(d)` and `An@(d,Xi:l:s)`. Sizes are
suffixes; where an instruction has a choice and none is written it is
`.w`, as GNU as defaults (`move`, `mulu`), and `.l` where ColdFire has
only that (`add`). As GNU as does, `move.l #n,Dn` with n in -128..127 is
`moveq` and `add`/`sub #1..8` is `addq`/`subq`. A branch target is `.+N`
or `.-N` bytes from the branch, or a numeric label of the template (`1:`
referred to as `1b` or `1f`); a branch without a size takes `.s` or `.w`,
whichever reaches. Statements are separated by newlines or `;`; `|`
starts a comment, as does `#` at the start of a line.

A template cannot name a symbol: its bytes carry no relocation, so `jsr f`
is refused (`"f" is a symbol, and inline asm cannot reach one`) -- pass
the address as an `"a"` operand and `jsr (%0)`, or write the code in a
`.S` file or a [file-scope block](#on-cortex-m-risc-v-mips32-and-avr),
where symbols relocate.

### Instructions

| Instruction | Operands |
|---|---|
| `nop`, `rts`, `rte`, `tpf`, `halt`, `illegal` | none |
| `move.b`, `move.w`, `move.l`, `movea.w`, `movea.l` | `<ea>,<ea>` in the pairs ColdFire encodes: no source with an extension word into an indexed or absolute destination, no indexed, absolute or immediate source into anything but a register, `(An)`, `(An)+` or `-(An)` |
| `move.w %sr,Dn`; `move.w Dn,%sr`, `move.w #imm,%sr`; the same with `%ccr`; `move.l %usp,An`, `move.l An,%usp` | |
| `moveq` | `#-128..127,Dn` |
| `lea`, `pea`, `jmp`, `jsr` | a control address: `(An)`, `(d,An)`, `(d,An,Xi)`, absolute, `%pc`-relative |
| `add`, `sub`, `and`, `or`, `cmp` (`.l`) | `<ea>,Dn`; `Dn,<mem>` (not `cmp`); `#imm,Dn`; `<ea>,An` (`add`, `sub`, `cmp`: `adda`/`suba`/`cmpa`) |
| `eor.l` | `Dn,Dn`; `Dn,<mem>`; `#imm,Dn` |
| `adda`, `suba`, `cmpa`; `addi`, `subi`, `andi`, `ori`, `eori`, `cmpi` (`.l`) | `<ea>,An`; `#imm,Dn` |
| `addq`, `subq` (`.l`) | `#1..8,<ea>` |
| `addx`, `subx` (`.l`) | `Dy,Dx` |
| `neg`, `negx`, `not` (`.l`); `swap`; `ext.w`, `ext.l`, `extb.l` | `Dn` |
| `clr`, `tst` (`.b`, `.w`, `.l`) | a data register or memory |
| `asl`, `asr`, `lsl`, `lsr` (`.l`) | `#1..8,Dn`; `Dx,Dn` |
| `muls`, `mulu` (`.w`, `.l`) | `<ea>,Dn` (`.l`: `Dn`, `(An)`, `(An)+`, `-(An)`, `(d16,An)`) |
| `divs.l`, `divu.l`; `rems.l`, `remu.l` | `<ea>,Dq`; `<ea>,Dr:Dq` (the same modes as `mul.l`) |
| `scc` (`st`, `sf`, `shi` ... `sle`, `shs`, `slo`) | `Dn` |
| `bra`, `bsr`, `bcc` (`.s`, `.w`) | `TARGET`; `cc` one of `hi`, `ls`, `cc`/`hs`, `cs`/`lo`, `ne`, `eq`, `vc`, `vs`, `pl`, `mi`, `ge`, `lt`, `gt`, `le` |
| `link` (`.w`); `unlk` | `An,#-32768..32767`; `An` |
| `movem.l` | a register list (`%d2-%d7/%a2-%a5`) and `(An)` or `(d16,An)`, either way |
| `movec` | `Rn,Rc`, Rc one of `%cacr`, `%asid`, `%acr0`-`%acr3`, `%mmubar`, `%vbr`, `%rombar`, `%rambar`, `%mbar` |
| `trap #0..15`; `stop #imm16` | |
| `btst`, `bchg`, `bclr`, `bset` | `#0..31,Dn` or `Dx,Dn`; `#0..7,<mem>` (`(An)`, `(An)+`, `-(An)`, `(d16,An)`) or `Dx,<mem>` |

What ColdFire lacks is refused by name: `rol.l: ColdFire has no rotate`,
`dbra: ColdFire has no dbcc`, `exg`, `cas`, `chk`, BCD, the FPU's
instructions, the MAC's; ISA_B's `mvs`, `mvz`, `mov3q` and `sats` and a
32-bit branch, on which the MCF5208 traps; a byte or word `add`
(`add.w: ColdFire's arithmetic and logic are .l only`); a predecrement
`movem`; anything else with `asm instruction "frob" is not in the ColdFire
vocabulary`.

### Callee-saved registers

`d2`-`d7` and `a2`-`a6` are callee-saved. A template may change any
register except `a6` and `a7` when it says so -- in its clobber list, or by
naming the register in the template, which counts as a clobber here even
when the list does not say it: a value live across the asm keeps out of
every register the asm changes, and a callee-saved one among them is
saved by the function's prologue (`movem.l`), as GCC saves it. A clobber
list naming `%sp` or `%fp` is refused (`ColdFire asm clobbers 'sp', the
stack pointer; EmbCC does not save it around an asm`). A call from a
template (`jsr (%0)`) must list the caller-saved registers it changes,
`d0`, `d1`, `a0` and `a1`, as with GCC.

### Example

```c
static inline unsigned short irq_save(void)
{
    unsigned short sr;
    __asm__ volatile("move.w %%sr,%0\n\tmove.w #0x2700,%%sr"
                     : "=d"(sr) :: "memory");
    return sr;
}

static inline void irq_restore(unsigned short sr)
{
    __asm__ volatile("move.w %0,%%sr" :: "d"(sr) : "memory");
}

static inline void set_vbr(void *table)
{
    __asm__ volatile("movec %0,%%vbr" :: "a"(table));
}
```

## AVR

This section applies to the `avr` target (ATmega328P).

### Constraints

| Letter | Meaning |
|---|---|
| `r`, `g`, `q` | registers chosen by EmbCC from the pool below |
| `m` | registers chosen by EmbCC, holding the address of the operand; see below |
| `d` | registers chosen from r16 to r31 (the registers `ldi`, `subi`, `andi`, `cpi` reach) |
| `a` | registers chosen from r16 to r23 |
| `w` | the pair r24, r26 or r30 (the pairs `adiw` and `sbiw` reach, except Y) |
| `e` | the pointer pair X (r26) or Z (r30) |
| `b` | the pointer pair Z (r30) |
| `x` | X (r26:r27) |
| `z` | Z (r30:r31) |
| `i`, `n`, `I`, `J`, `K`, `L`, `M`, `N`, `O`, `P`, `R` | an integer constant expression, substituted into the template as a decimal literal |
| `=`, `+`, `&` | see [Output operands](#output-operands) |

Precedence: a [register variable](#register-variables) wins; then `x`,
`y` or `z`; then the class letters (`r`, `g`, `d`, `a`, `w`, `e`, `b`,
`q`, `m`); then the constant letters. When a string holds several class
letters, a register is accepted if any one of them accepts it, so a
string containing `r`, `g`, `q` or `m` accepts every register of the
pool. The constant letters only require a constant; its range is checked
by the instruction that uses it (`adiw: 100 is outside 0..63`).

`y` is refused, because Y is the frame pointer:

```text
an asm operand is pinned to 'r28', which is half of Y, the frame pointer this function reaches its own locals through
```

`q` does not mean the stack pointer: it is treated like `r`.

Do not use `m` on AVR. The operand receives as many registers as the
expression's type has bytes, but the address loaded into them is two
bytes, so for a 1-byte object the address overwrites the next register,
which may belong to another operand. The operand is also not
necessarily placed in a pointer pair. To read or write through a
pointer, pass the pointer with `e`, `x` or `z` and write `%a`.

A constant letter with an expression that is not constant, and any other
letter, is refused when the statement is generated:
`asm constraint "S" is not valid for AVR`.

An operand occupies as many consecutive registers as its type has bytes,
starting on an even register when it is wider than one byte. Registers
are chosen from r18 to r27, r30 and r31, skipping registers bound to
another operand, listed as clobbers, or named in the template. An
operand wider than 4 bytes is refused with
`an asm operand of 8 bytes needs 8 consecutive registers, which is more than this backend keeps free across an asm`,
and an exhausted class with
`no register satisfying "d" is free for an asm operand of 2 bytes`.

### Modifiers

| Form | Register operand | Immediate operand |
|---|---|---|
| `%N` | the lowest register (`r24`) | the value |
| `%AN` | byte 0: the lowest register | the whole value |
| `%BN` | byte 1: the next register up | bits 8 to 15 |
| `%CN` | byte 2 | bits 16 to 23 |
| `%DN` | byte 3 | bits 24 to 31 |
| `%aN` | the pair as a pointer: `X`, `Y` or `Z` | |

The named forms (`%A[NAME]`, `%a[NAME]`, ...) work the same way. `%a` on
an operand that is not in X, Y or Z is refused:

```text
%a names operand %1 as a pointer, but it is in r19 -- only X, Y and Z address memory, which is what the "e" and "b" constraints are for
```

Any other modifier is refused with
`asm template modifier '%E' is not supported for AVR (%A..%D name the bytes of a wider operand)`.

For the low byte of an immediate use `lo8()`, since `%A` prints the whole
value: `ldi %A0, lo8(%1)` and `ldi %B0, hi8(%1)`.

### Template syntax

GNU AVR syntax. Statements are separated by **newlines only**: on AVR
`;` starts a comment, as do `#` at the start of a statement and `//`.
Mnemonics are case-insensitive.

Registers are `r0` to `r31`, `XL`, `XH`, `YL`, `YH`, `ZL`, `ZH`, the
pointer pairs `X`, `Y`, `Z` (with `+`, `-` and `Y+q`/`Z+q` forms), and
`__tmp_reg__` (r0) and `__zero_reg__` (r1). `__SREG__`, `__SP_L__` and
the other avr-libc I/O names are not known; write the I/O address
(`0x3f` for SREG).

Operands may be constant expressions with `+ - * / % & | ^ ~ ! << >>`,
parentheses, character constants, and `lo8()`, `hi8()`, `hlo8()`,
`hh8()`, `pm_lo8()`, `pm_hi8()`, `pm_hh8()` and `gs()` applied to
constants. A symbol name is not accepted in an inline template
(`'buf)' is not a number, a register or an expression this assembler knows`).

The target of a relative branch (`br...`, `rjmp`, `rcall`) must be
written relative to `.`, the address of the start of the instruction
(`brne .-4`); a bare number is refused with
``rjmp needs a target written relative to `.` (a label, or `.+4`); ...``.
`jmp` and `call` take an absolute, even byte address written as a
number; a `.`-relative target is refused for them. Labels are not
accepted.

### Instructions

The whole AVR5 instruction set:

| Group | Instructions |
|---|---|
| Two registers | `add`, `adc`, `sub`, `sbc`, `and`, `or`, `eor`, `mov`, `cp`, `cpc`, `cpse`, `mul`, `movw` |
| One register written twice | `clr`, `tst`, `lsl`, `rol` |
| Signed and fractional multiply | `muls`, `mulsu`, `fmul`, `fmuls`, `fmulsu` |
| Register and immediate | `subi`, `sbci`, `andi`, `ori`, `cpi`, `ldi`, `ser` |
| One register | `com`, `neg`, `swap`, `inc`, `asr`, `lsr`, `ror`, `dec`, `push`, `pop` |
| Word | `adiw`, `sbiw` |
| Memory | `ld`, `st`, `ldd`, `std`, `lds`, `sts`, `lpm`, `elpm` |
| I/O | `in`, `out`, `cbi`, `sbi`, `sbic`, `sbis` |
| Register bits | `bld`, `bst`, `sbrc`, `sbrs` |
| Branches | `brcs`, `brlo`, `brcc`, `brsh`, `breq`, `brne`, `brmi`, `brpl`, `brlt`, `brge`, `brvs`, `brvc`, `brhs`, `brhc`, `brts`, `brtc`, `brie`, `brid`, `brbs`, `brbc` |
| Jumps and calls | `rjmp`, `rcall`, `jmp`, `call`, `ijmp`, `icall`, `eijmp`, `eicall` |
| Status register | `sec`, `clc`, `sez`, `clz`, `sen`, `cln`, `sev`, `clv`, `ses`, `cls`, `seh`, `clh`, `set`, `clt`, `sei`, `cli`, `bset`, `bclr` |
| No operands | `ret`, `reti`, `nop`, `sleep`, `wdr`, `break`, `spm` |

`elpm`, `eijmp`, `eicall` and `spm` are encoded although the ATmega328P
does not implement them all. The XMEGA instructions `xch`, `las`, `lac`,
`lat` and `des` are refused with
`xch is an AVR instruction this assembler does not implement yet (it is XMEGA-only or a cryptographic accelerator); ...`.

### Reserved registers on AVR

A template, a clobber or an operand that names r2 to r17 (callee-saved)
or r28, r29 or `Y` (the frame pointer) is refused:

```text
AVR asm names register 'r28', which is half of Y, the frame pointer this function reaches its own locals through
AVR asm clobbers register 'r5', which is callee-saved, and EmbCC saves nothing around an asm
```

r0 and r1 may be used. r1 is the zero register: a template that changes
it, for example with `mul`, must clear it again (`clr r1`) before it
ends. EmbCC does not check this.

### Example

```c
static inline unsigned char irq_save(void)
{
    unsigned char sreg;
    __asm__ volatile("in %0, 0x3f\n\tcli" : "=r"(sreg) : : "memory");
    return sreg;
}

static inline void irq_restore(unsigned char sreg)
{
    __asm__ volatile("out 0x3f, %0" : : "r"(sreg) : "memory");
}

unsigned int mul8(unsigned char a, unsigned char b)
{
    unsigned int r;
    __asm__("mul %1, %2\n\tmovw %0, r0\n\tclr r1" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

unsigned int swap16(unsigned int v)
{
    unsigned int r;
    __asm__("mov %A0, %B1\n\tmov %B0, %A1" : "=r"(r) : "r"(v));
    return r;
}

unsigned char next_byte(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    unsigned char c;
    __asm__("ld %0, %a1+" : "=r"(c), "+e"(p));
    *pp = p;
    return c;
}
```

## Limitations

In summary, compared with GCC:

- Only the instructions listed for each target, and the directives
  under [Directives in a template](#directives-in-a-template), can appear
  in a template, and a function template cannot refer to a symbol on any
  target.
- `asm goto`, `asm inline` and flag-output constraints are not
  supported, nor are matching constraints (`"0"`) except on SPARC,
  PowerPC and ARM Cortex-M (where the input must be the output's own
  lvalue). Flag outputs are not always refused (see [x86-64](#x86-64) and
  [AArch64](#aarch64)).
- `m` outputs do not work (except on MIPS32, Xtensa, SPARC and PowerPC,
  where the operand's register holds the address), and `m` inputs are a
  register holding the
  address rather than a memory reference.
- On x86-64 and RISC-V, `i` and `n` give a register, not an immediate.
- Labels inside a function template are supported only for the x86-64
  `leaq Nf(%%rip)` form and as numeric labels (`1:`, `1b`) on Xtensa,
  TriCore, SPARC, PowerPC, RX and ColdFire;
  elsewhere branches use numeric displacements.
- Register variables are not supported on ARM Cortex-M and RISC-V, are
  limited to x0-x11 and x13-x15 on AArch64, and are not supported at
  file scope on any target.
- Assembler names on C declarations are not supported.
- File-scope asm accepts data and a few directives on every target, and
  four instructions on x86-64 only. Every block is in `.text`, so other
  sections are refused.
- On x86-64, a callee-saved register that a template changes is saved
  only when it holds an operand and the optimization level is `-O2` or
  `-Os`.
- Every `asm` statement is volatile, a memory barrier, and prevents
  inlining of its function.
