#!/bin/sh
# Enormous functions, and files of very many, compile in seconds: every
# step that looked at everything for each thing is caught by a shape that
# has a great many of that thing.
#
# GCC's limits-caselabels.c stacks 100,000 cases on one `break`: a
# decision tree whose every leaf went where the default goes, which the
# optimizer undid one node a round and gave up on after 64 (two minutes,
# and "did not converge"). Cases that go where the default goes are no
# longer tested at all. And the cases were sorted by insertion: 100,000
# written in descending order took 23 s at -O0.
#
# Labels in a row, too: the CFG cleanup walked from each label past every
# label after it on the same spot (100,000 took 11 s at -O2), and a
# function could have only 256 named labels at all.
#
# A chain of 32,768 tests, each a block of its own (GCC's 20001226-1.c's
# shape): the dominator tree was computed by iterating to a fixed point,
# the dominance frontiers walked on past what they had already found,
# and each branch looked for a test of its value in every block above it
# (157 s at x86-64 -O2). On Cortex-M0, at each branch, the code since a
# register was loaded was decoded again to see whether it still held its
# frame word, and each BL in it was looked up among all the branches:
# 8,192 tests did not finish in two minutes.
#
# And the rest, each seconds to minutes before (in brackets, where):
# 30,000 `a[x - k]` (the Thumb index-offset pass's search for a mate, the
# load CSE's for a key); 20,000 globals read and written (every store
# asked every load key); 600 short loops (LICM and the vectorizer, a
# whole-function scan per loop, one loop a call); a ladder of 20,000
# constants on Xtensa (the literal pool searched per constant); 40,000
# calls (x86-64's tail-call test scanned the function per call); 10,000
# double expressions without an FPU (Thumb's constant pool and pair
# ranking; Cortex-M0's data ranges asked at every halfword); 20,000
# functions in one file on Cortex-M0 (every range of the unit, again).
set -u
echo "TEST-MARKER big-functions"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
if [ "$TARGET" != x86_64-elf ]; then
    echo "big-functions: the front end's, the optimizer's and a few backends'; checked under x86_64-elf only"
    exit 0
fi
command -v python3 > /dev/null 2>&1 || { echo "big-functions: no python3, skipped"; exit 0; }
out=tests/golden/out/big-functions
rm -rf "${out:?}"; mkdir -p "$out"
limit() { perl -e '$t = shift; alarm $t; exec @ARGV' "$@"; }

python3 - "$out" <<'PYEOF'
import sys
d = sys.argv[1]
def w(name, text):
    with open(d + '/' + name, 'w') as f:
        f.write(text)
n = 100000
w('stacked.c', 'void f(long i) { switch (i) { ' +      # limits-caselabels.c's
  ' '.join('case %d:' % (100000 + k) for k in range(n)) + ' break; } }\n')
w('labels.c', 'int f(int x) { ' +                      # 300,000 on one spot
  ' '.join('l%d: ;' % k for k in range(300000)) + ' return x; }\n')
w('descending.c', 'int x; void f(long i) { switch (i) { ' +  # the sort's worst
  ' '.join('case %d: x = %d; break;' % (200000 - k, k % 13)
           for k in range(n)) + ' } }\n')
def chain(m):                               # m * m pairs, two tests each
    return ('unsigned f(int *x, int *y)\n{\n' +
            ''.join('    if (x[%d] > y[%d]) goto gt;\n'
                    '    if (x[%d] < y[%d]) goto lt;\n' % (i, j, i, j)
                    for j in range(m) for i in range(m)) +
            '    return 1;\ngt: return 2;\nlt: return 3;\n}\n')
w('chain128.c', chain(128))
w('chain64.c', chain(64))
w('index.c', 'int f(int *a, int i)\n{\n    int s = 0;\n' +
  ''.join('    s += a[(i ^ %d) - 1];\n' % (k + 1) for k in range(30000)) +
  '    return s;\n}\n')
w('globals.c', ''.join('int g%d;\n' % k for k in range(20000)) +
  'void f(int v)\n{\n' + ''.join('    g%d += v;\n' % k for k in range(20000)) +
  '}\n')
w('loops.c', 'int a[100];\nint f(int n)\n{\n    int s = 0;\n' +
  ''.join('    for (int i = 0; i < n; i++)\n        s += a[i] * %d;\n' % k
          for k in range(600)) + '    return s;\n}\n')
w('ladder.c', 'int f(int x)\n{\n' +
  ''.join('    %sif (x == %d) return %d;\n' % ('else ' if k else '', k * 7, k)
          for k in range(20000)) + '    return -1;\n}\n')
w('calls.c', 'void g(int, int);\nvoid f(int x)\n{\n' +
  ''.join('    g(x, %d);\n' % k for k in range(40000)) + '}\n')
w('floats.c', 'double f(double a, double b)\n{\n    double s = 0;\n' +
  ''.join('    s = s * a + b / %d.0;\n' % (k + 1) for k in range(10000)) +
  '    return s;\n}\n')
w('funcs.c', ''.join('int f%d(int x) { return x * %d + 1; }\n' % (k, k)
                     for k in range(20000)))
PYEOF

# in seconds or not at all: within SECONDS WHAT FILE OPTION...
bad=0
within() {
    secs=$1; what=$2; src=$3; shift 3
    limit "$secs" "$EMBCC" "$@" -w -c "$out/$src" -o "$out/x.o" > "$out/x.log" 2>&1
    st=$?
    if [ $st -eq 142 ]; then
        echo "FAIL: $what: no answer in $secs s"; bad=1
    elif [ $st -ne 0 ]; then
        head -3 "$out/x.log"; echo "FAIL: $what: exit $st"; bad=1
    elif grep -q 'did not converge' "$out/x.log"; then
        echo "FAIL: $what: the optimizer did not converge"; bad=1
    fi
}

for o in -O0 -O2 -Os; do
    within 20 "100,000 cases on one break, $o" stacked.c $o
done
within 10 "100,000 cases in descending order, -O0" descending.c -O0
within 30 "300,000 labels in a row, -O2" labels.c -O2
within 20 "32,768 tests in a chain, x86-64 -O2" chain128.c --target=x86_64-elf -O2
within 30 "8,192 tests in a chain, Cortex-M0 -O2" chain64.c --target=thumbv6m-none-eabi -O2
within 20 "30,000 indexed loads, Cortex-M3 -O2" index.c --target=thumbv7m-none-eabi -O2
within 8 "20,000 globals, x86-64 -O2" globals.c --target=x86_64-elf -O2
within 10 "600 loops, x86-64 -O2" loops.c --target=x86_64-elf -O2
within 8 "a ladder of 20,000 constants, Xtensa -O0" ladder.c --target=xtensa-elf -O0
within 8 "40,000 calls, x86-64 -O2" calls.c --target=x86_64-elf -O2
within 6 "10,000 double expressions, Cortex-M3 -O2" floats.c --target=thumbv7m-none-eabi -O2
within 6 "10,000 double expressions, Cortex-M0 -O2" floats.c --target=thumbv6m-none-eabi -O2
within 8 "20,000 functions, Cortex-M0 -O2" funcs.c --target=thumbv6m-none-eabi -O2
[ $bad -eq 0 ] || exit 1
echo "big-functions: switches of 100,000 cases, 300,000 labels, chains of tests, 30,000 indexed loads, 20,000 globals, 600 loops, 40,000 calls, 10,000 double expressions and 20,000 functions, in seconds"
