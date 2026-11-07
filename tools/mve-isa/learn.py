#!/usr/bin/env python3
"""Learn A32 NEON forms from llvm-mc, as tools/tricore-isa learns TriCore
from GNU objdump: each bit of an example flipped and decoded -- an opcode
bit changes the mnemonic or the shape, an operand bit one operand, a
reserved bit nothing -- then each operand bit's weight measured from the
word with every operand bit clear. A group's examples its opcode covers
are dropped and the rest learned again. Output: isa.json."""
import json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle import disasm
from shapes import parse

def vfp_imm(imm8, f64=False):
    a, b, cdefgh = imm8 >> 7, (imm8 >> 6) & 1, imm8 & 0x3f
    if f64:
        bits = (a << 63) | ((b ^ 1) << 62) | ((0xff if b else 0) << 54) | (cdefgh << 48)
        return struct.unpack('<d', struct.pack('<Q', bits))[0]
    bits = (a << 31) | ((b ^ 1) << 30) | ((0x1f if b else 0) << 25) | (cdefgh << 19)
    return struct.unpack('<f', struct.pack('<I', bits))[0]
F32 = {vfp_imm(k): k for k in range(256)}

def linear(mnem, atoms):
    """atoms with the non-linear immediates as the 8 bits they encode: a
    float as VFPExpandImm's input, an .i64 byte mask as its byte bits"""
    out = []
    for k, v in atoms:
        if k == 'F':
            out.append(('F', F32.get(v, -1)))
        elif k == 'N' and mnem == 'vmov.i64' and isinstance(v, int):
            bs = [(v >> (8 * j)) & 0xff for j in range(8)]
            if all(x in (0, 0xff) for x in bs):
                out.append(('N', sum(1 << j for j in range(8) if bs[j])))
            else:
                out.append(('N', -1))
        else:
            out.append((k, v))
    return out

def decode(words):
    res = []
    for d in disasm(words):
        if not d:
            res.append(None)
            continue
        p = parse(d[1])
        res.append((d[0], p[0], linear(d[0], p[1])) if p else None)
    return res

def learn(batch):
    """batch: [(mnem, shape, word)]. Returns entries."""
    flips = []
    for mn, sh, w in batch:
        flips.append(w)
        flips += [w ^ (1 << b) for b in range(32)]
    dec = decode(flips)
    ents = []
    pending = []
    for i, (mn, sh, w) in enumerate(batch):
        d0 = dec[i * 33]
        if not d0 or d0[0] != mn or d0[1] != sh:
            continue
        a0 = d0[2]
        op, res, slots = 0, 0, {k: [] for k in range(len(a0))}
        for b in range(32):
            d = dec[i * 33 + 1 + b]
            if not d or d[0] != mn or d[1] != sh or len(d[2]) != len(a0):
                op |= 1 << b
                continue
            diff = [k for k in range(len(a0)) if d[2][k] != a0[k]]
            if not diff:
                res |= 1 << b
            elif len(diff) == 1:
                # an operand bit moves its operand by a power of two; one
                # that moves it otherwise picks another form of it (an
                # immediate's cmode: x<<8 becomes x<<16)
                dv = d[2][diff[0]][1] - a0[diff[0]][1]
                if dv and (abs(dv) & (abs(dv) - 1)) == 0:
                    slots[diff[0]].append(b)
                else:
                    op |= 1 << b
            else:
                op |= 1 << b
        clear = w
        for k, bs in slots.items():
            for b in bs:
                clear &= ~(1 << b)
        pending.append((mn, sh, w, op, res, slots, a0, clear))
    # weights: from the cleared word, or from the example when the cleared
    # word is another shape (an offset of zero printed away)
    words = []
    for mn, sh, w, op, res, slots, a0, clear in pending:
        words.append(clear)
        for k, bs in slots.items():
            for b in bs:
                words.append(clear | (1 << b))
    dec = decode(words)
    pos = 0
    redo = []
    for t in pending:
        mn, sh, w, op, res, slots, a0, clear = t
        n = 1 + sum(len(v) for v in slots.values())
        rows = dec[pos:pos + n]
        pos += n
        base = rows[0]
        if not base or base[0] != mn or base[1] != sh:
            redo.append(t)
            continue
        ents.append(fields(t, rows, clear, False))
    if redo:
        words = []
        for mn, sh, w, op, res, slots, a0, clear in redo:
            words.append(w)
            for k, bs in slots.items():
                for b in bs:
                    words.append(w ^ (1 << b))
        dec = decode(words)
        pos = 0
        for t in redo:
            n = 1 + sum(len(v) for v in t[5].values())
            ents.append(fields(t, dec[pos:pos + n], t[2], True))
            pos += n
    return [e for e in ents if e]

def fields(t, rows, zero, from_ex):
    mn, sh, w, op, res, slots, a0, clear = t
    d0 = rows[0]
    if not d0 or d0[0] != mn or d0[1] != sh:
        return None
    v0 = d0[2]
    fs = [{'kind': v0[k][0], 'v0': v0[k][1], 'bits': []} for k in range(len(v0))]
    r = 1
    for k, bs in slots.items():
        for b in bs:
            d = rows[r]
            r += 1
            if not d or d[0] != mn or d[1] != sh:
                continue
            dv = d[2][k][1] - v0[k][1]
            if from_ex and (zero >> b) & 1:
                dv = -dv
            fs[k]['bits'].append([b, dv])
    # the value with every field bit clear, when measured from the example
    if from_ex:
        for f in fs:
            f['v0'] -= sum(wt for b, wt in f['bits'] if (zero >> b) & 1)
    fm = 0
    for f in fs:
        good = [x for x in f['bits'] if x[1] and (abs(x[1]) & (abs(x[1]) - 1)) == 0]
        f['bits'] = good
        for b, _ in good:
            fm |= 1 << b
    base = w & ~fm & ~res
    return {'mnem': mn, 'shape': sh, 'base': base, 'fieldmask': fm, 'reserved': res,
            'fields': fs}

def main():
    G = json.load(open('groups.json'))
    groups = [g for g in G if not g['shape'].startswith('?')]
    remaining = {(g['mnem'], g['shape']): [e[0] for e in g['ex']] for g in groups}
    table = []
    for rnd in range(8):
        batch = [(k[0], k[1], v[0]) for k, v in remaining.items() if v]
        if not batch:
            break
        got = learn(batch)
        learned = {}
        for e in got:
            table.append(e)
            learned.setdefault((e['mnem'], e['shape']), []).append(e)
        for k, v in remaining.items():
            if not v:
                continue
            es = learned.get(k)
            if not es:
                v.pop(0)
                continue
            keep = []
            for x in v:
                if not any((x & ~e['fieldmask'] & ~e['reserved']) == e['base'] for e in es):
                    keep.append(x)
            remaining[k] = keep
        print('round', rnd, 'learned', len(got), 'left', sum(len(v) for v in remaining.values()))
    json.dump(table, open('isa.json', 'w'))
    print(len(table), 'entries,', len({(e['mnem'], e['shape']) for e in table}), 'forms of',
          len(groups), 'groups')

if __name__ == '__main__':
    main()
