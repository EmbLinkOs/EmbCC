# src/arch

Everything that depends on the target machine lives here — one directory per
architecture, plus the few files every target shares. The rest of `src/` (the
lexer, preprocessor, parser, sema, IR, optimizer, DWARF and ELF writers,
driver) is target-neutral and never names a machine; it asks this directory.
What each target supports, feature by feature, is in
[docs/language/compatibility.md](../../docs/language/compatibility.md).

```
src/arch/
  target.c/.h      which machine: --target= parsing, and relocation KINDS
  backend.h        the contract a backend implements (codegen_unit*, site lists)
  code.c/.h        the machine-code byte buffer every encoder writes into
  predef.c/.h      which predefined-macro table the target uses
  x86_64/          x86-64, System V AMD64 ABI           -> x86_64/README.md
  aarch64/         AArch64, AAPCS64                      -> aarch64/README.md
```

Each architecture directory holds the same kinds of file:

| file        | what it is                                              | x86_64 | aarch64 |
|-------------|---------------------------------------------------------|:------:|:-------:|
| `codegen.c` | IR -> machine code, the calling convention               | yes    | yes     |
| `emit.c/.h` | the instruction encoder                                  | yes    | yes     |
| `irgen.c`   | the target's share of IR generation: `va_arg`, inline asm | yes    | yes     |
| `predef.c`  | predefined macros, generated from the target's gcc       | yes    | yes     |
| `asm.c/.h`  | inline-asm template assembler (GNU syntax)               | in `irgen.c` | yes |
| `topasm.c`  | file-scope `__asm__`: directives and labels on any target, x86-64 mnemonics | yes    | shared  |
| `as.c/.h`   | EmbAS, the standalone NASM-syntax assembler (`embas`)    | yes    | —       |

## Target selection

One process compiles for one target, chosen by `--target=` and fixed before
the front-end runs (`target.c`). Nothing reads the HOST architecture for any
reason: the compiler that runs on a Mac and the one that will run on
EmbLinkOS itself must make identical objects.

## Why relocation *kinds* exist

`target_reloc_type()` maps a machine-neutral `enum reloc_kind` to an ELF
relocation type. The indirection is not ceremony — it is where the two
machines disagree about *how many* relocations an act costs:

    &symbol   x86-64    lea rax, [rip+rel32]      one R_X86_64_PC32
              aarch64   adrp x9, sym              R_AARCH64_ADR_PREL_PG_HI21
                        add  x9, x9, #:lo12:sym   R_AARCH64_ADD_ABS_LO12_NC

So codegen records a KIND at each patch site (`strsite`/`gsite`/`fsite` in
`backend.h`), the aarch64 backend pushes two sites where the x86 backend
pushes one, and the driver turns (kind, target) into a type and an addend.
The addend differs too: x86-64's PC-relative fields are measured from the END
of the instruction and bias by -4, aarch64's from the instruction itself and
bias by 0. `RK_ABS64`/`RK_ABS32` are the absolute pointer and DWARF-offset
fields.

## The backend contract

`backend.h`: a backend takes a unit's IR and returns its `.text` plus the
sites the driver turns into relocations — calls to externals, string,
global and function addresses. The two backends are peers with the same
signature; the driver picks one on `--target=` and nothing downstream knows
which machine produced the image. `cg_wide_vregs` (the vregs holding a
16-byte long double) is shared analysis both use.

## Adding a target

A new directory with the files in the table, a column in `target.c`'s
relocation mapping, a predef table from `tools/gen-predef.sh`, a QEMU harness
under `tests/harness/<arch>/`, and a row in every table of
docs/language/compatibility.md. Then `tests/run.sh --target=<triple>` must pass, with
`agrees-with-gcc` refereeing it against that target's gcc.
