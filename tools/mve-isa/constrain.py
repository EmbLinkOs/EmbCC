#!/usr/bin/env python3
"""Which values each field takes, by llvm-mc's assembler.

A filling that round-trips (check.py's test) is each entry's base; each
field in turn takes candidate values -- every register; a number's ends,
zero, powers of two -- and the statement written from the shape is
assembled by llvm-mc. A value is the field's when llvm-mc takes the
statement and writes the word `fit` makes. Output: isa2.json, each field
with `ok`: 'all' or the values taken."""
import json, os, random, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import assemble
from learn import vfp_imm
from check import fit

def render(mnem, shape, vals):
    """the statement for these field values (the learner's linear ones)"""
    out, k, i = [], 0, 0
    def reg(kind, v):
        return {'Q': 'q', 'D': 'd', 'S': 's'}.get(kind, 'r') + str(v) if kind != 'R' else \
            {13: 'sp', 14: 'lr', 15: 'pc'}.get(v, 'r%d' % v)
    while i < len(shape):
        c = shape[i]
        if c == '{':
            j = shape.index('}', i)
            m = re.match(r'([QDS])(\d+)(s?)(\[\]|\[N\])?$', shape[i + 1:j])
            kind, n, sp, lane = m.group(1), int(m.group(2)), m.group(3), m.group(4)
            first = vals[k]; k += 1
            ln = ''
            if lane == '[]':
                ln = '[]'
            elif lane == '[N]':
                ln = '[%d]' % vals[k]; k += 1
            step = 2 if sp else 1
            out.append('{' + ', '.join(reg(kind, first + t * step) + ln for t in range(n)) + '}')
            i = j + 1
            continue
        if c in 'QDSR' and not shape[i:].startswith(('R!', 'R]')) or c in 'QDSR':
            v = vals[k]; k += 1
            if shape[i + 1:i + 4] == '[N]':
                out.append('%s[%d]' % (reg(c, v), vals[k])); k += 1
                i += 4
            else:
                out.append(reg(c, v))
                i += 1
            continue
        if shape.startswith('#N', i):
            v = vals[k]; k += 1
            if mnem == 'vmov.i64':
                v = sum(0xff << (8 * j) for j in range(8) if (v >> j) & 1)
            out.append('#%d' % v if v < 1 << 31 else '#0x%x' % v)
            i += 2
            continue
        if shape.startswith('#F', i):
            v = vals[k]; k += 1
            out.append('#%r' % vfp_imm(v))
            i += 2
            continue
        if c == 'N':
            out.append(str(vals[k])); k += 1
            i += 1
            continue
        m = re.match(r'(apsr_nzcv|fpscr|fpexc|fpsid|mvfr[012]|fpinst2?|zr|uxtw)', shape[i:])
        if m:
            out.append(m.group(1) + (' ' if m.group(1) == 'uxtw' else ''))
            i += len(m.group(1))
            continue
        if c == 'C':                    # (MVE's comparison: its condition)
            names = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls',
                     'ge', 'lt', 'gt', 'le']
            v = vals[k]; k += 1
            out.append(names[v] if 0 <= v < len(names) else 'c%d' % v)
            i += 1
            continue
        out.append(c)
        i += 1
    return '%s %s' % (mnem, ''.join(out).replace(',', ', '))

def cands(f):
    lo = f['v0'] + sum(b[1] for b in f['bits'] if b[1] < 0)
    hi = f['v0'] + sum(b[1] for b in f['bits'] if b[1] > 0)
    step = min((abs(b[1]) for b in f['bits']), default=1)
    if f['kind'] in 'QDSR' or hi - lo <= 64 * step:
        return list(range(lo, hi + 1, step))
    return sorted({lo, hi, f['v0'], lo + step, hi - step} | {f['v0'] + b[1] for b in f['bits']})

def main():
    T = json.load(open('isa.json'))
    random.seed(5)
    jobs = []      # (entry, field, value, vals)
    for ei, e in enumerate(T):
        # every register at its value with no bit set (the lowest, which
        # every list fits); an immediate one step up -- a zero immediate
        # is the first form's, whatever form this is
        # (an immediate with only negative weights -- the subtracting form
        # of a load's offset -- one step below zero: #0 is the adding one's)
        base = [f['v0'] + ((min((b[1] for b in f['bits'] if b[1] > 0), default=0) or
                            max((b[1] for b in f['bits'] if b[1] < 0), default=0))
                           if f['kind'] in 'NF' else 0) for f in e['fields']]
        # (MVE's: registers that must differ -- `Qd and Qn can't be
        # identical` -- so each register field its own, where it fits)
        used = {}
        for fi, f in enumerate(e['fields']):
            if f['kind'] in 'QR':
                for v in cands(f):
                    if v not in used.get(f['kind'], set()) and (f['kind'] != 'R' or v < 13):
                        base[fi] = v
                        used.setdefault(f['kind'], set()).add(v)
                        break
        jobs.append((ei, -1, None, base))
        for fi, f in enumerate(e['fields']):
            for v in cands(f):
                vals = list(base); vals[fi] = v
                jobs.append((ei, fi, v, vals))
    stmts, words = [], []
    for ei, fi, v, vals in jobs:
        e = T[ei]
        w = fit(e, vals)
        words.append(w)
        stmts.append(render(e['mnem'], e['shape'], vals) if w is not None else 'nop')
    got = []
    for c in range(0, len(stmts), 20000):
        got += assemble(stmts[c:c + 20000])
    ok, base_ok = {}, {}
    for (ei, fi, v, vals), w, g in zip(jobs, words, got):
        good = w is not None and g == w
        if fi < 0:
            base_ok[ei] = good
        else:
            ok.setdefault((ei, fi), {})[v] = good
    for ei, e in enumerate(T):
        e['base_ok'] = base_ok.get(ei, False)
        for fi, f in enumerate(e['fields']):
            r = ok.get((ei, fi), {})
            acc = sorted(v for v, g in r.items() if g)
            rej = [v for v, g in r.items() if not g]
            # (a number's candidates are a sample unless the field is small:
            # its refusals then mean another form's value, which the rank
            # order settles, not a value this field lacks)
            full = f['kind'] in 'QDSR' or len(cands(f)) == len(set(range(
                min(cands(f), default=0), max(cands(f), default=0) + 1,
                min((abs(b[1]) for b in f['bits']), default=1))))
            f['ok'] = 'all' if not rej or not full else acc
    json.dump(T, open('isa2.json', 'w'))
    nb = sum(1 for e in T if not e['base_ok'])
    print(len(T), 'entries;', nb, 'with a base llvm-mc does not take as fitted')
    for e in [e for e in T if not e['base_ok']][:12]:
        print('   ', e['mnem'], e['shape'])

if __name__ == '__main__':
    main()
