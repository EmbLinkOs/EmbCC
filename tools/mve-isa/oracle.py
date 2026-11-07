#!/usr/bin/env python3
"""llvm-mc as the oracle for MVE (ARMv8.1-M's M-profile Vector Extension,
Helium): Thumb words decoded to text, statements assembled to words. A
word is the first halfword in its high 16 bits, the second in its low
ones, as the architecture writes an encoding; in memory each halfword is
little-endian, the first one first. Shared by the learner's steps."""
import os, re, subprocess, tempfile

TRIPLE = ['-triple=thumbv8.1m.main-none-eabi', '-mattr=+mve.fp,+fp64,+lob']

def tbytes(w):
    h1, h2 = w >> 16 & 0xffff, w & 0xffff
    return (h1 & 255, h1 >> 8, h2 & 255, h2 >> 8)


def disasm(words):
    """words: 32-bit Thumb words. Returns per word (mnem, ops) or None.
    Each word is in brackets of its own -- llvm-mc's way to say where an
    instruction's bytes end: an invalid one, of which it would skip a
    single byte, cannot shift the rest out of halfword alignment -- and a
    bracketed marker (udf.w #0xabc) after it ends its segment; a word
    counts only when one instruction is exactly its four bytes."""
    d = tempfile.mkdtemp(prefix='md')
    p = os.path.join(d, 'in.txt')
    with open(p, 'w') as f:
        for w in words:
            f.write('[%s]\n[0xf0 0xf7 0xbc 0xaa]\n' % ' '.join('0x%02x' % b for b in tbytes(w)))
    r = subprocess.run(['llvm-mc', '--disassemble', '-show-encoding'] + TRIPLE + [p],
                       capture_output=True, text=True)
    res, seg = [], []
    for l in r.stdout.splitlines():
        m = re.search(r'encoding: \[([^\]]*)\]', l)
        if not m or not l.startswith('\t'):
            continue
        bs = [int(x, 16) for x in m.group(1).split(',')]
        if bs == [0xf0, 0xf7, 0xbc, 0xaa]:
            k = len(res)
            if k < len(words):
                want = list(tbytes(words[k]))
                hit = None
                if seg and seg[0][0] == want:
                    txt = seg[0][1].split('@')[0].strip()
                    parts = txt.split(None, 1)
                    hit = (parts[0], parts[1].strip() if len(parts) > 1 else '')
                res.append(hit)
            seg = []
            continue
        seg.append((bs, l))
    while len(res) < len(words):
        res.append(None)
    return res

MARK = 0xf7f0aabc                      # udf.w #0xabc, after every statement

def assemble(stmts):
    """stmts: text lines. Returns per line its 32-bit word or None (refused,
    or not one 32-bit instruction). A predicated statement (vaddt) is
    assembled inside a `vpst` block of its own, which is not part of its
    word."""
    out = [None] * len(stmts)
    keep = list(range(len(stmts)))
    d = tempfile.mkdtemp(prefix='ma')
    for rnd in range(30):
        p = os.path.join(d, 'a.s')
        with open(p, 'w') as f:
            f.write(' .syntax unified\n .thumb\n')
            for i in keep:
                f.write(' %s\n udf.w #0xabc\n' % stmts[i])
        r = subprocess.run(['llvm-mc', '-show-encoding'] + TRIPLE + [p], capture_output=True, text=True)
        bad = set()
        for m in re.finditer(r'a\.s:(\d+):\d+: error', r.stderr):
            ln = int(m.group(1)) - 3
            # (an error on a marker is the statement's before it: a vpt
            # opens a block the marker is then in)
            if 0 <= ln < 2 * len(keep):
                bad.add(keep[ln // 2])
        if not bad:
            cur, k = [], 0
            for e in re.findall(r'encoding: \[([^\]]*)\]', r.stdout):
                bs = [int(x, 16) for x in e.split(',')]
                if len(bs) == 4:
                    x = (bs[0] | bs[1] << 8) << 16 | (bs[2] | bs[3] << 8)
                else:
                    x = -1
                if x == MARK:
                    if k < len(keep) and len(cur) == 1 and cur[0] >= 0:
                        out[keep[k]] = cur[0]
                    k += 1
                    cur = []
                else:
                    cur.append(x)
            return out
        keep = [i for i in keep if i not in bad]
    return out
