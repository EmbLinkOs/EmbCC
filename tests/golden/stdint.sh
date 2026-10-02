#!/bin/sh
# <stdint.h>, on every target, against the reference compiler.
#
# EmbCC's stdint.h hard-wired x86-64's widths for years after there were other
# targets: int64_t was `long`, FOUR bytes on ARMv7-M, ARMv8-M, RV32 and AVR;
# int32_t was `int`, two bytes on AVR; intptr_t was `long`, four bytes on AVR.
# Every fixed-width type was the wrong width somewhere, silently, and no test
# noticed because every one of them spelled `long long` out.
#
# So this asks the question directly, for every freestanding target this
# compiler has: the size and signedness of every <stdint.h> type, the value of
# every limit, and the type and value of every constant macro -- compared with
# the REFERENCE COMPILER for the same target: the one tools/gen-predef.sh took
# that target's predefined macros from. That is x86_64-elf-gcc and
# aarch64-elf-gcc for the two hosted-capable targets and clang for the embedded
# ones, and it matters: the exact-width types are the same everywhere, but the
# int_fast types are a PLATFORM choice -- newlib's GCC makes int_fast8_t an
# int, bare-metal clang a signed char -- and the header must agree with the
# compiler whose macros it is built from, not with some other one.
#
# Each fact is stored in its own 8-byte global and read back by symbol NAME
# from both objects, so neither compiler's choice of global order matters.
set -u
echo "TEST-MARKER stdint"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/stdint
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
command -v clang >/dev/null 2>&1 || { echo "SKIP: no clang to referee"; exit 0; }

python3 - "$out/probe.c" <<'PY'
import sys
T = ['int8_t','int16_t','int32_t','int64_t','uint8_t','uint16_t','uint32_t','uint64_t',
     'int_least8_t','int_least16_t','int_least32_t','int_least64_t',
     'uint_least8_t','uint_least16_t','uint_least32_t','uint_least64_t',
     'int_fast8_t','int_fast16_t','int_fast32_t','int_fast64_t',
     'uint_fast8_t','uint_fast16_t','uint_fast32_t','uint_fast64_t',
     'intptr_t','uintptr_t','intmax_t','uintmax_t']
L = ['INT8_MIN','INT8_MAX','INT16_MIN','INT16_MAX','INT32_MIN','INT32_MAX','INT64_MIN','INT64_MAX',
     'UINT8_MAX','UINT16_MAX','UINT32_MAX','UINT64_MAX',
     'INT_LEAST8_MIN','INT_LEAST8_MAX','INT_LEAST16_MIN','INT_LEAST16_MAX',
     'INT_LEAST32_MIN','INT_LEAST32_MAX','INT_LEAST64_MIN','INT_LEAST64_MAX',
     'UINT_LEAST8_MAX','UINT_LEAST16_MAX','UINT_LEAST32_MAX','UINT_LEAST64_MAX',
     'INT_FAST8_MIN','INT_FAST8_MAX','INT_FAST16_MIN','INT_FAST16_MAX',
     'INT_FAST32_MIN','INT_FAST32_MAX','INT_FAST64_MIN','INT_FAST64_MAX',
     'UINT_FAST8_MAX','UINT_FAST16_MAX','UINT_FAST32_MAX','UINT_FAST64_MAX',
     'INTPTR_MIN','INTPTR_MAX','UINTPTR_MAX','INTMAX_MIN','INTMAX_MAX','UINTMAX_MAX',
     'PTRDIFF_MIN','PTRDIFF_MAX','SIZE_MAX','SIG_ATOMIC_MIN','SIG_ATOMIC_MAX',
     'WCHAR_MIN','WCHAR_MAX','WINT_MIN','WINT_MAX']
C = [('INT8_C','-100'),('INT16_C','-30000'),('INT32_C','-2000000000'),
     ('INT64_C','-9000000000000000000'),('UINT8_C','200'),('UINT16_C','60000'),
     ('UINT32_C','4000000000'),('UINT64_C','18000000000000000000'),
     ('INTMAX_C','-9000000000000000000'),('UINTMAX_C','18000000000000000000')]
o = ['#include <stdint.h>', '#include <stddef.h>', '#include <limits.h>',
     'typedef unsigned long long v;']
for t in T:
    o.append('v size_%s = sizeof(%s);' % (t, t))
    # (T)-1 itself: all ones where T is signed, 2^(8n)-1 where it is not --
    # which says the width as well as the signedness.
    o.append('v minus1_%s = (v)(%s)-1;' % (t, t))
for l in L:
    o.append('v lim_%s = (v)(%s);' % (l, l))
    o.append('v limsize_%s = sizeof(%s);' % (l, l))
for m, x in C:
    o.append('v con_%s = (v)%s(%s);' % (m, m, x))
    o.append('v consize_%s = sizeof(%s(%s));' % (m, m, x))
    o.append('v conminus1_%s = (v)(%s(0) - 1);' % (m, m))
for t in ['size_t', 'ptrdiff_t', 'wchar_t']:
    o.append('v size_%s = sizeof(%s);' % (t, t))
for l in ['INT_MAX', 'LONG_MAX', 'LLONG_MAX', 'SHRT_MAX', 'UINT_MAX', 'ULONG_MAX', 'CHAR_MIN', 'CHAR_MAX']:
    o.append('v lim_%s = (v)(%s);' % (l, l))
open(sys.argv[1], 'w').write('\n'.join(o) + '\n')
PY

# name=value for every probe global, read by symbol from .data
facts() {
    python3 - "$1" <<'PY'
import sys, subprocess, tempfile, os
obj = sys.argv[1]
nm = subprocess.run(['llvm-nm', '--defined-only', obj], capture_output=True, text=True).stdout
# To a FILE. llvm-objcopy cannot write its output to a pipe, and the first
# version of this asked it to write /dev/stdout: `raw` came back empty, every
# value decoded as zero, and the test compared 1592 zeros with 1592 zeros --
# passing against the very header it was written to catch. The check below
# makes that impossible to repeat quietly.
fd, tmp = tempfile.mkstemp()
os.close(fd)
subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.data', obj, tmp], check=True)
raw = open(tmp, 'rb').read()
os.unlink(tmp)
out = []
for line in nm.splitlines():
    f = line.split()
    if len(f) != 3:
        continue
    if f[1] in 'Bb':
        # A zero may land in .bss (clang puts it there) or .data (EmbCC);
        # either way it is zero, and where it lives is not the question.
        out.append('%s=0' % f[2].lstrip('_'))
        continue
    if f[1] not in 'Dd':
        continue
    off = int(f[0], 16)
    out.append('%s=%d' % (f[2].lstrip('_'), int.from_bytes(raw[off:off + 8], 'little')))
facts = dict(l.split('=') for l in out)
# The reader must have READ something: every target has a one-byte int8_t and
# a two-byte int16_t. Without this, a reader returning zeros agrees with itself.
if facts.get('size_int8_t') != '1' or facts.get('size_int16_t') != '2':
    sys.stderr.write('facts: read %s/%s for sizeof(int8_t)/sizeof(int16_t) -- '
                     'the reader is not reading\n'
                     % (facts.get('size_int8_t'), facts.get('size_int16_t')))
    sys.exit(1)
print('\n'.join(sorted(out)))
PY
}

checked=0
# EMBCC target | the reference compiler and its flags
for pair in "x86_64-elf|x86_64-elf-gcc" "aarch64-elf|aarch64-elf-gcc" \
            "thumbv7m-none-eabi|clang -target thumbv7m-none-eabi" \
            "thumbv7em-none-eabi|clang -target thumbv7em-none-eabi" \
            "thumbv8m.main-none-eabi|clang -target thumbv8m.main-none-eabi -mfloat-abi=soft" \
            "riscv32-unknown-elf|clang -target riscv32-unknown-elf" \
            "riscv64-unknown-elf|clang -target riscv64-unknown-elf" \
            "avr|clang -target avr -mmcu=atmega328p"; do
    et=${pair%%|*}; ref=${pair#*|}
    refcc=${ref%% *}
    command -v "$refcc" >/dev/null 2>&1 || {
        echo "SKIP $et: its reference compiler ($refcc) is not installed"; continue; }
    "$EMBCC" --target="$et" -c "$out/probe.c" -o "$out/e.o" 2> "$out/e.err" || {
        echo "$et: the probe did not compile:"; head -3 "$out/e.err"; exit 1; }
    # shellcheck disable=SC2086
    $ref -ffreestanding -c "$out/probe.c" -o "$out/c.o" 2> "$out/c.err" || {
        echo "$et: the reference ($ref) could not compile the probe:"
        head -3 "$out/c.err"; exit 1; }
    facts "$out/e.o" > "$out/e.$et" || { echo "$et: could not read EmbCC's object"; exit 1; }
    facts "$out/c.o" > "$out/c.$et" || { echo "$et: could not read clang's object"; exit 1; }
    [ -s "$out/c.$et" ] || { echo "$et: read nothing back from clang's object"; exit 1; }
    if ! cmp -s "$out/c.$et" "$out/e.$et"; then
        echo "$et: <stdint.h> disagrees with the reference compiler:"
        diff "$out/c.$et" "$out/e.$et" | grep '^[<>]' | head -12
        exit 1
    fi
    checked=$((checked + $(wc -l < "$out/c.$et")))
done
echo "<stdint.h> agrees with each target's reference compiler on all eight targets -- $checked
facts: the size and signedness of every type, every limit, and the value, type
and signedness of every constant macro"
