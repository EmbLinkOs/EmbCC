#!/usr/bin/env python3
"""The assembler against real compiler output, per target.

    tools/asm-audit/audit.py [--out DIR] [--show N] TARGET...
    tools/asm-audit/audit.py --list

The corpus is lib/libc and lib/rt compiled to assembly by the target's
reference compiler (clang, or a GCC where LLVM has no such backend), at
-O2 and -Os. Every file is assembled by EmbCC (`embcc -c FILE.s`), and by
the reference assembler (llvm-mc, or that GCC's as). For each file both
assemble, the two objects' code is disassembled by the reference
disassembler and compared instruction by instruction, operands included,
once branch displacements and the `.n`/`.w` width a relaxing assembler
picks are set aside (they move every address after them; what they
encode is the same). A file whose code differs is a lead to an encoding
EmbCC gets wrong.

Prints, per target: files assembled / files the reference assembles,
files whose code differs, and the commonest first errors. Exit status 0;
the golden tests/golden/asm-coverage.sh holds the floors."""
import collections, glob, os, re, shutil, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
HOME = os.path.expanduser("~")

# name: (embcc target + flags, compiler, compiler flags, reference assembler
#        command (with {src} {obj}), disassembler, comment characters)
T = {
  "x86":   ("--target=x86_64-elf", "clang", "--target=x86_64-elf",
            "llvm-mc -filetype=obj -triple=x86_64 {src} -o {obj}", "llvm-objdump", "#"),
  "a64":   ("--target=aarch64-elf", "clang", "--target=aarch64-none-elf",
            "llvm-mc -filetype=obj -triple=aarch64 {src} -o {obj}", "llvm-objdump", "//"),
  "m4":    ("--target=thumbv7em-none-eabi", "clang",
            "--target=thumbv7em-none-eabi -mcpu=cortex-m4 -mfloat-abi=soft",
            "llvm-mc -filetype=obj -triple=thumbv7em-none-eabi -mcpu=cortex-m4 {src} -o {obj}",
            "llvm-objdump", "@"),
  "m4f":   ("--target=thumbv7em-none-eabihf", "clang",
            "--target=thumbv7em-none-eabihf -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16",
            "llvm-mc -filetype=obj -triple=thumbv7em-none-eabihf -mcpu=cortex-m4 -mattr=+vfp4d16sp {src} -o {obj}",
            "llvm-objdump", "@"),
  "m0":    ("--target=thumbv6m-none-eabi", "clang",
            "--target=thumbv6m-none-eabi -mcpu=cortex-m0",
            "llvm-mc -filetype=obj -triple=thumbv6m-none-eabi -mcpu=cortex-m0 {src} -o {obj}",
            "llvm-objdump", "@"),
  "m33":   ("--target=thumbv8m.main-none-eabi -mcpu=cortex-m33", "clang",
            "--target=thumbv8m.main-none-eabi -mcpu=cortex-m33 -mfloat-abi=soft",
            "llvm-mc -filetype=obj -triple=thumbv8m.main-none-eabi -mcpu=cortex-m33 {src} -o {obj}",
            "llvm-objdump", "@"),
  "a7":    ("--target=armv7a-none-eabi", "clang",
            "--target=armv7a-none-eabi -marm -mfloat-abi=soft",
            "llvm-mc -filetype=obj -triple=armv7a-none-eabi {src} -o {obj}", "llvm-objdump", "@"),
  "rv32":  ("--target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32", "clang",
            "--target=riscv32-unknown-elf -march=rv32imac -mabi=ilp32 -mno-relax",
            "llvm-mc -filetype=obj -triple=riscv32 -mattr=+m,+a,+c,-relax {src} -o {obj}",
            "llvm-objdump", "#"),
  "rv64":  ("--target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d", "clang",
            "--target=riscv64-unknown-elf -march=rv64gc -mabi=lp64d -mno-relax",
            "llvm-mc -filetype=obj -triple=riscv64 -mattr=+m,+a,+f,+d,+c,-relax {src} -o {obj}",
            "llvm-objdump", "#"),
  "mips":  ("--target=mipsel-none-elf", "clang",
            "--target=mipsel-none-elf -mips32r2 -msoft-float -mno-abicalls -G0 -fno-pic",
            "llvm-mc -filetype=obj -triple=mipsel -mcpu=mips32r2 {src} -o {obj}", "llvm-objdump", "#"),
  "mips64": ("--target=mips64el-none-elf", "clang",
            "--target=mips64el-none-elf -msoft-float -mno-abicalls -G0 -fno-pic",
            "llvm-mc -filetype=obj -triple=mips64el {src} -o {obj}", "llvm-objdump", "#"),
  "ppc":   ("--target=powerpc-none-eabi", "clang", "--target=powerpc-none-eabi -msoft-float",
            "llvm-mc -filetype=obj -triple=powerpc {src} -o {obj}", "llvm-objdump", "#"),
  "sparc": ("--target=sparc-none-elf", "clang", "--target=sparc-none-elf -mcpu=leon3 -msoft-float",
            "llvm-mc -filetype=obj -triple=sparc -mcpu=leon3 {src} -o {obj}", "llvm-objdump", "!"),
  "avr":   ("--target=avr --mcu=atmega328p", "clang", "--target=avr -mmcu=atmega328p",
            "llvm-mc -filetype=obj -triple=avr -mcpu=atmega328p {src} -o {obj}", "llvm-objdump", ";"),
  "la64":  ("--target=loongarch64-unknown-elf", "clang",
            "--target=loongarch64-unknown-elf -mno-lsx -mno-lasx",
            "llvm-mc -filetype=obj -triple=loongarch64 {src} -o {obj}", "llvm-objdump", "#"),
  # where LLVM has no backend, a GCC is the referee (tools: ~/cross, brew)
  "xtensa": ("--target=xtensa-esp32-elf", HOME + "/cross/xtensa/xtensa-esp-elf/bin/xtensa-esp32-elf-gcc",
            "-mlongcalls",
            HOME + "/cross/xtensa/xtensa-esp-elf/bin/xtensa-esp32-elf-as --longcalls {src} -o {obj}",
            HOME + "/cross/xtensa/xtensa-esp-elf/bin/xtensa-esp32-elf-objdump", "#"),
  "rx":    ("--target=rx-none-elf", HOME + "/cross/rx-elf/bin/rx-elf-gcc", "",
            HOME + "/cross/rx-elf/bin/rx-elf-as {src} -o {obj}",
            HOME + "/cross/rx-elf/bin/rx-elf-objdump", ";"),
  "m68k":  ("--target=m68k-none-elf", "m68k-elf-gcc", "-mcpu=5208",
            "m68k-elf-as -mcpu=5208 {src} -o {obj}", "m68k-elf-objdump", "|"),
  "tricore": ("--target=tricore-none-elf", HOME + "/cross/tricore/bin/tricore-elf-gcc", "",
            HOME + "/cross/tricore/bin/tricore-elf-as {src} -o {obj}",
            HOME + "/cross/tricore/bin/tricore-elf-objdump", "#"),
}

def have(cmd):
    return shutil.which(cmd) is not None or os.path.exists(cmd)

def run(cmd, **kw):
    return subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True,
                          text=True, **kw)

def corpus(name, out):
    etgt, cc, cflags, _, _, _ = T[name]
    srcs = sorted(glob.glob(ROOT + "/lib/libc/src/*/*.c") + glob.glob(ROOT + "/lib/rt/*.c"))
    files = []
    for opt in ("-O2", "-Os"):
        for s in srcs:
            base = os.path.basename(os.path.dirname(s)) + "-" + os.path.basename(s)
            a = os.path.join(out, base + opt + ".s")
            if not (os.path.exists(a) and os.path.getmtime(a) > os.path.getmtime(s)):
                r = run([cc] + cflags.split() + [opt, "-S", "-w", "-ffreestanding",
                        "-fno-asynchronous-unwind-tables", "-fno-addrsig" if cc == "clang" else "-fno-ident",
                        "-I" + ROOT + "/lib/libc/include", "-I" + ROOT + "/lib/libc/src/math",
                        "-I" + ROOT + "/lib/rt", s, "-o", a])
                if r.returncode:
                    if os.path.exists(a):
                        os.remove(a)
                    continue
            files.append(a)
    return files

BR = re.compile(r"\b(b|bl|blx|bx|j|jal|jmp|call|beq|bne|blt|bge|bltu|bgeu|bnez|beqz|"
                r"cbz|cbnz|b\.\w+|br\w*|rjmp|rcall|bra|bsr|jl|loop\w*|bc\w*|bdnz\w*)\b")

def norm(dis):
    """The instructions of a disassembly (objdump -dr), without addresses,
    raw bytes, branch targets or the relaxation width; each with whether a
    relocation applies to it -- then its operands are the linker's, and
    one assembler may have resolved what the other left to relocate."""
    ins = []
    pair = False
    for l in dis.splitlines():
        if re.search(r":\s+R_[A-Z0-9_]+", l) and ins:
            ins[-1] = (ins[-1][0], True)
            # a RISC-V call's relocation covers the jalr after its auipc
            pair = bool(re.search(r"R_RISCV_CALL", l))
            continue
        m = re.match(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2,8}\s)*\s*(.*)$", l)
        if not m or not m[1].strip() or l.rstrip().endswith(">:"):
            continue
        t = re.sub(r"\s+", " ", m[1].split("<")[0].split("//")[0].split("@ ")[0]).strip()
        t = re.sub(r"\.(n|w)\b", "", t)
        op = t.split(" ")[0]
        if BR.match(op) or op.startswith(("b", "j")) and re.search(r"0x[0-9a-f]+|\.[+-]", t):
            t = re.sub(r"(0x)?[0-9a-f]+$|\.[+-]?\d+$|[+-]?\d+$", "T", t)
        ins.append((t, pair))
        pair = False
    return ins

def same(a, b):
    """Two normalized instruction lists agree: equal, or where either has
    a relocation, equal in mnemonic and in their register operands."""
    if len(a) != len(b):
        return -1
    for k, ((x, rx), (y, ry)) in enumerate(zip(a, b)):
        if x == y:
            continue
        if (rx or ry) and x.split(" ")[0] == y.split(" ")[0]:
            regs = lambda t: re.findall(r"%?\$?\b[a-z]+[0-9]+\b", t)
            if regs(x) == regs(y):
                continue
        return k
    return None

def audit(name, out, show):
    etgt, cc, _, refas, objdump, _ = T[name]
    if not have(cc) or not have(refas.split()[0]):
        print("%-8s (no referee: %s)" % (name, cc if not have(cc) else refas.split()[0]))
        return
    os.makedirs(out, exist_ok=True)
    files = corpus(name, out)
    ok = ref = differ = 0
    errs = collections.Counter()
    diffs = []
    for a in files:
        eo, ro = a[:-2] + ".e.o", a[:-2] + ".r.o"
        rr = run(refas.format(src=a, obj=ro))
        if rr.returncode:
            continue                      # the referee's own limit
        ref += 1
        r = run([ROOT + "/embcc"] + etgt.split() + ["-c", a, "-o", eo])
        if r.returncode:
            first = (r.stderr.strip().splitlines() or ["?"])[0]
            first = re.sub(r"^.*?error: ", "", first)
            errs[re.sub(r'"[^"]*"', '"..."', first)[:110]] += 1
            continue
        ok += 1
        # RISC-V's compressed forms print as their 32-bit aliases unless
        # asked not to, and a missed compression is a difference
        na = ["-M", "no-aliases"] if name.startswith("rv") else []
        de = run([objdump, "-dr", "--no-show-raw-insn"] + na + [eo]).stdout
        dr = run([objdump, "-dr", "--no-show-raw-insn"] + na + [ro]).stdout
        ne, nr = norm(de), norm(dr)
        k = same(ne, nr)
        if k is not None:
            differ += 1
            if k < 0:
                kk = next((i for i, (x, y) in enumerate(zip(ne, nr)) if x[0] != y[0]), min(len(ne), len(nr)))
                diffs.append("%s: %d instructions, reference %d (first apart: %s / %s)" % (
                    os.path.basename(a), len(ne), len(nr),
                    ne[kk][0] if kk < len(ne) else "-", nr[kk][0] if kk < len(nr) else "-"))
            else:
                diffs.append("%s: embcc '%s', reference '%s'" % (os.path.basename(a), ne[k][0], nr[k][0]))
    print("%-8s %4d/%-4d files assembled, %3d whose code differs" % (name, ok, ref, differ))
    for k, n in errs.most_common(show):
        print("           %4d  %s" % (n, k))
    for d in diffs[:show]:
        print("           diff  %s" % d)
    open(os.path.join(out, "result.txt"), "w").write("%d %d %d\n" % (ok, ref, differ))

def main(argv):
    out = os.path.join(ROOT, "tests/golden/out/asm-audit")
    show = 3
    names = []
    i = 0
    while i < len(argv):
        if argv[i] == "--out": out = argv[i + 1]; i += 2; continue
        if argv[i] == "--show": show = int(argv[i + 1]); i += 2; continue
        if argv[i] == "--list": print(" ".join(T)); return 0
        names.append(argv[i]); i += 1
    for n in names or list(T):
        if n not in T:
            print("unknown target %s; --list" % n); return 2
        audit(n, os.path.join(out, n), show)
    return 0

sys.exit(main(sys.argv[1:]))
