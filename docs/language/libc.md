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
| `<math.h>` | **not yet** — fdlibm is in emlibc and has to come across |
| `<time.h>`, `<setjmp.h>`, `<assert.h>`, `<inttypes.h>` | **not yet** |
| `<wchar.h>`, `<locale.h>`, `<signal.h>`, `<threads.h>` | **not yet** |
| `scanf` family | declared, **not yet implemented** |

**The acceptance test is the execution corpus.** 89 of the 90 x86-64
programs in `tests/exec` compile, link and run against this library with
**no newlib at all**, each producing its expected exit status. The one that
does not is `complex.c`, which needs `cabs` — math is the next piece.

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
