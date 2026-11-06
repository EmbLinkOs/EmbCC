#!/usr/bin/env python3
"""Every MVE word, decoded by llvm-mc: the Thumb coprocessor space MVE
lives in -- first halfwords 0xEC00-0xEFFF and 0xFC00-0xFFFF, with every
second halfword -- keeping the groups that name a vector register (VFP's
forms there are the hand-written encoders'). Output: groups.json, up to
100 examples (word, operand text) per (mnemonic, shape)."""
import concurrent.futures, json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import disasm
from shapes import parse

CHUNK = 1 << 20

def ranges():
    for lo, hi in ((0xEC000000, 0xF0000000), (0xFC000000, 0x100000000)):
        for base in range(lo, hi, CHUNK):
            yield [base + k for k in range(CHUNK)]

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
        # (MVE's: a vector register somewhere; VFP's forms are not)
        if p and 'Q' not in p[0]:
            continue
        if not p and not re.search(r'\bq[0-7]\b', d[1]):
            continue
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
