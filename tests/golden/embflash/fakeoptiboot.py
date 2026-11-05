#!/usr/bin/env python3
"""A model of Optiboot, the Arduino Uno's STK500v1 bootloader, on a pseudo-terminal.

    fakeoptiboot.py PORTFILE DUMP [--sig 1e950f] [--silent] [--ignore-syncs N]
                    [--corrupt-read ADDR]

Writes the pseudo-terminal's device path to PORTFILE, answers what Optiboot
answers (sync, signature, enter/leave programming mode, load address, program
and read page) and, when programming mode is left, writes the 32 KiB flash to
DUMP, prints a line on the port as the program would, and exits 0; when the
programmer hangs up first it exits 3. Any rule a
programmer breaks is reported on stderr with exit status 1: a page that is not
whole or not page-aligned, a write with no address loaded, a command it does
not know. --silent never answers; --ignore-syncs
drops the first N sync requests, as a bootloader still starting does;
--corrupt-read flips a bit of the byte at ADDR when it is read back."""
import os
import pty
import select
import sys
import time

FLASH, PAGE = 32768, 128
INSYNC, OK, EOP = 0x14, 0x10, 0x20


def main(argv):
    portfile, dump = argv[0], argv[1]
    opts = argv[2:]

    def opt(name, default=None):
        return opts[opts.index(name) + 1] if name in opts else default

    sig = bytes.fromhex(opt("--sig", "1e950f"))
    silent = "--silent" in opts
    ignore = int(opt("--ignore-syncs", "0"))
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

    def need(n):
        nonlocal buf, heard
        while len(buf) < n:
            if time.monotonic() > deadline:
                sys.exit("fakeoptiboot: timeout")
            r, _, _ = select.select([master], [], [], 0.2)
            if not r:
                continue
            try:
                got = os.read(master, 4096)
            except OSError:  # no slave open: not yet (wait for it), or hung up
                if heard:
                    sys.exit(3)
                time.sleep(0.05)
                continue
            if not got:  # end of file: the programmer closed the port (macOS)
                if heard:
                    sys.exit(3)
                time.sleep(0.05)
                continue
            heard = True
            buf += got
        out = bytes(buf[:n])
        del buf[:n]
        return out

    def reply(body=b""):
        if not silent:
            os.write(master, bytes([INSYNC]) + body + bytes([OK]))

    def rule(msg):
        print(f"fakeoptiboot: {msg}", file=sys.stderr)
        sys.exit(1)

    while True:
        cmd = need(1)[0]
        if cmd == 0x30:
            if need(1)[0] != EOP:
                rule("sync without CRC_EOP")
            if ignore > 0:
                ignore -= 1
                continue
            reply()
        elif cmd == 0x75:
            need(1)
            reply(sig)
        elif cmd == 0x50:
            need(1)
            prog = True
            reply()
        elif cmd == 0x55:
            lo, hi, _ = need(3)
            addr = (lo | hi << 8) * 2
            reply()
        elif cmd == 0x64:
            sh, sl, kind = need(3)
            n = sh << 8 | sl
            data = need(n)
            if need(1)[0] != EOP:
                rule("page without CRC_EOP")
            if not prog:
                rule("a page written outside programming mode")
            if addr is None:
                rule("a page written with no address loaded")
            if kind != ord("F") or n != PAGE or addr % PAGE:
                rule(f"a page that is not one whole aligned flash page: {n} bytes at 0x{addr:x}")
            flash[addr:addr + n] = data
            reply()
        elif cmd == 0x74:
            sh, sl, kind, _ = need(4)
            n = sh << 8 | sl
            data = bytearray(flash[addr:addr + n])
            if addr <= corrupt < addr + n:
                data[corrupt - addr] ^= 0x01
            reply(bytes(data))
        elif cmd == 0x51:
            need(1)
            reply()
            with open(dump, "wb") as f:
                f.write(flash)
            os.write(master, b"hello from the sketch\r\n")
            time.sleep(0.2)
            return 0
        else:
            rule(f"unknown command 0x{cmd:02x}")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
