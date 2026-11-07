#!/usr/bin/env python3
"""What each EVEX form is, from llvm-mc's assembler.

For each (mnemonic, shape) of forms.json, its statement is written with a
distinct register in every register operand (numbers above 15, so R',
V' and X are in play), assembled, and the EVEX bytes read back: the map,
pp, W, L'L and b, the opcode, the ModRM reg field when it is an opcode
extension, and which field -- ModRM reg, ModRM rm, vvvv, aaa -- holds each
operand. A memory form's displacement scale (disp8*N) is the largest N
whose `N(%rax)` llvm-mc writes as a disp8 of 1. And whether the form takes
a mask ({%k1}) and zeroing ({z}). Output: evex.json."""
import json, os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shapes import parse, split_ops

MATTR = ('-mattr=+avx2,+fma,+f16c,+avxvnni,+avxvnniint8,+avxvnniint16,+avxifma,+avxneconvert,+sm3,+sm4,+sha512,+avx512f,+avx512bw,+avx512dq,+avx512vl,+avx512cd,+avx512vbmi,+avx512vbmi2,'
         '+avx512ifma,+avx512vnni,+avx512bitalg,+avx512vpopcntdq,+avx512bf16,+avx512fp16,+gfni,'
         '+vaes,+vpclmulqdq,+avx512vp2intersect')

def asm(stmts):
    """per statement its bytes, or None"""
    out = [None] * len(stmts)
    keep = list(range(len(stmts)))
    d = tempfile.mkdtemp(prefix='er')
    for rnd in range(40):
        p = os.path.join(d, 'a.s')
        with open(p, 'w') as f:
            for i in keep:
                f.write('%s\nud2\n' % stmts[i])
        r = subprocess.run(['llvm-mc', '-show-encoding', '-triple=x86_64', MATTR, p],
                           capture_output=True, text=True)
        bad = {keep[(int(m.group(1)) - 1) // 2] for m in re.finditer(r'a\.s:(\d+):\d+: error', r.stderr)
               if int(m.group(1)) <= 2 * len(keep)}
        if not bad:
            k, cur = 0, []
            for l in r.stdout.splitlines():
                m = re.search(r'encoding: \[([^\]]*)\]', l)
                if not m:
                    continue
                bs = [int(x, 16) if x.startswith('0x') else 0 for x in m.group(1).split(',')]
                if bs == [0x0f, 0x0b]:
                    if k < len(keep) and len(cur) == 1:
                        out[keep[k]] = cur[0]
                    k += 1
                    cur = []
                else:
                    cur.append(bs)
            return out
        keep = [i for i in keep if i not in bad]
    return out

NAMES = {'Q': ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi'] + ['r%d' % i for i in range(8, 16)],
         'D': ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'] + ['r%dd' % i for i in range(8, 16)],
         'W': ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di'] + ['r%dw' % i for i in range(8, 16)],
         'B': ['al', 'cl', 'dl', 'bl', 'spl', 'bpl', 'sil', 'dil'] + ['r%db' % i for i in range(8, 16)]}

def regname(c, n):
    if c in 'ZYX':
        return '%%%smm%d' % ({'Z': 'z', 'Y': 'y', 'X': 'x'}[c], n)
    if c == 'K':
        return '%%k%d' % n
    return '%' + NAMES[c][n]

def render(mn, shape, picks, mem='(%rax)', mask=None, zero=False):
    """the statement: register operands numbered by picks, in order"""
    parts, k = [], 0
    for t in (shape.split(',') if shape else []):
        m = re.match(r'^([ZYXKQDWB]|M[XYZ]?|I|\{RC\}|\{sae\})((?:\{[^}]*\})*)$', t)
        base, decs = m.group(1), m.group(2)
        if base in ('MX', 'MY', 'MZ'):
            # a gather's memory: (%rax) indexed by a vector register
            vi = {'MX': 'xmm', 'MY': 'ymm', 'MZ': 'zmm'}[base]
            s = mem.replace(')', ',%%%s%d)' % (vi, picks[-1] if False else 9)) if '(' in mem else mem
            for d in re.findall(r'\{[^}]*\}', decs):
                s += ' ' + d.replace('{K}', '{%k1}')
            parts.append(s)
            continue
        if base == '{RC}':
            s = '{rn-sae}'
        elif base == '{sae}':
            s = '{sae}'
        elif base == 'M':
            s = mem
        elif base == 'I':
            s = '$3'
        else:
            s = regname(base, picks[k]); k += 1
        for d in re.findall(r'\{[^}]*\}', decs):
            if d == '{K}':
                s += ' {%%k%d}' % (mask if mask else 1)
            elif d == '{z}':
                s += ' {z}'
            else:
                s += ' ' + d
        parts.append(s)
    return '%s %s' % (mn, ', '.join(parts))

def fields(bs):
    """the fields of an EVEX or VEX encoding"""
    if bs[0] == 0xc5:                       # 2-byte VEX: map 1, W 0, no X/B
        R = (~bs[1] >> 7) & 1
        modrm = bs[3]
        return dict(enc=2, map=1, pp=bs[1] & 3, W=0, LL=(bs[1] >> 2) & 1, b=0, op=bs[2],
                    modrm=modrm, reg=((modrm >> 3) & 7) | (R << 3), rm=modrm & 7,
                    vvvv=(~bs[1] >> 3) & 15, aaa=0, z=0, n=len(bs), opat=2)
    if bs[0] == 0xc4:                       # 3-byte VEX
        R = (~bs[1] >> 7) & 1; X = (~bs[1] >> 6) & 1; B = (~bs[1] >> 5) & 1
        modrm = bs[4]
        return dict(enc=3, map=bs[1] & 31, pp=bs[2] & 3, W=bs[2] >> 7, LL=(bs[2] >> 2) & 1,
                    b=0, op=bs[3], modrm=modrm, reg=((modrm >> 3) & 7) | (R << 3),
                    rm=(modrm & 7) | (B << 3), vvvv=(~bs[2] >> 3) & 15, aaa=0, z=0,
                    n=len(bs), opat=3)
    p0, p1, p2 = bs[1], bs[2], bs[3]
    modrm = bs[5]
    R = (~p0 >> 7) & 1; X = (~p0 >> 6) & 1; B = (~p0 >> 5) & 1; Rp = (~p0 >> 4) & 1
    reg = ((modrm >> 3) & 7) | (R << 3) | (Rp << 4)
    rm = (modrm & 7) | (B << 3) | (X << 4)
    vvvv = ((~p1 >> 3) & 15) | (((~p2 >> 3) & 1) << 4)
    return dict(enc=4, map=p0 & 7, pp=p1 & 3, W=p1 >> 7, LL=(p2 >> 5) & 3, b=(p2 >> 4) & 1, op=bs[4],
                modrm=modrm, reg=reg, rm=rm, vvvv=vvvv, aaa=p2 & 7, z=p2 >> 7, n=len(bs), opat=4)

def main():
    F = [f for f in json.load(open('forms.json')) if not f['shape'].startswith('?')]
    F += json.load(open('kforms.json'))
    # registers per form, in order (not counting the {K} decoration)
    def regs_of(shape):
        return [t[0] for t in shape.split(',') if t and t[0] in 'ZYXKQDWB']
    # 1. two fillings with distinct registers, to tell the fields apart
    picks_a, picks_b, stmts = [], [], []
    for f in F:
        rs = regs_of(f['shape'])
        # (a VEX form's registers below 16: VEX has no more, and higher
        # ones would make llvm-mc write the EVEX form instead)
        vx = f.get('vex')
        pa = [([9, 3, 14, 6] if vx else [17, 26, 11, 4])[i] if c in 'ZYX' else [2, 5, 6, 3][i]
              if c == 'K' else [9, 3, 14, 6][i] for i, c in enumerate(rs)]
        pb = [([12, 6, 1, 10] if vx else [22, 7, 29, 13])[i] if c in 'ZYX' else [3, 6, 7, 2][i]
              if c == 'K' else [12, 6, 1, 10][i] for i, c in enumerate(rs)]
        picks_a.append(pa); picks_b.append(pb)
        stmts.append(render(f['mnem'], f['shape'], pa, mem='(%rax)'))
        stmts.append(render(f['mnem'], f['shape'], pb, mem='(%rax)'))
    got = asm(stmts)
    out = []
    for i, f in enumerate(F):
        a, b = got[2 * i], got[2 * i + 1]
        if not a or not b or a[0] not in (0x62, 0xc4, 0xc5) or b[0] != a[0] and \
                not ({a[0], b[0]} <= {0xc4, 0xc5}):
            continue
        # (no ModRM -- vzeroupper -- is the hand-written encoder's)
        need = 4 if a[0] == 0xc5 else 5 if a[0] == 0xc4 else 6
        if len(a) < need or len(b) < (4 if b[0] == 0xc5 else 5 if b[0] == 0xc4 else 6):
            continue
        fa, fb = fields(a), fields(b)
        rs = regs_of(f['shape'])
        roles = []
        for k, c in enumerate(rs):
            va, vb = picks_a[i][k], picks_b[i][k]
            role = None
            for fld in ('reg', 'rm', 'vvvv'):
                if fa[fld] == va and fb[fld] == vb:
                    role = fld
                    break
            if role is None and c == 'K' and fa['aaa'] == va and fb['aaa'] == vb:
                role = 'aaa'
            roles.append(role)
        has_mem = 'M' in f['shape']
        vsib = any(t[:2] in ('MX', 'MY', 'MZ') for t in f['shape'].split(','))
        if has_mem:
            roles_mem = 'rm'
        # the reg field fixed: an opcode extension
        regext = -1 if 'reg' in roles else (fa['modrm'] >> 3) & 7
        # an immediate byte the shape does not show: a predicate in the
        # mnemonic (vpcmpleuq is vpcmpuq $2)
        body = fa['opat'] + 2               # past the opcode and ModRM
        mrm = fa['modrm']
        if (mrm >> 6) != 3 and (mrm & 7) == 4:
            body += 1                       # a SIB (a gather's index)
        body += {1: 1, 2: 4}.get(mrm >> 6, 0)
        fixed = a[body] if fa['n'] == body + 1 and 'I' not in f['shape'].split(',') else -1
        e = dict(mnem=f['mnem'], shape=f['shape'], vex=1 if fa['enc'] != 4 else 0,
                 map=fa['map'], pp=fa['pp'], W=fa['W'],
                 LL=fa['LL'], b=fa['b'], op=fa['op'], regext=regext, roles=roles,
                 imm='I' in f['shape'].split(','), fixed=fixed, mem=has_mem)
        e['ok'] = None not in roles and (not has_mem or 'rm' not in roles) and \
            (has_mem or 'rm' in roles or len(rs) == 0)
        out.append(e)
    # 2. memory forms: disp8*N, and masking
    mem = [e for e in out if e['mem'] and e['ok'] and not e['vex']]
    stmts = []
    for e in mem:
        rs = regs_of(e['shape'])
        # (vector registers past 15: EVEX's alone, so llvm-mc cannot take
        # the shorter VEX form, whose displacement is never scaled)
        p = [[17, 18, 19, 20][k] if c in 'ZYX' else [1, 2, 3, 4][k] for k, c in enumerate(rs)]
        for n in (1, 2, 4, 8, 16, 32, 64):
            stmts.append(render(e['mnem'], e['shape'], p, mem='%d(%%rax)' % n))
    got = asm(stmts)
    for j, e in enumerate(mem):
        best = 1
        for t, n in enumerate((1, 2, 4, 8, 16, 32, 64)):
            bs = got[7 * j + t]
            # disp8 of 1: ModRM mod 01 and the byte after ModRM (no SIB with rax) is 1
            at = 7 if bs and len(bs) > 6 and (bs[5] & 7) == 4 else 6     # (past a SIB)
            if bs and len(bs) > at and bs[0] == 0x62 and (bs[5] >> 6) == 1 and bs[at] == 1:
                best = n
        e['N'] = best
    # 3. masking: {%k1} on the destination, and {z}
    stmts = []
    cand = [e for e in out if e['ok'] and '{K}' not in e['shape'] and not e['vex']]
    for e in cand:
        rs = regs_of(e['shape'])
        p = [[1, 2, 3, 4][k] for k in range(len(rs))]
        s = render(e['mnem'], e['shape'], p)
        stmts.append(s + ' {%k1}')
        stmts.append(s + ' {%k1} {z}')
    got = asm(stmts)
    for j, e in enumerate(cand):
        e['mask'] = 2 if got[2 * j + 1] else 1 if got[2 * j] else 0
    for e in out:
        e.setdefault('mask', 0)
    json.dump(out, open('evex.json', 'w'), indent=0)
    ok = [e for e in out if e['ok']]
    print(len(F), 'forms;', len(out), 'assembled;', len(ok), 'with every operand placed;',
          sum(1 for e in ok if e['mask']), 'take a mask,', sum(1 for e in ok if e['mask'] == 2), 'zeroing')
    for e in [e for e in out if not e['ok']][:10]:
        print('   unplaced:', e['mnem'], e['shape'], e['roles'])

if __name__ == '__main__':
    main()
