# embar — the EmbCC archiver

`embar` builds and lists static libraries (`.a` archives). EmbCC's own C
library, C++ library and compiler runtime are packaged with it, so a host
needs no binutils or LLVM `ar`. This page is the command reference.

## Synopsis

```text
embar [-]{r|q}[cSs] ARCHIVE FILE...
embar [-]t ARCHIVE
```

## Description

`embar` writes the System V / GNU archive format that `embld`, GNU `ld`,
`lld` and the binutils and LLVM tools all read:

- the `!<arch>` signature;
- a `/` symbol index;
- a `//` table for member names longer than fifteen characters;
- each member, padded to an even size.

The symbol index lists every global or weak symbol a member defines, and
common symbols, for little-endian ELF32 and ELF64 objects. Those are the
formats of every target EmbCC compiles for. A member in another format is
stored with no index entries. `embld` does not need the index, because it
reads the members themselves; other linkers do.

The output is deterministic. Every member's time, owner and group are 0,
and its mode is 644, so the same objects in the same order always give the
same bytes, as `ar D` does in binutils.

A member is named after the file's base name, without its directory.

## Options

The first argument is an operation letter, optionally followed by
modifier letters. A leading `-` is allowed.

### `r`

Insert each `FILE` into `ARCHIVE`, replacing a member of the same name in
place and appending the others. `ARCHIVE` is created if it does not exist.

### `q`

Append each `FILE`, without looking for a member of the same name.

### `t`

Print the members' names, one per line, in archive order.

### `c`

Do not report that the archive was created.

### `s`, `S`

Write the symbol index (`s`, the default) or leave it out (`S`).

### `D`, `u`, `v`

Accepted for compatibility with `ar`, and without effect: the output is
always deterministic, and every named file is always written.

## Exit status

0 on success. 1 when a file cannot be read or written, or an existing
archive is corrupt. 2 for a usage error.

## Examples

```sh
embcc --target=thumbv7em-none-eabi -c a.c -o a.o
embcc --target=thumbv7em-none-eabi -c b.c -o b.o
embar rcs libboard.a a.o b.o
embar t libboard.a
embld -e reset -Ttext 0x0 -Tdata 0x20000000 main.o libboard.a -o fw.elf
```

The build uses `embar` for every library unless `EMBCC_X86_AR`,
`EMBCC_AARCH64_AR` or `EMBCC_AR` names another archiver.

## See also

[embld](embld.md), [Libraries](../libraries.md),
[Porting EmbCC to a new host](../../internals/porting.md).
