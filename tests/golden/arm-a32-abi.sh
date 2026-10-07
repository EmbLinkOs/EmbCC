#!/bin/sh
# AAPCS in ARM state, against clang ACROSS THE CALL -- and EmbCC's runtime
# under clang's objects, and the two instruction sets calling each other.
#
# 1. tests/golden/embedded-abi-{caller,callee}.c (shared with the ARMv7-M
#    and RISC-V suites: structs by value and returned, a composite
#    straddling r3 and the stack, eight-byte scalars in even pairs,
#    variadics), and tests/golden/arm-a32-abi-*.c (soft-float doubles and
#    floats, small and large struct returns, _Complex, narrow arguments, a
#    36-byte struct by value, variadic doubles and structs, a far
#    movw/movt addend, a returned function pointer -- see arm-a32-abi.h). Each side is compiled by EmbCC or by clang
#    (--target=armv7a-none-eabi -mfloat-abi=soft), the objects are linked
#    by embld with the harness and lib/rt, and the image runs on QEMU virt.
#    clang calling clang is the reference; EmbCC calling EmbCC, EmbCC
#    calling clang and clang calling EmbCC must print the same, EmbCC's
#    side at -O0 and at -O2. A backend that read the ABI consistently
#    wrong would agree with itself through the whole exec corpus; only a
#    pairing with another compiler shows it.
# 2. tests/golden/embedded-aeabi.c, compiled by clang for armv7a, whose
#    divides, 64-bit shifts, block copies and soft-float arithmetic are
#    calls to the RTABI's __aeabi_* helpers, linked with EmbCC's lib/rt and
#    lib/libc: its output must be the host's. clang's object is checked to
#    call the helpers first, so a pass is not vacuous.
# 3. Interworking: the same pair with clang's side in THUMB state
#    (-mthumb) -- an A-profile core runs both, and a library built for one
#    links with code built for the other. embld turns each call between
#    the two into blx (R_ARM_CALL to a Thumb function, R_ARM_THM_CALL to
#    an ARM one); a return is bx lr or a pop into pc, which interwork by
#    themselves on ARMv7.
#
# 4. AAPCS-VFP: the arm-a32-abi pair again with both sides hard float
#    (armv7a-none-eabihf, -mfpu=vfpv3-d16 -mfloat-abi=hard): floats and
#    doubles in s0-s15/d0-d7, back-filling a single into the gap a double
#    left, homogeneous float aggregates in VFP registers, variadics still
#    in the core registers -- against clang the same way, on its own
#    runtime and harness.
#
# The clang objects carry REL relocations, so this is EmbLD's test for
# foreign A32 objects as well.
set -u
echo "TEST-MARKER arm-a32-abi"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "skipped: $QEMU not found"; exit 0; }
CLANG=${EMBCC_REF_CLANG_ARM:-clang}
command -v "$CLANG" >/dev/null 2>&1 &&
    "$CLANG" --target=armv7a-none-eabi -mfloat-abi=soft \
        -fsyntax-only -x c /dev/null 2>/dev/null || {
    echo "skipped: no clang with an ARM target (set EMBCC_REF_CLANG_ARM)"
    exit 0; }
NM=${EMBCC_LLVM_NM:-llvm-nm}

T=armv7a-none-eabi
CLF=-mfloat-abi=soft
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
top=tests/golden/out/arm-a32-abi
rm -rf "$top"; mkdir -p "$top"

setup() {               # setup DIR: the harness and the runtime for $T
    out=$1; mkdir -p "$out"
    export EMBCC_A32_HARNESS="$PWD/$out"
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c "tests/harness/arm-a32/$f.c" \
            -o "$out/$f.o" || { echo "the harness does not compile"; exit 1; }
    done
    { EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" &&
      EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib"; } > "$out/lib.log" 2>&1 || {
        echo "lib/rt or lib/libc does not build for $T"; tail -3 "$out/lib.log"
        exit 1; }
}
setup "$top"

# CC is embcc, clang (ARM state) or clangt (Thumb state)
compile() {             # compile CC OPT SRC OBJ
    case $1 in
    clang)  "$CLANG" --target=$T $CLF -ffreestanding -O1 \
                -I tests/golden -c "$3" -o "$4" ;;
    clangt) "$CLANG" --target=$T -mthumb $CLF -ffreestanding \
                -O1 -I tests/golden -c "$3" -o "$4" ;;
    *)      "$EMBCC" --target=$T "$2" -I tests/golden -c "$3" -o "$4" ;;
    esac
}

run_pair() {            # run_pair PROG CALLER-CC CALLEE-CC OPT TAG
    compile "$2" "$4" "tests/golden/$1-caller.c" "$out/$5-caller.o" || {
        echo "$5: $2 could not compile $1-caller.c"; return 1; }
    compile "$3" "$4" "tests/golden/$1-callee.c" "$out/$5-callee.o" || {
        echo "$5: $3 could not compile $1-callee.c"; return 1; }
    sh tests/harness/arm-a32/link.sh "$out/$5.elf" "$out/$5-caller.o" \
        "$out/$5-callee.o" "$out/lib/librt.a" > "$out/$5.lerr" 2>&1 || {
        echo "$5: embld could not link it:"; head -4 "$out/$5.lerr"
        return 1; }
    sh tests/harness/arm-a32/run.sh "$out/$5.elf" > "$out/$5.raw" 2>&1
    sed '/==END==/q' "$out/$5.raw" > "$out/$5.txt"
    grep -q '==END==' "$out/$5.txt" || {
        echo "$5: the image did not reach the end of main:"
        head -6 "$out/$5.raw"; return 1; }
    return 0
}

for prog in embedded-abi arm-a32-abi; do
run_pair $prog clang clang -O1 "$prog-cc" || exit 1
for opt in -O0 -O2; do
    for pair in "embcc embcc ee" "embcc clang ec" "clang embcc ce" \
                "embcc clangt et" "clangt embcc te"; do
        set -- $pair
        tag="$prog-$3$opt"
        run_pair $prog "$1" "$2" $opt "$tag" || exit 1
        diff -u "$out/$prog-cc.txt" "$out/$tag.txt" > "$out/$tag.diff" || {
            echo "$prog: $1 calling $2 at $opt disagrees with clang"
            echo "calling clang:"
            head -16 "$out/$tag.diff"; exit 1; }
    done
done
# the Thumb pairings did go through blx, both ways
for d in "et-O2-caller.o R_ARM_CALL" "te-O2-caller.o R_ARM_THM_CALL"; do
    set -- $d
    llvm-readelf -r "$out/$prog-$1" 2>/dev/null | grep -q "$2" || {
        echo "$prog-$1 makes no $2 call: the interworking pairing is vacuous"
        exit 1; }
done
echo "$prog: EmbCC and clang call each other identically at -O0 and -O2,"
echo "  in ARM state and across ARM and Thumb"
done

# 2. clang's objects on EmbCC's runtime
cc -std=c11 -w -o "$out/host" tests/golden/embedded-aeabi.c \
   tests/harness/thumb/hostio.c || { echo "FAIL: host build"; exit 1; }
"$out/host" > "$out/aeabi-ref.txt" || { echo "FAIL: host run"; exit 1; }
CI=$("$CLANG" -print-resource-dir)/include
for opt in -O1 -O2; do
    o=$out/aeabi$opt
    "$CLANG" --target=$T -mfloat-abi=soft $opt -ffreestanding -nostdinc \
        -isystem "$CI" -w -c tests/golden/embedded-aeabi.c -o $o.o || {
        echo "FAIL: clang does not compile embedded-aeabi.c"; exit 1; }
    n=$("$NM" -u $o.o | grep -c '__aeabi_' || true)
    [ "$n" -ge 4 ] || { echo "FAIL: $opt: clang called only $n helpers"; exit 1; }
    sh tests/harness/arm-a32/link.sh $o.elf $o.o "$out/lib/libc.a" \
        "$out/lib/librt.a" > $o.lerr 2>&1 || {
        echo "FAIL: $opt: does not link: $(head -1 $o.lerr)"; exit 1; }
    sh tests/harness/arm-a32/run.sh $o.elf > $o.raw 2>&1
    sed '/==END==/q' $o.raw > $o.txt
    diff -u "$out/aeabi-ref.txt" $o.txt > $o.diff || {
        echo "FAIL: $opt: clang's object on EmbCC's runtime disagrees with"
        echo "the host:"; head -12 $o.diff; exit 1; }
    echo "embedded-aeabi $opt: clang's object calls $n __aeabi_* helpers and agrees with the host"
done

# 4. hard float
T=armv7a-none-eabihf
CLF="-mfloat-abi=hard -mfpu=vfpv3-d16"
setup "$top/hf"
prog=arm-a32-abi
run_pair $prog clang clang -O1 "$prog-cc" || exit 1
for opt in -O0 -O2; do
    for pair in "embcc embcc ee" "embcc clang ec" "clang embcc ce"; do
        set -- $pair
        tag="$prog-$3$opt"
        run_pair $prog "$1" "$2" $opt "$tag" || exit 1
        diff -u "$out/$prog-cc.txt" "$out/$tag.txt" > "$out/$tag.diff" || {
            echo "$prog (hard float): $1 calling $2 at $opt disagrees with clang"
            head -16 "$out/$tag.diff"; exit 1; }
    done
done
llvm-objdump -d "$out/$prog-ee-O2-callee.o" 2>/dev/null | grep -q 'vadd' || {
    echo "the hard-float callee has no VFP arithmetic: the pairing is vacuous"
    exit 1; }
echo "$prog: AAPCS-VFP calls agree with clang's at -O0 and -O2"
echo "AAPCS calls in ARM state agree with clang's in both directions"
