#!/usr/bin/env python3
"""link-audit: does embld link what a reference toolchain's objects need,
to the bytes the reference linker writes?

For each target, EmbCC's C library and compiler runtime (lib/libc, lib/rt)
are compiled with clang into objects -- every relocation clang's code
generator and integrated assembler write for that machine -- a stub defines
whatever they leave undefined, and the whole set is linked twice, by embld
and by ld.lld, under one linker script. The two images are compared:

  - every symbol at the same address (the layout), then
  - the bytes inside every sized symbol (the relocations); the gaps
    between symbols are padding, which each linker fills its own way.

  tools/link-audit/audit.py [TARGET...]      (default: all)
  LINK_AUDIT_OUT=DIR keeps the work (default: a temporary directory)

Where LLVM has no linker for the machine (Xtensa, RX, ColdFire, AVR),
GCC compiles the corpus and GNU ld is the referee.

A target prints `N symbols, M placed differently, K differ in bytes`, the
first few differences by symbol, and what embld refused if it refused."""
import collections, glob, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))

# name: (clang flags, load address)
T = {
  "x86":  ("--target=x86_64-elf", 0x400000),
  "a64":  ("--target=aarch64-none-elf", 0x40080000),
  "m4":   ("--target=thumbv7em-none-eabi -mcpu=cortex-m4 -mfloat-abi=soft", 0x08000000),
  "m0":   ("--target=thumbv6m-none-eabi -mcpu=cortex-m0", 0x08000000),
  "m33":  ("--target=thumbv8m.main-none-eabi -mcpu=cortex-m33 -mfloat-abi=soft", 0x08000000),
  "a7":   ("--target=armv7a-none-eabi -marm -mfloat-abi=soft", 0x80000000),
  "rv32": ("--target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32 -mno-relax", 0x80000000),
  "rv64": ("--target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d -mno-relax -mcmodel=medany", 0x80000000),
  "mips": ("--target=mipsel-none-elf -mips32r2 -msoft-float -mno-abicalls -G0 -fno-pic", 0x80000000),
  "la64": ("--target=loongarch64-unknown-elf -mno-lsx -mno-lasx -mabi=lp64s -mno-relax", 0x80000000),
  "ppc":  ("--target=powerpc-none-eabi -msoft-float", 0x00100000),
}

# Where LLVM has no linker for the machine: GCC compiles the corpus and
# GNU ld is the referee. name: (compiler, flags, linker, linker flags,
# load address)
HOME = os.path.expanduser("~")
G = {
  "xtensa": (HOME + "/cross/xtensa/xtensa-esp-elf/bin/xtensa-esp32-elf-gcc",
             "-mlongcalls -mtext-section-literals",
             HOME + "/cross/xtensa/xtensa-esp-elf/bin/xtensa-esp32-elf-ld",
             "--no-relax", 0x40080000),
  "rx":     (HOME + "/cross/rx-elf/bin/rx-elf-gcc", "", HOME + "/cross/rx-elf/bin/rx-elf-ld",
             "", 0x00000000),
  "m68k":   ("m68k-elf-gcc", "-mcpu=5208", "m68k-elf-ld", "", 0x00000000),
  "avr":    ("avr-gcc", "-mmcu=atmega2560", "avr-ld", "-m avr6", 0x00000000),
}

# AVR: code from 0, data in its own address space (0x800000 up) loaded
# after the code, as avr-libc's scripts lay it out
AVR_SCRIPT = """ENTRY(audit_entry)
SECTIONS
{
    .text 0 : { *(.vectors) *(.trampolines*) *(.text.audit_entry) *(.progmem*) *(.text .text.*) }
    .data 0x800200 : AT (ADDR(.text) + SIZEOF(.text))
        { *(.rodata .rodata.*) *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.debug*) }
}
"""

SCRIPT = """ENTRY(audit_entry)
SECTIONS
{
    . = 0x%x;
    .text : { *(.text.audit_entry) *(.text .text.*) *(.literal .literal.*)
              *(P P.*) }
    .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) *(C C.* C_1 C_1.* C_2 C_2.*)
                *(W W.* W_1 W_1.* W_2 W_2.*) }
    .init_array : { KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array)) }
    .fini_array : { KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*))) KEEP(*(.fini_array)) }
    .data : { *(.data .data.*) *(.sdata .sdata.*) *(D D.* D_1 D_1.* D_2 D_2.*) }
    .bss : { *(.bss .bss.*) *(.sbss .sbss.*) *(B B.* B_1 B_1.* B_2 B_2.*) *(COMMON) }
    /* lld keeps a GOT where the objects ask for one and are not marked
     * for relaxation; embld resolves those loads directly. Last, so the
     * GOT moves nothing else. */
    .got : { *(.got) *(.got.plt) }
    /DISCARD/ : { *(.comment) *(.note.*) *(.ARM.exidx*) *(.ARM.extab*)
                  *(.eh_frame) *(.llvm_addrsig) *(.riscv.attributes)
                  *(.ARM.attributes) *(.MIPS.abiflags) *(.reginfo) *(.pdr)
                  *(.gnu.attributes) *(.xtensa.info) *(.xt.prop*) *(.xt.lit*) }
}
"""


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def compile_corpus(name, out):
    if name in G:
        cc, flags = [G[name][0]], G[name][1].split()
    else:
        cc, flags = ["clang"], T[name][0].split() + ["-fno-addrsig"]
    srcs = sorted(glob.glob(ROOT + "/lib/libc/src/*/*.c") + glob.glob(ROOT + "/lib/rt/*.c"))
    if name == "avr":
        # (the whole libc is past what an AVR's 16-bit pointers reach)
        srcs = sorted(glob.glob(ROOT + "/lib/rt/*.c") +
                      glob.glob(ROOT + "/lib/libc/src/string/*.c") +
                      glob.glob(ROOT + "/lib/libc/src/ctype/*.c") +
                      glob.glob(ROOT + "/lib/libc/src/stdlib/qsort.c") +
                      glob.glob(ROOT + "/lib/libc/src/stdlib/strtol.c"))
    objs = []
    for s in srcs:
        o = os.path.join(out, os.path.basename(os.path.dirname(s)) + "-" +
                         os.path.basename(s)[:-2] + ".o")
        if not (os.path.exists(o) and os.path.getmtime(o) > os.path.getmtime(s)):
            # (Xtensa: one .text and one .literal an object -- GNU ld's
            # emulation regroups per-function literal sections its own way)
            fs = [] if name == "xtensa" else ["-ffunction-sections", "-fdata-sections"]
            r = run(cc + flags + ["-O2", "-c", "-w", "-ffreestanding",
                     "-fno-asynchronous-unwind-tables", "-fno-unwind-tables"] + fs + [
                     "-I" + ROOT + "/lib/libc/include", "-I" + ROOT + "/lib/libc/src/math",
                     "-I" + ROOT + "/lib/rt", s, "-o", o])
            if r.returncode:
                continue
        objs.append(o)
    return objs


def nm(path):
    r = run(["llvm-nm", "-P", "--no-sort", path])
    return [l.split() for l in r.stdout.splitlines() if len(l.split()) >= 2]


def pick(objs):
    """A set with each strong global defined once: an object that defines
    a name an earlier one did is left out."""
    seen, keep = set(), []
    for o in objs:
        defs = {f[0] for f in nm(o) if f[1] in "TDRBSC"}
        if defs & seen:
            continue
        seen |= defs
        keep.append(o)
    return keep


def stub(name, objs, out):
    """Every name the set uses and does not define, as a function of the
    stub, and the entry."""
    defined, used = set(), set()
    for o in objs:
        for f in nm(o):
            if f[1] in "Uwv":
                used.add(f[0])
            elif f[1] in "TDRBSCVWtdrb":
                defined.add(f[0])
    need = sorted(n for n in used - defined
                  if re.match(r"^[A-Za-z_.$][A-Za-z0-9_.$]*$", n))
    src = os.path.join(out, "audit-stub.c")
    # (each by its object-file name, an asm label: RX's C names carry an
    # underscore, and some names are no C identifier)
    with open(src, "w") as f:
        for k, n in enumerate(need):
            f.write('void s%d(void) __asm__("%s");\nvoid s%d(void) {}\n' % (k, n, k))
        f.write('void audit_entry(void) __asm__("audit_entry");\n'
                '__attribute__((section(".text.audit_entry"))) void audit_entry(void) {}\n')
    o = os.path.join(out, "audit-stub.o")
    cc = [G[name][0]] + G[name][1].split() if name in G else \
         ["clang"] + T[name][0].split() + ["-fno-addrsig"]
    r = run(cc + ["-O1", "-c", "-w", "-ffreestanding",
             "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
             "-ffunction-sections", "-fno-builtin", src, "-o", o])
    return o if r.returncode == 0 else None


def symtab(path):
    """name -> (address, size) of the sized, allocated symbols"""
    r = run(["llvm-nm", "-P", "-S", "--defined-only", path])
    t = {}
    for l in r.stdout.splitlines():
        f = l.split()
        if len(f) >= 4 and f[1] in "TtDdRrBbVv":
            t[f[0]] = (int(f[2], 16), int(f[3], 16))
    return t


def image(path):
    """the loaded bytes, by address: (base, bytes) of each allocated
    PROGBITS section"""
    r = run(["llvm-readelf", "-SW", path])
    secs = []
    with open(path, "rb") as fh:
        data = fh.read()
    for l in r.stdout.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+\S+\s+(\S+)", l)
        if m and "A" in m.group(5):
            addr, off, size = int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)
            secs.append((addr, data[off:off + size]))
    return secs


def byte_at(secs, a):
    for base, b in secs:
        if base <= a < base + len(b):
            return b[a - base]
    return None


def unmerge(objs, out):
    """Copies with SHF_MERGE cleared: where several copies of a string
    could serve, the two linkers may each pick another, both right; with
    nothing merged every byte has one right value."""
    plain = os.path.join(out, "plain")
    os.makedirs(plain, exist_ok=True)
    res = []
    for o in objs:
        r = run(["llvm-readelf", "-SW", o])
        args = []
        for l in r.stdout.splitlines():
            m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+PROGBITS\s+\S+\s+\S+\s+\S+\s+\S+\s+(\S+)", l)
            if m and "M" in m.group(2):
                args += ["--set-section-flags", m.group(1) + "=alloc,load,readonly,data"]
        p = os.path.join(plain, os.path.basename(o))
        if args:
            run(["llvm-objcopy"] + args + [o, p])
        else:
            shutil.copy(o, p)
        res.append(p)
    return res


def sizes(path):
    r = run(["llvm-readelf", "-SW", path])
    return {m.group(1): int(m.group(2), 16) for m in
            (re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+\S+\s+\S+\s+([0-9a-f]+)", l)
             for l in r.stdout.splitlines()) if m}


def audit(name, work):
    out = os.path.join(work, name)
    os.makedirs(out, exist_ok=True)
    merged = pick(compile_corpus(name, out))
    objs = unmerge(merged, out)
    st = stub(name, objs, out)
    if not st:
        return "%s: the stub does not compile" % name
    objs.append(st)
    ld = os.path.join(out, "audit.ld")
    with open(ld, "w") as f:
        sc = AVR_SCRIPT if name == "avr" else \
             SCRIPT % (G[name][4] if name in G else T[name][1])
        if name == "xtensa":
            # (the literals first, as one block: GNU ld's emulation
            # regroups literals and code its own way when they are mixed)
            sc = sc.replace("*(.text.audit_entry) *(.text .text.*) *(.literal .literal.*)",
                            "*(.text.audit_entry) *(.literal .literal.*) *(.text .text.*)")
        f.write(sc)
    e, g = os.path.join(out, "embld.elf"), os.path.join(out, "lld.elf")
    r = run([ROOT + "/embld", "-T", ld, "-o", e] + objs)
    if r.returncode:
        msg = (r.stderr.strip().splitlines() or ["?"])[0]
        return "%s: embld refused: %s" % (name, msg[:240])
    refname = "GNU ld" if name in G else "ld.lld"
    def reflink(dst, inputs):
        if name in G:
            return run([G[name][2]] + G[name][3].split() + ["-T", ld, "-o", dst] + inputs)
        rr = run(["ld.lld", "-O2", "--no-branch-to-branch", "-T", ld, "-o", dst, "--no-relax"] + inputs)
        if rr.returncode:
            rr = run(["ld.lld", "-O2", "--no-branch-to-branch", "-T", ld, "-o", dst] + inputs)
        return rr
    r = reflink(g, objs)
    if r.returncode:
        msg = (r.stderr.strip().splitlines() or ["?"])[0]
        return "%s: %s refused: %s" % (name, refname, msg[:240])
    se, sg = symtab(e), symtab(g)
    ie, ig = image(e), image(g)
    common = sorted(set(se) & set(sg))
    moved = [n for n in common if se[n][0] != sg[n][0]]
    differ = collections.OrderedDict()
    for n in common:
        if n in moved:
            continue
        a, size = sg[n]
        for k in range(size):
            x, y = byte_at(ie, a + k), byte_at(ig, a + k)
            if x != y and y is not None:
                differ.setdefault(n, []).append(k)
    lines = ["%s: %d objects, %d symbols, %d placed differently, %d differ in bytes"
             % (name, len(objs), len(common), len(moved), len(differ))]
    # and merged as they come: the same sizes
    me, mg = os.path.join(out, "embld-m.elf"), os.path.join(out, "lld-m.elf")
    rm = run([ROOT + "/embld", "-T", ld, "-o", me] + merged + [st])
    rg = reflink(mg, merged + [st])
    if rm.returncode or rg.returncode:
        lines.append("    merged: %s refused" % ("embld" if rm.returncode else refname))
    else:
        a, b = sizes(me), sizes(mg)
        bad = ["%s %d/%d" % (k, a.get(k, 0), b[k]) for k in (".text", ".rodata", ".data")
               if k in b and a.get(k, 0) != b[k]]
        lines.append("    merged: " + ("sizes as %s's" % refname if not bad else "embld/%s " % refname + ", ".join(bad)))
    for n in moved[:5]:
        lines.append("    at 0x%x, %s 0x%x: %s" % (se[n][0], refname, sg[n][0], n))
    for n, ks in list(differ.items())[:8]:
        lines.append("    %s+0x%x (%d bytes)" % (n, ks[0], len(ks)))
    return "\n".join(lines)


def main():
    names = sys.argv[1:] or list(T) + [n for n in G if shutil.which(G[n][2])]
    for t in ("clang", "ld.lld", "llvm-nm", "llvm-readelf"):
        if not shutil.which(t):
            sys.exit("link-audit: needs %s" % t)
    keep = os.environ.get("LINK_AUDIT_OUT")
    work = keep or tempfile.mkdtemp(prefix="link-audit-")
    try:
        for n in names:
            print(audit(n, work), flush=True)
    finally:
        if not keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
