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
#
# The object can be a pointer: `unsigned *volatile p` -- a cursor an
# interrupt handler advances. The parser dropped a qualifier after `*`,
# so that pointer was an ordinary one, polled once and stored once --
# and so was a volatile written after a typedef or struct name, `u32
# volatile *r`, the other common spelling of a device register.
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
typedef unsigned u32;
unsigned f_post(u32 volatile *r)        { return *r + *r + *r; }
unsigned f_spost(struct dev volatile *d) { return d->dr + d->dr + d->dr; }
CEOF
cat > "$out/p.c" <<'CEOF'
unsigned *volatile vp;
struct ring { unsigned *volatile head; };
typedef unsigned *volatile vptr_t;
vptr_t tp;
int f_vglobal(void)           { return (vp != 0) + (vp != 0) + (vp != 0); }
int f_vmember(struct ring *r) { return (r->head != 0) + (r->head != 0) + (r->head != 0); }
int f_vtypedef(void)          { return (tp != 0) + (tp != 0) + (tp != 0); }
void f_vstore(unsigned *q)    { vp = q; vp = q; vp = q; }
CEOF

# Volatile BIT-FIELDS, a peripheral register's usual shape: the storage
# unit was loaded and stored through an unqualified type, so `r->mode`
# three times was one load at -O1 and up and three stores to `r->cnt`
# were one. A store also re-read the unit for the assignment's value --
# a second access to a register that may clear on read.
cat > "$out/b.c" <<'CEOF'
struct reg { unsigned en : 1, mode : 3, cnt : 12; };
unsigned f_bread(volatile struct reg *r) { return r->mode + r->mode + r->mode; }
void f_bstore(volatile struct reg *r) { r->cnt = 1; r->cnt = 2; r->cnt = 3; }
unsigned f_bset(volatile struct reg *r) { return (r->en = 1) + (r->en = 1) + (r->en = 1); }
CEOF

# Volatile LOCALS: their accesses are to the frame, and each must happen.
# Store forwarding handed `volatile int v = 3`'s 3 straight to its reads,
# dead-code elimination dropped `(void)v`, and the register allocator kept
# such a local in a register, where no access is one to memory at all.
cat > "$out/l.c" <<'CEOF'
int f_lread(void)   { volatile int v = 3; return v + v + v; }
int f_lvoid(void)   { volatile int v = 3; (void)v; (void)v; (void)v; return 0; }
void f_lstore(void) { volatile int v; v = 1; v = 2; v = 3; }
CEOF

# target, objdump flags, and a pattern for a load / a store from
# something other than the frame
check() {
    T=$1 ODF=$2 LD=$3 ST=$4 SRC=${5:-v}
    FNS=${6:-"f_deref f_index f_arrow f_global f_cast f_store f_post f_spost"}
    for opt in -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$out/$SRC.c" -o "$out/v.o" || {
            echo "$T $opt: does not compile"; exit 1; }
        # shellcheck disable=SC2086
        "$OD" -d $ODF "$out/v.o" > "$out/v.s"
        for f in $FNS; do
            pat=$LD; case $f in *store) pat=$ST ;; esac
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
# target, objdump flags, a load / store pattern: each must reach the FRAME
# -O2 and -Os, where temporaries are in registers and the frame accesses
# left are the local's own (at -O1 on Thumb every temporary has a slot)
checkl() {
    T=$1 ODF=$2 LD=$3 ST=$4
    for opt in -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$out/l.c" -o "$out/l.o" || {
            echo "$T $opt: does not compile"; exit 1; }
        # shellcheck disable=SC2086
        "$OD" -d $ODF "$out/l.o" > "$out/l.s"
        for f in f_lread f_lvoid f_lstore; do
            pat=$LD; case $f in *store) pat=$ST ;; esac
            n=$(sed -n "/<$f>:/,/^\$/p" "$out/l.s" | grep -E "$pat" |
                grep -cE 'sp[],)]|\[x29|\(%rsp|\(%rbp|\(s0\)|\(fp\)')
            [ "$n" = 3 ] || {
                echo "$T $opt $f: $n accesses to a volatile local, not 3"
                sed -n "/<$f>:/,/^\$/p" "$out/l.s"; exit 1; }
        done
    done
}
checkl thumbv7em-none-eabi "--triple=thumbv7em" '[[:space:]]ldr' '[[:space:]]str'
checkl riscv32-unknown-elf "--mattr=+c,+m" '[[:space:]](c\.)?lw[[:space:]]' \
       '[[:space:]](c\.)?sw[[:space:]]'
checkl aarch64-elf "" '[[:space:]]ldu?r[[:space:]]+w' '[[:space:]]stu?r[[:space:]]+w'
checkl x86_64-elf "" 'mov[l]?[[:space:]]+-?(0x[0-9a-f]+)?\(%r[a-z0-9]+\),' \
       'mov[l]?[[:space:]]+(\$0x[0-9a-f]+|%[a-z0-9]+),[[:space:]]*-?(0x[0-9a-f]+)?\(%r'
# bit-fields: three reads, three stores, and an assignment's value that
# is not a fourth read (f_bset: three read-modify-writes, so three loads)
BF="f_bread f_bstore f_bset"
check thumbv7em-none-eabi "--triple=thumbv7em" '[[:space:]]ldr' '[[:space:]]str' b "$BF"
check riscv32-unknown-elf "--mattr=+c,+m" '[[:space:]](c\.)?lw[[:space:]]' \
      '[[:space:]](c\.)?sw[[:space:]]' b "$BF"
check aarch64-elf "" '[[:space:]]ldr[[:space:]]+w' '[[:space:]]str[[:space:]]+w' b "$BF"
check x86_64-elf "" 'mov[l]?[[:space:]]+(0x[0-9a-f]+)?\(%r[a-z0-9]+\),' \
      'mov[l]?[[:space:]]+(\$0x[0-9a-f]+|%[a-z0-9]+),[[:space:]]*(0x[0-9a-f]+)?\(%r' b "$BF"
# the pointer objects, at pointer width
PF="f_vglobal f_vmember f_vtypedef f_vstore"
check thumbv7em-none-eabi "--triple=thumbv7em" '[[:space:]]ldr' '[[:space:]]str' p "$PF"
check riscv32-unknown-elf "--mattr=+c,+m" '[[:space:]](c\.)?lw[[:space:]]' \
      '[[:space:]](c\.)?sw[[:space:]]' p "$PF"
check aarch64-elf "" '[[:space:]]ldr[[:space:]]+x' '[[:space:]]str[[:space:]]+x' p "$PF"
check x86_64-elf "" 'movq?[[:space:]]+(0x[0-9a-f]+)?\(%r[a-z0-9]+\),' \
      'movq?[[:space:]]+%[a-z0-9]+,[[:space:]]*(0x[0-9a-f]+)?\(%r' p "$PF"
echo "three volatile reads are three loads, and three writes three stores,
through *p, p[i], p->m, a global, a cast address and a qualifier written
after a typedef or struct name, of a volatile bit-field, and of a volatile
pointer object (a global, a member, a typedef), and of a volatile
local (read, read and discarded, written; at -O2 and -Os), on Thumb,
RISC-V, aarch64 and x86-64 at -O1, -O2 and -Os"
