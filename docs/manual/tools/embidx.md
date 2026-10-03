# embidx — the cross-unit interface index

This page is the reference for `embidx`, which keeps a persistent index
of what every translation unit of a project defines and depends on, and
answers questions about it that no single unit can: which units must be
rebuilt after an edit, which units disagree about a declaration, and
where a declaration is defined and used. It is for people who drive
incremental builds or check a multi-file project with EmbCC. It covers
the four commands, the index file format, and the limits of each
command's answer.

## NAME

`embidx` — build and query a project's cross-unit interface index

## SYNOPSIS

```text
embidx build [-o FILE] [FLAG]... UNIT...
embidx stale FILE
embidx check FILE
embidx who USR FILE
embidx --help
```

## DESCRIPTION

`embidx` does not parse C itself. For each unit it runs
[`embcc --emit-interfaces`](../invoking.md#--emit-interfaces), which
compiles the unit through semantic analysis, generates no code, and
prints the unit's interface record. `embidx build` stores those records,
for many units at once, in one index file. The other commands read the
index.

The compiler is the program named by the `EMBCC` environment variable,
or `./embcc` in the current directory when `EMBCC` is unset or empty. It
is run through the shell as

```text
EMBCC --emit-interfaces FLAG... UNIT
```

with its standard error discarded. A unit path or a flag that contains a
space or a shell metacharacter is split or reinterpreted by the shell and
does not reach the compiler intact; a unit whose path contains a space is
skipped. To see why a unit was skipped, run the same
`embcc --emit-interfaces` command by hand.

Unit paths are stored exactly as given, and `stale` reopens them by
those paths. Run `embidx stale` from the directory `embidx build` was run
from, or give absolute paths.

`embidx` is not a build system: it compiles no objects and runs nothing
but the front end. The index is a cache. Deleting it loses nothing that
`embidx build` cannot reconstruct from the sources.

`embidx` is built by `make embidx` (and by `make all`) and installed by
`make install` next to `embcc`; see
[Getting started](../getting-started.md).

### Units, USRs and hashes

A unit's record lists three kinds of fact:

- `file`: every file the preprocessor read for the unit, the unit itself
  included, with a hash of the file's bytes.
- `provides`: every function and variable the unit defines, with an
  interface hash. This includes `static` functions and variables, and
  tentative definitions such as `int g;`. A weak definition is marked
  `weak`: one declared `__attribute__((weak))`, and every C++ inline
  function, template instance and member function defined in its class.
- `uses`: every function and variable the unit declares, does not
  define, and refers to; and every struct or union that the unit's
  declarations reach. A declaration reaches a struct or union when it is
  the type of a parameter, a return value, a local variable or a global
  variable, directly or through pointers and arrays, or the type of a
  member of a struct or union already reached. A function or variable that is
  declared and never referred to is not listed.

Each entity is named by a USR, a name that stays the same while
unrelated code changes:

| Entity | USR | Example |
|---|---|---|
| function with external linkage | `c:@F@NAME` | `c:@F@shared` |
| variable with external linkage | `c:@V@NAME` | `c:@V@g` |
| `static` function | `c:FILE@F@NAME` | `c:cfg.h@F@sq` |
| `static` variable at file scope | `c:FILE@V@NAME` | `c:cfg.h@V@LIMIT` |
| `static` variable inside a function | `c:UNIT@V@FUNCTION.NAME` | `c:t1.c@V@tick.n` |
| struct | `c:@S@TAG` | `c:@S@P` |
| union | `c:@U@TAG` | `c:@U@U` |

`FILE` is the file that contains the definition, as the compiler named
it: for a `static` function defined in a header, the header. `UNIT` is
the unit's source file. A USR that names a file is one of an
internal-linkage entity: each unit has its own, so `check` never
compares two of them. In a C++ unit, a function's USR carries its
mangled name (`c:@F@_Z4cxxfP1S`).
Enumerations and typedefs never appear in the index.

Two hashes are recorded, and they answer different questions:

- A file hash changes whenever a file's text changes, comments included.
  It decides whether a unit must be re-examined.
- An interface hash changes only when something a dependent can observe
  changes. It decides whether a unit must be rebuilt.

An interface hash covers, for a function, its linkage, return type,
parameter types, whether it is variadic, `_Noreturn` or weak; for a
variable, its linkage, type, size, alignment and whether it is weak; for
a struct or union, its size, alignment, and each member's name, type,
offset and bit-field position. A struct named in a function's signature
contributes only its tag to the function's hash; its layout is in the
struct's own `uses` entry. Function bodies, initializers, comments, line
numbers and declaration order are not hashed.

## COMMANDS

### `build`

```text
embidx build [-o FILE] [FLAG]... UNIT...
```

Run the compiler on each `UNIT` with every `FLAG`, and write the index
to `FILE`, or to standard output when `-o` is not given. Each argument
that begins with `-`, other than `-o`, is a compiler flag; every other
argument is a unit. Flags and units may be mixed in any order, and every
flag applies to every unit. Units are C or C++ source files.

With `-o`, a summary follows on standard output:

```text
indexed 2 units into project.embidx
```

A unit the compiler cannot process (a missing file, a compile error, an
input `--emit-interfaces` does not accept) is reported on standard error
and left out:

```text
embidx: src/net.c: could not be read (skipped)
```

The index is still written, with the units that could be read, and the
summary ends with `(some could not be read)`. The exit status is then 1.

`build` always writes the whole index anew; it does not update an
existing one. The same units, flags and file contents give a
byte-identical index, whatever order the units are listed in.

Refusals:

```text
embidx: no units to index
embidx: too many units
embidx: too many compiler flags
embidx: cannot write FILE
```

At most 4096 units can be given. The flags, each counted with one
following space, may total at most 8190 bytes.

### `stale`

```text
embidx stale FILE
```

Report which units of the index must be rebuilt, given the sources as
they are now on disk. `stale` runs the compiler on every unit again, with
the flags recorded in the index, and compares the fresh record with the
stored one:

1. A unit that no longer compiles, or no longer exists, must be rebuilt.
2. A unit whose `file` entries all match (same files, same hashes) is
   unchanged and is not reported.
3. Otherwise the unit is re-examined. It must be rebuilt if its own
   source file's hash changed, if its `provides` or `uses` entries
   differ from the stored ones in any USR or hash, or if the hash of its
   preprocessed text differs. That last is every token the compiler
   sees, from the unit and every header, without line markers and blank
   lines; a comment, which the preprocessor removes, does not change it.
   If only other files changed and none of these did, it is not
   rebuilt.

Each unit to rebuild is printed on standard output with the reason, and
a count follows on standard error. After an edit to a function body in
`a.c`, a member added to a struct that `a.c` and `b.c` both reach, and the
deletion of `c.c`:

```text
a.c: rebuild (its own source changed)
b.c: rebuild (an interface it observes changed)
c.c: rebuild (it no longer compiles, or is gone)
2 units re-examined, 3 need rebuilding
```

A unit rebuilt for its text alone is reported as
`rebuild (the text it compiles changed)`: a macro used inside a body, the
value of an enumeration constant, the body of a `static inline` function
or the initializer of a variable in a header, or a struct named only in
a `sizeof` or a cast. When a unit's own source changed, that reason is
given even if an interface changed too. `an interface it observes changed` is printed for
a change to the unit's `provides` entries as well as to its `uses`
entries. Units in the third category are counted as re-examined; units
in the first are not.

A comment added to a header that several units include changes the
header's file hash and no interface hash: every unit that includes it is
re-examined and none is rebuilt. A change to the layout of a struct
declared in that header rebuilds every unit that reaches the struct. An
edit to one function's body rebuilds that function's unit only.

`stale` never writes the index. After rebuilding, run `embidx build`
again so that the index describes the new sources. `stale` exits with
status 0 whether or not anything needs rebuilding.

#### What `stale` does not detect

A change to the compiler flags: `stale` reuses the flags recorded by
`build`. To change them, run `build` again. An index written before the
text hash was recorded has none, and every re-examined unit in it is
rebuilt until `build` is run again.

A change to a declaration in a header that a unit includes but never
uses rebuilds the unit too, because its preprocessed text changed: the
text hash cannot tell an unused declaration from a used one.

### `check`

```text
embidx check FILE
```

Report what is inconsistent across the units of the index, on standard
output. Two kinds of problem are reported.

A `conflict` is a USR for which two units recorded different interface
hashes: two units compiled against different versions of one struct, or
a declaration that has drifted from its definition. Each unit compiles
and the link succeeds, but the program is wrong. When both units use the
USR, the report reads:

```text
conflict c:@S@P
  a.c sees 523ff34061e7bbcf
  c.c sees 093f0f8baef5a9e2
```

When one unit uses it and the other defines it:

```text
conflict c:@F@shared
  a_use.c uses     063e545517d091c8
  z_def.c provides 4cec7e075c64d934
```

A `defined twice` problem is a function or variable with external
linkage that two units both define, neither of them weakly, whatever
their hashes:

```text
defined twice c:@V@g
  t1.c
  t2.c
```

A problem is reported once for each pair of units involved. The last
line counts the units, the problems, and the references to functions or
variables that no unit in the index defines, counted once per unit and
name:

```text
3 units, 2 problems, 0 calls or references to outside the index
```

A reference to outside the index is not a problem: it may be satisfied
by a library at link time. The exit status is 1 when at least one
problem was reported, and 0 otherwise.

#### Limits of `check`

- A `uses` entry is compared with a `provides` entry only when the unit
  that uses the declaration sorts before the unit that defines it (byte
  order of the paths). With `a_def.c` defining `long shared(long)` and
  `z_use.c` declaring `int shared(int)`, no conflict is reported.
- A non-`static` C `inline` function defined in a header is reported
  as `defined twice` for each pair of units that include it. This is
  accurate: EmbCC emits it as an external definition in every unit (see
  [Inline functions](../c-language.md#inline-functions)), and the link
  fails.

### `who`

```text
embidx who USR FILE
```

Print every unit that defines `USR`, then every unit that uses it, with
the hash each recorded:

```text
provides b.c d45ae76166fc1e58
uses     a.c d45ae76166fc1e58
uses     c.c d45ae76166fc1e58
```

`USR` must be given exactly as the index spells it; see
[Units, USRs and hashes](#units-usrs-and-hashes). When no unit mentions
it, `who` prints `no unit in the index mentions USR` and exits with
status 1.

## OPTIONS

### `-o FILE`

`build` only. Write the index to `FILE` instead of standard output, and
print the summary line. `-o` and `FILE` must be separate arguments:
`-oFILE` begins with `-` and is passed to the compiler as a flag, which
makes every unit fail. A `-o` that is the last argument is likewise
passed to the compiler.

### `FLAG`

`build` only. Any other argument that begins with `-` is passed to the
compiler, unchanged, for every unit, and is recorded in the index for
`stale`. A flag must be a single argument. Attach the value to options
that take one (`-Iinclude`, `-DNDEBUG=1`, `-isystemDIR`,
`--target=riscv32-unknown-elf`): in `-I include`, `include` is taken as a
unit, and the `-I` swallows the next unit when the compiler runs.

### `--help`, `-h`

Print a usage summary on standard error and exit with status 0. The
option is recognized only as the first argument.

Running `embidx` with no arguments, or with a first argument that is not
a command, prints the same summary and exits with status 2.

## INDEX FILE FORMAT

The index is line-oriented text. `embidx build` writes:

```text
; EmbCC index v1
; a persistent cross-TU index (vision 8.2). Discard it freely:
; everything here is rebuilt from the sources by 'embidx build'.
unit a.c
  args -Iinclude 
  text 6ea31017449bd3c5
  file 3d2d8cb47f5c17d9 a.c
  file e3acb06fe74e21f6 include/hdr.h
  provides d45ae76166fc1e58 c:@F@f
  provides 3769e4c2b874ce54 c:@V@g
  uses d45ae76166fc1e58 c:@F@shared
  uses 523ff34061e7bbcf c:@S@P
unit b.c
  args -Iinclude 
  text 0f1d4fb4ef267d4f
  file 2eb669e3815a0f37 b.c
  file e3acb06fe74e21f6 include/hdr.h
  provides d45ae76166fc1e58 c:@F@shared
  uses 523ff34061e7bbcf c:@S@P
```

- The first line is `; EmbCC index v1`. A file whose first line does not
  begin with it is refused:
  `embidx: FILE is not an EmbCC index, or is a version this does not know`.
- Lines that begin with `;`, and empty lines, are comments.
- `unit PATH` begins a unit's record; `PATH` is the rest of the line. The
  lines up to the next `unit` belong to it, indented by two spaces. A
  fact before the first `unit` is refused:
  `embidx: FILE: a fact before any unit`.
- `args FLAGS` holds the compiler flags, each followed by one space. A
  unit indexed with no flags has `args` followed by a single space.
- `text HASH` is the hash of the unit's preprocessed text, line markers
  and blank lines left out, which `stale` compares (see
  [`stale`](#stale)). An index without it is read, and its units are
  rebuilt when re-examined.
- `file HASH PATH`, `provides HASH USR` and `uses HASH USR` are the facts
  of [Units, USRs and hashes](#units-usrs-and-hashes). The hash comes
  first, unlike in the `--emit-interfaces` record. A weak definition's
  `provides` line ends in ` weak`.
- A hash is 16 lowercase hexadecimal digits and is only ever compared
  for equality. A file hash is the 64-bit FNV-1a hash of the file's
  bytes, or `0000000000000000` when the file could not be read.
- Units are sorted by path, and within a unit the `file`, `provides` and
  `uses` lines each sorted by their last field, all in byte order.
- A line with an unknown keyword, or with fewer than three fields, is
  ignored when the index is read.

Because the output is sorted and contains no time stamps, the indexes of
two builds can be compared with `diff`.

## OUTPUT

| Command | Standard output | Standard error |
|---|---|---|
| `build` | the index (without `-o`), or the summary line (with `-o`) | units skipped |
| `stale` | one line per unit to rebuild | the re-examined/rebuild count |
| `check` | every problem, and the summary line | — |
| `who` | the defining and using units | — |

Every error message is printed on standard error, prefixed with
`embidx:`. An index that cannot be opened is reported as
`embidx: cannot read FILE (build it with 'embidx build')`.

## EXIT STATUS

| Status | Meaning |
|---|---|
| 0 | `build`: every unit was indexed. `stale`: the report was produced. `check`: no problem was found. `who`: the USR was found. `--help` |
| 1 | `build`: at least one unit was skipped, there were too many units or flags, or the index could not be written. `check`: at least one problem was found. `who`: no unit mentions the USR. Any command: a missing argument, or an index that cannot be read |
| 2 | no command, or an unknown command |

## ENVIRONMENT

### `EMBCC`

The compiler `embidx` runs. When unset or empty, `./embcc`, relative to
the current directory.

## EXAMPLES

Index every unit of a project:

```sh
export EMBCC=/opt/embcc/bin/embcc
embidx build -o project.embidx -Iinclude src/*.c
```

After editing, list what must be rebuilt, rebuild it, and bring the
index up to date:

```sh
for u in $(embidx stale project.embidx 2>/dev/null | sed 's/: rebuild (.*)$//'); do
    embcc -c -Iinclude "$u" -o "${u%.c}.o"
done
embidx build -o project.embidx -Iinclude src/*.c
```

Index a firmware project for a target, with a macro defined:

```sh
embidx build -o fw.embidx --target=thumbv7m-none-eabi -DBOARD=3 -Iinclude src/*.c
```

Fail a CI job when two units disagree about a declaration:

```sh
embidx build -o project.embidx -Iinclude src/*.c
embidx check project.embidx || exit 1
```

Find where a function is defined and every unit that calls it:

```sh
embidx who c:@F@shared project.embidx
```

Compare the interfaces of two builds:

```sh
embidx build -Iinclude src/*.c > before.embidx
# ... edit ...
embidx build -Iinclude src/*.c > after.embidx
diff before.embidx after.embidx
```

## SEE ALSO

[Invoking EmbCC](../invoking.md#--emit-interfaces) (`--emit-interfaces`,
[`-MD`](../invoking.md#-md--mmd)), [`embld`](embld.md),
[Getting started](../getting-started.md),
[Testing](../../internals/testing.md)
