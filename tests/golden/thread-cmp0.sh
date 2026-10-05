#!/bin/sh
# A 0/1 merged from two arms and then compared with zero is threaded: each
# arm jumps where the branch would have gone, and neither the 0/1 nor the
# compare is built.
#
# C writes `(cond ? 1 : 0) == 0` (FreeRTOS: `listLIST_IS_EMPTY(l) ==
# pdFALSE`, a macro that yields pdTRUE/pdFALSE). The compare between the
# merge and the branch hid the merge from the jump threading that `a && b`
# already got; the branch now reads the merged value directly. In
# FreeRTOS's queue.c that was 13% of the code at -Os.
set -u
echo "TEST-MARKER thread-cmp0"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/thread-cmp0
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
cat > "$out/f.c" <<'EOF'
struct list { unsigned n; };
void yes(void);
void no(void);
void f(struct list *l)
{
    if (((l->n == 0) ? 1 : 0) == 0)
        yes();
    else
        no();
}
EOF
for t in thumbv7m-none-eabi riscv32-unknown-elf x86_64-elf; do
    for O in -O1 -O2 -Os; do
        "$EMBCC" inspect ir --target=$t $O "$out/f.c" > "$out/f.ir" 2>&1 ||
            fail "$t $O: inspect ir"
        # the merged value and its compare are gone: one branch, on n
        # itself, and no 0/1 left for a merge
        n=$(grep -Ec '^  br(n?z)' "$out/f.ir")
        [ "$n" = 1 ] || { cat "$out/f.ir"; fail "$t $O: $n branches, want 1"; }
        if grep -Eq ' = const\.[0-9a-z]* [01]	' "$out/f.ir"; then
            cat "$out/f.ir"; fail "$t $O: a 0/1 constant is still built"
        fi
    done
done
echo "thread-cmp0: a merged 0/1 compared with zero is threaded on Thumb, RISC-V and x86-64"
