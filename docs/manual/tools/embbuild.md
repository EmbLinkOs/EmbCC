# EmbBuild manifests (`.ebm`)

This page is the reference for EmbBuild manifests, the `.ebm` build files
that `embbuild`, the build tool of EmbLinkOS, executes. It is for anyone
who builds EmbCC, the EmbLinkOS kernel or their own programs on
EmbLinkOS, and for contributors who keep EmbCC's own manifest,
`build.ebm`, in step with the sources. It covers how `embbuild` walks a
manifest, the complete file syntax, how it decides what to rebuild, its
messages and exit status, and the manifest tools that ship with EmbCC.

## NAME

`embbuild` — run the recipes of an EmbBuild manifest, rebuilding only
the targets whose inputs or commands changed

## SYNOPSIS

```text
embbuild MANIFEST
```

The manifest tools in the EmbCC source tree:

```text
tools/gen-embbuild-manifest.sh > build.ebm
MYOS=DIR tools/gen-kernel-manifest.sh > kernel.build.ebm
tools/embbuild-run.sh MANIFEST [OUTDIR]
```

## DESCRIPTION

`embbuild` is an EmbLinkOS application, installed as
`/data/apps/embbuild/embbuild.elf`; the EmbLinkOS shell finds it by the
name `embbuild`. It is not part of EmbCC and is not built from this
repository: its source is `shell/tools/embbuild.c` in the EmbLinkOS
tree. This page describes the version whose version string is
`embbuild v1.0.0 (2024-06-05)`. `embcc` does not read `.ebm` files and
has no `build` command; `embcc build` fails with
`embcc: error: unknown argument 'build'`.

<!-- UNVERIFIED on EmbLinkOS itself: the behavior on this page was
checked against embbuild.c and by running that source on the host, with
spawn/wait shimmed to posix_spawn/waitpid and /data/ redirected. -->

A manifest is a list of *stanzas*. Each stanza names one output file,
the input files it depends on, and the command that produces it.
`embbuild` executes the stanzas in the order they are written and skips
a stanza when its inputs, its command and the tool are unchanged since
the stanza last succeeded. Apart from the values that `measure` stanzas
define, it has no variables; it has no functions, pattern rules or
parallel jobs, and it never runs a shell.

The EmbCC source tree provides:

| File | Purpose |
|---|---|
| `build.ebm` | The manifest that builds EmbCC on EmbLinkOS. See [EmbCC's manifest](#embccs-manifest-buildebm). |
| `tools/gen-embbuild-manifest.sh` | Generates `build.ebm` from the Makefile's source list, with each unit's header dependencies derived. |
| `tools/gen-kernel-manifest.sh` | Generates the manifest that builds the EmbLinkOS kernel with `embcc` and `embld`. See [The kernel manifest](#the-kernel-manifest). |
| `tools/embbuild-run.sh` | A host-side walker that runs EmbCC's manifest with the host's `embcc` and `embld`. See [The host walker](#the-host-walker). |
| `tests/golden/x86_64/embbuild.sh` | Fails when `build.ebm` is out of date, then builds EmbCC from it with the host walker. |
| `tests/golden/x86_64/embbuild-kernel.sh` | Builds the kernel from its generated manifest and boots it in QEMU. Runs only with `EMBCC_KM1=1`. |

### How a run proceeds

1. The manifest is read in full. A syntax error, an unknown key or a
   stanza without `name`, `kind` or `output` stops the run before
   anything executes.
2. The [ordering check](#order-of-stanzas) runs. A manifest that
   consumes an output before the stanza producing it stops here.
3. The directories `/data/build/stamps/PROJECT/` and
   `/data/build/out/PROJECT/` are created if they are missing.
4. The stanzas are processed one at a time, from the top of the file.
   For each stanza:
   - A `measure` stanza computes its value and binds it to a name.
   - Any other stanza has `${NAME}` references in its `args` replaced,
     its *stamp* computed (see [Incremental rebuilds](#incremental-rebuilds)),
     and is skipped when the stamp matches the stamp recorded by its
     last successful run and its output file exists. Otherwise its
     action runs, and on success the new stamp is recorded.
5. The first stanza that fails stops the walk. Later stanzas do not run,
   and `embbuild` exits with status 1.
6. After the last stanza, `embbuild` prints how many stanzas ran and
   how many were up to date.

Every run walks the whole manifest. There is no way to build a single
stanza or a subset.

### Running a recipe

For every kind except `install`, `measure` and `package`, the stanza's
`args` is the argument vector of one process. The first word is the
program; it is passed unchanged as both the program path and `argv[0]`.

- There is no shell: no quoting, escaping, globbing, redirection or
  pipes. Every word of `args` reaches the program as one argument.
- There is no `PATH` search. Write the program as an absolute path, as
  in `/data/apps/embcc/embcc.elf`.
- The process starts with an empty environment.
- `embbuild` waits for the process to exit. Exit status 0 is success;
  any other status fails the stanza.

### Incremental rebuilds

A stanza's stamp is a CRC-32C checksum computed, in this order, over:

1. each entry of `inputs`, in order: its path, then the file's bytes;
2. each word of `args`, after `${NAME}` replacement;
3. the version string of `embbuild` itself;
4. for a `package` stanza, its `version`, `caps` and `grant` values.

The stamp is stored as eight hexadecimal digits in
`/data/build/stamps/PROJECT/NAME.stamp`. A stanza is skipped only when
that file holds the same value **and** the stanza's `output` exists.

What follows from this:

- File times play no part. Touching a file rebuilds nothing, and
  neither does an edit that is undone before the next run.
- Changing a flag in `args`, or the order or spelling of `inputs`,
  reruns the stanza.
- A file not listed in `inputs` is not watched. A header a unit includes
  but does not list can change without the unit being rebuilt. The
  generators in this repository derive each unit's header list for that
  reason.
- The program in `args` is identified by its path only. Replacing a
  compiler binary at the same path does not rerun anything unless the
  binary is also listed in `inputs`.
- Inputs are read when the walk reaches the stanza, so a stanza sees the
  bytes an earlier stanza produced in the same run. When a rebuilt
  object comes out byte for byte the same as before (after an edit to a
  comment, for example), the stanzas that consume it stay up to date and
  do not run.
- Deleting an output reruns its stanza even though the stamp matches. A
  stanza whose command never creates its `output` runs on every walk.
- `kind` and `output` are not part of the stamp.
- A different `embbuild` version reruns every stanza.
- A failed stanza records no stamp. If a stamp cannot be written,
  `embbuild` reports it and continues; the stanza runs again on the next
  walk. A `name` containing `/` cannot be used as a stamp file name, so
  such a stanza runs on every walk.
- Stamps belong to a project and a stanza name. Two stanzas with the same
  name in one manifest overwrite each other's stamp and both run on every
  walk. Two manifests with the same `project` share their stamps.

To rebuild a project from scratch, delete `/data/build/stamps/PROJECT/`.
Deleting `/data/build` removes every stamp and every staged output.

### Order of stanzas

The order of the stanzas in the file is the build order. `embbuild`
builds no dependency graph and does not reorder anything. Before
running, it checks the order the author chose: when an entry of a
stanza's `inputs` is exactly the `output` of a stanza that comes later in
the file, the run stops with

```text
embbuild: ordering error: stanza 'A' at line N consumes output of stanza 'B' at line M, but appears later in the manifest
```

where `A` is the consuming stanza and `B` the producing one. Paths are
compared as text: `/data/build/out/p/a.o` and `/data/build/out/p/./a.o`
are different paths to the check. A stanza that lists its own output as
an input is not reported by the check; on its first run it fails with
`embbuild: input missing`.

An input that does not exist when the walk reaches its stanza is an
error, whatever the reason.

### Where outputs go

The directory `/data/build/out/PROJECT/` is created by every run and is
where a project's outputs normally go. `embbuild` creates no other
directory: an output in a directory that does not exist fails the
stanza. `embbuild` does not write into `/system`: an `install` stanza
whose output is `/system` or lies under `/system/` is refused. A program
that belongs in `/system` is built into `/data/build/out/` and then
adopted into `/system` by a separate, deliberate step (EmbLinkOS
`docs/BUILD.md`, section 3).

## BUILD FILE SYNTAX

### Lines

A manifest is a text file read one line at a time.

- Spaces and tabs at the start of a line, and white space (including a
  carriage return) at its end, are ignored. Files with CRLF line endings
  are accepted.
- An empty line ends the current stanza.
- A line whose first non-blank character is `#` is a comment. A comment
  line does not end a stanza. A `#` anywhere else is ordinary text:
  `args: /bin/x a.c  # note` passes `#` and `note` to the program as
  arguments.
- Every other line has the form `KEY: VALUE`. The line is split at its
  first colon, and both sides are trimmed, so `output :  /x` is accepted
  and a value may itself contain colons. A line without a colon is an
  error.
- A line may hold at most 8190 characters. A longer line is read as two,
  and the manifest is then misread, usually reported as a syntax error
  or a missing key.
- There are no continuation lines, quotes, escapes, include files or
  conditionals. The only substitution is `${NAME}` in `args` (see
  [Derived values](#derived-values)).

### Value types

| Type | Meaning |
|---|---|
| word | The rest of the line after the colon, trimmed. At most 255 bytes; a longer value is truncated. |
| list | The rest of the line, split at spaces and tabs. At most 128 items, each at most 255 bytes; more items, or a longer item, is an error. |

A key given twice in one stanza keeps its last value, except `grant`,
which accumulates one entry per line.

### The `project` line

```text
project: NAME
```

Required. `NAME` names the project: it is the directory of the stamps
(`/data/build/stamps/NAME/`) and of the staged outputs
(`/data/build/out/NAME/`), and it appears in the final summary line. The
`project` line does not begin or end a stanza and may appear anywhere;
when there are several, the last one counts. Write it once, at the top
of the file. A manifest without it is refused with
`embbuild: manifest missing 'project:' declaration`.

### Stanza keys

A stanza begins at the first key line after an empty line (or after the
start of the file) and ends at the next empty line. Two stanzas without
an empty line between them are one stanza: the second `name` replaces
the first.

| Key | Type | Required | Meaning |
|---|---|---|---|
| `name` | word | yes | Identifies the stanza in messages and names its stamp file, `NAME.stamp`. Use a name that is unique in the project and valid as a file name. |
| `kind` | word | yes | What the stanza does. See [Stanza kinds](#stanza-kinds). |
| `inputs` | list of paths | no; default empty | The files the stanza depends on. Their paths and contents are part of the stamp. For `install`, `measure` and `package`, the single file the stanza acts on. |
| `args` | list | for kinds that run a program | The argument vector of the program to run, program first. For `measure`, the operation. `${NAME}` is replaced. |
| `output` | word | yes | The file the stanza produces. For `measure`, the name of the value it defines. |
| `version` | word | no; default `0.0.0` | `package` only: the package version. |
| `caps` | list | no; default empty | `package` only: the capability names the package declares. |
| `grant` | word, repeatable | no | `package` only: one file-system grant per line, `ro PREFIX` or `rw PREFIX`. |

Any other key stops the run with
`embbuild: unknown key 'KEY' in stanza 'NAME' at line N`.

The stamp file's path, `/data/build/stamps/PROJECT/NAME.stamp`, must fit
in 255 bytes; keep project and stanza names short.

### Stanza kinds

| Kind | Action | Uses | Stamped |
|---|---|---|---|
| `compile`, `link` | Runs the program in `args`. | `inputs`, `args`, `output` | yes |
| any other word | Runs the program in `args`. | `inputs`, `args`, `output` | yes |
| `install` | Copies its one input to `output`. | `inputs`, `output` | yes |
| `measure` | Defines a value from the size of its one input. | `inputs`, `args`, `output` | no |
| `package` | Writes a package specification and runs `pkgbuild`. | `inputs`, `output`, `version`, `caps`, `grant` | yes |

#### `compile` and `link`

The conventional kinds for compiling a unit and linking a program. They
behave identically: the kind is shown in the progress line and has no
other effect. `kind` is not checked against a list, so any word other
than `install`, `measure` and `package` behaves the same way.

#### `install`

Copies the stanza's single input to `output`, byte for byte, creating or
truncating the file with mode `0755`. `args` is not used. The directory
of `output` must exist. The output must not be `/system` or a path under
`/system/`:

```text
embbuild: install stanza 'NAME' cannot write into /system -- stage to /data/build/out and ADOPT (BUILD.md §3)
```

The usual use is to copy a staged program to its application
directory, `/data/apps/NAME/NAME.elf`.

#### `measure`

Measures the stanza's single input and binds the result, as a decimal
number, to the name given in `output`. `args` holds exactly one word,
the operation:

| Operation | Value |
|---|---|
| `size` | the file's size in bytes |
| `sectors` | the file's size divided by 512, rounded up |

A `measure` stanza has no stamp. It runs on every walk and is counted
among the stanzas that ran. Its input must exist when the walk reaches
it, so a stanza that produces the input must come earlier. At most 32
names can be defined; measuring a name again replaces its value.

#### `package`

Builds an EmbLinkOS package from the stanza's single input, a linked
program. `embbuild` writes `DIR/NAME.pkgspec`, where `DIR` is the
directory of `output`:

```text
name: NAME
version: VERSION
caps: CAP...
grant: MODE PREFIX
```

with one `grant` line per `grant` key, and then runs

```text
/data/apps/pkgbuild/pkgbuild.elf DIR/NAME.pkgspec INPUT DIR
```

The `version`, `caps` and `grant` values are part of the stamp, so
changing the declared authority rebuilds the package. What `pkgbuild`
writes is described in EmbLinkOS `docs/PACKAGING_AND_SDK.md`.

### Derived values

`${NAME}` in a word of `args` is replaced by the value an earlier
`measure` stanza bound to `NAME`. The reference may be part of a word,
as in `-DSECTORS=${kernel_sectors}`. Replacement happens before the stamp
is computed, so a changed value reruns the stanza that uses it.

- A name that no earlier `measure` stanza defined is an error:
  `embbuild: undefined ${NAME}`.
- Only `args` is expanded. `inputs`, `output` and the other keys are
  used as written.
- A `$` that is not followed by `{`, and a `${` without a closing `}`,
  are kept as written.
- A word longer than 255 bytes after replacement is an error.

### Limits

| Limit | Value | When exceeded |
|---|---|---|
| Stanzas in a manifest | 128 | `embbuild: too many stanzas in manifest` |
| Items in one `inputs`, `args` or `caps` list | 128 | `embbuild: too many inputs in stanza 'NAME'` (or `args`, `caps`) |
| Length of one list item | 255 bytes | `embbuild: inputs token too long in stanza 'NAME'` (or `args`, `caps`) |
| Length of `project`, `name`, `kind`, `output`, `version` and of each `grant` | 255 bytes | truncated without a message |
| Length of a line | 8190 characters | the line is split and the manifest misread |
| `grant` lines in one stanza | 128 | further lines are ignored |
| Names defined by `measure` | 32 | `embbuild: too many measured values (>32)`; the name stays undefined |

## OPTIONS

`embbuild` has no options. It takes exactly one argument, the path of
the manifest. Any other number of arguments prints

```text
Usage: PROGRAM <manifest>
```

and exits with status 1. An argument that looks like an option is taken
as a manifest path.

## OUTPUT

Progress goes to standard output:

| Line | When |
|---|---|
| `[embbuild] NAME KIND -> OUTPUT` | before a stanza runs; `NAME` is padded to 16 columns |
| `embbuild: skipping 'NAME' (up to date)` | a stanza is up to date |
| `[embbuild] NAME measure OP(INPUT) -> VAR = VALUE` | a `measure` stanza ran |
| `[embbuild] PROJECT: N ran, M up_to_date` | the walk finished without error |

Errors go to standard error, each prefixed with `embbuild:`. Whatever a
recipe's program prints is its own.

## EXIT STATUS

| Status | Meaning |
|---|---|
| 0 | every stanza ran successfully or was up to date |
| 1 | a usage error, an unreadable or invalid manifest, an ordering error, a missing input, an undefined `${NAME}`, or a stanza that failed |

When the walk stops at a failing stanza, the stanzas that completed
before it keep their new stamps. The next run skips them, unless their
inputs changed again, and starts work at the failing stanza.

## DIAGNOSTICS

Each message below is printed with the prefix `embbuild: `. Errors found
while reading the manifest stop the run before any stanza executes:

| Message | Cause |
|---|---|
| `cannot open manifest: PATH` | the manifest cannot be opened |
| `syntax error in manifest at line N: TEXT` | a line that is neither empty, a comment nor `KEY: VALUE` |
| `unknown key 'KEY' in stanza 'NAME' at line N` | a key not in the [table](#stanza-keys); `NAME` is empty when the key comes before `name` |
| `manifest missing 'project:' declaration` | no `project` line, or an empty one |
| `stanza at line N missing 'name:'` | |
| `stanza 'NAME' at line N missing 'kind:'` | |
| `stanza 'NAME' at line N missing 'output:'` | |
| `too many stanzas in manifest` | more than 128 stanzas |
| `too many inputs in stanza 'NAME'` | more than 128 items in a list (also `args`, `caps`) |
| `inputs token too long in stanza 'NAME'` | a list item of 256 bytes or more (also `args`, `caps`) |
| `ordering error: ...` | see [Order of stanzas](#order-of-stanzas) |

Errors during the walk stop it at the failing stanza. When a stanza's
program, `install` copy or `package` step fails, the message is
followed by `embbuild: stanza 'NAME' failed`:

| Message | Cause |
|---|---|
| `input missing: PATH`, then `failed to hash input 'PATH' for stanza 'NAME'` | an entry of `inputs` does not exist |
| `read error: PATH` | an input could not be read |
| `undefined ${VAR}`, then `arg interpolation failed in stanza 'NAME'` | `args` uses a name no earlier `measure` defined |
| `failed to spawn 'PROGRAM': N` | the program could not be started; `N` is the error code |
| `command 'PROGRAM' exited with status N` | the program failed |
| `install stanza 'NAME' must have exactly one input` | |
| `install stanza 'NAME' cannot write into /system -- ...` | see [`install`](#install) |
| `failed to open input 'PATH'`, `failed to open output 'PATH'`, `error copying 'IN' to 'OUT'` | an `install` copy failed |
| `measure 'NAME' needs exactly one input` | |
| `measure 'NAME' needs one arg (op: size \| sectors)` | `args` is empty or has more than one word |
| `measure 'NAME': unknown op 'OP' (want size \| sectors)` | |
| `measure 'NAME': cannot stat 'PATH'` | the measured file does not exist |
| `package stanza 'NAME' needs exactly one input (the linked ELF)` | |
| `cannot write PATH` | the `.pkgspec` could not be written |
| `cannot run pkgbuild (N)`, `pkgbuild failed for 'NAME'` | `pkgbuild` could not be started, or failed |

One message is a warning and does not stop the walk:
`failed to write stamp for stanza 'NAME'`.

## ENVIRONMENT

`embbuild` reads no environment variables, and the programs it runs
receive none.

## FILES

| Path | Contents |
|---|---|
| `/data/apps/embbuild/embbuild.elf` | the program |
| `/data/build/stamps/PROJECT/NAME.stamp` | the stamp of each stanza that has succeeded |
| `/data/build/out/PROJECT/` | the staging directory, created by every run |
| `/data/apps/pkgbuild/pkgbuild.elf` | the packager that `package` stanzas run |

## EMBCC'S MANIFEST: `build.ebm`

`build.ebm`, at the top of the EmbCC source tree, builds EmbCC on
EmbLinkOS with the EmbCC and EmbLD installed there. It is generated by
`tools/gen-embbuild-manifest.sh`; do not edit it by hand.

### Layout

The manifest uses the EmbLinkOS paths:

| Path on EmbLinkOS | Contents |
|---|---|
| `/data/src/embcc/` | the `src/` directory of this tree, without the `src/` prefix (`src/driver/main.c` is `/data/src/embcc/driver/main.c`) |
| `/data/src/embcc/tools/` | the units taken from `tools/` (`tools/embdbg/embdbg.c`) |
| `/data/apps/embcc/embcc.elf` | the compiler that runs every compile |
| `/data/apps/embcc/include/` | EmbCC's own headers, `include/` in this tree |
| `/system/abi/include/` | the newlib headers |
| `/system/abi/crt0.o`, `/system/abi/syscalls.o`, `/system/abi/libc.a` | the start-up object, the system-call layer and the C library the link uses |
| `/data/apps/embld/embld.elf` | the linker |
| `/data/build/out/embcc/` | where every object and `embcc.elf` are written |

### Contents

The manifest's project is `embcc`. It holds one `compile` stanza for
each source file in the Makefile's `SRCS` list, one for
`tools/embdbg/embdbg.c`, and one `link` stanza, `embcc.elf`, last.

- A compile stanza's name is the source's path below `src/` (or below
  `tools/`), with `/` replaced by `_` and `.c` by `.o`:
  `src/driver/main.c` becomes `driver_main.o`.
- Its `inputs` are the source file followed by the headers it includes,
  directly or indirectly, from this tree and from newlib, each listed
  once.
- Its command is
  `/data/apps/embcc/embcc.elf -c -I/data/apps/embcc/include -I/system/abi/include SOURCE -o /data/build/out/embcc/OBJECT`.
  `tools/embdbg/embdbg.c` is compiled with `-DEMBDBG_NO_MAIN -Wno-unused-function`
  added before the source, the flags the Makefile uses for it.
- The link stanza's `inputs` and its command list `crt0.o`,
  `syscalls.o`, every object in stanza order, and `libc.a`:
  `/data/apps/embld/embld.elf -o /data/build/out/embcc/embcc.elf /system/abi/crt0.o /system/abi/syscalls.o OBJECTS... /system/abi/libc.a`.

The compiler binary is not one of the inputs. After installing a new
`/data/apps/embcc/embcc.elf`, delete `/data/build/stamps/embcc/` to
rebuild every object with it.

With the sources and `build.ebm` in place under `/data/src/embcc/`,
build EmbCC with:

```sh
embbuild /data/src/embcc/build.ebm
```

<!-- UNVERIFIED: run on the host build of embbuild.c with the paths mapped
to the host tree (all 90 stanzas ran; a second run reported "0 ran, 90
up_to_date"); not run on EmbLinkOS. -->

### Regenerating `build.ebm`

`build.ebm` lists each unit's header dependencies, so it is out of date
when a source file is added or removed, when an `#include` line
changes, or when a header starts including another. Regenerate it from
the top of the source tree:

```sh
tools/gen-embbuild-manifest.sh > build.ebm
```

The generator needs:

- `make`, from which it reads `SRCS`, `SRCS_TOOLCORE` and
  `TOOLCORE_CFLAGS` (with `make -pn`; nothing is built);
- the host C compiler as `cc`, whose `-MM` output gives each unit's
  header dependencies;
- the newlib headers for x86-64, in `NEWLIB_INC`. The default is the
  `include` directory of the x86-64 newlib that `tools/hostpaths.sh`
  locates (`EMBCC_X86_NEWLIB` overrides its location).

A dependency outside `include/`, `src/`, `tools/` and `NEWLIB_INC` is
a file of the host only (a file of the macOS SDK, for example) and is
left out of the manifest.

`tests/golden/x86_64/embbuild.sh` regenerates the manifest into
`tests/golden/out/embbuild/embcc.build.ebm` and compares it with the
committed `build.ebm`. When they differ it fails with

```text
committed build.ebm is STALE — re-run tools/gen-embbuild-manifest.sh
```

When they match, it builds EmbCC from the manifest with the host walker.
The test prints `skipped: PATH not present on this host` and passes when
the newlib headers, `crt0.o`, `syscalls.o` or `libc.a` are missing.

### The kernel manifest

`tools/gen-kernel-manifest.sh` writes, to standard output, a manifest
that builds the EmbLinkOS kernel with `embcc` and `embld` alone:

```sh
MYOS=$HOME/EmbLinkOs tools/gen-kernel-manifest.sh > kernel.build.ebm
```

`EMBCC_MYOS`, or else `MYOS`, names the EmbLinkOS source tree; the
default is the tree `tools/hostpaths.sh` finds. The generator reads
`KERNEL_SRC` and the `-I` directories of `CFLAGS` from that tree's
Makefile by running `make -p` there, which also runs that Makefile's
default target. It derives each unit's headers with `cc -MM`, keeping
the headers inside the tree. The manifest's project is `kernel`, and it
contains:

- one `compile` stanza for each C file in `KERNEL_SRC`, compiled with
  `-mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -O2`, the
  Makefile's include directories as `-I/data/src/DIR`, and
  `-I/data/apps/embcc/include`;
- one `compile` stanza for each of the six kernel `.asm` files, which
  `embcc -c` assembles (see [embas](embas.md));
- one `link` stanza, `kernel.elf`, linked with
  `-e _start -Ttext 0xFFFFFFFF80100000 --lma-offset 0xFFFFFFFF80000000`
  (see [embld](embld.md)).

The object names are the path below `kernel/` with `/` and `.` replaced
by `_`, followed by `.o`, so a `.c` and an `.asm` file of the same name
do not collide. Outputs go to `/data/build/out/kernel/`.

The kernel manifest has one stanza per kernel source file, more than the
128 stanzas `embbuild` v1.0.0 accepts, and its link command has more
than 128 words. `embbuild` refuses it with
`embbuild: too many stanzas in manifest`. The opt-in test
`tests/golden/x86_64/embbuild-kernel.sh` walks it with a host walker of
its own, which has no such limits, and boots the resulting kernel.

### The host walker

`tools/embbuild-run.sh` runs a manifest on the development host, so that
`build.ebm` can be shown to build before it is shipped:

```text
tools/embbuild-run.sh MANIFEST [OUTDIR]
```

It changes to the top of the source tree, deletes and recreates
`OUTDIR` (default `tests/golden/out/embbuild`), and walks the manifest,
replacing each EmbLinkOS path in `args` with its host equivalent
(host paths below are relative to the top of the source tree):

| EmbLinkOS path | Host path |
|---|---|
| `/data/apps/embcc/embcc.elf` | `embcc` |
| `/data/apps/embld/embld.elf` | `embld` |
| `/data/apps/embcc/include` | `include` |
| `/system/abi/include` | `$NEWLIB_INC` |
| `/system/abi/crt0.o`, `syscalls.o`, `libc.a` | `$CRT0`, `$SYSCALLS`, `$LIBC` |
| `/data/src/embcc/tools/...` | `tools/...` |
| `/data/src/embcc/...` | `src/...` |
| `/data/build/out/embcc` | `OUTDIR/stage` |

A path glued to `-I` is mapped too. `NEWLIB_INC`, `CRT0`, `SYSCALLS` and
`LIBC` default to the x86-64 newlib and the EmbLinkOS build directory
that `tools/hostpaths.sh` finds; when one of them is missing, the script
prints `skipped: PATH not present on this host` and exits with status 0.

The host walker is not a second implementation of `embbuild`:

- It keeps no stamps: every stanza runs on every walk.
- It ignores `inputs` and `output`, so it performs neither the ordering
  check nor the missing-input check.
- It does not tell kinds apart. Every stanza that has a `kind` has its
  `args` run as a command, so `install`, `measure` and `package`
  stanzas, and `${NAME}` references, are not supported.
- It splits `args` with the shell, so a word containing `*`, `?` or `[`
  is subject to file-name expansion.
- It recognizes a key only at the very start of a line and followed by
  a colon and a space (`name: `), and ignores every line it does not
  recognize.

It prints `  [KIND] NAME` for each stanza and stops at the first failing
command with `embbuild-run: recipe failed for NAME` (status 1). After the
walk it requires `OUTDIR/stage/embcc.elf` to be an `ET_EXEC` file with no
undefined symbols, so it accepts only EmbCC's own manifest. On success
the last line is

```text
manifest built embcc.elf (N bytes): ET_EXEC, every symbol resolved
```

The resulting `embcc.elf` is an EmbLinkOS program linked against newlib
and does not run on the host.

## EXAMPLES

A program of two units, linked against the EmbLinkOS C library and
installed into its application directory. `/data/apps/hello/` must
exist.

```text
# /data/src/hello/build.ebm
project: hello

name: main.o
kind: compile
inputs: /data/src/hello/main.c /data/src/hello/greet.h
args: /data/apps/embcc/embcc.elf -c -I/data/apps/embcc/include -I/system/abi/include /data/src/hello/main.c -o /data/build/out/hello/main.o
output: /data/build/out/hello/main.o

name: greet.o
kind: compile
inputs: /data/src/hello/greet.c /data/src/hello/greet.h
args: /data/apps/embcc/embcc.elf -c -I/data/apps/embcc/include -I/system/abi/include /data/src/hello/greet.c -o /data/build/out/hello/greet.o
output: /data/build/out/hello/greet.o

name: hello.elf
kind: link
inputs: /system/abi/crt0.o /system/abi/syscalls.o /data/build/out/hello/main.o /data/build/out/hello/greet.o /system/abi/libc.a
args: /data/apps/embld/embld.elf -o /data/build/out/hello/hello.elf /system/abi/crt0.o /system/abi/syscalls.o /data/build/out/hello/main.o /data/build/out/hello/greet.o /system/abi/libc.a
output: /data/build/out/hello/hello.elf

name: install
kind: install
inputs: /data/build/out/hello/hello.elf
output: /data/apps/hello/hello.elf
```

The first run builds everything:

```text
$ embbuild /data/src/hello/build.ebm
[embbuild] main.o           compile -> /data/build/out/hello/main.o
[embbuild] greet.o          compile -> /data/build/out/hello/greet.o
[embbuild] hello.elf        link -> /data/build/out/hello/hello.elf
[embbuild] install          install -> /data/apps/hello/hello.elf
[embbuild] hello: 4 ran, 0 up_to_date
```

A second run, with nothing changed, runs nothing:

```text
embbuild: skipping 'main.o' (up to date)
embbuild: skipping 'greet.o' (up to date)
embbuild: skipping 'hello.elf' (up to date)
embbuild: skipping 'install' (up to date)
[embbuild] hello: 0 ran, 4 up_to_date
```

After a change to the code in `greet.c`, only that unit, the link and
the install run:

```text
embbuild: skipping 'main.o' (up to date)
[embbuild] greet.o          compile -> /data/build/out/hello/greet.o
[embbuild] hello.elf        link -> /data/build/out/hello/hello.elf
[embbuild] install          install -> /data/apps/hello/hello.elf
[embbuild] hello: 3 ran, 1 up_to_date
```

After adding a comment to `greet.h`, both units are recompiled, because
both list the header. Their objects come out unchanged, so the link and
the install do not run:

```text
[embbuild] main.o           compile -> /data/build/out/hello/main.o
[embbuild] greet.o          compile -> /data/build/out/hello/greet.o
embbuild: skipping 'hello.elf' (up to date)
embbuild: skipping 'install' (up to date)
[embbuild] hello: 2 ran, 2 up_to_date
```

Pass a file's size, in 512-byte sectors, to a later compile:

```text
project: boot

name: kernel_sectors
kind: measure
inputs: /data/build/out/boot/kernel.elf
args: sectors
output: kernel_sectors

name: loader.o
kind: compile
inputs: /data/src/boot/loader.c
args: /data/apps/embcc/embcc.elf -c -DSECTORS=${kernel_sectors} /data/src/boot/loader.c -o /data/build/out/boot/loader.o
output: /data/build/out/boot/loader.o
```

```text
[embbuild] kernel_sectors   measure sectors(/data/build/out/boot/kernel.elf) -> kernel_sectors = 92
[embbuild] loader.o         compile -> /data/build/out/boot/loader.o
[embbuild] boot: 2 ran, 0 up_to_date
```

When `kernel.elf` grows to 102 sectors, the next run compiles
`loader.o` again with `-DSECTORS=102`; while it stays the same size,
`loader.o` is up to date.

Regenerate EmbCC's manifest after adding a source file, and check it.
The test runs from the top of the source tree and uses the `embcc` and
`embld` built there:

```sh
make embcc embld
tools/gen-embbuild-manifest.sh > build.ebm
sh tests/golden/x86_64/embbuild.sh
```

## SEE ALSO

[embld](embld.md), [embas](embas.md),
[Invoking EmbCC](../invoking.md), [Targets](../targets.md#x86-64),
[Testing](../../internals/testing.md),
[Contributing](../../internals/contributing.md),
EmbLinkOS `docs/BUILD.md`
