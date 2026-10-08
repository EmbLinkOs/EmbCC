#!/bin/sh
# C++ on the 32-bit embedded boards: every tests/cxx-embedded program built
# by EmbCC with -fno-exceptions -fno-rtti for a Cortex-M3 (thumbv7m), a
# Cortex-M4F with the hard-float convention (thumbv7em-none-eabihf), a
# Cortex-M0 (thumbv6m), a Cortex-M33 (thumbv8m.main), a Cortex-A15 in ARM
# state (armv7a-none-eabi) and RV32 (riscv32-unknown-elf), at -O0, -O1,
# -O2 and -Os, linked with the embedded
# C++ runtime (tools/build-libcxx.sh), lib/libc and lib/rt, and run under
# QEMU -- and each must print exactly what the same source prints built
# by the host's clang++ and run here.
#
# The programs are the C++ an RTOS's wrappers are written in: classes and
# RAII, virtual functions and abstract interfaces, templates, constexpr,
# namespaces, references, operator overloading, static objects with
# constructors (.init_array) and destructors, function-local statics
# (guards), placement new and new[]/delete[] (cookies), member pointers and
# lambdas -- and, in rtti.cc (built with -frtti), typeid and dynamic_cast.
# They print a line per property and nothing that differs between
# a 32-bit board and a 64-bit host (no sizes, no addresses), and end with
# the sentinel ==END==.
set -u
echo "TEST-MARKER cxx-embedded"
. "$(dirname "$0")/../lib.sh"

HOSTCXX=${EMBCC_HOST_CXX:-clang++}
command -v "$HOSTCXX" >/dev/null 2>&1 || HOSTCXX=c++
command -v "$HOSTCXX" >/dev/null 2>&1 || {
    echo "skipped: no host C++ compiler for the reference (EMBCC_HOST_CXX)"
    exit 0; }
command -v qemu-system-arm >/dev/null 2>&1 &&
    command -v qemu-system-riscv32 >/dev/null 2>&1 || {
    echo "skipped: qemu-system-arm and qemu-system-riscv32 are needed"
    exit 0; }

cd "$EMBCC_ROOT"
out=tests/golden/out/cxx-embedded
rm -rf "$out"; mkdir -p "$out/host"
EMBCC=${EMBCC:-$PWD/embcc}
EMBLD=${EMBLD:-$PWD/embld}
export EMBCC EMBLD

# ---- what is refused, by name ----------------------------------------------
# Exceptions want unwind tables -- EHABI's .ARM.exidx on ARM, .eh_frame on
# RV32 -- and EmbCC writes neither for these machines: a unit with
# exceptions on is refused, and so is an explicit request for the tables.
# Without them a C++ unit writes no .eh_frame at all (it once got
# x86-64's CFI in a section typed SHT_ARM_EXIDX).
printf 'struct A { virtual ~A(); int f(int); };\nA::~A() {}\nint A::f(int x) { return x; }\n' \
    > "$out/refuse.cc"
for t in thumbv7m-none-eabi armv7a-none-eabi riscv32-unknown-elf \
         mipsel-none-elf mips-none-elf mips64-none-elf powerpc-none-eabi \
         sparc-none-elf rx-none-elf m68k-none-elf xtensa-none-elf \
         tricore-none-elf; do
    if "$EMBCC" --target=$t -c "$out/refuse.cc" -o "$out/r.o" \
            2> "$out/r.err"; then
        echo "$t: C++ with exceptions was accepted"; exit 1
    fi
    grep -q "C++ exceptions are not supported for $t" "$out/r.err" || {
        echo "$t: exceptions refused, but not by name:"; cat "$out/r.err"
        exit 1; }
    if "$EMBCC" --target=$t -fno-exceptions -funwind-tables \
            -c "$out/refuse.cc" -o "$out/r.o" 2> "$out/r.err"; then
        echo "$t: -funwind-tables was accepted"; exit 1
    fi
    grep -q "unwind tables are not supported for $t" "$out/r.err" || {
        echo "$t: unwind tables refused, but not by name:"; cat "$out/r.err"
        exit 1; }
    "$EMBCC" --target=$t -fno-exceptions -c "$out/refuse.cc" -o "$out/r.o" || {
        echo "$t: C++ with -fno-exceptions does not compile"; exit 1; }
    if "${READELF:-llvm-readelf}" -S "$out/r.o" | grep -q 'eh_frame'; then
        echo "$t: a C++ unit without exceptions wrote an .eh_frame"; exit 1
    fi
done
# The 64-bit embedded targets compile C++ but write no unwind tables
# either: with -fno-exceptions a unit has none (RV64 once got an x86-64
# CIE in its .eh_frame, and MIPS64 refused every C++ unit for asking for
# tables by default), and an explicit request is refused by name -- as it
# is for C on AVR, which also once got the x86-64 CIE.
for t in riscv64-unknown-elf mips64el-none-elf loongarch64-unknown-elf; do
    "$EMBCC" --target=$t -fno-exceptions -c "$out/refuse.cc" -o "$out/r.o" \
        2> "$out/r.err" || {
        echo "$t: C++ with -fno-exceptions does not compile:"
        cat "$out/r.err"; exit 1; }
    if "${READELF:-llvm-readelf}" -S "$out/r.o" | grep -q 'eh_frame'; then
        echo "$t: a C++ unit without exceptions wrote an .eh_frame"; exit 1
    fi
    if "$EMBCC" --target=$t -fno-exceptions -funwind-tables \
            -c "$out/refuse.cc" -o "$out/r.o" 2> "$out/r.err"; then
        echo "$t: -funwind-tables was accepted"; exit 1
    fi
    grep -q "unwind tables are not supported for $t" "$out/r.err" || {
        echo "$t: unwind tables refused, but not by name:"; cat "$out/r.err"
        exit 1; }
done
printf 'int f(int a) { return a + 1; }\n' > "$out/plain.c"
if "$EMBCC" --target=avr -funwind-tables -c "$out/plain.c" -o "$out/r.o" \
        2> "$out/r.err"; then
    echo "avr: -funwind-tables was accepted"; exit 1
fi
grep -q "unwind tables are not supported for avr" "$out/r.err" || {
    echo "avr: unwind tables refused, but not by name:"; cat "$out/r.err"; exit 1; }
for t in avr; do
    if "$EMBCC" --target=$t -fno-exceptions -c "$out/refuse.cc" \
            -o "$out/r.o" 2> "$out/r.err"; then
        echo "$t: C++ was accepted for a C++ ABI nobody has checked"; exit 1
    fi
    grep -q "C++ is not yet supported for $t" "$out/r.err" || {
        echo "$t: C++ refused, but not by name:"; cat "$out/r.err"; exit 1; }
done
echo "exceptions and unwind tables on ARM, RISC-V, MIPS, PowerPC, SPARC, RX,"
echo "ColdFire, Xtensa, TriCore, LoongArch and AVR, and C++ on AVR, are refused"
echo "by name"

# ---- the reference: the host's own C++ compiler, run here ----------------
progs=
for cc in tests/cxx-embedded/*.cc; do
    n=$(basename "$cc" .cc)
    # (EMBCC_CXX_PROGS: only these programs, for a quick run)
    case " ${EMBCC_CXX_PROGS:-$n} " in *" $n "*) ;; *) continue ;; esac
    progs="$progs $n"
    "$HOSTCXX" -std=c++20 -w -o "$out/host/$n" "$cc" || {
        echo "$n: the host's $HOSTCXX does not build it"; exit 1; }
    "$out/host/$n" > "$out/host/$n.raw" 2>&1
    sed -n '1,/==END==/p' "$out/host/$n.raw" > "$out/host/$n.txt"
    grep -q '==END==' "$out/host/$n.txt" || {
        echo "$n: the host build does not reach ==END=="; exit 1; }
done

# ---- each board: its libraries and harness, then every program ----------
# A board is its triple, the harness directory whose link.sh links it (and
# the variable that says where that harness's boot.o and io.o were built),
# the harness sources, and how it runs. The Cortex-M, ARM and RV32 boards
# boot with QEMU's -kernel and a board.c that sends write() to the UART;
# the others boot as their own exec suites do (tests/golden/*-exec.sh):
# boot.c built with -DHARNESS_LIBC, so main's return goes through exit()
# and stdio is flushed, and io.c's weak write() to the UART.
boards="thumbv7m-none-eabi thumbv7em-none-eabihf thumbv6m-none-eabi
        thumbv8m.main-none-eabi armv7a-none-eabi riscv32-unknown-elf
        mipsel-none-elf mips-none-elf powerpc-none-eabi sparc-none-elf
        rx-none-elf m68k-none-elf xtensa-none-elf tricore-none-elf
        mips64-none-elf"
[ -n "${EMBCC_CXX_BOARDS:-}" ] && boards=$EMBCC_CXX_BOARDS
# the harness: SRC (boot.c and io.c), H (link.sh and run.sh), its variable
harness() {
    case $1 in
        thumbv7m*)  H=tests/harness/thumb; hv=EMBCC_THUMB_HARNESS ;;
        thumbv7em*) H=tests/harness/thumb-m4f; hv=EMBCC_THUMB_HARNESS ;;
        thumbv6m*)  H=tests/harness/thumb-m0; hv=EMBCC_THUMB_M0_HARNESS ;;
        thumbv8m*)  H=tests/harness/thumb-m33; hv=EMBCC_M33_HARNESS ;;
        armv7a*)    H=tests/harness/arm-a32; hv=EMBCC_A32_HARNESS ;;
        riscv32*)   H=tests/harness/riscv; hv=EMBCC_RISCV_HARNESS ;;
        mips64*)    H=tests/harness/mips64; hv=EMBCC_MIPS64_HARNESS ;;
        mips*)      H=tests/harness/mips; hv=EMBCC_MIPS_HARNESS ;;
        powerpc*)   H=tests/harness/ppc; hv=EMBCC_PPC_HARNESS ;;
        sparc*)     H=tests/harness/sparc; hv=EMBCC_SPARC_HARNESS ;;
        rx*)        H=tests/harness/rx; hv=EMBCC_RX_HARNESS ;;
        m68k*)      H=tests/harness/coldfire; hv=EMBCC_CF_HARNESS ;;
        xtensa*)    H=tests/harness/xtensa; hv=EMBCC_XTENSA_HARNESS ;;
        tricore*)   H=tests/harness/tricore; hv=EMBCC_TRICORE_HARNESS ;;
    esac
    SRC=$H
    case $1 in mips64*) SRC=tests/harness/mips ;; esac
}
for t in $boards; do
    d=$out/$t; mkdir -p "$d"
    harness "$t"
    flags=
    case $t in
        thumbv6m*) flags=-DSRAM_TOP=0x20010000u ;;
        thumb*|armv7a*|riscv32*) ;;
        *) flags="-O1 -DHARNESS_LIBC" ;;
    esac
    { sh tools/build-rt.sh "$t" "$d" && sh tools/build-libc.sh "$t" "$d" &&
      sh tools/build-libcxx.sh "$t" "$d"; } > "$d/build.log" 2>&1 || {
        echo "$t: the libraries do not build:"; tail -3 "$d/build.log"
        exit 1; }
    "$EMBCC" --target="$t" $flags -c "$SRC/boot.c" -o "$d/boot.o" || exit 1
    "$EMBCC" --target="$t" -O1 -c "$SRC/io.c" -o "$d/io.o" || exit 1
    # the M0 harness's io.c already sends write() to its UART, and so do
    # the exec suites' harnesses (a weak write)
    board=
    case $t in
        thumbv6m*) ;;
        thumb*|armv7a*|riscv32*)
            "$EMBCC" --target="$t" -c tests/cxx-embedded/board.c \
                -o "$d/board.o" || exit 1
            board=$d/board.o ;;
    esac
    echo "$H $hv $board" > "$d/harness"
done
# TriCore's UART is a QEMU plugin watching its stores
case " $(echo $boards) " in *" tricore-none-elf "*)
    inc=${QEMU_PLUGIN_INC:-/opt/homebrew/include}
    cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
       -undefined dynamic_lookup -o "$out/putc.so" \
       tests/harness/tricore/putc.c 2>/dev/null ||
    cc -shared -fPIC -O2 -I"$inc" $(pkg-config --cflags glib-2.0 2>/dev/null) \
       -o "$out/putc.so" tests/harness/tricore/putc.c || {
        echo "the TriCore harness's output plugin does not build"; exit 1; }
    EMBCC_TRICORE_PLUGIN=$PWD/$out/putc.so
    export EMBCC_TRICORE_PLUGIN ;;
esac

cat > "$out/one.sh" <<'EOF'
# one.sh TRIPLE OPT NAME OUT: build one program for one board, run it,
# compare its output with the host's
t=$1 opt=$2 n=$3 out=$4
d=$out/$t
read H hv board < "$d/harness"
o=$d/$n$opt
rm -f "$o.o" "$o.elf"
# a program's own flags after the suite's (rtti.cc: -frtti)
extra=$(sed -n 's|^// embedded-flags: *||p' "tests/cxx-embedded/$n.cc")
if ! "$EMBCC" --target="$t" $opt -fno-exceptions -fno-rtti $extra \
        -Ilib/libc/include -c "tests/cxx-embedded/$n.cc" -o "$o.o" \
        > "$o.err" 2>&1; then
    echo "FAIL $t $opt $n: does not compile: $(head -1 "$o.err")"; exit 0
fi
if ! env "$hv=$d" sh "$H/link.sh" "$o.elf" "$o.o" $board "$d/libcxx.a" \
        "$d/libc.a" "$d/librt.a" > "$o.err" 2>&1; then
    echo "FAIL $t $opt $n: does not link: $(head -1 "$o.err")"; exit 0
fi
case $t in
    thumbv7m*)  set -- qemu-system-arm -M lm3s6965evb -cpu cortex-m3 ;;
    thumbv7em*) set -- qemu-system-arm -M mps2-an386 -cpu cortex-m4 ;;
    thumbv6m*)  set -- qemu-system-arm -M microbit \
                    -global nrf51-soc.sram-size=65536 ;;
    thumbv8m*)  set -- qemu-system-arm -M mps2-an505 -cpu cortex-m33 ;;
    armv7a*)    set -- qemu-system-arm -M virt -cpu cortex-a15 -m 128 \
                    -monitor none -semihosting ;;
    riscv32*)   set -- qemu-system-riscv32 -M virt -bios none -m 8 ;;
    *)          set -- ;;
esac
if [ $# -gt 0 ]; then
    sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" --until '==END==' \
        "$@" -nographic -kernel "$o.elf" > "$o.raw" 2>/dev/null
else
    # the board's own runner, which stops at the harness's ==EXIT sentinel
    # (after ==END==, once main has returned and exit() has run)
    img=$o.elf
    case $t in rx*) img=$o.bin ;; esac
    EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-30} sh "$H/run.sh" "$img" \
        > "$o.raw" 2>/dev/null
fi
# (a board's UART may end lines with CR LF)
tr -d '\r' < "$o.raw" | sed -n '1,/==END==/p' > "$o.txt"
if diff "$out/host/$n.txt" "$o.txt" > "$o.diff"; then
    echo "PASS $t $opt $n"
else
    echo "FAIL $t $opt $n: output differs from the host's: $(sed -n 2,3p "$o.diff" | tr '\n' ' ')"
fi
EOF

for t in $boards; do
    for opt in -O0 -O1 -O2 -Os; do
        for n in $progs; do
            echo "$t $opt $n $out"
        done
    done
done | xargs -P "${EMBCC_JOBS:-8}" -n 4 sh "$out/one.sh" > "$out/results.txt"

sort "$out/results.txt" | grep '^FAIL' | head -40
pass=$(grep -c '^PASS' "$out/results.txt")
fail=$(grep -c '^FAIL' "$out/results.txt")
nb=$(echo $boards | wc -w)
np=$(echo $progs | wc -w)
echo "$pass passed, $fail failed"
[ "$fail" = 0 ] && [ "$pass" -ge $((nb * np * 4)) ] || exit 1
echo "C++ runs on the Cortex-M, ARM, RV32, MIPS32 (both byte orders), MIPS64"
echo "big-endian, PowerPC, SPARC, RX, ColdFire, Xtensa and TriCore boards as it"
echo "does on the host"
