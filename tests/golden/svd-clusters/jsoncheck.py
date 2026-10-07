"""Check embsvd's --json output against its schema, and turn it into what
the other two witnesses can be compared with.

    python3 jsoncheck.py OUT.json HEADER.h REGS.txt ASSERTS.c

REGS.txt gets one line per register, in svdref.py's format, for a diff
against the SVD itself. ASSERTS.c gets, for every register of every
peripheral the header defines, a _Static_assert that the generated
header puts it at the JSON's address, with the JSON's size:

    BASE + offsetof(TYPE, PATH) == ADDRESS, sizeof(TYPE.PATH) == SIZE/8

so compiling ASSERTS.c checks the JSON against the header's layout.
"""
import json
import re
import sys

ACCESS = {"read-only", "write-only", "read-write", "writeOnce", "read-writeOnce"}


def fail(msg):
    raise SystemExit("jsoncheck: " + msg)


def need(obj, key, types, where):
    if key not in obj:
        fail("%s has no %r" % (where, key))
    v = obj[key]
    t = types if isinstance(types, tuple) else (types,)
    if not isinstance(v, t) or (isinstance(v, bool) and bool not in t):
        fail("%s: %r is %r" % (where, key, v))
    return v


S = (str, type(None))


def main():
    jpath, hdr, regs_out, c_out = sys.argv[1:5]
    with open(jpath) as f:
        d = json.load(f)
    if d.get("schema") != 1:
        fail("schema is %r, not 1" % d.get("schema"))
    need(d, "generator", str, "the document")
    dev = need(d, "device", dict, "the document")
    for k in ("name",):
        need(dev, k, str, "device")
    for k in ("vendor", "version", "description", "headerDefinitionsPrefix"):
        need(dev, k, S, "device")
    need(dev, "addressUnitBits", int, "device")
    need(dev, "width", int, "device")
    cpu = d.get("cpu", "missing")
    if cpu == "missing":
        fail("the document has no 'cpu'")
    if cpu is not None:
        for k in ("name", "revision", "endian"):
            need(cpu, k, S, "cpu")
        for k in ("mpuPresent", "fpuPresent", "fpuDP", "vendorSystickConfig"):
            need(cpu, k, bool, "cpu")
        need(cpu, "nvicPrioBits", int, "cpu")
        need(cpu, "deviceNumInterrupts", (int, type(None)), "cpu")
    for m in need(d, "memories", list, "the document"):
        need(m, "name", str, "memory")
        need(m, "origin", int, "memory")
        need(m, "length", int, "memory")
        need(m, "access", str, "memory")
    irqs = need(d, "interrupts", list, "the document")
    for q in irqs:
        need(q, "name", str, "interrupt")
        need(q, "value", int, "interrupt")
        need(q, "description", S, "interrupt")
    if [q["value"] for q in irqs] != sorted(q["value"] for q in irqs):
        fail("the interrupts are not in order")
    prefix = dev["headerDefinitionsPrefix"] or ""
    lines, asserts = [], []
    names = {}
    for p in need(d, "peripherals", list, "the document"):
        pn = need(p, "name", str, "peripheral")
        w = "peripheral " + pn
        names[pn] = p
        for k in ("description", "groupName", "derivedFrom"):
            need(p, k, S, w)
        base = need(p, "baseAddress", int, w)
        tname = need(p, "typeName", str, w)
        for b in need(p, "addressBlocks", list, w):
            need(b, "offset", int, w + " addressBlock")
            need(b, "size", int, w + " addressBlock")
            need(b, "usage", S, w + " addressBlock")
            if need(b, "address", int, w + " addressBlock") != base + b["offset"]:
                fail(w + ": an addressBlock's address is not base + offset")
        for q in need(p, "interrupts", list, w):
            need(q, "name", str, w + " interrupt")
            need(q, "value", int, w + " interrupt")
        bmac = prefix + re.sub(r"[^A-Za-z0-9]", "_", pn) + "_BASE"
        for r in need(p, "registers", list, w):
            rn = need(r, "name", str, w + " register")
            wr = "%s.%s" % (pn, rn)
            path = need(r, "path", str, wr)
            idx = need(r, "index", list, wr)
            if not all(isinstance(i, int) and not isinstance(i, bool) for i in idx):
                fail(wr + ": index is not a list of integers")
            addr = need(r, "address", int, wr)
            off = need(r, "offset", int, wr)
            size = need(r, "size", int, wr)
            if addr != base + off:
                fail(wr + ": address is not baseAddress + offset")
            if size not in (8, 16, 32, 64):
                fail(wr + ": size %d" % size)
            if need(r, "access", str, wr) not in ACCESS:
                fail(wr + ": access " + r["access"])
            need(r, "resetValue", int, wr)
            need(r, "resetMask", int, wr)
            need(r, "alternate", S, wr)
            need(r, "description", S, wr)
            for fd in need(r, "fields", list, wr):
                wf = wr + "." + need(fd, "name", str, wr + " field")
                lsb = need(fd, "bitOffset", int, wf)
                width = need(fd, "bitWidth", int, wf)
                if width < 1 or lsb + width > size:
                    fail(wf + ": outside the register")
                if need(fd, "access", str, wf) not in ACCESS:
                    fail(wf + ": access " + fd["access"])
                need(fd, "description", S, wf)
                for e in need(fd, "enumeratedValues", list, wf):
                    need(e, "name", str, wf + " enumeratedValue")
                    need(e, "value", (int, type(None)), wf + " enumeratedValue")
                    need(e, "isDefault", bool, wf + " enumeratedValue")
                    need(e, "usage", str, wf + " enumeratedValue")
                    need(e, "description", S, wf + " enumeratedValue")
            lines.append("%s 0x%08x %d %s" % (pn, addr, size, rn))
            asserts.append(
                "_Static_assert(%s + offsetof(%s, %s) == 0x%xULL, \"%s\");\n"
                "_Static_assert(sizeof(((%s *)0)->%s) == %d, \"%s size\");\n"
                % (bmac, tname, path, addr, wr, tname, path, size // 8, wr))
    for p in d["peripherals"]:
        if p["derivedFrom"] is not None and p["derivedFrom"] not in names:
            fail("%s is derivedFrom %s, which is not listed" % (p["name"], p["derivedFrom"]))
    with open(regs_out, "w") as f:
        f.write("".join(l + "\n" for l in sorted(lines)))
    with open(c_out, "w") as f:
        f.write("/* from %s: every register where the JSON says */\n" % jpath)
        f.write("#include <stddef.h>\n#include \"%s\"\n" % hdr)
        f.write("".join(asserts))
    print("%d peripherals, %d registers" % (len(d["peripherals"]), len(lines)))


main()
