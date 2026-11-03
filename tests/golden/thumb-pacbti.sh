#!/bin/sh
# ARMv8.1-M branch protection (-mbranch-protection=), as clang compiles it:
#
#   - on the Cortex-M85 (PACBTI): a function that saves lr starts with
#     pac (pacbti with BTI) r12, lr, sp, pushes r12 beside lr, and returns
#     with bxaut r12, lr, sp; a leaf keeps bx lr unless +leaf; bti alone
#     is each function's first instruction;
#   - on the Cortex-M55 (no PACBTI): aut r12, lr, sp and bx lr, never
#     bxaut, which is UNDEFINED there (pac, aut, bti are hints);
#   - the macros are clang's, for both parts and for AArch64;
#   - v7-M and +b-key are refused.
# The exec corpus runs with it on the M55 (exec-boards.sh's m55pac board).
set -u
echo "TEST-MARKER thumb-pacbti"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
out=$PWD/tests/golden/out/thumb-pacbti
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
OD=$(command -v llvm-objdump || ls /opt/homebrew/opt/llvm/bin/llvm-objdump 2>/dev/null || true)
CLANG=$(ls /opt/homebrew/opt/llvm/bin/clang 2>/dev/null || command -v clang || true)
[ -n "$OD" ] || { echo "SKIP: no llvm-objdump"; exit 0; }
T=thumbv8.1m.main-none-eabi

cat > "$out/p.c" <<'EOF'
int g(int);
int calls(int x) { return g(x) + 1; }
int leaf(int x) { return x + 1; }
EOF
# the mnemonics of each function, one line each
seq() {
    "$OD" -d --mattr=+pacbti "$1" | awk '
        /^[0-9a-f]+ <[a-z]+>:/ { if (f) print f ":" s; f = $2; gsub(/[<>:]/, "", f); s = ""; next }
        /^ +[0-9a-f]+:/ { n = split($0, a, "\t"); s = s " " a[2] }
        END { if (f) print f ":" s }'
}
want() {      # cpu kind function expected-mnemonics
    "$EMBCC" --target=$T -mcpu=$1 -mbranch-protection=$2 -O2 -c "$out/p.c" \
        -o "$out/p-$1-$2.o" || fail "$1 $2 does not compile"
    got=$(seq "$out/p-$1-$2.o" | grep "^$3:" | sed "s/^$3: //")
    [ "$got" = "$4" ] || fail "$1 -mbranch-protection=$2, $3: '$got', want '$4'"
}
want cortex-m85 standard calls "pacbti push.w bl adds pop.w bxaut"
want cortex-m85 standard leaf "bti adds bx"
want cortex-m85 pac-ret calls "pac push.w bl adds pop.w bxaut"
want cortex-m85 pac-ret leaf "adds bx"
want cortex-m85 pac-ret+leaf leaf "pac push.w adds pop.w bxaut"
want cortex-m85 bti calls "bti push bl adds pop"
want cortex-m55 pac-ret calls "pac push.w bl adds pop.w aut bx"
"$OD" -d --mattr=+pacbti "$out/p-cortex-m85-standard.o" |
    grep -q 'push.w.*{r12, lr}' || fail "r12 is not pushed beside lr"
echo "M85: pacbti/pac at entry, r12 pushed with lr, bxaut; bti alone; +leaf signs a leaf"
echo "M55: aut and bx lr, not bxaut"

# the macros, against clang's
for cfg in "cortex-m85 standard" "cortex-m85 pac-ret+leaf" "cortex-m55 bti"; do
    set -- $cfg
    "$EMBCC" --target=$T -mcpu=$1 -mbranch-protection=$2 -E -dM -x c /dev/null |
        grep -E 'PAC|BTI|PAUTH' | sort > "$out/m.e"
    if [ -n "$CLANG" ]; then
        "$CLANG" --target=$T -mcpu=$1 -mbranch-protection=$2 -E -dM -x c /dev/null |
            grep -E 'PAC|BTI|PAUTH' | sort > "$out/m.c"
        diff "$out/m.c" "$out/m.e" > /dev/null ||
            fail "$1 $2: the macros are not clang's: $(diff "$out/m.c" "$out/m.e")"
    fi
done
"$EMBCC" --target=aarch64-linux-gnu -mbranch-protection=pac-ret+leaf+b-key -E -dM \
    -x c /dev/null | grep -q '__ARM_FEATURE_PAC_DEFAULT 6' ||
    fail "AArch64's __ARM_FEATURE_PAC_DEFAULT for pac-ret+leaf+b-key is not 6"
echo "__ARM_FEATURE_PAUTH/BTI/BTI_DEFAULT/PAC_DEFAULT: clang's"

"$EMBCC" --target=thumbv7em-none-eabi -mbranch-protection=bti -c "$out/p.c" \
    -o "$out/x.o" 2> "$out/v7.err" && fail "v7-M accepted -mbranch-protection"
grep -q "ARMv8.1-M" "$out/v7.err" || fail "v7-M's refusal: $(cat "$out/v7.err")"
"$EMBCC" --target=$T -mcpu=cortex-m85 -mbranch-protection=pac-ret+b-key -c \
    "$out/p.c" -o "$out/x.o" 2> "$out/bk.err" && fail "+b-key accepted on ARMv8.1-M"
grep -q "one key" "$out/bk.err" || fail "+b-key's refusal: $(cat "$out/bk.err")"
echo "v7-M and +b-key are refused"
