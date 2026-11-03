#!/bin/sh
# -fsanitize=address, thread and memory with their runtimes (src/ir/
# sanitize.c, lib/san), run natively at -O0 and -O2: each program of
# tests/golden/sanitizers takes a mode as its argument -- 0 correct, the
# rest one bug each -- and the test is that mode 0 runs clean and prints
# its result, and every other mode is reported, by the right kind:
#
#   asan.c  1 heap overflow, 2 stack overflow, 3 global overflow,
#           4 use after free, 5 double free
#   tsan.c  1 a race (exit 66); 0 a mutex and 2 release/acquire atomics
#           race-free
#   msan.c  1 an uninitialized local, 2 heap, 3 a field through a call;
#           4 sscanf's and 5 strdup's results initialized
set -u
echo "TEST-MARKER sanitizers-runtime"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
out=$PWD/tests/golden/out/sanitizers-runtime
rm -rf "$out"; mkdir -p "$out"
src=$PWD/tests/golden/sanitizers
fail() { echo "FAIL: $*"; exit 1; }
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) T=arm64-apple-darwin ;;
    Linux-x86_64) T=x86_64-linux-gnu ;;
    *) echo "skipped: no native target on $(uname -s)-$(uname -m)"; exit 0 ;;
esac

# run SAN PROG MODE EXPECT: EXPECT is "ok" (clean, prints ok/done), or the
# text the report must contain
run() {
    "$out/$2" "$3" > "$out/run.log" 2>&1
    rc=$?
    if [ "$4" = ok ]; then
        [ "$rc" = 0 ] && grep -q '^\(ok\|done\)' "$out/run.log" ||
            fail "$1 $2 mode $3 is not clean (rc $rc): $(head -3 "$out/run.log")"
        ! grep -q 'Sanitizer' "$out/run.log" ||
            fail "$1 $2 mode $3 reported: $(grep Sanitizer "$out/run.log" | head -2)"
    else
        grep -q -- "$4" "$out/run.log" ||
            fail "$1 $2 mode $3: no '$4' (rc $rc): $(head -3 "$out/run.log")"
    fi
}

for O in -O0 -O2; do
    "$EMBCC" --target=$T $O -fsanitize=address "$src/asan.c" -o "$out/a" ||
        fail "asan build $O"
    run asan a 0 ok
    run asan a 1 "heap-buffer-overflow"
    run asan a 2 "stack-buffer-overflow"
    run asan a 3 "global-buffer-overflow"
    run asan a 4 "heap-use-after-free"
    run asan a 5 "attempting double-free"
    "$EMBCC" --target=$T $O -fsanitize=thread "$src/tsan.c" -o "$out/t" ||
        fail "tsan build $O"
    run tsan t 0 ok
    run tsan t 2 ok
    run tsan t 1 "ThreadSanitizer: data race"
    "$out/t" 1 > /dev/null 2>&1
    [ $? = 66 ] || fail "a race did not exit 66"
    "$EMBCC" --target=$T $O -fsanitize=memory "$src/msan.c" -o "$out/m" ||
        fail "msan build $O"
    run msan m 0 ok
    run msan m 4 ok
    run msan m 5 ok
    run msan m 1 "use-of-uninitialized-value"
    run msan m 2 "use-of-uninitialized-value"
    run msan m 3 "use-of-uninitialized-value"
    "$out/m" 1 > /dev/null 2>&1
    [ $? = 77 ] || fail "an uninitialized read did not exit 77"
done
echo "address, thread and memory: every bug reported by kind, every correct
run clean, at -O0 and -O2"
