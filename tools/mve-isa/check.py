#!/usr/bin/env python3
"""The learned table against llvm-mc both ways.

1. Random fillings of every entry are encoded with the learned fields and
   decoded by llvm-mc: each operand must read back as the value put in.
2. The decoded statements are assembled by llvm-mc: the word it writes is
   the form it prefers, and `fit` over the statement's (mnemonic, shape)
   entries must find that word among the ones that hold it -- the rank
   GNU's and LLVM's assemblers choose by. Output: rank.json."""
import json, os, random, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import disasm, assemble
from learn import decode

def fit(e, vals):
    w = e['base']
    for f, v in zip(e['fields'], vals):
        x = v - f['v0']
        bits = f['bits']
        neg = [b for b in bits if b[1] < 0]
        pos = sorted([b for b in bits if b[1] > 0], key=lambda b: -b[1])
        if neg and all(b[1] < 0 for b in bits):          # every weight negative
            x, pos, neg = -x, sorted([[b[0], -b[1]] for b in bits], key=lambda b: -b[1]), []
        ch = []
        if neg and x < 0:
            x -= neg[0][1]
            ch.append(neg[0][0])
        for b in pos:
            if x >= b[1]:
                x -= b[1]
                ch.append(b[0])
        if x:
            return None
        for b in ch:
            w |= 1 << b
    return w

def main():
    T = json.load(open('isa.json'))
    random.seed(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
    words, exp = [], []
    for i, e in enumerate(T):
        for t in range(4):
            vals = []
            for f in e['fields']:
                v = f['v0']
                for b, wt in f['bits']:
                    if random.random() < 0.5:
                        v += wt
                vals.append(v)
            w = fit(e, vals)
            if w is None:
                continue
            words.append(w)
            exp.append((i, vals))
    dec = decode(words)
    bad = {}
    texts = []
    for (i, vals), d, w in zip(exp, dec, words):
        e = T[i]
        if not d or d[0] != e['mnem'] or d[1] != e['shape'] or [a[1] for a in d[2]] != vals:
            bad[i] = bad.get(i, 0) + 1
            continue
        texts.append((i, w))
    print('round trip:', len(words), 'words,', sum(bad.values()), 'wrong, in', len(bad), 'entries')
    for i in list(bad)[:10]:
        print('   ', T[i]['mnem'], T[i]['shape'], hex(T[i]['base']))
    # 2. reassembly: llvm-mc's word for each statement
    raw = disasm([w for _, w in texts])
    stmts = ['%s %s' % d for d in raw]
    got = assemble(stmts)
    by = {}
    for k, e in enumerate(T):
        by.setdefault((e['mnem'], e['shape']), []).append(k)
    rank, miss, noparse = {}, 0, 0
    for (i, w), g, d in zip(texts, got, raw):
        if g is None:
            noparse += 1
            continue
        e = T[i]
        cands = []
        dd = decode([g])[0]
        if not dd:
            continue
        vals = [a[1] for a in dd[2]]
        for k in by[(e['mnem'], e['shape'])]:
            if fit(T[k], vals) == g:
                rank[k] = rank.get(k, 0) + 1
                break
        else:
            miss += 1
    json.dump(rank, open('rank.json', 'w'))
    print('reassembly:', len(texts), 'statements, llvm-mc refused', noparse,
          ', its word in no entry', miss)

if __name__ == '__main__':
    main()
