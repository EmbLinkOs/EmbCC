#!/bin/sh
# PowerPC's static data, byte for byte against clang.
#
# tests/golden/be-data.c is all initialisers whose bytes depend on the byte
# order -- integers of every width, floats, doubles, long double, complex,
# bit-fields (across units, signed, zero-width, packed across a unit, a
# 64-bit unit), unions initialised through one member of several, nested
# aggregates with padding, narrow and wide strings, and pointers with
# addends. It is compiled by EmbCC and by clang for powerpc-none-eabi
# (-mcpu=e500 -mno-spe -msoft-float -mlong-double-64 -fno-common), and for
# every object both define:
#
#   - the bytes must be identical, the relocated fields aside;
#   - the relocations must be the same: at the same offset in the object,
#     of the same type, against the same thing -- a global by name plus
#     an addend, or a string literal by its contents (each compiler places
#     its literals differently) -- the addend from the RELA entry;
#   - both objects must say ELFDATA2MSB, EM_PPC and e_flags 0.
#
# Then the same program, linked by embld with the harness, prints the same
# values on the ppce500 board (qemu-system-ppc) from EmbCC's object at
# -O0 -O1 -O2 as from clang's: what the bytes MEAN to the code each compiler
# generated, read back the way a program reads them.
set -u
echo "TEST-MARKER ppc-data"
. "$(dirname "$0")/../lib.sh"

CLANG=${EMBCC_REF_CLANG_PPC:-clang}
CT=powerpc-none-eabi T=powerpc-none-eabi
REF="-mcpu=e500 -mno-spe -msoft-float -mlong-double-64"
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=$CT $REF -fsyntax-only \
        -x c /dev/null 2>/dev/null || {
    echo "skipped: no clang with a PowerPC target (set EMBCC_REF_CLANG_PPC)"
    exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "skipped: no python3"; exit 0; }

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/ppc-data
rm -rf "$out"; mkdir -p "$out"
src=tests/golden/be-data.c

"$CLANG" --target=$CT $REF \
    -fno-common -ffreestanding -isystem lib/libc/include -O1 -w \
    -c "$src" -o "$out/clang.o" || { echo "clang cannot compile $src"; exit 1; }
for opt in -O0 -O1 -O2; do
    "$EMBCC" --target=$T $opt -c "$src" -o "$out/embcc$opt.o" ||
        { echo "EmbCC cannot compile $src at $opt"; exit 1; }
done

cat > "$out/cmp.py" <<'PY'
import struct, sys

def load(path):
    b = open(path, 'rb').read()
    if b[:4] != b'\x7fELF' or b[4] != 1:
        sys.exit('%s: not ELF32' % path)
    e = '>' if b[5] == 2 else '<'
    (etype, mach, ver, entry, phoff, shoff, flags, ehsz, phes, phn, shes,
     shn, shstrndx) = struct.unpack(e + 'HHIIIIIHHHHHH', b[16:52])
    secs = []
    for i in range(shn):
        f = struct.unpack(e + 'IIIIIIIIII', b[shoff + 40 * i:shoff + 40 * i + 40])
        secs.append(dict(name=f[0], type=f[1], flags=f[2], off=f[4],
                         size=f[5], link=f[6], info=f[7]))
    shs = secs[shstrndx]
    def cstr(off):
        return b[off:b.index(b'\0', off)].decode()
    for s in secs:
        s['nm'] = cstr(shs['off'] + s['name'])
        s['data'] = b[s['off']:s['off'] + s['size']] if s['type'] != 8 else bytes(s['size'])
    syms = []
    for s in secs:
        if s['type'] == 2:
            st = secs[s['link']]
            for k in range(s['size'] // 16):
                n, v, sz, info, oth, ndx = struct.unpack(e + 'IIIBBH', s['data'][16 * k:16 * k + 16])
                syms.append(dict(name=cstr(st['off'] + n) if n else '', value=v,
                                 size=sz, type=info & 15, bind=info >> 4, shndx=ndx))
    rels = []
    for s in secs:
        if s['type'] in (9, 4):              # SHT_REL, SHT_RELA
            esz = 8 if s['type'] == 9 else 12
            for k in range(s['size'] // esz):
                ent = struct.unpack(e + ('II' if esz == 8 else 'IIi'), s['data'][esz * k:esz * k + esz])
                rels.append(dict(sec=s['info'], off=ent[0], sym=ent[1] >> 8,
                                 type=ent[1] & 255,
                                 addend=ent[2] if esz == 12 else None))
    return dict(e=e, mach=mach, flags=flags, data5=b[5], secs=secs,
                syms=syms, rels=rels)

def objects(o):
    """name -> (bytes with relocated fields zeroed, [relocations])"""
    out = {}
    for y in o['syms']:
        if y['type'] != 1 or y['shndx'] == 0 or y['shndx'] >= 0xff00 or \
           not y['name'] or anon(y['name']):
            continue
        sec = o['secs'][y['shndx']]
        img = bytearray(sec['data'][y['value']:y['value'] + y['size']])
        rl = []
        for r in o['rels']:
            if r['sec'] != y['shndx'] or not (y['value'] <= r['off'] < y['value'] + y['size']):
                continue
            at = r['off'] - y['value']
            a = r['addend']
            if a is None:                    # REL: the addend is the field
                a = struct.unpack(o['e'] + 'i', bytes(img[at:at + 4]))[0]
            img[at:at + 4] = b'\0\0\0\0'
            rl.append((at, r['type'], target(o, o['syms'][r['sym']], a)))
        out[y['name']] = (bytes(img), sorted(rl))
    return out

def anon(name):
    # an assembler-local label: clang's string literals ($.str, .L.str)
    return name.startswith('$') or name.startswith('.L')

def target(o, s, a):
    if s['type'] != 3 and s['name'] and not anon(s['name']):
        return '%s%+d' % (s['name'], a)       # a named symbol, not a section
    if s['type'] != 3:                        # a local label: its section
        a += s['value']
    sec = o['secs'][s['shndx']]
    for y in o['syms']:                       # a named object covering it
        if y['shndx'] == s['shndx'] and y['type'] == 1 and y['name'] and \
           not anon(y['name']) and \
           y['value'] <= a < y['value'] + max(y['size'], 1):
            return '%s%+d' % (y['name'], a - y['value'])
    d = sec['data']                           # an anonymous string literal
    return 'string %r' % d[a:d.index(b'\0', a)]

ref, got = load(sys.argv[1]), load(sys.argv[2])
bad = 0
for o, w in ((ref, 'clang'), (got, 'EmbCC')):
    if o['data5'] != 2 or o['mach'] != 20 or o['flags'] != 0:
        print('%s: not a big-endian EM_PPC object with e_flags 0' % w); bad = 1
ro, go = objects(ref), objects(got)
if set(ro) != set(go):
    print('the objects define different data: clang only %s, EmbCC only %s'
          % (sorted(set(ro) - set(go)), sorted(set(go) - set(ro)))); bad = 1
n = 0
for name in sorted(set(ro) & set(go)):
    rb, rr = ro[name]
    gb, gr = go[name]
    n += 1
    if rb != gb:
        bad = 1
        print('%s: bytes differ\n  clang %s\n  EmbCC %s' % (name, rb.hex(), gb.hex()))
    if rr != gr:
        bad = 1
        print('%s: relocations differ\n  clang %s\n  EmbCC %s' % (name, rr, gr))
if bad:
    sys.exit(1)
print('%d objects, %d bytes and %d relocations identical' %
      (n, sum(len(v[0]) for v in go.values()), sum(len(v[1]) for v in go.values())))
PY

for opt in -O0 -O1 -O2; do
    python3 "$out/cmp.py" "$out/clang.o" "$out/embcc$opt.o" > "$out/cmp$opt.txt" 2>&1 || {
        echo "FAIL: EmbCC's $opt object's data differs from clang's:"
        head -30 "$out/cmp$opt.txt"; exit 1; }
done
echo "data: $(cat "$out/cmp-O0.txt") (at -O0, -O1 and -O2)"

# The debug information big-endian: every DWARF field in the target's
# order (llvm-dwarfdump --verify reads a unit length written backwards as
# a unit four gigabytes long), and a bit-field's DW_AT_data_bit_offset in
# memory order, as clang gives it.
DD=${EMBCC_LLVM_DWARFDUMP:-llvm-dwarfdump}
if command -v "$DD" >/dev/null 2>&1; then
    printf '%s\n' 'struct bf1 { unsigned a : 3, b : 5, c : 9, d : 15; };' \
        'int f(struct bf1 *p) { struct bf1 x = *p; return x.c + x.d; }' \
        > "$out/dbg.c"
    "$EMBCC" --target=$T -g -O0 -c "$out/dbg.c" -o "$out/dbg.o" &&
    "$EMBCC" --target=$T -g -O1 -c "$src" -o "$out/dbg2.o" || {
        echo "EmbCC cannot compile with -g"; exit 1; }
    for o in dbg dbg2; do
        "$DD" --verify "$out/$o.o" > "$out/$o.verify" 2>&1 || {
            echo "FAIL: the -g object's DWARF does not verify:"
            grep -i 'error\|warning' "$out/$o.verify" | head -4; exit 1; }
    done
    offs=$("$DD" --debug-info "$out/dbg.o" |
           sed -n 's/.*DW_AT_data_bit_offset.*(0x0*\([0-9a-f]*\)).*/\1/p' |
           tr '\n' ' ')
    [ "$offs" = " 3 8 11 " ] || [ "$offs" = "0 3 8 11 " ] || {
        echo "FAIL: bit-field DW_AT_data_bit_offset '$offs', clang's is 0 3 8 0x11"
        exit 1; }
    echo "-g: the DWARF verifies, and bit-field offsets are clang's"
fi

# On the board, when there is one.
QEMU=${EMBCC_QEMU_PPC:-qemu-system-ppc}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "(no $QEMU: the bytes are checked, the program is not run)"; exit 0; }
EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" ||
    { echo "lib/rt does not build for $T"; exit 1; }
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "tests/harness/ppc/$f.c" -o "$out/$f.o" ||
        { echo "the harness does not compile"; exit 1; }
done
export EMBCC_PPC_HARNESS="$PWD/$out"
for o in clang.o embcc-O0.o embcc-O1.o embcc-O2.o; do
    sh tests/harness/ppc/link.sh "$out/$o.elf" "$out/$o" "$out/lib/librt.a" \
        > "$out/$o.lerr" 2>&1 || {
        echo "embld cannot link $o:"; head -4 "$out/$o.lerr"; exit 1; }
    sh tests/harness/ppc/run.sh "$out/$o.elf" > "$out/$o.txt" 2>&1
    grep -q '==EXIT 0 ==' "$out/$o.txt" || {
        echo "$o did not run to the end:"; head -6 "$out/$o.txt"; exit 1; }
done
for o in embcc-O0.o embcc-O1.o embcc-O2.o; do
    diff -u "$out/clang.o.txt" "$out/$o.txt" > "$out/$o.diff" || {
        echo "FAIL: $o prints other values than clang's object on the board:"
        head -20 "$out/$o.diff"; exit 1; }
done
echo "the board prints the same values from EmbCC's objects as from clang's"
