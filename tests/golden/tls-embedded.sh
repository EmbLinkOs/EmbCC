#!/bin/sh
# _Thread_local on the embedded targets (#184): EmbCC compiled a
# thread-local there as one object every thread shared, without a word.
#  1. ARM (Cortex-M at both levels, ARM state) and RISC-V make it
#     local-exec, as GCC does: __aeabi_read_tp() plus a literal word with
#     R_ARM_TLS_LE32; lui/add tp/addi with R_RISCV_TPREL_HI20, _ADD and
#     _LO12_I. (tests/exec/tls-threads.c runs two thread blocks on every
#     board and checks each keeps its own values.)
#  2. A target with no thread pointer EmbCC uses refuses a use by name.
#  3. embld gives the RTOS the template, under picolibc's names:
#     __tdata_start/_end/_size, __tbss_start/_end/_size, __tls_size,
#     __tls_align -- here an 8-byte-aligned block of 4 + 8 + 4 bytes.
set -u
echo "TEST-MARKER tls-embedded"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/tls-embedded
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
printf '_Thread_local int t = 3;\nint f(void) { return t; }\n' > "$out/t.c"

relocs() {   # relocs TRIPLE NAME... -- the object relocates with each
    t=$1; shift
    "$EMBCC" --target=$t -O2 -c "$out/t.c" -o "$out/t-$t.o" || fail "$t does not compile"
    llvm-readelf -rW "$out/t-$t.o" > "$out/t-$t.r"
    for r in "$@"; do
        grep -q "$r" "$out/t-$t.r" || { cat "$out/t-$t.r"; fail "$t: no $r"; }
    done
}
relocs thumbv7em-none-eabi R_ARM_TLS_LE32 __aeabi_read_tp
relocs thumbv6m-none-eabi R_ARM_TLS_LE32 __aeabi_read_tp
relocs armv7a-none-eabi R_ARM_TLS_LE32 __aeabi_read_tp
relocs riscv32-unknown-elf R_RISCV_TPREL_HI20 R_RISCV_TPREL_ADD R_RISCV_TPREL_LO12_I
relocs riscv64-unknown-elf R_RISCV_TPREL_HI20 R_RISCV_TPREL_ADD R_RISCV_TPREL_LO12_I
echo "ARM and RISC-V address a thread-local local-exec, as GCC does"

for t in mipsel-none-elf powerpc-none-eabi sparc-none-elf xtensa-none-elf \
         tricore-none-elf rx-none-elf m68k-none-elf avr; do
    if "$EMBCC" --target=$t -O2 -c "$out/t.c" -o "$out/r.o" 2> "$out/r.err"; then
        fail "$t: a thread-local was compiled, as one shared object"
    fi
    grep -q "'t' is thread-local, which .* has no thread pointer" "$out/r.err" ||
        { cat "$out/r.err"; fail "$t: refused, but not by name"; }
done
echo "eight targets with no thread pointer refuse a thread-local by name"

cat > "$out/s.c" << 'EOF2'
_Thread_local long long a = 1;
_Thread_local int b = 2;
_Thread_local int c;
extern char __tdata_start[], __tdata_end[], __tdata_size[], __tbss_start[],
            __tbss_end[], __tbss_size[], __tls_size[], __tls_align[];
void *const syms[8] = { __tdata_start, __tdata_end, __tdata_size,
    __tbss_start, __tbss_end, __tbss_size, __tls_size, __tls_align };
int *ab(void) { return &b; }
void *__aeabi_read_tp(void) { return 0; }
void reset(void) { }
EOF2
"$EMBCC" --target=thumbv7em-none-eabi -O2 -c "$out/s.c" -o "$out/s.o" || fail "s.c"
"$EMBLD" -e reset -Ttext 0x0 -Tdata 0x20000000 "$out/s.o" -o "$out/s.elf" ||
    fail "the template's symbols do not link"
llvm-nm "$out/s.elf" > "$out/s.nm"
v() { sed -n "s/^\([0-9a-f]*\) . $1\$/\1/p" "$out/s.nm"; }
[ "$(v __tdata_size)" = 0000000c ] || fail "__tdata_size is $(v __tdata_size), not 12"
[ "$(v __tbss_size)" = 00000004 ] || fail "__tbss_size is $(v __tbss_size), not 4"
[ "$(v __tls_size)" = 00000010 ] || fail "__tls_size is $(v __tls_size), not 16"
[ "$(v __tls_align)" = 00000008 ] || fail "__tls_align is $(v __tls_align), not 8"
[ $((0x$(v __tdata_end) - 0x$(v __tdata_start))) = 12 ] || fail "__tdata_start/_end"
[ $((0x$(v __tbss_end) - 0x$(v __tbss_start))) = 4 ] || fail "__tbss_start/_end"
echo "embld gives the thread block's template under picolibc's names"
