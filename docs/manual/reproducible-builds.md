# Reproducible builds and SBOMs

A build is reproducible when the same sources, built with the same
compiler and options, give the same bytes, whoever builds them, wherever
and whenever. EmbCC's output is deterministic: it has no randomness, no
timestamps of its own, and no dependence on the order a directory lists
its files. Two things a build brings in from outside can still change the
bytes, the build directory's path and the clock, and both have options.

A software bill of materials (SBOM) lists what went into an image: every
source and header with its hash and license, the libraries linked, and
the compiler. EmbCC writes one from the compile itself, so it lists the
files that were really read, not the ones a manifest claims.

## Paths

A path is written into the output in three places: `__FILE__` (an
`assert`'s message, a log line), the DWARF debug information, and the
coverage notes. Each prefix map rewrites a path that starts with `OLD` to
start with `NEW`:

| Option | GCC's spelling | Rewrites the paths in |
|---|---|---|
| `--prefix-map=OLD=NEW` | `-ffile-prefix-map=OLD=NEW` | All three |
| | `-fmacro-prefix-map=OLD=NEW` | `__FILE__` and `__BASE_FILE__` |
| | `-fdebug-prefix-map=OLD=NEW` | The DWARF |
| | `-fprofile-prefix-map=OLD=NEW` | The coverage notes |

```sh
embcc -g -O2 --prefix-map="$PWD"=/src -c motor.c
```

Several maps may be given. A path is rewritten by the last map that
matches it, as with GCC, so put the general maps first and the specific
ones after them. The DWARF's compile directory is rewritten too, so a
debugger finds the sources when `/src` is set as its source path (in GDB,
`set substitute-path /src /home/me/fw`).

## Time

`__DATE__` and `__TIME__` are the time the compile started, unless
`SOURCE_DATE_EPOCH` is set, in which case they are that time in UTC, as
the [reproducible-builds.org](https://reproducible-builds.org/specs/source-date-epoch/)
specification asks. The SBOM's creation time follows the same rule.
Nothing else EmbCC writes carries a time: its objects, archives and COFF
files have no timestamps.

```sh
SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) make
```

`-frandom-seed=` is accepted and changes nothing, since there is nothing
random to seed.

## Software bill of materials

```sh
embcc --sbom -c motor.c -o motor.o                 # the object records its inputs
embcc --sbom -c main.c -o main.o
embcc motor.o main.o -o fw.elf --link-script=board.ld --sbom=fw.spdx.json
```

`--sbom` on a compile records, in a debug section of the object
(`.debug_embcc_sbom`, kept by every linker and loaded by no loader), the
source and every header the preprocessor read. Each comes with its
SHA-256 and the `SPDX-License-Identifier` it declares, if it has one in
its first 4 KiB. `--sbom=FILE` on the link writes the image's bill. It
lists the image and its SHA-256, the compiler that built it, and every
file that went in: the sources, the headers, and the objects and
archives linked, including EmbCC's C library and runtime. Each file
carries its hash, and the image has a `GENERATED_FROM` relationship to
each. One command that compiles and links does both.

On macOS the record is in the object's `__EMBCC,__sbom` section, which
ld64 keeps in the image, so `--sbom=FILE` at a Darwin link and
`--sbom-from` on a Darwin image work as they do for ELF. A COFF object
carries it in `.debug_embcc_sbom`.

| Option | Effect |
|---|---|
| `--sbom` | Record the unit's inputs in its object |
| `--sbom=FILE` | At a link, write the image's bill to `FILE`. At a compile, also record the inputs |
| `--sbom-format=FORMAT` | `spdx`, SPDX 2.3 JSON (the default), or `cyclonedx`, CycloneDX 1.5 JSON |
| `--sbom-from IMAGE [-o FILE]` | Write the bill of an image already linked from objects built with `--sbom` (to standard output without `-o`) |

```json
{
  "spdxVersion": "SPDX-2.3",
  "name": "fw.elf",
  "creationInfo": { "created": "2023-11-14T22:13:20Z", "creators": ["Tool: EmbCC-0.0.1-beta"] },
  "files": [
    { "fileName": "/src/motor.c", "checksums": [{"algorithm": "SHA256", "checksumValue": "..."}],
      "licenseInfoInFiles": ["MIT"] },
    ...
```

The paths in the bill are rewritten by the prefix maps, and its time is
`SOURCE_DATE_EPOCH`'s, so the bill of a reproducible build is itself
reproducible: two builds of the same tree in different directories give
the same image and the same bill, byte for byte.

A temporary object that one compile-and-link command makes and deletes is
not listed as a file of the image. Its source and headers are.
