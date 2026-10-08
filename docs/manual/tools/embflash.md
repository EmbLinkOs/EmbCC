# embflash — an image put into a target

`embflash` programs a firmware image into a target through the GDB remote
serial protocol. That one protocol reaches nearly every way of programming
a part: QEMU's gdbstub, OpenOCD, pyOCD, SEGGER's J-Link GDB server and the
Black Magic Probe all speak it. The server runs the part's flash algorithm.
`embflash` reads the image, erases and writes the flash, verifies it, and
can start the program. This page is the command reference.

## Synopsis

```text
embflash IMAGE --gdb TARGET [--base ADDR] [--verify] [--run]
               [--entry ADDR] [--vector-table ADDR] [--monitor CMD]...
               [--dry-run] [--timeout SECONDS] [-v]
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
  a part's flash. It has **not yet** been tested against real probes.
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
