#!/bin/sh
# Loop unswitching, -O3's own pass (src/opt/unswitch.c): a test the loop
# cannot change is made once, before it, and picks one of two copies.
#
# tests/exec/unswitch.c checks the answers -- every function against a
# twin whose test is read through a volatile, which stays one loop -- and
# exec-boards runs it at -O3 on the embedded boards. This checks which
# loops the pass took, on a 64-bit target and on two 32-bit ones (an
# optimizer test that asks x86-64 only once missed a pass that never ran
# on a 32-bit target), and that -O2 and -Os, which it does not belong to,
# never take one.
set -u
echo "TEST-MARKER unswitch"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/unswitch
rm -rf "$out"; mkdir -p "$out"
src=$EMBCC_ROOT/tests/exec/unswitch.c
fail() { echo "FAIL: $*"; exit 1; }

# ---- 1. which loops ------------------------------------------------------
took="two_arms sum_live find ret_in nested inner_only two_tests ptr_cond dowhile fsum chcond"
for t in x86_64-elf thumbv7em-none-eabi riscv32-unknown-elf; do
    "$EMBCC" --target=$t -O3 -fremarks -c "$src" -o /dev/null > "$out/$t.txt" 2>&1 ||
        fail "$t -O3 does not compile: $(head -3 "$out/$t.txt")"
    for f in $took; do
        grep -q "unswitched '$f'" "$out/$t.txt" ||
            fail "$t: '$f' was not unswitched, and its test is one the loop cannot change:
$(grep "'$f'" "$out/$t.txt" | sed 's/^/      /')"
    done
    # the twins read their tests through a volatile; addr_taken's test is
    # a variable stored through a pointer inside the loop
    for f in $took addr_taken; do
        if grep -q "unswitched '${f}_ref'" "$out/$t.txt"; then
            fail "$t: '${f}_ref' was unswitched, but its test reads a volatile"
        fi
    done
    if grep -q "unswitched 'addr_taken'" "$out/$t.txt"; then
        fail "$t: 'addr_taken' was unswitched, but the loop stores to its test's variable"
    fi
    # two tests, four copies: the second test is taken out of both copies
    n=$(grep -c "unswitched 'two_tests'" "$out/$t.txt")
    [ "$n" -ge 3 ] || fail "$t: two_tests was unswitched $n times, want 3 (four copies)"
done
echo "eleven loops unswitched on x86-64, the Cortex-M4 and RV32, two_tests into four;"
echo "the volatile twins and the loop that stores to its test left alone"

# ---- 2. only at -O3 ------------------------------------------------------
for spec in "-O2" "-Os" "-O3 --no-pass=unswitch" "-O3 -fno-unswitch"; do
    # shellcheck disable=SC2086
    n=$("$EMBCC" --target=thumbv7em-none-eabi $spec -fremarks -c "$src" \
          -o /dev/null 2>&1 | grep -c "unswitched '" || true)
    [ "$n" = 0 ] || fail "$spec unswitched $n loops"
done
echo "-O2, -Os and --no-pass=unswitch take none"

# ---- 3. the answers, hosted ------------------------------------------------
ran=
for O in -O0 -O2 -O3; do
    "$EMBCC" --target="$TARGET" "$O" -c "$src" -o "$out/u.o" 2> "$out/cc.log" ||
        fail "$O does not compile: $(head -3 "$out/cc.log")"
    t_link "$out/u" "$out/u.o" > "$out/ln.log" 2>&1 || {
        echo "(no hosted link for $TARGET here: the answers are exec-boards')"
        break; }
    t_run "$out/u" > /dev/null 2>&1
    rc=$?
    [ "$rc" = 42 ] ||
        fail "$TARGET $O: exits $rc, want 42 (1: a copy disagrees with its twin)"
    ran="$ran $O"
done
[ -z "$ran" ] || echo "the copies agree with their twins at$ran on $TARGET"
echo "PASS: unswitch"
