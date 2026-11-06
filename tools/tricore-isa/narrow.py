#!/usr/bin/env python3
"""When does GNU as write a 3-operand statement as a 2-operand form?

For every mnemonic with a 3-operand shape X,Y,Z and a 16-bit form of the
shape without operand 1 (X,Z) or without operand 2 (X,Y), GNU assembles
`m r, r, v` (the destination is the first source) and `m r, v, r` (the
second); a 2-byte result is a narrowing. Registers are d1/a2 -- not d15,
a15 or sp, which have 3-operand 16-bit forms of their own. Output:
narrow.json, {"mnem shape": {"a": len, "b": len}}."""
import json, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from learn import AS

T = json.load(open('isa3.json'))
by = {}
for e in T:
    by.setdefault(e['mnem'], set()).add((e['shape'], e['len']))

def ops(shape):
    out, depth, cur = [], 0, ''
    for c in shape:
        if c == ',' and not depth:
            out.append(cur); cur = ''
            continue
        depth += c == '['
        depth -= c == ']'
        cur += c
    out.append(cur)
    return out

REG = {'D': '%d1', 'A': '%a2', 'E': '%e2'}
OTHER = {'D': '%d2', 'A': '%a3', 'E': '%e4', 'N': '1'}
tests = []
for m, shs in sorted(by.items()):
    for sh, ln in sorted(shs):
        o = ops(sh)
        # `sat.b d5, d5` is `sat.b d5`
        if len(o) == 2 and o[0] in REG and o[0] == o[1] and (o[0], 2) in shs:
            tests.append((m, sh, 'a', '%s %s,%s' % (m, REG[o[0]], REG[o[0]])))
            continue
        if len(o) != 3 or o[0] not in REG or o[0] != o[1]:
            continue
        for side, drop in (('a', 1), ('b', 2)):
            two = ','.join(x for k, x in enumerate(o) if k != drop)
            if (two, 2) not in shs:
                continue
            if o[2] not in OTHER or (side == 'b' and o[2] != o[0]):
                continue
            v = [REG[o[0]], REG[o[0]], OTHER[o[2]]] if side == 'a' else \
                [REG[o[0]], OTHER[o[1]], REG[o[0]]]
            tests.append((m, sh, side, '%s %s' % (m, ','.join(v))))

def one(t):
    m, sh, side, txt = t
    d = tempfile.mkdtemp(prefix='tnar')
    s, o = os.path.join(d, 'a.s'), os.path.join(d, 'a.o')
    open(s, 'w').write(' .text\nS: %s\nE: nop\n' % txt)
    r = subprocess.run([AS, '-mtc162', s, '-o', o], capture_output=True, text=True)
    if r.returncode:
        return t, None
    nm = subprocess.run([os.path.expanduser('~/cross/tricore/bin/tricore-elf-nm'), o],
                        capture_output=True, text=True).stdout
    a = {p[2]: int(p[0], 16) for p in (l.split() for l in nm.splitlines()) if len(p) == 3}
    return t, a['E'] - a['S']

import concurrent.futures
out = {}
with concurrent.futures.ThreadPoolExecutor(8) as ex:
    for (m, sh, side, txt), n in ex.map(one, tests):
        if n is not None:
            out.setdefault('%s %s' % (m, sh), {})[side] = n
json.dump(out, open('narrow.json', 'w'), indent=0)
for k, v in sorted(out.items()):
    print('%-20s %s' % (k, v))
