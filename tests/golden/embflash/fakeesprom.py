#!/usr/bin/env python3
"""A model of the ESP32 ROM's serial download mode on a pseudo-terminal.

    fakeesprom.py PORTFILE DUMP [--magic 00f01d83] [--silent] [--ignore-syncs N]
                  [--corrupt-md5] [--deaf-above BAUD] [--fill] [--drop-read N]
                  [--corrupt-read ADDR] [--stub INC]

Writes the pseudo-terminal's device path to PORTFILE and answers what the
ESP32's ROM answers, in SLIP frames with its four status bytes: sync (several
times over, as the ROM does), the chip-detect register, SPI attach and
parameters, flash begin and data, the MD5 of a flash range, and the
RAM loader (MEM_BEGIN, MEM_DATA, MEM_END), READ_FLASH_SLOW (64 bytes an
answer, whatever was asked, as the ROM's), and CHANGE_BAUDRATE, after which
every command must come at the new rate: the pseudo-terminal's rate, which
the programmer sets, is checked against the ROM's on each one. FLASH_END is
answered with error
0x06, as an ESP32's ROM answers it, and so is a FLASH_BEGIN that does not
start on a 4 KiB sector. When the programmer hangs up after
writing, it writes the 4 MiB flash to DUMP and exits 0; on MEM_END it writes
what was loaded to DUMP, one
line per piece, `ADDRESS LENGTH SHA256`, and `entry ADDRESS`, then prints a
line on the port as the program would, and exits 0; when the programmer
hangs up first it exits 3. A rule a programmer breaks is reported on stderr
with exit status 1: a block whose checksum is wrong, out of sequence, or
outside the range FLASH_BEGIN erased, a write before the flash is attached,
a RAM block that is not its load's next or overruns it, a command at a rate
the ROM is not at, a CHANGE_BAUDRATE with the stub's old-rate word, or a
command the model does not know. --silent never answers; --deaf-above
answers nothing once the rate is above BAUD, as an adapter that cannot carry
it; --ignore-syncs
drops the first N syncs, as a chip still in reset does; --corrupt-md5 answers
a wrong digest. --fill starts the flash as byte i = (7 i + 3) mod 256 rather
than erased; --drop-read N loses the answer to the Nth READ_FLASH_SLOW (from
1), as the line can; --corrupt-read ADDR flips a bit of the byte at ADDR in
what READ_FLASH_SLOW answers, but not in the flash the MD5 is of. A
programmer that only read and hangs up ends it with 0.

--stub INC (tools/embflash/esp32stub.inc) models embflash's flasher stub:
a MEM_END to its entry, after RAM loads that are its pieces byte for byte,
starts it -- the greeting frame "EMBS" -- and from then on the stub's
commands are answered as esp32stub.c answers them: blocks up to 16 KiB,
LZ4-compressed blocks (FLASH_LZ_BEGIN/DATA, decoded here by a decoder of
the model's own), the MD5 as 16 bytes, READ_FLASH streamed and
acknowledged, ERASE_FLASH and ERASE_REGION."""
import hashlib
import os
import re
import pty
import select
import struct
import sys
import termios
import time

FLASH = 4 << 20


def lz4_decode(src, cap):
    """LZ4's block format (the model's own decoder): None for a bad block"""
    out, i = bytearray(), 0
    while i < len(src):
        t = src[i]; i += 1
        lit = t >> 4
        if lit == 15:
            while True:
                if i >= len(src):
                    return None
                c = src[i]; i += 1; lit += c
                if c != 255:
                    break
        if i + lit > len(src):
            return None
        out += src[i:i + lit]; i += lit
        if i >= len(src):
            break
        if i + 2 > len(src):
            return None
        off = src[i] | src[i + 1] << 8; i += 2
        if off == 0 or off > len(out):
            return None
        ml = t & 15
        if ml == 15:
            while True:
                if i >= len(src):
                    return None
                c = src[i]; i += 1; ml += c
                if c != 255:
                    break
        for _ in range(ml + 4):
            out.append(out[-off])
        if len(out) > cap:
            return None
    return bytes(out) if len(out) <= cap else None


def stub_inc(path):
    """the stub's entry and pieces, from esp32stub.inc"""
    inc = open(path).read()
    entry = int(re.search(r"ESP32STUB_ENTRY 0x([0-9a-f]+)u", inc).group(1), 16)
    pieces = {}
    for addr, n, k in re.findall(r"\{ 0x([0-9a-f]+)u, (\d+), g_esp32stub_(\d+) \}", inc):
        body = re.search(r"g_esp32stub_%s\[\d+\] = \{(.*?)\};" % k, inc, re.S).group(1)
        pieces[int(addr, 16)] = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", body))
    return entry, pieces


def main(argv):
    portfile, dump = argv[0], argv[1]
    opts = argv[2:]

    def opt(name, default=None):
        return opts[opts.index(name) + 1] if name in opts else default

    magic = int(opt("--magic", "00f01d83"), 16)
    silent = "--silent" in opts
    ignore = int(opt("--ignore-syncs", "0"))
    corrupt = "--corrupt-md5" in opts
    deaf = int(opt("--deaf-above", "0"))
    drop_read = int(opt("--drop-read", "0"))
    corrupt_read = int(opt("--corrupt-read", "-1"), 0)
    reads = 0
    stub_entry, stub_pieces = stub_inc(opt("--stub")) if opt("--stub") else (None, None)
    stub = False
    wr = None  # the stub's open write: [address, end, next sequence, lz]
    rate = 115200  # the ROM's, at reset
    codes = {getattr(termios, f"B{r}"): r for r in (9600, 19200, 38400, 57600, 115200, 230400,
                                                      460800, 921600) if hasattr(termios, f"B{r}")}
    master, slave = pty.openpty()
    path = os.ttyname(slave)
    os.close(slave)  # the programmer's open is the only one: its close is a hang-up
    with open(portfile, "w") as f:
        f.write(path)
    flash = bytearray(bytes((i * 7 + 3) & 0xFF for i in range(256)) * (FLASH // 256)
                      if "--fill" in opts else b"\xff" * FLASH)
    attached = False
    region = None  # (offset, size, blocks, block size, next sequence number)
    ram = []  # [address, size, blocks, block size, next sequence, bytes]
    buf = bytearray()
    heard = False
    deadline = time.monotonic() + 60

    def rule(msg):
        print(f"fakeesprom: {msg}", file=sys.stderr)
        sys.exit(1)

    wrote = False

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
                if heard and (wrote or reads):
                    with open(dump, "wb") as f:
                        f.write(flash)
                    sys.exit(0)
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

    def host_rate():  # the rate the programmer set on its end of the line
        sp = termios.tcgetattr(master)[5]
        return codes.get(sp, sp)

    def reply(cmd, value=0, data=b"", status=(0, 0)):
        if silent or (deaf and rate > deaf):
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
        if host_rate() != rate:
            rule(f"command 0x{cmd:02x} sent at {host_rate()} baud; the ROM is at {rate}")
        if stub:
            if cmd == 0x08:
                reply(cmd)
            elif cmd in (0x02, 0xE0):
                esize, nblk, bsize, off = struct.unpack("<IIII", data)
                if off % 0x1000 or bsize > 16384 or off + esize > FLASH:
                    reply(cmd, status=(1, 5))
                    continue
                lo, hi = off, (off + esize + 0xFFF) & ~0xFFF
                flash[lo:hi] = b"\xff" * (hi - lo)
                wr = [off, hi, 0, cmd == 0xE0]
                reply(cmd)
            elif cmd in (0x03, 0xE1):
                if wr is None or (cmd == 0xE1) != wr[3]:
                    rule(f"stub: 0x{cmd:02x} with no FLASH{'_LZ' if cmd == 0xE1 else ''}_BEGIN")
                n, seq, ulen, _ = struct.unpack_from("<IIII", data)
                block = data[16:]
                if n != len(block) or seq != wr[2]:
                    rule(f"stub: block {seq} of {len(block)} bytes ({n} said) where {wr[2]} was due")
                sm = 0xEF
                for c in block:
                    sm ^= c
                if sm != checksum & 0xFF:
                    reply(cmd, status=(1, 7))
                    continue
                if cmd == 0xE1:
                    out = lz4_decode(block, 16384)
                    if out is None or len(out) != ulen:
                        rule(f"stub: block {seq} does not decode to its {ulen} bytes")
                    block = out
                elif len(block) > 16384:
                    rule(f"stub: a block of {len(block)} bytes, more than 16 KiB")
                w = (len(block) + 3) & ~3
                if wr[0] + w > wr[1]:
                    rule(f"stub: a block past the erased region (0x{wr[0]:x} + {w})")
                flash[wr[0]:wr[0] + w] = block.ljust(w, b"\xff")
                wr[0] += w
                wr[2] += 1
                wrote = True
                reply(cmd)
            elif cmd == 0x13:
                addr, n, _, _ = struct.unpack("<IIII", data)
                dg = bytearray(hashlib.md5(bytes(flash[addr:addr + n])).digest())
                if corrupt:
                    dg[0] ^= 1
                reply(cmd, data=bytes(dg))
            elif cmd == 0xD0:
                flash[:] = b"\xff" * FLASH
                wrote = True
                reply(cmd)
            elif cmd == 0xD1:
                off, n = struct.unpack("<II", data)
                if off % 0x1000 or n % 0x1000:
                    reply(cmd, status=(1, 5))
                    continue
                flash[off:off + n] = b"\xff" * n
                wrote = True
                reply(cmd)
            elif cmd == 0xD2:
                off, n, pkt, inflight = struct.unpack("<IIII", data)
                reply(cmd)
                sent = acked = 0
                body = bytearray(flash[off:off + n])
                if off <= corrupt_read < off + n:
                    body[corrupt_read - off] ^= 1
                reads += 1
                while acked < n:
                    if sent < n and sent - acked < pkt * inflight:
                        k = min(pkt, n - sent)
                        frm = bytes(body[sent:sent + k])
                        os.write(master, b"\xc0" + frm.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc") + b"\xc0")
                        sent += k
                        continue
                    ack = frame()
                    if len(ack) != 4:
                        rule(f"stub: a READ_FLASH acknowledgement of {len(ack)} bytes")
                    acked = struct.unpack("<I", ack)[0]
                    if acked > sent:
                        rule(f"stub: {acked} acknowledged, {sent} sent")
                dg = hashlib.md5(bytes(flash[off:off + n])).digest()
                os.write(master, b"\xc0" + dg.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc") + b"\xc0")
            elif cmd == 0x04:
                wr = None
                reply(cmd)
            else:
                rule(f"stub: unknown command 0x{cmd:02x}")
            continue
        if cmd == 0x08:
            if data != b"\x07\x07\x12\x20" + b"\x55" * 32:
                rule("a sync with the wrong pattern")
            if ignore > 0:
                ignore -= 1
                continue
            for _ in range(8):
                reply(cmd)
        elif cmd == 0x0F:
            new, old = struct.unpack("<II", data)
            if old != 0:
                rule(f"CHANGE_BAUDRATE with old rate {old}: that word is the stub's; the ROM's is 0")
            reply(cmd)  # at the old rate; the new one from here on
            rate = new
            print(f"fakeesprom: now at {rate} baud", file=sys.stderr)
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
            if off % 0x1000:  # the chip's answer: it erases whole sectors only
                reply(cmd, status=(1, 6))
                continue
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
            wrote = True
            reply(cmd)
        elif cmd == 0x13:
            addr, n, _, _ = struct.unpack("<IIII", data)
            digest = hashlib.md5(bytes(flash[addr:addr + n])).hexdigest()
            if corrupt:
                digest = digest[:-1] + ("0" if digest[-1] != "0" else "1")
            reply(cmd, data=digest.encode())
        elif cmd == 0x0E:
            if not attached:
                rule("READ_FLASH_SLOW before SPI_ATTACH")
            addr, n = struct.unpack("<II", data)
            if n > 64 or addr + n > FLASH:
                rule(f"READ_FLASH_SLOW of {n} bytes at 0x{addr:x}: the ROM reads 64 at most")
            reads += 1
            if reads == drop_read:
                continue
            block = bytearray(flash[addr:addr + 64].ljust(64, b"\xff"))
            if addr <= corrupt_read < addr + n:
                block[corrupt_read - addr] ^= 0x01
            reply(cmd, data=bytes(block))
        elif cmd == 0x05:
            rsize, nblk, bsize, off = struct.unpack("<IIII", data)
            if nblk * bsize < rsize or rsize % 4:
                rule(f"MEM_BEGIN {rsize} bytes in {nblk} blocks of {bsize} at 0x{off:x}")
            ram.append([off, rsize, nblk, bsize, 0, bytearray()])
            reply(cmd)
        elif cmd == 0x07:
            if not ram:
                rule("MEM_DATA before MEM_BEGIN")
            n, seq, _, _ = struct.unpack_from("<IIII", data)
            block = data[16:]
            load = ram[-1]
            if seq != load[4] or seq >= load[2] or len(block) != n:
                rule(f"RAM block {seq} ({len(block)} bytes) where {load[4]} was due")
            sm = 0xEF
            for c in block:
                sm ^= c
            if sm != checksum & 0xFF:
                rule(f"RAM block {seq}: checksum 0x{checksum:x}, the bytes give 0x{sm:02x}")
            load[5] += block
            load[4] += 1
            if len(load[5]) > load[1]:
                rule(f"RAM load at 0x{load[0]:x} overruns its {load[1]} bytes")
            reply(cmd)
        elif cmd == 0x06:
            stay, entry = struct.unpack("<II", data)
            for load in ram:
                if len(load[5]) != load[1]:
                    rule(f"RAM load at 0x{load[0]:x}: {len(load[5])} of {load[1]} bytes")
            if stub_entry is not None and entry == stub_entry:
                loaded = {load[0]: bytes(load[5]) for load in ram}
                for a, b in stub_pieces.items():
                    pad = b.ljust((len(b) + 3) & ~3, b"\0")
                    if loaded.get(a) != pad:
                        rule(f"the stub's piece at 0x{a:x} was not loaded as esp32stub.inc has it")
                reply(cmd)
                os.write(master, b"\xc0EMBS\xc0")
                stub = True
                ram = []
                continue
            reply(cmd)
            with open(dump, "w") as f:
                for load in ram:
                    f.write(f"0x{load[0]:08x} {load[1]} {hashlib.sha256(bytes(load[5])).hexdigest()}\n")
                f.write(f"entry 0x{entry:08x} stay {stay}\n")
            try:  # a programmer with no console may have hung up already
                os.write(master, f"hello from RAM at 0x{entry:08x}\r\n".encode())
            except OSError:
                pass
            time.sleep(1.0)
            return 0
        elif cmd == 0x04:
            reply(cmd, status=(1, 6))  # the ROM's answer (it would leave the loader)
        else:
            rule(f"unknown command 0x{cmd:02x}")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
