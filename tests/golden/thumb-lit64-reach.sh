#!/bin/sh
# A 64-bit constant loaded from the literal pool at the end of the
# function (`ldrd lo, hi, [pc, #n]`, n up to 1020) must reach it on every
# pass, not only the one that planned it.
#
# Later passes only shorten the code -- branches the first pass made
# 32-bit become 16-bit -- but a load's base is its own address rounded
# down to a word. Two bytes saved before the load and none after it put
# the base four bytes further from the pool, so a first pass that used
# the last four bytes of reach made a later one miss, and the compile
# stopped with an internal error (fuzz seed 7227). The plan keeps those
# four bytes free (T_LIT64_REACH).
#
# The sweep: a loop and B more conditional branches before the load (each
# a two-byte saving later), I two-byte nops beside it, K statements and J
# nops between it and the pool -- so some combination puts the load within
# the last words of reach with each alignment. Before the fix, eight of
# them stopped. The test also checks that some load really is that far,
# so it does not pass by never getting there.
set -u
echo "TEST-MARKER thumb-lit64-reach"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/thumb-lit64-reach
rm -rf "$out"; mkdir -p "$out"
bad=0 n=0 far=0
for K in 118 119 120 121 122 123; do
  for B in 0 1 2 3; do
    for I in 0 1 2 3; do
      for J in 0 1 2 3 4 5 6 7; do
        f=$out/k$K-$B-$I-$J
        {
            echo 'typedef unsigned long long u64;'
            echo 'u64 f(int k, volatile int *p, volatile u64 *q) {'
            echo '    for (int j = 0; j < k; j++) { if (p[j] == 3) p[j + 1] = j; else if (p[j] == 9) break; }'
            echo '    if (k > 7) p[0] = 1;'
            b=0; while [ $b -lt $B ]; do
                echo "    if (p[$((b + 10))] == $b) p[$((b + 20))] = k;"; b=$((b + 1)); done
            i=0; while [ $i -lt $I ]; do echo '    __asm__ volatile("nop");'; i=$((i + 1)); done
            echo '    *q = 0x123456789abcdef1ULL;'
            i=0; while [ $i -lt $K ]; do
                echo "    p[$((i % 50))] = k * $((i * 7 + 3));"; i=$((i + 1)); done
            j=0; while [ $j -lt $J ]; do echo '    __asm__ volatile("nop");'; j=$((j + 1)); done
            echo '    *q += 0x0fedcba987654321ULL;'
            echo '    return *q;'
            echo '}'
        } > "$f.c"
        n=$((n + 1))
        if ! "$EMBCC" --target=thumbv7em-none-eabi -O2 -c "$f.c" -o "$f.o" 2> "$f.err"; then
            echo "K=$K B=$B I=$I J=$J: $(head -1 "$f.err")"
            bad=$((bad + 1))
            continue
        fi
        if command -v llvm-objdump > /dev/null 2>&1 &&
           llvm-objdump -d "$f.o" | grep -qE 'ldrd.*\[pc, #10[01][0-9]\]'; then
            far=$((far + 1))
        fi
      done
    done
  done
done
[ $bad -eq 0 ] || { echo "FAIL: $bad of $n compiles stopped"; exit 1; }
if command -v llvm-objdump > /dev/null 2>&1 && [ $far -eq 0 ]; then
    echo "FAIL: no load came within 1000 bytes of its pool: the sweep no longer"
    echo "      reaches the case it is for"
    exit 1
fi
echo "thumb-lit64-reach: $n functions compile, $far with a 64-bit literal 1000-1019 bytes from its pool"
