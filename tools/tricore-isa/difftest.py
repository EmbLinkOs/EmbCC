#!/usr/bin/env python3
"""EmbCC's TriCore assembler against GNU as, over every form.

Random encodings of every table entry are decoded by GNU objdump into
statements; GNU as (-mtc162) and `embcc -c` assemble the same file; each
statement's bytes are compared (labels L<i> mark them). A branch target is
written `L<i>+disp`. Statements GNU refuses are dropped first.

    difftest.py EMBCC [N per entry] [seed]"""
import json, os, random, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from learn import disasm, atoms, atom_value, norm_shape, AS
NM = os.path.expanduser('~/cross/tricore/bin/tricore-elf-nm')
OC = os.path.expanduser('~/cross/tricore/bin/tricore-elf-objcopy')

EMBCC = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 4
random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
T = json.load(open('isa3.json'))

batch, ents = [], []
for e in T:
    if not e.get('gnu_base_ok'):
        continue
    for k in range(N):
        w = e['base']
        for f in e['fields']:
            for b in f['bits']:
                if random.random() < 0.5:
                    w |= 1 << b[0]
        batch.append((e['len'], w))
        ents.append(e)
dis, pcs = disasm(batch)
stmts = []
for (ln, w), d, pc, e in zip(batch, dis, pcs, ents):
    if not d or '(unknown SFR)' in d[1]:
        continue
    mn, ops = d
    tk = [k for k, f in enumerate(e['fields']) if f['target']]
    if tk and mn == e['mnem'] and norm_shape(ops) == e['shape']:
        at = atoms(ops)[tk[-1]]
        disp = atom_value(at, pc, True)[1]
        i = len(stmts)
        parts = ops.rsplit(at, 1)
        ops = parts[0] + 'L%d%+d' % (i, disp) + parts[1]
    elif re.match(r'^(j|loop|call|fcall)', mn) and mn not in ('ji', 'jli', 'calli', 'fcalli'):
        continue
    stmts.append('%s %s' % (mn, ops))

# variants: the first register repeated in each later one of its file, and
# the implicit registers of the 16-bit forms (d15, a15, sp) in each place
if os.environ.get('DT_EQ'):
    more = []
    R = re.compile(r'%(d|a|e)(\d+)(?![0-9lu])')
    for t in stmts:
        if 'L' in t.split(' ', 1)[-1] and re.search(r'L\d+', t):
            continue
        mn, _, ops = t.partition(' ')
        regs = list(R.finditer(ops))
        if not regs:
            continue
        f0 = regs[0]
        for r in regs[1:]:
            if r.group(1) == f0.group(1):
                more.append(mn + ' ' + ops[:r.start()] + f0.group(0) + ops[r.end():])
        for r in regs:
            for imp in ({'d': ['%d15'], 'a': ['%a15', '%sp'], 'e': []}[r.group(1)]):
                v = ops[:r.start()] + imp + ops[r.end():]
                more.append(mn + ' ' + v)
                # and the first register repeated too
                rr = list(R.finditer(v))
                if len(rr) > 1 and rr[0].group(1) == rr[-1].group(1):
                    more.append(mn + ' ' + v[:rr[-1].start()] + rr[0].group(0) + v[rr[-1].end():])
    stmts += sorted(set(more))

def run_gnu(keep):
    src = [' .text'] + ['L%d: %s' % (i, stmts[i]) for i in keep] + ['Lend: nop']
    open('/tmp/dt.s', 'w').write('\n'.join(src) + '\n')
    return subprocess.run([AS, '-mtc162', '/tmp/dt.s', '-o', '/tmp/dt_g.o'], capture_output=True, text=True)

keep = list(range(len(stmts)))
for rnd in range(30):
    r = run_gnu(keep)
    bad = {keep[int(m.group(1)) - 2] for m in re.finditer(r'/tmp/dt\.s:(\d+): (?:Error|Fatal)', r.stderr)}
    if not bad and r.returncode == 0:
        break
    keep = [i for i in keep if i not in bad]
# EmbCC: drop what it refuses, and say what
ours_bad = {}
for rnd in range(30):
    r = subprocess.run([EMBCC, '--target=tricore-none-elf', '-c', '/tmp/dt.s', '-o', '/tmp/dt_e.o'],
                       capture_output=True, text=True)
    bad = {}
    for m in re.finditer(r'/tmp/dt\.s:(\d+):(?:\d+:)? error: (.*)', r.stderr):
        ln = int(m.group(1))
        if 2 <= ln < len(keep) + 2:
            bad[keep[ln - 2]] = m.group(2)
    if not bad and r.returncode == 0:
        break
    if not bad:
        print(r.stderr[:2000]); sys.exit(1)
    ours_bad.update(bad)
    keep = [i for i in keep if i not in bad]
    run_gnu(keep)
run_gnu(keep)

def image(o):
    nm = subprocess.run([NM, o], capture_output=True, text=True).stdout
    a = {p[2]: int(p[0], 16) for p in (l.split() for l in nm.splitlines()) if len(p) == 3}
    subprocess.run([OC, '-O', 'binary', '-j', '.text', o, o + '.bin'], check=True)
    return a, open(o + '.bin', 'rb').read()
ga, gb = image('/tmp/dt_g.o')
ea, eb = image('/tmp/dt_e.o')
diff = 0
seen = set()
for n, i in enumerate(keep):
    nx = 'L%d' % keep[n + 1] if n + 1 < len(keep) else 'Lend'
    g = gb[ga['L%d' % i]:ga[nx]]
    e = eb[ea['L%d' % i]:ea[nx]]
    tgt = 'L%d' % i in stmts[i]
    if g != e and not (tgt and len(g) == len(e)):
        diff += 1
        key = stmts[i].split()[0]
        if key not in seen or diff < 40:
            seen.add(key)
            print('DIFF %-40s gnu %-12s embcc %s' % (stmts[i], g.hex(), e.hex()))
print('%d statements, GNU took %d, EmbCC refused %d, %d differ' %
      (len(stmts), len(keep) + len(ours_bad), len(ours_bad), diff))
import collections
c = collections.Counter(re.sub(r"'[^']*'", "'X'", v)[:90] for v in ours_bad.values())
for k, v in c.most_common(8):
    print('  %4d  %s' % (v, k))
for i, v in list(ours_bad.items())[:12]:
    print('  refused: %s -- %s' % (stmts[i], v[:120]))
