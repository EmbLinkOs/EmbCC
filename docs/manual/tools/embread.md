# embread — the EMBX image reader and verifier

This page is the reference for `embread`, which prints the contents of an
EMBX image and checks it against the rules an EmbLinkOS loader applies.
It is for people who produce EMBX images, with [`embld --embx`](embld.md)
or another tool, and want a producer error reported in full on the host
rather than as a refusal from the kernel. `embread` reads EMBX images
only; it does not read ELF files.

## NAME

`embread` — dump and verify an EMBX image

## SYNOPSIS

```text
embread [-q] FILE
```

## DESCRIPTION

`embread` reads `FILE` in the order the EmbLinkOS loader reads an EMBX
image, prints what it finds, and reports every rule the image breaks. The
header checksum is checked before anything else in the header is used: if
the file is shorter than a header, does not start with the EMBX magic
bytes, or has a header checksum that does not match, `embread` stops
there, because no offset in such a header can be trusted. Otherwise it
goes on to check every segment, the capability table, and the contract of
the image's binary type, and reports all the problems it finds.

Each problem is printed on standard error with the section of the EMBX
specification (version 2) that states the rule and the error code a
loader would return:

```text
embread: §6.10 ECHECKSUM: segment 0: payload CRC32C is cff3dfbf, table says 936a095e
embread: 1 problem found
```

Before reading the file, `embread` checks its own CRC32C implementation
against a known value and its EMBX structure sizes against the format. If
either check fails it prints
`embread: CRC32C selftest FAILED — this build would disagree with the kernel`
or `embread: EMBX structure sizes are wrong for this build` and exits
with status 2.

### What is checked

| Section, code | Rule |
|---|---|
| §6.1 `ETRUNC` | the file is at least as long as the 128-byte header (stops) |
| §6.2 `EMAGIC` | the file begins with the EMBX magic (stops) |
| §6.3 `ECHECKSUM` | the CRC32C of header bytes 0 to 123 equals `header_checksum` (stops) |
| §6.4 `EVERSION` | `version_major` is 1 and `header_size` is 128 |
| §6.5 `EFEATURE` | no `feature_incompat` bit is set (version 1 knows none) |
| §6.7 `EMACHINE` | `machine` is 1 (x86-64) |
| §6.7 `EABI` | `abi_version` is at most 1 |
| §6.8 `ETRUNC` | `image_size` equals the file size |
| §8 `EMALFORMED` | `segment_entry_size` is 64 and `capability_entry_size` is 16 |
| §5.4 `EMALFORMED` | `grantor` is 0 |
| §8 `EMALFORMED` | the reserved header fields are 0 |
| §8 `EMALFORMED` | a nonzero `capability_count` has a nonzero table offset |
| §3.5 `EMALFORMED` | the reserved `EMBX_F_PIE` flag is not set |
| §8 `EMALFORMED` | a `BOOT` image has its segment table at offset 128 |
| §8 `ETRUNC` | the segment table and every segment's payload lie inside the file |
| §8 `EMALFORMED` | each segment's `reserved0` is 0, its `paddr` is 0 outside a `BOOT` image, and `mem_size` is at least `file_size` |
| §8 `EMALFORMED` | each `LOAD` segment's alignment is a power of two of at least 4096, and its address is congruent to its file offset modulo the alignment |
| §8 `EMALFORMED` | no `LOAD` segment is both writable and executable |
| §8 `EMALFORMED` | in an `APP` image, no `LOAD` segment reaches address `0x0000800000000000` |
| §8 `EMALFORMED` | `LOAD` segments are sorted by address and do not overlap |
| §6.10 `ECHECKSUM` | each `LOAD` segment's payload matches its CRC32C |
| §8 `ETRUNC` | the capability table lies inside the file |
| §8 `EMALFORMED` | every capability id is known (1 to 9), its flags and reserved fields are 0, and the table is sorted with no duplicates |
| §4.3 `EMALFORMED` | a `FW` image has entry point 0 and declares no capabilities |
| §8 `EMALFORMED` | an `APP` image has a nonzero entry point that lies inside an executable `LOAD` segment |

Segments of a type other than `LOAD` get the common checks only, as a
loader ignores them.

### What is not checked

- The build ID. `embread` does not implement SHA-256; the value is
  printed and labelled `(displayed, NOT verified — no SHA-256 here)`.
- Whether the capabilities are granted. That depends on the process that
  starts the program; only the shape of the table is checked.

## OPTIONS

### `-q`

Print nothing unless a rule is broken. Problems and the final count are
still printed on standard error.

Any other argument that begins with `-` is refused with
`embread: unknown option 'ARG'`. A second file name is refused with
`embread: one file at a time`. Both exit with status 2. There is no
`--help`; run `embread` with no arguments for the usage line.

## OUTPUT

Without `-q`, `embread` prints the file name, the header, each segment
with its permissions and checksum result, and the capability table,
followed by `all guards passed` when there is no problem:

```text
app.embx
EMBX 1.0  APP (.embx)  machine 1  abi 1
  entry_point        0x400000
  image_size         8196 bytes
  flags              0
  feature_incompat   0
  feature_compat     0
  segments           2 at +128
  capabilities       2 at +256
  header_checksum    2ca1c51f  OK
  build_id           4b04ab57...92ae  (displayed, NOT verified — no SHA-256 here)

segments:
  [0] LOAD     r-x vaddr 0x400000 file +4096/8 mem 8 align 4096
      checksum 936a095e OK
  [1] LOAD     rw- vaddr 0x401000 file +8192/4 mem 404 align 4096
      checksum 5167f30d OK
      bss tail 400 bytes zero-filled at load

capabilities: 2 declared
  [0] 1 FILESYSTEM
  [1] 2 NETWORK

all guards passed
```

(The build ID is shortened here.) The `flags` line adds `(STRIPPED)` when
that flag is set, and `feature_compat` adds `(.embdbg sidecar)` when the
image declares a debug sidecar. Binary types are printed as `APP (.embx)`,
`DLL (.embdll)`, `MOD (.embmod)`, `DRV (.embdrv)`, `FW (.embfw)` and
`BOOT (.embboot)`.

## EXIT STATUS

| Status | Meaning |
|---|---|
| 0 | every check passed |
| 1 | at least one check failed |
| 2 | a usage error, a file that cannot be opened or read, or a failed self-test |

## ENVIRONMENT

`embread` reads no environment variables.

## EXAMPLES

Link an EMBX image and inspect it:

```sh
embld --embx --cap filesystem -o app.embx crt0.o app.o libc.a
embread app.embx
```

Use it as a check in a build script:

```sh
embread -q app.embx || exit 1
```

## SEE ALSO

[`embld`](embld.md), [Object formats](../../internals/object-formats.md)
