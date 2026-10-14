#!/bin/sh
# Handle-like structs -- `typedef struct { uint32_t raw; } handle_t;`, the
# typed handle of every RTOS API -- returned and passed between EmbCC's
# objects and clang's. EmbCC returns one as the integer it holds where the
# ABI says that is the same bits (ty_scalar_struct_ret), so the struct can
# leave memory; this is the proof that it is the same bits: half the
# program from each compiler, all four pairings, at -O0 and -O2, on a
# Cortex-M3, RV32 and RV64 -- and on x86-64 and (where this runs on one)
# AArch64 macOS, which keep returning the struct as a struct. The
# 8- and 16-bit ones and the two-field ones stay structs and are here too,
# with values whose upper bits would show an extension gone wrong.
set -u
echo "TEST-MARKER struct-handle-abi"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/struct-handle-abi
rm -rf "$out"; mkdir -p "$out"
command -v clang > /dev/null 2>&1 || { echo "SKIP: needs clang"; exit 0; }

cat > "$out/abi.h" << 'E'
typedef struct { unsigned int raw; } h32;
typedef struct { void *p; } hp;
typedef struct { struct { int v; } in; } hn;
typedef struct { unsigned int a[1]; } ha;
typedef struct { unsigned long long raw; } h64;
typedef struct { unsigned short raw; } h16;
typedef struct { unsigned char raw; } h8;
typedef struct { unsigned short lo, hi; } hpair;
h32 mk32(unsigned v);
unsigned get32(h32 h);
h32 add32(h32 a, h32 b);
h32 sel32(int c, h32 a, h32 b);
h32 cur32(void);
void set32(h32 h);
hp mkp(void *p);
hn mkn(int v);
int getn(hn h);
ha mka(unsigned v);
h64 mk64(unsigned long long v);
unsigned long long get64(h64 h);
h16 mk16(unsigned short v);
h8 mk8(unsigned char v);
hpair mkpair(unsigned short lo, unsigned short hi);
extern h32 (*fp32)(unsigned);
int run(void);
E

cat > "$out/callee.c" << 'E'
#include "abi.h"
static h32 g32;
h32 mk32(unsigned v) { h32 h; h.raw = v; return h; }
unsigned get32(h32 h) { return h.raw; }
h32 add32(h32 a, h32 b) { h32 r = a; r.raw += b.raw; return r; }
h32 sel32(int c, h32 a, h32 b) { return c ? a : b; }
h32 cur32(void) { return g32; }
void set32(h32 h) { g32 = h; }
hp mkp(void *p) { hp h = { p }; return h; }
hn mkn(int v) { hn h; h.in.v = -v; return h; }
int getn(hn h) { return h.in.v; }
ha mka(unsigned v) { ha h = { { v * 3u } }; return h; }
h64 mk64(unsigned long long v) { h64 h = { v ^ 0x8000000000000001ull }; return h; }
unsigned long long get64(h64 h) { return h.raw; }
h16 mk16(unsigned short v) { h16 h = { (unsigned short)(v ^ 0x8001u) }; return h; }
h8 mk8(unsigned char v) { h8 h = { (unsigned char)(v ^ 0x81u) }; return h; }
hpair mkpair(unsigned short lo, unsigned short hi) { hpair p = { lo, hi }; return p; }
h32 (*fp32)(unsigned) = mk32;
E

cat > "$out/caller.c" << 'E'
#include "abi.h"
static int n;
static volatile unsigned vk = 0x80000005u;
#define CHECK(e) do { n++; if (!(e)) return n; } while (0)
int run(void)
{
    int obj;
    h32 a = mk32(vk), b = mk32(7), c;
    CHECK(a.raw == 0x80000005u);
    CHECK(get32(a) == 0x80000005u);
    c = add32(a, b);
    CHECK(c.raw == 0x8000000cu);
    c = sel32((int)(vk & 1), b, a);
    CHECK(c.raw == 7);
    set32(mk32(vk + 1));
    CHECK(cur32().raw == 0x80000006u);
    for (int i = 0; i < 3; i++)          /* a handle carried round a loop */
        a = add32(a, b);
    CHECK(a.raw == 0x8000001au);
    CHECK(fp32(vk).raw == 0x80000005u);
    CHECK(mkp(&obj).p == &obj);
    CHECK(mkn(5).in.v == -5 && getn(mkn(-9)) == 9);
    CHECK(mka(vk).a[0] == 0x8000000fu);
    CHECK(mk64(0x7000000000000002ull).raw == 0xf000000000000003ull);
    CHECK(get64(mk64(vk)) == (0x8000000000000001ull ^ 0x80000005u));
    CHECK(mk16(0x7ffe).raw == 0xffffu);
    CHECK(mk8(0x7e).raw == 0xffu);
    hpair p = mkpair(0xfffe, 0x8001);
    CHECK(p.lo == 0xfffe && p.hi == 0x8001);
    return 0;
}
E

cat > "$out/board.c" << 'E'
void puts_(const char *s);
void putn(long v);
int run(void);
int main(void) { putn(run()); puts_("\n==END==\n"); return 0; }
E
cat > "$out/host.c" << 'E'
int run(void);
int main(void) { int r = run(); return r ? r : 42; }
E

fail=0; runs=0
cc_obj() {    # WHO TARGET OPT SRC OBJ CLANGFLAGS...
    who=$1 t=$2 o=$3 src=$4 obj=$5; shift 5
    if [ "$who" = e ]; then
        "$EMBCC" --target=$t $o -c "$src" -o "$obj"
    else
        clang --target=$t "$@" $o -ffreestanding -c "$src" -o "$obj"
    fi
}
board() {    # TRIPLE HARNESS CLANGFLAGS QEMU...
    t=$1 H=$2 cf=$3; shift 3
    d=$out/$t; mkdir -p "$d"
    for f in boot io; do "$EMBCC" --target=$t -c "$H/$f.c" -o "$d/$f.o" || return 1; done
    "$EMBCC" --target=$t -c "$out/board.c" -o "$d/board.o" || return 1
    for pr in e:e e:c c:e c:c; do
        for o in -O0 -O2; do
            a=${pr%:*} b=${pr#*:} tag=$a$b$o
            # shellcheck disable=SC2086
            cc_obj $a $t $o "$out/caller.c" "$d/caller.$tag.o" $cf &&
            # shellcheck disable=SC2086
            cc_obj $b $t $o "$out/callee.c" "$d/callee.$tag.o" $cf || {
                echo "$t $tag: does not compile"; fail=1; continue; }
            env EMBCC_THUMB_HARNESS="$d" EMBCC_RISCV_HARNESS="$d" sh "$H/link.sh" \
                "$d/$tag.elf" "$d/caller.$tag.o" "$d/callee.$tag.o" "$d/board.o" \
                > "$d/$tag.ld" 2>&1 || { echo "$t $tag: does not link"; head -3 "$d/$tag.ld"; fail=1; continue; }
            sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" --until '==END==' "$@" \
                -nographic -kernel "$d/$tag.elf" > "$d/$tag.txt" 2> /dev/null
            r=$(sed -n '1p' "$d/$tag.txt" | tr -d '\r ')
            [ "$r" = 0 ] || { echo "$t caller $a callee $b $o: check ${r:-(none)} failed"; fail=1; }
            runs=$((runs + 1))
        done
    done
}
if command -v qemu-system-arm > /dev/null 2>&1; then
    board thumbv7m-none-eabi tests/harness/thumb \
        "-mcpu=cortex-m3 -mfloat-abi=soft" \
        qemu-system-arm -M lm3s6965evb -cpu cortex-m3 || fail=1
fi
if command -v qemu-system-riscv32 > /dev/null 2>&1; then
    board riscv32-unknown-elf tests/harness/riscv \
        "-march=rv32imac -mabi=ilp32 -mno-relax" \
        qemu-system-riscv32 -M virt -bios none -m 8 || fail=1
fi
if command -v qemu-system-riscv64 > /dev/null 2>&1; then
    board riscv64-unknown-elf tests/harness/riscv \
        "-march=rv64imac -mabi=lp64 -mno-relax -mcmodel=medany" \
        qemu-system-riscv64 -M virt -bios none -m 8 || fail=1
fi
# x86-64: the harness's own runner, which answers with the exit status
d=$out/x86_64; mkdir -p "$d"
"$EMBCC" --target=x86_64-elf -c "$out/host.c" -o "$d/host.o" || fail=1
for pr in e:e e:c c:e c:c; do
    for o in -O0 -O2; do
        a=${pr%:*} b=${pr#*:} tag=$a$b$o
        cc_obj $a x86_64-elf $o "$out/caller.c" "$d/caller.$tag.o" -mno-red-zone &&
        cc_obj $b x86_64-elf $o "$out/callee.c" "$d/callee.$tag.o" -mno-red-zone || {
            echo "x86_64 $tag: does not compile"; fail=1; continue; }
        tests/harness/x86_64/link.sh -o "$d/$tag" "$d/host.o" "$d/caller.$tag.o" \
            "$d/callee.$tag.o" > "$d/$tag.ld" 2>&1 || {
            echo "x86_64 $tag: does not link"; head -3 "$d/$tag.ld"; fail=1; continue; }
        tests/harness/x86_64/run.sh "$d/$tag" > /dev/null 2>&1
        r=$?
        [ $r = 42 ] || { echo "x86_64 caller $a callee $b $o: check $r failed"; fail=1; }
        runs=$((runs + 1))
    done
done
# AArch64, natively, where this is an Apple Silicon Mac
if [ "$(uname -s)/$(uname -m)" = Darwin/arm64 ]; then
    d=$out/a64; mkdir -p "$d"
    for pr in e:e e:c c:e c:c; do
        for o in -O0 -O2; do
            a=${pr%:*} b=${pr#*:} tag=$a$b$o
            for side in caller callee; do
                who=$a; [ $side = callee ] && who=$b
                if [ $who = e ]; then
                    "$EMBCC" --target=aarch64-apple-darwin $o -c "$out/$side.c" -o "$d/$side.$tag.o"
                else
                    clang $o -c "$out/$side.c" -o "$d/$side.$tag.o"
                fi || { echo "a64 $tag: $side does not compile"; fail=1; continue 2; }
            done
            clang -o "$d/$tag" "$out/host.c" "$d/caller.$tag.o" "$d/callee.$tag.o" || {
                echo "a64 $tag: does not link"; fail=1; continue; }
            "$d/$tag"; r=$?
            [ $r = 42 ] || { echo "a64 caller $a callee $b $o: check $r failed"; fail=1; }
            runs=$((runs + 1))
        done
    done
fi
[ $fail = 0 ] || exit 1
[ $runs -ge 8 ] || { echo "FAIL: only $runs runs"; exit 1; }
echo "struct-handle-abi: $runs runs: EmbCC's and clang's objects pass handle structs to each other"
