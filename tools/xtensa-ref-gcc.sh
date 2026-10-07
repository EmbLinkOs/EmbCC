#!/bin/sh
# Make the Xtensa reference compiler the goldens use: Espressif's GCC,
# generating for QEMU's de212 core rather than for an ESP32.
#
# Espressif's toolchain (crosstool-NG esp-16.1.0) selects its core at run
# time through a configuration plugin (-mdynconfig=xtensa_<core>.so); it
# ships the ESP32's, ESP32-S2's and ESP32-S3's. Those cores have MULUH and
# MULSH, which the de212 the tests run on does not, and GCC uses them for
# every 64-bit multiply. So the plugin is built for the de212 itself, from
# Espressif's xtensa-dynconfig and the de212 configuration overlay -- the
# same core description QEMU's de212 is generated from -- and GCC then
# emits only what the board executes.
#
# On macOS the toolchain is signed with Espressif's team identity and will
# not load a plugin signed otherwise, so the copy made here is re-signed
# ad hoc. Nothing outside DEST is touched.
#
#   usage: tools/xtensa-ref-gcc.sh [DEST]   (default ~/EmbRef/xtensa-de212-gcc-16.1.0)
set -eu
DEST=${1:-$HOME/EmbRef/xtensa-de212-gcc-16.1.0}
WORK=${TMPDIR:-/tmp}/xtensa-ref-gcc.$$
REL=esp-16.1.0_20260609
case "$(uname -s)-$(uname -m)" in
Darwin-arm64)  HOST=aarch64-apple-darwin ;;
Darwin-x86_64) HOST=x86_64-apple-darwin ;;
Linux-x86_64)  HOST=x86_64-linux-gnu ;;
Linux-aarch64) HOST=aarch64-linux-gnu ;;
*) echo "xtensa-ref-gcc: no Espressif build for this host" >&2; exit 1 ;;
esac
mkdir -p "$WORK" "$DEST"
curl -sfL -o "$WORK/tc.tar.xz" \
    "https://github.com/espressif/crosstool-NG/releases/download/$REL/xtensa-esp-elf-16.1.0_${REL#esp-16.1.0_}-$HOST.tar.xz"
tar -xJf "$WORK/tc.tar.xz" -C "$DEST"
curl -sfL -o "$WORK/de212.tar.gz" \
    https://raw.githubusercontent.com/jcmvbkbc/xtensa-toolchain-build/master/overlays/xtensa_de212.tar.gz
mkdir -p "$WORK/conf/xtensa_de212"
tar -xzf "$WORK/de212.tar.gz" -C "$WORK/conf/xtensa_de212"
git clone -q --depth 1 https://github.com/espressif/xtensa-dynconfig.git "$WORK/dynconfig"
make -s -C "$WORK/dynconfig" CONF_DIR="$WORK/conf" xtensa_de212.so
cp "$WORK/dynconfig/xtensa_de212.so" "$DEST/xtensa-esp-elf/lib/"
if [ "$(uname -s)" = Darwin ]; then
    find "$DEST/xtensa-esp-elf" -type f -perm -u+x | while read -r f; do
        if file "$f" | grep -q Mach-O; then codesign --force -s - "$f" 2>/dev/null; fi
    done
    codesign --force -s - "$DEST/xtensa-esp-elf/lib/xtensa_de212.so"
fi
rm -rf "$WORK"
"$DEST/xtensa-esp-elf/bin/xtensa-esp-elf-gcc" \
    -mdynconfig="$DEST/xtensa-esp-elf/lib/xtensa_de212.so" -dM -E -x c /dev/null |
    grep -q '__XCHAL_HAVE_MUL32_HIGH 0' || {
    echo "xtensa-ref-gcc: the plugin did not select the de212" >&2; exit 1; }
echo "xtensa-ref-gcc: $DEST/xtensa-esp-elf/bin/xtensa-esp-elf-gcc -mdynconfig=$DEST/xtensa-esp-elf/lib/xtensa_de212.so"
