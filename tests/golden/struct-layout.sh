#!/bin/sh
# Struct, union and bit-field layout, on every target, against the reference
# compiler each target's data model was taken from.
#
# A layout difference is silent: the code that uses the struct agrees with
# itself, and only a struct shared with another compiler's object, laid over a
# register block or sent down a wire comes out a different shape. Checking it
# against a reference found three:
#
#   * AVR ALIGNMENT. Every type on AVR has alignment 1 -- avr-gcc and clang
#     agree -- and EmbCC aligned each scalar to its size, so
#     `struct { char c; int i; }` was four bytes where avr-gcc makes it three:
#     77 of 121 layout facts differed.
#   * AVR BIT-FIELDS. GCC's placement rules are written in the type's
#     ALIGNMENT, not its size: a `:0` rounds to the next alignment boundary and
#     a field may span as many alignment units as its type has
#     (stor-layout.c's excess_unit_span). With alignment 1 a 12-bit field of a
#     16-bit type straddles bytes; EmbCC started it on a fresh unit.
#   * UNNAMED BIT-FIELDS raise a struct's alignment on ARM (AAPCS, AAPCS64) and
#     do NOT on x86-64 SysV or RISC-V. EmbCC applied ARM's rule everywhere, so
#     on x86-64 `struct { char c; unsigned :4; char d; }` was four bytes where
#     GCC makes it three -- and `unsigned :4; // reserved` is how a hardware
#     register block is written.
#
# What is compared: the size and alignment of every struct, the offset of
# every named member, and -- for every bit-field -- the raw bytes of a zeroed
# object with that one field set to all ones, as a STATIC initializer, so the
# placement is in the object to be read. Each fact is read back by symbol from
# both objects.
set -u
echo "TEST-MARKER struct-layout"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/struct-layout
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}

python3 - "$out/probe.c" <<'PYEOF'
import sys
S = [
 ('a','char c; int i;'), ('b','char c; short s; char d;'), ('c','char c; long long ll;'),
 ('d','char c; double d;'), ('e','short s; char c[3];'), ('f','char c; void *p;'),
 ('g','int i; char c;'), ('h','long l; char c;'), ('i','char c; long double ld;'),
 ('j','char c; float f; char d;'), ('k','struct { char x; int y; } in; char z;'),
 ('l','char c; int arr[];'),
 ('m','unsigned a:3, b:5, c:8;'), ('n','unsigned a:1; unsigned b:31; unsigned c:1;', 4),
 ('o','char a:3; char b:6;'), ('p','unsigned a:4; unsigned :0; unsigned b:4;'),
 ('q','unsigned short a:12; unsigned short b:12;'), ('r','char c; unsigned a:7;'),
 ('s','int a:3; long long b:40;'), ('t','unsigned char a:1; unsigned short b:9; unsigned char c:7;'),
 ('u','unsigned a:5; unsigned char b; unsigned c:5;'), ('v','long long a:1; char c;'),
 ('w','unsigned a:31; unsigned b:2;', 4), ('x','char c; unsigned :0; char d;'),
 ('y','unsigned short a:8; unsigned char b:8; unsigned short c:4;'),
 ('z','unsigned long a:20; unsigned long b:20;'),
 ('A','signed char a:4; short b:9; int c:15;'), ('B','_Bool a:1; _Bool b:1; char c;'),
 ('C','struct { unsigned a:3; } inner; unsigned b:5;'),
 ('D','char c; struct { char x; } __attribute__((packed)) p; int i;'),
 ('E','char c; int i __attribute__((aligned(8)));'),
 ('F','char c; unsigned :4; char d;'), ('G','char c; unsigned long long :3; char d;'),
 ('H','char c; unsigned n:4; char d;'),
 ('I','char c; int i; short s;', 0, 'packed'),
 ('J','unsigned a:3; unsigned b:7; unsigned c:30;', 4, 'packed'),
 ('K','enum { E0, E1, E2 } e; char c;'),
 ('L','union { unsigned a:3; unsigned char b; } u; char c;'),
 ('M','char c; struct { char x; short y; } arr[2];'),
]
offs = {'a':'ci','b':'csd','c':['c','ll'],'d':'cd','e':'sc','f':'cp','g':'ic','h':'lc','i':['c','ld'],
        'j':'cfd','k':['in','z'],'l':['c','arr'],'r':'c','u':'b','x':'cd','v':'c','B':'c','D':'pi',
        'E':'i','F':'cd','G':'cd','H':'cd','I':'cis','K':'ec','L':'uc','M':['c','arr']}
bf = {'m':'abc','n':'abc','o':'ab','p':'ab','q':'ab','r':'a','s':'ab','t':'abc','u':'ac','v':'a',
      'w':'ab','y':'abc','z':'ab','A':'abc','B':'ab','H':'n','J':'abc'}
o = ['#include <stddef.h>', 'typedef unsigned long long v;']
for st in S:
    k, body = st[0], st[1]
    minint = st[2] if len(st) > 2 else 0
    attr = ' __attribute__((%s))' % st[3] if len(st) > 3 else ''
    if minint: o.append('#if __SIZEOF_INT__ >= %d' % minint)
    o.append('struct S%s { %s }%s;' % (k, body, attr))
    o.append('v sz_%s = sizeof(struct S%s); v al_%s = _Alignof(struct S%s);' % (k, k, k, k))
    for m in offs.get(k, []):
        o.append('v off_%s_%s = offsetof(struct S%s, %s);' % (k, m, k, m))
    for f in bf.get(k, ''):
        o.append('union U%s_%s { struct S%s s; unsigned char b[sizeof(struct S%s)]; } '
                 'bits_%s_%s = { .s = { .%s = -1 } };' % (k, f, k, k, k, f, f))
    if minint: o.append('#endif')
open(sys.argv[1], 'w').write('\n'.join(o) + '\n')
PYEOF

# name=bytes(hex) for every data symbol -- to a FILE (llvm-objcopy cannot
# write a pipe), and refusing to answer if it read nothing real.
dump() {
    python3 - "$1" <<'PYEOF'
import sys, subprocess, tempfile, os
obj = sys.argv[1]
nm = subprocess.run(['llvm-nm', '-S', '--defined-only', obj], capture_output=True, text=True).stdout
fd, tmp = tempfile.mkstemp(); os.close(fd)
subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.data', obj, tmp], check=True)
raw = open(tmp, 'rb').read(); os.unlink(tmp)
out = {}
for line in nm.splitlines():
    p = line.split()
    if len(p) != 4: continue
    off, size, kind, name = int(p[0], 16), int(p[1], 16), p[2], p[3].lstrip('_')
    if kind in 'Bb': out[name] = '00' * size
    elif kind in 'Dd': out[name] = raw[off:off + size].hex()
# struct Sa is at least two bytes on every target, so its size cannot read 0.
if int.from_bytes(bytes.fromhex(out.get('sz_a', '')), 'little') < 2:
    sys.stderr.write('dump: sizeof(struct Sa) read back as %r -- not reading\n' % out.get('sz_a'))
    sys.exit(1)
for n in sorted(out): print('%s=%s' % (n, out[n]))
PYEOF
}

n=0
for pair in "x86_64-elf|x86_64-elf-gcc" "aarch64-elf|aarch64-elf-gcc" \
            "thumbv7m-none-eabi|clang -target thumbv7m-none-eabi" \
            "thumbv7em-none-eabi|clang -target thumbv7em-none-eabi" \
            "thumbv8m.main-none-eabi|clang -target thumbv8m.main-none-eabi -mfloat-abi=soft" \
            "riscv32-unknown-elf|clang -target riscv32-unknown-elf" \
            "riscv64-unknown-elf|clang -target riscv64-unknown-elf" \
            "avr|clang -target avr -mmcu=atmega328p"; do
    et=${pair%%|*}; ref=${pair#*|}
    command -v "${ref%% *}" >/dev/null 2>&1 || {
        echo "SKIP $et: its reference compiler (${ref%% *}) is not installed"; continue; }
    # shellcheck disable=SC2086
    $ref -ffreestanding -w -c "$out/probe.c" -o "$out/r.o" 2> "$out/r.err" || {
        echo "$et: the reference could not compile the probe:"; head -3 "$out/r.err"; exit 1; }
    "$EMBCC" --target="$et" -c "$out/probe.c" -o "$out/e.o" 2> "$out/e.err" || {
        echo "$et: EmbCC could not compile the probe:"; head -3 "$out/e.err"; exit 1; }
    dump "$out/r.o" > "$out/r.$et" || exit 1
    dump "$out/e.o" > "$out/e.$et" || exit 1
    if ! cmp -s "$out/r.$et" "$out/e.$et"; then
        echo "$et: a layout differs from the reference compiler's:"
        diff "$out/r.$et" "$out/e.$et" | grep '^[<>]' | head -12
        exit 1
    fi
    n=$((n + $(wc -l < "$out/r.$et")))
done
echo "struct, union and bit-field layout agrees with each target's reference
compiler on all eight targets -- $n facts: sizes, alignments, member offsets,
and the exact bits every bit-field occupies"
