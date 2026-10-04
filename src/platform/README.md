# `src/platform` — every host interaction, in one place

*Vision §16: "Porting EmbCC to a new host means implementing this layer and
nothing else."*

Above this directory no stage asks the host anything ISO C does not have
words for: no system header beyond the C library is included anywhere else
in `src/`, and nothing above it tests which host it runs on. (The driver
writes some outputs with `fopen` itself; that is ISO C, and every host has
it.) That is checkable:

```console
$ grep -rln "#include <unistd.h>" src/
src/platform/platform_posix.c
```

## What is here

| | |
|---|---|
| `platform.h` | the whole contract — the file calls, the environment, the console, the program's own path, `argv[0]`, and the source provider |
| `platform_common.c` | what every host shares, in ISO C: files, the environment, `argv[0]`, the source provider |
| `platform_posix.c` | the console and the program's path for macOS, Linux and **EmbLinkOS** (newlib provides `<unistd.h>`) |
| `platform_iso.c` | the same two answers in nothing but ISO C, for a hobby or non-POSIX OS: no automatic colour, and the program is where `argv[0]` says |

`make PLATFORM=iso` builds the last instead of `platform_posix.c`. A host
with more to say gets its own `platform_NAME.c` beside these, chosen by the
build (`PLATFORM=NAME`) — not an `#ifdef` inside one of them.
[docs/internals/porting.md](../../docs/internals/porting.md) is the guide.

## Why it is this small

EmbCC's host needs are: read a file, write a file, ask the environment a
question. Nothing else is NEEDED. EmbLinkOS has no `fork`/`exec`
(ARCHITECTURE §1), so a driver that spawned `as` or `ld` could not be
hosted on the target at all: the assembler and the linker are libraries
in the same process, and stay that way.

Running a program is the one optional capability. A host that can says
so through `plat_can_run`, and `plat_run_start`/`plat_run_wait` start a
program and wait for one to end; the driver uses them only to compile
several sources in one command, which on a host that cannot it refuses
with the way round it. The build chooses the implementation, as it
chooses the platform:

| `PROCESS` | file | hosts |
|---|---|---|
| `spawn` (default) | `process_spawn.c` | POSIX `posix_spawn`: macOS, Linux |
| `none` (default with `PLATFORM=iso`) | `process_none.c` | EmbLinkOS (its manifest, `tools/gen-embbuild-manifest.sh`), any host with only a C library |

A host with another way to start a program (Windows' `CreateProcess`,
EmbLinkOS's own spawn if it gains one) is another `process_NAME.c`.

There is no `mkdir`, no `stat`, no directory iteration either, because no
stage needs them. `plat_file_exists` is the one query, used to pick between
candidates when searching include directories — never as a guarantee, since
the answer can be stale by the time the file is opened.

## Sources are not ordinary files

Reading a *source* is different from reading an object, because an editor
holds sources that are not on disk yet (vision §7). So source bytes come
through `src_read`, and a language server installs its own provider:

```c
src_set_provider(my_buffers, ctx);   /* now the frontend sees the editor's text */
```

Both the file named on the command line and every `#include` go through it,
so an unsaved header is seen too, not just the file the editor asked about.
Everything else — objects, archives, dependency files, the output — is an
ordinary file and uses `plat_read_file` / `plat_write_file`.

With no provider installed, `src_read` *is* `plat_read_file`, so the batch
compiler is unaffected.

## Writing: build, then hand over

Stages build their output and hand it over as bytes rather than streaming to
a `FILE*` they opened themselves. `struct outbuf` in `src/driver/util.h` is
the buffer for text; binary writers (the ELF object writer, the linker)
already assembled a whole image and now simply pass it.

This is why the object writer no longer pads with `fputc(0)` loops: the image
is allocated zeroed, and the gaps *are* the padding.
