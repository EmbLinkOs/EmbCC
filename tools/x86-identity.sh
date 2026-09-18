#!/bin/sh
# x86-64 regression cover for a host that cannot RUN x86-64 code.
#
# Builds embcc at a baseline git revision, then compiles the same inputs with
# it and with the working tree's ./embcc and requires the objects to be
# byte-identical. On a Linux x86-64 box `make test` runs what embcc emits; on
# an arm64 Mac it cannot, so this is how a change to shared code (the front
# end, irgen, the optimizer, the driver) proves it left the x86 backend alone.
# Identical bytes are a stronger statement than "the tests still pass" -- they
# say nothing changed at all -- and a change that is SUPPOSED to alter x86
# output will fail here, which is the point: look at the diff, then move the
# baseline.
#
#   usage: tools/x86-identity.sh [BASELINE_REV]      (default: HEAD)
#
# Inputs: every tests/exec/*.c at -O0/-O1/-O2 and with -g, plus, when an
# EmbLinkOS tree is found ($EMBCC_MYOS, default ~/EmbLinkOs), every x86 kernel
# C file at -O0 and -O2 with the kernel's own flags. A file both compilers
# refuse is counted, not failed: that is a gap, not a regression.
set -eu

rev=${1:-HEAD}
cd "$(dirname "$0")/.."
root=$PWD
[ -x ./embcc ] || { echo "x86-identity: build ./embcc first" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

git archive "$rev" | tar -x -C "$work"
mkdir -p "$work/base"
# The baseline is built exactly as it was, except that clang's
# -Wunused-but-set-variable is not allowed to stop an old tree that predates
# the fix for it: the point is to compare its CODEGEN, not to re-lint it.
make -C "$work" -s embcc \
    CFLAGS="-std=c99 -Wall -Wextra -Werror -Wno-unused-but-set-variable -g" \
    >/dev/null 2>&1 || { echo "x86-identity: baseline $rev does not build" >&2; exit 1; }
BASE=$work/embcc
NEW=$root/embcc

same=0; differ=0; refused=0
compare() {   # compare NAME FLAGS... SRC
    name=$1; shift
    "$BASE" "$@" -o "$work/base/a.o" >/dev/null 2>&1 && rb=0 || rb=$?
    "$NEW"  "$@" -o "$work/base/b.o" >/dev/null 2>&1 && rn=0 || rn=$?
    if [ "$rb" -ne 0 ] && [ "$rn" -ne 0 ]; then refused=$((refused + 1)); return; fi
    if [ "$rb" -ne "$rn" ]; then
        echo "  DIFFERS  $name: baseline exit $rb, now exit $rn"; differ=$((differ + 1)); return
    fi
    if cmp -s "$work/base/a.o" "$work/base/b.o"; then same=$((same + 1))
    else echo "  DIFFERS  $name: object bytes"; differ=$((differ + 1)); fi
}

for c in tests/exec/*.c; do
    for fl in "-O0" "-O1" "-O2" "-O0 -g" "-O2 -g"; do
        # $fl is split on purpose: it carries one or two flags.
        # shellcheck disable=SC2086
        compare "$c [$fl]" $fl -c "$c"
    done
done

MYOS=${EMBCC_MYOS:-$HOME/EmbLinkOs}
if [ -d "$MYOS/kernel" ]; then
    list=$work/kernel-srcs
    printf 'include Makefile\n__x86_identity:\n\t@printf "%%s\\n" $(KERNEL_SRC) | grep "\\.c$$" > %s\n' "$list" > "$work/print.mk"
    (cd "$MYOS" && make -f "$work/print.mk" --no-print-directory __x86_identity >/dev/null 2>&1) || :
    if [ -s "$list" ]; then
        KFLAGS="-mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -I$MYOS/kernel"
        while read -r src; do
            for lv in -O0 -O2; do
                # shellcheck disable=SC2086
                compare "$src [$lv]" $KFLAGS $lv -c "$MYOS/$src"
            done
        done < "$list"
    fi
else
    echo "x86-identity: no EmbLinkOS tree at $MYOS -- kernel inputs skipped"
fi

echo "x86-identity vs $rev: $same identical, $differ different, $refused refused by both"
[ "$differ" -eq 0 ]
