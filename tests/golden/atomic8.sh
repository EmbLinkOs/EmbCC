#!/bin/sh
# Eight-byte atomics on the 32-bit targets: calls to libatomic's
# __atomic_*_8 (src/ir/irgen.c, target_atomic8_libcall), which
# lib/rt/atomic8.c defines, weak, by masking interrupts.
#
#   THE CALLS are clang's: for every builtin form, every target clang
#   knows here calls the same function from the same EmbCC function
#   (calls.c), read off llvm-objdump's relocations. At RV32 the memory
#   order arguments are read too: the immediate each order register is
#   loaded with before the call, against clang's.
#
#   THE RUNTIME'S MACHINE CODE, where EmbCC has no inline assembler
#   (lib/rt/atomic8.h): tools/rtblobs encodes the same instructions with
#   the ColdFire, Xtensa and RX backends' own encoders; llvm-mc the SPARC
#   and PowerPC ones.
#
#   THE VALUES, on every board: tests/golden/atomic8/prog.c -- the
#   __atomic and __sync builtins, the generic forms on a double and a
#   struct, the operators on an _Atomic long long, <stdatomic.h> -- linked
#   with the board's librt.a, at four levels, against the host.
#
#   OVERRIDES: order.c defines its own __atomic_*_8, which replace the weak
#   ones, and records the order each call passes.
#
#   AN INTERRUPT RACE on the Cortex-M3 (SysTick) and RV32 (the machine
#   timer): atomic8-race.sh.
set -u
echo "TEST-MARKER atomic8"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
export EMBLD
D=tests/golden/atomic8
out=tests/golden/out/atomic8
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# ---- the runtime's machine code ----------------------------------------
cc -std=c99 -Wall -Wextra -o "$out/rtblobs" \
   tools/rtblobs/rtblobs.c src/arch/coldfire/emit.c src/arch/xtensa/emit.c \
   src/arch/rx/emit.c src/arch/code.c src/driver/util.c src/driver/diag.c \
   src/arch/target.c src/sema/type.c src/sema/ldfloat.c \
   src/platform/platform_common.c src/platform/platform_posix.c ||
    fail "tools/rtblobs does not build"
"$out/rtblobs" > "$out/rtblobs.txt" || {
    grep DIFFERENT "$out/rtblobs.txt"
    fail "lib/rt/atomic8.h's machine code is not what the backends encode"; }
if command -v llvm-mc >/dev/null 2>&1; then
    # words from the header, the routine as llvm-mc encodes it
    words() {
        sed -n "/#define $1 /,/[^\\\\]\$/p" lib/rt/atomic8.h |
            grep -oE '0x[0-9a-f]+u' | sed 's/u$//' | tr '\n' ' '
    }
    enc() {
        printf '%b' "$2" > "$out/r.s"
        llvm-mc -triple=$1 -show-encoding "$out/r.s" |
            sed -n 's/.*encoding: \[\(.*\)\]/\1/p' |
            awk -F, '{ printf "0x"; for (i = 1; i <= NF; i++) { b = $i
                sub(/^0x/, "", b); printf "%s", b } printf " " }'
    }
    chk() {
        [ "$(words $1)" = "$(enc $2 "$3")" ] ||
            fail "$1 in lib/rt/atomic8.h: $(words $1) is not $(enc $2 "$3")"
    }
    chk RT_SPARC_OFF sparc 'rd %psr, %o0\nor %o0, 0xf00, %o1\nwr %o1, 0, %psr\nnop\nnop\nnop\nretl\nand %o0, 0xf00, %o0\n'
    chk RT_SPARC_ON sparc 'rd %psr, %o1\nandn %o1, 0xf00, %o1\nor %o1, %o0, %o1\nwr %o1, 0, %psr\nnop\nnop\nretl\nnop\n'
    chk RT_PPC_OFF ppc32 'mfmsr 3\nrlwinm 4, 3, 0, 17, 15\nmtmsr 4\nandi. 3, 3, 0x8000\nblr\n'
    chk RT_PPC_ON ppc32 'mfmsr 4\nrlwinm 4, 4, 0, 17, 15\nor 4, 4, 3\nmtmsr 4\nblr\n'
fi
echo "  the runtime's interrupt-mask routines are what the ColdFire, Xtensa and RX encoders and llvm-mc (SPARC, PowerPC) make"

# ---- the calls, against clang's ---------------------------------------
if command -v clang >/dev/null 2>&1 && command -v llvm-objdump >/dev/null 2>&1; then
    calls() {   # OBJ -> "function callee" per call to an atomic routine
        llvm-objdump -dr "$1" | awk '
            /^[0-9a-f]+ <[^>]+>:$/ { f = $2; gsub(/[<>:]/, "", f) }
            /R_[A-Z0-9_]+[ \t]+__(atomic|sync)_/ { print f, $NF }'
    }
    for pair in "thumbv7m-none-eabi thumbv7m-none-eabi" \
                "riscv32-unknown-elf riscv32" "mipsel-none-elf mipsel" \
                "mips-none-elf mips" "sparc-none-elf sparc" \
                "powerpc-none-eabi powerpc" "armv7a-none-eabi -"; do
        t=${pair% *}; ct=${pair#* }
        "$EMBCC" --target=$t -O2 -c $D/calls.c -o "$out/calls.o" ||
            fail "$t: calls.c does not compile"
        calls "$out/calls.o" > "$out/calls-$t.txt"
        n=$(wc -l < "$out/calls-$t.txt" | tr -d ' ')
        [ "$n" -ge 20 ] || fail "$t: only $n atomic calls in calls.c"
        [ "$ct" = - ] && continue      # clang inlines ldrexd on ARMv7-A
        march=; case $ct in riscv32) march=-march=rv32imac ;; esac
        clang --target=$ct $march -O2 -w -c $D/calls.c -o "$out/calls-clang.o" ||
            fail "clang does not compile calls.c for $ct"
        calls "$out/calls-clang.o" > "$out/calls-clang-$t.txt"
        cmp -s "$out/calls-clang-$t.txt" "$out/calls-$t.txt" || {
            diff "$out/calls-clang-$t.txt" "$out/calls-$t.txt" | head -8
            fail "$t: the calls differ from clang's (< clang, > embcc)"; }
    done
    echo "  every builtin form calls clang's __atomic_*_8, from the same function, on six targets"
    # RV32: the order arguments' immediates
    orders() {
        llvm-objdump -dr --no-show-raw-insn "$1" | awk '
            /^[0-9a-f]+ <[^>]+>:$/ { f = $2; gsub(/[<>:]/, "", f); delete li; next }
            $2 == "li" { r = $3; sub(/,$/, "", r); li[r] = $4; next }
            $2 == "mv" { r = $3; sub(/,$/, "", r); li[r] = li[$4]; next }
            /R_RISCV_CALL/ {
                n = $NF; s = n ~ /load_8/ ? "a1" : n ~ /compare_exchange_8/ ? "a4 a5" : "a3"
                k = split(s, rs, " "); o = ""
                for (i = 1; i <= k; i++) o = o " " (rs[i] in li ? li[rs[i]] : "?")
                print f, n, o }'
    }
    "$EMBCC" --target=riscv32-unknown-elf -O2 -c $D/calls.c -o "$out/calls.o"
    clang --target=riscv32 -march=rv32imac -O2 -w -c $D/calls.c -o "$out/calls-clang.o"
    orders "$out/calls.o" > "$out/orders.txt"
    orders "$out/calls-clang.o" > "$out/orders-clang.txt"
    grep -q '?' "$out/orders-clang.txt" && fail "clang's order arguments were not read"
    cmp -s "$out/orders-clang.txt" "$out/orders.txt" || {
        diff "$out/orders-clang.txt" "$out/orders.txt" | head -8
        fail "RV32: the memory-order arguments differ from clang's"; }
    echo "  and at RV32 every call passes clang's memory-order arguments"
else
    echo "  (SKIP: no clang or llvm-objdump for the calls)"
fi

# ---- the values, on every board ----------------------------------------
HOSTCC=${HOSTCC:-cc}
"$HOSTCC" -std=c11 -w -O2 -o "$out/host" $D/prog.c tests/harness/thumb/hostio.c ||
    fail "the host does not build prog.c"
"$out/host" > "$out/want.txt"
# board triple harness qemu
boards="m3 thumbv7m-none-eabi thumb qemu-system-arm
m0 thumbv6m-none-eabi thumb-m0 qemu-system-arm
m23 thumbv8m.base-none-eabi thumb-m23 qemu-system-arm
a7 armv7a-none-eabi arm-a32 qemu-system-arm
rv32 riscv32-unknown-elf riscv qemu-system-riscv32
mipsel mipsel-none-elf mips qemu-system-mipsel
mips mips-none-elf mips qemu-system-mips
sparc sparc-none-elf sparc qemu-system-sparc
ppc powerpc-none-eabi ppc qemu-system-ppc
coldfire m68k-none-elf coldfire qemu-system-m68k
tricore tricore-none-elf tricore qemu-system-tricore
xtensa xtensa-none-elf xtensa qemu-system-xtensa
rx rx-none-elf rx qemu-system-rx"
# TriCore's harness prints through a QEMU plugin (tests/harness/tricore)
inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
   -undefined dynamic_lookup -o "$out/putc.so" \
   tests/harness/tricore/putc.c 2>/dev/null ||
cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
   -o "$out/putc.so" tests/harness/tricore/putc.c 2>/dev/null ||
    echo "  (the TriCore output plugin does not build)"
EMBCC_TRICORE_PLUGIN=$PWD/$out/putc.so
export EMBCC_TRICORE_PLUGIN
echo "$boards" | while read -r b t h q; do
    [ -n "${EMBCC_BOARDS:-}" ] && case " $EMBCC_BOARDS " in *" $b "*) ;; *) continue ;; esac
    command -v "$q" >/dev/null 2>&1 || { echo "  (SKIP $b: no $q)"; continue; }
    lib=build/libc/$t/librt.a
    [ -f "$lib" ] || fail "$b: no $lib (make rt-embedded)"
    o=$out/$b; mkdir -p "$o"
    for f in boot io; do
        "$EMBCC" --target=$t -O1 -c tests/harness/$h/$f.c -o "$o/$f.o" ||
            fail "$b: the harness's $f.c does not compile"
    done
    for O in -O0 -O1 -O2 -Os; do
        for p in prog order; do
            "$EMBCC" --target=$t $O -c $D/$p.c -o "$o/$p.o" 2> "$o/err" || {
                head -3 "$o/err"; fail "$b $O: $p.c does not compile"; }
            EMBCC_THUMB_HARNESS=$o EMBCC_THUMB_M0_HARNESS=$o EMBCC_M23_HARNESS=$o \
            EMBCC_A32_HARNESS=$o EMBCC_RISCV_HARNESS=$o EMBCC_MIPS_HARNESS=$o \
            EMBCC_SPARC_HARNESS=$o EMBCC_PPC_HARNESS=$o EMBCC_CF_HARNESS=$o \
            EMBCC_TRICORE_HARNESS=$o EMBCC_XTENSA_HARNESS=$o EMBCC_RX_HARNESS=$o \
                sh tests/harness/$h/link.sh "$o/$p.elf" "$o/$p.o" "$lib" \
                2> "$o/err" || { head -3 "$o/err"; fail "$b $O: $p.c does not link"; }
            EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20} \
                sh tests/harness/$h/run.sh "$o/$p.$( [ $b = rx ] && echo bin || echo elf)" \
                $( [ $b = rv32 ] && echo 32 ) 2>/dev/null | tr -d '\r' |
                sed -n '1,/^DONE/p' > "$o/got-$p.txt"
        done
        cmp -s "$out/want.txt" "$o/got-prog.txt" || {
            diff "$out/want.txt" "$o/got-prog.txt" | head -6
            fail "$b $O: the eight-byte atomics differ from the host"; }
        cmp -s $D/order.want "$o/got-order.txt" || {
            diff $D/order.want "$o/got-order.txt" | head -6
            fail "$b $O: a call passed the wrong memory order, or the weak runtime was not replaced"; }
    done
    echo "  $b ($t): every eight-byte atomic as the host computes it, and the orders passed, at -O0, -O1, -O2 and -Os"
done || exit 1
