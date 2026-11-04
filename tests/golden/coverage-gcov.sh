#!/bin/sh
# --coverage and the profile options (src/ir/gcov.c, lib/gcov):
#
#   1. a program built with --coverage, run twice, gives GCC 16's gcov
#      the counts a reading of the program gives (the loop's lines 12 and
#      10 times, each function 2), its .gcda merged across the runs;
#   2. --coverage-version=llvm writes what llvm-cov gcov reads;
#   3. --gcov-extract turns a board's console dump back into the .gcda,
#      and merges a second one into it (every count doubled);
#   4. -fprofile-use reads the profile -fprofile-generate's run wrote: the
#      never-run function goes to .text.unlikely, its call is a cold call
#      site the inliner leaves, and a missing profile is warned about.
#
# The programs run natively: on an arm64 Mac as Darwin programs, on an
# x86-64 Linux host as Linux ones.
set -u
echo "TEST-MARKER coverage-gcov"
. "$(dirname "$0")/../lib.sh"
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
out=$PWD/tests/golden/out/coverage-gcov
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) T=arm64-apple-darwin ;;
    Linux-x86_64) T=x86_64-linux-gnu ;;
    *) echo "skipped: no native target on $(uname -s)-$(uname -m)"; exit 0 ;;
esac

cat > "$out/t.c" <<'EOF'
int g;
int f(int x) {
    int s = 0;
    for (int i = 0; i < x; i++)
        if (i & 1) s += i; else s -= g;
    return s;
}
int never(int x) { return x * 7 + 1; }
int main(int argc, char **argv) {
    (void)argv;
    int r = f(5) == 3;
    if (argc > 5) r += never(argc);
    return r;
}
EOF

# ---- 1. GCC's gcov reads it, merged over two runs ---------------------
( cd "$out" && "$EMBCC" --target=$T --coverage t.c -o x ) || fail "--coverage build"
[ -f "$out/x-t.gcno" ] || fail "no x-t.gcno beside the image (GCC's and clang's name)"
( cd "$out" && ./x; ./x ) || true
[ -f "$out/x-t.gcda" ] || fail "the run wrote no x-t.gcda"
GCOV=$(command -v x86_64-elf-gcov || command -v gcov-16 || true)
if [ -n "$GCOV" ] && "$GCOV" --version 2>/dev/null | grep -q 'GCC) 1[2-9]'; then
    ( cd "$out" && "$GCOV" x-t.gcda > gcov.log 2>&1 ) || fail "gcov: $(cat "$out/gcov.log")"
    for want in '2:    2:int f' '12:    4:' '10:    5:' '2:   11:' '#####:    8:'; do
        grep -q -- "$want" "$out/t.c.gcov" ||
            fail "gcov's t.c.gcov lacks '$want':$(sed -n 1,20p "$out/t.c.gcov")"
    done
    echo "GCC $("$GCOV" --version | head -1 | sed 's/.* //') reads the notes and the merged counts"
else
    echo "(GCC 12+'s gcov not found: its reading skipped)"
fi

# ---- 2. llvm-cov's generation -------------------------------------------
LLVMCOV=$(command -v llvm-cov || ls /opt/homebrew/opt/llvm/bin/llvm-cov 2>/dev/null || true)
if [ -n "$LLVMCOV" ]; then
    mkdir -p "$out/llvm"
    cp "$out/t.c" "$out/llvm/"
    ( cd "$out/llvm" && "$EMBCC" --target=$T --coverage --coverage-version=llvm t.c -o x &&
      ./x; "$LLVMCOV" gcov x-t.gcda > cov.log 2>&1 ) || true
    grep -q 'Lines executed:.*of' "$out/llvm/cov.log" ||
        fail "llvm-cov gcov did not read --coverage-version=llvm: $(cat "$out/llvm/cov.log")"
    grep -q ' 6:    4:' "$out/llvm/t.c.gcov" ||
        fail "llvm-cov's count of line 4 (one run) is not 6"
    echo "llvm-cov gcov reads --coverage-version=llvm"
fi

# ---- 3. a board's console dump ------------------------------------------
# What lib/gcov prints where there is no file system, made here from the
# file the run wrote: `@gcda PATH N`, hex, `@end`, amid other output.
dump() {
    n=$(wc -c < "$1" | tr -d ' ')
    printf 'boot\n@gcda %s %s\n' "$2" "$n"
    od -An -v -tx1 "$1" | tr -d ' \n' | fold -w 64
    printf '\n@end\nmore output\n'
}
dump "$out/x-t.gcda" "$out/board/x-t.gcda" > "$out/console.log"
mkdir -p "$out/board"
"$EMBCC" --gcov-extract "$out/console.log" > "$out/ex1.log" || fail "--gcov-extract"
cmp -s "$out/x-t.gcda" "$out/board/x-t.gcda" || fail "the extracted .gcda differs"
"$EMBCC" --gcov-extract "$out/console.log" > "$out/ex2.log" || fail "--gcov-extract again"
grep -q merged "$out/ex2.log" || fail "the second extract did not merge: $(cat "$out/ex2.log")"
if [ -n "$GCOV" ]; then
    cp "$out/x-t.gcno" "$out/board/"; cp "$out/t.c" "$out/board/"
    ( cd "$out/board" && "$GCOV" x-t.gcda > /dev/null 2>&1 )
    grep -q '24:    4:' "$out/board/t.c.gcov" ||
        fail "merging doubled no count: $(grep ':    4:' "$out/board/t.c.gcov")"
fi
echo "--gcov-extract writes a board's dump, and merges the next"

# ---- 4. -fprofile-generate, then -fprofile-use --------------------------
mkdir -p "$out/pgo"
cp "$out/t.c" "$out/pgo/"
( cd "$out/pgo" && "$EMBCC" --target=$T -O2 -fprofile-generate t.c -o gen && ./gen ) || true
[ -f "$out/pgo/gen-t.gcda" ] || fail "-fprofile-generate's run wrote no profile"
( cd "$out/pgo" && "$EMBCC" --target=x86_64-linux-gnu -O2 -fprofile-use=gen-t.gcda \
    -fremarks -c t.c -o use.o ) > "$out/pgo/use.log" 2>&1 || fail "-fprofile-use: $(cat "$out/pgo/use.log")"
grep -q "cold-call-site" "$out/pgo/use.log" ||
    fail "the never-run call is not a cold call site: $(grep never "$out/pgo/use.log")"
"$READELF" -S "$out/pgo/use.o" 2>/dev/null | grep -q 'text.unlikely' ||
    fail "the never-run function is not in .text.unlikely"
( cd "$out/pgo" && "$EMBCC" --target=$T -O2 -fprofile-use -c t.c -o miss.o ) \
    > "$out/pgo/miss.log" 2>&1
grep -q "missing-profile" "$out/pgo/miss.log" || fail "no --warn=missing-profile"
echo "-fprofile-use: the cold function apart, its call left alone, a missing profile said"
