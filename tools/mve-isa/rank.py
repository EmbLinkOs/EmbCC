#!/usr/bin/env python3
"""Between two forms of one mnemonic and shape that both hold a value,
the one llvm-mc writes. For each pair, the value both fit at their lowest
(every field clear -- where cmode forms overlap, at zero) is assembled;
llvm-mc's word says which goes first. The order is the pairs' order,
then how often check.py saw llvm-mc pick each. Output: order.json, the
entries' indices in the order the assembler tries them."""
import itertools, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import assemble
from constrain import render
from check import fit

T = json.load(open('isa2.json'))
RANK = {int(k): v for k, v in json.load(open('rank.json')).items()}
by = {}
for i, e in enumerate(T):
    if e.get('base_ok'):
        by.setdefault((e['mnem'], e['shape']), []).append(i)
probes = []
for k, ids in by.items():
    for a, b in itertools.combinations(ids, 2):
        for vals in ([f['v0'] for f in T[a]['fields']], [f['v0'] for f in T[b]['fields']]):
            wa, wb = fit(T[a], vals), fit(T[b], vals)
            if wa is not None and wb is not None and wa != wb:
                probes.append((a, b, wa, wb, render(k[0], k[1], vals)))
                break
got = assemble([p[4] for p in probes])
before = {}                       # x -> set of entries x goes before
for (a, b, wa, wb, t), g in zip(probes, got):
    if g == wa:
        before.setdefault(a, set()).add(b)
    elif g == wb:
        before.setdefault(b, set()).add(a)
order = []
for k, ids in by.items():
    # a topological order of the pairs' answers, the frequency breaking ties
    left = sorted(ids, key=lambda i: -RANK.get(i, 0))
    while left:
        for i in left:
            if not any(i in before.get(j, ()) for j in left if j != i):
                order.append(i)
                left.remove(i)
                break
        else:
            order.extend(left)
            break
json.dump(order, open('order.json', 'w'))
print(len(probes), 'pairs asked;', sum(1 for g in got if g is not None), 'answered')
