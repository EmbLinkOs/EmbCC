#!/usr/bin/env python3
"""A load or store whose offset llvm-mc's sample gave in one sign only: the
other sign's form, from it. MVE's offset is a magnitude and an add bit
(bit 23 of the word, bit 7 of the first halfword); the learner, given only
subtracting examples of a form, learns its adding twin with the
subtracting base, which no value fits. Here each form with a negative
offset and no positive twin gets one -- the add bit set, the weights
negated -- kept only if llvm-mc assembles three of its values to the words
it fits. Run after constrain.py; rewrites isa2.json."""
import copy, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import assemble
from constrain import render
from check import fit

T = json.load(open('isa2.json'))
def signs(e):
    for f in e['fields']:
        if f['kind'] == 'N' and f['bits']:
            ws = [b[1] for b in f['bits']]
            if all(w < 0 for w in ws): return -1
            if all(w > 0 for w in ws): return 1
    return 0
have = {(e['mnem'], e['shape'], signs(e)) for e in T if e.get('base_ok')}
added = 0
for e in list(T):
    if not e.get('base_ok') or signs(e) != -1 or (e['mnem'], e['shape'], 1) in have:
        continue
    if not ('vldr' in e['mnem'] or 'vstr' in e['mnem']):
        continue
    n = copy.deepcopy(e)
    n['base'] = e['base'] | 0x800000
    for f in n['fields']:
        if f['kind'] == 'N':
            f['bits'] = [[b[0], -b[1]] for b in f['bits']]
            f['ok'] = 'all'
    tests = []
    for k in (1, 2, 5):
        vals = [f['v0'] + (sum(b[1] for b in f['bits'][:1]) * k if f['kind'] == 'N' else t)
                for t, f in enumerate(n['fields'])]
        w = fit(n, vals)
        if w is not None:
            tests.append((render(n['mnem'], n['shape'], vals), w))
    got = assemble([s for s, _ in tests])
    if tests and all(g == w for (_, w), g in zip(tests, got)):
        T.append(n)
        have.add((e['mnem'], e['shape'], 1))
        added += 1
        print('  +', n['mnem'], n['shape'], hex(n['base']))
    else:
        print('  ? not taken:', e['mnem'], e['shape'], [s for s, _ in tests], got)
json.dump(T, open('isa2.json', 'w'))
print(added, 'forms added')
