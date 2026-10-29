#!/bin/sh
# RISC-V: a local's address is the frame base and an offset, not a value.
#
# `&x` was a temporary like any other: given a stack slot of its own when
# registers were short (or at -Os, where they always were for it), and
# loaded back for every access through it -- building a float complex in
# a local was `addi t0, sp, 4; sw t0, 0(sp); lw t2, 0(sp); sw a0, 0(t2);
# lw t2, 0(sp); sw a1, 4(t2) ...`. Now it is rebuilt where it is read (one
# addi), and an access through it is the frame base with the offset folded
# in: `sw a0, 0(sp)`. Checked on RV32 and RV64 at -Os and -O2:
#   - mk builds a float complex and returns it: no address in a t register
#     and no load of a saved address -- the stores and loads are sp's;
#   - pass hands &p to a call: made from sp into a0. (Keeping such an
#     address out of the allocator altogether, so the fields are stored
#     at sp, measured 0.2% larger over lib/libc: a register holding it is
#     usually worth keeping.)
# tests/exec covers what these compute, on the boards.
set -u
echo "TEST-MARKER riscv-frame-addr"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: $OD not found"; exit 0; }
out=tests/golden/out/riscv-frame-addr
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
cat > "$out/fa.c" <<'EOF'
float _Complex mk(float a, float b)
{
    float _Complex z;
    __real__ z = a;
    __imag__ z = b;
    return z;
}
struct pt { int x, y; };
int use(struct pt *);
int pass(int a, int b) { struct pt p = { a, b }; return use(&p) + 1; }
EOF
for t in riscv32-unknown-elf riscv64-unknown-elf; do
    for O in -Os -O2; do
        "$EMBCC" --target=$t $O -c "$out/fa.c" -o "$out/fa.o" || fail "$t $O: compile"
        "$OD" -d --no-show-raw-insn "$out/fa.o" > "$out/fa.dis"
        body() { sed -n "/<$1>:/,/^\$/p" "$out/fa.dis"; }
        body mk | grep -qE '(addi|mv)[[:space:]]+t[0-9], sp' &&
            { body mk; fail "$t $O: mk keeps the local's address in a t register"; }
        body mk | grep -qE 'l[wd][[:space:]]+t[0-9],' &&
            { body mk; fail "$t $O: mk loads a saved address back"; }
        n=$(body mk | grep -cE '^[[:space:]]+[0-9a-f]+:')
        [ "$n" -le 8 ] || { body mk; fail "$t $O: mk is $n instructions, want at most 8"; }
        body pass | grep -qE '(addi|mv)[[:space:]]+a0, sp' ||
            { body pass; fail "$t $O: pass does not make &p in a0 from sp"; }
    done
done
echo "riscv-frame-addr: a local's address is sp and an offset, folded into the access, RV32 and RV64"
