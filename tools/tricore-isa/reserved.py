#!/usr/bin/env python3
"""What GNU as writes in each form's reserved bits.

A reserved bit changes nothing objdump prints, so the learner cannot see
its value -- but GNU as sets some (updfl writes 1s where its operand's
register number repeats). Each entry's base statement is assembled by GNU;
the reserved bits of what it wrote are kept. Output: gnures.json,
{entry index: the reserved bits' values}."""
import json, os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from learn import disasm, atoms, atom_value, norm_shape, AS
from formsel import fit
NM = os.path.expanduser('~/cross/tricore/bin/tricore-elf-nm')
OC = os.path.expanduser('~/cross/tricore/bin/tricore-elf-objcopy')

T = json.load(open('isa3.json'))
words, idx = [], []
for ei, e in enumerate(T):
    if not e.get('gnu_base_ok') or not e.get('reserved'):
        continue
    # two fillings: all-zero fields and a second with registers 1/odd
    for alt in (0, 1):
        vals = []
        for f in e['fields']:
            v = f['v0'] if not isinstance(f['v0'], str) else 0
            ok = f.get('ok', 'all')
            if isinstance(ok, list) and ok:
                v = ok[0]
            elif alt and f['kind'] in 'DA' and f['bits']:
                v = f['v0'] + min(b[1] for b in f['bits'] if b[1] and b[1] > 0) * 3
            vals.append(v)
        w = fit(e, vals)
        if w is None:
            continue
        words.append((e['len'], w))
        idx.append(ei)
dis, pcs = disasm(words)
stmts = []
for (ln, w), d, pc, ei in zip(words, dis, pcs, idx):
    e = T[ei]
    if not d or d[0] != e['mnem'] or norm_shape(d[1]) != e['shape'] or '(unknown SFR)' in d[1]:
        continue
    ops = d[1]
    tk = [k for k, f in enumerate(e['fields']) if f['target']]
    i = len(stmts)
    if tk:
        at = atoms(ops)[tk[-1]]
        disp = atom_value(at, pc, True)[1]
        parts = ops.rsplit(at, 1)
        ops = parts[0] + 'L%d%+d' % (i, disp) + parts[1]
    stmts.append((ei, w, '%s %s' % (d[0], ops)))
keep = list(range(len(stmts)))
for rnd in range(20):
    src = [' .text'] + ['L%d: %s' % (i, stmts[i][2]) for i in keep] + ['Lend: nop']
    open('/tmp/rs.s', 'w').write('\n'.join(src) + '\n')
    r = subprocess.run([AS, '-mtc162', '/tmp/rs.s', '-o', '/tmp/rs.o'], capture_output=True, text=True)
    bad = {keep[int(m.group(1)) - 2] for m in re.finditer(r'/tmp/rs\.s:(\d+): (?:Error|Fatal)', r.stderr)}
    if not bad and r.returncode == 0:
        break
    keep = [i for i in keep if i not in bad]
nm = subprocess.run([NM, '/tmp/rs.o'], capture_output=True, text=True).stdout
a = {p[2]: int(p[0], 16) for p in (l.split() for l in nm.splitlines()) if len(p) == 3}
subprocess.run([OC, '-O', 'binary', '-j', '.text', '/tmp/rs.o', '/tmp/rs.bin'], check=True)
b = open('/tmp/rs.bin', 'rb').read()
out, conflict, mirrors = {}, set(), {}
for n, i in enumerate(keep):
    ei, w, txt = stmts[i]
    e = T[ei]
    nx = 'L%d' % keep[n + 1] if n + 1 < len(keep) else 'Lend'
    got = b[a['L%d' % i]:a[nx]]
    if len(got) != e['len']:
        continue
    g = int.from_bytes(got, 'little')
    # (what GNU sets may depend on the operands: then it is not a constant)
    rb = g & e['reserved']
    if (g & ~e['reserved'] & ~e['fieldmask']) != (e['base'] & ~e['reserved'] & ~e['fieldmask']):
        continue
    if ei in out and out[ei] != rb:
        conflict.add(ei)
    out[ei] = rb
    # a register written twice: the reserved bits are a field's value
    lo = (e['reserved'] & -e['reserved']).bit_length() - 1
    for k, f in enumerate(e['fields']):
        if f['kind'] in 'DAE' and f['bits']:
            v = f['v0'] + sum(bb[1] for bb in f['bits'] if (g >> bb[0]) & 1)
            if (v << lo) & 0xffffffff == rb and (v << lo) & ~e['reserved'] == 0:
                mirrors.setdefault(ei, set()).add((k, lo))
            else:
                mirrors.setdefault(ei, set()).add(None)
nz = {k: v for k, v in out.items() if v and k not in conflict}
# a mirror holds in every filling measured (one candidate field each time)
mir = {}
for k in conflict:
    cands = [m for m in mirrors.get(k, ()) if m]
    if len(cands) == 1 and None not in mirrors[k] or cands and len(mirrors[k] - {None}) == 1:
        mir[k] = cands[0]
json.dump({'set': nz, 'mirror': mir}, open('gnures.json', 'w'))
print('mirrors:', {T[k]['mnem']: v for k, v in mir.items()})
print('%d entries with reserved bits measured, %d where GNU sets some, %d operand-dependent' %
      (len(out), len(nz), len(conflict)))
for k in sorted(conflict | set(nz))[:20]:
    print('  %-10s %-14s reserved %#010x gnu %#010x%s' % (T[k]['mnem'], T[k]['shape'], T[k]['reserved'],
          out[k], ' (depends on operands)' if k in conflict else ''))
