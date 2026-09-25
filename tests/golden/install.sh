#!/bin/sh
# Being installable, which EmbCC was not until now.
#
# `embcc hello.c` could not find <stdio.h>. Every test in this tree
# passed -Ilib/libc/include by hand, so the compiler worked and nobody
# could have used it -- there was no install target, no prefix, and no
# default search path of any kind.
#
# What is checked here is the property that makes it usable: the
# compiler finds its own files RELATIVE TO ITS OWN BINARY, so the same
# executable works from a build tree, from an unpacked tarball, and
# from wherever `make install PREFIX=...` put it, with nothing compiled
# in and no rebuild after moving.
#
# A test that only exercised the build tree would prove the case that
# already worked. So this stages a real `make install` and runs THAT
# copy, from an unrelated directory, with no flags.
set -eu
echo "TEST-MARKER install"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/install
rm -rf "$out"; mkdir -p "$out"

# ---- 1. the build tree finds itself --------------------------------------
"$EMBCC" --print-search-dirs > "$out/tree.txt" 2>&1
grep -q '^layout: build tree' "$out/tree.txt" || {
    echo "FAIL: running from the build tree was not recognised as one:"
    cat "$out/tree.txt"; exit 1; }
#    Checked FIRST in paths.c, so that working on EmbCC uses the tree
#    being worked on and never a copy installed earlier.
grep -q "^lib: $EMBCC_ROOT\$" "$out/tree.txt" || {
    echo "FAIL: the build tree resolved to the wrong root:"
    cat "$out/tree.txt"; exit 1; }

cat > "$out/hello.c" << 'EOF'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void)
{
    char *p = malloc(16);
    strcpy(p, "ok");
    printf("%s %d\n", p, 42);
    free(p);
    return 42;
}
EOF
#    No -I of any kind: that is the whole point.
( cd "$out" && "$EMBCC" --target=x86_64-linux-gnu -c hello.c -o tree.o ) \
    2> "$out/cc1.log" || {
    echo "FAIL: the build tree cannot compile <stdio.h> without -I:"
    cat "$out/cc1.log"; exit 1; }
echo "a build-tree binary finds its own headers: <stdio.h> with no -I"

# ---- 2. a staged install, and the copy that results ----------------------
#
# The install rule itself, into a DESTDIR -- which is what a package
# build does, and what proves the rule rather than a hand-made
# directory that happens to match it.
#
# `install-files` rather than `install`: the second builds what it
# needs first, and a test that did THAT would rebuild the compiler
# while the rest of the suite is running on it. The pieces it copies
# are already built by the time any test runs; a missing one is
# reported below as a missing file, which is the honest failure.
stage=$out/stage
( cd "$EMBCC_ROOT" && make install-files DESTDIR="$stage" \
    PREFIX=/usr/local ) > "$out/install.log" 2>&1 || {
    echo "FAIL: make install:"; tail -20 "$out/install.log"; exit 1; }
IE=$stage/usr/local/bin/embcc
[ -x "$IE" ] || { echo "FAIL: no installed embcc at $IE"; exit 1; }

#    The version in the PATH and the version the compiler prints have to
#    be the same string, or an upgrade silently reads the old version's
#    headers. src/driver/version.h is the one definition; this is the
#    check that the Makefile and the compiler still agree about it.
ver=$("$IE" --version | sed -n '1s/^EmbCC \([^ ]*\) .*/\1/p')
[ -d "$stage/usr/local/lib/embcc/$ver" ] || {
    echo "FAIL: --version says '$ver' but there is no"
    echo "      lib/embcc/$ver -- the Makefile and version.h disagree"
    ls "$stage/usr/local/lib/embcc/" 2>&1; exit 1; }

"$IE" --print-search-dirs > "$out/inst.txt" 2>&1
grep -q '^layout: installed' "$out/inst.txt" || {
    echo "FAIL: the installed copy does not know it is installed:"
    cat "$out/inst.txt"; exit 1; }

#    Every default include directory it named must actually be there. A
#    search path that points at nothing is how "cannot find <stdio.h>"
#    happens to somebody who installed correctly.
sed -n 's/^include: //p' "$out/inst.txt" | while read -r d; do
    [ -d "$d" ] || { echo "FAIL: search dir does not exist: $d"; exit 1; }
done

#    From an unrelated working directory, so nothing is found by
#    accident of where the test happens to run.
( cd /tmp && "$IE" --target=x86_64-linux-gnu -c "$out/hello.c" \
    -o "$out/inst.o" ) 2> "$out/cc2.log" || {
    echo "FAIL: the installed compiler cannot compile <stdio.h>:"
    cat "$out/cc2.log"; exit 1; }

cat > "$out/hello.cc" << 'EOF'
#include <cstdio>
#include <vector>
int main() { std::vector<int> v{1, 2, 3}; std::printf("%zu\n", v.size()); }
EOF
( cd /tmp && "$IE" --target=x86_64-linux-gnu -c -x c++ "$out/hello.cc" \
    -o "$out/instcc.o" ) 2> "$out/cc3.log" || {
    echo "FAIL: the installed compiler cannot compile <vector>:"
    cat "$out/cc3.log"; exit 1; }
echo "an installed copy compiles C and C++ from an unrelated directory
with no flags, and the version in its path is the one it prints"

# ---- 3. -nostdinc still means what it says --------------------------------
if ( cd /tmp && "$IE" -nostdinc --target=x86_64-linux-gnu -c \
        "$out/hello.c" -o /dev/null ) 2> "$out/nostd.log"; then
    echo "FAIL: -nostdinc still found the default headers"; exit 1
fi
grep -q 'cannot find include file' "$out/nostd.log" || {
    echo "FAIL: -nostdinc failed for the wrong reason:"
    cat "$out/nostd.log"; exit 1; }
echo "-nostdinc removes them, for a build that supplies its own"

# ---- 4. the per-target files, and a program built only from them ---------
#
# A compiler that finds its headers but not its libraries is half
# installed. These are the files a link needs, and the check is that a
# program built from the installed ones RUNS.
L=$stage/usr/local/lib/embcc/$ver
for f in libc.a crt1.o link.ld; do
    [ -f "$L/x86_64-linux-gnu/$f" ] || {
        echo "FAIL: $f was not installed for x86_64-linux-gnu:"
        ls "$L/x86_64-linux-gnu/" 2>&1; exit 1; }
done

LD=${EMBCC_X86_LD:-x86_64-elf-ld}
if command -v "$LD" > /dev/null 2>&1; then
    "$LD" -static -T "$L/x86_64-linux-gnu/link.ld" -o "$out/hello.exe" \
        "$L/x86_64-linux-gnu/crt1.o" "$out/inst.o" \
        "$L/x86_64-linux-gnu/libc.a" 2> "$out/ld.log" || {
        echo "FAIL: linking from installed files:"; cat "$out/ld.log"
        exit 1; }
    if "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 --check \
           > /dev/null 2>&1; then
        set +e
        "$EMBCC_ROOT/tests/harness/linux/run.sh" x86_64 "$out/hello.exe" \
            > "$out/run.txt" 2>&1
        rc=$?
        set -e
        [ "$rc" = 42 ] || { echo "FAIL: the installed toolchain built a
program that exited $rc, wanted 42:"; cat "$out/run.txt"; exit 1; }
        grep -qx 'ok 42' "$out/run.txt" || {
            echo "FAIL: wrong output:"; cat "$out/run.txt"; exit 1; }
        echo "and a program built ONLY from installed files -- compiler,
crt1.o, libc.a, link.ld -- runs on a real kernel and exits 42"
    else
        echo "per-target files install; running the result needs a kernel
for tests/harness/linux/run.sh"
    fi
else
    echo "per-target files install (no $LD to link them with here)"
fi

# ---- 5. EMBCC_PREFIX, and what it does when it is wrong ------------------
EMBCC_PREFIX=$stage/usr/local "$IE" --print-search-dirs \
    > "$out/pfx.txt" 2>&1
grep -q '^layout: installed' "$out/pfx.txt" || {
    echo "FAIL: EMBCC_PREFIX did not select the installed layout:"
    cat "$out/pfx.txt"; exit 1; }
#    Pointed somewhere with no EmbCC in it, it says so ONCE rather than
#    letting every #include fail separately and leaving the reader to
#    work out which of them was the real problem.
EMBCC_PREFIX=/nonexistent-prefix "$IE" --print-search-dirs \
    > "$out/bad.txt" 2>&1 || true
grep -q 'has no lib/embcc' "$out/bad.txt" || {
    echo "FAIL: a wrong EMBCC_PREFIX is not reported:"
    cat "$out/bad.txt"; exit 1; }
echo "EMBCC_PREFIX selects a layout, and a wrong one is reported once
rather than as a stream of missing headers"
