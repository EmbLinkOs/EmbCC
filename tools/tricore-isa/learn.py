#!/usr/bin/env python3
"""Learn TriCore encodings from GNU objdump, used as an oracle.

For every shape given (mnemonic + normalized operands + an example
encoding), flip each bit of the example and disassemble: a bit that changes
the mnemonic or the shape is an opcode bit, one that changes exactly one
operand belongs to that operand, one that changes nothing is reserved. Then,
from the example with all operand bits cleared, flip each operand bit alone:
how much the operand's value moves is that bit's weight. Output: isa.json.
"""
import json, re, subprocess, sys, os

OBJDUMP = os.path.expanduser("~/cross/tricore/bin/tricore-elf-objdump")
AS = os.path.expanduser("~/cross/tricore/bin/tricore-elf-as")
HERE = os.path.dirname(os.path.abspath(__file__))

def norm_shape(ops):
    s = re.sub(r' \(unknown SFR\)', '', ops)
    s = re.sub(r'%d(1[0-5]|[0-9])([lu]{0,2})\b', lambda m: 'D' + m.group(2), s)
    s = re.sub(r'%a(1[0-5]|[0-9])\b', 'A', s)
    s = s.replace("%sp", "A")
    s = re.sub(r'%e(1[0-4]|[02468])\b', 'E', s)
    s = re.sub(r'%p(1[0-4]|[02468])\b', 'P', s)
    s = re.sub(r'\$[a-z_][a-z0-9_]*|\$0x[0-9a-f]+', '$S', s)
    s = re.sub(r'-?0x[0-9a-f]+|-?\b[0-9]+\b', 'N', s)
    return s

# the operand atoms of an operand string, in order: registers, numbers, SFRs
ATOM = re.compile(r'%sp|%[dae p](?:1[0-5]|[0-9])(?:[lu]{0,2})?|%[dap](?:1[0-5]|[0-9])[lu]{0,2}|'
                  r'\$[a-z_][a-z0-9_]*|\$0x[0-9a-f]+|-?0x[0-9a-f]+|-?\b[0-9]+\b')

def atoms(ops):
    ops = re.sub(r' \(unknown SFR\)', '', ops)
    out = []
    for m in re.finditer(r'%sp|%[daep](?:1[0-5]|[0-9])[lu]{0,2}|\$[a-z_][a-z0-9_]*|\$0x[0-9a-f]+|-?0x[0-9a-f]+|-?\b[0-9]+\b', ops):
        out.append(m.group(0))
    return out

def atom_value(a, pc, is_target):
    if a == '%sp':
        return ('A', 10)
    m = re.match(r'%([daep])(\d+)([lu]*)$', a)
    if m:
        return (m.group(1).upper(), int(m.group(2)))
    if a.startswith('$'):
        if a.startswith('$0x'):
            return ('S', int(a[1:], 16))
        return ('S', a[1:])           # a named SFR; resolved by sfr map
    v = int(a, 0)
    if is_target:
        # a 32-bit address: the displacement wraps with it
        return ('L', ((v - pc + (1 << 31)) & 0xffffffff) - (1 << 31))
    return ('N', v)

def disasm(words):
    """words: list of (len, value). Returns per word (mnem, opsstr) or None,
    and each word's address.

    Each word is followed by a 16-bit nop: an invalid 32-bit word prints as
    `.hword` and objdump goes on two bytes later, where its upper half may
    begin a 32-bit instruction that swallows the start of the NEXT word --
    which then decoded as nothing, and the learner took the bit flipped in
    it for an opcode bit (div.u came out with every register fixed). The
    nop is what such a stray half swallows instead."""
    src = [' .text']
    pcs, at = [], 0
    for ln, v in words:
        src.append((' .short 0x%04x' if ln == 2 else ' .word 0x%08x') % v)
        src.append(' .short 0')
        pcs.append(at)
        at += ln + 2
    open('/tmp/tcl.s', 'w').write('\n'.join(src) + '\n')
    subprocess.run([AS, '-mtc162', '/tmp/tcl.s', '-o', '/tmp/tcl.o'], check=True)
    out = subprocess.run([OBJDUMP, '-d', '/tmp/tcl.o'], capture_output=True, text=True).stdout
    res = {}
    for l in out.splitlines():
        m = re.match(r'\s*([0-9a-f]+):\t([0-9a-f ]+)\t(\S+)\s*(.*)$', l.rstrip())
        if m:
            res[int(m.group(1), 16)] = (m.group(3), m.group(4).strip(), len(m.group(2).split()))
    r = []
    for (ln, v), pc in zip(words, pcs):
        d = res.get(pc)
        # (a decode longer than the word took the guard: not this word's)
        r.append(d[:2] if d and d[2] == ln else None)
    return r, pcs

BRANCHY = re.compile(r'^(j|jl|ja|jla|call|calla|loop|loopu|jeq|jne|jge|jlt|jgez|jgtz|jlez|jltz|jnz|jz|jned|jnei|jeq\.a|jne\.a|jz\.a|jnz\.a|jz\.t|jnz\.t|jge\.u|jlt\.u|fcall|fcalla)$')

ABSOLUTE = {'ja', 'jla', 'calla', 'fcalla'}

def is_target_slot(mnem, shape_atoms, k):
    # the last numeric operand of a pc-relative branch is its target: the
    # j*, loop*, call and fcall families, but not the absolute ones
    # (ja, jla, calla, fcalla) or the indirect (ji, jli, calli, fcalli)
    if mnem in ABSOLUTE or mnem in ('ji', 'jli', 'calli', 'fcalli'):
        return False
    if not (mnem.startswith('j') or mnem.startswith('loop') or
            mnem in ('call', 'fcall')):
        return False
    nums = [i for i, a in enumerate(shape_atoms) if a[0] in 'N']
    return bool(nums) and k == nums[-1]

def learn(shapes):
    table = []
    # pass 1: flips of each example
    batch, idx = [], []
    for si, sh in enumerate(shapes):
        ln = 2 if len(sh['hex']) == 2 else 4
        base = int.from_bytes(bytes(int(b, 16) for b in sh['hex']), 'little')
        sh['len'], sh['ex'] = ln, base
        batch.append((ln, base)); idx.append((si, -1))
        for b in range(1, ln * 8):
            batch.append((ln, base ^ (1 << b))); idx.append((si, b))
    dis, pcs = disasm(batch)
    info = {}
    for (si, b), d, pc in zip(idx, dis, pcs):
        info.setdefault(si, {})[b] = (d, pc)
    for si, sh in enumerate(shapes):
        d0, pc0 = info[si][-1]
        if not d0:
            continue
        mn, ops = d0
        a0 = atoms(ops)
        kinds = []
        for k, a in enumerate(a0):
            kinds.append(atom_value(a, pc0, False)[0])
        opbits, slots, reserved = 0, {k: [] for k in range(len(a0))}, 0
        for b in range(1, sh['len'] * 8):
            d, pc = info[si][b]
            if not d or d[0] != mn or norm_shape(d[1]) != norm_shape(ops):
                opbits |= 1 << b
                continue
            a1 = atoms(d[1])
            # (a branch's target compared pc-relative: each variant sits
            # at its own address)
            def val(a, p, k):
                return atom_value(a, p, is_target_slot(mn, kinds, k))
            diff = [k for k in range(len(a0))
                    if val(a0[k], pc0, k) != val(a1[k], pc, k)] if len(a1) == len(a0) else None
            if diff is None:
                opbits |= 1 << b
            elif not diff:
                reserved |= 1 << b
            elif len(diff) == 1:
                slots[diff[0]].append(b)
            else:
                # one bit moving two operands is another form's: an opcode bit
                opbits |= 1 << b
        sh['opbits'], sh['reserved'], sh['slots'], sh['kinds'] = opbits, reserved, slots, kinds
        sh['atoms0'] = a0
    # pass 2: weights from the example with every operand bit cleared
    batch, idx = [], []
    for si, sh in enumerate(shapes):
        if 'slots' not in sh:
            continue
        clear = sh['ex']
        for k, bits in sh['slots'].items():
            for b in bits:
                clear &= ~(1 << b)
        sh['zero'] = clear
        sh['zero_is_ex'] = 0
        batch.append((sh['len'], clear)); idx.append((si, None, -1))
        for k, bits in sh['slots'].items():
            for b in bits:
                batch.append((sh['len'], clear | (1 << b))); idx.append((si, k, b))
    # a zero form that prints another shape (an offset of 0 left out):
    # weights are measured from the example instead
    zb = [(sh['len'], sh['zero']) for sh in shapes if 'zero' in sh]
    zs = [sh for sh in shapes if 'zero' in sh]
    zd, _ = disasm(zb)
    redo = False
    for sh, d in zip(zs, zd):
        if not d or d[0] != sh['mnem0'] or norm_shape(d[1]) != sh['shape']:
            sh['zero'] = sh['ex']
            sh['zero_is_ex'] = 1
            redo = True
    if redo:
        batch, idx = [], []
        for si, sh in enumerate(shapes):
            if 'zero' not in sh:
                continue
            batch.append((sh['len'], sh['zero'])); idx.append((si, None, -1))
            for k, bits in sh['slots'].items():
                for b in bits:
                    batch.append((sh['len'], sh['zero'] ^ (1 << b))); idx.append((si, k, b))
    dis, pcs = disasm(batch)
    res = {}
    for (si, k, b), d, pc in zip(idx, dis, pcs):
        res.setdefault(si, []).append((k, b, d, pc))
    for si, rows in res.items():
        sh = shapes[si]
        k0, b0, d0, pc0 = rows[0]
        if not d0:
            sh['bad'] = 'zero form does not decode'
            continue
        a0 = atoms(d0[1])
        mn = d0[0]
        tgt = [is_target_slot(mn, sh['kinds'], k) for k in range(len(a0))]
        v0 = [atom_value(a, pc0, tgt[k]) for k, a in enumerate(a0)]
        fields = []
        for k in range(len(a0)):
            fields.append({'kind': v0[k][0], 'v0': v0[k][1], 'bits': [], 'target': tgt[k]})
        for k, b, d, pc in rows[1:]:
            if not d or norm_shape(d[1]) != norm_shape(d0[1]):
                sh.setdefault('odd', []).append(b)
                continue
            a1 = atoms(d[1])
            if len(a1) != len(a0) or k >= len(a0):
                sys.stderr.write('atoms differ: %s %r vs %r\n' % (mn, d0[1], d[1]))
                sh.setdefault('odd', []).append(b)
                continue
            v1 = atom_value(a1[k], pc, tgt[k])[1]
            if isinstance(v1, str) or isinstance(v0[k][1], str):
                fields[k]['bits'].append([b, None, a1[k]])
            else:
                dv = v1 - v0[k][1]
                if sh.get('zero_is_ex') and (sh['zero'] >> b) & 1:
                    dv = -dv          # the bit was set: clearing it moved it
                fields[k]['bits'].append([b, dv])
        sh['fields'] = fields
        sh['mnem'] = mn
        table.append(sh)
    return table

def main():
    shapes = []
    for l in open(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'valid.shapes')):
        p = l.split()
        # mnem shape count hexbytes... example ops
        mn = p[0]
        rest = l[len(mn):].strip()
        m = re.match(r'(\S*)\s+(\d+)\s+((?:[0-9a-f]{2} )+[0-9a-f]{2}|[0-9a-f]{2} [0-9a-f]{2})\s+(.*)$', rest)
        if not m:
            m = re.match(r'()(\d+)\s+((?:[0-9a-f]{2} ?)+)\s*(.*)$', rest)
        if not m:
            continue
        hexb = m.group(3).split()
        if len(hexb) not in (2, 4):
            continue
        shapes.append({'mnem0': mn, 'shape': m.group(1), 'hex': hexb, 'exops': m.group(4)})
    t = learn(shapes)
    json.dump(t, open(sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'isa.json'), 'w'), indent=0)
    print(len(t), 'shapes learned of', len(shapes))

if __name__ == '__main__':
    main()
