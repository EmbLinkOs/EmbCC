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
  # x86-64-v3: AVX2, FMA, BMI1/2, MOVBE -- what -march=haswell and later emit
  "x86v3": ("--target=x86_64-elf", "clang", "--target=x86_64-elf -march=x86-64-v3",
            "llvm-mc -filetype=obj -triple=x86_64 {src} -o {obj}", "llvm-objdump", "#"),
  "x86v4": ("--target=x86_64-elf", "clang", "--target=x86_64-elf -march=x86-64-v4",
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
  # ARMv8.1-M with MVE (Helium): clang vectorizes with it, and uses the
  # low-overhead loops and csel
  "m55":   ("--target=thumbv8m.main-none-eabihf -mcpu=cortex-m55", "clang",
            "--target=thumbv8.1m.main-none-eabihf -mcpu=cortex-m55 -mfloat-abi=hard",
            "llvm-mc -filetype=obj -triple=thumbv8.1m.main-none-eabi -mattr=+mve.fp,+fp64,+lob {src} -o {obj}",
            "llvm-objdump", "@"),
  "m33":   ("--target=thumbv8m.main-none-eabi -mcpu=cortex-m33", "clang",
            "--target=thumbv8m.main-none-eabi -mcpu=cortex-m33 -mfloat-abi=soft",
            "llvm-mc -filetype=obj -triple=thumbv8m.main-none-eabi -mcpu=cortex-m33 {src} -o {obj}",
            "llvm-objdump", "@"),
  "a7":    ("--target=armv7a-none-eabi", "clang",
            "--target=armv7a-none-eabi -marm -mfloat-abi=soft",
            "llvm-mc -filetype=obj -triple=armv7a-none-eabi {src} -o {obj}", "llvm-objdump", "@"),
  # Cortex-A with NEON: clang vectorizes the corpus's loops
  "a7neon": ("--target=armv7a-none-eabihf -mfpu=neon-vfpv4", "clang",
            "--target=armv7a-none-eabihf -marm -mfpu=neon-vfpv4 -mfloat-abi=hard",
            "llvm-mc -filetype=obj -triple=armv7a-none-eabihf -mattr=+neon,+vfp4 {src} -o {obj}",
            "llvm-objdump", "@"),
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
  # (literals in .text before each function, EmbCC's Xtensa convention:
  # GNU as's --text-section-literals)
  "xtensa": ("--target=xtensa-esp32-elf -mlongcalls -mtext-section-literals",
            HOME + "/cross/xtensa/xtensa-esp-elf/bin/xtensa-esp32-elf-gcc",
            "-mlongcalls -mtext-section-literals",
            HOME + "/cross/xtensa/xtensa-esp-elf/bin/xtensa-esp32-elf-as --longcalls --text-section-literals --no-target-align {src} -o {obj}",
            HOME + "/cross/xtensa/xtensa-esp-elf/bin/xtensa-esp32-elf-objdump", "#"),
  "rx":    ("--target=rx-none-elf -fpu", HOME + "/cross/rx-elf/bin/rx-elf-gcc", "",
            HOME + "/cross/rx-elf/bin/rx-elf-as {src} -o {obj}",
            HOME + "/cross/rx-elf/bin/rx-elf-objdump", ";"),
  "m68k":  ("--target=m68k-none-elf", "m68k-elf-gcc", "-mcpu=5208",
            "m68k-elf-as -mcpu=5208 {src} -o {obj}", "m68k-elf-objdump", "|"),
  "tricore": ("--target=tricore-none-elf", HOME + "/cross/tricore/bin/tricore-elf-gcc", "",
            HOME + "/cross/tricore/bin/tricore-elf-as -mtc162 {src} -o {obj}",
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
                r"cbz|cbnz|b\.\w+|br\w*|rjmp|rcall|bra|bsr|jl|loop\w*|bc\w*|bdnz\w*|"
                r"bgtu|bleu|bgt|ble|bpz|bn|bo|bno|bnc|bz|bnz)\b")

PADDING = False
BAREHEX = False      # TriCore's objdump prints a target as bare hex too

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
        # (GNU objdump: the raw bytes, when shown, end at a tab -- an
        # instruction's own text may start with hex letters: `add`)
        m = re.match(r"^\s*[0-9a-f]+:\t(?:(?:[0-9a-f]{2,8} ?)+ +\t)?(.*)$", l) or \
            re.match(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2,8}\s)*\s*(.*)$", l)
        if not m or not m[1].strip() or l.rstrip().endswith(">:"):
            continue
        t = re.sub(r"\s+", " ", m[1].split("<")[0].split("//")[0].split("@ ")[0]).strip()
        t = re.sub(r"\.(n|w)\b", "", t)
        # zero padding: GNU as's Xtensa property tables (.xt.prop) tell
        # objdump it is data (.byte 00), and without them it reads as ill
        if PADDING and re.match(r"^(ill|\.byte 0x?0+|0+)$", t):
            continue
        op = t.split(" ")[0]
        if BR.match(op) or op.startswith(("b", "j")) and re.search(r"0x[0-9a-f]+|\.[+-]", t):
            t = re.sub(r"(0x)?[0-9a-f]+$|\.[+-]?\d+$|[+-]?\d+$", "T", t)
        # (Xtensa's objdump prints a target as bare hex: call8 ad0)
        elif PADDING and re.match(r"^(b\w*|j|call\d+|loop\w*)$", op) and \
                re.search(r"\s[0-9a-f]+$", t):
            t = re.sub(r"[0-9a-f]+$", "T", t)
        elif BAREHEX and re.match(r"^(j\w*(\.\w+)?|loop\w*|f?call)$", op) and \
                not re.match(r"^(ji|jli|ja|jla)$", op) and re.search(r"[\s,][0-9a-f]+$", t):
            t = re.sub(r"[0-9a-f]+$", "T", t)
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

NOP = re.compile(r"^(nop\w*|data16|cs|xchg %ax, ?%ax|mov r8, r8)\b")

def sized(dis):
    """(bytes, normalized text) of each instruction but the padding nops,
    from a disassembly with raw bytes"""
    r = []
    for l in dis.splitlines():
        # llvm-objdump's bytes are pairs (`01 02 03`), GNU's a run per
        # word (`046136`), ended by a tab
        g = re.match(r"^\s*[0-9a-f]+:\t((?:[0-9a-f]{2,8} ?)+) *\t(.*)$", l)
        m = g or re.match(r"^\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} )+)\s*(.*)$", l)
        if not m or l.rstrip().endswith(">:"):
            continue
        nbytes = len(m[1].replace(" ", "")) // 2
        t = re.sub(r"\s+", " ", m[2].split("<")[0].split("#")[0]).strip()
        t = re.sub(r"^([0-9a-f]{2} ?)+(?= |$)", "", t).strip()   # (a long nop's wrapped bytes)
        if not t or NOP.match(t):
            continue
        op = t.split(" ")[0]
        if BR.match(op) or op.startswith("j"):
            # (RX writes a branch's width as its suffix: beq.s, .b, .w, .a)
            t = re.sub(r"\.(s|b|w|a)$", "", op)
        r.append((nbytes, t))
    return r

def xt_relaxed(ins):
    """Xtensa's relaxation choices, written one way: a long call (l32r aN
    and callxN aN) is the call it may become, and a narrow move (mov.n,
    printed mov) and an instruction widened to align a loop or a target
    (or aR, aS, aS; add.n as add) are the same instruction"""
    out, k = [], 0
    while k < len(ins):
        n, t = ins[k]
        t = re.sub(r"\.n\b", "", t)
        if re.match(r"^(\.byte 0+|ill)$", t):       # zero padding
            k += 1
            continue
        if t.startswith("l32r ") and k + 1 < len(ins) and \
                re.match(r"^callx(4|8|12) ", ins[k + 1][1]):
            out.append((3, "call" + ins[k + 1][1][5:].split(" ")[0]))
            k += 2
            continue
        op = t.split(" ")[0]
        if (op.startswith(("b", "call", "loop", "j")) and op != "break") or \
                op in ("l32r",):
            t = op                          # (a target moves with the layout)
        m = re.match(r"^or (a\d+), (a\d+), (a\d+)$", t)
        if m and m[2] == m[3]:
            t = "mov %s, %s" % (m[1], m[2])
        out.append((3 if re.match(r"^(mov|add|addi|l32i|s32i|movi|beqz|bnez|ret|retw|nop) ", t + " ") else n, t))
        k += 1
    return out

ODX = []

def relaxed_only(objdump, eo, ro):
    """The two objects differ only where a branch took another width (and
    the alignment padding after it moved): every instruction the same,
    its length too unless it is a relative branch. llvm-mc keeps some
    branches long that reach short -- a layout settled from a bigger
    start; EmbCC's settles from the smallest (gas.c)."""
    a = sized(run([objdump] + ODX + ["-d", eo]).stdout)
    b = sized(run([objdump] + ODX + ["-d", ro]).stdout)
    if PADDING:
        a, b = xt_relaxed(a), xt_relaxed(b)
    if len(a) != len(b):
        return False
    for (la, ta), (lb, tb) in zip(a, b):
        if ta != tb:
            if not re.search(r"\bR_", ta) and ta.split(" ")[0] == tb.split(" ")[0] and \
               (BR.match(ta.split(" ")[0]) or ta.startswith("j")):
                continue
            # operands that name an address move with the layout
            if re.sub(r"0x[0-9a-f]+", "A", ta) == re.sub(r"0x[0-9a-f]+", "A", tb):
                continue
            return False
        if la != lb and not (BR.match(ta.split(" ")[0]) or ta.startswith("j")):
            return False
    return True

def lengths_differ(objdump, eo, ro):
    """None when every instruction has the reference's length, "branch"
    when only branches differ, else the index of the first that does."""
    a = sized(run([objdump] + ODX + ["-d", eo]).stdout)
    b = sized(run([objdump] + ODX + ["-d", ro]).stdout)
    if PADDING:
        a, b = xt_relaxed(a), xt_relaxed(b)
    if len(a) != len(b):
        return None          # (the text compare already passed: padding)
    br = False
    for k, ((la, ta), (lb, tb)) in enumerate(zip(a, b)):
        if la == lb:
            continue
        op = ta.split(" ")[0]
        if BR.match(op) or op.startswith(("j", "b")):
            br = True
            continue
        return k
    return "branch" if br else None

def audit(name, out, show):
    etgt, cc, _, refas, objdump, _ = T[name]
    if not have(cc) or not have(refas.split()[0]):
        print("%-8s (no referee: %s)" % (name, cc if not have(cc) else refas.split()[0]))
        return
    os.makedirs(out, exist_ok=True)
    files = corpus(name, out)
    ok = ref = differ = relaxed = 0
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
        # (MVE's instructions disassemble only with the extension asked)
        global ODX
        ODX = ["--mattr=+mve.fp,+lob"] if name == "m55" else []
        de = run([objdump] + ODX + ["-dr", "--no-show-raw-insn"] + na + [eo]).stdout
        dr = run([objdump] + ODX + ["-dr", "--no-show-raw-insn"] + na + [ro]).stdout
        global PADDING
        PADDING = name == "xtensa"
        global BAREHEX
        BAREHEX = name == "tricore"
        ne, nr = norm(de), norm(dr)
        k = same(ne, nr)
        # the text agrees, but a 16- and a 32-bit form can print alike
        # (TriCore's mov.aa, Thumb's add after .w is stripped): the lengths
        # must agree too, every instruction's but a branch's
        if k is None:
            r = lengths_differ(objdump, eo, ro)
            if r == "branch":
                relaxed += 1
                continue
            if r is not None:
                differ += 1
                a_, b_ = sized(run([objdump] + ODX + ["-d", eo]).stdout), sized(run([objdump] + ODX + ["-d", ro]).stdout)
                if PADDING:
                    a_, b_ = xt_relaxed(a_), xt_relaxed(b_)
                diffs.append("%s: '%s' is %d bytes, the reference's %d" % (
                    os.path.basename(a), a_[r][1], a_[r][0], b_[r][0]))
                continue
        if k is not None and relaxed_only(objdump, eo, ro):
            relaxed += 1
            continue
        if k is not None:
            differ += 1
            if k < 0:
                kk = next((i for i, (x, y) in enumerate(zip(ne, nr)) if x[0] != y[0]), min(len(ne), len(nr)))
                diffs.append("%s: %d instructions, reference %d (first apart: %s / %s)" % (
                    os.path.basename(a), len(ne), len(nr),
                    ne[kk][0] if kk < len(ne) else "-", nr[kk][0] if kk < len(nr) else "-"))
            else:
                diffs.append("%s: embcc '%s', reference '%s'" % (os.path.basename(a), ne[k][0], nr[k][0]))
    print("%-8s %4d/%-4d files assembled, %3d whose code differs%s" % (name, ok, ref, differ,
          ", %d more only in a branch's width" % relaxed if relaxed else ""))
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
