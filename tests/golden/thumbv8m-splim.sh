#!/bin/sh
# The ARMv8-M stack-limit registers in inline asm: MSPLIM, PSPLIM and
# their TrustZone _ns aliases.
#  1. Encodings: `mrs`/`msr` of each assemble to the bytes clang gives,
#     on both ARMv8-M Mainline ABIs.
#  2. The board: tests/golden/thumbv8m-splim/splim.c on QEMU's
#     mps2-an505 (Cortex-M33), at -O0 and -O2. It writes both limits and
#     reads them back, then lets a thread on the PSP recurse past PSPLIM.
#     The core must stop it with a UsageFault whose CFSR has STKOF, with
#     the PSP still at or above the limit -- the per-task stack
#     protection an RTOS builds on ARMv8-M.
#  3. ARMv7-M has no stack-limit registers: they are refused by name.
set -u
echo "TEST-MARKER thumbv8m-splim"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
QA=${EMBCC_QEMU_ARM:-qemu-system-arm}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
CLANG=${EMBCC_REF_GCC_THUMB:-clang}
out=tests/golden/out/thumbv8m-splim
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# ---- 1. the encodings, against clang ----------------------------------------
cat > "$out/regs.c" <<'EOF'
void w(unsigned v)
{
    __asm__ volatile("msr msplim, %0" : : "r"(v));
    __asm__ volatile("msr psplim, %0" : : "r"(v));
    __asm__ volatile("msr msplim_ns, %0" : : "r"(v));
    __asm__ volatile("msr psplim_ns, %0" : : "r"(v));
}
unsigned r(void)
{
    unsigned a, b, c, d;
    __asm__ volatile("mrs %0, msplim" : "=r"(a));
    __asm__ volatile("mrs %0, psplim" : "=r"(b));
    __asm__ volatile("mrs %0, msplim_ns" : "=r"(c));
    __asm__ volatile("mrs %0, psplim_ns" : "=r"(d));
    return a ^ b ^ c ^ d;
}
EOF
for T in thumbv8m.main-none-eabi thumbv8m.main-none-eabihf; do
    "$EMBCC" --target=$T -O2 -c "$out/regs.c" -o "$out/e.o" ||
        fail "$T: the stack-limit registers do not assemble"
    n=$("$OD" -d --triple=thumbv8m.main "$out/e.o" | grep -cE '	(msr|mrs)	.*(ms|ps)plim')
    [ "$n" = 8 ] || fail "$T: $n of the 8 msr/mrs disassemble as the stack-limit registers"
    if command -v "$CLANG" >/dev/null 2>&1 &&
       "$CLANG" --target=$T -mcpu=cortex-m33 -mcmse -O2 -c "$out/regs.c" -o "$out/c.o" 2>/dev/null; then
        # the special-register field is what matters; the register
        # allocation may differ, so compare the mnemonic and the SYSm
        "$OD" -d --triple=thumbv8m.main "$out/e.o" | grep -oE '	(msr|mrs)	[a-z_]*, [a-z_0-9]*|	(mrs)	r[0-9]+, [a-z_]+' |
            sed -E 's/r[0-9]+/R/g' > "$out/e.txt"
        "$OD" -d --triple=thumbv8m.main "$out/c.o" | grep -oE '	(msr|mrs)	[a-z_]*, [a-z_0-9]*|	(mrs)	r[0-9]+, [a-z_]+' |
            sed -E 's/r[0-9]+/R/g' > "$out/c.txt"
        cmp -s "$out/e.txt" "$out/c.txt" || { diff "$out/c.txt" "$out/e.txt"
            fail "$T: the msr/mrs of the stack-limit registers are not clang's"; }
    fi
done
if command -v "$CLANG" >/dev/null 2>&1; then
    echo "  msplim, psplim and their _ns aliases assemble as clang's, on both ARMv8-M ABIs"
else
    echo "  msplim, psplim and their _ns aliases assemble (no clang: the encodings were not compared)"
fi

# ---- 3. refused on ARMv7-M ------------------------------------------------------
"$EMBCC" --target=thumbv7m-none-eabi -O2 -c "$out/regs.c" -o "$out/v7.o" 2> "$out/v7.err" &&
    fail "thumbv7m accepted msplim"
grep -q '"msplim" is not an ARMv7-M special register' "$out/v7.err" ||
    { cat "$out/v7.err"; fail "the ARMv7-M refusal does not name msplim"; }
echo "  refused by name on ARMv7-M, which has no stack limit"

# ---- 2. the board ---------------------------------------------------------------
command -v "$QA" >/dev/null 2>&1 || { echo "SKIP: no $QA for the board run"; exit 0; }
"$QA" -M help 2>/dev/null | grep -q '^mps2-an505 ' || { echo "SKIP: this QEMU has no mps2-an505"; exit 0; }
want='msplim 0x101f8000 read back
psplim read back
recursing on the PSP
usagefault STKOF psp at or above psplim'
for O in -O0 -O2; do
    "$EMBCC" --target=thumbv8m.main-none-eabi $O -c tests/golden/thumbv8m-splim/splim.c \
        -o "$out/s$O.o" &&
    "$EMBLD" -e reset -Ttext 0x10000000 -Tdata 0x10100000 "$out/s$O.o" -o "$out/s$O.elf" ||
        fail "splim.c $O does not build"
    sh tests/harness/qrun.sh 20 "$QA" -M mps2-an505 -cpu cortex-m33 -nographic \
        -chardev file,id=semi,path="$out/s$O.txt" \
        -semihosting-config enable=on,chardev=semi \
        -kernel "$out/s$O.elf" > /dev/null 2>&1
    st=$?
    [ "$(cat "$out/s$O.txt" 2>/dev/null)" = "$want" ] && [ $st = 0 ] || {
        cat "$out/s$O.txt" 2>/dev/null
        fail "splim.c $O: the limits are not enforced as written (status $st)"; }
done
echo "  on the Cortex-M33 at -O0 and -O2: both limits read back, and a thread past PSPLIM takes a UsageFault (STKOF) with the PSP still above it"
