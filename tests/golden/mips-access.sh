#!/bin/sh
# MIPS32: a word or halfword access the program can see as ONE access is
# one instruction.
#
# A misaligned lw/sw traps, so an access the compiler cannot prove aligned
# (a packed struct's member) is two: lwl/lwr (swl/swr) for a word, two
# bytes for a halfword. That is right for a packed member and wrong for
# everything that relies on the access being single: an atomic load or
# store, which an interrupt between the halves would tear, and a volatile
# device register, which a read-modify-write would read twice and write
# in two pieces. C promises those are aligned, so they must be lw/sw
# (lhu/sh) -- including inside `x op= v` and `x++`, whose load and store
# irgen builds by hand. And a packed member must still be split, op= and
# ++ included, or it traps.
#
# Each function is disassembled and its access instructions checked; the
# packed ones then run on the board at a misaligned address, in an image
# whose .data ends mid-word -- which EmbLD must pad, or the startup's
# word loop over .bss traps.
set -u
# Run BIG-endian (mips-none-elf) as tests/golden/mips-be-access.sh, which sets
# MIPS_BE=1.
if [ "${MIPS_BE:-0}" = 1 ]; then
    NAME=mips-be-access T=mips-none-elf MT=mips-unknown-elf
    QEMU=${EMBCC_QEMU_MIPSEB:-qemu-system-mips}
else
    NAME=mips-access T=mipsel-none-elf MT=mipsel-unknown-elf
    QEMU=${EMBCC_QEMU_MIPS:-qemu-system-mipsel}
fi
echo "TEST-MARKER $NAME"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "skipped: $OD not found"; exit 0; }
out=tests/golden/out/$NAME
rm -rf "$out"; mkdir -p "$out"

cat > "$out/a.c" <<'CEOF'
#include <stdatomic.h>
#include <stdarg.h>
struct regs { unsigned con, stat; unsigned short half; };
#define DEV ((volatile struct regs *)0xbf880000u)
#define REG (*(volatile unsigned *)0xbf880010u)
#define HREG (*(volatile unsigned short *)0xbf880014u)
struct __attribute__((packed)) pk { char c; int x; short h; };
struct cnt { int n; short s; };

atomic_int aw;
_Atomic unsigned short ah;
_Atomic int ax;

/* single: no lwl/lwr/swl/swr, no byte pair for a halfword */
int  single_aload(void)        { return atomic_load(&aw); }
void single_astore(int v)      { atomic_store(&aw, v); }
int  single_aload_h(void)      { return atomic_load(&ah); }
void single_astore_h(int v)    { atomic_store(&ah, (unsigned short)v); }
int  single_aplain(int v)      { ax = v; return ax; }
void single_vor(void)          { REG |= 4; }
void single_vmember(void)      { DEV->con ^= 1; DEV->stat += 2; }
void single_vhalf(void)        { HREG += 1; DEV->half |= 0x80; }
void single_vinc(void)         { DEV->stat++; --REG; }
void single_inc(struct cnt *p) { p->n++; p->s--; }
void single_op(struct cnt *p, int k) { p->n += k; p->s *= 3; }
void single_idx(int *a, int i) { a[i] += 7; }
/* not about atomicity: va_arg's slots are words, and its reads were lwl/lwr */
long long single_va(int n, ...)
{ va_list ap; union { double d; long long q; } u; va_start(ap, n);
  int k = va_arg(ap, int); u.d = va_arg(ap, double); long long q = va_arg(ap, long long);
  va_end(ap); return u.q + k + q; }

/* split: a packed member, read, written, op= and ++ */
int  split_load(struct pk *p)        { return p->x; }
void split_op(struct pk *p, int k)   { p->x += k; }
void split_inc(struct pk *p)         { p->x++; }
void split_hop(struct pk *p)         { p->h += 3; }
CEOF

for opt in -O0 -O2; do
    "$EMBCC" --target=$T $opt -c "$out/a.c" -o "$out/a$opt.o" || {
        echo "$opt: the probe does not compile"; exit 1; }
    "$OD" -d "$out/a$opt.o" > "$out/a$opt.dis" || exit 1
    # function name, then its partial-access count
    awk '/^[0-9a-f]+ </ { fn = $2; gsub(/[<>:]/, "", fn); n[fn] += 0; next }
         /\t(lwl|lwr|swl|swr|lbu|lb|sb)\t/ { n[fn]++ }
         END { for (f in n) print f, n[f] }' "$out/a$opt.dis" | sort > "$out/n$opt"
    bad=$(awk '$1 ~ /^single_/ && $2 != 0' "$out/n$opt")
    [ -z "$bad" ] || {
        echo "$opt: an access C promises aligned (atomic, volatile, op=, ++, va_arg) is split:"
        echo "$bad"; exit 1; }
    bad=$(awk '$1 ~ /^split_/ && $2 == 0' "$out/n$opt")
    [ -z "$bad" ] || {
        echo "$opt: a packed member is accessed with an aligned-only instruction:"
        echo "$bad"; exit 1; }
    nsingle=$(grep -c '^single_' "$out/n$opt")
    nsplit=$(grep -c '^split_' "$out/n$opt")
    [ "$nsingle" -eq 13 ] && [ "$nsplit" -eq 4 ] || {
        echo "$opt: found $nsingle single_ and $nsplit split_ functions, wanted 13 and 4"
        exit 1; }
done
echo "atomic, volatile, op=, ++ and va_arg accesses are single lw/sw/lhu/sh at -O0 and -O2; packed members are split"

# ---- the packed ones, on the board at a misaligned address ---------------
command -v "$QEMU" >/dev/null 2>&1 || { echo "board run skipped: $QEMU not found"; exit 0; }
export EMBCC_MIPS_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -O1 -c "tests/harness/mips/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile"; exit 1; }
done
cat > "$out/m.c" <<'CEOF'
struct __attribute__((packed)) pk { char c; int x; short h; };
int  split_load(struct pk *p);
void split_op(struct pk *p, int k);
void split_inc(struct pk *p);
void split_hop(struct pk *p);
/* h is word-aligned, so v[0].x is at h + 5 */
static struct { int a; struct pk v[2]; } h = { 9, { { 2, -5, -7 }, { 1, 0x11223344, 0x5566 } } };
/* .data ends mid-word, and .bss (a.c's atomics) follows: the startup's
 * word loops must still start and end on words (EmbLD pads on MIPS) */
static volatile char tail[3] = { 1, 2, 3 };
int main(void)
{
    struct pk *p = &h.v[0];
    if (((unsigned)&p->x & 3) == 0) return 1;
    if (split_load(p) != -5 || split_load(&h.v[1]) != 0x11223344) return 2;
    split_op(p, 1000);
    if (p->x != 995) return 3;
    split_inc(p);
    if (p->x != 996 || p->c != 2 || p->h != -7) return 4;
    split_hop(p);
    if (p->h != -4 || p->x != 996) return 5;
    if (h.a != 9 || h.v[1].c != 1 || h.v[1].x != 0x11223344) return 6;
    if (tail[0] + tail[1] + tail[2] != 6) return 7;
    return 42;
}
CEOF
for opt in -O0 -O2; do
    "$EMBCC" --target=$T -O1 -c "$out/m.c" -o "$out/m$opt.o" || {
        echo "the board program does not compile"; exit 1; }
    sh tests/harness/mips/link.sh "$out/m$opt.elf" "$out/m$opt.o" "$out/a$opt.o" || {
        echo "$opt: the board program does not link"; exit 1; }
    sh tests/harness/mips/run.sh "$out/m$opt.elf" > "$out/m$opt.out" 2>&1
    grep -q '==EXIT 42 ==' "$out/m$opt.out" || {
        echo "$opt: the board run went wrong (a packed member, or the startup's word loops):"
        cat "$out/m$opt.out"; exit 1; }
done
echo "packed members at a misaligned address read, op= and ++ correctly on the board"
