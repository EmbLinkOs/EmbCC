# Compatibility — what EmbCC supports, per architecture

EmbCC compiles C for the two machines EmbLinkOS runs on. One process compiles
for one target, chosen with `--target=`:

| | **x86_64-elf** (default) | **aarch64-elf** |
|---|---|---|
| Machine | x86-64 | AArch64 (ARMv8-A; QEMU `virt`, Cortex-A72) |
| ABI | System V AMD64 | AAPCS64 |
| Source | [`src/arch/x86_64/`](../src/arch/x86_64/README.md) | [`src/arch/aarch64/`](../src/arch/aarch64/README.md) |
| Reference compiler (tests, predefined macros) | `x86_64-elf-gcc` 16.2 | `aarch64-elf-gcc` 16.2 |
| Tests run on | QEMU `qemu-system-x86_64` (natively on x86-64 Linux) | QEMU `qemu-system-aarch64 -M virt` |

Everything in `src/` outside `src/arch/` is shared by both. ✓ means supported
and tested on that target; — means not applicable; ✗ means refused with a
diagnostic (never miscompiled).

## Types

| | x86_64 | aarch64 |
|---|---|---|
| Data model | LP64 | LP64 |
| plain `char` | signed | **unsigned** |
| `wchar_t` | `int` | `unsigned int` |
| `float` / `double` | IEEE binary32 / binary64 | IEEE binary32 / binary64 |
| `long double` | **x87 80-bit extended**, 16 bytes, 16-aligned | **IEEE binary128**, 16 bytes, 16-aligned |
| `_Complex` (float, double, long double) | ✓ | ✓ |
| Largest honoured alignment | 16 | 16 |

## The C language

C11 is supported in full on both targets; the rows below are the parts that
took target-specific work, or that a reader might doubt.

| | x86_64 | aarch64 |
|---|---|---|
| C99/C11 core (declarations, initializers incl. designated, compound literals, bitfields, flexible arrays, `_Generic`, `_Static_assert`, `_Alignas`/`_Alignof`, wide and Unicode literals) | ✓ | ✓ |
| Variable-length arrays (incl. parameters, `sizeof`, release on `break`/`continue`/`goto`) | ✓ | ✓ |
| `long double` arithmetic, conversions, exact constants | ✓ x87 | ✓ libgcc soft-float |
| `_Complex` arithmetic (`*`, `/` through libgcc, Annex G) | ✓ | ✓ |
| Variadic functions, `va_arg` (incl. `double`, `long double`), `va_copy` | ✓ | ✓ |
| `_Atomic`, `__atomic_*`, `__sync_*` | ✓ `lock`-prefixed | ✓ `ldxr`/`stxr` + barriers |
| Statement expressions, `typeof`, computed `goto`, `__real__`/`__imag__`, imaginary constants | ✓ | ✓ |
| Attributes: `packed`, `aligned`, `weak`, `noreturn`, `section` (file-scope variables); `embcc_sret` on a first parameter (the ABI's indirect-result pointer — what C++ lowering writes) | ✓ | ✓ (`x8`) |
| Builtins: bit family, `expect`, `frame_address`/`return_address`, `constant_p`, `unreachable`, ... | ✓ | ✓ |
| Extended inline asm | ✓ AT&T, the x86 kernel's vocabulary; constraints `a b c d S D r m i x +` | ✓ GNU A64, the ARM kernel's 67 templates; constraints `r =r +r i`, register variables |
| File-scope `__asm__` | ✓ (crt0's vocabulary) | ✗ |
| Preprocessor (full, incl. `#include_next`), newlib headers | ✓ | ✓ |

## C++

In progress toward C++20 with libstdc++ (D-013, [CXX.md](CXX.md)), the same
on both targets: `src/cxx` lowers C++ to C for the pipeline above. Names,
class layout and calls follow the Itanium C++ ABI, as g++ does on both
targets, so EmbCC's C++ objects link with g++'s and with libstdc++.

| | x86_64 | aarch64 |
|---|---|---|
| **CX1** `bool`, `nullptr`, references, namespaces (nested, reopened, inline, unnamed, aliases, `using`), `extern "C"`, overloading, default arguments, enums (scoped, fixed underlying type), the named casts, `auto`/`decltype` for variables | ✓ | ✓ |
| **CX1** classes: members, `this`, static and const members, constructors (delegating, mem-initializers, default member initializers), destructors on every scope exit, temporaries' lifetimes, `new`/`delete` and `new[]`/`delete[]` (array cookies) | ✓ | ✓ |
| **CX1** namespace-scope objects (`.init_array`, `__cxa_atexit`), function-local statics (`__cxa_guard_*`) | ✓ | ✓ |
| **CX2** operator overloading, argument-dependent lookup, conversion functions and converting constructors, copy and move (implicit memberwise, implicit move on return, NRVO), pointers to members, class `operator new`/`delete` | ✓ | ✓ |
| A class that is not trivially copyable, by value: argument by reference to the caller's temporary; result through the return slot | ✓ slot first, in `rdi` | ✓ slot in `x8` (`embcc_sret`) |
| **CX3** single and multiple inheritance with g++'s layouts (empty bases, tail padding reuse), virtual functions and destructors, pure virtuals, thunks, vtables and typeinfo by the key-function rule, `typeid`, `dynamic_cast` | ✓ | ✓ |
| Itanium mangling (nested names, `St`, substitutions, member pointers, conversion functions, thunks, typeinfo); EmbCC objects linking with g++ objects both ways — including one class hierarchy split across the two compilers | ✓ | ✓ |
| **CX4** class, function, member, alias and variable templates; deduction, explicit and partial specialization (the most specialized chosen), partial ordering, SFINAE, explicit instantiation; variadic templates (type, value and function parameter packs, every expansion context, `sizeof...`, fold expressions); Itanium names for all of it (dependent expressions and pack expansions included) | ✓ | ✓ |
| Virtual base classes (CX3b); CX5..CX9 — exceptions, lambdas, C++20, libstdc++ compiled by EmbCC, C++ on the OS | ✗ refused, naming the milestone | ✗ |

    embcc -c prog.cc -o prog.o          # .cc .cpp .cxx .C .c++, or -x c++
    embcc --emit-c prog.cc              # the C it lowers to

## Code generation

| | x86_64 | aarch64 |
|---|---|---|
| `-O0` / `-O1` / `-O2` | ✓ slot coalescing, RAX cache (`-O1`), graph-colouring register allocator (`-O2`) | ✓ IR optimizations at `-O1`/`-O2`; the backend itself keeps every value in a stack slot (D-011) |
| `-g` (DWARF 4: lines, locals, structs, arrays, complex) | ✓ frame base `rbp` | ✓ frame base `x29` |
| Kernel modes | `-mno-red-zone`, `-mno-sse` (x87 still allowed), `-mcmodel=kernel` | `-mgeneral-regs-only` (any FP refused, as in gcc) |

## Calling convention

| | x86_64 (SysV) | aarch64 (AAPCS64) |
|---|---|---|
| Integer / FP argument registers | 6 (`rdi`..`r9`) / 8 (`xmm0-7`) | 8 (`x0-7`) / 8 (`v0-7`) |
| Aggregates | classified by eightbyte: INTEGER / SSE / MEMORY | HFA in `v` registers; ≤16 bytes in `x` registers; larger by reference (B.3); result > 16 bytes through `x8` |
| `long double` | in memory (X87 class), returned in `st0` | `q` register |
| `_Complex` | float: one `xmm`; double: two; long double: memory in, `st0`/`st1` out | HFA of two: `s`, `d` or `q` registers |
| `va_list` | `char *` to a `__va_list_tag` (what newlib reads) | `char *` to the 32-byte record (B.3 makes it gcc-compatible) |

Each of these is checked against gcc by linking an EmbCC half and a gcc half
that call each other in both directions: `tests/golden/sysv-abi.sh`,
`cross-varargs.sh`, `ldouble-abi.sh`, `complex-abi.sh` — on both targets.

## Toolchain components

| | x86_64 | aarch64 |
|---|---|---|
| `embcc -c file.c` | ✓ | ✓ |
| `embcc -c file.asm` / `embas` (NASM syntax, byte-identical to nasm on the kernel's `.asm`) | ✓ | — (NASM is x86 syntax) |
| `embld` (ELF and EMBX linker, orphan-section brackets, higher-half kernels) | ✓ | ✗ x86-64 objects only; link with the cross `ld` |
| `embdbg` (DWARF reader, symbolizer, x86-64 disassembler) | ✓ | ✗ |
| gdb on EmbCC's DWARF, live in QEMU | ✓ | ✓ |
| Self-hosting (EmbCC compiles and links itself, fixed point) | ✓ | — |

## EmbLinkOS

| | x86_64 | aarch64 |
|---|---|---|
| Kernel C files compile | 165 / 165 (`-O2`) | 131 / 131 (`-mgeneral-regs-only`) |
| Kernel `.asm` | 6 / 6 through embas | `.S` through the cross assembler |
| Kernel built entirely by EmbCC (compile, assemble, link) and booted | ✓ `tests/golden/x86_64/embbuild-kernel.sh` (opt-in) | not yet (no embld for aarch64) |
| Userspace against newlib | ✓ | ✓ |

## Test coverage per target

| suite | x86_64 | aarch64 |
|---|---|---|
| `tests/exec/*.c` — compiled, RUN, and agreeing with gcc (`agrees-with-gcc`) | 81 + 3 x86-only | 81 |
| `tests/cxx/*.cc` — C++ compiled, RUN, and agreeing with g++ (`cxx-agrees-with-gxx`); an EmbCC half linked with a g++ half (`cxx-abi`) | 12 | 12 |
| `tests/golden/*.sh` — gcc cross-ABI, gdb, predef, optimizer | 14 | 14 |
| `tests/golden/<arch>/` — machine-specific referees | 25 (embas vs nasm, embld, embdbg, EMBX, self-host, kernel) | 2 (encoder vs objdump, inline asm vs `aarch64-elf-as`) |
| `tests/compile/` — refusals | ✓ | — |

    make test         # tests/run.sh                    (x86_64-elf)
    make test-arm64   # tests/run.sh --target=aarch64-elf

## Refused, on both targets

A construct outside what EmbCC supports fails with a diagnostic naming it
(THE RULE) — none of these is miscompiled:

- GNU integer complex types (`_Complex int`, `2i`).
- `va_arg` of a struct or a complex (passing one through `...` works).
- `section("name")` on a function or a local.
- A static complex initializer that multiplies or divides two complex values.
- C++ past the milestones done (below): each refusal names its milestone.
- `__thread`/TLS, position-independent output — out of scope for now
  (ARCHITECTURE §8, D-008).
