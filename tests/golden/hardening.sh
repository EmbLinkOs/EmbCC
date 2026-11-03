#!/bin/sh
# The hardening options:
#
#   -fstack-protector, -strong, -all   a run that overruns a guarded
#                      buffer aborts (the canary); one that does not, runs
#   -fstack-clash-protection           the probed allocas and frames
#                      compute what the unprobed ones do
#   -fcf-protection=full (x86-64)      endbr64 at the entries and the
#                      indirect targets, notrack on a jump table's jump,
#                      the IBT and SHSTK property note
#   -mbranch-protection=standard+leaf (AArch64)  paciasp/autiasp around
#                      every function (leaves too), bti j after setjmp and
#                      at a table's targets, the BTI and PAC note, and the
#                      unwind tables' negate_ra_state
set -u
echo "TEST-MARKER hardening"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
out=$PWD/tests/golden/out/hardening
rm -rf "$out"; mkdir -p "$out"
src=$PWD/tests/golden/hardening
fail() { echo "FAIL: $*"; exit 1; }
T=
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) T=arm64-apple-darwin ;;
    Linux-x86_64) T=x86_64-linux-gnu ;;
esac

if [ -n "$T" ]; then
    for mode in -fstack-protector -fstack-protector-strong -fstack-protector-all; do
        for O in -O0 -O2; do
            "$EMBCC" --target=$T $O $mode "$src/ssp.c" -o "$out/ssp" ||
                fail "$mode $O build"
            r=$("$out/ssp" 16 2>&1)
            [ "$r" = "130 48" ] || fail "$mode $O: an in-bounds run gave '$r'"
            # (an inner shell, whose own report of the abort goes nowhere)
            rc=$(sh -c '"$1" 64 > /dev/null 2>&1; echo $?' sh "$out/ssp" \
                 2> /dev/null)
            [ "$rc" -ge 128 ] || fail "$mode $O: an overrun was not stopped (rc $rc)"
        done
    done
    echo "-fstack-protector: the overrun stopped, the rest run (three modes, -O0 and -O2)"
    for O in -O0 -O2; do
        "$EMBCC" --target=$T $O "$src/clash.c" -o "$out/c0" &&
        "$EMBCC" --target=$T $O -fstack-clash-protection "$src/clash.c" -o "$out/c1" ||
            fail "stack-clash build $O"
        a=$("$out/c0" 20000); b=$("$out/c1" 20000)
        [ "$a" = "$b" ] || fail "stack-clash $O: '$b', not '$a'"
    done
    echo "-fstack-clash-protection: the same results, probed"
else
    echo "(no native target: the runs skipped)"
fi

OD=$(command -v llvm-objdump || ls /opt/homebrew/opt/llvm/bin/llvm-objdump 2>/dev/null || true)
RE=$(command -v llvm-readelf || ls /opt/homebrew/opt/llvm/bin/llvm-readelf 2>/dev/null || true)
DD=$(command -v llvm-dwarfdump || ls /opt/homebrew/opt/llvm/bin/llvm-dwarfdump 2>/dev/null || true)
[ -n "$OD" ] && [ -n "$RE" ] || { echo "(no llvm-objdump/readelf: the object checks skipped)"; exit 0; }
"$EMBCC" --target=x86_64-linux-gnu -O2 -fcf-protection=full -c "$src/cf.c" -o "$out/x.o" ||
    fail "-fcf-protection build"
"$OD" -d "$out/x.o" > "$out/x.dis"
n=$(grep -c endbr64 "$out/x.dis")
[ "$n" -ge 7 ] || fail "only $n endbr64 (entries 5, labels 2, after setjmp 1)"
# notrack is the 3e prefix on the table's indirect jmp (llvm-objdump shows
# it on a memory operand as a %ds: override, which is the same byte)
grep -qE 'notrack|3e ff 24' "$out/x.dis" || fail "no notrack jump"
"$RE" -n "$out/x.o" | grep -q 'x86 feature: IBT, SHSTK' || fail "no IBT/SHSTK note"
"$EMBCC" --target=x86_64-linux-gnu -O2 -c "$src/cf.c" -o "$out/x0.o"
! "$OD" -d "$out/x0.o" | grep -q endbr64 || fail "endbr64 without -fcf-protection"
echo "-fcf-protection: endbr64, notrack and the note"

"$EMBCC" --target=aarch64-linux-gnu -O2 -mbranch-protection=standard+leaf -c "$src/cf.c" \
    -o "$out/a.o" || fail "-mbranch-protection build"
"$OD" -d "$out/a.o" > "$out/a.dis"
np=$(grep -c paciasp "$out/a.dis"); na=$(grep -c autiasp "$out/a.dis")
nf=$(grep -c '>:$' "$out/a.dis")
[ "$np" = "$nf" ] || fail "$np paciasp for $nf functions"
[ "$na" -ge "$np" ] || fail "$na autiasp for $np paciasp"
grep -q 'bti	j' "$out/a.dis" || fail "no bti j"
"$RE" -n "$out/a.o" | grep -q 'aarch64 feature: BTI, PAC' || fail "no BTI/PAC note"
if [ -n "$DD" ]; then
    "$DD" --eh-frame "$out/a.o" 2>/dev/null | grep -q 'DW_CFA_AARCH64_negate_ra_state' ||
        fail "no negate_ra_state in .eh_frame"
fi
"$EMBCC" --target=aarch64-linux-gnu -O2 -mbranch-protection=pac-ret+b-key -c "$src/cf.c" \
    -o "$out/b.o" && "$OD" -d "$out/b.o" | grep -q pacibsp || fail "b-key: no pacibsp"
echo "-mbranch-protection: pac-ret, bti, the note and the unwind tables"

# ---- nocf_check: no landing pad, at that function only -------------------
printf '__attribute__((nocf_check)) int a(void) { return 1; }\nint b(void) { return 2; }\n' > "$out/nocf.c"
"$EMBCC" --target=x86_64-linux-gnu --cf-protection=branch -c "$out/nocf.c" -o "$out/nocf.o" ||
    fail "nocf_check was refused"
"$OD" -d "$out/nocf.o" | sed -n '/<a>:/,/^$/p' | grep -q endbr64 && fail "nocf_check kept a's endbr64"
"$OD" -d "$out/nocf.o" | sed -n '/<b>:/,/^$/p' | grep -q endbr64 || fail "b lost its endbr64"
echo "nocf_check: no endbr64 on that function"
