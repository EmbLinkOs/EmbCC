#!/bin/sh
# #pragma GCC diagnostic (and clang's spelling, and _Pragma): a file says
# which warnings it wants where. Each warning is decided by the pragmas
# before it in the preprocessed stream, push and pop applied in order --
# GCC's rule, which clang follows. Every expectation below is what clang
# reports for the same file.
set -u
echo "TEST-MARKER pragma-diagnostic"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=$EMBCC_ROOT/tests/golden/out/pragma-diagnostic
rm -rf "${out:?}"; mkdir -p "$out"
fail=0

cat > "$out/leak.h" <<'CEOF'
#pragma GCC diagnostic ignored "-Wunused-variable"
CEOF
cat > "$out/scoped.h" <<'CEOF'
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
int in_header(int p);
int in_header(int p) { return 0; }
#pragma GCC diagnostic pop
static inline int helper(int x) { return x; }
CEOF
cat > "$out/p.c" <<'CEOF'
#include "scoped.h"
int a1(int p1) { return 0; }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
int a2(void) { int v2; return 0; }
#pragma GCC diagnostic pop
int a3(void) { int v3; return 0; }
#define QUIET _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"")
#define LOUD _Pragma("GCC diagnostic warning \"-Wunused-parameter\"")
QUIET
int a4(int p4) { return 0; }
LOUD
int a5(int p5) { return 0; }
#pragma clang diagnostic error "-Wunused-parameter"
int a6(int p6) { return 0; }
#pragma GCC diagnostic warning "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wno-such-warning"
#include "leak.h"
int a7(void) { int v7; return 0; }
CEOF

# -Wall -Wextra: the command line turns both warnings on.
"$EMBCC" -Wall -Wextra -c "$out/p.c" -o "$out/p.o" > "$out/p.txt" 2>&1
rc=$?
grep -E 'warning:|error:' "$out/p.txt" |
    sed -En 's|.*/([a-z.]+):([0-9]+)(:[0-9]+)?: ([a-z]+):.*\[-W([a-z-]+)\]|\1:\2 \4 \5|p' \
    > "$out/got.txt"
cat > "$out/want.txt" <<'TXT'
p.c:2 warning unused-parameter
p.c:7 warning unused-variable
p.c:13 warning unused-parameter
p.c:15 error unused-parameter
TXT
cmp -s "$out/want.txt" "$out/got.txt" || {
    echo "FAIL: -Wall -Wextra: expected"; cat "$out/want.txt"
    echo "got"; cat "$out/p.txt"; fail=1; }
[ $rc -ne 0 ] || { echo "FAIL: the pragma's error did not fail the compile"
                   fail=1; }
[ $fail = 0 ] && echo "push/pop, ignored, warning, error, _Pragma, a header" \
    "that leaks and one that does not: as clang"

# With nothing on the command line, `warning` turns one on; under
# -Werror it stays a warning, which is how a file downgrades one.
cat > "$out/q.c" <<'CEOF'
#pragma GCC diagnostic warning "-Wunused-variable"
int b1(void) { int v; return 0; }
CEOF
r=$("$EMBCC" -c "$out/q.c" -o "$out/q.o" 2>&1)
case "$r" in *"q.c:2:"*"warning: unused variable 'v'"*) ;;
    *) echo "FAIL: a pragma did not turn a warning on: $r"; fail=1 ;; esac
r=$("$EMBCC" -Wall -Werror -c "$out/q.c" -o "$out/q.o" 2>&1) || {
    echo "FAIL: 'warning' under -Werror became an error: $r"; fail=1; }
[ $fail = 0 ] && echo "a pragma turns a warning on, and keeps it a warning" \
    "under -Werror"
exit $fail
