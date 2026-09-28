#!/bin/sh
# Every volatile access happens, on every target, at every level.
#
# A volatile object is a device register or memory shared with an
# interrupt: each read is an event (it may pop a FIFO, clear a status
# flag) and the value may differ each time, so two reads may never become
# one. That is exactly what `*p` did at -O2 on x86-64, aarch64 and Thumb:
# the dereference built its load by hand and never marked it volatile,
# while every other path went through emit_load, which did -- so a status
# register polled twice was read once.
#
# Each function below reads a volatile object THREE times, through one
# lvalue form each, and the object code must contain three loads from
# memory that is not the frame. A merged read shows as fewer.
set -u
echo "TEST-MARKER volatile-access"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/volatile-access
rm -rf "$out"; mkdir -p "$out"
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }

cat > "$out/v.c" <<'CEOF'
struct dev { volatile unsigned dr, sr; };
volatile unsigned vg;
unsigned f_deref(volatile unsigned *p)  { return *p + *p + *p; }
unsigned f_index(volatile unsigned *p)  { return p[1] + p[1] + p[1]; }
unsigned f_arrow(struct dev *d)         { return d->sr + d->sr + d->sr; }
unsigned f_global(void)                 { return vg + vg + vg; }
unsigned f_cast(unsigned long a)
{ return *(volatile unsigned *)a + *(volatile unsigned *)a +
         *(volatile unsigned *)a; }
void f_store(volatile unsigned *p)      { *p = 1; *p = 2; *p = 3; }
CEOF

# target, objdump flags, and a pattern for a load / a store from
# something other than the frame
check() {
    T=$1 ODF=$2 LD=$3 ST=$4
    for opt in -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$out/v.c" -o "$out/v.o" || {
            echo "$T $opt: does not compile"; exit 1; }
        # shellcheck disable=SC2086
        "$OD" -d $ODF "$out/v.o" > "$out/v.s"
        for f in f_deref f_index f_arrow f_global f_cast f_store; do
            pat=$LD; [ $f = f_store ] && pat=$ST
            n=$(sed -n "/<$f>:/,/^\$/p" "$out/v.s" | grep -E "$pat" |
                grep -vcE 'sp[],)]|\[x29|\(%rsp|\(%rbp|\(s0\)|\(fp\)')
            [ "$n" = 3 ] || {
                echo "$T $opt $f: $n accesses to a volatile object, not 3"
                sed -n "/<$f>:/,/^\$/p" "$out/v.s"; exit 1; }
        done
    done
}
check thumbv7em-none-eabi "--triple=thumbv7em" '[[:space:]]ldr' '[[:space:]]str'
check riscv32-unknown-elf "--mattr=+c,+m" '[[:space:]](c\.)?lw[[:space:]]' \
      '[[:space:]](c\.)?sw[[:space:]]'
check aarch64-elf "" '[[:space:]]ldr[[:space:]]+w' '[[:space:]]str[[:space:]]+w'
check x86_64-elf "" 'mov[l]?[[:space:]]+(0x[0-9a-f]+)?\(%r[a-z0-9]+\),' \
      'mov[l]?[[:space:]]+(\$0x[0-9a-f]+|%[a-z0-9]+),[[:space:]]*(0x[0-9a-f]+)?\(%r'
echo "three volatile reads are three loads, and three writes three stores,
through *p, p[i], p->m, a global and a cast address, on Thumb, RISC-V,
aarch64 and x86-64 at -O1, -O2 and -Os"
