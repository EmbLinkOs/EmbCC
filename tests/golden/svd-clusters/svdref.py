"""Where every register of an SVD file is, computed straight from the
CMSIS-SVD specification -- a second opinion on embsvd, sharing none of its
code.

    python3 svdref.py DEVICE.svd

prints one line per register element, sorted:

    PERIPHERAL ADDRESS SIZE NAME

ADDRESS in hex, SIZE in bits, NAME the register's name with its %s
replaced by the element's dimIndex name. Clusters add their
addressOffset (and dimIncrement per element) to the registers inside;
derivedFrom on registers, clusters and peripherals is followed.
"""
import sys
import xml.etree.ElementTree as ET


def num(s):
    s = s.strip()
    mult = 1
    if s[-1] in "kK":
        mult, s = 1 << 10, s[:-1]
    elif s[-1] in "mM":
        mult, s = 1 << 20, s[:-1]
    elif s[-1] in "gG":
        mult, s = 1 << 30, s[:-1]
    if s.startswith("#"):
        return int(s[1:], 2) * mult
    return int(s, 0) * mult


def text(e, tag):
    k = e.find(tag)
    return None if k is None else k.text.strip()


def plain(name):
    return name.replace("[%s]", "").replace("%s", "")


def indices(e, n):
    t = text(e, "dimIndex")
    if t is None:
        return [str(k) for k in range(n)]
    if "," in t:
        return [x.strip() for x in t.split(",")]
    a, b = t.split("-")
    if a.isdigit():
        return [str(k) for k in range(int(a), int(b) + 1)]
    return [chr(c) for c in range(ord(a), ord(b) + 1)]


def find(scope, tag, name):
    for k in scope:
        if k.tag == tag and (text(k, "name") == name or plain(text(k, "name")) == name):
            return k
    raise SystemExit("svdref: no %s %s" % (tag, name))


def derived(e, scope, periphs):
    """e, with derivedFrom's base underneath it"""
    df = e.get("derivedFrom")
    if df is None:
        return e
    parts = df.split(".")
    if len(parts) == 1:
        base = find(scope, e.tag, parts[0])
        base = derived(base, scope, periphs)
    else:
        p = periphs[parts[0]]
        while p.find("registers") is None:
            p = periphs[p.get("derivedFrom")]
        sc = p.find("registers")
        for c in parts[1:-1]:
            sc = derived(find(sc, "cluster", c), sc, periphs)
        base = derived(find(sc, e.tag, parts[-1]), sc, periphs)
    out = ET.Element(e.tag)
    mine = {k.tag for k in e}
    keepdim = "%s" in text(e, "name")
    for k in base:
        if k.tag in mine:
            continue
        if not keepdim and k.tag in ("dim", "dimIncrement", "dimIndex", "dimName"):
            continue
        out.append(k)
    for k in e:
        out.append(k)
    return out


def walk(scope, at, size, periphs, pname, out):
    for e in list(scope):
        if e.tag not in ("register", "cluster"):
            continue
        e = derived(e, scope, periphs)
        name = text(e, "name")
        off = at + num(text(e, "addressOffset"))
        sz = num(text(e, "size")) if e.find("size") is not None else size
        if e.find("dim") is not None:
            n = num(text(e, "dim"))
            inc = num(text(e, "dimIncrement"))
            elems = [(off + k * inc, name.replace("%s", ix))
                     for k, ix in enumerate(indices(e, n))]
        else:
            elems = [(off, name)]
        for a, nm in elems:
            if e.tag == "cluster":
                walk(e, a, sz, periphs, pname, out)
            else:
                out.append((pname, a, sz, nm))


def main():
    root = ET.parse(sys.argv[1]).getroot()
    dsize = num(text(root, "size")) if root.find("size") is not None else 32
    plist = root.find("peripherals").findall("peripheral")
    periphs = {text(p, "name"): p for p in plist}
    out = []
    for p in plist:
        names = [text(p, "name")]
        bases = [num(text(p, "baseAddress"))]
        if p.find("dim") is not None:
            n = num(text(p, "dim"))
            inc = num(text(p, "dimIncrement"))
            names = [names[0].replace("%s", ix) for ix in indices(p, n)]
            bases = [bases[0] + k * inc for k in range(n)]
        lay = p
        while lay.find("registers") is None and lay.get("derivedFrom"):
            lay = periphs[lay.get("derivedFrom")]
        if lay.find("registers") is None:
            continue
        size = num(text(lay, "size")) if lay.find("size") is not None else dsize
        for nm, b in zip(names, bases):
            walk(lay.find("registers"), b, size, periphs, nm, out)
    for pname, a, sz, nm in sorted(out):
        print("%s 0x%08x %d %s" % (pname, a, sz, nm))


main()
