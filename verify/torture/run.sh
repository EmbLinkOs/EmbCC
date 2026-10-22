#!/bin/sh
# verify/torture/run.sh [--update] [compile|execute|all]
#
# GCC's own torture suite (gcc/testsuite/gcc.c-torture: about 2,000 files
# to compile and 1,900 programs to run, every one code GCC accepts) as a
# regression test, against the baselines in verify/torture/expected/:
#
#   compile  every file, for x86-64, Cortex-M4, RV32 and AVR, at -O0 and
#            -O2. A file that compiled before must still compile, and no
#            file may crash EmbCC -- a signal, an internal error, or no
#            answer in 60 s -- ever: EmbCC compiles or refuses by name.
#   execute  every execute/ program, built for this host (aarch64-apple-
#            darwin on an arm64 Mac, x86_64-elf on x86-64 Linux, linked
#            by the host's cc as tests/run.sh links its corpus) at -O0 and
#            -O2, and run. One that passed (exit 0) before must still pass.
#
# A file that newly compiles or passes is reported, not failed: --update
# records it. The suite comes from verify/torture/fetch.sh, at the commit
# the baselines were made against; without it this says SKIP.
#
# This found four silent miscompiles when it was a survey (char
# a[0x100000001] sized 1, a struct over 2 GB wrapped, an address stored as
# a pointer difference, a void ?: merging an undefined temporary); as a
# test it keeps what has been won.
set -u
UPDATE=0
[ "${1:-}" = --update ] && { UPDATE=1; shift; }
WHAT=${1:-all}
EMBCC=${EMBCC:-$PWD/embcc}
DIR=${EMBCC_TORTURE_DIR:-build/gcc-torture}
TORTURE=$DIR/gcc/testsuite/gcc.c-torture
HERE=verify/torture
EXP=$HERE/expected
# each run's results, one file per configuration, kept for reading (and for
# CI to upload): STATUS, FILE and DETAIL, tab-separated
OUT=${EMBCC_TORTURE_OUT:-build/torture-out}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
# a quicker run: EMBCC_TORTURE_TARGETS="x86_64-elf" EMBCC_TORTURE_OPTS=-O0
TARGETS=${EMBCC_TORTURE_TARGETS:-x86_64-elf thumbv7em-none-eabi riscv32-unknown-elf avr}
OPTS=${EMBCC_TORTURE_OPTS:--O0 -O2}
echo "TEST-MARKER torture"

if [ ! -d "$TORTURE" ]; then
    echo "SKIP: no GCC torture suite at $DIR (sh verify/torture/fetch.sh fetches it)"
    exit 0
fi
[ -x "$EMBCC" ] || { echo "FAIL: $EMBCC is not built"; exit 1; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/embtorture.XXXXXX") || exit 2
trap 'rm -rf "${WORK:?}"' EXIT INT TERM
export EMBCC TORTURE WORK
mkdir -p "$EXP" "$OUT"

# the host's own target, for execute
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) HOST=aarch64-apple-darwin ;;
    Linux-x86_64) HOST=x86_64-elf ;;
    *) HOST= ;;
esac

fail=0
# check NAME RESULTS GOOD: compare the files RESULTS calls GOOD with the
# baseline NAME, and report crashes
check() {
    name=$1 res=$2 good=$3
    awk -F'\t' -v g="$good" '$1 == g { print $2 }' "$res" | sort > "$WORK/now"
    awk -F'\t' '$1 == "crash"' "$res" > "$WORK/crash"
    cp "$res" "$OUT/$name.tsv"
    total=$(wc -l < "$res" | tr -d ' ')
    now=$(wc -l < "$WORK/now" | tr -d ' ')
    if [ $UPDATE = 1 ]; then
        cp "$WORK/now" "$EXP/$name.txt"
        echo "$name: $now of $total $good, recorded"
    else
        [ -f "$EXP/$name.txt" ] || { echo "FAIL $name: no baseline $EXP/$name.txt (--update)"; fail=1; return; }
        comm -23 "$EXP/$name.txt" "$WORK/now" > "$WORK/lost"
        comm -13 "$EXP/$name.txt" "$WORK/now" > "$WORK/won"
        lost=$(wc -l < "$WORK/lost" | tr -d ' ')
        won=$(wc -l < "$WORK/won" | tr -d ' ')
        if [ "$lost" -gt 0 ]; then
            echo "FAIL $name: $lost that were $good no longer are:"
            while read -r f; do grep -F "	$f	" "$res" | head -1; done < "$WORK/lost" | head -20
            fail=1
        fi
        if [ "$won" -gt 0 ]; then
            echo "$name: $now of $total $good, $won newly (--update records them)"
        else
            echo "$name: $now of $total $good"
        fi
    fi
    # crashes: the known ones are listed in crash-known.txt, each a bug to
    # fix (and take off the list); any other is a failure
    awk -F'\t' -v n="$name" '{ print n "\t" $2 }' "$WORK/crash" | sort > "$WORK/crashed"
    if [ $UPDATE = 1 ]; then
        { grep -v "^$name	" "$EXP/crash-known.txt" 2>/dev/null; cat "$WORK/crashed"; } |
            sort > "$WORK/known" && cp "$WORK/known" "$EXP/crash-known.txt"
        [ -s "$WORK/crashed" ] &&
            echo "$name: $(wc -l < "$WORK/crashed" | tr -d ' ') crashes recorded as known -- each one is a bug"
        return
    fi
    grep "^$name	" "$EXP/crash-known.txt" 2>/dev/null | sort > "$WORK/known"
    comm -23 "$WORK/crashed" "$WORK/known" > "$WORK/newcrash"
    comm -13 "$WORK/crashed" "$WORK/known" > "$WORK/fixed"
    if [ -s "$WORK/newcrash" ]; then
        echo "FAIL $name: EmbCC crashed on $(wc -l < "$WORK/newcrash" | tr -d ' ') it did not crash on before:"
        cut -f2 "$WORK/newcrash" | while read -r f; do grep -F "	$f	" "$WORK/crash" | head -1; done | head -20
        fail=1
    fi
    [ -s "$WORK/known" ] && [ -s "$WORK/crashed" ] &&
        echo "  ($(comm -12 "$WORK/crashed" "$WORK/known" | wc -l | tr -d ' ') known crashes: $EXP/crash-known.txt)"
    [ -s "$WORK/fixed" ] &&
        echo "  ($(wc -l < "$WORK/fixed" | tr -d ' ') known crashes no longer crash: --update takes them off the list)"
}

if [ "$WHAT" = compile ] || [ "$WHAT" = all ]; then
    find "$TORTURE/compile" "$TORTURE/execute" -name '*.c' | sort > "$WORK/files"
    for t in $TARGETS; do
        for o in $OPTS; do
            xargs -P "$JOBS" -n 1 sh "$HERE/one.sh" compile "$t" "$o" < "$WORK/files" > "$WORK/res"
            check "compile-$t$o" "$WORK/res" ok
        done
    done
fi
if [ "$WHAT" = execute ] || [ "$WHAT" = all ]; then
    if [ -z "$HOST" ]; then
        echo "SKIP execute: no native run on $(uname -s)-$(uname -m) (arm64 macOS or x86-64 Linux)"
    else
        find "$TORTURE/execute" -name '*.c' | sort > "$WORK/files"
        for o in $OPTS; do
            xargs -P "$JOBS" -n 1 sh "$HERE/one.sh" execute "$HOST" "$o" < "$WORK/files" > "$WORK/res"
            check "execute-$HOST$o" "$WORK/res" pass
        done
    fi
fi
[ $fail = 0 ] && echo "torture: as the baselines say" || exit 1
