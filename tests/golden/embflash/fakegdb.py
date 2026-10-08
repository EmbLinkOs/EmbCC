"""A GDB server that models a part's flash, for tests/golden/embflash.sh.

    fakegdb.py SOCKET DUMP [--flash START,LEN,BLOCK] [--ram START,LEN]
               [--no-x] [--no-map] [--nak-every N] [--packet-size N]
               [--noack] [--m-profile] [--corrupt ADDR] [--ram-first]

Listens on the Unix socket SOCKET for one client, and speaks enough of the
GDB remote protocol for a programmer: qSupported, the memory map
(qXfer:memory-map:read), vFlashErase / vFlashWrite / vFlashDone, X and M
writes, m reads, qRcmd, P, c and D.

The flash keeps the rules a real one does, and a programmer that breaks
one is reported, not forgiven:
  - an erase is of whole blocks, aligned;
  - a vFlashWrite lands only in bytes erased since the last commit, and
    only inside flash;
  - X/M into flash is refused (E01): flash is not memory;
  - nothing written is visible until vFlashDone.

When the client goes, DUMP is written: one line per region, the bytes in
hex, and a line per violation. The exit status is 1 if there was any.
--no-x answers X with an empty reply (as QEMU does); --no-map advertises no
memory map; --nak-every N NAKs every Nth packet once, so the client must
send it again; --noack offers QStartNoAckMode; --m-profile describes a
Cortex-M's registers (target.xml), and P writes to them are dumped;
--corrupt ADDR makes an m read of ADDR answer a wrong byte, as a cell that
did not take its value would. The flash starts out holding 0xa5 everywhere
(the firmware before this one) and the RAM 0. --ram-first lists the RAM
before the flash in the memory map (pyOCD's order; OpenOCD's is the other).
"""
import os
import socket
import sys


def parse_args(argv):
    o = {"flash": None, "ram": None, "no_x": False, "no_map": False,
         "nak": 0, "psize": 0x1000, "noack": False, "mprof": False,
         "corrupt": None, "ramfirst": False}
    sock, dump = argv[0], argv[1]
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == "--flash":
            s, l, b = (int(x, 0) for x in argv[i + 1].split(","))
            o["flash"] = (s, l, b); i += 2
        elif a == "--ram":
            s, l = (int(x, 0) for x in argv[i + 1].split(","))
            o["ram"] = (s, l); i += 2
        elif a == "--no-x":
            o["no_x"] = True; i += 1
        elif a == "--no-map":
            o["no_map"] = True; i += 1
        elif a == "--nak-every":
            o["nak"] = int(argv[i + 1]); i += 2
        elif a == "--noack":
            o["noack"] = True; i += 1
        elif a == "--m-profile":
            o["mprof"] = True; i += 1
        elif a == "--ram-first":
            o["ramfirst"] = True; i += 1
        elif a == "--corrupt":
            o["corrupt"] = int(argv[i + 1], 0); i += 2
        elif a == "--packet-size":
            o["psize"] = int(argv[i + 1], 0); i += 2
        else:
            sys.exit("fakegdb: unknown option " + a)
    return sock, dump, o


def main():
    sock_path, dump, o = parse_args(sys.argv[1:])
    fs, fl, fb = o["flash"] or (0, 0, 1)
    rs, rl = o["ram"] or (0, 0)
    flash = bytearray(b"\xa5" * fl)         # committed: the old firmware
    pending = {}                             # address -> byte, before Done
    erased = bytearray(fl)                   # 1 where erased since the commit
    ram = bytearray(rl)
    bad = []
    regs = {}

    def in_flash(a, n=1):
        return fl and fs <= a and a + n <= fs + fl

    def in_ram(a, n=1):
        return rl and rs <= a and a + n <= rs + rl

    def read(a):
        if in_flash(a):
            return pending.get(a, flash[a - fs])
        if in_ram(a):
            return ram[a - rs]
        return None

    if os.path.exists(sock_path):
        os.unlink(sock_path)
    srv = socket.socket(socket.AF_UNIX)
    srv.bind(sock_path)
    srv.listen(1)
    srv.settimeout(30)
    c, _ = srv.accept()
    c.settimeout(30)
    buf = b""
    noack = False
    count = 0

    def get_packet():
        nonlocal buf
        while True:
            while b"$" not in buf or b"#" not in buf[buf.index(b"$"):] or \
                    len(buf) < buf.index(b"#", buf.index(b"$")) + 3:
                d = c.recv(65536)
                if not d:
                    return None
                buf += d
            st = buf.index(b"$")
            h = buf.index(b"#", st)
            body, cs = buf[st + 1:h], buf[h + 1:h + 3]
            buf = buf[h + 3:]
            return body, cs

    def send(body):
        b = body.encode("latin-1") if isinstance(body, str) else body
        c.sendall(b"$" + b + b"#%02x" % (sum(b) & 0xff))

    def unescape(b):
        # $ # } * travel escaped; a raw one is a client bug (a '#' would
        # have ended the packet: its checksum fails instead)
        for ch in b"$*":
            if ch in b:
                bad.append("a raw %r in binary data" % chr(ch))
                break
        out, i = bytearray(), 0
        while i < len(b):
            if b[i] == 0x7D:
                out.append(b[i + 1] ^ 0x20); i += 2
            else:
                out.append(b[i]); i += 1
        return bytes(out)

    while True:
        p = get_packet()
        if p is None:
            break
        body, cs = p
        count += 1
        if len(body) + 4 > o["psize"]:
            bad.append("a %d-byte packet; PacketSize is %d" % (len(body) + 4, o["psize"]))
        if not noack:
            if o["nak"] and count % o["nak"] == 0:
                c.sendall(b"-")              # resent; counted again
                continue
            if (sum(body) & 0xff) != int(cs, 16):
                bad.append("a packet with a bad checksum")
                c.sendall(b"-")
                continue
            c.sendall(b"+")
        cmd = body
        s = cmd.decode("latin-1")
        if s.startswith("qSupported"):
            feats = "PacketSize=%x" % o["psize"]
            if not o["no_map"] and o["flash"]:
                feats += ";qXfer:memory-map:read+"
            if o["mprof"]:
                feats += ";qXfer:features:read+"
            if o["noack"]:
                feats += ";QStartNoAckMode+"
            send(feats)
        elif s.startswith("qXfer:memory-map:read::"):
            off, ln = (int(x, 16) for x in s.split("::")[1].split(","))
            fx = ('<memory type="flash" start="0x%x" length="0x%x">\n'
                  '  <property name="blocksize">0x%x</property>\n</memory>\n'
                  % (fs, fl, fb)) if o["flash"] else ""
            rx = ('<memory type="ram" start="0x%x" length="0x%x"/>\n'
                  % (rs, rl)) if o["ram"] else ""
            xml = ('<?xml version="1.0"?>\n<memory-map>\n' +
                   (rx + fx if o["ramfirst"] else fx + rx) + "</memory-map>\n")
            chunk = xml[off:off + ln]
            send(("l" if off + ln >= len(xml) else "m") + chunk)
        elif s.startswith("qXfer:features:read:") and o["mprof"]:
            annex, rng = s[len("qXfer:features:read:"):].split(":")
            off, ln = (int(x, 16) for x in rng.split(","))
            if annex == "target.xml":
                xml = ('<?xml version="1.0"?><target><architecture>arm</architecture>'
                       '<xi:include href="core.xml"/></target>')
            elif annex == "core.xml":
                xml = '<feature name="org.gnu.gdb.arm.m-profile">'
                xml += "".join('<reg name="r%d" bitsize="32"/>' % k for k in range(13))
                xml += ('<reg name="sp" bitsize="32" type="data_ptr"/>'
                        '<reg name="lr" bitsize="32"/>'
                        '<reg name="pc" bitsize="32" type="code_ptr"/>'
                        '<reg name="xpsr" bitsize="32" regnum="25"/></feature>')
            else:
                send("E00"); continue
            chunk = xml[off:off + ln]
            send(("l" if off + ln >= len(xml) else "m") + chunk)
        elif s.startswith("qXfer:"):
            send("")
        elif s.startswith("vFlashErase:"):
            a, n = (int(x, 16) for x in s.split(":")[1].split(","))
            if (a - fs) % fb or n % fb or not in_flash(a, n):
                bad.append("an erase of 0x%x,+0x%x that is not whole blocks of flash" % (a, n))
                send("E01")
                continue
            for k in range(a, a + n):
                erased[k - fs] = 1
                pending[k] = 0xFF
            send("OK")
        elif cmd.startswith(b"vFlashWrite:"):
            rest = cmd[len(b"vFlashWrite:"):]
            colon = rest.index(b":")
            a = int(rest[:colon], 16)
            data = unescape(rest[colon + 1:])
            if not in_flash(a, len(data)):
                bad.append("a flash write outside flash at 0x%x" % a)
                send("E01")
                continue
            for k, v in enumerate(data):
                if not erased[a + k - fs] or pending.get(a + k) != 0xFF:
                    bad.append("a flash write to 0x%x, which was not erased" % (a + k))
                    break
                pending[a + k] = v
            send("OK")
        elif s == "vFlashDone":
            for a, v in pending.items():
                flash[a - fs] = v
            pending.clear()
            erased = bytearray(fl)
            send("OK")
        elif cmd.startswith(b"X"):
            if o["no_x"]:
                send("")
                continue
            hdr, data = cmd[1:].split(b":", 1)
            a, n = (int(x, 16) for x in hdr.decode().split(","))
            data = unescape(data)
            if len(data) != n:
                bad.append("an X packet whose length is not its count")
                send("E02"); continue
            if n and in_flash(a, 1):
                send("E01"); continue
            if n and not in_ram(a, n):
                bad.append("an X write outside memory at 0x%x" % a)
                send("E01"); continue
            ram[a - rs:a - rs + n] = data
            send("OK")
        elif s.startswith("M"):
            hdr, data = s[1:].split(":", 1)
            a, n = (int(x, 16) for x in hdr.split(","))
            if in_flash(a, 1):
                send("E01"); continue
            if not in_ram(a, n):
                bad.append("an M write outside memory at 0x%x" % a)
                send("E01"); continue
            ram[a - rs:a - rs + n] = bytes.fromhex(data)
            send("OK")
        elif s.startswith("m"):
            a, n = (int(x, 16) for x in s[1:].split(","))
            vals = [read(a + k) for k in range(n)]
            if o["corrupt"] is not None and a <= o["corrupt"] < a + n and \
                    vals[o["corrupt"] - a] is not None:
                vals[o["corrupt"] - a] ^= 0x10
            if any(v is None for v in vals):
                send("E01")
            else:
                send("".join("%02x" % v for v in vals))
        elif s.startswith("QStartNoAckMode"):
            send("OK")
            noack = True
        elif s.startswith("qRcmd,"):
            text = bytes.fromhex(s[6:]).decode()
            send("O" + ("monitor: %s\n" % text).encode().hex())
            send("OK")
        elif s.startswith("P"):
            r, v = s[1:].split("=")
            regs[int(r, 16)] = v
            send("OK")
        elif s == "c":
            regs["c"] = "continued"         # no reply: the target runs
            break
        elif s == "D":
            send("OK")
            break
        else:
            send("")
    with open(dump, "w") as f:
        if o["flash"]:
            f.write("flash %x %s\n" % (fs, flash.hex()))
        if o["ram"]:
            f.write("ram %x %s\n" % (rs, ram.hex()))
        for r, v in sorted(regs.items(), key=str):
            f.write("reg %s %s\n" % (r, v))
        for b in bad:
            f.write("VIOLATION %s\n" % b)
    sys.exit(1 if bad else 0)


main()
