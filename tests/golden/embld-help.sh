#!/bin/sh
# `embld --help` (and -h) prints the usage on stdout and exits 0, the way
# `embsim --help` and `embmap --help` do. A bare `embld` with no inputs keeps
# printing the same usage to stderr and exits 2. Each case checks the exit
# status and which stream got the text, not only that the words appear.
set -eu
echo "TEST-MARKER embld-help"

EMBLD=${EMBLD:-./embld}
[ -x "$EMBLD" ] || { echo "skipped: embld is not built (make embld)"; exit 0; }
out=tests/golden/out/embld-help
rm -rf "$out"; mkdir -p "$out"

for flag in --help -h; do
    rc=0
    "$EMBLD" $flag > "$out/help.out" 2> "$out/help.err" || rc=$?
    [ "$rc" -eq 0 ] || { echo "FAIL: $flag exited $rc, want 0"; cat "$out/help.err"; exit 1; }
    [ ! -s "$out/help.err" ] || { echo "FAIL: $flag wrote to stderr"; cat "$out/help.err"; exit 1; }
    grep -q '^usage: embld ' "$out/help.out" ||
        { echo "FAIL: $flag printed no usage on stdout"; cat "$out/help.out"; exit 1; }
    grep -q 'embld --doctor' "$out/help.out" ||
        { echo "FAIL: $flag usage lacks the --doctor line"; cat "$out/help.out"; exit 1; }
    echo "$flag: usage on stdout, exit 0"
done

rc=0
"$EMBLD" > "$out/bare.out" 2> "$out/bare.err" || rc=$?
[ "$rc" -eq 2 ] || { echo "FAIL: bare embld exited $rc, want 2"; exit 1; }
[ ! -s "$out/bare.out" ] || { echo "FAIL: bare embld wrote to stdout"; exit 1; }
grep -q '^usage: embld ' "$out/bare.err" ||
    { echo "FAIL: bare embld printed no usage on stderr"; cat "$out/bare.err"; exit 1; }
echo "bare embld: usage on stderr, exit 2"
echo "PASS embld-help"
