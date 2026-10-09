#!/bin/sh
# verify/fuzz keeps working: its runner tells a program's true checksum
# from a wrong one on the x86-64 and Cortex-M4 boards, and three gen 2
# programs pass there. A fuzzer that cannot see a mismatch finds nothing,
# and one that no test runs rots (it lived outside the tree until the
# redesign moved it in: docs/internals/redesign.md).
set -u
echo "TEST-MARKER fuzz-selftest"
command -v python3 >/dev/null 2>&1 || { echo "SKIP: no python3"; exit 0; }
command -v "${FUZZ_CC:-clang}" >/dev/null 2>&1 || { echo "SKIP: no host clang"; exit 0; }
command -v qemu-system-x86_64 >/dev/null 2>&1 &&
    command -v qemu-system-arm >/dev/null 2>&1 || { echo "SKIP: no QEMU"; exit 0; }
out=tests/golden/out/fuzz-selftest
rm -rf "$out"; mkdir -p "$out"
FUZZ_OUT=$out sh verify/fuzz/run.sh -b x86,m4 --selftest > "$out/self.log" 2>&1 ||
    { echo "FAIL: the selftest: $(cat "$out/self.log")"; exit 1; }
cat "$out/self.log"
FUZZ_OUT=$out sh verify/fuzz/run.sh -b x86,m4 -O -O2 1 3 > "$out/run.log" 2>&1
grep -v "done$" "$out/run.log" | grep -q . &&
    { echo "FAIL: seeds 1-3: $(grep -v 'done$' "$out/run.log")"; exit 1; }
[ "$(grep -c 'done$' "$out/run.log")" = 3 ] || { echo "FAIL: the run did not finish: $(cat "$out/run.log")"; exit 1; }
echo "ok fuzz-selftest"
