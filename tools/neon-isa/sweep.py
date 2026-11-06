#!/usr/bin/env python3
"""Every A32 NEON word, decoded by llvm-mc: the data-processing space
(0xF2000000-0xF3FFFFFF) and the element/structure loads and stores
(0xF4000000-0xF4FFFFFF) whole, and the coprocessor-10/11 space the
transfers between NEON and core registers share with VFP (cond 1110,
0xEC..0xEE) with every condition-free bit. Output: groups.json, up to 40
examples (word, operand text) per (mnemonic, shape)."""
import concurrent.futures, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import disasm
from shapes import parse

CHUNK = 1 << 20

def ranges():
    for base in range(0xF2000000, 0xF5000000, CHUNK):
        yield [base + k for k in range(CHUNK)]
    # cp10/cp11 transfers: bits 8..11 = 1010 or 1011
    for hi in (0xEC, 0xED, 0xEE):
        words = []
        for k in range(1 << 21):
            low8 = k & 0xff
            mid = (k >> 8) & 0xfff
            cp = 0xa | ((k >> 20) & 1)
            words.append((hi << 24) | (mid << 12) | (cp << 8) | low8)
            if len(words) == CHUNK:
                yield words
                words = []
        if words:
            yield words

K = 100

def one(words):
    # a random sample of each group (reservoir): the first examples of a
    # group share their high bits, and a field that is two forms -- an
    # immediate's cmode -- would be learned as one
    import random
    rnd = random.Random(words[0])
    g, seen = {}, {}
    for w, d in zip(words, disasm(words)):
        if not d:
            continue
        p = parse(d[1])
        k = (d[0], p[0]) if p else (d[0], '?' + d[1])
        if not p and len(g.get(k, [])) >= 1:
            continue
        n = seen[k] = seen.get(k, 0) + 1
        lst = g.setdefault(k, [])
        if len(lst) < K:
            lst.append((w, d[1]))
        else:
            j = rnd.randrange(n)
            if j < K:
                lst[j] = (w, d[1])
    return g

if __name__ == '__main__':
    groups = {}
    with concurrent.futures.ProcessPoolExecutor(8) as ex:
        for g in ex.map(one, ranges()):
            for k, v in g.items():
                lst = groups.setdefault(k, [])
                lst.extend(v)
    import random
    rnd = random.Random(1)
    for k in groups:
        if len(groups[k]) > K:
            groups[k] = rnd.sample(groups[k], K)
    out = [{'mnem': k[0], 'shape': k[1], 'ex': v} for k, v in sorted(groups.items())]
    json.dump(out, open('groups.json', 'w'))
    bad = [o for o in out if o['shape'].startswith('?')]
    print(len(out), 'groups,', len({o['mnem'] for o in out}), 'mnemonics,', len(bad), 'unparsed shapes')
    for o in bad[:15]:
        print('  unparsed:', o['mnem'], o['shape'][1:])
