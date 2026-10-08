#!/bin/sh
# Pointers to member functions on x86-64 at -Os. The Itanium C++ ABI's
# member-function pointer says "virtual" with the low bit of its function
# address, so a non-virtual member function must sit at an even address.
# -Os aligned functions to 1 byte, and a member function that landed at
# an odd one (A::b after a nine-byte A::a) was called through a vtable the
# class does not have: the program crashed at -Os only. Every function of
# a C++ unit is now 2-aligned there, as clang aligns member functions.
set -u
echo "TEST-MARKER cxx-mfp-align"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/cxx-mfp-align
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
src=tests/golden/cxx-mfp-align/prog.cc
NM=${EMBCC_LLVM_NM:-llvm-nm}
"$EMBCC" --target=x86_64-elf -Os -fno-exceptions -fno-rtti -c "$src" \
    -o "$out/p.o" || fail "prog.cc at -Os"
"$NM" "$out/p.o" | awk '$2 == "T" || $2 == "W" { print $1, $3 }' > "$out/syms"
[ -s "$out/syms" ] || fail "no function symbols"
while read -r addr name; do
    case $addr in
        *[13579bdf]) fail "$name is at the odd address 0x$addr at -Os" ;;
    esac
done < "$out/syms"
# a C unit keeps -Os's byte alignment: nothing else moves
printf 'int f(int a) { return a + 1; }\nint g(int a) { return a * 3; }\n' > "$out/c.c"
"$EMBCC" --target=x86_64-elf -Os -c "$out/c.c" -o "$out/c.o" || fail "c.c"
# (g starts where f ends: no padding between them)
"$NM" -S "$out/c.o" > "$out/c.nm"
fa=$(awk '$4 == "f" { print $1 }' "$out/c.nm"); fs=$(awk '$4 == "f" { print $2 }' "$out/c.nm")
ga=$(awk '$4 == "g" { print $1 }' "$out/c.nm")
[ -n "$fa" ] && [ -n "$fs" ] && [ -n "$ga" ] || fail "c.o's symbols: $(cat "$out/c.nm")"
[ $((0x$fa + 0x$fs)) -eq $((0x$ga)) ] ||
    fail "a C unit's functions are padded: $(cat "$out/c.nm")"
echo "every function of a C++ unit is 2-aligned at -Os"
# and the calls through the member pointers, run
if [ -x tests/harness/x86_64/run.sh ] && command -v qemu-system-x86_64 >/dev/null 2>&1; then
    for O in -O0 -O2 -Os; do
        "$EMBCC" --target=x86_64-elf $O -fno-exceptions -fno-rtti -c "$src" \
            -o "$out/r.o" || fail "prog.cc at $O"
        tests/harness/x86_64/link.sh -o "$out/r" "$out/r.o" > "$out/link.log" 2>&1 ||
            fail "prog.cc at $O does not link: $(head -3 "$out/link.log")"
        tests/harness/x86_64/run.sh "$out/r" > /dev/null 2>&1
        st=$?
        [ $st = 42 ] || fail "prog.cc at $O exits $st, not 42"
    done
    echo "the calls through member pointers return 42 at -O0, -O2 and -Os"
else
    echo "SKIP: the run (no x86-64 harness or qemu-system-x86_64)"
fi
echo "ok cxx-mfp-align"
