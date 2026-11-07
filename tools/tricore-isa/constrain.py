#!/usr/bin/env python3
"""Which operand values does GNU as accept, field by field?

For each entry, a statement GNU accepts is the base (found among random
fillings). Then each field in turn takes candidate values -- every
register, or a numeric field's ends, its zero, odd values and powers of
two -- with the other fields left at the base's; GNU's verdict on each is
recorded. Output: isa3.json, each field with `ok` (the values accepted, or
a rule: 'even', a range)."""
import json, random, re, subprocess, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from learn import disasm, atoms, atom_value, norm_shape, AS

T = json.load(open('isa2.json'))

def word_for(e, vals):
    from formsel import fit
    return fit(e, vals)

def field_range(f):
    neg = sum(b[1] for b in f['bits'] if b[1] < 0)
    pos = sum(b[1] for b in f['bits'] if b[1] > 0)
    return f['v0'] + neg, f['v0'] + pos

def text_of(mn, ops_template, vals, kinds, tgt):
    return None

def render(e, vals):
    """the statement for entry e with field values vals, through the
    decoder (it spells every operand as the assembler reads it)"""
    from formsel import fit
    w = fit(e, vals)
    return w

def gnu_accepts(stmts):
    """stmts: list of text lines (each may name L<i> for its own label).
    Returns a set of indexes GNU refused."""
    refused = set()
    keep = list(range(len(stmts)))
    for rnd in range(20):
        src = [' .text']
        for i in keep:
            src.append('L%d: %s' % (i, stmts[i]))
        open('/tmp/con.s', 'w').write('\n'.join(src) + '\n')
        r = subprocess.run([AS, '-mtc162', '/tmp/con.s', '-o', '/tmp/con.o'],
                           capture_output=True, text=True)
        bad = []
        for l in r.stderr.splitlines():
            m = re.match(r'/tmp/con\.s:(\d+): Error', l)
            if m:
                bad.append(keep[int(m.group(1)) - 2])
        if not bad:
            break
        refused |= set(bad)
        keep = [i for i in keep if i not in bad]
    return refused

def main():
    from formsel import fit
    random.seed(9)
    # 1. statement text per (entry, values): encode, decode
    jobs = []          # (entry index, field index, value, word)
    for ei, e in enumerate(T):
        fs = e['fields']
        # a base filling: field values from random bits
        base_vals = []
        for f in fs:
            v = f['v0'] if not isinstance(f['v0'], str) else 0
            for b in f['bits']:
                if b[1] is not None and random.random() < 0.3 and b[1] > 0:
                    v += b[1]
            if f['kind'] in 'A' and v % 2:
                v -= 1 if v > f['v0'] else 0
            # a small shift or count: 0 or 1 is what every form takes
            if f['kind'] == 'N' and len(f['bits']) <= 2:
                # (1: every form takes it; some take nothing else)
                v = f['v0'] + (min(b[1] for b in f['bits']) if f['bits'] else 0)
            base_vals.append(v)
        jobs.append((ei, -1, None, base_vals))
        for fi, f in enumerate(fs):
            if isinstance(f['v0'], str) or not f['bits']:
                continue
            lo, hi = field_range(f)
            step = min(abs(b[1]) for b in f['bits'])
            if f['kind'] in 'DAEP' or hi - lo <= 64 * step:
                cands = list(range(lo, hi + 1, step))
            else:
                cands = sorted({lo, hi, f['v0'], lo + step, hi - step, step, -step if lo < 0 else step,
                                (lo + hi) // 2 // step * step} |
                               {b[1] + f['v0'] for b in f['bits'] if b[1] > 0})
            for v in cands:
                vals = list(base_vals)
                vals[fi] = v
                jobs.append((ei, fi, v, vals))
    words = []
    for ei, fi, v, vals in jobs:
        e = T[ei]
        w = fit(e, vals)
        words.append((e['len'], w if w is not None else e['base']))
    dis, pcs = disasm(words)
    stmts = []
    for (ei, fi, v, vals), d, pc in zip(jobs, dis, pcs):
        e = T[ei]
        if not d or d[0] != e['mnem'] or norm_shape(d[1]) != e['shape']:
            stmts.append('nop')
            continue
        ops = d[1].replace(' (unknown SFR)', '')
        # targets as label expressions
        tk = [k for k, f in enumerate(e['fields']) if f['target']]
        if tk:
            k = tk[-1]
            at = atoms(ops)[k]
            disp = atom_value(at, pc, True)[1]
            i = len(stmts)
            parts = ops.rsplit(at, 1)
            ops = parts[0] + 'L%d%+d' % (i, disp) + parts[1]
        stmts.append('%s %s' % (d[0], ops))
    refused = gnu_accepts(stmts)
    # 2. per field: what was accepted, given the base was
    out = {}
    base_ok = {}
    for j, (ei, fi, v, vals) in enumerate(jobs):
        if fi == -1:
            base_ok[ei] = j not in refused
    for j, (ei, fi, v, vals) in enumerate(jobs):
        if fi < 0 or not base_ok.get(ei):
            continue
        out.setdefault((ei, fi), {})[v] = j not in refused
    for ei, e in enumerate(T):
        e['gnu_base_ok'] = base_ok.get(ei, False)
        for fi, f in enumerate(e['fields']):
            r = out.get((ei, fi))
            if not r:
                continue
            acc = sorted(v for v, ok in r.items() if ok)
            rej = sorted(v for v, ok in r.items() if not ok)
            f['tested'] = len(r)
            if not rej:
                f['ok'] = 'all'
            elif acc and all(v % 2 == 0 for v in acc) and all(v % 2 for v in rej):
                f['ok'] = 'even'
            else:
                f['ok'] = acc
                f['rejected'] = rej
    json.dump(T, open('isa3.json', 'w'), indent=0)
    nb = sum(1 for e in T if not e['gnu_base_ok'])
    print('entries', len(T), 'GNU refused the base of', nb)
    for e in T:
        for f in e['fields']:
            if 'ok' in f and f['ok'] != 'all':
                print('  %-12s %-18s %s %s' % (e['mnem'], e['shape'], f['kind'],
                      f['ok'] if f['ok'] == 'even' else 'only %s' % (f['ok'][:12],)))

if __name__ == '__main__':
    main()
