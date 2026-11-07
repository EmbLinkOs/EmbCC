# embas — the EmbCC assembler

This page is the reference for `embas`, EmbCC's assembler. It assembles
for every target EmbCC compiles for: GNU syntax for the target you name,
as that target's `as` reads it, and NASM syntax for x86. It is for people
who assemble hand-written assembly -- a Cortex-M startup file, an RTOS
port's context switch, a boot sector, an operating-system kernel's entry
code -- without binutils or NASM.

## NAME

`embas` — assemble GNU-syntax source for any target, or NASM-syntax x86
source, into an ELF object or a flat binary

## SYNOPSIS

```text
embas [--target=TRIPLE] [-mcpu=...|-march=...|-mmcu=...] [-o FILE] [-I DIR] [-D NAME[=VALUE]] FILE.s|FILE.S
embas [-f elf64|bin] [-o FILE] [-I DIR] [-D NAME[=VALUE]] [-U NAME] [-E] FILE.asm
```

## DESCRIPTION

The file's name says its syntax:

- **`.s` and `.S`: GNU syntax**, for the target `--target` names (the
  default is `x86_64-elf`), with that target's options (`-mcpu=`,
  `-march=`, `-mmcu=`, `-mfloat-abi=`, `-mthumb`...). A `.S` file is read
  by the C preprocessor first, with `-I`, `-D` and `-U`. The object is the
  one `embcc -c FILE` makes; the syntax each target takes is described in
  [GNU-syntax assembly](#gnu-syntax-assembly) below.
- **`.asm` (or `-f`, or `--syntax=nasm`): NASM syntax**, x86's: NASM's
  preprocessor, its directives and Intel-syntax instructions, in 16-, 32-
  and 64-bit code, written to an x86-64 ELF64 object (`-f elf64`) or a
  flat image (`-f bin`), with the forms, prefixes and relocations NASM
  writes. EmbLinkOS's boot stages, AP trampoline and kernel entry code come
  out of it as NASM 3 makes them, and `tests/golden/nasm-asm.sh` compares a
  boot sector and a preprocessor fixture with `nasm` on every run.
  `--syntax=gnu` reads a file of another name as GNU syntax.

`embas` is `embcc` called by another name: both syntaxes reach the same
assembler and the same encoder for each target as `embcc -c` and inline
`__asm__` do, so an instruction one takes the others take. `embcc -c
FILE.asm` is the NASM assembler too.

The output is named as `as` and NASM name it: the input's name with `.o`,
or for `-f bin` without its extension.

## OPTIONS

For a GNU-syntax file:

| Option | Meaning |
|---|---|
| `--target=TRIPLE` | the target, any [EmbCC target](../target-list.md); `x86_64-elf` by default |
| `-m...` | the target's options, as `embcc` takes them |
| `-g` | debug information for the assembly |
| `-I DIR`, `-D NAME[=VALUE]`, `-U NAME` | the C preprocessor's, for a `.S` file |
| `-o FILE` | the object |
| `-E` | the preprocessed source |

For a NASM-syntax file:

| Option | Meaning |
|---|---|
| `-f elf64` | an x86-64 ELF64 relocatable object (the default) |
| `-f bin` | a flat binary: the sections placed from `org`, every reference resolved |
| `-o FILE` | the output; by default the input's name with `.o` (elf64) or without its extension (bin), as NASM names it |
| `-I DIR`, `-IDIR` | a directory `%include` and `incbin` search, after the current directory and the including file's |
| `-D NAME[=VALUE]` | `%define NAME VALUE` before the source |
| `-U NAME` | `%undef NAME` before the source |
| `-E` | write the preprocessed source instead of assembling it |
| `-O...`, `-w...` | accepted and ignored (`embas` always optimizes as NASM's `-Ox`) |

`-f elf32`, `win64`, `macho64` and NASM's other formats, listings (`-l`)
and dependency files (`-M`) are refused by name.

## THE PREPROCESSOR

| Directive | Meaning |
|---|---|
| `%define NAME BODY`, `%define NAME(a, b) BODY` | a single-line macro, expanded where it is used; `%idefine` case-blind |
| `%xdefine NAME BODY` | the body expanded now, where it is defined |
| `%undef NAME` | |
| `%assign NAME EXPR` | the value of a constant expression |
| `%strlen NAME 'string'` | |
| `%macro NAME N[-M][+] [defaults]` ... `%endmacro` | a multi-line macro: `%1`..`%N`, `%{N}`, `%{-1}`, `%0` (the count), `%00` (the label before the call), `%%name` (a label of its own per call), `%+` (concatenation), `%?` (its name); `+` takes the rest of the line into the last parameter; `%rotate N` |
| `%rep N` ... `%endrep`, `%exitrep` | |
| `%if EXPR`, `%ifdef`, `%ifndef`, `%ifidn`, `%ifidni`, `%ifnum`, `%ifstr`, `%ifid`, `%ifmacro`, `%ifempty`, their `%elif...` and `%ifn...` forms, `%else`, `%endif` | |
| `%include "FILE"` | |
| `%error`, `%fatal`, `%warning` | |

A constant expression has NASM's operators: `/` and `%` unsigned, `//`
and `%%` signed, `<<` `>>` `>>>`, `&` `|` `^`, comparisons, `&&` `||`
`^^`, `!` `~`. Numbers in NASM's spellings: `0x1F`, `1Fh`, `$1F`, `0b101`,
`101b`, `17o`, `17q`, `1_000`, and characters `'ab'`, `"ab"`, `` `\n` ``.

## DIRECTIVES

| Directive | Meaning |
|---|---|
| `bits 16`, `bits 32`, `bits 64`, `use16/32/64`, `[bits N]` | the size of the code that follows; `-f bin` starts in 16-bit code, `-f elf64` in 64-bit |
| `org ADDR` | `-f bin` only: where the image is loaded |
| `section NAME [attrs]`, `segment` | NASM's defaults by name (`.text` exec, align 16; `.data`, `.rodata`, `.bss` align 4; others align 1); attributes `alloc`, `noalloc`, `exec`, `noexec`, `write`, `nowrite`, `progbits`, `nobits`, `note`, `tls`, `align=N`. In `-f bin`, sections other than `.text` are aligned to 4; `start=`, `vstart=` and `follows=` are refused |
| `global NAME[:function|data [SIZE]] [hidden]` | |
| `extern NAME`, `common NAME SIZE[:ALIGN]` | `common` is an ELF COMMON symbol |
| `default rel`, `default abs` | `[x]` RIP-relative or absolute in 64-bit code; `[rel x]`, `[abs x]` choose for one operand |
| `NAME equ EXPR` | |
| `db dw dd dq dt do dy dz` | numbers, expressions, strings (padded to the item size), `?`; floats in `dd` and `dq` |
| `resb resw resd resq rest reso resy resz N` | |
| `times N STATEMENT` | `N` may depend on `$`: `times 510-($-$$) db 0` |
| `incbin "FILE"[, SKIP[, COUNT]]` | |
| `align N[, nop | db V]`, `alignb N` | padding with `nop` (0x90) or the byte given; the section's alignment grows to `N`, as NASM's `sectalign on` |
| `absolute ADDR`, `struc NAME` ... `endstruc`, `istruc NAME` / `at FIELD, ...` / `iend` | `struc` defines `NAME.field` and `NAME_size` |
| `cpu`, `float`, `sectalign`, `[warning]`, `[list]` | accepted and ignored |

`$` is the current place and `$$` the start of the current section.
A label is `name:`, or a name alone or before a data directive, `equ` or
`times`; a label beginning with `.` belongs to the last other label
(`main.loop`).

## INSTRUCTIONS

Intel syntax, as NASM writes it: the destination first, a size keyword
(`byte`, `word`, `dword`, `qword`, `tword`, `oword`, `yword`, `zword`)
where no register gives the size, `[base + index*scale + disp]` with an
optional segment (`[es:di]`), `strict`, `short`, `near`, `far`,
`jmp SEL:OFF`, the prefixes `lock`, `rep...`, `o16`, `o32`, `o64`, `a16`,
`a32`, and AVX-512's `{k1}`, `{z}`, `{1to16}` and `{rn-sae}`. Where NASM
and GNU as pick different encodings of one instruction, `embas` picks
NASM's: `mov r64, imm` is the five-byte `mov r32, imm` when the value fits
it and `movabs` for a symbol, a VEX register move keeps its load form, and
the prefixes are in NASM's order.

## OUTPUT

`-f elf64`: an ELF64 relocatable object laid out as NASM lays it out --
the sections in the order the source names them, a `STT_FILE` symbol, a
section symbol per section, then the labels and the `extern` symbols that
are used in the order they were defined or declared. A reference to a
symbol defined in the object is relocated against its section; `call
symbol` is `R_X86_64_PC32` (`wrt ..plt` makes it `R_X86_64_PLT32`, `wrt
..gotpcrel` `R_X86_64_GOTPCREL`).

`-f bin`: `.text` at `org`, then the other sections with contents at their
alignment, then the `nobits` ones (not written). A name the file does not
define is refused, since there is no linker to find it.

## DIAGNOSTICS

```text
FILE:LINE: error: MESSAGE
```

`FILE` and `LINE` are the source line's, in an `%include`d file or a
macro's definition as well. An instruction `embas` cannot encode is named
with the reason.

## EXIT STATUS

| Status | Meaning |
|---|---|
| 0 | the output was written |
| 1 | the source could not be read or did not assemble |
| 2 | a usage error: no input, an unknown option or format, a missing option argument |

## EXAMPLES

A boot sector:

```asm
[BITS 16]
[ORG 0x7C00]
start:
    xor ax, ax
    mov ds, ax
    mov si, msg
.print:
    lodsb
    or al, al
    jz $
    mov ah, 0x0E
    int 0x10
    jmp .print
msg db `Hello\r\n`, 0
times 510 - ($ - $$) db 0
dw 0xAA55
```

```sh
embas -f bin boot.asm -o boot.bin
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
%assign n 0
%rep 32
STUB n
%assign n n + 1
%endrep
isr_common:
    call isr_dispatch
    iretq
```

## GNU-SYNTAX ASSEMBLY

`embas` reads NASM syntax. Assembly in GNU syntax is assembled by `embcc`
itself, for every target: x86-64 (AT&T syntax, and Intel syntax after
`.intel_syntax noprefix`; 16- and 32-bit code after `.code16` and
`.code32`), ARM (Thumb and A32), AArch64, RISC-V, MIPS, LoongArch64,
Xtensa, TriCore, SPARC, PowerPC, RX, ColdFire and AVR:

```sh
embcc --target=thumbv7m-none-eabi -c startup.S -o startup.o
```

A `.S` file is preprocessed first; a `.s` file is not. Instructions are encoded by the same per-target
assembler that inline `__asm__` uses, so the accepted instructions are the
inline-assembly vocabulary listed in [Inline assembly](../inline-asm.md);
driver behavior and output naming are described in
[Invoking EmbCC](../invoking.md#assembly-input).

The assembler reads what a CMSIS or vendor startup file and an RTOS port
are written in. Each Cortex-M startup file in ARM's CMSIS_5 and every
STM32F4 one in ST's cmsis-device-f4 assembles to the object clang's
assembler makes from it -- the same sections, instructions, symbols and
relocations -- which `tests/golden/gas-gnu.sh` checks.

### Source

- **Comments.** `/* ... */` anywhere, also across lines; `#` and `//`;
  `@` on ARM, `!` on SPARC, `;` on AVR and RX and `|` on ColdFire. On
  ARM, AArch64, SPARC, RX and ColdFire, `#` starts a comment only as a
  line's first character (an immediate's prefix on ARM, RX and ColdFire,
  `#function` in SPARC's `.type`).
- **Statements.** `;` separates two statements on one line (not on AVR
  and RX, where it is the comment; RX separates them with `!`). Any number
  of `label:` may precede one.
  Numeric local labels `0:` to `9:` are referred to as `1b` and `1f`.
- **Letter case.** Mnemonics, register names, conditions and operand
  keywords are case-insensitive (`MRS r0, PRIMASK`); symbols are not.

### Expressions

Wherever a directive takes a value: numbers in C's notation and `0b`
binary, `'c'` characters, symbols, `.` (here), and GNU as's operators with
GNU as's precedence -- `*` `/` `%` `<<` `>>` bind tightest, then `|` `&`
`^` `!`, then `+` `-` and the comparisons, then `&&` `||`. A label minus a
label in the same section is a plain value, a label plus a value stays an
address, and an address in a data word becomes a relocation (a label in
this file or an external symbol, with any addend). Anything that mixes
addresses otherwise is refused.

### Directives

| Directive | Effect |
|---|---|
| `.text`, `.data`, `.rodata`, `.bss` | switch to that section |
| `.section NAME[, "FLAGS"[, @TYPE[, ENTSIZE]]]` | switch to `NAME`, creating it. With no flags, a name GNU as knows gets its flags (`.text.*` code, `.data.*` and `.bss.*` writable, `.rodata.*` read-only, `.init_array` and friends, `.tdata`/`.tbss`) and any other name none at all (not allocated), as in GNU as. `FLAGS` from `a w x M S T R y`; `TYPE` `progbits`, `nobits`, `note`, `init_array`, `fini_array`, `preinit_array`. Declaring a section again with other flags or type is refused |
| `.pushsection ...`, `.popsection`, `.previous` | switch, and switch back |
| `.byte`, `.short`/`.half`/`.hword`/`.2byte`, `.word`, `.long`/`.int`/`.4byte`, `.quad`/`.dword`/`.8byte` | values of 1, 2, 4 (2 on AVR for `.word`), 4 and 8 bytes; each an expression |
| `.ascii`, `.asciz`, `.string` | string data; `.asciz` and `.string` add a terminating zero |
| `.space N[, FILL]`, `.zero N`, `.skip N[, FILL]` | `N` bytes of `FILL` (0), or reserved space in a NOBITS section |
| `.fill REPEAT[, SIZE[, VALUE]]` | `REPEAT` values of `SIZE` bytes |
| `.org OFFSET` | advance to `OFFSET` in this section |
| `.align N`, `.p2align N` (and `w`/`l` variants) | align to 2^`N` bytes; code is padded with the target's no-op (with zeros on Xtensa, whose instructions are three bytes; with GNU as's multi-byte nops on RX, and a `bra.b` over the rest of a gap of 8 or more). On Xtensa, SPARC, RX and ColdFire `.align N` counts bytes, a power of two, as GNU as reads it there |
| `.balign N` (and `w`/`l` variants) | align to `N` bytes |
| `.equ NAME, EXPR`, `.set NAME, EXPR`, `NAME = EXPR`, `.equiv NAME, EXPR` | define `NAME`: absolute for a value, an alias for an address; it may name a label further down |
| `.thumb_set NAME, EXPR` | as `.set`, and `NAME` is a Thumb function (how a startup file aliases weak handlers to its default one) |
| `.global`/`.globl`, `.weak`, `.local`, `.hidden`, `.internal`, `.protected` | a comma-separated list of names |
| `.type NAME, %function`/`%object`/`%notype` (also `@...`) | the symbol's type |
| `.size NAME, EXPR` | the symbol's size, usually `. - NAME` |
| `.lcomm NAME, SIZE[, ALIGN]`, `.comm ...` | reserve space in `.bss` (`.comm` makes it global) |
| `.inst`, `.inst.n`, `.inst.w` | an instruction by its encoding |
| `.ltorg`, `.pool` | place the literal pool here (ARM) |
| `.literal_position`, `.literal NAME, EXPR, ...` | Xtensa: a literal pool here; words in the current pool, `NAME` on the first. A section's pools are at its start, at each `.literal_position` and before the labels of each `entry`, and `movi aN, EXPR` whose value is a symbol or wider than 12 bits loads from the latest one -- as GNU as's `--text-section-literals` places them |
| `.begin NAME`, `.end NAME` | Xtensa: GNU as's relaxation blocks; `no-transform`, `literal_prefix`, `schedule`, `density`, `target-align` and their `no-` forms are accepted (nothing here relaxes), `longcalls` and `absolute-literals` refused |
| `.macro NAME [PARAMS]` ... `.endm`, `.purgem`, `.exitm` | a macro: parameters with `=default`, `:req` or `:vararg`, used as `\name`; `\@` counts expansions and `\()` separates |
| `.rept N`, `.irp SYM, A, B...`, `.irpc SYM, CHARS` ... `.endr` | repetition |
| `.if EXPR`, `.ifdef`, `.ifndef`, `.ifeq`, `.ifne`, `.ifgt`, `.ifge`, `.iflt`, `.ifle`, `.ifb`, `.ifnb`, `.ifc`, `.ifnc`, `.ifeqs`, `.ifnes`, `.else`, `.elseif`, `.endif` | conditional assembly, decided from values known where the `.if` stands (numbers and earlier `.equ`/`.set`; not a label's address) |
| `.include "FILE"` | the file, found as given or beside this one |
| `.end` | the rest of the file is not assembled (on MIPS `.end f` and on Xtensa `.end NAME` close a block instead) |
| `.error "MSG"`, `.warning "MSG"`, `.print "MSG"` | a diagnostic |
| `.cpu`, `.arch`, `.arch_extension`, `.fpu`, `.syntax`, `.thumb`, `.code 16`, `.eabi_attribute`, `.object_arch`, `.file`, `.ident`, `.loc`, `.cfi_*`, `.attribute`, `.option` | accepted; the target comes from `--target`/`-mcpu` |
| `.fnstart`, `.fnend`, `.cantunwind`, `.save`, `.vsave`, `.setfp`, `.pad`, `.movsp`, `.personality`, `.personalityindex`, `.handlerdata`, `.unwind_raw` | accepted; no unwind table is written (an exception unwinding through this code stops) |

Refused by name: `.arm` and `.code 32` (an M-profile core has no ARM
state), subsections (`.text 1`), `.weakref`, and any other directive
(`directive ".NAME" is not one this assembler knows`).

### ARM specifics

- **`ldr rd, =EXPR`.** A constant a move can make is that move -- `mov.w`
  for a modified immediate, `mvn.w` for its complement, `movw` up to
  0xffff -- and anything else, an address included, is a word in the
  literal pool, loaded pc-relative. The pool is placed at the next
  `.ltorg`/`.pool` or at the end of the section; identical entries share
  a word. None of these sets the flags, as `ldr` does not.
- **Relaxation.** A branch and a literal load are assembled in their
  two-byte form when the target is in reach, and in the four-byte form
  otherwise; `.w` and `.n` force one. The passes repeat until no label
  moves, so the layout is the one GNU as and clang produce.
- **Symbols.** A branch to an external symbol, to a label in another
  section or to a weak symbol is relocated (`R_ARM_THM_CALL`,
  `R_ARM_THM_JUMP24`), since the linker decides where those end up.
  `.thumb_func`, `.type NAME, %function` and `.thumb_set` make a symbol a
  Thumb function, which carries the interworking bit. `.L` labels stay
  out of the symbol table.
- **ARMv8.1-M** (`-mcpu=cortex-m55`, `m85`): the conditional selects
  (`csel`, `csinc`, `csinv`, `csneg`, and `cset`, `csetm`, `cinc`, `cinv`,
  `cneg`), the low-overhead loops (`dls`, `dlstp`, `wls`, `wlstp`, `le`,
  `letp`, `lctp`), `clrm` and `vscclrm`; and MVE (Helium) -- its vector
  instructions from a table learned from llvm-mc (`tools/mve-isa`), a
  statement predicated in a VPT block (`vaddt.i32`), `vpt` and `vpst`
  with their masks, `vctp`, the scalar shifts (`lsll`, `asrl`, `uqshl`...)
  and `vmrs`/`vmsr` of `vpr` and `p0`. On an earlier core each is refused
  by name.
- **The object** has `$t`/`$d` mapping symbols in sections that contain
  code, and the same `.ARM.attributes` the compiler writes for the target,
  so a disassembler decodes it and the linker can check it.

### RX specifics

- **Names.** A C symbol is `_name` in an RX object, and an assembly file
  writes it so (`.global _main`, `bsr _c_function`), as with GNU as; the
  object's names are the file's. In a file-scope block or a naked
  function in C, `_name` is C's `name`.
- **Relaxation.** A branch written without a size takes the shortest form
  that reaches, as GNU as relaxes it: `bra` `.s`/`.b`/`.w`/`.a`, `bsr`
  `.w`/`.a`, `beq`/`bne` `.s`/`.b`/`.w` and then the inverse over a
  `bra.a`, any other condition `.b` and then the inverse over a `bra.w`
  or `bra.a`. A form only ever lengthens, so the passes settle; a written
  size is kept, and refused if it does not reach.
- **Sections** are GNU as's defaults, as GCC runs it: `.text`, `.data`
  and `.bss` select Renesas's `P`, `D_1` and `B_1`, which every object
  holds, first, even empty; `.section .text` is a section of its own
  (code, whatever its flags say). Each is aligned as the file asks and
  padded to its alignment at its end, as GNU as pads it. A local
  symbol's value plus the addend is written into an `R_RX_DIR32` field
  as well as into the relocation, as GNU as writes it. `.word` is four
  bytes, as GNU as makes it here.

### ColdFire specifics

- **Syntax.** GNU as's Motorola syntax, the register `%` optional and
  either case, MIT's `An@(d)` accepted; FreeRTOS's ColdFire V2 port
  assembles as it is.
- **Relaxation.** A branch written without a size takes `.s` or `.w`,
  whichever reaches; the MCF5208 has no 32-bit branch, so `.l` is refused
  and a label beyond 32 KiB is reached with `jmp`. `move.l #n,Dn` is
  `moveq` for -128..127 and `add`/`sub #1..8` is `addq`/`subq`, as GNU as
  makes them.
- **Data.** Big-endian; `.word` is two bytes, as GNU as makes it for m68k.

A symbol that the file does not define may be named only in the
instruction forms that carry a relocation:

| Target | Forms |
|---|---|
| RISC-V | `call SYMBOL`, `la REG, SYMBOL` |
| ARM | `bl SYMBOL`, `b SYMBOL`, `ldr REG, =SYMBOL`, `movw`/`movt` with `#:lower16:`/`#:upper16:` |
| AArch64 | `bl SYMBOL` |
| AVR | `call`, `jmp`, `rcall`, `rjmp` and conditional branches to a symbol; `lds`/`sts` with a symbol address; `ldi REG, lo8(SYMBOL)`, `hi8(...)`, `pm_lo8(...)`, `pm_hi8(...)`, and `lo8(gs(SYMBOL))`, `hi8(gs(SYMBOL))` |
| SPARC | `call SYMBOL` (`R_SPARC_WDISP30`), a branch to one (`R_SPARC_WDISP22`); `sethi %hi(SYMBOL)` and any `%lo(SYMBOL)` operand -- an `or`, an `add`, a load's or store's offset -- (`R_SPARC_HI22`, `R_SPARC_LO10`), and `set SYMBOL, REG` (both), relocated even for a label of this file |
| ColdFire | `bra`/`bsr SYMBOL` as `jmp`/`jsr` to its address (`R_68K_32`), `bcc SYMBOL` and a written `.w` as `.w` (`R_68K_PC16`); a bare `SYMBOL` or `#SYMBOL` operand of `jsr`, `jmp`, `lea`, `pea`, `move`, an ALU instruction (`R_68K_32`); `.long SYMBOL` (`R_68K_32`), `.word SYMBOL` (`R_68K_16`) |
| RX | `mov.l #SYMBOL, REG` (`R_RX_DIR32`); `bra`/`bsr SYMBOL` (`.a`, `R_RX_DIR24S_PCREL`), `beq`/`bne SYMBOL` (`.w`, `R_RX_DIR16S_PCREL`), another `bCND SYMBOL` (`.b`, `R_RX_DIR8S_PCREL`), or the size written; `.long`/`.word SYMBOL` |
| PowerPC | `b`, `bl` (`R_PPC_REL24`), `ba`, `bla` (`R_PPC_ADDR24`) and a conditional branch (`R_PPC_REL14`) to a symbol; `SYMBOL@ha`, `@h` and `@l` in any 16-bit immediate or offset (`R_PPC_ADDR16_HA`, `_HI`, `_LO`), relocated even for a label of this file |

Any other use is refused, for example
`"STATEMENT" names 'NAME', which is not defined in this file, in a form this target's assembler cannot relocate`.
Errors have the form `FILE:LINE: error: MESSAGE`.

## SEE ALSO

[`embld`](embld.md), [Inline assembly](../inline-asm.md),
[Invoking EmbCC](../invoking.md#assembly-input), [Targets](../targets.md)
