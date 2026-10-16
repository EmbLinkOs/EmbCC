#!/bin/sh
# Points the package recipes at a release.
#
#   usage: packaging/set-version.sh TAG [SHA256]
#
# Sets the version, and the SHA-256 of GitHub's source tarball for TAG
# (https://github.com/EmbLinkOs/EmbCC/archive/refs/tags/TAG.tar.gz), in
# packaging/homebrew/embcc.rb, packaging/rpm/embcc.spec and
# packaging/arch/PKGBUILD. Without SHA256 it downloads the tarball and
# computes it, so the tag has to be pushed first.
#
# Each format spells a pre-release its own way, so that it sorts before
# the release it precedes; for v1.1.0-rc1:
#
#   Homebrew  the tag itself, in the URL
#   RPM       Version: 1.1.0~rc1
#   Arch      pkgver=1.1.0rc1
set -eu
if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "usage: $0 TAG [SHA256]" >&2
    exit 1
fi
tag=$1
case $tag in
    v[0-9]*) ;;
    *) echo "$0: '$tag' is not a release tag (vX.Y.Z)" >&2; exit 1 ;;
esac
case $tag in
    *[!A-Za-z0-9._+-]*)
        echo "$0: '$tag' is not a release tag (vX.Y.Z)" >&2; exit 1 ;;
esac
version=${tag#v}
here=$(cd "$(dirname "$0")" && pwd)

ours=$(sed -n 's/.*EMBCC_VERSION "\(.*\)".*/\1/p' \
       "$here/../src/driver/version.h" 2> /dev/null || true)
if [ -n "$ours" ] && [ "$ours" != "$version" ]; then
    echo "$0: note: src/driver/version.h says $ours, not $version" >&2
fi

if [ $# -eq 2 ]; then
    sha=$2
else
    url=https://github.com/EmbLinkOs/EmbCC/archive/refs/tags/$tag.tar.gz
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/embcc-set-version.XXXXXX")
    trap 'rm -rf "${tmp:?}"' EXIT
    echo "downloading $url"
    curl -fsSL -o "$tmp/source.tar.gz" "$url"
    if command -v sha256sum > /dev/null 2>&1; then
        sha=$(sha256sum "$tmp/source.tar.gz" | cut -d ' ' -f 1)
    else
        sha=$(shasum -a 256 "$tmp/source.tar.gz" | cut -d ' ' -f 1)
    fi
fi
case $sha in
    *[!0-9a-f]* | '') echo "$0: '$sha' is not a SHA-256" >&2; exit 1 ;;
esac
[ "${#sha}" -eq 64 ] || { echo "$0: '$sha' is not a SHA-256" >&2; exit 1; }

rpmver=$(printf '%s\n' "$version" | sed 's/-/~/; s/-/./g')
archver=$(printf '%s\n' "$version" | sed 's/-//; s/-/./g')

# rewrite FILE SED-SCRIPT: edit FILE in place, the same way on every sed.
rewrite() {
    sed "$2" "$1" > "$1.new"
    if cmp -s "$1" "$1.new"; then
        rm -f "$1.new"
        echo "unchanged: $1"
    else
        mv "$1.new" "$1"
        echo "updated: $1"
    fi
}

rewrite "$here/homebrew/embcc.rb" "
s|^\(  url \"https://github.com/EmbLinkOs/EmbCC/archive/refs/tags/\)[^\"]*\(\.tar\.gz\"\)|\1$tag\2|
s|^\(  sha256 \"\)[0-9a-f]*\(\"\)|\1$sha\2|"

rewrite "$here/rpm/embcc.spec" "
s|^\(%global upstream_version \).*|\1$version|
s|^\(Version: *\).*|\1$rpmver|
s|^\(Release: *\)[0-9]*|\11|"

rewrite "$here/arch/PKGBUILD" "
s|^_tag=.*|_tag=$tag|
s|^pkgver=.*|pkgver=$archver|
s|^pkgrel=.*|pkgrel=1|
s|^sha256sums=.*|sha256sums=('$sha')|"
