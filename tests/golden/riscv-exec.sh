#!/bin/sh
# What the RISC-V backend COMPUTES, at BOTH widths (D-016).
#
# Each program is compiled by EmbCC, linked by embld into an image, and
# run on QEMU's `virt` board -- then compiled by clang for the same
# triple, linked by the same embld, and run on the same board. The two
# outputs must agree.
#
# That shape matters: the reference travels through EmbCC's own linker
# and harness, so a difference is the COMPILER's and not a difference in
# how the image was built.
#
# Everything runs TWICE, once at RV32 and once at RV64, and that is the
# point of the suite rather than a doubling of it. One backend serves
# both widths, so a bug in the width-dependent parts -- the register
# pairs, the `w` instruction forms, the load and store sizes -- passes at
# one width and fails at the other. Three of the four bugs found while
# writing this backend were exactly that shape, including an `lwu`
# emitted at RV32, where the instruction does not exist.
#
# The PROGRAMS are tests/golden/embedded-*.c, shared with the ARMv7-M
# suite: ordinary C that names no machine.
set -u
echo "TEST-MARKER riscv-exec"
. "$(dirname "$0")/../lib.sh"

CLANG=${EMBCC_REF_GCC_RISCV:-clang}
command -v "$CLANG" >/dev/null 2>&1 || {
    echo "SKIP: no reference compiler for RISC-V (set EMBCC_REF_GCC_RISCV)"
    exit 0; }

H=tests/harness/riscv
out=tests/golden/out/riscv-exec
rm -rf "$out"; mkdir -p "$out"
# tests/run.sh runs the golden tests concurrently and each may write only
# inside its own output directory, so the harness objects go there.
export EMBCC_RISCV_HARNESS="$PWD/$out"

any=0
for x in 32 64; do
    QEMU=${EMBCC_QEMU_RISCV:-qemu-system-riscv$x}
    command -v "$QEMU" >/dev/null 2>&1 || {
        echo "SKIP rv$x: $QEMU not found"; continue; }
    any=1
    T=riscv$x-unknown-elf
    if [ "$x" = 32 ]; then MARCH=rv32im; MABI=ilp32; else MARCH=rv64im; MABI=lp64; fi

    # The harness is built by EmbCC. Its startup is C; the four
    # instructions that set sp before it are embld's (-Tstack), because
    # RISC-V has no hardware equivalent of a Cortex-M's vector table and
    # C cannot write sp.
    for f in boot io; do
        "$EMBCC" --target=$T -c "$H/$f.c" -o "$out/$f.o" || {
            echo "rv$x: the harness does not compile"; exit 1; }
    done

    run_image() {       # run_image OBJ... -> $out/$TAG.txt
        tag=$1; shift
        sh "$H/link.sh" "$out/$tag.elf" "$@" || {
            echo "$tag: embld could not link the image"; return 1; }
        sh "$H/run.sh" "$out/$tag.elf" "$x" > "$out/$tag.txt" 2>&1
        grep -q '==END==' "$out/$tag.txt" || {
            echo "$tag: the image did not reach the end of main:"
            sed -n '1,10p' "$out/$tag.txt"
            return 1; }
        return 0
    }

    # 1. The stress program, against clang for the same triple. This is
    #    the strongest check here: both sides are the same target, so
    #    even `long`'s width agrees and any difference is the compiler's.
    src=tests/golden/embedded-stress.c
    # -mcmodel=medany, and not as a detail: medlow materialises an
    # address with `lui`, which SIGN-EXTENDS bit 31, so at RV64 it
    # cannot name anything between 0x80000000 and 0xffffffff7fffffff --
    # and that is where this image is. EmbCC hit the same wall and its
    # answer was the same, PC-relative `auipc` at both widths (D-016).
    "$CLANG" -target $T -march=$MARCH -mabi=$MABI -mcmodel=medany \
             -ffreestanding -Os \
             -c "$src" -o "$out/stress-ref$x.o" || {
        echo "rv$x: the reference compiler could not compile the stress program"
        exit 1; }
    run_image "stress-ref$x" "$out/stress-ref$x.o" || exit 1

    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c "$src" -o "$out/stress$x$opt.o" || {
            echo "rv$x $opt: EmbCC could not compile the stress program"
            exit 1; }
        run_image "stress$x$opt" "$out/stress$x$opt.o" || exit 1
        if ! diff -u "$out/stress-ref$x.txt" "$out/stress$x$opt.txt" \
             > "$out/stress$x$opt.diff"; then
            echo "rv$x: the stress program at $opt does not agree with $CLANG:"
            head -20 "$out/stress$x$opt.diff"
            exit 1
        fi
    done
    echo "rv$x stress: EmbCC agrees with $CLANG at -O0, -O1, -O2 and -Os"

    # 2. 64-bit integers, against the HOST. `long long` arithmetic has
    #    one answer whatever the register width, so the machine this
    #    suite runs on is as good a reference -- and a better one at
    #    RV32, where EmbCC calls its own __divdi3 and a clang image would
    #    be comparing two runtimes rather than two compilers. At RV64 it
    #    is a check that nothing calls a helper at all.
    "$EMBCC" --target=$T -Os -c lib/rt/int64.c -o "$out/int64$x.o" || {
        echo "rv$x: the 64-bit runtime does not compile"; exit 1; }
    cc -std=c99 -w -o "$out/host64" tests/golden/embedded-int64.c \
       "$H/hostio.c" || {
        echo "the 64-bit program does not compile for the host"; exit 1; }
    "$out/host64" > "$out/int64-ref.txt" || {
        echo "the 64-bit program failed on the host"; exit 1; }
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c tests/golden/embedded-int64.c \
                 -o "$out/i64$x$opt.o" || {
            echo "rv$x $opt: the 64-bit program does not compile"; exit 1; }
        run_image "i64$x$opt" "$out/i64$x$opt.o" "$out/int64$x.o" || exit 1
        if ! diff -u "$out/int64-ref.txt" "$out/i64$x$opt.txt" \
             > "$out/i64$x$opt.diff"; then
            echo "rv$x: 64-bit arithmetic at $opt does not agree with the host:"
            head -20 "$out/i64$x$opt.diff"
            exit 1
        fi
    done
    echo "rv$x int64: 64-bit arithmetic agrees with the host at four levels"

    # 3. Floating point, the same way and for the same reason: IEEE
    #    arithmetic has one answer, and the host's hardware gives it in
    #    one instruction where this target gives it in a call into
    #    lib/rt/softfp.c. Compared as BIT PATTERNS, so a result off by
    #    one ulp fails.
    "$EMBCC" --target=$T -Os -c lib/rt/softfp.c -o "$out/softfp$x.o" || {
        echo "rv$x: the soft-float runtime does not compile"; exit 1; }
    "$EMBCC" --target=$T -Os -Ilib/libc/include -c lib/libc/src/math/sqrt.c \
             -o "$out/sqrt$x.o" || {
        echo "rv$x: libc's sqrt does not compile"; exit 1; }
    cc -std=c99 -w -o "$out/hostfp" tests/golden/embedded-float.c \
       "$H/hostio.c" -lm || {
        echo "the float program does not compile for the host"; exit 1; }
    "$out/hostfp" > "$out/float-ref.txt" || {
        echo "the float program failed on the host"; exit 1; }
    for opt in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$T $opt -c tests/golden/embedded-float.c \
                 -o "$out/fp$x$opt.o" || {
            echo "rv$x $opt: the float program does not compile"; exit 1; }
        run_image "fp$x$opt" "$out/fp$x$opt.o" "$out/softfp$x.o" \
                  "$out/int64$x.o" "$out/sqrt$x.o" || exit 1
        if ! diff -u "$out/float-ref.txt" "$out/fp$x$opt.txt" \
             > "$out/fp$x$opt.diff"; then
            echo "rv$x: IEEE results at $opt are not the host's bit patterns:"
            head -20 "$out/fp$x$opt.diff"
            exit 1
        fi
    done
    echo "rv$x float: IEEE results are bit-identical to the host at four levels"

    # 4. Aggregates by value and variadic calls: the two halves of the
    #    psABI that are not arithmetic, and the two where this target
    #    disagrees with AAPCS32 most. Against the host, which passes
    #    them by its own rules -- so what is checked is that EmbCC's
    #    caller and EmbCC's callee agree with each other about a
    #    convention, and that the VALUES come out right.
    for prog in aggregate varargs; do
        cc -std=c99 -w -o "$out/host-$prog" "tests/golden/embedded-$prog.c" \
           "$H/hostio.c" || {
            echo "the $prog program does not compile for the host"; exit 1; }
        "$out/host-$prog" > "$out/$prog-ref.txt" || {
            echo "the $prog program failed on the host"; exit 1; }
        for opt in -O0 -O1 -O2 -Os; do
            "$EMBCC" --target=$T $opt -c "tests/golden/embedded-$prog.c" \
                     -o "$out/$prog$x$opt.o" || {
                echo "rv$x $opt: the $prog program does not compile"; exit 1; }
            run_image "$prog$x$opt" "$out/$prog$x$opt.o" \
                      "$out/int64$x.o" "$out/softfp$x.o" || exit 1
            if ! diff -u "$out/$prog-ref.txt" "$out/$prog$x$opt.txt" \
                 > "$out/$prog$x$opt.diff"; then
                echo "rv$x: $prog at $opt does not agree with the host:"
                head -20 "$out/$prog$x$opt.diff"
                exit 1
            fi
        done
        echo "rv$x $prog: agrees with the host at four levels"
    done

    # 5. The ABI, against clang ACROSS THE CALL. Every check above
    #    compiles both sides with the same compiler, so a backend that
    #    read the psABI consistently wrong would agree with itself all
    #    the way through. This links an EmbCC-compiled caller against a
    #    clang-compiled callee and the other way round, and requires all
    #    four pairings to print the same thing.
    #
    #    It is where the rules that differ from AAPCS32 are actually
    #    tested: a fixed 2*XLEN scalar in an odd register pair, a
    #    variadic one in an even pair, an aggregate packed by bytes, one
    #    passed by reference, and a variadic callee's register save area
    #    lining up with where the caller left the arguments.
    abi_pair() {            # abi_pair CALLER-CC CALLEE-CC TAG
        for side in caller callee; do
            eval "cc=\$$([ $side = caller ] && echo 1 || echo 2)"
            if [ "$cc" = clang ]; then
                "$CLANG" -target $T -march=$MARCH -mabi=$MABI \
                    -mcmodel=medany -ffreestanding -O1 -I tests/golden \
                    -c "tests/golden/embedded-abi-$side.c" \
                    -o "$out/$3$x-$side.o" || {
                    echo "$3: clang could not compile the $side"; return 1; }
            else
                "$EMBCC" --target=$T -O1 -I tests/golden \
                    -c "tests/golden/embedded-abi-$side.c" \
                    -o "$out/$3$x-$side.o" || {
                    echo "$3: EmbCC could not compile the $side"; return 1; }
            fi
        done
        run_image "$3$x" "$out/$3$x-caller.o" "$out/$3$x-callee.o" \
                  "$out/int64$x.o"
    }
    abi_pair embcc embcc ee || exit 1
    abi_pair embcc clang ec || exit 1
    abi_pair clang embcc ce || exit 1
    abi_pair clang clang cc || exit 1
    for tag in ec ce cc; do
        diff -u "$out/ee$x.txt" "$out/$tag$x.txt" > "$out/$tag$x.abidiff" || {
            echo "rv$x: the $tag pairing disagrees with EmbCC calling itself:"
            head -12 "$out/$tag$x.abidiff"; exit 1; }
    done
    echo "rv$x abi: EmbCC and clang call each other's aggregates identically"
done

[ "$any" = 1 ] || { echo "SKIP: no qemu-system-riscv32/64 found"; exit 0; }
echo "RISC-V images build with embld and run on QEMU's virt board"
