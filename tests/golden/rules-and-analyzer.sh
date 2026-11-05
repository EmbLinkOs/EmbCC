#!/bin/sh
# The coding-standard rules (--misra, --cert: src/sema/rules.c) and the
# static analyzer (--analyze: src/sema/analyze.c), each finding a warning
# named after what it checks:
#
#   violations.c  breaks one MISRA rule a line: every expected rule fires
#   cert.c        CERT's EXP30 (i = i++ + 1) and DCL30 (two returns of a
#                 local's address)
#   clean.c       nothing fires: no rule reported where code complies
#   isr.c         a SysTick handler and main sharing four objects: every
#                 interrupt finding, none on the access inside the critical
#                 section; and the memory findings (double free, unchecked
#                 malloc, a leak -- and not at `if (!q) return 1;`)
#   lib/libc      the analyzer finds nothing in EmbCC's own C library
#
# And a rule is one warning like any other: --no-warn=misra-15.5 silences
# it, --error= makes it an error.
set -u
echo "TEST-MARKER rules-and-analyzer"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
out=$PWD/tests/golden/out/rules-and-analyzer
rm -rf "$out"; mkdir -p "$out"
src=$PWD/tests/golden/rules
fail() { echo "FAIL: $*"; exit 1; }
names() { sed -n 's/.*\[--warn=\([a-z0-9.-]*\)\].*/\1/p' "$1" | sort -u | tr '\n' ' '; }

"$EMBCC" --target=x86_64-linux-gnu --misra --cert -c "$src/violations.c" \
    -o /dev/null > "$out/v.log" 2>&1 || fail "violations.c did not compile"
got=$(names "$out/v.log")
for r in misra-7.1 misra-7.2 misra-7.3 misra-7.4 misra-8.11 misra-10.1 \
         misra-10.3 misra-10.4 misra-11.5 misra-12.3 misra-14.1 misra-14.4 \
         misra-15.1 misra-15.5 misra-15.6 misra-15.7 misra-16.3 misra-16.4 \
         misra-17.2 misra-17.7 misra-17.8 misra-2.7 misra-21.3 misra-21.24 \
         cert-arr30 cert-err33 cert-flp30 cert-mem30 cert-msc30 cert-str30; do
    case " $got " in *" $r "*) ;; *) fail "$r did not fire (fired: $got)";; esac
done
"$EMBCC" --target=x86_64-linux-gnu --cert -c "$src/cert.c" -o /dev/null \
    > "$out/c.log" 2>&1
[ "$(grep -c 'cert-dcl30' "$out/c.log")" = 2 ] || fail "DCL30: $(cat "$out/c.log")"
grep -q 'cert-exp30' "$out/c.log" || fail "no EXP30"
"$EMBCC" --target=thumbv7m-none-eabi --misra -c "$src/clean.c" -o /dev/null \
    > "$out/clean.log" 2>&1 || fail "clean.c did not compile"
[ ! -s "$out/clean.log" ] || fail "rules fired on clean code: $(cat "$out/clean.log")"
"$EMBCC" --target=x86_64-linux-gnu --misra --no-warn=misra-15.5 -c "$src/violations.c" \
    -o /dev/null > "$out/q.log" 2>&1
! grep -q 'misra-15.5' "$out/q.log" || fail "--no-warn=misra-15.5 did not silence it"
if "$EMBCC" --target=x86_64-linux-gnu --misra --error=misra-12.3 -c "$src/violations.c" \
    -o /dev/null > /dev/null 2>&1; then fail "--error=misra-12.3 did not fail"; fi
echo "MISRA and CERT: every expected rule, none on clean code, each a warning"

"$EMBCC" --target=thumbv7m-none-eabi --analyze -c "$src/isr.c" -o /dev/null \
    > "$out/a.log" 2>&1 || fail "isr.c did not compile"
got=$(names "$out/a.log")
for r in isr-shared-nonvolatile isr-torn-access isr-race-rmw isr-reentrancy \
         isr-unsafe-call analyzer-double-free analyzer-possible-null-dereference \
         analyzer-malloc-leak; do
    case " $got " in *" $r "*) ;; *) fail "$r did not fire (fired: $got)";; esac
done
! grep -q "'flags'" "$out/a.log" || fail "the access inside the critical section was reported"
[ "$(grep -c 'analyzer-malloc-leak' "$out/a.log")" = 1 ] ||
    fail "the leak was reported where q is NULL too: $(grep leak "$out/a.log")"
for f in lib/libc/src/*/*.c lib/rt/*.c; do
    "$EMBCC" --analyze -Ilib/libc/include -Ilib/libc/src/math -c "$f" -o /dev/null 2>&1
done | grep 'analyzer-\|isr-' > "$out/libc.log"
[ ! -s "$out/libc.log" ] || fail "the analyzer reported lib/libc: $(head -3 "$out/libc.log")"
echo "--analyze: the interrupt and memory findings, none on lib/libc and lib/rt"
