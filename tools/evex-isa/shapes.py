#!/usr/bin/env python3
"""AT&T operand text -> (shape, atoms). Registers by class -- Z (zmm),
Y (ymm), X (xmm), K (k mask), Q/D/W/B (64/32/16/8-bit general) -- an
immediate $N as I, memory disp(base,index,scale) as M (with a broadcast,
M{1to16}); decorations kept: {%k1} as {K}, {z}, a rounding {rn-sae} as
{RC}, {sae}. Atoms: (class, number) for registers, ('M', text), ('I', v),
('RC', mode)."""
import re

GPR = {}
for i, n in enumerate(['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di']):
    GPR['r' + n] = ('Q', i); GPR['e' + n] = ('D', i); GPR[n] = ('W', i)
for i, n in enumerate(['al', 'cl', 'dl', 'bl', 'spl', 'bpl', 'sil', 'dil']):
    GPR[n] = ('B', i)
for i in range(8, 16):
    GPR['r%d' % i] = ('Q', i); GPR['r%dd' % i] = ('D', i)
    GPR['r%dw' % i] = ('W', i); GPR['r%db' % i] = ('B', i)

def reg(t):
    t = t.lstrip('%')
    m = re.match(r'^(zmm|ymm|xmm)(\d+)$', t)
    if m:
        return ({'zmm': 'Z', 'ymm': 'Y', 'xmm': 'X'}[m.group(1)], int(m.group(2)))
    m = re.match(r'^k([0-7])$', t)
    if m:
        return ('K', int(m.group(1)))
    return GPR.get(t)

RC = {'rn-sae': 0, 'rd-sae': 1, 'ru-sae': 2, 'rz-sae': 3}

def split_ops(s):
    out, depth, cur = [], 0, ''
    for c in s:
        if c == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
            continue
        if c in '({': depth += 1
        if c in ')}': depth -= 1
        cur += c
    if cur.strip():
        out.append(cur.strip())
    return out

def parse(ops):
    shape, atoms = [], []
    for o in split_ops(ops):
        tail = ''
        # decorations after the operand: {%k1} {z} {1to16}
        decs = re.findall(r'\{([^}]*)\}', o)
        core = re.sub(r'\s*\{[^}]*\}', '', o).strip()
        if not core and len(decs) == 1:            # {rn-sae} / {sae} alone
            d = decs[0]
            if d in RC:
                shape.append('{RC}'); atoms.append(('RC', RC[d]))
                continue
            if d == 'sae':
                shape.append('{sae}')
                continue
            return None
        r = reg(core) if core.startswith('%') else None
        if r:
            s = r[0]
            atoms.append(r)
        elif core.startswith('$'):
            try:
                v = int(core[1:], 0)
            except ValueError:
                return None
            s = 'I'
            atoms.append(('I', v))
        elif re.match(r'^-?(0x[0-9a-f]+|\d+)?\(', core) or re.match(r'^-?(0x[0-9a-f]+|\d+)$', core):
            # a vector index (VSIB: a gather's, a scatter's) is the shape's:
            # MX, MY, MZ by the index's size
            vi = re.search(r',\s*%(xmm|ymm|zmm)\d+', core)
            s = 'M' + {'xmm': 'X', 'ymm': 'Y', 'zmm': 'Z'}[vi.group(1)] if vi else 'M'
            atoms.append(('M', core))
        else:
            return None
        for d in decs:
            m = re.match(r'^%k([0-7])$', d)
            if m:
                s += '{K}'; atoms.append(('K', int(m.group(1))))
            elif d == 'z':
                s += '{z}'
            elif re.match(r'^1to\d+$', d):
                s += '{' + d + '}'
            else:
                return None
        shape.append(s)
    return ','.join(shape), atoms

if __name__ == '__main__':
    for t in ['%zmm2, %zmm1, %zmm0', '(%rax){1to16}, %zmm1, %zmm2 {%k1} {z}', '{rn-sae}, %zmm2, %zmm1, %zmm0',
              '$17, %zmm2, %zmm1, %zmm0', '%k1, %k2', '%eax, %zmm0', '16(%rip), %xmm1']:
        print('%-44s %s' % (t, parse(t)))
