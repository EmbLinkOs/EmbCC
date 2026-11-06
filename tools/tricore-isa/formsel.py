#!/usr/bin/env python3
"""Which form does GNU as pick for a statement? Random encodings of every
entry are decoded to text; GNU as assembles the text; the bytes it writes
are compared with a model: among the entries of the statement's mnemonic and
shape, the shortest whose fields hold the operands."""
import json, random, re, subprocess, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from learn import disasm, atoms, atom_value, norm_shape, AS, OBJDUMP

T = json.load(open('isa2.json'))
BY = {}
for e in T:
    BY.setdefault((e['mnem'], e['shape']), []).append(e)

def fit(e, vals):
    """the word for entry e holding vals (per field, ints or names), or None"""
    w = e['base']
    for f, v in zip(e['fields'], vals):
        if isinstance(v, str) or isinstance(f['v0'], str):
            return None
        bits = f['bits']
        d = v - f['v0']
        if not bits:
            if d != 0:
                return None
            continue
        # solve d = sum of weights of the bits set: the negative one (a sign
        # bit) first, then the positive ones greedily from the largest
        neg = [b for b in bits if b[1] < 0]
        pos = sorted([b for b in bits if b[1] > 0], key=lambda b: -b[1])
        chosen = []
        best = None
        for use_neg in ([False, True] if neg else [False]):
            r = d
            ch = []
            if use_neg:
                r -= neg[0][1]
                ch.append(neg[0][0])
            for b in pos:
                if r >= b[1]:
                    r -= b[1]
                    ch.append(b[0])
            if r == 0:
                best = ch
                break
        if best is None:
            return None
        for b in best:
            w |= 1 << b
    return w

def model(mnem, shape, vals, pc):
    cands = BY.get((mnem, shape), [])
    out = []
    for e in sorted(cands, key=lambda e: e['len']):
        vv = []
        for f, v in zip(e['fields'], vals):
            vv.append(v)
        w = fit(e, vv)
        if w is not None:
            out.append((e['len'], w))
    return out

def main():
    random.seed(3)
    stmts = []
    batch = []
    for e in T:
        for k in range(int(sys.argv[1]) if len(sys.argv) > 1 else 4):
            w = e['base']
            for f in e['fields']:
                for b in f['bits']:
                    if random.random() < 0.5:
                        w |= 1 << b[0]
            batch.append((e['len'], w))
    dis, pcs = disasm(batch)
    lines = []
    disps = []
    for (ln, w), d, pc in zip(batch, dis, pcs):
        if d and not d[1].endswith('(unknown SFR)'):
            lines.append(d)
            # each target's displacement, from the place it was decoded at
            sh = norm_shape(d[1]); c = BY.get((d[0], sh), [])
            dd = 0
            if c and any(f['target'] for f in c[0]['fields']):
                k = [j for j, f in enumerate(c[0]['fields']) if f['target']][-1]
                dd = atom_value(atoms(d[1])[k], pc, True)[1]
            disps.append(dd)
    # assemble each statement at a known place: one per line, each preceded
    # by a label so its address and length are recoverable
    # a branch's target as an expression of its own label (GNU takes no
    # absolute number for a pc-relative one)
    text = []
    for i, (mn, ops) in enumerate(lines):
        sh = norm_shape(ops)
        cands = BY.get((mn, sh), [])
        ats = atoms(ops)
        if cands and any(f['target'] for f in cands[0]['fields']):
            k = [j for j, f in enumerate(cands[0]['fields']) if f['target']][-1]
            # the decoded address assumed the encoding's own place: take
            # the displacement from the batch (recomputed below)
            text.append((mn, ops, k))
        else:
            text.append((mn, ops, None))
    errs = {}
    keep = list(range(len(lines)))
    for rnd in range(6):
        src = [' .text']
        for j, i in enumerate(keep):
            mn, ops, k = text[i]
            if k is not None:
                ats = atoms(ops)
                disp = disps[i]
                parts = ops.rsplit(ats[k], 1)
                ops = parts[0] + 'L%d%+d' % (i, disp) + parts[1]
            src.append('L%d: %s %s' % (i, mn, ops))
        src.append('Lend:')
        open('/tmp/sel.s', 'w').write('\n'.join(src) + '\n')
        if os.path.exists('/tmp/sel.o'):
            os.remove('/tmp/sel.o')
        r = subprocess.run([AS, '-mtc162', '/tmp/sel.s', '-o', '/tmp/sel.o'], capture_output=True, text=True)
        bad = []
        for l in r.stderr.splitlines():
            m = re.match(r'/tmp/sel\.s:(\d+): Error: (.*)', l)
            if m:
                i = keep[int(m.group(1)) - 2]
                errs[i] = m.group(2)
                bad.append(i)
        if not bad:
            break
        keep = [i for i in keep if i not in bad]
    nm = subprocess.run([os.path.expanduser('~/cross/tricore/bin/tricore-elf-nm'), '/tmp/sel.o'],
                        capture_output=True, text=True).stdout
    addr = {}
    for l in nm.splitlines():
        p = l.split()
        if len(p) == 3:
            addr[p[2]] = int(p[0], 16)
    subprocess.run([os.path.expanduser('~/cross/tricore/bin/tricore-elf-objcopy'), '-O', 'binary',
                    '-j', '.text', '/tmp/sel.o', '/tmp/sel.bin'], check=True)
    data = open('/tmp/sel.bin', 'rb').read()
    agree = differ = errors = 0
    samples = []
    cat = {}
    for i, (mn, ops) in enumerate(lines):
        if i in errs:
            errors += 1
            samples.append('ERR  %s %s : %s' % (mn, ops, errs[i]))
            continue
        a = addr.get('L%d' % i)
        j = keep.index(i) if i in keep else -1
        b = addr.get('L%d' % keep[j + 1]) if 0 <= j < len(keep) - 1 else addr.get('Lend')
        if a is None or b is None:
            continue
        got = int.from_bytes(data[a:b], 'little'), b - a
        vals = []
        sh = norm_shape(ops)
        cands = BY.get((mn, sh), [])
        if not cands:
            continue
        e0 = cands[0]
        for k, at in enumerate(atoms(ops)):
            if k < len(e0['fields']) and e0['fields'][k]['target']:
                vals.append(disps[i])
            else:
                vals.append(atom_value(at, a, False)[1])
        m = model(mn, sh, vals, a)
        pred = (m[0][1], m[0][0]) if m else None
        if pred == got:
            agree += 1
        else:
            differ += 1
            samples.append('DIFF %s %s : gnu %s/%d model %s' % (mn, ops, hex(got[0]), got[1],
                           (hex(pred[0]), pred[1]) if pred else None))
            cat.setdefault((mn, sh, got[1], pred[1] if pred else None), []).append(ops)
    print('agree', agree, 'differ', differ, 'gnu errors', errors)
    for k, v in sorted(cat.items()):
        print('  %-12s %-16s gnu %d model %s  %d  e.g. %s' % (k[0], k[1], k[2], k[3], len(v), v[0]))
    errk = {}
    for s2 in samples:
        if s2.startswith('ERR'):
            p = s2.split(' : ')
            errk.setdefault(p[1][:60], []).append(p[0])
    for k, v in sorted(errk.items(), key=lambda x: -len(x[1]))[:25]:
        print('  ERR %3d %s   e.g. %s' % (len(v), k, v[0][5:]))

if __name__ == '__main__':
    main()
