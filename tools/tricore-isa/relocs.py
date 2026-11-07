#!/usr/bin/env python3
"""Which relocation does GNU as write for a symbol in each numeric field?

Every entry's statement (a GNU-accepted base, as constrain.py made) has each
numeric field replaced in turn by `sym`, `hi:sym`, `lo:sym`, `sm:sym`,
`up:sym` and `oth` (a label in another section); GNU assembles what it
takes, and the relocation type, the offset of the field and the form it
chose (by length) are recorded. Output: relocs.json."""
import json, re, subprocess, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from learn import disasm, atoms, atom_value, norm_shape, AS, OBJDUMP
from formsel import fit

T = json.load(open('isa3.json'))
OPS = ['sym', 'hi:sym', 'lo:sym', 'sm:sym', 'up:sym', 'oth']

def main():
    jobs = []
    words = []
    for ei, e in enumerate(T):
        if not e.get('gnu_base_ok'):
            continue
        vals = []
        for f in e['fields']:
            v = f['v0'] if not isinstance(f['v0'], str) else 0
            ok = f.get('ok', 'all')
            if isinstance(ok, list) and ok:
                v = ok[0]
            vals.append(v)
        w = fit(e, vals)
        if w is None:
            continue
        words.append((e['len'], w))
        jobs.append((ei, vals))
    dis, pcs = disasm(words)
    stmts = []
    for (ei, vals), d in zip(jobs, dis):
        e = T[ei]
        if not d or d[0] != e['mnem'] or norm_shape(d[1]) != e['shape']:
            continue
        ops = d[1].replace(' (unknown SFR)', '')
        ats = atoms(ops)
        for k, f in enumerate(e['fields']):
            if f['kind'] not in 'NL' or not f['bits']:
                continue
            # replace the k-th atom by each operand form (by position)
            pos = [m for m in re.finditer(re.escape(ats[k]), ops)]
            # the k-th atom's occurrence: count atoms before it
            spans = []
            i = 0
            for m in re.finditer(r'%sp|%[daep](?:1[0-5]|[0-9])[lu]{0,2}|\$[a-z_][a-z0-9_]*|\$0x[0-9a-f]+|-?0x[0-9a-f]+|-?\b[0-9]+\b', ops):
                spans.append(m.span())
            if k >= len(spans):
                continue
            a, b = spans[k]
            for op in OPS:
                txt = ops[:a] + op + ops[b:]
                stmts.append((ei, k, op, '%s %s' % (e['mnem'], txt)))
    # each statement alone in a file of its own (a fatal error ends GNU's
    # whole file), in parallel
    import concurrent.futures, tempfile
    RE = os.path.expanduser('~/cross/tricore/bin/tricore-elf-readelf')
    def one(i):
        ei, k, op, txt = stmts[i]
        d = tempfile.mkdtemp(prefix='trel')
        sp, op_ = os.path.join(d, 'a.s'), os.path.join(d, 'a.o')
        open(sp, 'w').write(' .section .text.oth,"ax",@progbits\noth: nop\n .text\nS: %s\nE: nop\n' % txt)
        r = subprocess.run([AS, '-mtc162', sp, '-o', op_], capture_output=True, text=True)
        if r.returncode or not os.path.exists(op_):
            why = (r.stderr.strip().splitlines() or ['?'])[-1]
            why = re.sub(r'^.*?(Error|Fatal error): ', '', why)
            return i, None, why
        nm = subprocess.run([os.path.expanduser('~/cross/tricore/bin/tricore-elf-nm'), op_],
                            capture_output=True, text=True).stdout
        addr = {p[2]: int(p[0], 16) for p in (l.split() for l in nm.splitlines()) if len(p) == 3}
        rel = subprocess.run([RE, '-rW', op_], capture_output=True, text=True).stdout
        rs, sec = [], None
        for l in rel.splitlines():
            if l.startswith('Relocation section'):
                sec = l.split("'")[1]
                continue
            p = l.split()
            if sec == '.rela.text' and len(p) >= 3 and re.match(r'^[0-9a-f]{8}$', p[0]):
                rs.append((int(p[0], 16) - addr.get('S', 0), p[2], ' '.join(p[4:])))
        data = subprocess.run([os.path.expanduser('~/cross/tricore/bin/tricore-elf-objcopy'), '-O', 'binary',
                               '-j', '.text', op_, os.path.join(d, 'a.bin')], capture_output=True)
        b = open(os.path.join(d, 'a.bin'), 'rb').read() if os.path.exists(os.path.join(d, 'a.bin')) else b''
        n = addr.get('E', 0) - addr.get('S', 0)
        word = int.from_bytes(b[addr.get('S', 0):addr.get('S', 0) + n], 'little')
        return i, (n, word, rs), None
    out = []
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        for i, res, why in ex.map(one, range(len(stmts))):
            ei, k, op, txt = stmts[i]
            o = {'entry': ei, 'mnem': T[ei]['mnem'], 'shape': T[ei]['shape'], 'field': k, 'op': op, 'text': txt}
            if res is None:
                o['refused'] = why
            else:
                o['len'], o['word'], o['relocs'] = res
            out.append(o)
    refused = [o for o in out if 'refused' in o]
    json.dump(out, open('relocs.json', 'w'), indent=0)
    acc = [o for o in out if 'refused' not in o]
    print('statements', len(stmts), 'accepted', len(acc), 'refused', len(refused))
    import collections
    c = collections.Counter()
    for o in acc:
        for off, t, s in o['relocs']:
            c[(o['op'], t)] += 1
    for k, v in sorted(c.items()):
        print('  %-8s %-24s %d' % (k[0], k[1], v))

if __name__ == '__main__':
    main()
