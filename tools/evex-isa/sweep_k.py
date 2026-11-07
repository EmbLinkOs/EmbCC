#!/usr/bin/env python3
"""The opmask instructions (kmov, kand, kshift, ...), VEX-encoded: the
3-byte VEX space of maps 1 and 3, each pp, W and L, every opcode, decoded
by llvm-mc as sweep.py decodes EVEX's; the forms whose mnemonic starts
with k are kept. Output: kforms.json."""
import concurrent.futures, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shapes import parse
import sweep

def cands():
    for mm in (1, 2, 3):
        for pp in range(4):
            for W in range(2):
                for L in range(2):
                    b1 = 0xe0 | mm                         # R X B set (none)
                    b2 = (W << 7) | (0xf << 3) | (L << 2) | pp
                    for op in range(256):
                        for reg in range(8):
                            yield [0xc4, b1, b2, op, 0xc0 | (reg << 3) | 1, 0x02]
                            yield [0xc4, b1, b2, op, 0x00 | (reg << 3), 0x02]

def run(batch):
    got = {}
    import re, subprocess, tempfile
    d = tempfile.mkdtemp(prefix='ek')
    p = os.path.join(d, 'in.txt')
    with open(p, 'w') as f:
        for c in batch:
            f.write(' '.join('0x%02x' % x for x in c + sweep.GUARD) + '\n')
    r = subprocess.run(['llvm-mc', '--disassemble', '-show-encoding', '-triple=x86_64',
                        '-mattr=+avx512f,+avx512bw,+avx512dq,+avx2,+fma,+f16c,+avxvnni,+avxvnniint8,'
                        '+avxvnniint16,+avxifma,+avxneconvert,+gfni,+vaes,+vpclmulqdq,+sm3,+sm4,+sha512', p],
                       capture_output=True, text=True)
    seg, k = [], 0
    for l in r.stdout.splitlines():
        m = re.match(r'^\t(\S+)(?:\t(.*?))?\s*# encoding: \[([^\]]*)\]', l)
        if not m:
            continue
        mn, ops, enc = m.group(1), (m.group(2) or '').strip(), m.group(3)
        if mn == 'ud2':
            if seg and k < len(batch):
                f0 = seg[0]
                bs = [int(x, 16) for x in f0[2].split(',') if x.startswith('0x')]
                if f0[0][0] in 'kv' and bs and bs[0] in (0xc4, 0xc5):
                    got.setdefault((f0[0], f0[1]), f0[1])
            seg = []
            k += 1
            continue
        seg.append((mn, ops, enc))
    return got

if __name__ == '__main__':
    allc = list(cands())
    batches = [allc[i:i + 20000] for i in range(0, len(allc), 20000)]
    seen = {}
    with concurrent.futures.ProcessPoolExecutor(8) as ex:
        for got in ex.map(run, batches):
            for (mn, ops), txt in got.items():
                p = parse(ops)
                if p:
                    seen.setdefault((mn, p[0]), txt)
    out = [{'mnem': k[0], 'shape': k[1], 'ops': v, 'vex': 1} for k, v in sorted(seen.items())]
    json.dump(out, open('kforms.json', 'w'), indent=0)
    print(len(out), 'k forms,', len({o['mnem'] for o in out}), 'mnemonics')
