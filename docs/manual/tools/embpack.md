# embpack — a linked image as the file a programmer takes

`embpack` turns a linked ELF image into the file a flash programmer or
bootloader takes: a raw binary, Intel HEX, Motorola S-records, or UF2.
It can also stamp a CRC-32 into the image, append one, and write a JSON
manifest of what it packed. It reads images from EmbLD, GNU ld or lld:
32- or 64-bit, either byte order. This page is the command reference.

## Synopsis

```text
embpack IMAGE -o OUT [--format bin|hex|srec|uf2] [--fill BYTE]
                     [--pad-to SIZE] [--crc32 SYMBOL] [--append-crc32]
                     [--uf2-family ID] [--manifest FILE.json]
```

The format follows from the output's name:

| Extension | Format |
|---|---|
| `.hex`, `.ihex` | Intel HEX |
| `.srec`, `.s19`, `.s37`, `.mot` | S-records |
| `.uf2` | UF2 |
| anything else | raw binary |

`--format` overrides the extension. Sizes take a `K` or `M` suffix.

## What is packed

The image's **stored** bytes are packed: every allocated section with
contents, at its **load** address. That covers code, read-only data, and
the initial values of `.data` where the linker script stores them after
the code. `.bss` takes no space and is not packed. The packed bytes are
exactly what `llvm-objcopy` writes for the same image.

```sh
embld -T stm32.ld startup.o main.o -o fw.elf
embpack fw.elf -o fw.bin            # raw, from the lowest load address
embpack fw.elf -o fw.hex            # Intel HEX
embpack fw.elf -o fw.uf2 --uf2-family rp2040
```

## The formats

- **`bin`** — raw bytes from the lowest load address to the highest.
  - Gaps between sections are filled with `--fill`. The default is 0, as
    objcopy fills; 0xff is what erased flash holds.
  - `--pad-to SIZE` extends the file to a fixed size.
  - A raw image has no addresses of its own: the programmer is told where
    it goes, usually the start of flash. If the stored bytes span more
    than 1 GiB (flash and RAM far apart), it is refused; use a format with
    addresses instead.
- **`hex`** — Intel HEX:
  - data records of up to 16 bytes;
  - an extended linear address record (04) where the upper half of the
    address changes;
  - the start address (05) from the image's entry point;
  - the end record.

  Only stored bytes are written; gaps stay absent.
- **`srec`** — Motorola S-records:
  - an S0 header with the file's name;
  - S3 data records with 32-bit addresses;
  - an S7 record with the entry point.
- **`uf2`** — the USB Flashing Format used by the RP2040 and the many
  bootloaders that mount as a drive.
  - The file is 512-byte blocks, each carrying 256 bytes for one address.
  - Only 256-byte pages holding stored bytes are written, so the
    bootloader leaves the rest of flash alone.
  - `--uf2-family` sets the family ID, which a bootloader checks before
    flashing. It takes a number, or one of `rp2040`, `rp2350-arm-s`,
    `samd21`, `samd51`, `nrf52840`, `stm32f4`, `esp32s2` and `esp32s3`.

## Integrity

**`--crc32 SYMBOL`** writes a CRC-32 (zlib's: polynomial 0xEDB88320,
reflected, inverted) into the four bytes at `SYMBOL`. The CRC covers every
stored byte except those four, and is written in the image's byte order.
Firmware can then check itself at boot. Reserve the word as a `const` so it
is stored in flash:

```c
const unsigned image_crc __attribute__((used)) = 0;   /* embpack fills it */
```

At boot, compute the CRC of the image with those four bytes skipped and
compare it with `image_crc`. A symbol in `.bss` or `.data` is refused,
because the value would not be in the packed image.

**`--append-crc32`** (raw binary only) adds the CRC of the whole binary
after it, as four little-endian bytes.

**`--manifest FILE.json`** records the input, format, machine, byte order,
entry point, base address, size, fill, CRC-32 and SHA-256 of the packed
bytes, and every section packed with its load address. A release can
publish it beside the firmware.

## Exit status

0 when the file was written. 2 when the image cannot be read, an option is
malformed, or a request cannot be met. Each refusal names the reason: an
object file instead of a linked image, `--pad-to` smaller than the image,
a CRC symbol that is missing or not stored, or `--append-crc32` on a
format without room for it.

## Checked by

`tests/golden/embpack.sh` checks:

- **Raw binary:** byte-for-byte against `llvm-objcopy -O binary`, also
  with a 0xff fill.
- **HEX and S-records:** the same bytes at the same addresses as
  `llvm-objcopy` writes, with every checksum valid. This runs on an image
  at address 0 and on one at `0x80000000`.
- **Boot:** the raw binary boots on QEMU's Cortex-M3 and prints what the
  ELF prints.
- **UF2:** decoded block by block.
- **CRC and manifest:** the stamped CRC on little- and big-endian images,
  the appended CRC, and the manifest's SHA-256 and CRC-32, all compared
  with Python's `zlib` and `hashlib`.
- **Padding and refusals:** `--pad-to`, and each refusal above.
