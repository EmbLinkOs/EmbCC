#!/bin/sh
# An atomic reads its operands where the register allocator put them.
#
# Every atomic used to be lowered from raw stack slots -- the allocator
# kept its address, its value and its expected and desired values in
# memory (ra_target.atomic_in_reg was 0) -- so `__atomic_exchange_n(p, v)`
# with p and v arriving in rdi and rsi stored both to the frame and
# loaded them straight back: a frame, two stores and three loads around
# one xchg, in exactly the small hot functions a kernel's locks are made
# of. Here each one is a function whose operands are all parameters, so
# none of them has any reason to touch the stack:
#
#  1. at -O2 and -Os no function makes a frame access, or builds a frame;
#  2. each still contains its atomic instruction, as objdump decodes it --
#     the encoder takes any register and any base now, so the byte forms
#     of rsi and rdi need a REX prefix (`%sil`, not `%dh`), and objdump is
#     the referee for that;
#  3. a compare-exchange writes `expected` back only when it MISSED, so
#     the store sits behind a `je` -- C11 says a match leaves it alone.
#
# What they compute is tests/exec/atomics-reg.c's business, which runs
# the same shapes against gcc at -O0, -O2 and -Os.
set -u
echo "TEST-MARKER atomics-in-registers"
. "$(dirname "$0")/../../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/atomics-in-registers
rm -rf "$out"; mkdir -p "$out"

cat > "$out/a.c" <<'EOF'
#define SEQ __ATOMIC_SEQ_CST
signed char xchg8(signed char *p, signed char v)
{ return __atomic_exchange_n(p, v, SEQ); }
long xchg64(long *p, long v) { return __atomic_exchange_n(p, v, SEQ); }
short xadd16(short *p, short v) { return __atomic_fetch_add(p, v, SEQ); }
int xadd32(int *p, int v) { return __atomic_fetch_sub(p, v, SEQ); }
unsigned char cas8(unsigned char *p, unsigned char d, unsigned char e)
{ return __sync_val_compare_and_swap(p, e, d); }
int cas32(int *p, int e, int d) { return __sync_bool_compare_and_swap(p, e, d); }
int cx16(short *p, short *e, short d)
{ return __atomic_compare_exchange_n(p, e, d, 0, SEQ, SEQ); }
int cx64(long *p, long *e, long d)
{ return __atomic_compare_exchange_n(p, e, d, 0, SEQ, SEQ); }
unsigned and32(unsigned *p, unsigned v) { return __atomic_fetch_and(p, v, SEQ); }
long nand64(long *p, long v) { return __atomic_fetch_nand(p, v, SEQ); }
EOF
# function -> the instruction it must contain
want='xchg8 xchg
xchg64 xchg
xadd16 xadd
xadd32 xadd
cas8 cmpxchg
cas32 cmpxchg
cx16 cmpxchg
cx64 cmpxchg
and32 cmpxchg
nand64 cmpxchg'

for O in -O2 -Os; do
    "$EMBCC" --target=x86_64-elf $O -c "$out/a.c" -o "$out/a$O.o" \
        2> "$out/cc.log" || { echo "FAIL: compile at $O:"; cat "$out/cc.log"
                              exit 1; }
    objdump -d "$out/a$O.o" > "$out/a$O.dis" || {
        echo "FAIL: objdump rejected the object"; exit 1; }
    # one line per function: name, frame accesses, frame setups, and the
    # decoded instructions (with a lock prefix where there is one)
    awk '/^[0-9a-f]+ <[a-z0-9]+>:$/ { if (fn != "") print fn, fr, pu, mn
                                    fn = $2; gsub(/[<>:]/, "", fn)
                                    fr = 0; pu = 0; mn = ""; next }
         fn != "" && /\t/ { n = split($0, f, "\t"); ins = f[n]
                            if (ins ~ /\(%r[bs]p\)/) fr++
                            if (ins ~ /^push +%rbp/) pu++
                            gsub(/ +/, " ", ins); mn = mn "|" ins }
         END { if (fn != "") print fn, fr, pu, mn }' "$out/a$O.dis" \
        > "$out/fn$O.txt"
    echo "$want" | while read -r f insn; do
        line=$(grep "^$f " "$out/fn$O.txt") || {
            echo "FAIL: $O: no function $f in the object"; exit 1; }
        fr=$(echo "$line" | cut -d' ' -f2)
        pu=$(echo "$line" | cut -d' ' -f3)
        if [ "$fr" != 0 ] || [ "$pu" != 0 ]; then
            echo "FAIL: $O: $f touches the stack ($fr frame accesses, $pu"
            echo "      frame setups) though every operand is a parameter:"
            sed -n "/<$f>:/,/^\$/p" "$out/a$O.dis"
            exit 1
        fi
        case "$line" in
        *"|lock $insn "*|*"|$insn "*) ;;
        *) echo "FAIL: $O: $f has no $insn as objdump reads it:"
           sed -n "/<$f>:/,/^\$/p" "$out/a$O.dis"; exit 1 ;;
        esac
        case "$insn" in
        xchg) ;;
        *) case "$line" in *"|lock $insn "*) ;;
           *) echo "FAIL: $O: $f's $insn is not locked"; exit 1 ;; esac ;;
        esac
    done || exit 1
    # the byte form with the desired value in the second argument register
    grep -q "^cas8 .*cmpxchg %sil,(%rdi)" "$out/fn$O.txt" || {
        echo "FAIL: $O: cas8 should be lock cmpxchg %sil,(%rdi):"
        sed -n "/<cas8>:/,/^\$/p" "$out/a$O.dis"; exit 1; }
    # a miss writes expected back; a match does not
    for f in cx16 cx64; do
        grep "^$f " "$out/fn$O.txt" | grep -q "cmpxchg[^|]*|je [^|]*|mov " || {
            echo "FAIL: $O: $f does not skip the write-back on a match:"
            sed -n "/<$f>:/,/^\$/p" "$out/a$O.dis"; exit 1; }
    done
done
echo "ten atomics with their operands in registers make no frame access,"
echo "at -O2 and -Os, and objdump reads each one's locked instruction"
