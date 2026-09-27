#!/bin/sh
# What the AVR backend refuses, and that it refuses BY NAME.
#
# THE RULE: this compiler would rather stop than emit plausible code for
# something it has not learned. On AVR that matters more than on the other
# four targets, because the machine's wrong answers are all valid: a
# multiply lowered to the wrong sequence still runs, a value narrowed to
# two bytes instead of four still returns.
#
# Each case below names one thing this version cannot do and asserts that
# the diagnostic says WHICH -- "unsupported" sends the reader through the
# whole lowering, where "a multiply by a value" names the line to look at.
# When one of these lands, its case here should be deleted and a test that
# runs it added to avr-exec.sh; a case that starts passing is the signal.
set -u
echo "TEST-MARKER avr-refuse"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-refuse
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}

n=0
refuses() {                       # refuses <name> <expected phrase> <source>
    n=$((n + 1))
    printf '%s\n' "$3" > "$out/c.c"
    if "$EMBCC" --target=avr -c "$out/c.c" -o "$out/c.o" 2> "$out/e.txt"; then
        echo "$1: compiled. This backend cannot do it, so what it emitted"
        echo "        is another answer to the question -- which is the one"
        echo "        outcome every refusal here exists to prevent."
        exit 1
    fi
    grep -q "$2" "$out/e.txt" || {
        echo "$1: refused, but not by name -- the message does not contain"
        echo "        '$2':"
        head -3 "$out/e.txt"
        exit 1; }
}

# The runtime helpers. AVR's `mul` is 8x8 into r1:r0 and destroys the zero
# register; there is no divide instruction at all. Both are library calls
# on every AVR toolchain, so they are lib/rt work rather than instruction
# selection. Multiplying by a CONSTANT does work -- an array index scales
# by its element size, so refusing that would refuse tab[1].x -- and
# avr-exec.sh covers it.
refuses "a multiply by a value" "a multiply by a value" \
    'long f(long a, long b) { return a * b; }'
refuses "divide" "div" \
    'long f(long a, long b) { return a / b; }'
refuses "modulo" "mod" \
    'long f(long a, long b) { return a % b; }'

# Floating point. `float` and `double` are BOTH four-byte IEEE single on
# this target, and every operation on either is a soft-float call.
refuses "float arithmetic" "floating point" \
    'float f(float a, float b) { return a + b; }'
refuses "double arithmetic" "floating point" \
    'double f(double a, double b) { return a * b; }'

# 64-bit integers: eight consecutive registers and a carry chain twice as
# long as the four-byte one. A legalisation pass, not a second set of
# hand-written chains.
refuses "long long" "64-bit integer" \
    'long long f(long long a, long long b) { return a + b; }'

# ABI work of its own, each.
refuses "a variadic function" "variadic" \
    '#include <stdarg.h>
     int f(int n, ...) { va_list ap; int s; va_start(ap, n); s = va_arg(ap, int); va_end(ap); return s; }'
refuses "a struct parameter" "struct" \
    'struct s { long a, b; }; int f(struct s v) { return (int)v.a; }'
refuses "a struct return" "struct" \
    'struct s { long a, b; }; struct s g(void); long f(void) { return g().a; }'

echo "all $n unsupported constructs are refused, and each diagnostic names
which one -- so nothing here can be mistaken for code that works"
