#!/usr/bin/env python3
"""The type spellings llvm-mc takes for each NEON mnemonic: for every
table mnemonic (llvm-mc's printing, `vadd.i32`), each other data type
written after the base (`vadd.s32`, `vadd.u32`, `vadd.32`, ...) is offered
with the same operands; one that assembles to the same word is a
synonym. Output: synonyms.json, {spelling: canonical}."""
import json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import assemble
from constrain import render
from check import fit

TYPES = ['i8', 'i16', 'i32', 'i64', 's8', 's16', 's32', 's64', 'u8', 'u16', 'u32',
         'u64', '8', '16', '32', '64', 'f32', 'f16', 'f64', 'p8', 'p16', 'p64']

def main():
    T = json.load(open('isa2.json'))
    seen, jobs = set(), []
    for e in T:
        if not e.get('base_ok') or e['mnem'] in seen or '.' not in e['mnem']:
            continue
        seen.add(e['mnem'])
        # two fillings: every field clear, and each at its smallest step
        # above that (a zero immediate hides a type's scaling: vext.8 #0
        # is vext.32 #0)
        vals = [f['v0'] for f in e['fields']]
        vals2 = [f['v0'] + min((b[1] for b in f['bits'] if b[1] > 0), default=0)
                 for f in e['fields']]
        w, w2 = fit(e, vals), fit(e, vals2)
        if w is None or w2 is None:
            continue
        txt = render(e['mnem'], e['shape'], vals)
        txt2 = render(e['mnem'], e['shape'], vals2)
        base, ty = e['mnem'].split('.', 1)
        tail = ''
        if '.' in ty:                       # vcvt.f32.s32: the last type only
            ty, tail = ty.rsplit('.', 1)[0], '.' + ty.rsplit('.', 1)[1]
            ty = ty
        for t in TYPES:
            alt = '%s.%s' % (base, t) + ('' if '.' not in e['mnem'][len(base) + 1:] else '')
            if alt == e['mnem']:
                continue
            jobs.append((e['mnem'], alt, w, txt.replace(e['mnem'], alt, 1),
                         w2, txt2.replace(e['mnem'], alt, 1)))
    got = assemble([j[3] for j in jobs])
    got2 = assemble([j[5] for j in jobs])
    syn = {}
    for (canon, alt, w, txt, w2, t2), g, g2 in zip(jobs, got, got2):
        if g == w and g2 == w2 and alt not in seen:
            syn.setdefault(alt, canon)
    json.dump(syn, open('synonyms.json', 'w'), indent=0)
    print(len(syn), 'synonyms for', len({v for v in syn.values()}), 'mnemonics')
    for k in list(syn)[:12]:
        print('  %s -> %s' % (k, syn[k]))

if __name__ == '__main__':
    main()
