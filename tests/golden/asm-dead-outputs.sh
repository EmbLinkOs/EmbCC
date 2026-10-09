#!/bin/sh
# An asm whose outputs nothing reads. The register allocator may give two
# dead values one register (a dead value interferes with nothing), and the
# backends moved every value output to its home as one parallel move --
# two writes to one register, which is no move: Thumb and ARM stopped with
# "internal error: an asm's outputs are not a well-formed move", RISC-V,
# LoongArch and Xtensa refused the asm. GCC compiles it; an output nothing
# reads simply has no home to fill.
#
#   - two to four outputs, some never read, compile on twelve targets at
#     -O0, -O1, -O2 and -Os with the IR verifier on;
#   - on the Cortex-M3 and RV32 boards, a template writing three outputs
#     whose first two are dead (one in a register variable, as the case
#     was found) runs and returns the third's value, and a live output
#     beside the dead ones keeps its own.
set -u
echo "TEST-MARKER asm-dead-outputs"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/asm-dead-outputs
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
export EMBCC_VERIFY=1

cat > "$out/dead.c" <<'EOF'
unsigned f(unsigned p, unsigned b)
{
    unsigned lo, hi, st;
    __asm__ volatile("" : "=&r"(lo), "=&r"(hi), "=&r"(st) : "r"(p), "r"(b));
    return st;
}
unsigned g(unsigned p)
{
    unsigned a, b, c, d;
    __asm__ volatile("" : "=r"(a), "=r"(b), "=r"(c), "=r"(d) : "r"(p));
    return b;
}
EOF
for T in x86_64-elf aarch64-none-elf thumbv7m-none-eabi thumbv6m-none-eabi \
         armv7a-none-eabi riscv32-unknown-elf riscv64-unknown-elf \
         mipsel-none-elf mips64el-none-elf loongarch64-none-elf \
         xtensa-none-elf tricore-none-elf; do
    for O in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $O -c "$out/dead.c" -o "$out/dead.o" \
            > "$out/dead.err" 2>&1 ||
            fail "$T $O: $(head -2 "$out/dead.err")"
    done
done
echo "asms with dead outputs compile on twelve targets at four levels"

# ---- run: the third output's value comes back -------------------------------
prog() {        # prog FILE TEMPLATE: the program, around one target's template
    cat > "$1" <<EOF
void putn(long v);
void puts_(const char *s);
static volatile unsigned vp = 11, vb = 22;
__attribute__((noinline)) static unsigned three(unsigned p, unsigned b)
{
    $3
    unsigned st;
    __asm__ volatile($2 : "=&r"(lo), "=&r"(hi), "=&r"(st) : "r"(p), "r"(b));
    return st;
}
__attribute__((noinline)) static unsigned live(unsigned p, unsigned b)
{
    unsigned x, y, z;
    __asm__ volatile($2 : "=&r"(x), "=&r"(y), "=&r"(z) : "r"(p), "r"(b));
    return y;          /* x and z dead, y read */
}
int main(void)
{
    puts_("three "); putn((long)three(vp, vb)); puts_("\n");
    puts_("live "); putn((long)live(vp, vb)); puts_("\n");
    return 42;
}
EOF
}
check_out() {   # check_out LOG WHAT
    tr -d '\r' < "$1" > "$1.txt"
    grep -q "^three 7 *$" "$1.txt" && grep -q "^live 22 *$" "$1.txt" ||
        fail "$2 did not print three 7 / live 22: $(head -4 "$1.txt")"
}

QARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
if command -v "$QARM" >/dev/null 2>&1; then
    T=thumbv7m-none-eabi
    d=$out/m3
    mkdir -p "$d"
    prog "$d/prog.c" '"mov %0, %3\n\tmov %1, %4\n\tmovs %2, #7"' \
        'register unsigned lo __asm__("r2"); register unsigned hi __asm__("r3");'
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c tests/harness/thumb/$f.c -o "$d/$f.o" || fail "$f.c"
    done
    for O in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $O -c "$d/prog.c" -o "$d/prog.o" > "$d/c.err" 2>&1 ||
            fail "M3 $O: $(head -2 "$d/c.err")"
        EMBCC_THUMB_HARNESS=$d sh tests/harness/thumb/link.sh "$d/prog.elf" \
            "$d/prog.o" > "$d/link.log" 2>&1 ||
            fail "the M3 image does not link: $(head -3 "$d/link.log")"
        sh tests/harness/qrun.sh 10 "$QARM" -M lm3s6965evb -cpu cortex-m3 \
            -nographic -kernel "$d/prog.elf" > "$d/console$O.log" 2>&1
        check_out "$d/console$O.log" "the M3 image at $O"
    done
    echo "on the Cortex-M3 the read output keeps its value beside dead ones, at four levels"
else
    echo "SKIP: the M3 run ($QARM not found)"
fi

QRV=${EMBCC_QEMU_RISCV32:-qemu-system-riscv32}
if command -v "$QRV" >/dev/null 2>&1; then
    T=riscv32-unknown-elf
    d=$out/rv32
    mkdir -p "$d"
    prog "$d/prog.c" '"mv %0, %3\n\tmv %1, %4\n\tli %2, 7"' \
        'register unsigned lo __asm__("a2"); register unsigned hi __asm__("a3");'
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c tests/harness/riscv/$f.c -o "$d/$f.o" || fail "$f.c"
    done
    for O in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $O -c "$d/prog.c" -o "$d/prog.o" > "$d/c.err" 2>&1 ||
            fail "RV32 $O: $(head -2 "$d/c.err")"
        EMBCC_RISCV_HARNESS=$d sh tests/harness/riscv/link.sh "$d/prog.elf" \
            "$d/prog.o" > "$d/link.log" 2>&1 ||
            fail "the RV32 image does not link: $(head -3 "$d/link.log")"
        EMBCC_QEMU_RISCV=$QRV sh tests/harness/riscv/run.sh "$d/prog.elf" 32 \
            > "$d/console$O.log" 2>&1
        check_out "$d/console$O.log" "the RV32 image at $O"
    done
    echo "on RV32 too"
else
    echo "SKIP: the RV32 run ($QRV not found)"
fi
echo "ok asm-dead-outputs"
