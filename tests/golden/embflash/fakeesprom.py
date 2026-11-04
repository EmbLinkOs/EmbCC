#!/usr/bin/env python3
"""A model of the ESP32 ROM's serial download mode on a pseudo-terminal.

    fakeesprom.py PORTFILE DUMP [--magic 00f01d83] [--silent] [--ignore-syncs N]
                  [--corrupt-md5]

Writes the pseudo-terminal's device path to PORTFILE and answers what the
ESP32's ROM answers, in SLIP frames with its four status bytes: sync (several
times over, as the ROM does), the chip-detect register, SPI attach and
parameters, flash begin, data and end, and the MD5 of a flash range. On
FLASH_END it writes the 4 MiB flash to DUMP and exits 0; when the programmer
hangs up first it exits 3. A rule a programmer breaks is reported on stderr
with exit status 1: a block whose checksum is wrong, out of sequence, or
outside the range FLASH_BEGIN erased, a write before the flash is attached,
or a command the model does not know. --silent never answers; --ignore-syncs
drops the first N syncs, as a chip still in reset does; --corrupt-md5 answers
a wrong digest."""
import hashlib
import os
import pty
import select
import struct
import sys
import time

FLASH = 4 << 20


def main(argv):
    portfile, dump = argv[0], argv[1]
    opts = argv[2:]

    def opt(name, default=None):
        return opts[opts.index(name) + 1] if name in opts else default

    magic = int(opt("--magic", "00f01d83"), 16)
    silent = "--silent" in opts
    ignore = int(opt("--ignore-syncs", "0"))
    corrupt = "--corrupt-md5" in opts
    master, slave = pty.openpty()
    path = os.ttyname(slave)
    os.close(slave)  # the programmer's open is the only one: its close is a hang-up
    with open(portfile, "w") as f:
        f.write(path)
    flash = bytearray(b"\xff" * FLASH)
    attached = False
    region = None  # (offset, size, blocks, block size, next sequence number)
    buf = bytearray()
    heard = False
    deadline = time.monotonic() + 60

    def rule(msg):
        print(f"fakeesprom: {msg}", file=sys.stderr)
        sys.exit(1)

    def read_more():
        nonlocal heard
        while True:
            if time.monotonic() > deadline:
                sys.exit("fakeesprom: timeout")
            r, _, _ = select.select([master], [], [], 0.2)
            if not r:
                continue
            try:
                got = os.read(master, 65536)
            except OSError:
                got = b""
            if not got:
                if heard:
                    sys.exit(3)
                time.sleep(0.05)
                continue
            heard = True
            return got

    def frame():
        nonlocal buf
        while True:
            start = buf.find(b"\xc0")
            if start >= 0:
                end = buf.find(b"\xc0", start + 1)
                if end > start + 1:
                    raw = bytes(buf[start + 1:end])
                    del buf[:end]
                    out = bytearray()
                    i = 0
                    while i < len(raw):
                        if raw[i] == 0xDB:
                            nxt = raw[i + 1]
                            if nxt not in (0xDC, 0xDD):
                                rule(f"a broken SLIP escape 0xdb 0x{nxt:02x}")
                            out.append(0xC0 if nxt == 0xDC else 0xDB)
                            i += 2
                        else:
                            if raw[i] == 0xC0:
                                rule("an unescaped 0xc0 inside a frame")
                            out.append(raw[i])
                            i += 1
                    return bytes(out)
                if end == start + 1:
                    del buf[:end]
                    continue
            buf += read_more()

    def reply(cmd, value=0, data=b"", status=(0, 0)):
        if silent:
            return
        body = data + bytes([status[0], status[1], 0, 0])
        pkt = struct.pack("<BBHI", 1, cmd, len(body), value) + body
        esc = pkt.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc")
        os.write(master, b"\xc0" + esc + b"\xc0")

    while True:
        pkt = frame()
        if len(pkt) < 8 or pkt[0] != 0:
            rule(f"a request that is not one: {pkt[:8].hex()}")
        _, cmd, size, checksum = struct.unpack_from("<BBHI", pkt)
        data = pkt[8:]
        if len(data) != size:
            rule(f"command 0x{cmd:02x}: size {size}, {len(data)} bytes")
        if cmd == 0x08:
            if data != b"\x07\x07\x12\x20" + b"\x55" * 32:
                rule("a sync with the wrong pattern")
            if ignore > 0:
                ignore -= 1
                continue
            for _ in range(8):
                reply(cmd)
        elif cmd == 0x0A:
            (addr,) = struct.unpack("<I", data)
            reply(cmd, magic if addr == 0x40001000 else 0)
        elif cmd == 0x0D:
            if size != 8:
                rule(f"SPI_ATTACH with {size} bytes; the ESP32 ROM takes 8")
            attached = True
            reply(cmd)
        elif cmd == 0x0B:
            if size != 24:
                rule(f"SPI_SET_PARAMS with {size} bytes")
            reply(cmd)
        elif cmd == 0x02:
            if not attached:
                rule("FLASH_BEGIN before SPI_ATTACH")
            esize, nblk, bsize, off = struct.unpack("<IIII", data)
            if nblk * bsize < esize or off + esize > FLASH:
                rule(f"FLASH_BEGIN {esize} bytes in {nblk} blocks of {bsize} at 0x{off:x}")
            lo, hi = off & ~0xFFF, (off + esize + 0xFFF) & ~0xFFF  # whole sectors
            flash[lo:hi] = b"\xff" * (hi - lo)
            region = [off, esize, nblk, bsize, 0, lo, hi]
            reply(cmd)
        elif cmd == 0x03:
            if region is None:
                rule("FLASH_DATA before FLASH_BEGIN")
            n, seq, _, _ = struct.unpack_from("<IIII", data)
            block = data[16:]
            off, esize, nblk, bsize, want, lo, hi = region
            if n != bsize or len(block) != n:
                rule(f"a block of {n} bytes ({len(block)} sent), not {bsize}")
            if seq != want or seq >= nblk:
                rule(f"block {seq} where {want} was due")
            s = 0xEF
            for c in block:
                s ^= c
            if s != checksum & 0xFF:
                rule(f"block {seq}: checksum 0x{checksum:x}, the bytes give 0x{s:02x}")
            at = off + seq * bsize
            if at + n > hi:
                rule(f"block {seq} reaches 0x{at + n:x}, past the erased 0x{hi:x}")
            flash[at:at + n] = block
            region[4] += 1
            reply(cmd)
        elif cmd == 0x13:
            addr, n, _, _ = struct.unpack("<IIII", data)
            digest = hashlib.md5(bytes(flash[addr:addr + n])).hexdigest()
            if corrupt:
                digest = digest[:-1] + ("0" if digest[-1] != "0" else "1")
            reply(cmd, data=digest.encode())
        elif cmd == 0x04:
            reply(cmd)
            with open(dump, "wb") as f:
                f.write(flash)
            time.sleep(0.2)
            return 0
        else:
            rule(f"unknown command 0x{cmd:02x}")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
