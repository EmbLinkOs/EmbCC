#!/usr/bin/env python3
"""EmbCC's MVE against llvm-mc over every form: random field values (each a
value llvm-mc takes there) rendered as statements, assembled by both,
compared word for word. Every MVE instruction is one 32-bit Thumb word,
so statement k is word k once both took the same statements.

    difftest.py EMBCC [N per form] [seed] [syn]   (syn: also the type
                                                    synonyms' spellings)"""
import json, os, random, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import assemble
from constrain import render
from check import fit

EMBCC = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 3
random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
SYN = len(sys.argv) > 4
SKIP = re.compile(r'^(vpt|vpst)')
T = [e for e in json.load(open('isa2.json')) if e.get('base_ok') and not SKIP.match(e['mnem'])]
syn = {}
for k, v in json.load(open('synonyms.json')).items():
    syn.setdefault(v, []).append(k)
stmts = []
for e in T:
    for t in range(N):
        vals = []
        for f in e['fields']:
            ok = f.get('ok', 'all')
            if isinstance(ok, list) and ok:
                vals.append(random.choice(ok))
            else:
                v = f['v0']
                for b, wt in f['bits']:
                    if random.random() < 0.5:
                        v += wt
                vals.append(v)
        if fit(e, vals) is None:
            continue
        s = render(e['mnem'], e['shape'], vals)
        if SYN and syn.get(e['mnem']) and random.random() < 0.5:
            s = s.replace(e['mnem'], random.choice(syn[e['mnem']]), 1)
        stmts.append(s)
ref = assemble(stmts)
keep = [k for k, w in enumerate(ref) if w is not None]
src = ' .syntax unified\n .thumb\n .text\n' + ''.join(' %s\n' % stmts[k] for k in keep)
open('/tmp/mdt.s', 'w').write(src)
r = subprocess.run([EMBCC, '--target=thumbv8m.main-none-eabihf', '-mcpu=cortex-m55', '-c',
                    '/tmp/mdt.s', '-o', '/tmp/mdt.o'],
                   capture_output=True, text=True)
if r.returncode:
    errs = r.stderr.strip().splitlines()
    print('EmbCC refused %d statements, e.g.:' % len(errs))
    for l in errs[:12]:
        print('  ', l[:200])
    sys.exit(1)
subprocess.run(['llvm-objcopy', '-O', 'binary', '-j', '.text', '/tmp/mdt.o', '/tmp/mdt.bin'], check=True)
b = open('/tmp/mdt.bin', 'rb').read()
ours = [(b[4 * k] | b[4 * k + 1] << 8) << 16 | (b[4 * k + 2] | b[4 * k + 3] << 8)
        for k in range(len(b) // 4)]
diff = 0
for n, k in enumerate(keep):
    if n >= len(ours) or ours[n] != ref[k]:
        diff += 1
        if diff <= 20:
            print('DIFF %-48s llvm %08x embcc %s' % (stmts[k], ref[k],
                  '%08x' % ours[n] if n < len(ours) else '-'))
print('%d statements, llvm-mc took %d, %d words, %d differ' % (len(stmts), len(keep), len(ours), diff))
