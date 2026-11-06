#!/bin/sh
# --opt=max (-O3) beyond loop unswitching (unswitch.sh): the constants
# every call passes (src/opt/ipcp.c), and the inliner weighing a callee
# as it will be once optimized rather than as irgen wrote it.
#
# tests/exec/ipcp.c checks the answers, every case against its value by
# hand, and exec-boards runs it at -O3 on the boards; its first run found
# a function whose address was only in a static initializer given its one
# direct call's constant, which the call through the pointer did not
# pass. This checks which parameters were taken and which refused, on a
# 32-bit target, and that -O2 and -Os keep their own decisions.
set -u
echo "TEST-MARKER opt-max"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/opt-max
rm -rf "$out"; mkdir -p "$out"
src=$EMBCC_ROOT/tests/exec/ipcp.c
fail() { echo "FAIL: $*"; exit 1; }
T=thumbv7em-none-eabi

# ---- 1. which parameters --------------------------------------------------
"$EMBCC" --target=$T -O3 -fremarks -c "$src" -o /dev/null > "$out/o3.txt" 2>&1 ||
    fail "-O3 does not compile: $(head -3 "$out/o3.txt")"
grep -q "propagated 'wrap_sum': parameter 1 is 16 on all 2 calls" "$out/o3.txt" ||
    fail "wrap_sum's n is 16 at both calls and was not propagated"
grep -q "propagated 'scaled': parameter 2 is 3 on all 2 calls" "$out/o3.txt" ||
    fail "scaled's shift is 3 at both calls and was not propagated"
if grep -q "propagated 'scaled': parameter 1 " "$out/o3.txt"; then
    fail "scaled's x differs between its calls, and was propagated"
fi
# differ: 3 and 5; escapes: its address is in a static initializer, and
# the call through it passes 9; depth passes its own parameter down
for f in differ escapes depth; do
    if grep -q "propagated '$f'" "$out/o3.txt"; then
        fail "'$f' was given a constant, but not every call passes it:
$(grep "propagated '$f'" "$out/o3.txt" | sed 's/^/      /')"
    fi
done
echo "the two parameters every call gives one constant were propagated;"
echo "two constants, an escaped address and a recursive pass-through were not"

# ---- 2. the inliner at -O3 ------------------------------------------------
# cls is a table lookup: 34 instructions as irgen writes it, a handful once
# mem2reg is done, and -O2's budget of 24 counts the first.
cat > "$out/cls.c" <<'EOF'
static const unsigned short tab[257] = { [1 + 'a'] = 4, [1 + '0'] = 2 };
static unsigned short cls(int c) { return (c >= -1 && c < 256) ? tab[c + 1] : 0; }
int isalpha_(int c) { return (cls(c) & 4) != 0; }
int isdigit_(int c) { return (cls(c) & 2) != 0; }
int isalnum_(int c) { return (cls(c) & 6) != 0; }
EOF
for O in -O2 -O3 -Os; do
    "$EMBCC" --target=$T $O -fremarks -c "$out/cls.c" -o /dev/null > "$out/cls$O.txt" 2>&1 ||
        fail "$O: cls.c does not compile"
done
n=$(grep -c "remark: inlined 'cls'" "$out/cls-O3.txt" || true)
[ "$n" = 3 ] || fail "-O3 inlined cls $n times, want 3:
$(grep "'cls'" "$out/cls-O3.txt" | sed 's/^/      /')"
grep -q "remark: inlined 'cls': [0-9]* instructions as optimized" "$out/cls-O3.txt" ||
    fail "-O3's remark does not say the size it weighed: $(grep "inlined 'cls'" "$out/cls-O3.txt" | head -1)"
for O in -O2 -Os; do
    if grep -q "remark: inlined 'cls'" "$out/cls$O.txt"; then
        fail "$O inlined cls, and its budget is the plain count's"
    fi
done
echo "-O3 inlines a 34-instruction table lookup by what it will be; -O2 and -Os do not"

# ---- 3. only at -O3 ---------------------------------------------------------
for spec in "-O2" "-Os" "-O3 --no-pass=ipcp"; do
    # shellcheck disable=SC2086
    n=$("$EMBCC" --target=$T $spec -fremarks -c "$src" -o /dev/null 2>&1 |
          grep -c "propagated '" || true)
    [ "$n" = 0 ] || fail "$spec propagated $n parameters"
done
echo "-O2, -Os and --no-pass=ipcp propagate none"

# ---- 4. the answers, hosted -------------------------------------------------
ran=
for O in -O0 -O2 -O3; do
    "$EMBCC" --target="$TARGET" "$O" -c "$src" -o "$out/p.o" 2> "$out/cc.log" ||
        fail "$O does not compile: $(head -3 "$out/cc.log")"
    t_link "$out/p" "$out/p.o" > "$out/ln.log" 2>&1 || {
        echo "(no hosted link for $TARGET here: the answers are exec-boards')"
        break; }
    t_run "$out/p" > /dev/null 2>&1
    rc=$?
    [ "$rc" = 42 ] || fail "$TARGET $O: exits $rc, want 42"
    ran="$ran $O"
done
[ -z "$ran" ] || echo "the answers are right at$ran on $TARGET"
echo "PASS: opt-max"
