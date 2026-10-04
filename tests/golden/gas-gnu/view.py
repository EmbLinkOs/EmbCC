# view.py OBJ: the parts of an ARM object two assemblers must agree on, one
# record per line, independent of REL (clang) versus RELA (EmbCC): a
# relocated word is shown with its effective addend, whichever of the two
# places holds it.
import re, subprocess, sys

obj = sys.argv[1]

def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

out = []
for l in run("llvm-readelf", "-SW", obj).splitlines():
    m = re.match(r"\s+\[\s*\d+\]\s+(\S+)\s+(\S+)\s+\S+\s+\S+\s+(\S+)\s+\S+\s+(\S*)", l)
    if not m:
        continue
    name, typ, size, flags = m.groups()
    if typ in ("REL", "RELA", "SYMTAB", "STRTAB", "ARM_EXIDX", "ARM_ATTRIBUTES", "NULL") \
            or re.match(r"\.(ARM\.|note|comment)", name):
        continue
    if re.fullmatch(r"\d+", flags):
        flags = ""
    out.append("S %s %s %s %s" % (name, typ, size, flags))
out.sort()

sec = None
pending = None   # (index into out, value) of a data word awaiting its relocation
for l in run("llvm-objdump", "-dr", "--no-show-raw-insn", obj).splitlines():
    m = re.match(r"Disassembly of section (\S+):", l)
    if m:
        sec = m.group(1).rstrip(":")
        out.append("D " + sec)
        continue
    m = re.match(r"\s+([0-9a-f]+):\s+(R_ARM_\S+)\s+(\S+)", l)
    if m:
        off, typ, sym = m.groups()
        addend = 0
        sm = re.match(r"(.*?)([+-]0x[0-9a-f]+)$", sym)
        if sm:
            sym, addend = sm.group(1), int(sm.group(2), 16)
        if pending is not None and typ == "R_ARM_ABS32":
            i, word = pending
            out[i] = " .word <reloc>"
            addend += word           # REL: in the word; RELA: in the record
        if "ARM.exidx" not in (sec or "") and "unwind_cpp" not in sym:
            out.append("R %s %s %s %s%+d" % (sec, off, typ, sym, addend))
        pending = None
        continue
    m = re.match(r"\s+([0-9a-f]+):\s+(.*)", l)
    if m:
        text = re.sub(r"\s+@.*$", "", m.group(2))
        text = re.sub(r"\s+", " ", text).strip()
        wm = re.search(r"\.word\s+(0x[0-9a-f]+)", text)
        out.append(" " + text)
        pending = (len(out) - 1, int(wm.group(1), 16)) if wm else None
        continue
    m = re.match(r"[0-9a-f]+ <(.*)>:$", l)
    if m:
        out.append("L " + m.group(1))
        pending = None

syms = []
for l in run("llvm-nm", obj).splitlines():
    f = l.split()
    if re.search(r"\$[tdx]|__aeabi_unwind_cpp|^\.L", f[-1]):
        continue
    syms.append("Y %s %s %s" % (f[-1], f[-2], f[0] if len(f) == 3 else ""))
out += sorted(syms)
print("\n".join(out))
