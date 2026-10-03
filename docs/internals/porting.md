# Porting EmbCC to a new host

This page is for people who want to run EmbCC on an operating system it
does not support yet: a hobby OS, a non-POSIX OS, or any system with a C
library and no GCC or Clang. It covers what EmbCC needs from the host, the
platform layer you implement or choose, how the compiler finds its headers
and libraries, and how to build it.

## What EmbCC needs from the host

The compiler and its linker run in one process. They never start another
program, and they need no threads, signals, sockets or directory listings.
On a host, EmbCC needs:

- A hosted ISO C library (C99) with the functions below.
- A way to start a program with arguments (`argc`/`argv`).
- A file system that `fopen` can reach.

The compiler's sources include only ISO C headers: `<stdio.h>`,
`<stdlib.h>`, `<string.h>`, `<ctype.h>`, `<stdarg.h>`, `<setjmp.h>`,
`<stddef.h>`, `<limits.h>`, `<stdint.h>` and `<math.h>`. The one exception
is `src/platform/platform_posix.c`, which a POSIX host builds. Built with
`PLATFORM=iso` (below), the compiler calls only these C library functions:

| Header | Functions |
| --- | --- |
| `<stdio.h>` | `fopen` `fclose` `fread` `fwrite` `fseek` `ftell` `rewind` `remove` `fgetc` `fputc` `fputs` `printf` `fprintf` `vfprintf` `snprintf` `vsnprintf` `sprintf` `sscanf`, and `stdout`/`stderr` |
| `<stdlib.h>` | `malloc` `calloc` `realloc` `free` `exit` `abort` `atexit` `getenv` `qsort` `atoi` `atol` `strtol` `strtoul` `strtoull` `strtod` |
| `<string.h>` | `memcpy` `memmove` `memset` `memcmp` `memchr` `strlen` `strcmp` `strncmp` `strcpy` `strncpy` `strcat` `strchr` `strrchr` `strstr` `strcspn` |
| `<ctype.h>` | `isalnum` `isalpha` `isdigit` `isspace` `isxdigit` `tolower` `toupper` |
| `<setjmp.h>` | `setjmp` `longjmp` |

`tests/golden/host-iso.sh` builds the compiler this way and fails if it
references a POSIX call.

`getenv` may return `NULL` for every name: a host with no environment is
supported. The optional tools are a different matter. `embdbg`, `embls`
and `embidx` use POSIX (processes, sockets, terminals), and nothing in the
compiler depends on them.

## The platform layer

Everything EmbCC asks the host goes through `src/platform/platform.h`. No
other file in `src/` may include a system header beyond the ISO C library
or test which host it runs on.

| Function | What it answers |
| --- | --- |
| `plat_read_file`, `plat_write_file`, `plat_file_exists` | Whole-file reads and writes, and whether a path can be opened |
| `plat_getenv` | An environment variable, or `NULL` |
| `plat_stderr_is_terminal` | Whether diagnostics may be coloured automatically |
| `plat_self_path` | The path of the running compiler, or `NULL` |
| `plat_set_argv0`, `plat_argv0_path` | `argv[0]`, recorded by the driver at startup |
| `src_read`, `src_exists`, `src_set_provider` | Source files, which a language server can serve from its buffers |

The layer is split in three files:

- **`platform_common.c`** holds the files, the environment, `argv[0]` and
  the source provider, in ISO C. Every host builds it.
- **`platform_posix.c`** is for macOS, Linux and EmbLinkOS. It uses
  `isatty` for colour, and `/proc/self/exe` or `_NSGetExecutablePath` for
  the compiler's own path.
- **`platform_iso.c`** is for every other host. Diagnostics are never
  coloured automatically (`-fdiagnostics-color` still works), and the
  compiler's path is `argv[0]` when that is a path.

The Makefile chooses with `PLATFORM`:

```sh
make embcc                 # PLATFORM=posix, the default
make embcc PLATFORM=iso    # nothing but ISO C
```

A host that can answer more, such as one with a terminal API or a call
that returns the executable's path, gets its own `platform_NAME.c` beside
these, built with `PLATFORM=NAME`. It must define `plat_stderr_is_terminal`
and `plat_self_path`. Do not add `#ifdef`s to the existing files.

## Finding headers and libraries

The compiler looks for its support files, which are the C headers, the C++
headers and the per-target libraries, in this order:

1. **`EMBCC_PREFIX`** from the environment: `$EMBCC_PREFIX/lib/embcc/VERSION`.
2. **The compiler's own path** (`plat_self_path`):
   - a build tree, when `lib/libc/include/stdio.h` is beside the binary;
   - otherwise an installed layout, `PREFIX/bin/embcc` with the files in
     `PREFIX/lib/embcc/VERSION`.
3. **A prefix compiled in** with `make DEFAULT_PREFIX=/path`. This is used
   only when the host cannot say where the compiler is, for example with
   no environment and an `argv[0]` that is a bare name.

If none of these applies, the compiler uses only what its command line
gives it (`-I`, `-isystem`, `-nostdinc`). `embcc --print-search-dirs` shows
what was found:

```text
layout: build tree
self: ./embcc
lib: .
```

## Building EmbCC

The source tree compiles with any C99 compiler, EmbCC included. The default
build uses `make`:

```sh
make embcc embld          # the compiler and the linker
make install PREFIX=/opt/embcc
```

`make install` copies the programs to `PREFIX/bin` and the support files to
`PREFIX/lib/embcc/VERSION`. `DESTDIR` stages the copy elsewhere, as for
packaging.

The libraries are packaged with [`embar`](../manual/tools/embar.md),
EmbCC's own archiver, which is ISO C like the compiler. Building EmbCC and
its libraries needs a C compiler and nothing else from a toolchain.

On a host with no `make`, compile every file of the `SRCS` list in the
Makefile (`make -pn | sed -n 's/^SRCS := //p'` prints it on a machine that
has `make`), plus `build/embdbg_core.o` from `tools/embdbg/embdbg.c` with
`-DEMBDBG_NO_MAIN`, and link them into one program. EmbLinkOS does exactly
this from a generated manifest, `build.ebm` (see
[EmbBuild manifests](../manual/tools/embbuild.md)).

### Building EmbCC for an OS that has no compiler yet

Use EmbCC on another machine to cross-compile EmbCC for the new OS:

1. Build `embcc` and `embld` on a machine that has them.
2. Compile each source file with `embcc --target=ARCH-elf -c`, using the new
   OS's C headers (`-nostdinc -isystem OS/include`) and `PLATFORM=iso`'s
   files (`platform_common.c` and `platform_iso.c`).
3. Link the objects with `embld`, the OS's startup object and its C
   library, at the address its program loader expects (`-e`, `-Ttext`).
4. Copy the result and the support files (`lib/libc/include`, `include/`
   and the target libraries) to the OS, and set `EMBCC_PREFIX` or build
   with `DEFAULT_PREFIX`.

EmbLinkOS is built this way: its C library is newlib, and its `embcc.elf`
comes from `build.ebm`.
