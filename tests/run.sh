#!/bin/sh
# Test runner (ARCHITECTURE.md §7).
#
#   usage: tests/run.sh [--target=x86_64-elf|aarch64-elf] [--exec-only]
#
# --exec-only skips the .sh tests and runs just the compiled-and-RUN corpus —
# the quick loop while working on codegen.
#
# Two kinds of test:
#   *.sh under tests/{exec,compile,golden}/ — run with $EMBCC set; passes
#     when it exits 0 AND its output contains "TEST-MARKER <name>". The
#     marker catches CONTRIBUTING's lie #2 (a test that silently never
#     ran).
#   *.c under tests/exec/ — compiled by embcc and RUN; the exit code must
#     equal the '// expect-exit: N' line in the file. These are the tests
#     that count: they assert the machine ran what we emitted. Artifacts
#     are rebuilt from scratch every run (lie #1: the stale binary).
#
# How "RUN" happens depends on the target and the host:
#
#   x86_64-elf   on a Linux x86-64 host, linked with the host cc and executed
#                directly — there the host IS the target. Anywhere else,
#                linked into a Multiboot image against the cross newlib and
#                booted under qemu-system-x86_64, with the debug console
#                carrying stdout and an exit marker back out
#                (tests/harness/x86_64/). EMBCC_X86_RUNNER=host|qemu forces
#                one or the other.
#   aarch64-elf  linked into a bare-metal image against the cross newlib
#                and executed under qemu-system-aarch64 -M virt, with ARM
#                semihosting carrying stdout and the exit status back out
#                (tests/harness/aarch64/).
#
# Either way the code runs on the architecture it was compiled for; nothing
# here is a cross-check against a model of what it would have done.
set -u

TARGET=x86_64-elf
EXEC_ONLY=0
for a in "$@"; do
    case "$a" in
        --target=*) TARGET=${a#--target=} ;;
        --exec-only) EXEC_ONLY=1 ;;
        *) echo "run.sh: unknown argument '$a'" >&2; exit 1 ;;
    esac
done

QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-20}
export EMBCC_QEMU_TIMEOUT=$QEMU_TIMEOUT
export EMBCC_TARGET=$TARGET      # the golden tests read it through tests/lib.sh

if [ -z "${EMBCC_X86_RUNNER:-}" ]; then
    if [ "$(uname -s)-$(uname -m)" = Linux-x86_64 ]; then
        EMBCC_X86_RUNNER=host
    else
        EMBCC_X86_RUNNER=qemu
    fi
fi

cd "$(dirname "$0")/.."
EMBCC="$PWD/embcc"
export EMBCC

# Turn on the optimizer's IR verifier for the whole suite: every -O1/-O2 compile
# (optimizer.sh, regalloc-O2.sh, the on-metal kernel build) then checks that no
# pass dropped a live value or left var_scope indices stale, and aborts loudly if
# so. -O0 skips the optimizer entirely, so this is free there. It exists because
# the var_scope-after-DCE miscompile passed the whole suite once (see
# tests/exec/scope-dce-shift.c) — this makes that class a hard failure, not a
# boot-the-kernel-to-find-out.
export EMBCC_VERIFY=1

if [ ! -x "$EMBCC" ]; then
    echo "run.sh: $EMBCC not built (run make first)" >&2
    exit 1
fi

pass=0
fail=0
skip=0

ok()   { echo "PASS $1"; pass=$((pass + 1)); }
bad()  { echo "FAIL $1 ($2)"; [ -n "$3" ] && printf '%s\n' "$3" | sed 's/^/     | /'; fail=$((fail + 1)); }

# The .sh tests drive the x86-64 toolchain (embld, embas, the golden
# disassemblies). Under --target=aarch64-elf only the ones that are about
# aarch64 run; the rest would be asserting the wrong machine.
# tests/golden/*.sh run for every target (they compile with --target=$TARGET);
# tests/golden/<arch>/ holds what is about one machine only — the x86-64
# toolchain (embas, embld, embdbg, the kernel build) or the aarch64 encoder
# and inline-asm referees. The refusal tests drive the default target.
arch_dir=tests/golden/${TARGET%-elf}
if [ "$TARGET" = aarch64-elf ]; then
    sh_tests="tests/golden/*.sh $arch_dir/*.sh"
else
    sh_tests="tests/exec/*.sh tests/compile/*.sh tests/golden/*.sh $arch_dir/*.sh"
fi

[ "$EXEC_ONLY" = 1 ] && sh_tests=""

for t in $sh_tests; do
    [ -e "$t" ] || continue
    name=$(basename "$t" .sh)
    out=$(sh "$t" 2>&1)
    status=$?
    if [ $status -eq 0 ] && printf '%s\n' "$out" | grep -q "TEST-MARKER $name" &&
       printf '%s\n' "$out" | grep -q '^skipped:'; then
        # A test that could not run here proves nothing, so it is not counted
        # as a pass — on a host missing its prerequisites it says why.
        echo "SKIP $t ($(printf '%s\n' "$out" | grep -m1 '^skipped:' | cut -c10-))"
        skip=$((skip + 1))
    elif [ $status -eq 0 ] && printf '%s\n' "$out" | grep -q "TEST-MARKER $name"; then
        ok "$t"
    elif [ $status -eq 0 ]; then
        bad "$t" "exit 0 but marker 'TEST-MARKER $name' missing — did it run?" "$out"
    else
        bad "$t" "exit $status" "$out"
    fi
done

# tests/exec/*.c run on every target; tests/exec/<arch>/*.c on that one only
for c in tests/exec/*.c tests/exec/${TARGET%-elf}/*.c; do
    [ -e "$c" ] || continue
    name=$(basename "$c" .c)
    # `// target: TRIPLE` pins a test to one machine: an x86-64 inline-asm
    # test is not a program for aarch64 at all, so it is skipped there rather
    # than counted as a gap it is not.
    only=$(sed -n 's|.*// target: *\([a-z0-9_-]*\).*|\1|p' "$c" | head -1)
    if [ -n "$only" ] && [ "$only" != "$TARGET" ]; then
        echo "SKIP $c (target: $only)"
        skip=$((skip + 1))
        continue
    fi
    expect=$(sed -n 's|.*// expect-exit: *\([0-9][0-9]*\).*|\1|p' "$c" | head -1)
    if [ -z "$expect" ]; then
        bad "$c" "no '// expect-exit: N' line — cannot assert anything" ""
        continue
    fi
    out_dir="tests/exec/out"
    mkdir -p "$out_dir"
    obj="$out_dir/$name.o"
    exe="$out_dir/$name"
    rm -f "$obj" "$exe"
    if ! msg=$("$EMBCC" --target="$TARGET" -c "$c" -o "$obj" 2>&1); then
        bad "$c" "embcc failed" "$msg"
        continue
    fi
    # Where the code runs: on this host when it IS the target, else on the
    # target's QEMU harness (tests/harness/<arch>/{link,run}.sh).
    if [ "$TARGET" = x86_64-elf ] && [ "$EMBCC_X86_RUNNER" = host ]; then
        if ! msg=$(cc -no-pie -o "$exe" "$obj" 2>&1); then
            bad "$c" "host link failed" "$msg"
            continue
        fi
        "$exe" >/dev/null 2>&1 # golden/agrees-with-gcc.sh diffs the output
        got=$?
    else
        harness=tests/harness/${TARGET%-elf}
        if ! msg=$("$harness/link.sh" -o "$exe" "$obj" 2>&1); then
            bad "$c" "${TARGET%-elf} link failed" "$msg"
            continue
        fi
        "$harness/run.sh" "$exe" >/dev/null 2>&1
        got=$?
        case $got in
            124) bad "$c" "timed out after ${QEMU_TIMEOUT}s under QEMU" ""; continue ;;
            125) bad "$c" "the guest crashed or reset before exiting" ""; continue ;;
        esac
    fi
    if [ "$got" -eq "$expect" ]; then
        ok "$c (exit $got)"
    else
        bad "$c" "exit $got, expected $expect" ""
    fi
done

# C++ (docs/language/cpp-levels.md): each tests/cxx program compiled by embcc over the
# target's newlib, linked with the reference libstdc++ (tools/build-ref-gxx.sh)
# and run on the QEMU harness — never the host, whose C++ runtime is not the
# one EmbLinkOS uses.
. tools/hostpaths.sh
if [ "$TARGET" = aarch64-elf ]; then
    cxx_newlib=$AARCH64_NEWLIB ref_gxx=$AARCH64_REF_GXX
else
    cxx_newlib=$X86_NEWLIB ref_gxx=$X86_REF_GXX
fi
for cc in tests/cxx/*.cc; do
    [ -e "$cc" ] || continue
    name=$(basename "$cc" .cc)
    if [ ! -f "$ref_gxx/$TARGET/lib/libstdc++.a" ]; then
        echo "SKIP $cc (no reference libstdc++ at $ref_gxx: tools/build-ref-gxx.sh $TARGET)"
        skip=$((skip + 1))
        continue
    fi
    expect=$(sed -n 's|.*// expect-exit: *\([0-9][0-9]*\).*|\1|p' "$cc" | head -1)
    if [ -z "$expect" ]; then
        bad "$cc" "no '// expect-exit: N' line — cannot assert anything" ""
        continue
    fi
    out_dir="tests/cxx/out/${TARGET%-elf}"
    mkdir -p "$out_dir"
    obj="$out_dir/$name.o"
    exe="$out_dir/$name"
    rm -f "$obj" "$exe"
    if ! msg=$("$EMBCC" --target="$TARGET" -I"$cxx_newlib/include" -c "$cc" \
                   -o "$obj" 2>&1); then
        bad "$cc" "embcc failed" "$msg"
        continue
    fi
    harness=tests/harness/${TARGET%-elf}
    if ! msg=$(EMBCC_REF_GXX=$ref_gxx "$harness/link.sh" --cxx -o "$exe" "$obj" 2>&1); then
        bad "$cc" "${TARGET%-elf} link failed" "$msg"
        continue
    fi
    "$harness/run.sh" "$exe" >/dev/null 2>&1
    got=$?
    case $got in
        124) bad "$cc" "timed out after ${QEMU_TIMEOUT}s under QEMU" ""; continue ;;
        125) bad "$cc" "the guest crashed or reset before exiting" ""; continue ;;
    esac
    if [ "$got" -eq "$expect" ]; then
        ok "$cc (exit $got)"
    else
        bad "$cc" "exit $got, expected $expect" ""
    fi
done

total=$((pass + fail))
if [ $total -eq 0 ]; then
    # Zero tests is a failure, not a green run: an empty suite proves nothing.
    echo "run.sh: no tests found" >&2
    exit 1
fi
echo "-----"
if [ "$skip" -gt 0 ]; then
    echo "$pass/$total passed ($skip skipped: see SKIP lines)"
else
    echo "$pass/$total passed"
fi
[ $fail -eq 0 ]
