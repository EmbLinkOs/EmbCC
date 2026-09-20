# `lib/libc` — our C library

*One implementation, ported to an OS by one small backend. The same shape as
`src/platform` for the compiler itself: everything portable above a thin
seam, and the seam is the only thing a new host has to write.*

A compiler that targets several operating systems cannot borrow one OS's C
library. EmbCC used newlib for its test targets and EmbLinkOS had its own
`emlibc`, which meant two `printf`s, two `strtod`s, and two sets of bugs —
the duplication this project removes everywhere else.

## Layout

    include/          the hosted C11 headers
    src/              the library, portable C
      string/ ctype/ stdlib/ stdio/ time/ math/
      setjmp/         the one part that cannot be portable C
    os/<name>/        the backend: 11 primitives (os/backend.h)
      posixlike/      write/read/open/... -- the test harness, anything Unix
      emblinkos/      EmbLinkOS's syscall ABI; this is what replaced emlibc

`src/setjmp/` holds one file per architecture, each wrapped whole in
`#ifdef __x86_64__` / `#ifdef __aarch64__` so the build picks one and the
other compiles to nothing. That is file selection rather than conditional
code: no function in this library has two bodies. See
`docs/language/libc.md` for why those two functions are machine code.

## The seam

`os/backend.h` is the whole contract between the library and an operating
system. A new OS implements those and gets the entire C library; nothing
above the seam knows which OS it is on, and there are no `#ifdef`s there.

EmbLinkOS is the proof. Its C library was `emlibc`; it is now
`os/emblinkos/backend.c`, about 200 lines, and that OS gets the same
printf, malloc and math as every other target.

## Status

What exists, what does not, and what each part is tested by is in
`docs/language/libc.md`. The acceptance test is `tests/golden/libc.sh`:
every program in `tests/exec` built and run against this library with no
newlib at all.
