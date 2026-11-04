# embflash — an image put into a target

`embflash` programs a firmware image into a target through the GDB remote
serial protocol. That one protocol reaches nearly every way of programming
a part: QEMU's gdbstub, OpenOCD, pyOCD, SEGGER's J-Link GDB server and the
Black Magic Probe all speak it. The server runs the part's flash algorithm.
`embflash` reads the image, erases and writes the flash, verifies it, and
can start the program. A part with an STK500 serial bootloader, such as an
Arduino Uno with Optiboot, needs no server: `--stk500` programs it
directly. Neither does an ESP32, through its ROM: `--esp`. This page is the
command reference.

## Synopsis

```text
embflash IMAGE --gdb TARGET [--base ADDR] [--verify] [--run]
               [--entry ADDR] [--vector-table ADDR] [--monitor CMD]...
               [--dry-run] [--timeout SECONDS] [-v]
embflash IMAGE --stk500 serial:DEV[@BAUD] [--base ADDR] [--verify]
               [--no-reset] [--timeout SECONDS]
embflash IMAGE --esp serial:DEV[@BAUD] [--base ADDR] [--verify]
               [--no-reset] [--timeout SECONDS]
```

`TARGET` is where the server listens:

| Target | Reaches |
|---|---|
| `HOST:PORT` | a TCP port: OpenOCD's `3333`, a J-Link GDB server's `2331`, pyOCD's `3333` |
| `unix:PATH` | a Unix-domain socket, such as QEMU's `-gdb chardev:` |
| `serial:DEV[@BAUD]` | a serial line at `BAUD` (default 115200), such as the Black Magic Probe's GDB port |

## Images

| Image | Read as |
|---|---|
| ELF | its stored bytes at their **load** addresses, as `embpack` packs them; the entry point comes from the header |
| `.hex`, `.ihex` | Intel HEX (record types 0 to 5) |
| `.srec`, `.s19`, `.s28`, `.s37`, `.mot` | Motorola S-records (S1–S3 data, S7–S9 entry) |
| anything else | raw bytes, placed at `--base ADDR` |

Every checksum is checked. A HEX file must end with its end-of-file
record, so a truncated download is refused. Two pieces that overlap are
refused, and pieces that touch are merged.

## How each byte gets there

`embflash` asks the server for its memory map (`qXfer:memory-map:read`):

- **Flash.** Every block the image touches is erased, in runs of
  blocks, with `vFlashErase`; blocks the image does not touch keep their
  contents. The bytes are written with `vFlashWrite` and committed with
  `vFlashDone`. The block size is the server's.
- **RAM, and anything the server does not map.** Written as memory with
  `X` (binary). If the server does not take `X`, `embflash` falls back to
  `M` (hex). QEMU's gdbstub has no memory map and writes its flash this
  way.

Packets are as large as the server's `PacketSize`. A NAKed packet is sent
again. No-ack mode is used when the server offers it.

- `--verify` reads every byte back with `m` and names the first one that
  differs.
- `--monitor CMD` sends `CMD` to the server first (`qRcmd`). For example,
  `--monitor "reset halt"` for OpenOCD. Repeat the flag for more commands,
  up to 16. The server's console output is copied to stderr.

## Starting the program

`--run` sets the registers and continues. The registers are found **by
name** in the server's target description (`qXfer:features:read`), so
`embflash` keeps no table of register numbers.

- **Cortex-M.** If the description has `org.gnu.gdb.arm.m-profile`, the
  core starts the way it does at reset:
  - `sp` is the vector table's first word;
  - `pc` is the reset handler, with its Thumb bit cleared;
  - the Thumb bit is set in `xpsr`.

  The vector table is at the image's lowest address, or at
  `--vector-table ADDR`.
- **Other cores.** `pc` is the ELF entry, or `--entry ADDR`. HEX and
  S-records carry an entry in their start records.

Without `--run`, `embflash` detaches (`D`) and leaves the target halted.

## A serial bootloader: --stk500

`--stk500 serial:DEV[@BAUD]` takes `--gdb`'s place for an AVR part with a
bootloader that speaks STK500 version 1. Optiboot, the Arduino Uno's, does.
The baud rate is 115200 when left out; older Arduino bootloaders take 57600.

1. **Reset.** DTR and RTS go down for 250 ms, then up: through the board's
   capacitor that resets the part into its bootloader. `--no-reset` leaves
   the lines alone, for a board reset by hand.
2. **Sync.** `embflash` sends the sync request until the bootloader answers,
   up to ten times within three seconds.
3. **The part.** The signature names it, and so its flash, page and
   bootloader sizes:

   | Part | Signature | Flash | Page | Bootloader |
   |---|---|---|---|---|
   | ATmega328P | `1E 95 0F` | 32 KiB | 128 B | 512 B |
   | ATmega328 | `1E 95 14` | 32 KiB | 128 B | 512 B |
   | ATmega328PB | `1E 95 16` | 32 KiB | 128 B | 512 B |

   Any other signature is refused. An image that reaches the bootloader
   (0x7E00 on these parts) is refused before anything is written.
4. **Pages.** Every page the image touches is written whole. The bytes of
   the page the image does not give are the erased `0xff`, and pages it
   does not touch keep their contents. `--verify` reads every written page
   back and names the first byte that differs.
5. **Start.** Leaving programming mode starts the program. `--run`,
   `--monitor`, `--entry` and `--vector-table` belong to `--gdb` and are
   refused here.

## The Espressif ROM: --esp

`--esp serial:DEV[@BAUD]` programs an ESP32 through the serial download
mode in its ROM, the protocol esptool speaks, at 115200 baud unless given.
The image's bytes go to flash at their addresses. A raw `.bin` goes at
`--base`: 0x1000 is where the ESP32's ROM boots from.

1. **Into the ROM.** A development board's auto-reset circuit ties DTR to
   IO0 and RTS to EN, each through an inverting transistor. `embflash`
   holds EN low, pulls IO0 low, and releases EN, as esptool's classic reset
   does: the chip starts in download mode. On a board without the circuit,
   hold BOOT while pressing EN and pass `--no-reset`.
2. **Sync.** The sync request is sent up to ten times. The ROM answers it
   several times over, and the extra answers are drained.
3. **The chip.** The chip-detect register (0x40001000) names it. The ESP32,
   `0x00f01d83`, is the one known, with 4 MiB of flash assumed. Any other
   value is refused, and so is an image beyond the flash.
4. **Writing.** The flash is attached and its parameters given. Then each
   piece of the image is erased (`FLASH_BEGIN`) and written in blocks of
   1024 bytes, each with its checksum. `--verify` asks the ROM for the MD5
   of each piece as written and compares it with the image's.
5. **Start.** `FLASH_END`, then an EN pulse with IO0 high, starts the
   program from flash. `--no-reset` leaves the chip in the ROM.

The ROM writes at 115200 baud, about 10 KiB/s. An image tool makes the
bootable image itself, with the header, segments and checksum the ROM loads;
`embflash` writes what it is given.

## Examples

```sh
# QEMU: a board started with no program, its gdbstub on a socket
qemu-system-arm -M lm3s6965evb -nographic -S \
    -chardev socket,path=g.sock,server=on,wait=off,id=g -gdb chardev:g &
embflash fw.elf --gdb unix:g.sock --verify --run

# OpenOCD with an ST-Link
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg &
embflash fw.hex --gdb localhost:3333 --monitor "reset halt" --verify

# a Black Magic Probe
embflash fw.elf --gdb serial:/dev/ttyACM0 --verify --run

# an Arduino Uno through Optiboot, on the ATmega16U2's USB serial port
embflash fw.hex --stk500 serial:/dev/cu.usbmodem1101 --verify

# an ESP32 development board, its image where the ROM boots from
embflash app.bin --esp serial:/dev/cu.usbserial-0001 --base 0x1000 --verify

# what would be written, with no target
embflash fw.bin --base 0x08000000 --dry-run
```

`--dry-run` prints the image's pieces and its entry. With `--gdb`, it
also prints the server's memory map, and writes nothing.

## Exit status

`embflash` exits with:

- `0` when the image was written, and verified if `--verify` was given.
- `2` when:
  - the image cannot be read;
  - an option is malformed;
  - the server refuses a request or does not answer within `--timeout`
    (10 s by default);
  - a byte reads back wrong.

Each failure names its reason. A server that answers an erase or a write
with an error is reported with that error.

## Limits

- `embflash` has been tested against QEMU and against a server that models
  a part's flash. It has **not yet** been tested against real probes. The
  STK500 path has been tested against a model of Optiboot, not yet against
  a board.
- `--stk500` knows the ATmega328 family only. STK500 version 2, which the
  Arduino Mega's bootloader speaks, is not implemented.
- `--esp` knows the ESP32 only, assumes 4 MiB of flash and writes at the
  ROM's 115200 baud. Faster rates (`CHANGE_BAUDRATE`), the flasher stub,
  compression and other chips are not implemented. It has been tested
  against a model of the ROM, not yet against a chip.
- Option bytes, mass erase and read-out protection are server-specific.
  Reach them with `--monitor`.
- The transports need POSIX sockets and termios. Elsewhere `embflash`
  builds, but it can only `--dry-run`.

## Checked by

`tests/golden/embflash.sh` runs against `tests/golden/embflash/fakegdb.py`.
That server models a part's flash and reports any rule a programmer breaks:

- an erase that is not whole, aligned blocks;
- a write to bytes that were not erased;
- `X` or `M` into flash;
- a packet over `PacketSize`;
- a raw `$` or `*` in binary data.

The checks:

- **Every format:** one firmware as ELF, HEX, S-records and raw binary
  (all from `embpack`) lands byte for byte. Exactly the blocks it touches
  are erased (`check.py`), and the memory map is read in both orders
  (flash first, as OpenOCD sends it, and RAM first, as pyOCD does).
- **Escapes:** every byte value survives the binary escapes, in
  `vFlashWrite` and in `X`, split over 256-byte packets.
- **Protocol:** NAKed packets are sent again, and no-ack mode is taken.
  Without a memory map the image is written as memory, with `X` or, when
  the server has no `X`, with `M`.
- **Run and verify:** `--run` sets `sp`, `pc` and `xpsr` from the vector
  table. `--verify` catches a byte the server reads back wrong.
- **Refusals:** a truncated HEX file, a bad checksum, overlapping
  records, and a raw binary without `--base` are each refused for their
  own reason.
- **QEMU:** a program put into QEMU's `lm3s6965evb`, started with no
  image, runs and prints what it should.

Each of these checks has been seen to fail against a mutant of
`embflash` (18 of them).

`tests/golden/embflash-stk500.sh` runs against
`tests/golden/embflash/fakeoptiboot.py`. That model of Optiboot runs on a
pseudo-terminal and reports a page that is not whole or aligned, a write
with no address loaded, and a command Optiboot does not know. The checks:

- a HEX image with pieces in three pages, one partial, lands byte for byte,
  and is verified;
- a raw binary at `--base 0` lands the same way;
- a bootloader that misses the first three syncs is still synced;
- an image over the bootloader, an unknown signature, a bootloader that
  never answers, a byte that reads back wrong, and `--run` are each
  refused with exit status 2.

`tests/golden/embflash-esp.sh` runs against
`tests/golden/embflash/fakeesprom.py`. That model of the ESP32 ROM runs on a
pseudo-terminal and reports:

- a block with a wrong checksum, out of sequence, or outside the erased
  range;
- an unescaped SLIP frame;
- a write before the flash is attached.

The checks:

- a binary holding every byte value lands byte for byte at 0x1000, and the
  ROM's MD5 matches;
- a HEX image in two pieces lands the same way;
- a ROM that misses three syncs is still synced;
- an unknown chip, a ROM that never answers, a wrong MD5 and an image beyond
  the flash are each refused with exit status 2.
