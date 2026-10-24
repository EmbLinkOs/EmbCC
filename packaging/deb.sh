#!/bin/sh
# A Debian package of an installed EmbCC, and its SHA-256 file.
#
#   usage: packaging/deb.sh DIR VERSION ARCH OUTDIR
#
# DIR is a prefix `make install` wrote into (DIR/bin/embcc, ...); its
# contents go under /usr. ARCH is Debian's name for the processor the
# programs in DIR run on: amd64 or arm64. This writes
#
#   OUTDIR/embcc_VERSION_ARCH.deb
#   OUTDIR/embcc_VERSION_ARCH.deb.sha256
#
# The package's own version is VERSION with its first '-' as '~' (1.1.0-rc1
# becomes 1.1.0~rc1), so that a pre-release sorts before the release it
# precedes. The file name keeps VERSION as it is: GitHub renames release
# files whose names have special characters, and the .sha256 file would
# then name a file that does not exist.
#
# The only dependency is the C library, at the newest symbol version any
# of the programs uses. Needs dpkg-deb, and objdump to read those
# versions. .github/workflows/release.yml runs it on Linux.
set -eu
if [ $# -ne 4 ]; then
    echo "usage: $0 DIR VERSION ARCH OUTDIR" >&2
    exit 1
fi
dir=$1
version=$2
arch=$3
out=$4
[ -f "$dir/bin/embcc" ] || {
    echo "$0: $dir/bin/embcc does not exist; run make install first" >&2
    exit 1; }
command -v dpkg-deb > /dev/null 2>&1 || {
    echo "$0: dpkg-deb is not installed" >&2; exit 1; }

debver=$(printf '%s\n' "$version" | sed 's/-/~/; s/-/./g')
mkdir -p "$out"
out=$(cd "$out" && pwd)
root=$(mktemp -d "${TMPDIR:-/tmp}/embcc-deb.XXXXXX")
trap 'rm -rf "${root:?}"' EXIT
mkdir -p "$root/DEBIAN" "$root/usr"
cp -RP "$dir/." "$root/usr/"

glibc=
if command -v objdump > /dev/null 2>&1; then
    glibc=$(find "$root/usr/bin" -type f -exec objdump -T {} + 2> /dev/null |
            sed -n 's/.*GLIBC_\([0-9][0-9.]*\).*/\1/p' |
            sort -t . -k 1,1n -k 2,2n -k 3,3n | tail -n 1)
fi
if [ -n "$glibc" ]; then
    depends="libc6 (>= $glibc)"
else
    depends=libc6
fi
# Debian's copyright file (DEP-5), beside the LICENSE and NOTICE that
# make install put in the same directory
mkdir -p "$root/usr/share/doc/embcc"
cat > "$root/usr/share/doc/embcc/copyright" << EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: EmbCC
Source: https://github.com/EmbLinkOs/EmbCC

Files: *
Copyright: 2026 Téo Motsou and contributors
License: Apache-2.0
 On Debian systems, the full text of the Apache License, Version 2.0 is in
 /usr/share/common-licenses/Apache-2.0, and in
 /usr/share/doc/embcc/LICENSE.

Files: lib/libc/src/math/fdlibm/*
Copyright: 1993 Sun Microsystems, Inc.
License: fdlibm
 Permission to use, copy, modify, and distribute this software is freely
 granted, provided that this notice is preserved.

Files: lib/libc/src/math/msun/catrig*.c lib/libc/src/math/msun/s_csqrt*.c
 lib/libc/src/math/msun/s_cexp*.c lib/libc/src/math/msun/k_exp*.c
 lib/libc/src/math/msun/s_ctanh*.c lib/libc/src/math/msun/s_cproj*.c
 lib/libc/src/math/msun/s_ccosh*.c lib/libc/src/math/msun/s_csinh*.c
Copyright: 2012 Stephen Montgomery-Smith
 2007-2011 David Schultz
 2005-2025 Bruce D. Evans and Steven G. Kargl
License: BSD-2-Clause
 Each file carries the licence text in full.

Files: lib/libc/src/math/msun/s_cpow*.c
Copyright: 2008 Stephen L. Moshier
License: ISC-like
 Permission to use, copy, modify, and distribute this software for any
 purpose with or without fee is hereby granted, provided that the above
 copyright notice and this permission notice appear in all copies.

Files: lib/libc/src/math/msun/s_erf*.c lib/libc/src/math/msun/e_lgamma*.c
 lib/libc/src/math/msun/k_sinf.c lib/libc/src/math/msun/k_cosf.c
 lib/libc/src/math/msun/k_tanf.c
Copyright: 1993, 2004 Sun Microsystems, Inc.
License: fdlibm
 Permission to use, copy, modify, and distribute this software is freely
 granted, provided that this notice is preserved.

Files: lib/libc/src/math/msun/s_*pi*.c lib/libc/src/math/msun/k_*pi.h
 lib/libc/src/math/msun/s_tgammaf.c
Copyright: 2017-2026 Steven G. Kargl
 2008 David Schultz
License: BSD-2-Clause
 Each file carries the licence text in full.

Files: lib/libc/src/math/msun/bsdsrc/b_*.c
Copyright: 1985-1993 The Regents of the University of California
License: BSD-3-Clause
 Each file carries the licence text in full.

Files: tools/embsim/avr.c tools/embsim/riscv.c
Copyright: 2018-2019 Free Software Foundation, Inc.
License: FSFAP
 Copying and distribution of this file, with or without modification, are
 permitted in any medium without royalty provided the copyright notice and
 this notice are preserved.
EOF
size=$(du -sk "$root/usr" | cut -f 1)

cat > "$root/DEBIAN/control" << EOF
Package: embcc
Version: $debver
Architecture: $arch
Maintainer: ${EMBCC_DEB_MAINTAINER:-EmbLinkOs <https://github.com/EmbLinkOs/EmbCC>}
Installed-Size: $size
Depends: $depends
Section: devel
Priority: optional
Homepage: https://github.com/EmbLinkOs/EmbCC
Description: C and C++ compiler and toolchain for embedded systems
 EmbCC compiles C and C++ for Arm Cortex-M and Cortex-A, RISC-V, AVR,
 Xtensa, TriCore, RX, ColdFire, MIPS, PowerPC, SPARC and LoongArch, and
 for x86-64 and AArch64 operating systems. One embcc binary targets every
 one of them. The package also holds the assembler, the linker, the
 debugger, the simulator, the firmware tools, and the headers and
 libraries for each target.
EOF

deb=embcc_${version}_$arch.deb
rm -f "$out/$deb"
# xz rather than dpkg-deb's default, which on Ubuntu is zstd and which
# older dpkg cannot read.
dpkg-deb -Zxz --root-owner-group --build "$root" "$out/$deb"
(cd "$out" && sha256sum "$deb") > "$out/$deb.sha256"
echo "packaged: $out/$deb"
