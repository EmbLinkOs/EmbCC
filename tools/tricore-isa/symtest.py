#!/usr/bin/env python3
"""EmbCC's TriCore symbol operands against GNU as: every statement of
relocs.json GNU took, with `sym` undefined and `oth` a label of another
section, assembled by both; each statement's bytes (fields a relocation
covers ignored) and relocations (offset in the statement, type, symbol,
addend) compared.

    symtest.py EMBCC"""
import json, os, re, subprocess, sys
AS = os.path.expanduser('~/cross/tricore/bin/tricore-elf-as')
RE = os.path.expanduser('~/cross/tricore/bin/tricore-elf-readelf')
NM = os.path.expanduser('~/cross/tricore/bin/tricore-elf-nm')
OC = os.path.expanduser('~/cross/tricore/bin/tricore-elf-objcopy')
EMBCC = sys.argv[1]
R = json.load(open('relocs.json'))
st = []
seen = set()
for o in R:
    if 'refused' in o or o['text'] in seen:
        continue
    seen.add(o['text'])
    st.append(o['text'])

def build(cmd, keep, obj):
    src = [' .section .text.oth,"ax",@progbits', 'oth: nop', ' .text']
    src += ['S%d: %s' % (i, st[i]) for i in keep] + ['Send: nop']
    open('/tmp/sy.s', 'w').write('\n'.join(src) + '\n')
    return subprocess.run(cmd + ['/tmp/sy.s', '-o', obj], capture_output=True, text=True)

def errs(r):
    return {int(m.group(1)): m.group(2) for m in
            re.finditer(r'/tmp/sy\.s:(\d+):(?:\d+:)? (?:Error|error|Fatal error): (.*)', r.stderr)}

keep = list(range(len(st)))
ours_bad = {}
for rnd in range(40):
    g = build([AS, '-mtc162'], keep, '/tmp/sy_g.o')
    e = build([EMBCC, '--target=tricore-none-elf', '-c'], keep, '/tmp/sy_e.o')
    ge, ee = errs(g), errs(e)
    if not ge and not ee and g.returncode == 0 and e.returncode == 0:
        break
    drop = set()
    for ln in ge:
        if 4 <= ln < 4 + len(keep):
            drop.add(keep[ln - 4])
    for ln, why in ee.items():
        if 4 <= ln < 4 + len(keep):
            drop.add(keep[ln - 4])
            if keep[ln - 4] not in {keep[l - 4] for l in ge if 4 <= l < 4 + len(keep)}:
                ours_bad[keep[ln - 4]] = why
    if not drop:
        print('no line to drop:', g.stderr[:300], e.stderr[:300])
        break
    keep = [i for i in keep if i not in drop]

def info(o):
    nm = subprocess.run([NM, o], capture_output=True, text=True).stdout
    a = {p[2]: int(p[0], 16) for p in (l.split() for l in nm.splitlines()) if len(p) == 3}
    rel = subprocess.run([RE, '-rW', o], capture_output=True, text=True).stdout
    rs, sec = [], None
    for l in rel.splitlines():
        if l.startswith('Relocation section'):
            sec = l.split("'")[1]
            continue
        p = l.split()
        if sec == '.rela.text' and len(p) >= 4 and re.match(r'^[0-9a-f]{8}$', p[0]):
            sym = p[4] if len(p) > 4 else ''
            add = ''.join(p[5:]) if len(p) > 5 else ''
            rs.append((int(p[0], 16), p[2], sym, add))
    subprocess.run([OC, '-O', 'binary', '-j', '.text', o, o + '.bin'], check=True)
    return a, rs, open(o + '.bin', 'rb').read()
ga, gr, gb = info('/tmp/sy_g.o')
ea, er, eb = info('/tmp/sy_e.o')
diff = 0
for n, i in enumerate(keep):
    nx = 'S%d' % keep[n + 1] if n + 1 < len(keep) else 'Send'
    g0, g1 = ga['S%d' % i], ga[nx]
    e0, e1 = ea['S%d' % i], ea[nx]
    grs = [(off - g0, t, s, a) for off, t, s, a in gr if g0 <= off < g1 and t != 'R_TRICORE_RELAX']
    ers = [(off - e0, t, s, a) for off, t, s, a in er if e0 <= off < e1]
    # (GNU's section symbol is `.text.oth`, ours may be the label)
    norm = lambda rs: [(o, t, '.text.oth' if s in ('oth', '.text.oth') else s, a) for o, t, s, a in rs]
    if norm(grs) != norm(ers) or g1 - g0 != e1 - e0:
        diff += 1
        if diff <= 30:
            print('DIFF %-34s gnu %s %s | embcc %s %s' % (st[i], gb[g0:g1].hex(), grs, eb[e0:e1].hex(), ers))
print('%d statements, both took %d, EmbCC alone refused %d, %d differ' %
      (len(st), len(keep), len(ours_bad), diff))
import collections
c = collections.Counter(re.sub(r"'[^']*'", "'X'", v)[:100] for v in ours_bad.values())
for k, v in c.most_common(10):
    print('  %4d  %s' % (v, k))
for i, v in list(ours_bad.items())[:10]:
    print('  refused: %s -- %s' % (st[i], v[:130]))
