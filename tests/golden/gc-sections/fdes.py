#!/usr/bin/env python3
# Print each FDE of a linked x86-64 image's .eh_frame as "BEGIN RANGE",
# found by the __eh_frame_start/__eh_frame_end symbols EmbLD defines,
# decoded here rather than by the unwinder under test. Assumes the
# encoding EmbCC's CIEs use (pcrel|sdata4 FDE pointers), and says so.
import struct, sys

data = open(sys.argv[1], 'rb').read()
assert data[:4] == b'\x7fELF' and data[4] == 2, 'an ELF64 image'
shoff, = struct.unpack_from('<Q', data, 0x28)
phoff, = struct.unpack_from('<Q', data, 0x20)
phnum, = struct.unpack_from('<H', data, 0x38)
shnum, = struct.unpack_from('<H', data, 0x3c)
loads = []
for i in range(phnum):
    t, f, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', data, phoff + 56 * i)
    if t == 1:
        loads.append((va, off, fsz))
def at(va):
    for v, o, n in loads:
        if v <= va < v + n:
            return o + (va - v)
    raise SystemExit('address %#x is in no loaded segment' % va)
syms = {}
for i in range(shnum):
    name, typ, flags, addr, off, size, link, info, al, ent = struct.unpack_from('<IIQQQQIIQQ', data, shoff + 64 * i)
    if typ == 2:
        soff = struct.unpack_from('<Q', data, shoff + 64 * link + 24)[0]
        for k in range(size // 24):
            n, inf, oth, ndx, val, sz = struct.unpack_from('<IBBHQQ', data, off + 24 * k)
            s = data[soff + n:data.index(b'\0', soff + n)].decode()
            syms[s] = val
lo, hi = syms['__eh_frame_start'], syms['__eh_frame_end']
p, end = at(lo), at(lo) + (hi - lo)
while p + 8 <= end:
    ln, cid = struct.unpack_from('<II', data, p)
    if ln == 0:
        break
    if cid != 0:
        rel, rng = struct.unpack_from('<iI', data, p + 8)
        va = lo + (p + 8 - at(lo))
        print('%x %x' % ((va + rel) & 0xffffffffffffffff, rng))
    p += 4 + ln
