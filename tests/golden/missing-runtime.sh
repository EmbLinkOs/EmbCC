#!/bin/sh
# A hosted target whose runtime is missing says which file, and what to
# do about it. A review of EmbCC for EmbLinkRTOS built the compiler on
# Linux with the make target for the compiler alone; CMake's first check
# links a program for the host, and got "no crt1.o for x86_64-linux-gnu --
# the target's library is not built or not installed", which left the
# reader to find the make target. In a build tree the message names it;
# installed, it names `make install`. (A plain `make` builds every
# runtime now; the Makefile says why.)
#
# Neither case touches the real tree: the build tree is a copy of the
# compiler beside an empty header directory, and the install is a prefix
# with headers and no libraries.
set -u
echo "TEST-MARKER missing-runtime"
EMBCC=${EMBCC:-./embcc}
case $EMBCC in /*) ;; *) EMBCC=$PWD/$EMBCC ;; esac
out=$PWD/tests/golden/out/missing-runtime
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

printf 'int main(void) { return 0; }\n' > "$out/m.c"
printf 'int main() { return 0; }\n' > "$out/m.cc"

# ---- a build tree with no runtime built -----------------------------------
tree=$out/tree
mkdir -p "$tree/lib/libc/include" "$tree/lib/libcxx/include" "$tree/include"
: > "$tree/lib/libc/include/stdio.h"
cp "$EMBCC" "$tree/embcc"
"$tree/embcc" --target=x86_64-linux-gnu "$out/m.c" -o "$out/m" 2> "$out/c.err" &&
    fail "a link with no crt1.o succeeded"
grep -qF "embcc: error: no crt1.o for x86_64-linux-gnu: its C library is not built; \`make libc-linux-x86_64\` in $tree builds it" "$out/c.err" ||
    { cat "$out/c.err"; fail "the build tree's message does not name the make target"; }
"$tree/embcc" --target=x86_64-linux-gnu "$out/m.cc" -o "$out/m" 2> "$out/cxx.err" &&
    fail "a C++ link with no libcxx.a succeeded"
grep -qF "no libcxx.a for x86_64-linux-gnu: its C++ runtime is not built; \`make libcxx-linux-x86_64\`" "$out/cxx.err" ||
    { cat "$out/cxx.err"; fail "the C++ runtime's message does not name the make target"; }
echo "missing-runtime: in a build tree, the message names the make target that builds the runtime"

# ---- an install with no libraries ------------------------------------------
ver=$("$EMBCC" --version | sed -n '1s/^EmbCC \([^ ]*\) .*/\1/p')
pfx=$out/prefix
mkdir -p "$pfx/lib/embcc/$ver/include"
: > "$pfx/lib/embcc/$ver/include/stdio.h"
EMBCC_PREFIX=$pfx "$EMBCC" --target=x86_64-linux-gnu "$out/m.c" -o "$out/m" 2> "$out/i.err" &&
    fail "a link against an install with no libraries succeeded"
grep -qF "no crt1.o for x86_64-linux-gnu: its C library is not installed; \`make install\` from EmbCC's source installs every target's" "$out/i.err" ||
    { cat "$out/i.err"; fail "the installed compiler's message does not name make install"; }
echo "missing-runtime: installed, it names make install"
