#!/bin/sh
# --stack-info and --stack-report (src/driver/stackinfo.c, src/opt/wcet.c):
# a Cortex-M program linked with the runtime (built --stack-info), whose
# report must follow main into __divdi3 and udivmod64, bound udivmod64's
# loop in cycles, call main's spin loop unbounded (and say why), find the
# SysTick handler, add Cortex-M's 32-byte exception frame, and fail a
# --stack-limit it cannot show holds -- and pass one when every path is
# known (a leaf-only program).
set -u
echo "TEST-MARKER stack-report"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
EMBLD=${EMBLD:-$PWD/embld}
out=$PWD/tests/golden/out/stack-report
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
T=thumbv7m-none-eabi
RT=$PWD/build/libc/$T/librt.a
[ -f "$RT" ] || { echo "skipped: no $RT (make rt-embedded)"; exit 0; }
printf 'MEMORY { FLASH (rx) : ORIGIN = 0, LENGTH = 256K  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K }\nSECTIONS { .text : { *(.text*) *(.rodata*) } > FLASH .data : { *(.data*) } > RAM AT > FLASH .bss : { *(.bss*) *(COMMON) } > RAM }\n' > "$out/m.ld"
"$EMBCC" --target=$T -O2 --stack-info -c tests/golden/hardening/stack.c -o "$out/s.o" ||
    fail "--stack-info build"
"$EMBLD" -T "$out/m.ld" -e main "$out/s.o" "$RT" -o "$out/s.elf" || fail "link"
"$EMBCC" --stack-report "$out/s.elf" > "$out/r.txt" 2>&1 || true
for want in 'main > __divdi3 > udivmod64' 'SysTick_Handler.*(interrupt)' \
            'unknown: a loop with no bound' '32 bytes (Cortex-M'; do
    grep -q "$want" "$out/r.txt" || fail "the report lacks '$want': $(cat "$out/r.txt")"
done
grep -q '^div64 .* [0-9][0-9]* *div64 > __divdi3' "$out/r.txt" ||
    fail "div64's cycles are not bounded: $(grep '^div64' "$out/r.txt")"
if "$EMBCC" --stack-report "$out/s.elf" --stack-limit=100000 > /dev/null 2>&1; then
    fail "--stack-limit passed with an unbounded path"
fi
cat > "$out/leaf.c" <<'EOF2'
static int sq(int x) { return x * x; }
int main(void) { volatile int buf[8]; int s = 0; for (int i = 0; i < 10; i++) { s += sq(i); buf[i & 7] = s; } return s + buf[3]; }
EOF2
"$EMBCC" --target=$T -O1 --stack-info -c "$out/leaf.c" -o "$out/l.o" &&
"$EMBLD" -T "$out/m.ld" -e main "$out/l.o" -o "$out/l.elf" || fail "leaf build"
"$EMBCC" --stack-report "$out/l.elf" --stack-limit=4096 > "$out/l.txt" 2>&1 ||
    fail "--stack-limit=4096 failed a bounded program: $(cat "$out/l.txt")"
grep -q '^main .* [0-9][0-9]*  main' "$out/l.txt" || fail "main's cycles unbounded: $(cat "$out/l.txt")"
if "$EMBCC" --stack-report "$out/l.elf" --stack-limit=8 > /dev/null 2>&1; then
    fail "--stack-limit=8 passed a 32-byte frame: $(cat "$out/l.txt")"
fi
echo "--stack-report: the paths, the bounds, the unknowns and the limit"
