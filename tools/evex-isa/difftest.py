#!/usr/bin/env python3
"""EmbCC's AVX-512 against llvm-mc over every form: random registers
(0-31), masks and {z} where the form takes them, memory with random base,
index, scale and a displacement about the disp8*N boundaries, assembled
by llvm-mc (per statement, -show-encoding) and by `embcc -c`; each
statement's bytes compared (a nop marker between statements).

    difftest.py EMBCC [N per form] [seed]"""
import json, os, random, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from roles import asm, regname

EMBCC = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 2
random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
E = [e for e in json.load(open('evex.json')) if e['ok'] and e['mnem'][0] in 'vk']
GP = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi'] + ['r%d' % i for i in range(8, 16)]

def mem(e, vi=None):
    base = random.choice(GP)
    n = e.get('N', 1)
    disp = random.choice([0, n, -n, 127 * n, -128 * n, 128 * n, n + 1, 4096, -4])
    idx = ''
    if vi:
        idx = ',%%%s%d,%d' % (vi, random.randrange(16 if e['vex'] else 32), random.choice([1, 2, 4, 8]))
    elif random.random() < 0.3:
        i = random.choice([g for g in GP if g != 'rsp'])
        idx = ',%%%s,%d' % (i, random.choice([1, 2, 4, 8]))
    if random.random() < 0.1 and not vi:
        return '%d(%%rip)' % disp
    return '%d(%%%s%s)' % (disp, base, idx)

def stmt(e):
    parts = []
    toks = e['shape'].split(',')
    last = len(toks) - 1
    for k, t in enumerate(toks):
        m = re.match(r'^([ZYXKQDWB]|M[XYZ]?|I|\{RC\}|\{sae\})((?:\{[^}]*\})*)$', t)
        b, decs = m.group(1), m.group(2)
        if b == '{RC}':
            s = random.choice(['{rn-sae}', '{rd-sae}', '{ru-sae}', '{rz-sae}'])
        elif b == '{sae}':
            s = '{sae}'
        elif b.startswith('M'):
            s = mem(e, {'MX': 'xmm', 'MY': 'ymm', 'MZ': 'zmm'}.get(b))
        elif b == 'I':
            s = '$%d' % random.randrange(256)
        elif b in 'ZYX':
            s = regname(b, random.randrange(16 if e['vex'] else 32))
        elif b == 'K':
            s = regname('K', random.randrange(8))
        else:
            s = regname(b, random.randrange(16))
        for d in re.findall(r'\{[^}]*\}', decs):
            s += ' ' + (('{%%k%d}' % random.randrange(1, 8)) if d == '{K}' else d)
        if k == last and e['mask'] and random.random() < 0.6:
            s += ' {%%k%d}' % random.randrange(1, 8)
            if e['mask'] == 2 and random.random() < 0.5:
                s += ' {z}'
        parts.append(s)
    return '%s %s' % (e['mnem'], ', '.join(parts))

stmts = [stmt(e) for e in E for _ in range(N)]
ref = asm(stmts)
keep = [k for k, r in enumerate(ref) if r is not None]
src = ''.join('%s\nnop\n' % stmts[k] for k in keep)
open('/tmp/edt.s', 'w').write(src)
r = subprocess.run([EMBCC, '--target=x86_64-elf', '-c', '/tmp/edt.s', '-o', '/tmp/edt.o'],
                   capture_output=True, text=True)
if r.returncode:
    errs = r.stderr.strip().splitlines()
    print('EmbCC refused %d, e.g.:' % len(errs))
    for l in errs[:15]:
        print('  ', l[:220])
    sys.exit(1)
subprocess.run(['llvm-objcopy', '-O', 'binary', '-j', '.text', '/tmp/edt.o', '/tmp/edt.bin'], check=True)
b = list(open('/tmp/edt.bin', 'rb').read())
pos, diff = 0, 0
for k in keep:
    want = ref[k]
    got = b[pos:pos + len(want)]
    if got != want:
        diff += 1
        if diff <= 20:
            print('DIFF %-60s llvm %s embcc %s' % (stmts[k][:60], bytes(want).hex(), bytes(b[pos:pos + len(want) + 2]).hex()))
        # resync at the next nop
        while pos < len(b) and b[pos] != 0x90:
            pos += 1
        pos += 1
        continue
    pos += len(want)
    if pos < len(b) and b[pos] == 0x90:
        pos += 1
print('%d statements, llvm-mc took %d, %d differ' % (len(stmts), len(keep), diff))
