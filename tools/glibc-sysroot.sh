#!/bin/sh
# glibc-sysroot.sh ARCH DIR -- a Linux system's root for --libc=system,
# made from Debian 12's glibc packages: the headers and start files
# (libc6-dev), the kernel's headers (linux-libc-dev) and the shared
# objects a program loads (libc6). ARCH is amd64 or arm64.
#
#   tools/glibc-sysroot.sh amd64 ~/EmbRef/dl/sysroot-debian12-amd64
#
# then  embcc --target=x86_64-linux-gnu --libc=system --sysroot=DIR ...
#
# A distribution's packages make absolute symlinks (/lib64/ld-linux-
# x86-64.so.2 -> /lib/x86_64-linux-gnu/...), right on the system and wrong
# anywhere else; they are made relative, so DIR works where it is. This is
# what tests/golden/libc-system.sh uses on a host that is not that Linux
# (tools/hostpaths.sh, GLIBC_SYSROOT_X86_64 and _AARCH64).
set -eu
[ $# -eq 2 ] || { echo "usage: glibc-sysroot.sh amd64|arm64 DIR" >&2; exit 2; }
arch=$1; dir=$2
case $arch in amd64|arm64) ;; *) echo "glibc-sysroot.sh: amd64 or arm64" >&2; exit 2 ;; esac
GLIBC=${GLIBC_DEB_VERSION:-2.36-9+deb12u14}
LINUX=${LINUX_DEB_VERSION:-6.1.176-1}
mkdir -p "$dir"
dl=$(mktemp -d "${TMPDIR:-/tmp}/glibc-sysroot.XXXXXX")
trap 'rm -rf "${dl:?}"' EXIT
fetch() {               # fetch POOL-PATH: from the archive, else security
    f=$(basename "$1")
    curl -sfL -o "$dl/$f" "https://deb.debian.org/debian/pool/main/$1" ||
    curl -sfL -o "$dl/$f" \
        "https://security.debian.org/debian-security/pool/updates/main/$1" ||
    { echo "glibc-sysroot.sh: cannot download $f" >&2; exit 1; }
    mkdir -p "$dl/x-$f"
    (cd "$dl/x-$f" && ar x "../$f")
    tar -xf "$dl/x-$f"/data.tar.* -C "$dir"
}
fetch "g/glibc/libc6_${GLIBC}_$arch.deb"
fetch "g/glibc/libc6-dev_${GLIBC}_$arch.deb"
fetch "l/linux/linux-libc-dev_${LINUX}_$arch.deb"
# absolute symlinks made relative to where they are
(cd "$dir" && find . -type l | while read -r l; do
    t=$(readlink "$l")
    case $t in
        /*) d=$(dirname "$l"); up=$(echo "$d" | sed 's|^\./||; s|[^/][^/]*|..|g')
            [ "$d" = . ] && up=.
            ln -sfn "$up${t}" "$l" ;;
    esac
done)
echo "glibc-sysroot.sh: $dir has glibc $GLIBC for $arch"
