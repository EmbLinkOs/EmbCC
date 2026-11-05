#!/usr/bin/env python3
"""A model of the Arduino Mega 2560's STK500v2 bootloader on a pseudo-terminal.

    fakestk500v2.py PORTFILE DUMP [--sig 1e9801] [--silent] [--ignore-msgs N]
                    [--corrupt-read ADDR]

Writes the pseudo-terminal's device path to PORTFILE and answers what the
bootloader answers, in Atmel's AVR068 messages (MESSAGE_START 0x1b, the
sequence number, a 16-bit size high byte first, TOKEN 0x0e, the body, the
xor of all of it; the answer echoing the number, its body the command and
a status): sign-on, enter and leave programming mode, the signature (as
CMD_READ_SIGNATURE_ISP), load address (a word address, high byte first, bit
31 the extended address's flag, which the bootloader drops), and program
and read a flash page. When programming mode is left it writes the 256 KiB
flash to DUMP, prints a line on the port as the program would, and exits
0; when the programmer hangs up first it exits 3. Any rule a programmer
breaks is reported on stderr with exit status 1: a broken checksum or
sequence number, a page that is not one whole aligned 256-byte page, a
write with no address loaded, outside programming mode, or into the
bootloader's 8 KiB, a command it does not know. --silent never answers;
--ignore-msgs drops the first N messages, as a bootloader still starting
does; --corrupt-read flips a bit of the byte at ADDR when it is read
back."""
import os
import pty
import select
import sys
import time

FLASH, PAGE, BOOT = 262144, 256, 8192


def main(argv):
    portfile, dump = argv[0], argv[1]
    opts = argv[2:]

    def opt(name, default=None):
        return opts[opts.index(name) + 1] if name in opts else default

    sig = bytes.fromhex(opt("--sig", "1e9801"))
    silent = "--silent" in opts
    ignore = int(opt("--ignore-msgs", "0"))
    corrupt = int(opt("--corrupt-read", "-1"), 0)
    master, slave = pty.openpty()
    path = os.ttyname(slave)
    os.close(slave)  # the programmer's open is the only one: its close is a hang-up
    with open(portfile, "w") as f:
        f.write(path)
    flash = bytearray(b"\xff" * FLASH)
    addr = None
    prog = False
    buf = bytearray()
    deadline = time.monotonic() + 30
    heard = False
    last_seq = None

    def need(n):
        nonlocal buf, heard
        while len(buf) < n:
            if time.monotonic() > deadline:
                sys.exit("fakestk500v2: timeout")
            r, _, _ = select.select([master], [], [], 0.2)
            if not r:
                continue
            try:
                got = os.read(master, 4096)
            except OSError:
                if heard:
                    sys.exit(3)
                time.sleep(0.05)
                continue
            if not got:
                if heard:
                    sys.exit(3)
                time.sleep(0.05)
                continue
            heard = True
            buf += got
        out = bytes(buf[:n])
        del buf[:n]
        return out

    def rule(msg):
        print(f"fakestk500v2: {msg}", file=sys.stderr)
        sys.exit(1)

    def answer(seq, body):
        if silent:
            return
        m = bytes([0x1B, seq, len(body) >> 8, len(body) & 0xFF, 0x0E]) + body
        x = 0
        for c in m:
            x ^= c
        os.write(master, m + bytes([x]))

    while True:
        while need(1)[0] != 0x1B:
            pass
        seq, hi, lo, tok = need(4)
        n = hi << 8 | lo
        body = need(n)
        ck = need(1)[0]
        x = 0x1B ^ seq ^ hi ^ lo ^ tok
        for c in body:
            x ^= c
        if tok != 0x0E or x != ck:
            rule(f"a message with a broken token or checksum (seq {seq})")
        if ignore > 0:
            ignore -= 1
            continue
        if last_seq is not None and body[0] != 0x01 and seq != (last_seq + 1) & 0xFF:
            rule(f"sequence number {seq} after {last_seq}")
        last_seq = seq
        cmd = body[0]
        if cmd == 0x01:                                  # CMD_SIGN_ON
            answer(seq, b"\x01\x00\x08AVRISP_2")
        elif cmd == 0x10:                                # CMD_ENTER_PROGMODE_ISP
            prog = True
            answer(seq, b"\x10\x00")
        elif cmd == 0x1B:                                # CMD_READ_SIGNATURE_ISP
            i = body[4] if len(body) >= 6 else 0
            answer(seq, bytes([0x1B, 0x00, sig[i % 3], 0x00]))
        elif cmd == 0x06:                                # CMD_LOAD_ADDRESS
            w = int.from_bytes(body[1:5], "big")
            if (w & 0x80000000) == 0 and (w & 0x7FFFFFFF) >= 0x10000:
                rule(f"word address 0x{w:x} past 64K words without bit 31")
            addr = (w & 0x7FFFFFFF) * 2
            answer(seq, b"\x06\x00")
        elif cmd == 0x13:                                # CMD_PROGRAM_FLASH_ISP
            k = body[1] << 8 | body[2]
            data = body[10:]
            if not prog:
                rule("a page written outside programming mode")
            if addr is None:
                rule("a page written with no address loaded")
            if k != PAGE or len(data) != k or addr % PAGE:
                rule(f"not one whole aligned page: {k} bytes ({len(data)} sent) at 0x{addr:x}")
            if addr + k > FLASH - BOOT:
                rule(f"a page at 0x{addr:x}, in the bootloader's 8 KiB")
            flash[addr:addr + k] = data
            addr += k
            answer(seq, b"\x13\x00")
        elif cmd == 0x14:                                # CMD_READ_FLASH_ISP
            k = body[1] << 8 | body[2]
            if addr is None:
                rule("a read with no address loaded")
            data = bytearray(flash[addr:addr + k])
            if addr <= corrupt < addr + k:
                data[corrupt - addr] ^= 0x01
            addr += k
            answer(seq, b"\x14\x00" + bytes(data) + b"\x00")
        elif cmd == 0x11:                                # CMD_LEAVE_PROGMODE_ISP
            answer(seq, b"\x11\x00")
            with open(dump, "wb") as f:
                f.write(flash)
            try:  # a programmer with no console may have hung up already
                os.write(master, b"hello from the sketch\r\n")
            except OSError:
                pass
            time.sleep(0.2)
            return 0
        else:
            rule(f"unknown command 0x{cmd:02x}")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
