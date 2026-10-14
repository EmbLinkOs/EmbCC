#!/bin/sh
# install.sh, the installer behind `curl ... | sh`, against a release
# built here.
#
# The release workflow (.github/workflows/release.yml) packs the tree
# `make install` writes with packaging/tarball.sh and uploads it to GitHub
# with its SHA-256 file. install.sh downloads the tarball for the machine,
# checks it against that file, and copies it into a prefix. This test
# builds the same tarball from `make install-files`, lays it out in a
# directory the way GitHub lays out a release's URLs, and points
# install.sh at it with EMBCC_INSTALL_BASE_URL, so nothing here touches the
# network. It runs install.sh under every POSIX shell this host has,
# because `curl ... | sh` runs it under whichever one /bin/sh is.
#
# What is checked:
#   1. the installed embcc reports the release's version, knows it is
#      installed, and compiles without flags from an unrelated directory;
#   2. installing a version again replaces it rather than merging into it;
#   3. a tarball that does not match its checksum, or has none, installs
#      nothing, and leaves an existing install as it was;
#   4. a machine with no tarball is told so and falls back to building
#      from source.
#
# EMBCC_INSTALL_SH runs another copy of the installer, which is how a
# broken one is shown to fail this test.
set -eu
echo "TEST-MARKER install-script"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
INSTALL_SH=${EMBCC_INSTALL_SH:-$EMBCC_ROOT/install.sh}
out=$EMBCC_ROOT/tests/golden/out/install-script
rm -rf "$out"; mkdir -p "$out"

# Nothing from the caller's environment chooses for install.sh, and a
# source build, if one is ever attempted, fails here rather than
# downloading.
unset EMBCC_INSTALL_PLATFORM
export EMBCC_INSTALL_SOURCE_URL="$out/no-source"

# The tarball's name, as install.sh derives it for this machine.
case $(uname -s) in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    *) os= ;;
esac
case $(uname -m) in
    x86_64 | amd64) cpu=x86_64 ;;
    aarch64 | arm64) cpu=aarch64 ;;
    *) cpu= ;;
esac
if [ "$os" = macos ] && [ "$cpu" = x86_64 ] &&
   [ "$(sysctl -n sysctl.proc_translated 2> /dev/null)" = 1 ]; then
    cpu=aarch64
fi
if [ -z "$os" ] || [ -z "$cpu" ]; then
    echo "skipped: no release tarball is built for $(uname -s) $(uname -m)"
    exit 0
fi
plat=$os-$cpu

ver=$("$EMBCC" --version | sed -n '1s/^EmbCC \([^ ]*\) .*/\1/p')
[ -n "$ver" ] || { echo "FAIL: embcc --version names no version"; exit 1; }
tag=v$ver

# Every shell the installer is run under: sh, and dash, bash and zsh
# where they are installed.
shells=sh
for s in dash bash zsh; do
    if command -v "$s" > /dev/null 2>&1; then
        shells="$shells $s"
    fi
done
for s in $shells; do
    "$s" -n "$INSTALL_SH" || {
        echo "FAIL: $s cannot parse install.sh"; exit 1; }
done

# ---- the release, as the workflow makes it --------------------------------
#
# `install-files` rather than `install`, as in tests/golden/install.sh:
# it only copies what is built, and never rebuilds the compiler the rest of
# the suite is running.
( cd "$EMBCC_ROOT" && make install-files DESTDIR="$out/stage" \
    PREFIX=/usr/local ) > "$out/stage.log" 2>&1 || {
    echo "FAIL: make install-files:"; tail -20 "$out/stage.log"; exit 1; }
rel=$out/release
sh "$EMBCC_ROOT/packaging/tarball.sh" "$out/stage/usr/local" "$ver" "$plat" \
    "$rel/download/$tag" > "$out/tarball.log" 2>&1 || {
    echo "FAIL: packaging/tarball.sh:"; cat "$out/tarball.log"; exit 1; }
mkdir -p "$rel/latest/download"
printf '%s\n' "$tag" > "$rel/latest/download/embcc-version.txt"
asset=embcc-$ver-$plat.tar.gz
[ -s "$rel/download/$tag/$asset" ] && [ -s "$rel/download/$tag/$asset.sha256" ] || {
    echo "FAIL: packaging/tarball.sh wrote no $asset and .sha256:"
    ls -R "$rel"; exit 1; }

cat > "$out/hello.c" << 'EOF'
#include <stdio.h>
#include <string.h>

int main(void)
{
    printf("hello from EmbCC %d\n", (int)strlen("embedded"));
    return 0;
}
EOF

# run_installer LOG ARGS...: install.sh against the local release, from an
# unrelated directory.
run_installer() {
    log=$1; shift
    ( cd /tmp && EMBCC_INSTALL_BASE_URL=$rel "$@" ) > "$log" 2>&1
}

# check_prefix PREFIX: what an installed EmbCC must be able to do.
check_prefix() {
    p=$1
    [ -x "$p/bin/embcc" ] || {
        echo "FAIL: no $p/bin/embcc after install.sh"; exit 1; }
    got=$("$p/bin/embcc" --version | sed -n '1s/^EmbCC \([^ ]*\) .*/\1/p')
    [ "$got" = "$ver" ] || {
        echo "FAIL: the installed embcc says '$got', the release is $ver"
        exit 1; }
    "$p/bin/embcc" --print-search-dirs > "$out/dirs.txt" 2>&1
    grep -q '^layout: installed' "$out/dirs.txt" || {
        echo "FAIL: the installed embcc does not know it is installed:"
        cat "$out/dirs.txt"; exit 1; }
    real=$(cd "$p" && pwd -P)
    grep -qx "lib: $real/lib/embcc/$ver" "$out/dirs.txt" || {
        echo "FAIL: the installed embcc uses another tree than $real:"
        cat "$out/dirs.txt"; exit 1; }
    [ "$(readlink "$p/bin/embcc-ar")" = embar ] || {
        echo "FAIL: bin/embcc-ar is not the link to embar it is in the tarball"
        ls -l "$p/bin"; exit 1; }
    ( cd /tmp && "$p/bin/embcc" --target=thumbv7em-none-eabi --opt=size \
        -c "$out/hello.c" -o "$out/hello-m4.o" ) 2> "$out/cc.log" || {
        echo "FAIL: the installed embcc cannot compile <stdio.h> for a Cortex-M4:"
        cat "$out/cc.log"; exit 1; }
}

# ---- 1. installed, under each shell --------------------------------------
for s in $shells; do
    run_installer "$out/install-$s.log" \
        "$s" "$INSTALL_SH" --prefix "$out/prefix-$s" || {
        echo "FAIL: install.sh under $s:"; cat "$out/install-$s.log"; exit 1; }
    grep -q 'checksum verified' "$out/install-$s.log" || {
        echo "FAIL: install.sh under $s did not check the checksum:"
        cat "$out/install-$s.log"; exit 1; }
    check_prefix "$out/prefix-$s"
done
P=$out/prefix-sh
#    A program, linked in-process, where the Linux library was built.
if [ -f "$P/lib/embcc/$ver/x86_64-linux-gnu/crt1.o" ]; then
    ( cd /tmp && "$P/bin/embcc" --target=x86_64-linux-gnu "$out/hello.c" \
        -o "$out/hello-linux" ) 2> "$out/link.log" || {
        echo "FAIL: the installed embcc cannot link for x86_64-linux-gnu:"
        cat "$out/link.log"; exit 1; }
    [ "$(od -An -tx1 -N4 "$out/hello-linux" | tr -d ' \n')" = 7f454c46 ] || {
        echo "FAIL: the linked program is not an ELF file"; exit 1; }
fi
echo "install.sh installs the release under $shells, from a local mirror:
the checksum is verified, and the installed embcc reports $ver and compiles"

# ---- 2. installing again replaces the version ----------------------------
echo stale > "$P/lib/embcc/$ver/include/stale.h"
run_installer "$out/again.log" sh "$INSTALL_SH" --prefix "$P" \
    --version "$ver" || {
    echo "FAIL: installing $ver again:"; cat "$out/again.log"; exit 1; }
[ ! -e "$P/lib/embcc/$ver/include/stale.h" ] || {
    echo "FAIL: installing $ver again kept a file the release does not have"
    exit 1; }
check_prefix "$P"
echo "installing $ver again (--version $ver) replaces it, files and links"

# ---- 3. a checksum that does not match, or none --------------------------
bad=$out/release-bad
cp -R "$rel" "$bad"
printf 'x' >> "$bad/download/$tag/$asset"
if ( cd /tmp && EMBCC_INSTALL_BASE_URL=$bad sh "$INSTALL_SH" \
        --prefix "$out/prefix-bad" ) > "$out/bad.log" 2>&1; then
    echo "FAIL: install.sh installed a tarball that does not match its checksum:"
    cat "$out/bad.log"; exit 1
fi
grep -q 'does not match its checksum' "$out/bad.log" || {
    echo "FAIL: install.sh failed, but not on the checksum:"
    cat "$out/bad.log"; exit 1; }
[ ! -e "$out/prefix-bad" ] || {
    echo "FAIL: a tarball that failed its checksum wrote into the prefix:"
    ls -R "$out/prefix-bad" | head -20; exit 1; }
#    Over an existing install, which stays exactly as it was.
ls -lR "$P" > "$out/before.txt"
if ( cd /tmp && EMBCC_INSTALL_BASE_URL=$bad sh "$INSTALL_SH" \
        --prefix "$P" ) > "$out/bad2.log" 2>&1; then
    echo "FAIL: install.sh replaced an install with a tarball that does not
match its checksum:"; cat "$out/bad2.log"; exit 1
fi
ls -lR "$P" > "$out/after.txt"
cmp -s "$out/before.txt" "$out/after.txt" || {
    echo "FAIL: a tarball that failed its checksum changed the install:"
    diff "$out/before.txt" "$out/after.txt" | head -20; exit 1; }
#    With no checksum file, the tarball is not installed unchecked.
rm "$bad/download/$tag/$asset.sha256"
cp "$rel/download/$tag/$asset" "$bad/download/$tag/$asset"
if ( cd /tmp && EMBCC_INSTALL_BASE_URL=$bad sh "$INSTALL_SH" \
        --prefix "$out/prefix-bad" ) > "$out/nosum.log" 2>&1; then
    echo "FAIL: install.sh installed a tarball with no checksum file:"
    cat "$out/nosum.log"; exit 1
fi
[ ! -e "$out/prefix-bad" ] || {
    echo "FAIL: a tarball with no checksum file wrote into the prefix"; exit 1; }
echo "a tarball that does not match its checksum, or has none, installs
nothing, and an existing install is left as it was"

# ---- 4. no tarball for the machine ---------------------------------------
if ( cd /tmp && EMBCC_INSTALL_BASE_URL=$rel \
        EMBCC_INSTALL_PLATFORM=linux-riscv64 sh "$INSTALL_SH" \
        --prefix "$out/prefix-src" ) > "$out/src.log" 2>&1; then
    echo "FAIL: install.sh succeeded with no tarball and no source:"
    cat "$out/src.log"; exit 1
fi
grep -q 'no binary release of EmbCC '"$tag"' for linux-riscv64; building from source instead' \
    "$out/src.log" &&
grep -q 'cannot download the source of '"$tag" "$out/src.log" || {
    echo "FAIL: with no tarball for the machine, install.sh did not fall
back to the source:"; cat "$out/src.log"; exit 1; }
[ ! -e "$out/prefix-src" ] || {
    echo "FAIL: a failed source build wrote into the prefix"; exit 1; }
echo "with no tarball for the machine, install.sh says so and builds from
source (here, a source that is missing: the build is not run)"

# The trees are 20 MB each; the logs stay.
rm -rf "${out:?}/stage" "${out:?}/release" "${out:?}/release-bad"
for s in $shells; do
    rm -rf "${out:?}/prefix-$s"
done
