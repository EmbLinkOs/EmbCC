#!/bin/sh
# Small switches at -Os on ARMv7-M.
#
# FreeRTOS's notify functions switch on eAction, five dense cases. Under
# -Os a table needed six, so it was a tree of compares: `cmp r8, #2; beq;
# cmp r8, #2; bgt; ...`, forty bytes where clang's table is eighteen --
# a tbb now, whose entries are bytes when every case is within reach. On
# ARMv7-M a table is now four cases or more (target_switch_table_min_os):
# its dispatch is cmp, bhs and tbh, and an entry two bytes. And a tree
# that asks `== k` and then `> k` of one register makes the compare once:
# the second branch reads the first one's flags (fl_end).
#
# Shapes first; then embedded-switch.c on the Cortex-M3 board at every
# level against the same program on the host.
set -u
echo "TEST-MARKER thumb-switch-os"
. "$(dirname "$0")/../lib.sh"

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: llvm-objdump not found"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/thumb-switch-os
rm -rf "$out"; mkdir -p "$out"

cat > "$out/s.c" <<'EOT'
extern void a(void), b(void), c(void), d(void), e(void), f(void), g(void);
void dn(unsigned x)
{
    switch (x) { case 0: a(); break; case 1: b(); break; case 2: c(); break;
                 case 3: d(); break; case 4: e(); break; }
}
void sp(int x)
{
    switch (x) { case 1: a(); break; case 50: b(); break; case 900: c(); break;
                 case 1000: d(); break; case 7000: e(); break;
                 case 9000: f(); break; default: g(); }
}
EOT
"$EMBCC" --target=thumbv7em-none-eabi -Os -c "$out/s.c" -o "$out/s.o" || {
    echo "FAIL: could not compile"; exit 1; }
"$OD" -d --no-show-raw-insn --triple=thumbv7em "$out/s.o" > "$out/s.dis"
body() {
    awk -v f="<$1>:" '$2 == f { on = 1; next } /^[0-9a-f]+ <.*>:$/ { on = 0 }
                      on && /^ *[0-9a-f]+:/' "$out/s.dis"
}
[ "$(body dn | grep -c 'tbb')" = 1 ] || {
    echo "FAIL: five dense cases at -Os should be a tbb table:"; body dn; exit 1; }
echo "five dense cases at -Os: a tbb table (byte entries, all within reach)"
# no compare repeated with only a branch between
body sp | awk '{ $1 = ""; print }' | sed 's/^ *//' > "$out/sp.txt"
awk 'prev2 != "" && $0 == prev2 && prev ~ /^b/ { bad = 1 }
     { prev2 = prev; prev = $0 } END { exit bad }' "$out/sp.txt" || {
    echo "FAIL: a compare is repeated across one branch:"; cat "$out/sp.txt"; exit 1; }
echo "a sparse switch's tree makes each compare once"

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "SKIP (exec): $QEMU not found (set EMBCC_QEMU_ARM)"; exit 0; }
T=thumbv7m-none-eabi
H=tests/harness/thumb
export EMBCC_THUMB_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -c "$H/$f.c" -o "$out/$f.o" || {
        echo "FAIL: the harness does not compile for $T"; exit 1; }
done
cc -std=c99 -w -o "$out/host" tests/golden/embedded-switch.c "$H/hostio.c" || {
    echo "FAIL: the program does not compile for the host"; exit 1; }
"$out/host" > "$out/ref.txt" || { echo "FAIL: the host run failed"; exit 1; }
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $opt -c tests/golden/embedded-switch.c \
             -o "$out/p$opt.o" || {
        echo "FAIL: $opt: the program does not compile"; exit 1; }
    sh "$H/link.sh" "$out/p$opt.elf" "$out/p$opt.o" || {
        echo "FAIL: $opt: embld could not link the image"; exit 1; }
    sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" --until '==END==' \
        "$QEMU" -M lm3s6965evb -cpu cortex-m3 -nographic \
        -kernel "$out/p$opt.elf" > "$out/p$opt.txt" 2>/dev/null
    grep -q '==END==' "$out/p$opt.txt" || {
        echo "FAIL: $opt: the image did not reach the end of main:"
        sed -n '1,10p' "$out/p$opt.txt"; exit 1; }
    diff -u "$out/ref.txt" "$out/p$opt.txt" > "$out/p$opt.diff" || {
        echo "FAIL: $opt: the switches do not agree with the host:"
        head -20 "$out/p$opt.diff"; exit 1; }
done
echo "embedded-switch: tables, trees and fallthrough agree with the host at four levels"
