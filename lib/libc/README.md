# `lib/libc` — our C library

*One implementation, ported to an OS by one small backend. The same shape as
`src/platform` for the compiler itself: everything portable above a thin
seam, and the seam is the only thing a new host has to write.*

A compiler that targets several operating systems cannot borrow one OS's C
library. EmbCC used newlib for its test targets and EmbLinkOS had its own
`emlibc`, which meant two `printf`s, two `strtod`s, and two sets of bugs —
the duplication this project removes everywhere else.

## Layout

    include/          the hosted C11 headers, and POSIX's
    src/              the library, portable C
      string/ ctype/ stdlib/ stdio/ time/ math/
      posix/          POSIX above C: descriptors, files, directories,
                      processes and spawn, popen, sockets and the resolver,
                      terminals and ptys, users, mmap, poll, sysconf; and
                      libgen, fnmatch, getopt, search, err; and GNU's argp,
                      obstack, argz, backtrace, the res_* resolver API
      setjmp/         the one part that cannot be portable C
    os/backend.h      the contract: 140 primitives, C's few and POSIX's many
    os/absent.inc     each primitive as an ENOSYS failure, for what a backend
                      has not got
    os/<name>/        the backends
      linux/          Linux system calls; no other libc underneath
      emblinkos/      EmbLinkOS's syscall ABI; this is what replaced emlibc,
                      and with LIBC=emblibc, newlib
      baremetal/      a part with no OS
      posixlike/      write/read/open/... -- the test harness, anything Unix

`src/setjmp/` holds one file per architecture, each wrapped whole in
`#ifdef __x86_64__` / `#ifdef __aarch64__` so the build picks one and the
other compiles to nothing. That is file selection rather than conditional
code: no function in this library has two bodies.

## The seam

`os/backend.h` is the whole contract between the library and an operating
system. A new OS implements those and gets the entire C library; nothing
above the seam knows which OS it is on, and there are no `#ifdef`s there.

EmbLinkOS is the proof. Its C library was `emlibc`, then newlib with a
retargeting file; it is now `os/emblinkos/backend.c`, and that OS gets the
same printf, malloc, math, time zones, sockets and processes as every
other target -- its whole userland builds on it (`make LIBC=emblibc` in
EmbLinkOS).

`tests/golden/libc-backends.sh` holds every backend to the whole contract.

## Status

What exists and what does not is in `docs/manual/libraries.md`. The
acceptance tests are `tests/golden/libc.sh` -- every program in
`tests/exec` built and run against this library with no newlib at all --
and `tests/golden/linux-libc-exec.sh`, which runs the POSIX programs on a
Linux kernel, where each value was first checked against glibc's.
