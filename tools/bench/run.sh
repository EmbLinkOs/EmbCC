#!/bin/sh
# tools/bench/run.sh [-O2|-Os] [m4|rv32 ...] -- EmbCC against clang on the
# boards, by guest instructions executed (tools/bench/icount.c).
#
# Every kernel of tools/bench/workload.c is built twice per compiler, with
# N=2 and N=6 units of work, and run on QEMU (mps2-an386 for the
# Cortex-M4, virt for RV32); the difference of the two counts is four
# units of the kernel alone -- startup, the .bss a compiler keeps or
# drops, and a kernel's one-time setup cancel. Both compilers' objects link against the same lib/rt and
# lib/libc, built by EmbCC, and the same driver, which prints the
# kernel's checksum: the two must agree, or the run says so.
#
# Not part of `make test`: a measurement, not a pass or fail. Needs
# qemu-system-arm/-riscv32, clang, and QEMU's qemu-plugin.h to build the
# counter (QEMU_PLUGIN_INC, default /opt/homebrew/include).
set -u
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd)
cd "$root"
opt=-O2; boards=
for a in "$@"; do case $a in -O*) opt=$a ;; *) boards="$boards $a" ;; esac; done
[ -n "$boards" ] || boards="m4 rv32"
out=${BENCH_OUT:-$root/tests/golden/out/bench}; mkdir -p "$out"
EMBCC=${EMBCC:-$root/embcc}; EMBLD=${EMBLD:-$root/embld}; export EMBCC EMBLD
inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
plug=$out/icount.so
cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
   -undefined dynamic_lookup -o "$plug" "$here/icount.c" 2>/dev/null ||
cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
   -o "$plug" "$here/icount.c" || { echo "cannot build the counting plugin"; exit 1; }
names="crc sort matrix list interp hash text state fixed"

for b in $boards; do
    case $b in
        m4) T=thumbv7em-none-eabi; X="-mcpu=cortex-m4 -mfloat-abi=soft"
            Q="qemu-system-arm -M mps2-an386 -cpu cortex-m4 -semihosting" ;;
        rv32) T=riscv32-unknown-elf; X="-march=rv32imac -mabi=ilp32 -mno-relax"
            Q="qemu-system-riscv32 -M virt -bios none -m 8" ;;
        *) echo "unknown board $b"; continue ;;
    esac
    L=$out/$b; mkdir -p "$L"
    { sh tools/build-rt.sh $T "$L" && sh tools/build-libc.sh $T "$L"; } > "$L/build.log" 2>&1 ||
        { echo "$b: the runtime does not build"; continue; }
    case $b in
        m4) for f in boot io; do "$EMBCC" --target=$T -DSRAM_TOP=0x20400000u \
                -c tests/harness/thumb-m4f/$f.c -o "$L/$f.o"; done
            cat > "$L/drv.c" <<'EOT'
int prog_main(void); void putn(long v); void puts_(const char *s);
extern volatile unsigned bench_result;
static volatile unsigned blk[2];
int main(void)
{
    prog_main();
    putn((long)bench_result); puts_("\n");
    blk[0] = 0x20026u; blk[1] = 0;
    __asm__ volatile("mov r1, %0\n\tmovs r0, #0x20\n\tbkpt #0xab"
                     : : "r"(blk) : "r0", "r1", "memory");
    for (;;) ;
}
EOT
            ;;
        rv32) for f in boot io; do "$EMBCC" --target=$T \
                -c tests/harness/riscv/$f.c -o "$L/$f.o"; done
            cat > "$L/drv.c" <<'EOT'
int prog_main(void); void putn(long v); void puts_(const char *s);
extern volatile unsigned bench_result;
int main(void)
{
    prog_main();
    putn((long)bench_result); puts_("\n");
    *(volatile unsigned *)0x100000u = 0x5555u;
    for (;;) ;
}
EOT
            ;;
    esac
    "$EMBCC" --target=$T -O1 -c "$L/drv.c" -o "$L/drv.o"
    echo "== $b $opt: guest instructions, EmbCC / clang"
    k=1
    for name in $names; do
        for c in embcc clang; do
          for n in 2 6; do
            o=$L/$name-$c-$n
            if [ $c = embcc ]; then
                "$EMBCC" --target=$T $opt -DKERNEL=$k -DN=$n -Dmain=prog_main \
                    -c tools/bench/workload.c -o $o.o
            else
                clang --target=$T $X $opt -ffreestanding -w -DKERNEL=$k -DN=$n \
                    -Dmain=prog_main -c tools/bench/workload.c -o $o.o
            fi || { echo "$name: $c does not compile"; continue 3; }
            case $b in
                m4) EMBCC_THUMB_HARNESS=$L sh tests/harness/thumb-m4f/link.sh \
                        $o.elf $o.o "$L/drv.o" "$L/libc.a" "$L/librt.a" ;;
                rv32) EMBCC_RISCV_HARNESS=$L sh tests/harness/riscv/link.sh \
                        $o.elf $o.o "$L/drv.o" "$L/libc.a" "$L/librt.a" ;;
            esac > $o.lerr 2>&1 || { echo "$name: $c does not link: $(head -1 $o.lerr)"; continue 3; }
            sh tests/harness/qrun.sh 60 $Q -nographic -plugin "$plug,out=$o.n" \
                -kernel $o.elf > $o.out 2>/dev/null
          done
        done
        ok=1
        for n in 2 6; do
            ve=$(grep -m1 -o '^-\{0,1\}[0-9][0-9]*' $L/$name-embcc-$n.out)
            vc=$(grep -m1 -o '^-\{0,1\}[0-9][0-9]*' $L/$name-clang-$n.out)
            [ -n "$ve" ] && [ "$ve" = "$vc" ] || { echo "  $name: CHECKSUMS DIFFER at N=$n (embcc ${ve:-none}, clang ${vc:-none})"; ok=0; }
        done
        k=$((k + 1))
        [ $ok = 1 ] || continue
        de=$(( $(cat $L/$name-embcc-6.n) - $(cat $L/$name-embcc-2.n) ))
        dc=$(( $(cat $L/$name-clang-6.n) - $(cat $L/$name-clang-2.n) ))
        r=$(echo "$de $dc" | awk '{ printf "%.3f", $1 / $2 }')
        printf '  %-8s %10d %10d  %s\n' $name $de $dc $r
        echo "$r" >> $L/ratios.$$
    done
    [ -f $L/ratios.$$ ] && awk '{ s += log($1); n++ } END { if (n) printf "  geomean  %.3f over %d kernels\n", exp(s / n), n }' $L/ratios.$$
    rm -f $L/ratios.$$
done
