#!/bin/sh
# Static data on MIPS64, byte for byte against clang, in both byte orders.
#
# tests/golden/mips64-data.c is tests/golden/be-data.c's initialisers --
# integers of every width, floats, doubles, long double, complex,
# bit-fields across units, unions through one member, nested aggregates
# with padding, narrow and wide strings, pointers with addends -- at n64's
# LP64, and what only a 64-bit target has: __int128 objects, binary128
# long doubles, a packed __int128 bit-field over a 17-byte unit, and
# pointers as eight-byte fields. It is compiled by EmbCC for
# mips64el-none-elf and mips64-none-elf and by clang for the
# -unknown-elf triples (-G0 -fno-common, so nothing goes to .sdata), and
# for every object both define:
#
#   - the bytes must be identical, the relocated fields aside;
#   - the relocations must be the same: at the same offset in the object,
#     of the same type, against the same thing -- a global by name plus
#     an addend, or a string literal by its contents -- read from n64's
#     r_info record (symbol, then three types, in the object's order);
#   - both objects must be ELFCLASS64 EM_MIPS for mips64r2, in the
#     triple's byte order, with no o32 ABI field.
#
# Then the same program, linked by embld with the harness, prints the same
# values on the board (qemu-system-mips64el / mips64) from EmbCC's object
# at -O0 -O1 -O2 as from clang's: what the bytes MEAN to the code each
# compiler generated, read back the way a program reads them.
set -u
echo "TEST-MARKER mips64-data"
. "$(dirname "$0")/../lib.sh"

CLANG=${EMBCC_REF_CLANG_MIPS:-clang}
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=mips64-unknown-elf -mcpu=mips64r2 -msoft-float \
        -fsyntax-only -x c /dev/null 2>/dev/null || {
    echo "skipped: no clang with a MIPS64 target (set EMBCC_REF_CLANG_MIPS)"
    exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "skipped: no python3"; exit 0; }

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/mips64-data
rm -rf "$out"; mkdir -p "$out"
src=tests/golden/mips64-data.c

cat > "$out/cmp.py" <<'PY'
import struct, sys

def load(path):
    b = open(path, 'rb').read()
    if b[:4] != b'\x7fELF' or b[4] != 2:
        sys.exit('%s: not ELF64' % path)
    e = '>' if b[5] == 2 else '<'
    (etype, mach, ver, entry, phoff, shoff, flags, ehsz, phes, phn, shes,
     shn, shstrndx) = struct.unpack(e + 'HHIQQQIHHHHHH', b[16:64])
    secs = []
    for i in range(shn):
        f = struct.unpack(e + 'IIQQQQIIQQ', b[shoff + 64 * i:shoff + 64 * i + 64])
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
            for k in range(s['size'] // 24):
                n, info, oth, ndx, v, sz = struct.unpack(e + 'IBBHQQ', s['data'][24 * k:24 * k + 24])
                syms.append(dict(name=cstr(st['off'] + n) if n else '', value=v,
                                 size=sz, type=info & 15, bind=info >> 4, shndx=ndx))
    rels = []
    for s in secs:
        if s['type'] == 4:                   # SHT_RELA
            for k in range(s['size'] // 24):
                off, = struct.unpack(e + 'Q', s['data'][24 * k:24 * k + 8])
                ri = s['data'][24 * k + 8:24 * k + 16]
                add, = struct.unpack(e + 'q', s['data'][24 * k + 16:24 * k + 24])
                # n64: r_sym (4 bytes, the object's order), r_ssym,
                # r_type3, r_type2, r_type
                sym, = struct.unpack(e + 'I', ri[0:4])
                if ri[4] or ri[5] or ri[6]:
                    sys.exit('%s: a composite relocation %r' % (path, ri))
                rels.append(dict(sec=s['info'], off=off, sym=sym, type=ri[7],
                                 addend=add))
        elif s['type'] == 9:
            sys.exit('%s: a REL section in an n64 object' % path)
    return dict(e=e, mach=mach, flags=flags, data5=b[5], secs=secs,
                syms=syms, rels=rels)

def anon(name):
    return name.startswith('$') or name.startswith('.L')

def target(o, s, a):
    if s['type'] != 3 and s['name'] and not anon(s['name']):
        return '%s%+d' % (s['name'], a)
    if s['type'] != 3:
        a += s['value']
    sec = o['secs'][s['shndx']]
    for y in o['syms']:
        if y['shndx'] == s['shndx'] and y['type'] == 1 and y['name'] and \
           not anon(y['name']) and \
           y['value'] <= a < y['value'] + max(y['size'], 1):
            return '%s%+d' % (y['name'], a - y['value'])
    d = sec['data']
    return 'string %r' % d[a:d.index(b'\0', a)]

def objects(o):
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
            n = 8 if r['type'] == 18 else 4    # R_MIPS_64, else R_MIPS_32
            img[at:at + n] = bytes(n)
            rl.append((at, r['type'], target(o, o['syms'][r['sym']], r['addend'])))
        out[y['name']] = (bytes(img), sorted(rl))
    return out

ref, got = load(sys.argv[1]), load(sys.argv[2])
want_be = sys.argv[3] == 'be'
bad = 0
for o, w in ((ref, 'clang'), (got, 'EmbCC')):
    if (o['data5'] == 2) != want_be or o['mach'] != 8 or \
       (o['flags'] & 0xf000) != 0 or (o['flags'] & 0xf0000000) != 0x80000000:
        print('%s: not a %s-endian n64 mips64r2 object (e_flags 0x%x)' %
              (w, 'big' if want_be else 'little', o['flags'])); bad = 1
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

for bo in el be; do
    if [ $bo = be ]; then
        T=mips64-none-elf CT=mips64-unknown-elf
        QEMU=${EMBCC_QEMU_MIPS64EB:-qemu-system-mips64}
    else
        T=mips64el-none-elf CT=mips64el-unknown-elf
        QEMU=${EMBCC_QEMU_MIPS64:-qemu-system-mips64el}
    fi
    d="$out/$bo"; mkdir -p "$d"
    "$CLANG" --target=$CT -mcpu=mips64r2 -msoft-float -mno-abicalls -G0 \
        -fno-common -ffreestanding -isystem lib/libc/include -O1 -w \
        -I tests/golden -c "$src" -o "$d/clang.o" ||
        { echo "clang cannot compile $src for $CT"; exit 1; }
    for opt in -O0 -O1 -O2; do
        "$EMBCC" --target=$T $opt -I tests/golden -c "$src" -o "$d/embcc$opt.o" ||
            { echo "EmbCC cannot compile $src at $opt for $T"; exit 1; }
        python3 "$out/cmp.py" "$d/clang.o" "$d/embcc$opt.o" $bo \
            > "$d/cmp$opt.txt" 2>&1 || {
            echo "FAIL: EmbCC's $opt object's data ($T) differs from clang's:"
            head -30 "$d/cmp$opt.txt"; exit 1; }
    done
    echo "$T data: $(cat "$d/cmp-O0.txt") (at -O0, -O1 and -O2)"

    command -v "$QEMU" >/dev/null 2>&1 || {
        echo "(no $QEMU: the bytes are checked, the program is not run)"
        continue; }
    EMBCC="$EMBCC" sh tools/build-rt.sh $T "$d/lib" ||
        { echo "lib/rt does not build for $T"; exit 1; }
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c "tests/harness/mips/$f.c" -o "$d/$f.o" ||
            { echo "the harness does not compile"; exit 1; }
    done
    for o in clang.o embcc-O0.o embcc-O1.o embcc-O2.o; do
        EMBCC_MIPS64_HARNESS="$PWD/$d" EMBLD="$EMBLD" \
            sh tests/harness/mips64/link.sh "$d/$o.elf" "$d/$o" \
            "$d/lib/librt.a" > "$d/$o.lerr" 2>&1 || {
            echo "embld cannot link $o:"; head -4 "$d/$o.lerr"; exit 1; }
        sh tests/harness/mips64/run.sh "$d/$o.elf" > "$d/$o.txt" 2>&1
        grep -q '==EXIT 0 ==' "$d/$o.txt" || {
            echo "$o ($T) did not run to the end:"; head -6 "$d/$o.txt"; exit 1; }
    done
    for o in embcc-O0.o embcc-O1.o embcc-O2.o; do
        diff -u "$d/clang.o.txt" "$d/$o.txt" > "$d/$o.diff" || {
            echo "FAIL: $o ($T) prints other values than clang's object on the board:"
            head -20 "$d/$o.diff"; exit 1; }
    done
    echo "$T: the board prints the same values from EmbCC's objects as from clang's"
done
echo "MIPS64 static data agrees with clang's in both byte orders"
