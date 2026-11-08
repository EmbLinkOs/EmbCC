#!/bin/sh
# lib/rt's interrupt-masking atomics in unprivileged Thread mode (#184).
#
# ARMv6-M has no exclusives, so its atomics -- and every 8-byte atomic on
# a 32-bit Cortex-M -- mask interrupts with PRIMASK (lib/rt/armv6m.c,
# atomic8.c). In unprivileged Thread mode, where an RTOS runs its tasks,
# CPSID and a write to PRIMASK are ignored: the routines ran unprotected.
# They now call __embcc_atomic_lock() and __embcc_atomic_unlock() there,
# which the RTOS defines; privileged code masks interrupts as before and
# never calls them; and with no lock defined the default traps.
#  1. On QEMU's Cortex-M3 board -- a Cortex-M0 has no unprivileged mode
#     (CONTROL.nPRIV is read-as-zero there, and it stays privileged) --
#     ARMv6-M code and ARMv7-M code, at -O0 and -O2: with the hooks, a
#     4-byte and an 8-byte update made privileged call no hook, the same
#     made unprivileged call one per update lib/rt makes (both widths on
#     ARMv6-M, the 8-byte one on ARMv7-M), and every value is right.
#  2. Without them, the first unprivileged update faults (exit 125).
set -u
echo "TEST-MARKER thumb-atomic-unpriv"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/thumb-atomic-unpriv
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
H=tests/harness/thumb-m0
command -v qemu-system-arm >/dev/null 2>&1 || { echo "skipped: no qemu-system-arm"; exit 0; }
run() {     # run NAME OPT DEFS... -> the exit status
    n=$1 o=$2; shift 2
    "$EMBCC" --target=$T $o "$@" -c tests/golden/thumb-atomic-unpriv/prog.c \
        -o "$out/$T/$n.o" || fail "$T $n does not compile"
    EMBCC_THUMB_M0_HARNESS=$out/$T sh $H/link.sh "$out/$T/$n.elf" \
        "$out/$T/$n.o" $L/libc.a $L/librt.a > "$out/$T/$n.ld" 2>&1 ||
        { cat "$out/$T/$n.ld"; fail "$T $n does not link"; }
    sh $H/run-m3.sh "$out/$T/$n.elf" > "$out/$T/$n.txt" 2>&1
}
for tn in thumbv6m-none-eabi:2 thumbv7m-none-eabi:1; do
    T=${tn%%:*} N=${tn#*:} L=build/libc/${tn%%:*}
    [ -f $L/librt.a ] && [ -f $L/libc.a ] ||
        fail "no $L/librt.a or libc.a (make rt-embedded libc-embedded)"
    mkdir -p "$out/$T"
    for f in boot io; do
        "$EMBCC" --target=$T -DHARNESS_LM3S -DSRAM_TOP=0x20010000u \
            -c $H/$f.c -o "$out/$T/$f.o" || fail "the harness's $f.c for $T"
    done
    for o in -O0 -O2; do
        run hooks$o $o -DHOOKS -DNHOOK=$N; r=$?
        [ $r = 42 ] || fail "$T $o with the RTOS's lock: exit $r, not 42 (1 a hook called privileged, 2 not as often as lib/rt was called unprivileged, 3 a wrong value)"
        run nohooks$o $o -DNHOOK=$N; r=$?
        [ $r = 125 ] || fail "$T $o with no lock defined: exit $r, not 125 (a fault)"
    done
done
echo "unprivileged Thread mode takes the RTOS's lock for lib/rt's atomics (ARMv6-M's and the 8-byte ones), privileged code masks interrupts, and no lock is a fault, at -O0 and -O2"
