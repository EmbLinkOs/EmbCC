"""check.py DUMP IMAGE REGION BLOCK: is what fakegdb.py dumped exactly
the image, programmed the way a part's flash is?

IMAGE says which bytes the image has: an Intel HEX file (as embpack writes
it), or FILE.bin@BASE for raw bytes, every one of them written, the gaps'
fill included. In the dumped REGION (flash or ram):
  - every image byte holds the image's value;
  - in flash, a byte outside the image but in a block the image touches
    reads 0xff (erased), and a byte in any other block still reads 0xa5,
    the old contents: nothing was erased that did not need to be;
  - in RAM, a byte outside the image is still 0.
Any VIOLATION line in the dump fails too. Prints what is wrong and exits
1, or exits 0 silently.
"""
import sys


def ihex(path):
    m, up = {}, 0
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        b = bytes.fromhex(line[1:])
        n, a, t = b[0], b[1] << 8 | b[2], b[3]
        if t == 0:
            for k in range(n):
                m[up + a + k] = b[4 + k]
        elif t == 4:
            up = (b[4] << 8 | b[5]) << 16
        elif t == 2:
            up = (b[4] << 8 | b[5]) << 4
    return m


def main():
    dump, hexf, region, block = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 0)
    if "@" in hexf:
        path, base = hexf.rsplit("@", 1)
        img = {int(base, 0) + k: v for k, v in enumerate(open(path, "rb").read())}
    else:
        img = ihex(hexf)
    bad = []
    mem = None
    for line in open(dump):
        w = line.split()
        if w[0] == "VIOLATION":
            bad.append(line.strip())
        elif w[0] == region:
            mem = (int(w[1], 16), bytes.fromhex(w[2]))
    if mem is None:
        sys.exit("check.py: no %s in %s" % (region, dump))
    start, data = mem
    touched = {(a - start) // block for a in img}
    shown = 0
    for k, v in enumerate(data):
        a = start + k
        if a in img:
            want, why = img[a], "the image's byte"
        elif region == "flash":
            want, why = (0xFF, "erased") if k // block in touched else (0xA5, "untouched")
        else:
            want, why = 0, "untouched"
        if v != want and shown < 5:
            bad.append("0x%x holds 0x%02x, not 0x%02x (%s)" % (a, v, want, why))
            shown += 1
    for a in img:
        if not start <= a < start + len(data):
            bad.append("the image's byte at 0x%x is outside the %s" % (a, region))
            break
    if bad:
        print("\n".join(bad))
        sys.exit(1)


main()
