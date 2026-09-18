#!/bin/sh
# Build the REFERENCE C++ toolchain EmbCC's C++ is tested against: g++ and a
# hosted libstdc++ for one target, built against the same newlib the test
# harness links (tools/hostpaths.sh). Homebrew's x86_64-elf-g++ and
# aarch64-elf-g++ are freestanding (--without-headers): they compile C++ but
# ship no libstdc++, so they can neither referee a program using the library
# nor supply the library EmbCC-compiled code links against while EmbCC grows
# into compiling it itself (DECISIONS D-013).
#
#   usage: tools/build-ref-gxx.sh x86_64-elf|aarch64-elf
#
# Installs into ~/cross/gcc-cxx-<target> (override: REF_GXX_PREFIX). The gcc
# version matches the installed cross gcc (16.2.0), so predefined macros and
# libstdc++'s expectations line up with the C side's reference. The same
# recipe as the OS's tools/cxx/build-gcc-cxx.sh, for both architectures.
set -eu
TARGET=${1:?usage: $0 x86_64-elf|aarch64-elf}
. "$(dirname "$0")/hostpaths.sh"
case "$TARGET" in
    x86_64-elf)  NEWLIB=$X86_NEWLIB ;;
    aarch64-elf) NEWLIB=$AARCH64_NEWLIB ;;
    *) echo "unknown target $TARGET" >&2; exit 2 ;;
esac
VER=16.2.0
SRCROOT=${REF_GXX_SRC:-$HOME/cross/src}
PREFIX=${REF_GXX_PREFIX:-$HOME/cross/gcc-cxx-$TARGET}
BREW=$(brew --prefix)

mkdir -p "$SRCROOT"
if [ ! -d "$SRCROOT/gcc-$VER" ]; then
    [ -f "$SRCROOT/gcc-$VER.tar.xz" ] ||
        curl -fL -o "$SRCROOT/gcc-$VER.tar.xz" \
            "https://ftp.gnu.org/gnu/gcc/gcc-$VER/gcc-$VER.tar.xz"
    tar -xJf "$SRCROOT/gcc-$VER.tar.xz" -C "$SRCROOT"
fi

# gcc looks for target headers and libraries in $PREFIX/$TARGET/: seed it
# with the newlib the harness links, so libstdc++ configures HOSTED on it.
mkdir -p "$PREFIX/$TARGET"
cp -R "$NEWLIB/include" "$PREFIX/$TARGET/"
cp -R "$NEWLIB/lib" "$PREFIX/$TARGET/"

BLD="$SRCROOT/build-gcc-cxx-$TARGET"
rm -rf "$BLD" && mkdir -p "$BLD" && cd "$BLD"
PATH="$BREW/bin:$PATH" "$SRCROOT/gcc-$VER/configure" \
    --target="$TARGET" --prefix="$PREFIX" --disable-nls \
    --enable-languages=c,c++ --with-newlib \
    --with-headers="$NEWLIB/include" \
    --enable-threads=single --disable-shared --disable-libssp \
    --disable-libquadmath --disable-libstdcxx-pch \
    --with-gmp="$BREW" --with-mpfr="$BREW" --with-mpc="$BREW" \
    --with-isl="$BREW" \
    --with-as="$BREW/bin/$TARGET-as" --with-ld="$BREW/bin/$TARGET-ld" \
    > configure.log 2>&1
echo "configured ($TARGET)"
J=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
make -j"$J" all-gcc > make-gcc.log 2>&1;                  echo "gcc built"
make -j"$J" all-target-libgcc > make-libgcc.log 2>&1;      echo "libgcc built"
make -j"$J" all-target-libstdc++-v3 > make-libstdcxx.log 2>&1
echo "libstdc++ built"
make install-gcc install-target-libgcc install-target-libstdc++-v3 \
    > install.log 2>&1
echo "installed: $PREFIX/bin/$TARGET-g++"
"$PREFIX/bin/$TARGET-g++" -print-file-name=libstdc++.a
