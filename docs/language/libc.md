# Our C library

*One implementation, ported to an operating system by one small backend.*

Code: [`lib/libc/`](../../lib/libc/README.md). Built with EmbCC itself:
`make libc-x86_64`, `make libc-aarch64`.

## Why

A compiler that targets several operating systems cannot borrow one OS's C
library. EmbCC's test targets used **newlib**; EmbLinkOS had its own
**emlibc**. That is two `printf`s, two `strtod`s and two sets of bugs — the
duplication this project removes everywhere else (R1), and it also meant
every new target inherited somebody else's porting problem.

## Shape

    lib/libc/
      include/        the hosted C11 headers
      src/            the library, portable C
      arch/<arch>/    what portable C cannot express
      os/<name>/      the backend

**`os/backend.h` is the entire contract with an operating system**: eleven
primitives — read, write, open, close, lseek, sbrk, time, clock, exit,
isatty, getentropy. A new OS implements those and gets the library. Nothing
above the seam knows which OS it is on, and there are no `#ifdef`s there.

Two backends exist. `os/posixlike/` maps the primitives onto the classic
calls, which is what the QEMU test harness and anything Unix-shaped
provides. `os/emblinkos/` is **EmbLinkOS**, and it is the proof the seam
works: see below.

Buffering, formatting and allocation policy live **above** the seam
deliberately, so every target gets the same behaviour and a fix lands once.

## State

| | |
|---|---|
| `<string.h>` | complete |
| `<ctype.h>` | complete ("C" locale) |
| `<errno.h>` | complete, with the POSIX numbers |
| `<stdlib.h>` | allocator, conversions, `qsort`/`bsearch`, process control, `rand` |
| `<stdio.h>` | `FILE`, buffering, the whole `printf` family, files by name, positioning |
| `<math.h>` | fdlibm's cores under their C11 names, plus what its 1993 set never carried |
| `<complex.h>` | complete — new; emlibc never had one |
| `<time.h>` | the calendar complete and exact; the clock is whatever the backend has |
| `<setjmp.h>` | complete, per architecture |
| `<assert.h>`, `<inttypes.h>` | complete |
| `scanf` family | complete, including `%[`, `%n`, `%a` and hex floats |
| `<wchar.h>`, `<locale.h>`, `<signal.h>`, `<threads.h>` | **not yet** |

**The acceptance test is the execution corpus.** All **90** x86-64 programs
in `tests/exec` compile, link and run against this library with **no newlib
at all**, each producing its expected exit status
(`tests/golden/libc.sh`).

## Where it deliberately differs

`isdigit('5')` returns **1**, not the internal class bit that newlib and
glibc hand back. C promises only "nonzero"; a program that prints or
compares the value is relying on something unspecified, and returning a
tidy 1 makes that bug visible on the first run instead of after a port.

## Choices worth knowing

- **Heapsort for `qsort`.** C does not forbid quadratic behaviour, but a
  library sort is exactly where an adversarial input arrives. Heapsort is
  n log n on every input and needs no scratch memory.
- **First-fit with coalescing for `malloc`.** Small enough to read in one
  sitting and to reason about when a target misbehaves. Nothing outside
  `malloc.c` knows the shape, so a better allocator is a local change.
- **One formatting engine.** Every `printf` variant is `__vformat` with a
  different sink. Writing it twice is how `%zu` ends up working in one and
  not the other.
- **`aligned_alloc` splices a real block header** rather than returning an
  interior pointer with the original stashed below it. C requires the
  result to be freed with plain `free`, so there must be exactly one kind
  of block.

## atexit belongs to exit, not to the C++ runtime

`__cxa_atexit`/`__cxa_finalize` are in `src/stdlib/exit.c`, on the same
list as `atexit`. C++ static destructors and C `atexit` handlers have to
interleave by registration order — an object constructed before an
`atexit()` call is destroyed after that handler runs — and two lists
cannot express that ordering however they are drained. `atexit` is stored
as the one-argument form with the function as its own argument, which
keeps one list at the cost of one indirect call.

The table is fixed at 256 entries rather than grown with `malloc`: it is
walked during exit, when calling the allocator may be the last thing a
failing program should do. Overflow is reported on stderr rather than
ignored, because a dropped destructor is a file that never got flushed.

## Math

`src/math/fdlibm/` is Sun's fdlibm, kept **verbatim** (its notice preserved)
so it stays auditable against the original — the same source newlib's libm
is built from, ~1 ulp. `src/math/math.c` is the layer over it: its
`__ieee754_*` cores under the names C11 uses, the functions its 1993 set
never carried (`log2`, `exp2`, `log1p`, the inverse hyperbolics, `lgamma`,
`tgamma`, `erf`), IEEE classification, and the float and long-double forms.

`sqrt` is `__builtin_sqrt`, so EmbCC emits the machine's own instruction —
`sqrtsd` on x86-64, `fsqrt` on aarch64. Both are correctly rounded by
hardware, which no software core matches, and it is one instruction rather
than a call. Inline asm would have served one target; the builtin serves
every target that has the instruction, which is why it belongs in the
compiler.

`<complex.h>` is new — emlibc never had one, and it is why `complex.c` was
the last program that would not link. `cabs` is `hypot`, not
`sqrt(x*x + y*y)`: the naive form overflows for magnitudes that are
perfectly representable and underflows small ones to zero, which is the
commonest bug in a hand-rolled complex library.

## setjmp, and why it is written as bytes

`setjmp` saves the registers the ABI says a callee must preserve; `longjmp`
resumes at a stored address with a stored stack pointer. No C expression
denotes either, so these two functions are machine code — the only such
code in the library.

They are placed with `.byte`/`.long` in a file-scope `__asm__` block, each
instruction carrying its disassembly in a comment. The alternative was to
grow a full assembler inside the compiler for the sake of two functions;
`embcc -S` had already made the same call for the same reason, that the
bytes are what runs and nothing downstream gets to re-decide them.

A comment is not checked by anything, which is how an encoding rots. So
`tests/golden/libc.sh` checks it: it strips the bytes from one column and
the mnemonics from the other, assembles the mnemonics with the platform's
own assembler, and requires the two to agree byte for byte. That test
caught the aarch64 `stp d8, d9` encoding being wrong the first time it was
written.

What must be saved is exactly what the ABI calls callee-saved. On x86-64
that is `rbx`, `rbp`, `r12`–`r15`, the stack pointer and the return
address; every vector register is caller-saved, so none appear. On aarch64
it is `x19`–`x28`, the frame pointer, the link register, `sp` — **and the
low 64 bits of `v8`–`v15`**. Omitting those is the classic aarch64
`setjmp` bug: it surfaces only when the compiler happens to keep a `double`
in `v8` across the `setjmp`, which depends on the optimisation level.

## Time without a timezone

The calendar conversions are exact over the whole range of `time_t`, using
the days-from-civil algorithm rather than a loop over years: shift the year
to start in March, which moves the leap day to the end where it perturbs
nothing, and the month lengths become a closed form. `-1` seconds is
23:59:59 on 1969-12-31, not a negative time of day, which is what a
division that floors gives and a division that truncates does not.

There is **no timezone database**, so `localtime` is `gmtime` and `%z` is
`+0000`. That is a real limitation, and it is stated rather than hidden
behind a `TZ` variable this library would silently ignore.

The clock is separate from the calendar on purpose. `time()` and `clock()`
return whatever `__os_time`/`__os_clock_ns` give, which on a bare harness
is `-1`; a program that converts a `time_t` it obtained some other way
works on a target with no clock at all.

## scanf

One engine over a get/unget pair, the mirror of `printf`'s one engine over
a sink. Scanning needs one character of lookahead — `%d` on `"12x"` has to
read the `x` to learn the number ended — and exactly one is ever needed,
which is why a `scanf` can be written against `ungetc` at all.

The subtlety worth stating is the return value. C distinguishes an **input
failure** (the input ended before the first conversion completed, return
`EOF`) from a **matching failure** (there was input and it was the wrong
shape, return the number assigned, possibly 0). `sscanf("x", "%d", &n)`
consumes nothing and returns **0**, because there is an `x` there still to
be read; `sscanf("", "%d", &n)` returns `EOF`. A library that returns `EOF`
for both breaks every read loop written against it, and it is an easy
mistake to make when the test for "nothing happened" is written as
"consumed no characters".

## EmbLinkOS is a backend, not a second library

`emlibc` was EmbLinkOS's own C library. It is now
[`os/emblinkos/backend.c`](../../lib/libc/os/emblinkos/backend.c) — about
200 lines — and the operating system gets the same `printf`, `strtod`,
`malloc`, `qsort` and fdlibm as every other target.

That is the arithmetic the seam was designed for. `emlibc/rim/syscalls.c`
was 184 lines of genuinely EmbLinkOS-specific code sitting under several
thousand lines of portable C that duplicated what newlib already did.
Only the 184 lines were ever really the OS's, and only they survive.
A bug fixed in `printf` is now fixed for EmbLinkOS too, which was the
entire argument.

Three things in that file are worth reading, because they are what "a
backend" actually means:

**Paths.** The kernel resolves absolute paths only, so the working
directory is a userspace fact, and it lives in the backend rather than in
the portable library. What a name refers to is part of how an operating
system names files. A target with no filesystem never compiles it.

**errno.** The kernel returns `-errno` using the same numbers this
library's `<errno.h>` defines — `kernel/include/errno.h` and
`lib/libc/include/errno.h` agree value for value — so the mapping is the
identity. Saying that in a comment is better than a translation table that
would rot silently if either side moved.

**`clock()` fails.** `SYS_uptime_ms` exists and would give a plausible
number, which is exactly why it is not used: the kernel has no per-process
CPU accounting, so under any load the number would be wrong in a way the
caller cannot detect. EmbLinkOS's own `clock_gettime` refuses the CPU-time
clocks for the same reason. What the kernel does not provide is absent, not
stubbed to lie.

Build it with `make libc-emblinkos EMBLINKOS=/path/to/EmbLinkOs`. It is
opt-in because it needs that OS's ABI headers, and it takes the syscall
numbers from the OS's own `<embk.h>` rather than from a copy in this
repository — those numbers belong to the kernel and are hand-synchronised
with it, so a second copy would be a second thing to forget.
