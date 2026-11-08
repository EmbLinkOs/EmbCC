#!/usr/bin/env python3
"""Draws every EmbCC logo asset from one definition of the mark.

    python3 .github/assets/brand.py      # rewrites the SVGs, then the PNGs with rsvg-convert

The mark is the "trace C": the letter C drawn as one circuit-board trace, a solder pad at each
end -- C the language, and C the compiler, in the same hand as EmbLinkRTOS's "trace E". The
arc's centre sits right of the tile's because the pads overhang its open side. The colours are
the EmbLink family's four, on paper.

Also drawn from data in the repository, not typed in:
  demo-terminal.svg   the session in SESSION below, as it was printed
  bench-vs-clang.svg  tools/bench/results/2026-11-08.txt (tools/bench/run.sh) and
                      tools/bench/results/2026-11-08-scoreboard.md (tools/scoreboard.sh --quick)
and tools/embdoc/brand.h, the documentation site's header mark and favicon.
"""
import math
import pathlib
import re
import shutil
import subprocess
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[2]
ASSETS = ROOT / ".github/assets"
BENCH = ROOT / "tools/bench/results/2026-11-08.txt"
SIZE = ROOT / "tools/bench/results/2026-11-08-scoreboard.md"

INK, TEAL, TEAL_DARK, MINT, PAPER = "#0b1220", "#0f766e", "#115e59", "#5eead4", "#f8fafc"
SLATE = "#94a3b8"
FONT = "Inter, Segoe UI, Helvetica Neue, Arial, sans-serif"
MONO = "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace"

# the mark's geometry on a 200 x 200 canvas: the arc, and where its two ends are
CX, CY, R, OPEN = 106, 100, 52, math.radians(48)
STROKE, PAD_R, HOLE_R = 24, 18, 6.5
ENDS = ((CX + R * math.cos(OPEN), CY - R * math.sin(OPEN)),
        (CX + R * math.cos(OPEN), CY + R * math.sin(OPEN)))


def arc(trim=0.0):
    """The C from its upper end round the left to its lower end; `trim` shortens it at both
    ends by that much of its length (radians = trim / R)."""
    d = trim / R
    a0, a1 = OPEN + d, 2 * math.pi - OPEN - d
    x0, y0 = CX + R * math.cos(a0), CY - R * math.sin(a0)
    x1, y1 = CX + R * math.cos(a1), CY - R * math.sin(a1)
    return f"M{x0:.2f} {y0:.2f}A{R} {R} 0 1 0 {x1:.2f} {y1:.2f}"


def mark(trace, pad, hole=None):
    """The mark's elements. Without a hole colour the drill holes are transparent: each pad is a
    ring and the trace stops inside it, so the mark works on any background."""
    if hole:   # opaque pads cover the arc's ends
        s = (f'<path d="{arc()}" fill="none" stroke="{trace}" stroke-width="{STROKE}" '
             f'stroke-linecap="round"/>')
        for x, y in ENDS:
            s += (f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{PAD_R}" fill="{pad}"/>'
                  f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{HOLE_R}" fill="{hole}"/>')
        return s
    ring = (PAD_R + HOLE_R) / 2
    s = (f'<path d="{arc(PAD_R - 3)}" fill="none" stroke="{trace}" stroke-width="{STROKE}"/>')
    for x, y in ENDS:
        s += (f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{ring}" fill="none" stroke="{pad}" '
              f'stroke-width="{PAD_R - HOLE_R}"/>')
    return s


def tile(bg, trace, pad, hole, rx=46):
    return f'<rect width="200" height="200" rx="{rx}" fill="{bg}"/>' + mark(trace, pad, hole)


def svg(w, h, body, view=None):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
            f'viewBox="{view or f"0 0 {w} {h}"}">\n'
            f'  <!-- Drawn by .github/assets/brand.py; edit the script, not this file -->\n'
            f'  {body}\n</svg>\n')


BRAND_TILE = tile(TEAL, PAPER, MINT, TEAL)   # the logo: white trace, mint pads on teal
DARK_TILE = tile(INK, MINT, PAPER, INK)      # on dark surfaces: the avatar


def wordmark(x, y, size, ink, accent):
    return (f'<text x="{x}" y="{y}" fill="{ink}" font-family="{FONT}" font-size="{size}" '
            f'font-weight="800" letter-spacing="{-size * 0.025:.1f}">Emb<tspan fill="{accent}">CC'
            f'</tspan></text>')


def lockup(ink, accent):
    return svg(620, 200, f'{BRAND_TILE}' + wordmark(232, 133, 104, ink, accent))


def chips(x, y, names, size, height):
    out, cx = [], x
    for n in names:
        w = int(len(n) * size * 0.62 + size * 1.8)
        out.append(f'<g transform="translate({cx} {y})"><rect width="{w}" height="{height}" '
                   f'rx="{height / 2}" fill="#1e293b" stroke="#334155"/><text x="{w / 2}" '
                   f'y="{height * 0.67}" text-anchor="middle" font-family="{MONO}" '
                   f'font-size="{size}" fill="#e2e8f0">{n}</text></g>')
        cx += w + 12
    return "".join(out)


BACKDROP = ('<defs><linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" '
            'stop-color="#0b1220"/><stop offset="1" stop-color="#13233f"/></linearGradient>'
            '<pattern id="grid" width="32" height="32" patternUnits="userSpaceOnUse"><path '
            'd="M32 0H0V32" fill="none" stroke="#ffffff" stroke-opacity="0.045"/></pattern></defs>')
FAMILIES = ["AVR", "Cortex-M", "RISC-V", "Xtensa", "TriCore", "MIPS", "x86-64"]


def banner():
    body = (BACKDROP + '<rect width="1280" height="320" fill="url(#bg)"/>'
            '<rect width="1280" height="320" fill="url(#grid)"/>'
            f'<g transform="translate(88 70) scale(0.9)">{BRAND_TILE}</g>'
            + wordmark(300, 150, 76, PAPER, MINT)
            + f'<text x="302" y="198" fill="#cbd5e1" font-family="{FONT}" font-size="28">'
              'One C and C++ toolchain, from 8-bit AVR to 64-bit RISC-V</text>'
            + chips(302, 228, FAMILIES + ["+7 more"], 19, 36))
    return svg(1280, 320, body)


def social():
    lines = ["Compiler, assembler, linker, debugger and simulator, in ISO C99",
             "Every target's code is run on QEMU against clang or GCC",
             "Within 2% of clang at -O2, 6% behind at -Os (Cortex-M4, RV32 cycles)"]
    text = "".join(f'<text x="110" y="{476 + 46 * i}" font-family="{FONT}" font-size="27" '
                   f'fill="#e2e8f0"><tspan fill="{MINT}">●</tspan>  {t}</text>'
                   for i, t in enumerate(lines))
    body = (BACKDROP + '<rect width="1280" height="640" fill="url(#bg)"/>'
            '<rect width="1280" height="640" fill="url(#grid)"/>'
            f'<g transform="translate(100 116) scale(1.1)">{BRAND_TILE}</g>'
            + wordmark(372, 226, 92, PAPER, MINT)
            + f'<text x="376" y="284" fill="#cbd5e1" font-family="{FONT}" font-size="30">'
              'A C and C++ toolchain for firmware and kernels</text>'
            + chips(376, 318, FAMILIES, 20, 40) + text)
    return svg(1280, 640, body)


# A real session, captured on 2026-11-08 (macOS arm64) in a fresh clone of main at f5bde5bd:
# the README's first firmware, built and run in EmbSim. The lines are as printed; "…" marks
# the build's own output, left out.
SESSION = [
    ("$", "make embcc embld embsim"),
    ("…", ""),
    ("$", "./embcc --target=thumbv7m-none-eabi -c tests/harness/thumb/boot.c -o boot.o"),
    ("$", "./embcc --target=thumbv7m-none-eabi -c tests/harness/thumb/io.c   -o io.o"),
    ("$", "./embcc --target=thumbv7m-none-eabi --opt=speed -c hello.c -o hello.o"),
    ("$", "./embld -e reset -Ttext 0x0 -Tdata 0x20000000 boot.o io.o hello.o -o hello.elf"),
    ("$", "./embsim hello.elf --stats"),
    ("", "hello from EmbCC"),
    ("", "1 4 9 16 25 "),
    ("", "DONE"),
    ("", "embsim: lockup: a fault with HardFault already active at 0x00000000; 933 instructions, 1665 cycles (est.)"),
]


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def terminal():
    w, lh, top = 1040, 26, 62
    h = top + lh * len(SESSION) + 26
    body = (f'<rect width="{w}" height="{h}" rx="12" fill="{INK}"/>'
            f'<rect width="{w}" height="40" rx="12" fill="#1e293b"/><rect y="28" width="{w}" '
            f'height="12" fill="#1e293b"/>'
            + "".join(f'<circle cx="{24 + 22 * k}" cy="20" r="6.5" fill="{c}"/>'
                      for k, c in enumerate(("#475569", "#475569", "#475569")))
            + f'<text x="{w / 2}" y="25" text-anchor="middle" font-family="{FONT}" font-size="14" '
              f'fill="#94a3b8">a first firmware, without a board</text>')
    for k, (p, t) in enumerate(SESSION):
        y = top + lh * k
        if p == "$":
            line = (f'<tspan fill="{MINT}">$ </tspan><tspan fill="#e2e8f0">{esc(t)}</tspan>')
        elif p == "…":
            line = '<tspan fill="#64748b">…</tspan>'
        else:
            line = f'<tspan fill="#cbd5e1">{esc(t)}</tspan>'
        body += (f'<text x="24" y="{y}" font-family="{MONO}" font-size="15" '
                 f'xml:space="preserve">{line}</text>')
    return svg(w, h, body)


def bench_rows():
    """Each board and level of the speed run: the geometric mean of the cycle ratios, and each
    kernel's."""
    rows, cur = [], None
    for line in BENCH.read_text().splitlines():
        m = re.match(r"== (\S+) (-O\S+):", line)
        if m:
            cur = {"board": m.group(1), "level": m.group(2), "k": []}
            rows.append(cur)
            continue
        m = re.match(r"\s+geomean\s+\S+ instructions, (\S+) cycles", line)
        if m and cur:
            cur["geo"] = float(m.group(1))
            continue
        f = line.split()
        if cur and len(f) == 7 and re.match(r"[a-z]", f[0]):
            cur["k"].append(float(f[6]))
    return rows


def size_rows():
    """tools/scoreboard.sh's code-size table: each target's per-object ratio, all code."""
    rows, on = [], False
    for line in SIZE.read_text().splitlines():
        if line.startswith("## "):
            on = line.startswith("## 2.")
        m = re.match(r"\| ([a-z0-9_.-]+) \| ([0-9.]+) \|", line) if on else None
        if m:
            rows.append((m.group(1), float(m.group(2))))
    return rows


BOARDS = {"m4": "Cortex-M4", "rv32": "RV32"}
TARGETS = {"thumbv7em-none-eabi": "Cortex-M4", "thumbv6m-none-eabi": "Cortex-M0",
           "riscv32-unknown-elf": "RV32", "riscv64-unknown-elf": "RV64", "x86_64-elf": "x86-64",
           "aarch64-elf": "AArch64", "mipsel-none-elf": "MIPS32", "powerpc-none-eabi": "PowerPC",
           "sparc-none-elf": "SPARC", "loongarch64-unknown-elf": "LoongArch64", "avr": "AVR"}


def bench_chart():
    """Both measurements on one axis, EmbCC / clang: speed (a bar for the geometric mean, a dot
    per kernel) and code size (a bar per target)."""
    speed, size = bench_rows(), size_rows()
    w, left, right = 1040, 150, 200
    lo, hi = 0.70, 1.55
    sx = lambda v: left + (v - lo) / (hi - lo) * (w - left - right)
    txt = lambda x, y, t, size=13, fill="#334155", anchor="start", weight=400: (
        f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}" font-family="{FONT}" '
        f'font-size="{size}" font-weight="{weight}" fill="{fill}">{t}</text>')

    def bar(y, v, h, label=True):
        x0, x1 = sorted((sx(1.0), sx(v)))
        out = (f'<rect x="{x0:.1f}" y="{y - h / 2:.1f}" width="{max(x1 - x0, 2):.1f}" '
               f'height="{h}" rx="3" fill="{TEAL if v < 1.0 else SLATE}"/>')
        if label:
            out += (txt(x1 + 8, y + 4.5, f"{v:.3f}") if v >= 1.0 else
                    txt(x0 - 8, y + 4.5, f"{v:.3f}", anchor="end"))
        return out

    def axis(top, bottom):
        out = ""
        for v in (0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4, 1.5):
            x = sx(v)
            one = abs(v - 1.0) < 1e-9
            out += (f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{bottom}" '
                    f'stroke="{INK if one else "#e2e8f0"}" stroke-width="{1.5 if one else 1}"/>'
                    + txt(x, bottom + 16, f"{v:.1f}", 12, "#64748b", "middle"))
        return out

    body = (f'<rect width="{w}" height="100%" rx="12" fill="#ffffff"/>'
            + txt(24, 36, "EmbCC ÷ clang 23.1.5: below 1.0, EmbCC is ahead", 20, INK, weight=700)
            + txt(24, 58, "main at f5bde5bd, 2026-11-08: speed from tools/bench on QEMU, size from tools/scoreboard.sh",
                  13, "#64748b"))
    # speed
    y0 = 92
    body += (txt(24, y0, "Speed: estimated cycles", 15, INK, weight=700)
             + txt(24 + 186, y0, "bar: geometric mean of 13 kernels · dot: one kernel", 13, "#64748b"))
    top, rh = y0 + 16, 40
    body += axis(top, top + rh * len(speed))
    for k, r in enumerate(speed):
        y = top + rh * k + rh / 2
        body += txt(24, y + 5, f'{BOARDS.get(r["board"], r["board"])} {r["level"]}', 14, INK, weight=600)
        g = r["geo"]
        body += bar(y, g, 18, label=False)
        body += "".join(f'<circle cx="{sx(v):.1f}" cy="{y:.1f}" r="3.4" fill="{INK}" '
                        f'stroke="#ffffff" stroke-width="1"/>' for v in r["k"])
        body += txt(w - 24, y + 5, f'{g:.3f} · {abs(g - 1) * 100:.1f}% '
                    f'{"fewer" if g < 1 else "more"} cycles', 13, "#334155", "end")
    # size
    y0 = top + rh * len(speed) + 58
    body += (txt(24, y0, "Code size: lib/libc at -Os", 15, INK, weight=700)
             + txt(24 + 206, y0, "object by object, all code, llvm-nm symbol sizes", 13, "#64748b"))
    top, rh = y0 + 16, 27
    body += axis(top, top + rh * len(size))
    for k, (t, v) in enumerate(size):
        y = top + rh * k + rh / 2
        body += txt(24, y + 5, TARGETS.get(t, t), 14, INK, weight=600) + bar(y, v, 14)
    h = top + rh * len(size) + 40
    body = body.replace('height="100%"', f'height="{h}"', 1)
    return svg(w, h, body)


def embdoc_header():
    """The documentation site's mark, in its top bar, and its favicon: the logo tile, for
    tools/embdoc/embdoc.c."""
    head = ('<svg viewBox="0 0 200 200" width="24" height="24" aria-hidden="true">'
            + BRAND_TILE + '</svg>')
    icon = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200">' + BRAND_TILE
            + '</svg>')
    fav = "data:image/svg+xml," + urllib.parse.quote(icon.replace('"', "'"), safe=" =:/,.'-")

    def c_string(s):
        out, s = [], s.replace("\\", "\\\\").replace('"', '\\"')
        while s:
            out.append(f'    "{s[:72]}"')
            s = s[72:]
        return "\n".join(out)
    return ("/* The EmbCC mark for embdoc's pages, drawn by .github/assets/brand.py:\n"
            " * edit the script, not this file. */\n"
            "static const char embdoc_brand_svg[] =\n" + c_string(head) + ";\n\n"
            "static const char embdoc_favicon[] =\n" + c_string(fav) + ";\n")


SVGS = {
    "logo.svg": svg(200, 200, BRAND_TILE),
    "logo-mono.svg": svg(200, 200, mark(TEAL, TEAL)),
    "lockup-light.svg": lockup(INK, TEAL),
    "lockup-dark.svg": lockup(PAPER, MINT),
    "avatar.svg": svg(512, 512, DARK_TILE, "0 0 200 200"),
    "banner.svg": banner(),
    "social-preview.svg": social(),
    "demo-terminal.svg": terminal(),
    "bench-vs-clang.svg": bench_chart(),
}
PNGS = {"avatar.png": ("avatar.svg", 512), "social-preview.png": ("social-preview.svg", 1280)}


def main():
    for name, text in SVGS.items():
        (ASSETS / name).write_text(text)
    (ROOT / "tools/embdoc/brand.h").write_text(embdoc_header())
    rsvg = shutil.which("rsvg-convert")
    if not rsvg:
        print("rsvg-convert not found: the PNGs were not redrawn (brew install librsvg)")
        return
    for png, (src, width) in PNGS.items():
        subprocess.run([rsvg, "-w", str(width), "-o", str(ASSETS / png), str(ASSETS / src)],
                       check=True)


if __name__ == "__main__":
    main()
