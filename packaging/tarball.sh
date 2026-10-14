#!/bin/sh
# The release tarball of an installed EmbCC, and its SHA-256 file.
#
#   usage: packaging/tarball.sh DIR VERSION PLATFORM OUTDIR
#
# DIR is a prefix `make install` wrote into (DIR/bin/embcc,
# DIR/lib/embcc/VERSION, DIR/share). PLATFORM is OS-ARCH as install.sh
# names it: linux-x86_64, linux-aarch64, macos-x86_64 or macos-aarch64.
# This writes
#
#   OUTDIR/embcc-VERSION-PLATFORM.tar.gz         one top directory,
#                                                embcc-VERSION-PLATFORM/
#   OUTDIR/embcc-VERSION-PLATFORM.tar.gz.sha256  in sha256sum's format
#
# The tree inside works wherever it is unpacked, because embcc finds its
# headers and libraries relative to its own binary.
#
# .github/workflows/release.yml publishes what this writes, install.sh
# installs it, and tests/golden/install-script.sh builds one the same way.
set -eu
if [ $# -ne 4 ]; then
    echo "usage: $0 DIR VERSION PLATFORM OUTDIR" >&2
    exit 1
fi
dir=$1
version=$2
platform=$3
out=$4
[ -f "$dir/bin/embcc" ] || {
    echo "$0: $dir/bin/embcc does not exist; run make install first" >&2
    exit 1; }

name=embcc-$version-$platform
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/embcc-tarball.XXXXXX")
trap 'rm -rf "${work:?}"' EXIT
mkdir "$work/$name"
cp -RP "$dir/." "$work/$name/"

# The files are owned by root in the archive, so that root unpacking it by
# hand does not get files owned by the account that built it. On macOS,
# no AppleDouble ._ files and no extended attributes: GNU tar warns about
# both.
rm -f "$out/$name.tar.gz"
if tar --version 2> /dev/null | grep -q bsdtar; then
    (cd "$work" && COPYFILE_DISABLE=1 tar --uid 0 --gid 0 --no-xattrs \
        -czf "$out/$name.tar.gz" "$name")
else
    (cd "$work" && tar --owner=0 --group=0 --numeric-owner \
        -czf "$out/$name.tar.gz" "$name")
fi

if command -v sha256sum > /dev/null 2>&1; then
    (cd "$out" && sha256sum "$name.tar.gz") > "$out/$name.tar.gz.sha256"
else
    (cd "$out" && shasum -a 256 "$name.tar.gz") > "$out/$name.tar.gz.sha256"
fi
echo "packaged: $out/$name.tar.gz"
