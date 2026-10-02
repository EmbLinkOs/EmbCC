#!/bin/sh
# A relocated value its field cannot hold is refused, by symbol and reach.
#
# Writing the low bits makes an address nothing reports: the field holds
# SOME address and the program goes there. A RIP-relative reference to
# data placed 8GB from the code linked without a word, and so did a
# RISC-V `lui` pair at 0x80000000 -- where lui sign-extends at RV64, so
# the program would have run with its globals at 0xffffffff80000000.
set -u
echo "TEST-MARKER embld-reloc-range"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/embld-reloc-range-$ARCH
rm -rf "${out:?}"; mkdir -p "$out"
printf 'int g = 5;\nint _start(void) { return g; }\n' > "$out/a.c"

# 1. x86-64: RIP-relative data 8GB from the code.
"$EMBCC" --target=x86_64-elf -c "$out/a.c" -o "$out/a.o" || {
    echo "FAIL: the x86-64 object does not compile"; exit 1; }
"$EMBLD" -e _start -Ttext 0x400000 "$out/a.o" -o "$out/near.elf" || {
    echo "FAIL: an ordinary x86-64 link was refused"; exit 1; }
if "$EMBLD" -e _start -Ttext 0x400000 -Tdata 0x200000000 "$out/a.o" \
        -o "$out/far.elf" 2> "$out/far.err"; then
    echo "FAIL: data 8GB from RIP-relative code linked, its displacement truncated"
    exit 1
fi
grep -q "R_X86_64_PC32 against 'g'" "$out/far.err" || {
    echo "FAIL: the refusal does not name the relocation and symbol:"
    cat "$out/far.err"; exit 1; }
echo "a RIP-relative reference out of reach is refused by name"

# 2. RV64: a medlow (lui) object linked where lui cannot reach.
CLANG=${EMBCC_REF_GCC_RISCV:-clang}
if command -v "$CLANG" > /dev/null 2>&1 &&
   "$CLANG" -target riscv64-unknown-elf -march=rv64im -mabi=lp64 \
       -mcmodel=medlow -mno-relax -O1 -c "$out/a.c" -o "$out/r.o" 2> /dev/null
then
    "$EMBLD" -e _start -Ttext 0x10000 -Tstack 0x80000 "$out/r.o" \
        -o "$out/rlow.elf" || {
        echo "FAIL: a medlow image in the low 2GB was refused"; exit 1; }
    if "$EMBLD" -e _start -Ttext 0x80000000 -Tstack 0x80800000 "$out/r.o" \
            -o "$out/rhigh.elf" 2> "$out/rhigh.err"; then
        echo "FAIL: lui reached 0x80000000 at RV64, sign-extended to a wrong address"
        exit 1
    fi
    grep -q "R_RISCV_HI20 against 'g'" "$out/rhigh.err" || {
        echo "FAIL: the RV64 refusal does not name the relocation:"
        cat "$out/rhigh.err"; exit 1; }
    echo "and a RISC-V lui pair out of reach at RV64 is refused by name"
else
    echo "skipped the RISC-V half: no $CLANG for riscv64"
fi
