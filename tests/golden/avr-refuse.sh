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

# Multiply, divide and remainder used to be here. They are calls into
# lib/rt/avr.c now and avr-exec.sh runs them, which is what a case leaving
# this file is supposed to look like.

# Floating point. `float` and `double` are BOTH four-byte IEEE single on
# this target, and every operation on either is a soft-float call.
refuses "float arithmetic" "floating point" \
    'float f(float a, float b) { return a + b; }'
refuses "double arithmetic" "floating point" \
    'double f(double a, double b) { return a * b; }'

# 64-bit integers, varargs and aggregates by value used to be here. They
# work now -- tests/golden/avr-wide.sh runs all three on the part -- which is
# what a case leaving this file is supposed to look like.
#
# What remains is floating point and the handful of builtins below, each
# named rather than lumped under "unsupported".
refuses "a byte swap" "bswap" \
    'unsigned long f(unsigned long a) { return __builtin_bswap32(a); }'

echo "all $n unsupported constructs are refused, and each diagnostic names
which one -- so nothing here can be mistaken for code that works"
