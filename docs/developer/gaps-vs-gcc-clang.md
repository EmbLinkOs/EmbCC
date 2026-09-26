# What EmbCC still lacks, measured against GCC and Clang

**Audit date:** 2026-09-26. **Method:** GCC trunk (`gcc-mirror/gcc`) and
`llvm/llvm-project` were cloned locally and their own tables read — GCC's
`gcc/common.opt` and `gcc/c-family/c.opt` (1129 front-end options),
Clang's `clang/include/clang/Options/Options.td`, `Builtins.td`,
`Attr.td`, and LLVM's `PassBuilderPipelines.cpp`. Every claim below was
then **probed against the built `./embcc`**, not inferred from reading
EmbCC's source. Where a probe contradicted a reading of the code, the
probe won; two of the findings in the first draft were wrong that way
and are corrected here.

This file complements `todo.md`, which is a corpus audit (does real C
compile at all). This one asks a different question: where does EmbCC
sit against the two compilers it will be compared to.

---

## The headline: the gap is not code quality

Measured on this tree's own real sources — `lib/libc` and `lib/rt`, 69
files that both compilers accept:

| | `.text` bytes, `-O2` |
|---|---|
| EmbCC | 134,858 |
| Clang | 96,407 |
| **ratio** | **1.39× Clang** |

On `tests/exec/*.c` the same measurement says 7.58×, and that number is
worthless: those are toy programs whose `main` Clang constant-folds to a
single `return`. **Measure on real code.** 1.39× is the honest figure,
and for a from-scratch compiler it is a good one.

EmbCC also has more optimizer than its reputation here suggests: a real
inliner with budgets (`INLINE_MAX_CALLEE` 24, `INLINE_SOLE_CALLEE` 200),
sixteen toggleable passes — `mem2reg gcse load-cse sccp licm vectorize
inline dse div-magic if-convert cfg-clean tail-recursion idiom sroa
unroll pre` — plus always-on fold, LVN, copy propagation, DCE,
reassociation, loop rotation, IV strength reduction, store forwarding
and immediate folding. A probe of `static inline` through two levels
inlines completely and leaves five instructions with no calls, which is
what Clang does with the same input.

The C++ front end is likewise further along than a "from scratch"
compiler suggests. Probed working: lambdas (including generic),
`constexpr`/`consteval`, concepts, coroutines, `<ranges>`, structured
bindings, `if constexpr`, fold expressions, `operator<=>`, exceptions,
RTTI and `dynamic_cast`.

**So the gap is elsewhere: in the driver surface, in the preprocessor
features that headers depend on, and in whole subsystems that were never
started.** Those are below, in priority order.

---

## Tier 0 — EmbCC cannot be dropped into an existing build

This is the highest-leverage tier by a wide margin. Every item is small
on its own; together they are the difference between "a compiler" and "a
compiler you can point at someone else's Makefile".

### ~~The `__has_*` family does not exist~~ — FIXED, and the cause was one line

*Landed. Kept here because the shape of the bug is worth remembering.*

The whole mechanism existed and was gated on one line in
`src/cpp/cpp.c`: `if (!predef_is_cxx()) return 0;`. It had been built
for libstdc++ and C never got it. What follows is what C saw.

```c
#if defined(__has_include) && __has_include(<foo.h>)
```

is the pattern every portable header written since about 2015 uses, and
it **fails to compile**: `error: trailing junk in #if expression`.
`__has_include`, `__has_builtin`, `__has_attribute`, `__has_feature` and
`__has_c_attribute` are all undefined, so the identifier becomes `0` and
the preprocessor then chokes on `(<foo.h>)`.

That is worse than the feature merely being absent. An absent feature
lets the guarded fallback run; this one turns a header that was written
*defensively* into a compile error. It is the single biggest interop
blocker found in this audit.

GCC and Clang both special-case these in the `#if` grammar. EmbCC must
too — the parse has to recognise the call form even when the answer is
"no".

### `_Pragma` does not exist

`_Pragma("GCC diagnostic ignored ...")` inside a macro is how headers
suppress warnings at their own definition sites. EmbCC handles `#pragma`
as a directive (`once`, `pack`, `GCC diagnostic` all work) but not the
operator form, so any macro that carries a pragma is a syntax error.

### `-include` is documented in `--help` and rejected by the driver

```
$ ./embcc -include pre.h -fsyntax-only x.c
embcc: error: unknown argument '-include'
```

`src/driver/main.c:122` promises `-include FILE  include it before the
file`. There is no pre-include machinery anywhere in `src/`. A help text
that names a flag the compiler refuses is the same class of thing THE
RULE exists to prevent, pointed at the user instead of at codegen. Fix
it or delete the line.

### Options a real build passes, all rejected

Probed by compiling a translation unit with each flag:

| Group | Rejected |
|---|---|
| Freestanding / kernel | `-ffreestanding` `-fno-builtin` `-fno-strict-aliasing` `-fwrapv` `-ftrapv` |
| Embedded link shaping | `-ffunction-sections` `-fdata-sections` `-fshort-enums` |
| Char / enum signedness | `-fsigned-char` `-funsigned-char` |
| Position independence | `-fPIC` `-fpic` `-fpie` `-fPIE` |
| Link driver | `-shared` `-static` `-nostdlib` `-nostartfiles` `-nodefaultlibs` `-l` `-L` |
| Preprocessor paths | `-imacros` `-iquote` `-idirafter` `-undef` |
| Debug | `-gdwarf-4` `-gdwarf-5` `-g3` `-ggdb` `-gsplit-dwarf` |
| Machine | `-march=` `-mtune=` `-mavx2` `-msse4.2` `-mfpu=` `-mthumb` |
| Ergonomics | `-v` `-save-temps` `-pipe` `-pedantic` `-ansi` `-pg` |

`-fshort-enums` deserves a line of its own: it is the **ARM EABI
default**, so any ARM code that assumes it is already compiling wrong
against EmbCC's enums, silently.

Also: GCC spells it `-print-search-dirs`, EmbCC spells it
`--print-search-dirs`. Accept both.

### One input file at a time, and no link driver

```
$ ./embcc -c a.c b.c
embcc: error: more than one input file (M1: one file at a time)
$ ./embcc a.c -o prog
embld: entry symbol '_start' is undefined
```

`embcc a.c -o prog` does reach `embld`, so the plumbing exists — what is
missing is default startup objects and libraries for a hosted target,
plus accepting `.o` inputs and `-l`/`-L`. `cc *.c -o prog`, the most
common compiler invocation in the world, does not work.

---

## Tier 1 — front-end holes that real code hits

### `typeof` fails on a function parameter

This is the most surprising finding in the audit, because the comment
above the implementation claims the opposite.

```c
int x; typeof(x) y;                      /* OK */
int f(void){int a=1; typeof(a) b=a;}     /* OK */
int f(int a){typeof(a) b=a;}             /* error: typeof of an unsupported expression */
int f(int a){typeof(a+1) b=0;}           /* error */
typeof(f()) y;                           /* error */
```

The parameter case is the *only* shape that matters, because it is the
shape `min()`, `max()` and `container_of()` expand into.

**Root cause, located:** `typeof` is resolved in the parser, by
`ce_type` (`src/parse/parse.c:1215`) reading a shadow symbol table
`g_fold_locals` (`:1185`) that exists for folding `sizeof(expr)`.
Function parameters are never pushed into that table — only block-scope
locals and file-scope globals are — so `fold_var_type` returns NULL and
the parser reports the expression as unsupported. `ce_type` also handles
only five expression kinds (var, cast, deref, member, pointer
arithmetic), which is why `a+1` and `f()` fail even for a local.

Two fixes, in increasing order of correctness:

1. **Contained:** push the parameter names and types into
   `g_fold_locals` when a function body opens (they are already captured
   in `ps->fn_pnames`). Fixes the common case; leaves `typeof(a+1)`
   broken.
2. **Right:** resolve `typeof` in sema, where real scopes and the real
   type rules live, rather than in a parse-time approximation of them.
   The parse-time table is a duplicate of a thing sema already has, and
   duplicates drift — this gap *is* that drift.

### Builtins that are missing

Present and working (probed): the whole bit-twiddling family
(`clz`/`ctz`/`popcount`/`parity`/`ffs` and the `ll` variants), `bswap16`/`32`,
`assume_aligned`, `expect`, `unreachable`, `trap`,
`alloca`, and — notably solid — **all of the atomics**:
`__atomic_load_n`, `store_n`, `exchange_n`, `compare_exchange_n`,
`fetch_add`, `thread_fence`, and the `__sync_*` legacy family.

Missing:

| Builtin | Why it matters |
|---|---|
| `__builtin_types_compatible_p` | kernel macro staple, pairs with `_Generic` |
| `__builtin_choose_expr` | the other half of that pair |
| `__builtin_object_size` | `_FORTIFY_SOURCE` is built on it |
| `__builtin_fabs` `copysign` `signbit` `isinf` `fma` | math headers call these directly |
| `__builtin_LINE` `FILE` `FUNCTION` | logging/assert macros |
| `__builtin_memcmp` `strlen` | already have memcpy/memset/memmove |
| `__builtin_setjmp` `clear_cache` | |
| `__builtin_add/sub/mul_overflow` | **C++ only** — C says "is not declared" |
| `__builtin_shufflevector` | needs vectors first |
| `__c11_atomic_*` | `_Atomic` works; Clang's spelling does not |

### `__attribute__` is not parsed on a `typedef`, in either position

```c
typedef int i __attribute__((aligned(16)));   /* error: expected ';' before '__attribute__' */
typedef __attribute__((aligned(16))) int i;   /* error: expected a type after 'typedef' */

int g __attribute__((aligned(16)));           /* OK */
void f(void) __attribute__((noreturn));       /* OK */
struct s { int a; } __attribute__((packed));  /* OK */
typedef struct { int a; } __attribute__((packed)) s;  /* OK */
```

So the attribute grammar covers variables, functions and struct
definitions but not a typedef NAME. `typedef int i
__attribute__((aligned(16)))` is an ordinary idiom, and every
vector-typedef in existence has this shape — which is the real reason the
`vector_size` probe fails, rather than anything to do with vectors.

This is a contained parser gap and is probably the cheapest item in this
whole document.

### Attributes: 30 recognised, 15 parsed and dropped, 3 refused by name

An unknown attribute warns (`-Wattributes`, "is not one EmbCC knows, and
is ignored") rather than erroring, which is correct GCC behaviour — but
it means **an attribute that changes layout or codegen is ignored with
only a warning**. These are in that category:

`vector_size` `mode` `transparent_union` `counted_by` `target`
`tls_model` `weakref` `ifunc` `access` `copy` `error` `noclone` `noipa`
`designated_init` `assume_aligned`

`vector_size` and `mode` are the two that change results rather than
hints. On a plain declaration they warn and leave the type alone:

```c
__attribute__((vector_size(16))) int g;
_Static_assert(sizeof(g) == 4, "");   /* passes -- still a scalar int */
```

A warning is a diagnostic, so this is not a THE RULE violation outright.
But `-Wattributes` is one line in a build log, and the code that follows
computes the wrong thing. Both belong in the refuse-by-name set until
they work.

Refused by name (the honest kind of missing): `cleanup`, `naked`,
`interrupt`. The last two are firmware staples.

Recognised and working: `noreturn` `packed` `aligned` `weak` `used`
`unused` `deprecated` `constructor` `destructor` `format` `pure` `const`
`may_alias` `warn_unused_result` `always_inline` `noinline` `hot` `cold`
`nonnull` `returns_nonnull` `malloc` `alloc_size` `returns_twice`
`flatten` `nothrow` `leaf` `sentinel` `gnu_inline` `optimize`
`no_sanitize` `section` `visibility`.

### Language features absent

| Feature | Status |
|---|---|
| nested functions | error |
| `__label__` | error |
| `__auto_type` | error |
| vector extensions | no typedef attribute, and `vector_size` is dropped |
| `asm goto` | error |
| case ranges `case 1 ... 5:` | error |
| `__VA_OPT__` | error |
| `_Float16`, `__float128` | error |
| **C23:** `constexpr` `auto` `nullptr` `[[attr]]` `enum : type` `typeof_unqual` `#embed` | all error |
| C++ modules | refused by name |

Working, for the record: computed goto, VLAs, statement expressions,
`_Complex`, `_Generic`, `__int128`, designated initialisers, compound
literals, `__thread`, flexible and zero-length arrays, anonymous
structs.

`asm goto` is worth singling out: the Linux kernel's static-key
infrastructure is built on it, and EmbCC's stated target is an OS
kernel.

---

## Tier 2 — subsystems that do not exist

| Subsystem | GCC/Clang | EmbCC |
|---|---|---|
| Warnings | 359 `-W` flags (GCC front end alone) | **12**, and every other `-W…` is silently swallowed |
| PIC / PIE | yes | none — so no shared libraries, no ASLR |
| LTO | yes | none |
| Sanitizers | ASan, UBSan, TSan, MSan | none |
| Coverage / PGO | `-fprofile-*`, `-pg`, `--coverage` | none |
| Architectures | GCC 79 target dirs, LLVM 62 | 5 (34 triple spellings) |
| `-g` on embedded | yes | **refused on thumb, rv32, rv64** |
| DWARF | v5, full | v4, 3 sections |

Two deserve expanding.

**UBSan is the one to want first.** Not ASan — on a microcontroller
there is no shadow memory to spare. But `-fsanitize=undefined` with
`-fsanitize-trap=all` costs a `brk`/`ebreak` on each check and catches
exactly the class of bug that is hardest to find on a board with no
debugger attached: signed overflow, shift past width, null deref,
misaligned access. For EmbCC's actual audience this is worth more than
LTO.

**Twelve warnings is the number to be uncomfortable about.** EmbCC
implements `-Wunused-variable/-parameter/-function`, `-Wshadow`,
`-Wsign-compare`, `-Wuninitialized`, `-Wmaybe-uninitialized`,
`-Wformat`, `-Wattributes`, `-Wdeprecated-declarations`,
`-Wunused-result`, `-Wwindows-abi`. Everything else a build passes —
`-Wstrict-prototypes`, `-Wmissing-prototypes`, `-Wcast-align`,
`-Wconversion`, `-Wnull-dereference`, `-Warray-bounds`, `-Wswitch` — is
accepted and does nothing. A project that turns on `-Wall -Wextra
-Werror` and builds clean under EmbCC has learned almost nothing.

**DWARF detail.** EmbCC emits `.debug_info`, `.debug_abbrev`,
`.debug_line` and gets structs, members, pointers, variables and
formal parameters right. Against Clang on the same input it is missing
`DW_TAG_enumeration_type`, `DW_TAG_enumerator`, `DW_TAG_typedef` and
`DW_TAG_lexical_block` — so an enum prints as an integer in a debugger,
a typedef'd type shows its underlying name, and block-scoped locals are
not scoped.

---

## Tier 3 — silent acceptance, which this project calls a bug

THE RULE says refuse loudly rather than emit wrong. These all accept
quietly:

- `-O9` — accepted.
- `-Wcompletely-made-up` — accepted. Any `-W…` is.
- `-std=c89` with a C99 `for(int i…)` — accepted.
- `-std=c++98` with a C++11 lambda — **accepted**, and compiled as
  C++20. `--version` says "EmbCC has one dialect per language", which is
  a defensible position, but then `-std=` should say so rather than
  nod.
- `-include` — documented, rejected.

Each of these is a small lie the compiler tells a build system, and each
one costs somebody an afternoon eventually.

---

## Suggested order

Ranked by (blocked work) ÷ (effort), not by size of the gap:

1. **`__has_include` / `__has_builtin` / `__has_attribute`** — one
   change in the `#if` expression parser; unblocks modern headers
   wholesale.
1b. **`__attribute__` on a `typedef`** — a few lines of declarator
   grammar; the cheapest item here.
2. **`typeof` on parameters** — push `ps->fn_pnames` into
   `g_fold_locals`; one function, unblocks every kernel macro.
3. **`_Pragma`** — preprocessor operator, contained.
4. **The freestanding/embedded option set** — `-ffreestanding`,
   `-fno-builtin`, `-ffunction-sections`, `-fdata-sections`,
   `-fshort-enums`, `-fsigned-char`/`-funsigned-char`, `-fwrapv`. Mostly
   accept-and-honour, a few accept-and-ignore-with-a-reason.
5. **`-include`, `-imacros`, `-iquote`, `-idirafter`** — and fix the
   help text either way.
6. **`-g` on thumb / rv32 / rv64** — the DWARF writer exists and works
   on two targets; this is the last thing between EmbCC and source-level
   firmware debugging, which is the whole point of the embedded work.
7. **`__builtin_types_compatible_p` + `__builtin_choose_expr`** — a pair,
   small, high header-compatibility value.
8. **Refuse what is not understood**: bad `-O`, unknown `-W`, unknown
   `-std=`. Cheap, and it is what this project says it believes.
9. **Multiple inputs and a real link driver** (`-l`, `-L`, `.o` inputs,
   default crt).
10. **`-fsanitize=undefined` with trap-on-error** — the highest-value
    new subsystem for this compiler's actual audience.

Everything after that — PIC/PIE, LTO, PGO, the remaining 347 warnings,
more architectures — is real but is not what is currently stopping
anybody.

---

# Part II — structure, optimization, targets, architecture

Part I audited the compiler's SURFACE: what it accepts. That is the
wrong half to stop at. This part audits how it is BUILT, what it
actually optimises, and which machines it can really serve.

## The correction Part I needs

Part I reported 1.39× Clang and called it good. That number is at
`-O2`, and at `-O2` **Clang grows code** — it inlines and unrolls for
speed. For anything embedded the level that matters is `-Os`, and there
the picture changes:

| level | EmbCC | Clang | ratio |
|---|---|---|---|
| `-O0` | 376,009 | 114,489 | 3.28× |
| `-O1` | 246,385 | 84,223 | 2.92× |
| `-O2` | 134,858 | 96,407 | **1.39×** |
| `-Os` | 134,010 | 63,387 | **2.11×** |

Same 69 real files, x86-64, `.text` bytes. Read the last two rows
together:

**`-Os` is `-O2` under a different name.** EmbCC goes from 134,858 to
134,010 — six tenths of one percent. Clang goes from 96,407 to 63,387 —
thirty-four percent. EmbCC accepts `-Os`, reports it, and does almost
nothing with it.

For a compiler whose stated audience is firmware on parts measured in
kilobytes, that is the single most valuable missing thing in this entire
document. 1.39× was the flattering framing; 2.11× is the honest one.

## Target gaps

Every target against Clang on the same real corpus, at `-Os`:

| target | EmbCC | Clang | ratio |
|---|---|---|---|
| `x86_64-elf` | 134,010 | 63,387 | 2.11× |
| `aarch64-elf` | 143,576 | 71,536 | 2.00× |
| `riscv64-unknown-elf` | 135,664 | 42,090 | 3.22× |
| `riscv32-unknown-elf` | 157,204 | 44,764 | 3.51× |
| `thumbv7m-none-eabi` | 141,668 | 36,408 | **3.89×** |

The two embedded families, the ones the recent work was for, are the
worst — and they are the ones where size is not a preference but a
budget.

### RISC-V has no compressed instructions, and that is most of its gap

Clang's default `-march` for `riscv32-unknown-elf` is **`rv32imac`**.
EmbCC emits `rv32im`. Holding everything else equal:

| | `.text` | vs EmbCC |
|---|---|---|
| EmbCC `rv32im` | 157,204 | — |
| Clang `rv32imac` (its default) | 44,764 | 3.51× |
| Clang `rv32im` (compressed off) | 62,808 | 2.50× |

**The C extension alone is 28.8% of Clang's code size.** It is 16-bit
encodings for the common register/immediate forms — the highest
size-per-effort item available on this target, and it does not need any
new optimisation, only new encodings in `src/arch/riscv/emit.c` and a
selector that prefers them.

### The RISC-V predefined macros claim the wrong code model

```
$ ./embcc --target=riscv32-unknown-elf --dump-predef | grep cmodel
#define __riscv_cmodel_medlow 1
```

The backend emits **medany** — PC-relative `auipc`, chosen deliberately
because `lui` sign-extends bit 31 and cannot name the addresses RV64
needs (D-016). `src/arch/riscv32/predef.c:361` says medlow. Code that
tests this macro to decide how to take an address will choose the wrong
sequence. It is a one-line fix and a real miscompile source.

### Machines that are missing, ranked by who would notice

| Gap | Who it locks out |
|---|---|
| **Thumb-1 / ARMv6-M** | Cortex-M0 and M0+ — the most shipped MCU core there is |
| **Hardware FP** | M4F, M7, and RISC-V F/D. Everything is soft float today |
| **`-march=` / `-mcpu=` at all** | there is no way to ASK for any of the above |
| ARMv8-M | anything with TrustZone-M |
| Xtensa | the entire ESP32 family |
| AVR, MSP430 | 8- and 16-bit MCU work |

The first three are one theme: EmbCC has one fixed ISA per target and no
vocabulary for saying which chip. That is the structural reason
compressed RISC-V, Thumb-1 and hardware FP cannot be added as options
today — there is nowhere to put the option.

## Structural gaps — the shape of the toolchain

### There is no assembler for any embedded target

```
$ ./embcc --target=riscv32-unknown-elf -c start.S -o start.o
embcc: error: unknown argument 'start.S'
```

`.s` and `.S` are not input types at all. The only standalone assembler
is `embas`, which is **NASM syntax, x86-64 only**. Firmware startup —
the reset vector, the stack setup before `main`, a context switch — is
hand-written assembly in every real project, and here it can only be
written as inline asm inside a C function.

The per-target `asm.c` files exist and work, but they are reachable only
through `__asm__` in C. Wiring them to a `.S` input is mostly plumbing.

### `-S` prints the x86 disassembler's output for non-x86 targets

```
$ ./embcc --target=riscv32-unknown-elf -S t.c -o -
f:
	.byte	0x13,0x01	# adc    (%rcx),%eax
```

That comment is the **x86-64 disassembler** run over RISC-V bytes. The
guard at `src/driver/main.c:1081` refuses `-S` for aarch64 only:

```c
if (ta == TARGET_AARCH64)
    diag_fatal(in, 0, "-S is x86-64 only: there is no aarch64 disassembler "
                      "here, and emitting text that is not the object would "
                      "be worse than refusing");
```

Thumb and both RISC-V targets were added after that line and fall
through it. The comment states the principle exactly and the code now
does the opposite for three of five targets. This is a THE RULE
violation with a two-line fix, and it should be fixed before anything
else in this document.

Note also that `-S` never emits real assembly on any target: it emits
`.byte` directives with a disassembly comment. There is no assembly
printer, only an encoder.

### Archiving is still binutils

`Makefile:301` and friends shell out to `x86_64-elf-ar` and
`aarch64-elf-ar`. There is no `ar` for thumb or RISC-V at all, so the
embedded targets cannot produce a static library with this toolchain.
A1 in `todo.md` called owning the build done at the assembler; archiving
was not part of it.

### Self-hosting is x86-64 only

`tests/golden/x86_64/self-host.sh` is the only one. The fixed point is a
real achievement and it covers one of five targets.

## Architectural gaps — how the compiler is built

### EmbIR is not SSA, and SSA lasts for one pass

`src/ir/ir.h` still opens with "Still no SSA and no passes — that
revision comes with the optimizer". The optimizer arrived; the revision
did not. What actually happens is narrower: `mem2reg` builds the CFG,
the dominator tree (Cooper-Harvey-Kennedy), dominance frontiers, inserts
phis, renames — and then **destructs SSA immediately**, realising each
phi as copies on its incoming edges. Every other pass runs on linear,
non-SSA three-address code.

The cost is visible in the source: `build_cfg` is called **16 times** in
`opt.c`. Each pass re-derives the control flow and the dataflow it
needs, because the IR does not carry them. In LLVM the IR is SSA from
the front end to register allocation, which is why GVN, SCCP, LICM and
jump threading there are both cheap and precise — a def is a single
value with a known set of uses, permanently.

This is the deepest item in this document and the one least suited to an
afternoon. It is also the one that would make the next ten passes easier
to write than the last ten were.

### There is no machine-IR level

The driver says so itself, at `main.c:2260`: "the separate machine-IR
level of the design (vision §9.2) does not exist". IR lowers straight to
encoded bytes. Four things follow, and all four showed up independently
elsewhere in this audit:

- **No instruction scheduling**, anywhere. Confirmed absent. On an
  in-order dual-issue core — Cortex-M7, most RISC-V microcontrollers —
  that is real cycles.
- **No machine-level peephole** across instruction boundaries, and no
  size-aware selection, which is part of why `-Os` does nothing and why
  Thumb's 16-bit encodings are under-used.
- **`-S` cannot print assembly**, only bytes plus a comment.
- **Each backend hand-writes its own assembler** for inline asm, because
  there is no shared MC layer to assemble against.

### Inlining is the only interprocedural transform

No argument promotion, no dead-argument elimination, no global
constant merging, no IPA constant propagation, no attribute inference
(`readnone`/`readonly`, which is what lets a caller keep values in
registers across a call). Everything else is within one function.

### What is genuinely good, so nobody "fixes" it

- **The register allocator** is a proper Chaitin-Briggs graph colourer:
  real live ranges across back-edges, an interference graph, coalescing
  by union-find, optimistic spilling, ABI hints. It is shared across all
  five targets. This is not a gap.
- **Alias analysis exists** (`opt.c:1979`), with type-based aliasing
  deliberately excluded and the reason written down.
- **The inliner** has sensible budgets and a sole-caller path.
- **Dependency generation** (`-M` and its whole family) is complete.

## Revised order

Part I's list was right about the surface and wrong about the weighting.
Merged and re-ranked:

1. **Fix `-S` on thumb and RISC-V** — two lines; it currently prints
   x86 mnemonics next to RISC-V instructions.
2. **Fix `__riscv_cmodel_medlow`** — one line; the macro contradicts the
   backend.
3. **`__has_include` / `__has_builtin` / `__has_attribute`** — unblocks
   modern headers wholesale.
4. **`__attribute__` on a `typedef`** — a few lines of declarator grammar.
5. **`typeof` on parameters** — one function; unblocks every kernel macro.
6. **RISC-V compressed instructions** — 28.8% of code size, measured. No
   new optimisation needed, only encodings and a selector that prefers
   them.
7. **Make `-Os` mean something** — it is currently `-O2`. Start with
   size-aware selection and not inlining/unrolling at `-Os`.
8. **`.S` input for every target** — the per-target assemblers already
   exist; this is plumbing, and firmware cannot ship without it.
9. **`-g` on the embedded targets** — last step to source-level firmware
   debugging.
10. **`-march=`/`-mcpu=`** — the vocabulary that Thumb-1, hardware FP and
    RISC-V extensions all need before they can exist.
11. The Part I driver-option set, `_Pragma`, the missing builtins.
12. **SSA as the IR's actual form** — the deep one. Not an afternoon, and
    the thing that makes everything after it cheaper.
