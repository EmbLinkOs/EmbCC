#!/bin/sh
# RISC-V: s0 is a register like s1, and the busiest callee-saved values
# get s0 and s1.
#
# The compressed loads, stores and most compressed ALU forms reach x8-x15
# only, and of the callee-saved registers just s0 (x8) and s1 (x9) are
# there. s0 used to be kept out of the allocator as "the frame pointer",
# which this backend does not keep -- frame slots are addressed from sp
# and DWARF's frame base is sp -- except in a function with a
# variable-length array, where s0 holds the frame base. And the colourer
# hands callee-saved registers out in pool order, so the busiest pointer
# could sit in s3 with every access a four-byte one; rv_lowregs renames
# them, and gen_func_best tries the function with and without the rename.
#
# Here, on RV32 and RV64 at -Os and -O2, tests/exec/lowregs-notify.c's
# notify_isr (FreeRTOS's xTaskGenericNotifyFromISR shape):
#   - the base most of its loads and stores use, sp aside, is in x8-x15;
#   - the default is no larger than either forced choice
#     (EMBCC_RV_LOWREGS=0/1);
# and a function with a variable-length array writes s0 only to set the
# frame base and to restore it. The programs run on the boards with the
# exec corpus (tests/exec/lowregs-notify.c, vla.c, alloca.c).
set -u
echo "TEST-MARKER riscv-lowregs"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
# A shape test of the full pool: held to EMBCC_RA_MAXPOOL registers, the
# pointer may have no register at all.
[ -n "${EMBCC_RA_MAXPOOL:-}" ] && { echo "SKIP: EMBCC_RA_MAXPOOL shrinks the pool"; exit 0; }
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "SKIP: no $OBJDUMP"; exit 0; }
out=tests/golden/out/riscv-lowregs
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
src=tests/exec/lowregs-notify.c
fsize() { "$OBJDUMP" -t "$1" | awk '/ notify_isr$/ { print $5 }'; }
cat > "$out/vla.c" <<'EOF'
void use(int *p, int n);
int g(int a, int b, int c, int d, int e, int n)
{
    int v[n];
    use(v, n);
    int s = a * b;
    use(v, s);
    s += c * d;
    use(v, e);
    return s + v[0] + a + b + c + d + e;
}
EOF
n=0
for t in riscv32-unknown-elf riscv64-unknown-elf; do
    for O in -Os -O2; do
        tag=$t$O
        "$EMBCC" --target=$t $O -c "$src" -o "$out/$tag.o" || fail "$tag: compile"
        "$OBJDUMP" -d --no-show-raw-insn "$out/$tag.o" |
            sed -n '/<notify_isr>:/,/^$/p' > "$out/$tag.dis"
        b=$(grep -oE '\((s[0-9]+|a[0-9]|t[0-9]|ra|gp|tp)\)' "$out/$tag.dis" |
            sort | uniq -c | sort -rn | awk 'NR == 1 { print $2 }')
        case "$b" in
            "(s0)"|"(s1)"|"(a0)"|"(a1)"|"(a2)"|"(a3)"|"(a4)"|"(a5)") ;;
            *) cat "$out/$tag.dis"; fail "$tag: the busiest base is $b, outside x8-x15" ;;
        esac
        d=$(fsize "$out/$tag.o")
        for L in 0 1; do
            EMBCC_RV_LOWREGS=$L "$EMBCC" --target=$t $O -c "$src" \
                -o "$out/$tag-$L.o" || fail "$tag: EMBCC_RV_LOWREGS=$L"
            f=$(fsize "$out/$tag-$L.o")
            [ $((0x$d)) -le $((0x$f)) ] ||
                fail "$tag: $((0x$d)) bytes by default, $((0x$f)) with EMBCC_RV_LOWREGS=$L"
        done
        "$EMBCC" --target=$t $O -c "$out/vla.c" -o "$out/vla-$tag.o" ||
            fail "$tag: vla.c"
        "$OBJDUMP" -d --no-show-raw-insn "$out/vla-$tag.o" > "$out/vla-$tag.dis"
        grep -q 'mv	s0, sp' "$out/vla-$tag.dis" ||
            { cat "$out/vla-$tag.dis"; fail "$tag: the VLA's frame base is not s0"; }
        if grep -E '	[a-z.]+	s0, ' "$out/vla-$tag.dis" |
               grep -vE 'mv	s0, sp|l[wd]	s0, |s[bhwd]	s0, '; then
            fail "$tag: s0 is written in a function whose frame base it is"
        fi
        n=$((n + 1))
    done
done
echo "riscv-lowregs: $n target/level pairs keep the busiest pointer in x8-x15, and a VLA's s0 is its frame base only"
