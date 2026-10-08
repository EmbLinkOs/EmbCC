#!/bin/sh
# The hardware-float ABIs at the LINK: the driver picks the runtime built
# for the ABI it was given, and embld refuses to put two float ABIs in one
# image.
#
# An ilp32f object and an ilp32 one disagree about where every float
# argument is -- fa0 or a0 -- so an image holding both reads registers the
# caller never wrote. The float ABI is in each object's e_flags; embld
# reads it, and the driver looks for a hard-float ABI's librt.a and libc.a
# under <triple>/<abi> (make rt-embedded builds them there). Linked from
# the soft-float directory, the program below would be refused.
set -u
echo "TEST-MARKER riscv-hf-link"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/riscv-hf-link
rm -rf "$out"; mkdir -p "$out"
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
command -v "$RE" >/dev/null 2>&1 || { echo "SKIP: no llvm-readelf"; exit 0; }

# A double multiply under ilp32f is __muldf3 from that ABI's runtime.
cat > "$out/p.c" <<'EOF'
double g;
int main(void) { volatile double a = 1.5; g = a * 3.0 + 1; return (int)g; }
void _start(void) { main(); for (;;) ; }
EOF
for cfg in "32 rv32imafc ilp32f single" "32 rv32imafdc ilp32d double" \
           "64 rv64imafc lp64f single" "64 rv64gc lp64d double"; do
    set -- $cfg
    T=riscv$1-unknown-elf
    [ -f "build/libc/$T/$3/librt.a" ] || {
        echo "SKIP $3: build/libc/$T/$3/librt.a is not built (make rt-embedded)"
        continue; }
    "$EMBCC" --target=$T -march=$2 -mabi=$3 -O2 "$out/p.c" \
        -Wl,-Ttext=0x80000000 -Wl,-e,_start -o "$out/p-$3.elf" \
        > "$out/p-$3.err" 2>&1 || {
        echo "$3: the driver could not link a program for it:"
        head -3 "$out/p-$3.err"; exit 1; }
    "$RE" -h "$out/p-$3.elf" | grep -q "$4-float ABI" || {
        echo "$3: the image does not say the $4-float ABI:"
        "$RE" -h "$out/p-$3.elf" | grep Flags; exit 1; }
done
echo "the driver links each hard-float ABI against its own runtime"

# Two float ABIs in one link, refused by name.
"$EMBCC" --target=riscv32-unknown-elf -O2 -c "$out/p.c" -o "$out/soft.o" &&
"$EMBCC" --target=riscv32-unknown-elf -march=rv32imafc -mabi=ilp32f -O2 \
    -c tests/harness/riscv/io.c -o "$out/hard.o" || {
    echo "the objects do not compile"; exit 1; }
if "${EMBLD:-./embld}" -e _start -Ttext 0x80000000 "$out/soft.o" "$out/hard.o" \
       -o "$out/mix.elf" > "$out/mix.err" 2>&1; then
    echo "embld linked a soft-float object with an ilp32f one"; exit 1
fi
grep -q "single-float (ilp32f/lp64f) object, and the ones before it are soft-float" \
    "$out/mix.err" || {
    echo "embld refused the mix without saying why:"; head -2 "$out/mix.err"
    exit 1; }
echo "embld refuses an image of two float ABIs, by name"
