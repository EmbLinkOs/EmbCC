#!/bin/sh
# EmbIR's textual form, round-tripped — §9.1's requirement.
#
#     "Round-trip textual form (print -> parse -> identical IR). This makes
#      every pass testable in isolation with text-in/text-out tests."
#
# The check: printing a PARSED unit reproduces the text it was parsed from,
# byte for byte. That proves the form is a lossless encoding of everything
# it claims to carry, and it fails the moment the printer emits something
# the parser cannot read back — which is the drift the requirement exists to
# prevent.
set -eu
echo "TEST-MARKER ir-roundtrip"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/ir-roundtrip
rm -rf "$out"; mkdir -p "$out"

# ---- the shapes the printer can emit ---------------------------------------
cat > "$out/p.c" << 'EOF'
extern int g, arr[8];
extern int h(int, int);
extern int  vfmt(const char *, ...);
extern void sink(long);
struct P { int a, b; };

static int helper(int x) { return x * 3; }

int shapes(int n, int m)
{
    int t = 0;
    volatile int v = 0;
    long w = 0;
    char c = (char)n;
    int *p = &t;
    for (int i = 0; i < n; i++) {
        if (i == m) continue;
        if (i > 100) break;
        switch (i & 3) {
        case 0:  t += h(i, n) + arr[i]; break;
        case 1:  t -= helper(i);        break;
        default: t ^= i;                break;
        }
        /* dense enough for a jump table: the IR has an IR_SWITCH with
         * its table, which the text must carry */
        switch (i & 7) {
        case 1: t += 1; break;  case 2: t += 3; break;  case 3: t += 5; break;
        case 5: t += 7; break;  case 6: t -= 1; break;
        default: break;
        }
        w += (long)i;
        v = i;
    }
    sink(w);
    /* A volatile READ: every access is kept, so this stays an ldvar at
     * any level. The char's reads used to be the only ldvar left at -O2,
     * and mem2reg promotes a char now. */
    t += v;
    vfmt("t=%d c=%d\n", t, (int)c);
    g = t + *p;
    return t;
}
EOF

for O in -O0 -O1 -O2; do
    "$EMBCC" inspect ir "$O" "$out/p.c" > "$out/a$O.ir"
    "$EMBCC" inspect ir "$out/a$O.ir" > "$out/b$O.ir"
    cmp "$out/a$O.ir" "$out/b$O.ir" || {
        diff "$out/a$O.ir" "$out/b$O.ir" | head -20
        echo "FAIL: the round-trip is not lossless at $O"; exit 1; }
done
echo "print -> parse -> print is byte-identical at -O0, -O1 and -O2"

# The text really is the only input: parsing does not touch the C file.
mv "$out/p.c" "$out/p.c.moved"
"$EMBCC" inspect ir "$out/a-O2.ir" > "$out/c.ir"
cmp "$out/a-O2.ir" "$out/c.ir" ||
    { echo "FAIL: parsing depended on the source file"; exit 1; }
mv "$out/p.c.moved" "$out/p.c"
echo "and it round-trips with the C source deleted: the text is the input"

# ---- what the form has to carry --------------------------------------------
# Each of these appears in the printed IR and therefore has to survive the
# trip. Asserted by name so a printer change that drops one is a failure
# here rather than a silent loss.
a="$out/a-O2.ir"
for want in "^func @shapes " "nparams=2" "nvars=" "vregs=" "labels=" \
            "^  local v" "size=" "align=" "^L[0-9]*:" "= const" \
            "= gaddr @g" "stvar" "ldvar" "brz" "jmp L" "ret "; do
    grep -qE "$want" "$a" || { echo "FAIL: the form lost '$want'"; exit 1; }
done
grep -qF "= call @h(" "$a" || { echo "FAIL: the form lost a direct call"; exit 1; }
grep -qE "; [0-9]+:[0-9]+" "$a" ||
    { echo "FAIL: provenance (line:col) is not in the text"; exit 1; }
grep -q "volatile" "$a" || { echo "FAIL: a volatile local is not marked"; exit 1; }
echo "the form carries: the symbol table, each function's shape, its frame
slots with their types, labels, and every instruction's line and column"

# ---- the invariant that matters: EmbCC's own source ------------------------
# 55 files, two optimization levels. Every pass that builds instructions --
# the inliner, mem2reg, SCCP, out-of-SSA -- is exercised at -O2.
ok=0; bad=0
for f in src/*/*.c src/*/*/*.c; do
    for O in -O0 -O2; do
        "$EMBCC" inspect ir "$O" -Isrc -isystem "$X86_NEWLIB/include" "$f" \
            > "$out/x.ir" 2>/dev/null || continue
        if "$EMBCC" inspect ir "$out/x.ir" > "$out/y.ir" 2>"$out/e.txt" &&
           cmp -s "$out/x.ir" "$out/y.ir"; then
            ok=$((ok + 1))
        else
            bad=$((bad + 1))
            [ "$bad" -le 2 ] && { echo "  FAIL $f $O"; head -2 "$out/e.txt"
                                  diff "$out/x.ir" "$out/y.ir" | head -6; }
        fi
    done
done
[ "$bad" = 0 ] || { echo "FAIL: $bad of $((ok + bad)) round-trips lost something"
                    exit 1; }
echo "EmbCC's own sources at -O0 and -O2: $ok round-trips, all byte-identical"

# ---- an error names the line, for whoever just changed the printer ---------
printf '; EmbIR\nfunc @f nparams=0 nvars=0 vregs=1 labels=0 {\n  %%0 = notanopcode 1\n}\n' \
    > "$out/bad.ir"
if "$EMBCC" inspect ir "$out/bad.ir" > "$out/berr.txt" 2>&1; then
    echo "FAIL: a bad opcode was accepted"; exit 1
fi
grep -q "bad.ir:3" "$out/berr.txt" ||
    { cat "$out/berr.txt"; echo "FAIL: the error does not name the line"; exit 1; }
echo "a malformed line is refused at its own line number"

# ---- and with -O, parsed IR is optimized before it is printed ---------------
# That is what makes a pass testable on IR written to provoke it. Without
# an -O level the file comes back as written, which the round-trips above
# depend on.
printf '; EmbIR\nfunc @f nparams=0 nvars=0 vregs=3 labels=0 {\n  %%0 = const.4 2\n  %%1 = const.4 3\n  %%2 = add.4s %%0, %%1\n  ret %%2\n}\n' \
    > "$out/opt.ir"
"$EMBCC" inspect ir "$out/opt.ir" > "$out/opt0.txt" 2>&1 &&
    "$EMBCC" inspect ir -O2 "$out/opt.ir" > "$out/opt2.txt" 2>&1 ||
    { cat "$out/opt0.txt" "$out/opt2.txt"; echo "FAIL: could not read opt.ir"; exit 1; }
grep -q "add\." "$out/opt0.txt" ||
    { cat "$out/opt0.txt"; echo "FAIL: without -O the IR should come back as written"; exit 1; }
! grep -q "add\." "$out/opt2.txt" && grep -qE "const\.4s? 5(\s*;.*)?$" "$out/opt2.txt" ||
    { cat "$out/opt2.txt"; echo "FAIL: -O2 should have folded 2 + 3 in the parsed IR"; exit 1; }
echo "with -O2, parsed IR goes through the optimizer: 2 + 3 comes back as 5"
