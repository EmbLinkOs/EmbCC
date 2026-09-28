#!/bin/sh
# Integer constants and constant expressions, on every target, against the
# reference compiler.
#
# Three things were wrong, and nothing compared any of them with anything:
#
#   * LITERAL TYPES. The lexer typed a constant with the HOST's INT_MAX and
#     LONG_MAX and a literal 0xffffffff, in three hand-kept copies, then
#     patched ILP32's long. So `4294967295U` and `0xffffffffU` were unsigned
#     long (a U constant takes unsigned int when that holds it), and on AVR,
#     where int is sixteen bits, 32768, 40000, 0x10000 and 65536U were all
#     `int` -- sizeof(40000) was 2. One function now implements C11 6.4.4.1
#     with the target's widths.
#
#   * THE STATIC-INITIALIZER FOLDER computed in the host's 64-bit long and
#     never reduced a result to its type: `unsigned long long x = 0u - 1;`
#     stored 0xffffffffffffffff, and `0xffffffffu + 1u` was 4294967296 where
#     it is 0 -- while the same expression at run time was right. It also
#     could not fold a comparison, so `int a = (1 < 2);` at file scope was
#     refused as "not a constant".
#
#   * THE PARSE-TIME FOLDER, which _Static_assert uses, had the same flaw and
#     no types at all: `_Static_assert(0u - 1 == 4294967295u, "")` failed on
#     every 32-bit-int target. A correct program was rejected.
#
# Each expression is checked TWICE: as a static initializer, whose value is
# read back from the object by symbol and compared with clang's; and as a
# _Static_assert that it equals clang's value, which EmbCC must accept. The
# expressions avoid anything C leaves undefined -- signed overflow, shifts by
# the width -- so there is one right answer for each.
set -u
echo "TEST-MARKER int-constants"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/int-constants
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
command -v clang >/dev/null 2>&1 || { echo "SKIP: no clang to referee"; exit 0; }

python3 - "$out/exprs" <<'PYEOF'
import sys
lits = ['32767','32768','65535','65536','2147483647','2147483648','4294967295','4294967296',
        '0x7fff','0x8000','0xffff','0x10000','0x7fffffff','0x80000000','0xffffffff','0x100000000',
        '32768U','65536U','2147483648U','4294967295U','4294967296U','0xffffffffU',
        '2147483648L','4294967295L','0x80000000L','0xffffffffL','4294967295UL',
        '9223372036854775807','9223372036854775808U','0x8000000000000000','0xffffffffffffffff',
        '1LL','0x7fffffffLL','0xffffffffULL','40000','40000U','0xfffe','0177777','017777777777',
        '0b1111111111111111']
ex = []
for l in lits:
    ex.append('sizeof(%s)' % l)
    ex.append('%s - %s - 1' % (l, l))           # -1 in the literal's own type
ex += ['0u - 1', '0ul - 1', '0ull - 1', '0xffffffffu + 1u', '65535u + 1u', '-1u', '~0u', '~0ul',
       '-1 / 2u', '-1 < 0u', '-1L < 0u', '-1 < 0ul', '(1u << 15) >> 15', '1u << 15',
       '0xffffffffffffffffull / 2', '0xffffffffffffffffull % 10', '0xffffffffffffffffull >> 63',
       '(unsigned char)300', '(signed char)200', '(short)40000', '(unsigned short)70000',
       '(1 < 2)', '(2 == 2)', '(3 != 3)', '(-1 >> 1)', '(unsigned char)-1 + 1', '(signed char)-1 + 1u',
       'sizeof(1 ? 1 : 1u)', 'sizeof(1 ? 1u : 1L)', '0x7fff + 0x7fff', '65535u * 65535u',
       # integer promotion, and the conversions between long and unsigned int:
       # both decided by the same functions sema uses for code, so a wrong
       # answer here is a wrong compare at run time. On AVR unsigned short
       # promotes to UNSIGNED int; on ILP32 long with unsigned int is UNSIGNED long.
       '(unsigned short)1 < -1', '(unsigned short)65535 < -1', '(unsigned short)0 - 1',
       'sizeof((unsigned short)1 + 0)', '(unsigned char)0 - 1', 'sizeof(1L + 1u)',
       '-1L + 0u', '-1L < 1u', '(long)-1 < (unsigned)1', '-1LL < 0u', '-1 < 0UL']
open(sys.argv[1], 'w').write('\n'.join(ex) + '\n')
PYEOF

# The initializer probe: one 8-byte global per expression.
python3 - "$out/exprs" "$out/init.c" <<'PYEOF'
import sys
ex = open(sys.argv[1]).read().splitlines()
o = ['typedef unsigned long long v;']
o += ['v e%d = (v)(%s);' % (k, e) for k, e in enumerate(ex)]
open(sys.argv[2], 'w').write('\n'.join(o) + '\n')
PYEOF

# name=value for every probe global -- to a FILE, never a pipe (llvm-objcopy
# cannot write one, and a reader that reads nothing agrees with itself).
facts() {
    python3 - "$1" <<'PYEOF'
import sys, subprocess, tempfile, os
obj = sys.argv[1]
nm = subprocess.run(['llvm-nm', '--defined-only', obj], capture_output=True, text=True).stdout
fd, tmp = tempfile.mkstemp(); os.close(fd)
subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.data', obj, tmp], check=True)
raw = open(tmp, 'rb').read(); os.unlink(tmp)
f = {}
for line in nm.splitlines():
    p = line.split()
    if len(p) != 3: continue
    n = p[2].lstrip('_')
    if p[1] in 'Bb': f[n] = 0
    elif p[1] in 'Dd':
        o = int(p[0], 16); f[n] = int.from_bytes(raw[o:o + 8], 'little')
if f.get('e0') is None or all(v == 0 for v in f.values()):
    sys.stderr.write('facts: read nothing useful from %s\n' % obj); sys.exit(1)
for n in sorted(f, key=lambda s: int(s[1:])): print('%s=%d' % (n, f[n]))
PYEOF
}

n=0
for pair in "x86_64-elf|x86_64-unknown-elf" "aarch64-elf|aarch64-unknown-elf" \
            "thumbv7m-none-eabi|thumbv7m-none-eabi" "thumbv7em-none-eabi|thumbv7em-none-eabi" \
            "thumbv8m.main-none-eabi|thumbv8m.main-none-eabi" \
            "riscv32-unknown-elf|riscv32-unknown-elf" "riscv64-unknown-elf|riscv64-unknown-elf" \
            "avr|avr"; do
    et=${pair%%|*}; ct=${pair#*|}
    extra=; [ "$et" = avr ] && extra=-mmcu=atmega328p
    clang -target "$ct" $extra -ffreestanding -w -c "$out/init.c" -o "$out/c.o" 2> "$out/c.err" || {
        echo "$et: clang could not compile the probe:"; head -3 "$out/c.err"; exit 1; }
    "$EMBCC" --target="$et" -c "$out/init.c" -o "$out/e.o" 2> "$out/e.err" || {
        echo "$et: EmbCC could not fold a static initializer:"; head -3 "$out/e.err"; exit 1; }
    facts "$out/c.o" > "$out/c.$et" || exit 1
    facts "$out/e.o" > "$out/e.$et" || exit 1
    if ! cmp -s "$out/c.$et" "$out/e.$et"; then
        echo "$et: a static initializer folds differently from clang:"
        paste -d' ' "$out/exprs" "$out/c.$et" "$out/e.$et" |
            awk '{ split($(NF-1), c, "="); split($NF, e, "=");
                   if (c[2] != e[2]) { NF -= 2; print "    " $0 ":  clang " c[2] "  EmbCC " e[2] } }' | head -10
        exit 1
    fi
    # The same expressions, as static assertions of clang's values.
    python3 - "$out/exprs" "$out/c.$et" "$out/sa.c" <<'PYEOF'
import sys
ex = open(sys.argv[1]).read().splitlines()
vals = [l.split('=')[1] for l in open(sys.argv[2]).read().splitlines()]
o = ['typedef unsigned long long v;']
o += ['_Static_assert((v)(%s) == %sull, "%d");' % (e, x, k) for k, (e, x) in enumerate(zip(ex, vals))]
o.append('int ok;')
open(sys.argv[3], 'w').write('\n'.join(o) + '\n')
PYEOF
    "$EMBCC" --target="$et" -c "$out/sa.c" -o "$out/sa.o" 2> "$out/sa.err" || {
        echo "$et: a _Static_assert that clang accepts, EmbCC rejects:"
        head -3 "$out/sa.err"; exit 1; }
    n=$((n + $(wc -l < "$out/c.$et")))
done
echo "integer constants agree with clang on all eight targets: $n (target,
expression) pairs, each checked twice -- as a static initializer's value and
as a _Static_assert -- covering the type of every literal form and the
wrap-around, conversion and comparison rules C gives constant expressions"
