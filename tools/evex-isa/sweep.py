#!/usr/bin/env python3
"""Every EVEX (AVX-512) form llvm-mc decodes: for each opcode map (1, 2, 3,
5, 6), prefix (pp), W, vector length (L'L), opcode byte, b bit, and ModRM
-- the register form with each /digit, and (%rax) memory with each -- the
bytes 62 P0 P1 P2 op modrm imm8 are decoded by llvm-mc, followed by
fifteen nops and a ud2: whatever a candidate leaves undecoded cannot reach
the marker, so each decode is its own. Output: forms.json, an example per
(mnemonic, shape)."""
import concurrent.futures, json, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shapes import parse

GUARD = [0x90] * 15 + [0x0f, 0x0b]

def cmp_cands():
    # the comparisons, whose predicate (the immediate) llvm-mc prints in the
    # mnemonic: vcmp (map 1 C2, map 5 C2), vpcmp/vpcmpu (map 3 1E 1F 3E 3F)
    for mmm, ops in ((1, (0xc2,)), (5, (0xc2,)), (3, (0x1e, 0x1f, 0x3e, 0x3f))):
        for pp in range(4):
            for W in range(2):
                for LL in range(3):
                    for b in range(2):
                        p0 = 0xf0 | mmm
                        p1 = (W << 7) | (0xf << 3) | 0x4 | pp
                        p2 = (LL << 5) | (b << 4) | 0x8
                        for op in ops:
                            for imm in range(32):
                                yield [0x62, p0, p1, p2, op, 0xc8 | 1, imm]
                                yield [0x62, p0, p1, p2, op, 0x08, imm]

def vsib_cands():
    # gathers and scatters (map 2 90-93, A0-A3, C6-C7): a mask is required,
    # and the memory operand's index is a vector register (VSIB, SIB byte)
    for pp in range(4):
        for W in range(2):
            for LL in range(3):
                p0 = 0xf0 | 2
                p1 = (W << 7) | (0xf << 3) | 0x4 | pp
                p2 = (LL << 5) | 0x8 | 1                    # {%k1}
                for op in list(range(0x90, 0x94)) + list(range(0xa0, 0xa4)) + [0xc6, 0xc7]:
                    for reg in range(8):
                        yield [0x62, p0, p1, p2, op, (reg << 3) | 4, 0x08, 0x02]

def cands():
    for mmm in (1, 2, 3, 5, 6):
        for pp in range(4):
            for W in range(2):
                for LL in range(3):
                    for b in range(2):
                        p0 = 0xf0 | mmm               # R X B R' set (inverted: none)
                        p1 = (W << 7) | (0xf << 3) | 0x4 | pp
                        p2 = (LL << 5) | (b << 4) | 0x8 | 0x0   # V' set, no mask
                        for op in range(256):
                            for reg in range(8):
                                yield [0x62, p0, p1, p2, op, 0xc0 | (reg << 3) | 1, 0x02]
                                yield [0x62, p0, p1, p2, op, 0x00 | (reg << 3), 0x02]

def run(batch):
    d = tempfile.mkdtemp(prefix='ev')
    p = os.path.join(d, 'in.txt')
    with open(p, 'w') as f:
        for c in batch:
            f.write(' '.join('0x%02x' % x for x in c + GUARD) + '\n')
    r = subprocess.run(['llvm-mc', '--disassemble', '-show-encoding', '-triple=x86_64',
                        '-mattr=+avx512f,+avx512bw,+avx512dq,+avx512vl,+avx512cd,+avx512vbmi,'
                        '+avx512vbmi2,+avx512ifma,+avx512vnni,+avx512bitalg,+avx512vpopcntdq,'
                        '+avx512bf16,+avx512fp16,+gfni,+vaes,+vpclmulqdq,+avx512vp2intersect', p],
                       capture_output=True, text=True)
    out = {}
    seg = []
    k = 0
    for l in r.stdout.splitlines():
        m = re.match(r'^\t(\S+)(?:\t(.*?))?\s*# encoding: \[([^\]]*)\]', l)
        if not m:
            continue
        mn, ops, enc = m.group(1), (m.group(2) or '').strip(), m.group(3)
        if mn == 'ud2':
            if seg and k < len(batch):
                first = seg[0]
                bs = [int(x, 16) for x in first[2].split(',') if x.startswith('0x')]
                c = batch[k]
                if bs and bs[0] == 0x62 and bs == c[:len(bs)]:
                    out.setdefault((first[0], first[1]), (c[:len(bs)], first[1]))
            seg = []
            k += 1
            continue
        seg.append((mn, ops, enc))
    return out

if __name__ == '__main__':
    allc = list(cands()) + list(cmp_cands()) + list(vsib_cands())
    batches = [allc[i:i + 20000] for i in range(0, len(allc), 20000)]
    seen = {}
    with concurrent.futures.ProcessPoolExecutor(8) as ex:
        for got in ex.map(run, batches):
            for (mn, ops), (bs, txt) in got.items():
                p = parse(ops)
                key = (mn, p[0] if p else '?' + ops)
                seen.setdefault(key, (bs, txt))
    out = [{'mnem': k[0], 'shape': k[1], 'bytes': v[0], 'ops': v[1]} for k, v in sorted(seen.items())]
    json.dump(out, open('forms.json', 'w'), indent=0)
    bad = [o for o in out if o['shape'].startswith('?')]
    print(len(allc), 'candidates;', len(out), 'forms,', len({o['mnem'] for o in out}),
          'mnemonics,', len(bad), 'unparsed')
    for o in bad[:12]:
        print('  ', o['mnem'], o['ops'])
