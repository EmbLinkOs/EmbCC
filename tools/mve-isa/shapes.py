#!/usr/bin/env python3
"""MVE operand text -> (shape, atoms). (tools/neon-isa's reading, with
MVE's own: a vector base `[q1, #8]`, a vector offset `[r0, q1, uxtw #2]`,
a comparison's condition `eq` as C, `zr`.)

A shape is the operands with each register reduced to its file -- Q, D,
S, R (a core register) -- each number to N, a register list to one atom
(its first register, with the count in the shape: `{D3}`, spaced `{D2s}`,
all lanes `{D2[]}`, one lane `{D2[N]}`), memory to `[R]`, `[R:128]`,
`[R]!`, `[R], R`. Atoms are the values, in order: a list is ONE atom, so
the learner sees one operand move when the first register's bits flip."""
import re

REG = r'(?:q(?:1[0-5]|[0-9])|d(?:3[01]|[12][0-9]|[0-9])|s(?:3[01]|[12][0-9]|[0-9])|r(?:1[0-2]|[0-9])|sp|lr|pc|ip|fp|sb|sl)'

def reg_kind(r):
    if r[0] == 'q': return 'Q', int(r[1:])
    if r[0] == 'd': return 'D', int(r[1:])
    if r[0] == 's' and r not in ('sp', 'sb', 'sl'): return 'S', int(r[1:])
    named = {'sp': 13, 'lr': 14, 'pc': 15, 'ip': 12, 'fp': 11, 'sb': 9, 'sl': 10}
    if r in named: return 'R', named[r]
    return 'R', int(r[1:])

def parse(ops):
    """(shape, atoms) or None. atoms: list of (kind, value)."""
    shape, atoms, i = [], [], 0
    s = ops.replace(' ', '')
    while i < len(s):
        c = s[i]
        if c == '{':
            j = s.index('}', i)
            body = s[i + 1:j]
            regs = [x for x in body.split(',') if x]
            items = []
            for x in regs:
                m = re.match(r'^(d(?:3[01]|[12][0-9]|[0-9])|q(?:1[0-5]|[0-9])|s(?:3[01]|[12][0-9]|[0-9]))(?:\[(\d*)\])?$', x)
                if m:
                    items.append((m.group(1), m.group(2)))
                    continue
                m = re.match(r'^([ds])(\d+)-[ds](\d+)$', x)
                if m:
                    a, b = int(m.group(2)), int(m.group(3))
                    items += [('%s%d' % (m.group(1), k), None) for k in range(a, b + 1)]
                    continue
                return None
            if not items:
                return None
            k0, v0 = reg_kind(items[0][0])
            vals = [reg_kind(r)[1] for r, _ in items]
            step = vals[1] - vals[0] if len(vals) > 1 else 1
            if any(vals[t + 1] - vals[t] != step for t in range(len(vals) - 1)):
                return None
            lane = items[0][1]
            tag = '{%s%d%s%s}' % (k0, len(items), 's' if step == 2 else '',
                                 '' if lane is None else ('[]' if lane == '' else '[N]'))
            shape.append(tag)
            atoms.append((k0, v0))
            if lane not in (None, ''):
                atoms.append(('N', int(lane)))
            i = j + 1
            continue
        if c in '[],!:':
            # an alignment: part of the shape
            m = re.match(r'^:(\d+)', s[i:])
            if m:
                shape.append(':' + m.group(1))
                i += len(m.group(0))
                continue
            shape.append(c)
            i += 1
            continue
        if c == '#':
            m = re.match(r'^#(-?0x[0-9a-f]+|-?\d+(?:\.\d+)?(?:e[+-]?\d+)?)', s[i:])
            if not m:
                return None
            t = m.group(1)
            if re.search(r'[.e]', t) and not t.startswith(('0x', '-0x')):
                shape.append('#F')
                atoms.append(('F', float(t)))
            else:
                shape.append('#N')
                atoms.append(('N', int(t, 0)))
            i += len(m.group(0))
            continue
        m = re.match('^(' + REG + r')(\[(\d+)\])?(?![0-9a-z])', s[i:])
        if m:
            k, v = reg_kind(m.group(1))
            atoms.append((k, v))
            if m.group(2):
                shape.append(k + '[N]')
                atoms.append(('N', int(m.group(3))))
            else:
                shape.append(k)
            i += len(m.group(0))
            continue
        m = re.match(r'^(-?0x[0-9a-f]+|-?\d+)', s[i:])
        if m:
            shape.append('N')
            atoms.append(('N', int(m.group(1), 0)))
            i += len(m.group(0))
            continue
        m = re.match(r'^(apsr_nzcv|fpscr|fpexc|fpsid|mvfr[012]|fpinst2?|uxtw|zr)(?![0-9a-z])', s[i:])
        if m:
            shape.append(m.group(1))
            i += len(m.group(0))
            continue
        m = re.match(r'^(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)(?![0-9a-z])', s[i:])
        if m:
            cc = {'eq':0,'ne':1,'cs':2,'hs':2,'cc':3,'lo':3,'mi':4,'pl':5,'vs':6,'vc':7,
                  'hi':8,'ls':9,'ge':10,'lt':11,'gt':12,'le':13}[m.group(1)]
            shape.append('C')
            atoms.append(('C', cc))
            i += len(m.group(0))
            continue
        return None
    return ''.join(shape), atoms

if __name__ == '__main__':
    for t in ['q0, q1, q2', 'q0, [r0, #16]!', 'q1, [q2, #-8]', 'q0, [r1, q2, uxtw #2]', '{q0, q1}, [r0]!',
              'eq, q0, q1', 'r0, r1, q2[3], q2[1]', 'q0, #1.000000e+00', 'lr, r2', 'q0, [r0], #-32']:
        print('%-28s %s' % (t, parse(t)))
