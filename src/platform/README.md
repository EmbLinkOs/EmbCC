# `src/platform` — every host interaction, in one place

*Vision §16: "Porting EmbCC to a new host means implementing this layer and
nothing else."*

Above this directory no stage knows what a `FILE*` is, which filesystem backs
a file, whether the host has an environment, or where bytes go when they are
written. That is checkable, and it is the point:

```console
$ grep -rn "fopen(" src/ | grep -v platform
$                                     # nothing
```

## What is here

| | |
|---|---|
| `platform.h` | the whole contract — four file calls, one environment call, and the source provider |
| `platform_posix.c` | the implementation for hosts with a C standard library: macOS, Linux, and **EmbLinkOS**, whose emlibc provides stdio |

A second host means a second file beside `platform_posix.c`, chosen by the
build — not an `#ifdef` inside it.

## Why it is this small

EmbCC's host needs are: read a file, write a file, ask the environment a
question. Nothing else. In particular there is **no process API here and
there must never be one**: EmbLinkOS has no `fork`/`exec` (ARCHITECTURE §1),
so a driver that spawned `as` or `ld` could not be hosted on the target at
all. The absence is load-bearing.

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
