#!/usr/bin/env python3
"""llvm-mc as the oracle for A32 NEON: words decoded to text, statements
assembled to words. Shared by the NEON learner's steps."""
import os, re, subprocess, tempfile

TRIPLE = ['-triple=armv7a-none-eabi', '-mattr=+neon,+vfp4,+fp16']

def disasm(words):
    """words: 32-bit values. Returns per word (mnem, ops) or None."""
    d = tempfile.mkdtemp(prefix='nd')
    p = os.path.join(d, 'in.txt')
    with open(p, 'w') as f:
        for w in words:
            f.write('0x%02x 0x%02x 0x%02x 0x%02x\n' % (w & 255, w >> 8 & 255, w >> 16 & 255, w >> 24 & 255))
    r = subprocess.run(['llvm-mc', '--disassemble'] + TRIPLE + [p], capture_output=True, text=True)
    bad = set()
    for m in re.finditer(r'in\.txt:(\d+):\d+: warning: invalid instruction encoding', r.stderr):
        bad.add(int(m.group(1)) - 1)
    outs = [l for l in r.stdout.splitlines() if l.startswith('\t') and not l.startswith('\t.')]
    res, k = [], 0
    for i in range(len(words)):
        if i in bad:
            res.append(None)
            continue
        if k >= len(outs):
            res.append(None)
            continue
        l = outs[k].strip()
        k += 1
        l = l.split('@')[0].strip()
        parts = l.split(None, 1)
        res.append((parts[0], parts[1].strip() if len(parts) > 1 else ''))
    return res

MARK = 0xe7fabcfd                      # udf #0xabcd, after every statement

def assemble(stmts):
    """stmts: text lines. Returns per line its 32-bit word or None (refused,
    or not one word). Each statement is followed by a marker instruction,
    so a statement that makes no word or two cannot shift the others."""
    out = [None] * len(stmts)
    keep = list(range(len(stmts)))
    d = tempfile.mkdtemp(prefix='na')
    for rnd in range(30):
        p = os.path.join(d, 'a.s')
        with open(p, 'w') as f:
            f.write(' .syntax unified\n .arm\n .fpu neon-vfpv4\n')
            for i in keep:
                f.write(' %s\n udf #0xabcd\n' % stmts[i])
        r = subprocess.run(['llvm-mc', '-show-encoding'] + TRIPLE + [p], capture_output=True, text=True)
        bad = set()
        for m in re.finditer(r'a\.s:(\d+):\d+: error', r.stderr):
            ln = int(m.group(1)) - 4
            if 0 <= ln < 2 * len(keep) and ln % 2 == 0:
                bad.add(keep[ln // 2])
        if not bad:
            encs = [int(e[3] + e[2] + e[1] + e[0], 16) for e in
                    re.findall(r'encoding: \[0x([0-9a-f]{2}),0x([0-9a-f]{2}),0x([0-9a-f]{2}),0x([0-9a-f]{2})\]', r.stdout)]
            cur, k = [], 0
            for x in encs:
                if x == MARK:
                    if k < len(keep) and len(cur) == 1:
                        out[keep[k]] = cur[0]
                    k += 1
                    cur = []
                else:
                    cur.append(x)
            return out
        keep = [i for i in keep if i not in bad]
    return out
