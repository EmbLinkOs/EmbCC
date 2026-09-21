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

**`os/backend.h` is the entire contract with an operating system**: thirteen
primitives — read, write, open, close, lseek, sbrk, time, clock, exit,
isatty, getentropy, remove, rename. A new OS implements those and gets
the library. Nothing
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

## strtod

Decimal text to a binary float is harder than it looks: the decimal value
almost never has an exact binary form, so the answer is a *rounding*, and
where it lands can depend on digits past any accumulator.

The fast path is **exact**, and that is its point. Powers of ten up to
10²² are exactly representable as doubles, and so is an integer of up to
15 significant digits. When both hold, one multiply or divide of two
exact values rounds once — and IEEE makes that single rounding the
nearest representable value. So `strtod("1e22")` compares *equal* to
`1e22`, not merely close, and the golden asserts equality.

Outside that range the value is scaled by repeated squaring, which rounds
more than once and can land one unit in the last place from nearest. C11
§7.22.1.3p10 permits exactly that, and this says so rather than claiming
better; correct rounding for every input needs arbitrary-precision
arithmetic. Hex floats (`0x1.8p3`) are exact by construction — they are
binary already — and are parsed separately for that reason.

## printf, the other direction

Binary float to decimal text, and here the answer *is* exact. A double is
*m* × 2^*e* with *m* an integer, so its value always has a finite decimal
expansion: for *e* ≥ 0 it is the integer *m*·2^*e*, and for *e* < 0 it is
*m*·5^−*e* with the point shifted, because 1/2^*k* is 5^*k*/10^*k*. The
digits therefore come out of **integer multiplication alone** — no
division, no approximation, and no question about where a tie falls.
`lib/libc/src/stdio/format.c` carries a small base-10⁹ big integer for
it; 86 limbs is the most any double needs, because the smallest subnormal
expands to 767 digits.

It did not start that way, and the difference is worth stating because it
is the shape of the bug every hand-written formatter has. The first
version scaled the value in floating point and rounded half away from
zero. That is wrong twice:

- `%.0f` of 2.5 printed `3`. An exact tie rounds to **even**, so C and
  every other library print `2`. Ties away from zero make a column of
  half-cent figures drift upwards, which is the whole reason the rule
  exists.
- `%.1f` of 0.35 printed `0.4`. This one is worse, because 0.35 is *not
  a tie at all* — the nearest double is slightly below it, so `0.3` is
  right. Multiplying the fraction by ten rounded it up to exactly 3.5 and
  manufactured a tie that was never there. The information was destroyed
  by the first multiply, and no amount of care afterwards recovers it.

A sweep of **29,090 conversions** — every style at every precision over
ties, subnormals, the extremes and four thousand random bit patterns —
now matches a known-correct library byte for byte. The golden keeps the
cases that used to fail.

Two things are absent rather than wrong: `%a`, and `long double`, which
narrows to a double before conversion — so `%Lf` of a value outside
double's range prints `inf`. Doing that exactly needs a big integer
fourteen times larger (5^16445 rather than 5^1074), paid on every call
for a conversion nothing here makes.

## The wide half, and three headers that reach the hardware

`<wchar.h>`, `<wctype.h>` and `<uchar.h>` arrived together, and the
substance is the **UTF-8 conversion**, not the string functions. The
multibyte encoding is locale-dependent in C; this library has one locale
(below) and its encoding is UTF-8, which is the only choice that is not
arbitrary.

Most of that file is **refusals**, and each is a real attack rather than
a corner case:

- **Overlong forms.** `C0 80` decodes arithmetically to U+0000, so a
  decoder that accepts it lets a NUL through a string that was checked
  for NULs. Every length has a smallest value it may encode.
- **Surrogates.** U+D800–U+DFFF are not characters; they exist only as
  UTF-16 code units. Accepting them gives one string two encodings.
- **Anything above U+10FFFF.** Not a code point. The old five- and
  six-byte forms encode them and are not UTF-8.

And the conversion **state** is a real object, not a placeholder: a
sequence split across two calls has to be remembered somewhere, and a
function that ignores it mangles exactly the input that arrives in
chunks — which is the case that only shows up in production. `mbrtowc`
returns −2 and keeps its place.

UTF-16 is where `<uchar.h>` earns its keep. A code point above U+FFFF
takes **two** code units, so `mbrtoc16` returns the high surrogate,
consumes the bytes, and returns the low one on the next call consuming
*nothing* — which is what the return value −3 means and is the only way
a one-unit-at-a-time interface can report it. `c16rtomb` mirrors it:
handed a high surrogate it writes nothing and returns 0.

`wchar_t` is 32 bits here, so one `wchar_t` is one code point and there
are no surrogates to think about in the wide direction. That is the
comfortable case, and it is worth saying because the uncomfortable one —
16-bit `wchar_t`, where a code point may take two — is why so much
wide-character code is subtly wrong.

**`<locale.h>` has one locale, named "C".** A locale is a table of
cultural conventions that has to come from somewhere, and on a
freestanding target there is nowhere; shipping somebody's idea of a
decimal separator and calling it the system's would be worse than
saying so. `setlocale` accepts `"C"` and `""` — the native locale here
*is* `"C"` — and returns NULL for anything else, which is the same
interface a hosted library offers. `<wctype.h>` follows from it: in the
"C" locale the classifications are exactly the basic execution character
set and "no" above it, because that is what the locale *means*.

**`<fenv.h>` reaches a hardware register**, and the two targets disagree
about where everything is. x86-64 keeps the sticky flags and the
rounding mode in one register, MXCSR — the flag bits *are* the `FE_*`
values, which is why they were numbered that way. aarch64 splits them:
FPSR for the flags, FPCR for the mode, in a different bit order, so both
directions are translated rather than passed through. Passing them
through would work on one target and silently test the wrong bit on the
other.

The golden does not merely read the mode back. It sets one and checks
that `1.0/3.0` rounds **differently** — the only check that proves the
right bits are being set.

**`<signal.h>` is the C standard's model, not POSIX's**: six signals, one
handler each, and a very short list of what a handler may legally do —
`abort`, `_Exit`, `quick_exit`, `signal` for its own signal, and
assigning to a `volatile sig_atomic_t`. Not printf, not malloc: the
handler can interrupt those mid-way and re-entering finds a broken
invariant. C has no way for an OS to *deliver* one, because C does not
know what an OS is, so `raise` is the only path a signal takes here —
and that is the real thing for a freestanding target, not a
simplification. A backend that gains delivery calls `__raise_signal` and
everything above it is already correct.

## Every width, and two ties-to-even bugs

`<tgmath.h>` is type-generic math: `sqrt(x)` calls `sqrtf`, `sqrt` or
`sqrtl` depending on what `x` is, through `_Generic`. Writing it
exposed a large hole — C11 requires **all three widths for every
function** in `<math.h>`, and about sixty were missing. The hole was
invisible until then, because a missing variant is only a compile error
when something asks for it by width, and nothing had.

The variants split into two kinds. The ones that **compute** a new value
go through the double implementation: exact for `float`, and for `long
double` a real loss — an 80-bit x87 long double has 64 mantissa bits and
gets 53. That is stated rather than hidden. The ones that **move or
inspect** a value — `truncl`, `floorl`, `roundl`, `fmodl`, `frexpl`,
`modfl`, `ldexpl`, `fabsl`, `copysignl` — are written in long double
throughout, because narrowing them would be a visible defect: `truncl`
of a value with more than 53 significant bits must not lose them.

Two functions were wrong, both in the same family as printf's old
rounding:

**`nearbyint` was `floor(x + 0.5)`,** which is `round()` under another
name. Its entire job is to follow the *current rounding direction*, and
the default is ties-to-even: `nearbyint(2.5)` is 2 and `round(2.5)` is
3. It now reads `fegetround()` and honours all four modes — which is
only possible because `<fenv.h>` exists.

**`remainder` had no tie rule.** `fmod` truncates the quotient; IEEE's
`remainder` rounds it to nearest with ties to **even**. `remainder(7, 2)`
was 1 and must be −1, because the nearest multiple of 2 to 7 is 8. This
is the case argument reduction depends on: reducing an angle by π/2
lands on a tie at every odd multiple of π/4.

The whole family was then diffed against a known-correct libm —
`nearbyint`, `rint`, `round`, `remainder`, `remquo`, `llrint`, `fmod`
across their interesting inputs — and matches byte for byte.

## C's threads, over the same seam

`<threads.h>` is built on the same eight OS primitives the C++
`<mutex>` and `<thread>` use. One set, two spellings: a target that
gains threads lights up both at once, and neither library knows which
OS it is on.

The locks are the three-state futex mutex described in
`lib/libcxx/include/mutex`, so the uncontended path is one
compare-exchange and never enters the kernel. Mutexes, condition
variables and `call_once` work fully on a single-threaded target;
`thrd_create` returns `thrd_error` rather than a thread that never runs
or one that runs on the caller — the second deadlocks the first time
anything joins from inside it.

`tss_create` also fails honestly. Thread-specific storage needs
thread-local storage underneath, which this compiler does not have, and
a key that every thread shared would be a global under another name —
with the failure invisible until two threads corrupted each other.
`thread_local` is likewise left undefined rather than aliased to
nothing.

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
