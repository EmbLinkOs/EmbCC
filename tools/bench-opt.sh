#!/bin/sh
# Where EmbCC's optimizer stands, against gcc and clang, on the same
# kernel.
#
# Every kernel in tests/bench/kernels.c exists because a named
# transformation is what makes it fast, so the per-kernel ratio says
# WHICH pass is missing rather than that the compiler is slow. The
# checksums must agree: a compiler that gets faster by computing
# something else is a bug, not a result.
#
#   usage: tools/bench-opt.sh [-O2] [SCALE]
#
# Measured on the Linux harness under QEMU, which is emulation -- so the
# absolute times mean nothing and the RATIOS mean everything. They are
# stable to a few percent across runs; anything under 1.1x is noise.
set -eu
cd "$(dirname "$0")/.."
root=$PWD
OPT=${1:--O2}
SCALE=${2:-1}
out=${TMPDIR:-/tmp}/embcc-bench.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT

L=$root/build/libc/linux-x86_64
[ -f "$L/libc.a" ] || { echo "no libc for linux-x86_64 (make libc-linux-x86_64)" >&2
                        exit 1; }
"$root/tests/harness/linux/run.sh" x86_64 --check > /dev/null 2>&1 || {
    echo "no kernel for tests/harness/linux" >&2; exit 1; }

GCC=${EMBCC_X86_GCC:-x86_64-elf-gcc}
SRC="$root/tests/bench/kernels.c $root/tests/bench/opaque.c"
INC="-I$root/lib/libc/include"

run_one() {                      # run_one NAME BINARY
    set +e
    "$root/tests/harness/linux/run.sh" x86_64 "$2" > "$out/$1.txt" 2>&1
    rc=$?
    set -e
    [ "$rc" = 42 ] || { echo "$1: exited $rc" >&2; cat "$out/$1.txt" >&2
                        exit 1; }
}

# ---- embcc ---------------------------------------------------------------
for s in $SRC; do
    "$root/embcc" --target=x86_64-linux-gnu "$OPT" -DSCALE=$SCALE \
        -c "$s" -o "$out/e_$(basename "$s" .c).o" || exit 1
done
"$root/embld" -o "$out/embcc.bin" "$L/crt1.o" "$out"/e_*.o "$L/libc.a" \
    "$L/librt.a" || exit 1
run_one embcc "$out/embcc.bin"

# ---- gcc, linked against the SAME libc so only the codegen differs -------
have_gcc=0
if command -v "$GCC" > /dev/null 2>&1; then
    have_gcc=1
    for s in $SRC; do
        "$GCC" "$OPT" $INC -c "$s" -DSCALE=$SCALE \
            -o "$out/g_$(basename "$s" .c).o" || exit 1
    done
    "$root/embld" -o "$out/gcc.bin" "$L/crt1.o" "$out"/g_*.o "$L/libc.a" \
        "$L/librt.a" "$("$GCC" -print-libgcc-file-name)" || exit 1
    run_one gcc "$out/gcc.bin"
fi

# ---- the report ----------------------------------------------------------
esum=$(grep '^checksum' "$out/embcc.txt" | awk '{print $2}')
if [ "$have_gcc" = 1 ]; then
    gsum=$(grep '^checksum' "$out/gcc.txt" | awk '{print $2}')
    [ "$esum" = "$gsum" ] || {
        echo "CHECKSUMS DIFFER: embcc $esum, gcc $gsum" >&2
        echo "  one of them is computing something else; the times below" >&2
        echo "  would be meaningless" >&2
        exit 1; }
fi

printf '%-16s %10s %10s %8s\n' kernel embcc gcc ratio
total_e=0; total_g=0
while read -r name et; do
    [ "$name" = checksum ] && continue
    if [ "$have_gcc" = 1 ]; then
        gt=$(awk -v n="$name" '$1==n{print $2}' "$out/gcc.txt")
        r=$(awk -v a="$et" -v b="$gt" 'BEGIN{ if (b>0) printf "%.2f", a/b;
                                              else printf "-" }')
        printf '%-16s %10s %10s %7sx\n' "$name" "$et" "$gt" "$r"
        total_e=$(awk -v a="$total_e" -v b="$et" 'BEGIN{print a+b}')
        total_g=$(awk -v a="$total_g" -v b="$gt" 'BEGIN{print a+b}')
    else
        printf '%-16s %10s %10s %8s\n' "$name" "$et" - -
    fi
done < "$out/embcc.txt"

if [ "$have_gcc" = 1 ]; then
    printf '%-16s %10.3f %10.3f %7.2fx\n' TOTAL "$total_e" "$total_g" \
        "$(awk -v a="$total_e" -v b="$total_g" 'BEGIN{print a/b}')"
    echo "(ratio > 1 means EmbCC is slower; checksums agree at $esum)"
fi
