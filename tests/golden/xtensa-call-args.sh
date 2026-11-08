#!/bin/sh
# An argument narrower than a word is extended by the caller on Xtensa,
# as GCC's callers do -- `extui` for an unsigned char or short. A
# constant is written as its own extension already (`movi a10, 6`), and
# its extui was an instruction per argument per call: an RTOS's trace
# calls, `trace(6, 105, 0)`, paid three (#184). So was a value a narrow
# load had just zero-extended (l8ui, l16ui). A variable is still
# extended. EXTUI is op0 0 with op1 4 or 5 (the third byte's low
# nibble), and no other instruction is; -S writes one instruction a line.
set -u
echo "TEST-MARKER xtensa-call-args"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/xtensa-call-args
rm -rf "$out"; mkdir -p "$out"
cat > "$out/c.c" <<'EOC'
void tr(unsigned char a, unsigned short b, unsigned char c, signed char d);
void k(void) { tr(6, 105, 0, -3); }
void v(unsigned x) { tr((unsigned char)x, (unsigned short)x, 200, (signed char)x); }
void m(const unsigned char *p, const unsigned short *q) { tr(*p, *q, p[1], 2); }
EOC
fail=0
for o in -O2 -Os; do
    "$EMBCC" --target=xtensa-esp32-elf $o -S "$out/c.c" -o "$out/c$o.s" || exit 1
    for fn in k v m; do
        sed -n "/^$fn:/,/\\.size/p" "$out/c$o.s" |
            grep -cE '\.byte[[:space:]]+0x[0-9a-f]?0,0x[0-9a-f]+,0x[0-9a-f]?[45]$' > "$out/$fn$o.n"
    done
    [ "$(cat "$out/k$o.n")" = 0 ] || {
        echo "FAIL $o: constant arguments extended again ($(cat "$out/k$o.n") extui)"; fail=1; }
    [ "$(cat "$out/m$o.n")" = 0 ] || {
        echo "FAIL $o: loaded narrow arguments extended again ($(cat "$out/m$o.n") extui)"; fail=1; }
    [ "$(cat "$out/v$o.n")" -ge 2 ] || {
        echo "FAIL $o: variable narrow arguments not extended ($(cat "$out/v$o.n") extui)"; fail=1; }
done
[ "$fail" = 0 ] || exit 1
echo "xtensa-call-args: a constant narrow argument is written extended, a variable one is extended, at -O2 and -Os"
