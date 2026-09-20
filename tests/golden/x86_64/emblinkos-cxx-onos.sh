#!/bin/sh
# CX9: C++ compiled ON EmbLinkOS. The toolchain that does it is EmbCC's own
# and runs on the metal — embcc.elf (EmbCC's output, EmbLD-linked) compiles a
# C++ program the kernel writes to /data/tmp, embld.elf links it against the
# sealed ABI (/system/abi: crt0.o, syscalls.o, libc.a), and the kernel runs
# the result and reads its exit code. No host compiler, no tcc, no gcc in the
# loop: `test embcc cxx` is the kernel's own oracle for it.
#
# The program is self-contained C++ (it defines operator new/delete, a local
# static's guard and __cxa_pure_virtual), so nothing but the C ABI has to be
# on the image: what is judged is the compiler, not a C++ library. It uses a
# global constructor, virtual dispatch through a base, templates over two
# types, new/delete and new[]/delete[], a guarded function-local static and
# recursion; it exits 42.
#
# Opt-in: it cross-builds two OS binaries, rebuilds the image and boots QEMU
# (~3 min), and only one QEMU may run at a time.
set -u
echo "TEST-MARKER emblinkos-cxx-onos"
[ "${EMBCC_OS_CXX:-}" = 1 ] || {
    echo "skipped: booting the OS is opt-in (set EMBCC_OS_CXX=1)"; exit 0; }
. "$(dirname "$0")/../../lib.sh"

HOST=$(cd "$(dirname "$0")/../../.." && pwd)
OS=$MYOS
[ -d "$OS/kernel" ] || { echo "skipped: no EmbLinkOS tree at $OS"; exit 0; }
[ -f "$OS/build/crt0.o" ] && [ -f "$OS/build/syscalls.o" ] || {
    echo "skipped: the OS has not been built (build/crt0.o, build/syscalls.o)"
    exit 0; }
grep -q '"test embcc cxx"' "$OS/kernel/selftests.c" 2>/dev/null || {
    echo "skipped: this OS kernel has no \`test embcc cxx\` oracle"; exit 0; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "skipped: qemu absent"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "skipped: python3 absent"; exit 0; }

out=$HOST/tests/golden/out/emblinkos-cxx-onos
rm -rf "$out"; mkdir -p "$out"

# 1. The compiler for the metal: EmbCC compiles its own sources, EmbLD links
#    them into embcc.elf (docs/developer/selfhost-on-os.md), and EmbLD itself is
#    cross-built into embld.elf.
( cd "$HOST" && MYOS="$OS" tools/gen-selfhost-ref.sh ) > "$out/stage.log" 2>&1 || {
    echo "gen-selfhost-ref.sh failed:"; tail -10 "$out/stage.log"; exit 1; }
( cd "$HOST" && MYOS="$OS" tools/os-build-embld.sh ) >> "$out/stage.log" 2>&1 || {
    echo "os-build-embld.sh failed:"; tail -10 "$out/stage.log"; exit 1; }
echo "staged embcc.elf ($(wc -c < "$OS/build/embcc.elf" | tr -d ' ') bytes)" \
     "and embld.elf ($(wc -c < "$OS/build/embld.elf" | tr -d ' ') bytes)"

# 2. The image carries them (STAGED_APPS: built outside the OS tree) plus
#    EmbCC's headers, which its C++ front end includes from /data/apps/embcc.
staged="build/embcc.elf build/embld.elf"
[ -f "$OS/build/cxxdemo.elf" ] && staged="$staged build/cxxdemo.elf"
( cd "$OS" && EMBK_EMBCC_ROOT="$HOST" make STAGED_APPS="$staged" embkfs.img ) \
    > "$out/mkimage.log" 2>&1 || {
    echo "the OS image did not build:"; tail -20 "$out/mkimage.log"; exit 1; }

# 3. Boot, and let the kernel judge.
( cd "$OS" && python3 tools/console_test.py "test embcc cxx" ) > "$out/boot.log" 2>&1
rc=$?
grep -q "\[cmd\] test embcc cxx: OK" "$out/boot.log" || {
    echo "the kernel's verdict was not OK:"
    grep -E "\[embcc c\+\+\]|test embcc cxx" "$out/boot.log" | tail -10; exit 1; }
[ "$rc" -eq 0 ] || { echo "console_test exited $rc"; exit 1; }
grep -E "\[embcc c\+\+\]" "$out/boot.log" | sed 's/^/  /'
echo "EmbCC compiled C++ on EmbLinkOS, EmbLD linked it, it ran: exit 42 (CX9)"
