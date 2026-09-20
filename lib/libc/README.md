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
      string/ ctype/ stdlib/ stdio/ time/ setjmp/ math/
    arch/<arch>/      what cannot be written in portable C
                      (setjmp's register save, atomics)
    os/<name>/        the backend: ~11 primitives (os/backend.h)

## The seam

`os/backend.h` is the whole contract between the library and an operating
system. A new OS implements those and gets the entire C library; nothing
above the seam knows which OS it is on, and there are no `#ifdef`s there.

## Status

Under construction. What exists, what does not, and what each part is tested
by is in `docs/language/libc.md`.
