#!/bin/sh
# The Mach-O object writer (D-014), judged by the platform's own tools.
#
# A format writer is one of the few things that can be checked against an
# absolute authority rather than against itself: the SDK declares the
# structures, and ld either accepts the file or does not. Both are used
# here, and nothing in this file trusts our own header.
#
# macOS only, and skipped elsewhere -- it needs the SDK to compare
# against and the system linker to be judged by.
set -eu
echo "TEST-MARKER macho"
. "$(dirname "$0")/../lib.sh"

case "$(uname -s)" in
    Darwin) ;;
    *) echo "skipped: the Mach-O writer is checked against the macOS SDK"
       exit 0 ;;
esac

out=$EMBCC_ROOT/tests/golden/out/macho-$ARCH
rm -rf "$out"; mkdir -p "$out"
D=$EMBCC_ROOT/tests/golden/darwin
SRC="$EMBCC_ROOT/src"

# 1. Our header against the SDK's. Every constant, every struct size,
#    every offset the writer depends on. A format is a contract with
#    someone else's parser, so the numbers are not ours to decide.
cc -I"$SRC/macho" -o "$out/machochk" "$D/machochk.c" 2> "$out/chk.log" || {
    echo "FAIL: the SDK conformance check did not build:"
    cat "$out/chk.log"; exit 1; }
"$out/machochk" > "$out/chk.txt" || {
    echo "FAIL: our Mach-O header disagrees with the SDK:"
    cat "$out/chk.txt"; exit 1; }
cat "$out/chk.txt"

# 2. A minimal object, written by the writer and read by the system.
cc -o "$out/machogen" "$D/machogen.c" "$SRC/macho/write.c" \
    "$SRC/driver/util.c" "$SRC/driver/diag.c" \
    "$SRC/platform/platform_posix.c" 2> "$out/gen.log" || {
    echo "FAIL: the generator did not build:"; cat "$out/gen.log"; exit 1; }

host=$(uname -m)
case "$host" in
    arm64|aarch64) cpu=arm64 ;;
    *)             cpu=x86_64 ;;
esac

"$out/machogen" "$cpu" "$out/t.o" || {
    echo "FAIL: the writer reported failure"; exit 1; }

# file(1) parses the header independently of otool.
file "$out/t.o" | grep -q "Mach-O 64-bit object $cpu" || {
    echo "FAIL: not recognised as a Mach-O object:"; file "$out/t.o"; exit 1; }

# otool reads the load commands: the three we emit, and no more.
otool -l "$out/t.o" > "$out/lc.txt" 2>&1
for c in LC_SEGMENT_64 LC_SYMTAB LC_BUILD_VERSION; do
    grep -q "$c" "$out/lc.txt" || {
        echo "FAIL: $c missing from the load commands:"
        head -30 "$out/lc.txt"; exit 1; }
done

# nm reads the symbol table, and proves the platform's leading
# underscore was applied: the C name is `main`, the symbol is `_main`.
nm "$out/t.o" > "$out/nm.txt" 2>&1
grep -q '^0*0 T _main$' "$out/nm.txt" || {
    echo "FAIL: _main is not a defined text symbol:"; cat "$out/nm.txt"; exit 1; }
echo "file, otool and nm all read it; the symbol carries the platform's underscore"

# 3. The judgement that matters: the system linker accepts it, and the
#    program runs. A writer whose output only our own tools like has
#    proved nothing.
cc -o "$out/prog" "$out/t.o" 2> "$out/link.log" || {
    echo "FAIL: the system linker refused our object:"
    cat "$out/link.log"; exit 1; }
set +e
"$out/prog"
rc=$?
set -e
[ "$rc" = 42 ] || { echo "FAIL: the linked program exited $rc, wanted 42"; exit 1; }
echo "the system linker accepts it and the program runs, exiting 42"
