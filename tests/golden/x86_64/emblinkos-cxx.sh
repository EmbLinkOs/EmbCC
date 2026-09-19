#!/bin/sh
# CX8's acceptance, on the metal: the OS's own C++ program
# (user/tests/cxxdemo/cxxdemo.cc, <iostream> and all) compiled by EmbCC and
# linked with the libstdc++ EmbCC built from GCC's sources — no g++ anywhere
# in the program or the library — staged onto an EmbLinkOS image and RUN by
# the kernel's own `test cxx` oracle, which spawns it and reads its exit
# code. It self-checks what breaks on a fresh C++ port: global constructors
# and their order, new/delete and new[]/delete[], templates, a
# function-local static (libsupc++'s guards), std::string, std::vector,
# iostream (the library's own ios_base::Init constructor), destructors at
# exit.
#
# STAGED_APPS is the OS's sanctioned way in for a binary built outside its
# tree, so nothing in the OS is modified. Opt-in: it rebuilds the image and
# boots QEMU (~2 min), and only one QEMU may run at a time.
set -u
echo "TEST-MARKER emblinkos-cxx"
[ "${EMBCC_OS_CXX:-}" = 1 ] || {
    echo "skipped: booting the OS is opt-in (set EMBCC_OS_CXX=1)"; exit 0; }
. "$(dirname "$0")/../../lib.sh"

HOST=$(cd "$(dirname "$0")/../../.." && pwd)
OS=$MYOS
DEMO=$OS/user/tests/cxxdemo/cxxdemo.cc
LIB=${EMBCC_LIBSTDCXX_LIB:-$HOST/build/libstdcxx-x86_64-elf}/ref/x86_64-elf/lib
REF=$X86_REF_GXX
[ -f "$DEMO" ] || { echo "skipped: no EmbLinkOS tree at $OS"; exit 0; }
[ -f "$OS/build/crt0.o" ] && [ -f "$OS/build/syscalls.o" ] || {
    echo "skipped: the OS has not been built (build/crt0.o, build/syscalls.o)"
    exit 0; }
[ -f "$LIB/libstdc++.a" ] || {
    echo "skipped: no EmbCC-built libstdc++ ($LIB) — make test-libstdcxx"
    exit 0; }
ver=$(ls "$REF/x86_64-elf/include/c++" 2>/dev/null | head -1)
[ -n "$ver" ] || { echo "skipped: no libstdc++ headers at $REF"; exit 0; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "skipped: qemu absent"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "skipped: python3 absent"; exit 0; }
INC="$REF/x86_64-elf/include/c++/$ver"

out=$HOST/tests/golden/out/emblinkos-cxx
rm -rf "$out"; mkdir -p "$out"

# 1. EmbCC compiles it, with the flags the OS's own C++ rule uses.
"$EMBCC" --target=x86_64-elf -mno-red-zone -fno-stack-protector -O2 -Wall \
    -fno-exceptions -fno-rtti -I"$INC" -I"$INC/x86_64-elf" -I"$OS/user/lib" \
    -isystem "$X86_NEWLIB/include" -c "$DEMO" -o "$out/cxxdemo.o" || {
    echo "embcc did not compile cxxdemo.cc"; exit 1; }
echo "compiled cxxdemo.cc ($(wc -c < "$out/cxxdemo.o") bytes of object)"

# 2. Linked as the OS links its apps (crt0 + syscalls + newlib.ld), with
#    EmbCC's libstdc++ and libsupc++.
( cd "$OS" && x86_64-elf-gcc -nostartfiles -static -T user/lib/newlib.ld \
    -L user/lib -L"$X86_NEWLIB/lib" build/crt0.o build/syscalls.o \
    "$out/cxxdemo.o" -L"$LIB" -lstdc++ -lsupc++ -lc -lgcc \
    -o "$out/cxxdemo.elf" ) || { echo "link failed"; exit 1; }
cp "$out/cxxdemo.elf" "$OS/build/cxxdemo.elf"
echo "linked with EmbCC's libstdc++ ($(wc -c < "$out/cxxdemo.elf") bytes)"

# 3. The OS packs it (STAGED_APPS: built outside its tree) and boots.
( cd "$OS" && make STAGED_APPS=build/cxxdemo.elf embkfs.img ) \
    > "$out/mkimage.log" 2>&1 || {
    echo "the OS image did not build:"; tail -20 "$out/mkimage.log"; exit 1; }
grep -q "data/apps/cxxdemo/cxxdemo.elf" "$out/mkimage.log" || {
    echo "cxxdemo.elf did not reach the image"; exit 1; }
( cd "$OS" && python3 tools/console_test.py "test cxx" ) > "$out/boot.log" 2>&1
rc=$?
grep -q "cxxdemo: all green" "$out/boot.log" || {
    echo "cxxdemo did not report all green on the OS:"
    grep -E "cxxdemo|test cxx" "$out/boot.log" | tail -20; exit 1; }
grep -q "\[cmd\] test cxx: exit=0 -> OK" "$out/boot.log" || {
    echo "the kernel's verdict was not OK:"
    grep -E "test cxx" "$out/boot.log" | tail -5; exit 1; }
[ "$rc" -eq 0 ] || { echo "console_test exited $rc"; exit 1; }
echo "EmbLinkOS ran it: $(grep -c '^  ok ' "$out/boot.log") checks ok, iostream"
echo "included, exit 0 — the program and libstdc++ both EmbCC's (CX8)"
