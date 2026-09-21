#!/bin/sh
# EmbLinkOS as a BACKEND of our C library, not a second C library.
#
# The claim the seam makes is falsifiable, so test it as a claim: the same
# portable sources that build for the test harness build for EmbLinkOS with
# exactly one different file, that file supplies all eleven primitives, and
# nothing above it mentions the kernel. If any of those stops being true the
# library has grown an OS dependency and the next target pays for it.
#
# Opt-in: it needs EmbLinkOS's ABI headers. Absent, it skips -- the point is
# to catch the library drifting, not to make this repository depend on that
# one.
set -eu
echo "TEST-MARKER libc-emblinkos"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/libc-emblinkos
rm -rf "$out"; mkdir -p "$out"

[ "${ARCH:-x86_64}" = x86_64 ] ||
    { echo "skipped: EmbLinkOS's userland ABI here is x86-64"; exit 0; }

# ---- the compiler must be able to encode the trap ------------------------
# EmbLinkOS's user-side ABI is written on `syscall`. Without it the compiler
# the OS is built with cannot compile that OS's own backend, which is a
# funny enough failure to be worth its own check.
cat > "$out/sc.c" << 'EOF'
long trap(long n, long a)
{
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a)
                      : "rcx", "r11", "memory");
    return r;
}
EOF
"$EMBCC" -c "$out/sc.c" -o "$out/sc.o"
x86_64-elf-objdump -d "$out/sc.o" > "$out/sc.txt" 2>/dev/null ||
    { echo "skipped: no x86_64-elf-objdump"; exit 0; }
grep -q "0f 05.*syscall" "$out/sc.txt" ||
    { grep syscall "$out/sc.txt" || true
      echo "FAIL: inline asm 'syscall' did not encode as 0f 05"; exit 1; }
echo "inline asm 'syscall' encodes as 0f 05"

EMBLINKOS=${EMBLINKOS:-$HOME/EmbLinkOs}
[ -f "$EMBLINKOS/user/lib/embk.h" ] ||
    { echo "skipped: no $EMBLINKOS/user/lib/embk.h (set EMBLINKOS)"; exit 0; }

# ---- the flags the contract names, as this kernel numbers them -----------
# __os_open's flags belong to os/backend.h, and this backend passes them
# through untranslated because EmbLinkOS numbers them the same way. That is
# a fact about two files in two repositories, so check it rather than
# trusting it -- a silent divergence here means fopen("w") stops
# truncating, which no test of ours would otherwise notice.
cat > "$out/flags.c" << 'EOF'
#include <stdbool.h>
#include <embk.h>
#include <os/backend.h>
_Static_assert(__OS_O_RDONLY == EMBK_O_RDONLY, "O_RDONLY");
_Static_assert(__OS_O_WRONLY == EMBK_O_WRONLY, "O_WRONLY");
_Static_assert(__OS_O_RDWR   == EMBK_O_RDWR,   "O_RDWR");
_Static_assert(__OS_O_CREAT  == EMBK_O_CREAT,  "O_CREAT");
_Static_assert(__OS_O_EXCL   == EMBK_O_EXCL,   "O_EXCL");
_Static_assert(__OS_O_TRUNC  == EMBK_O_TRUNC,  "O_TRUNC");
_Static_assert(__OS_O_APPEND == EMBK_O_APPEND, "O_APPEND");
int main(void) { return 0; }
EOF
"$EMBCC" -fsyntax-only -Ilib/libc/include -Ilib/libc \
    -I"$EMBLINKOS/user/lib" "$out/flags.c" ||
    { echo "FAIL: os/backend.h and EmbLinkOS disagree about the open flags"
      exit 1; }
echo "the open flags in os/backend.h are the numbers this kernel uses"

# ---- the same library, one different file --------------------------------
back=lib/libc/os/emblinkos/backend.c
"$EMBCC" -c -O2 -Ilib/libc/include -Ilib/libc/src/math \
    -I"$EMBLINKOS/user/lib" "$back" -o "$out/backend.o" ||
    { echo "FAIL: the EmbLinkOS backend does not compile"; exit 1; }

# Every primitive the contract names, defined here and nowhere else.
for sym in __os_write __os_read __os_open __os_close __os_lseek __os_sbrk \
           __os_time __os_clock_ns __os_exit __os_isatty __os_getentropy; do
    x86_64-elf-nm "$out/backend.o" | grep -q " T $sym$" ||
        { echo "FAIL: $back does not define $sym"; exit 1; }
done
echo "the backend defines all eleven primitives of os/backend.h"

# ---- and the library above it does not know which OS it is on ------------
# The real test of a seam: compile the portable half against EmbLinkOS's
# headers and require that not one object references the kernel's ABI. A
# single #ifdef up there would show up here as an undefined embk_* symbol.
mkdir -p "$out/lib"
n=0
for f in lib/libc/src/*/*.c lib/libc/src/math/fdlibm/*.c; do
    o="$out/lib/$(echo "$f" | tr / _ | sed 's/\.c$/.o/')"
    "$EMBCC" -c -O2 -Ilib/libc/include -Ilib/libc/src/math \
        -I"$EMBLINKOS/user/lib" "$f" -o "$o" || {
        echo "FAIL: $f does not build for EmbLinkOS"; exit 1; }
    n=$((n + 1))
done
x86_64-elf-nm "$out"/lib/*.o | grep -E "U (embk_|EMBK_)" > "$out/leak.txt" &&
    { sort -u "$out/leak.txt" | head
      echo "FAIL: the portable library references EmbLinkOS's ABI"; exit 1; }
echo "$n portable objects build for EmbLinkOS, and not one of them
references the kernel ABI -- the OS is confined to the backend"

# ---- the whole archive links ---------------------------------------------
# What is genuinely external, which is not the same as what nm calls
# undefined: a member that calls another member reports it undefined, so
# the closure is (undefined - defined) across the whole archive.
x86_64-elf-ar rcs "$out/libc.a" "$out"/lib/*.o "$out/backend.o"
x86_64-elf-nm "$out/libc.a" > "$out/nm.txt"
awk '$1 == "U" { print $2 }' "$out/nm.txt" | sort -u > "$out/undef.txt"
awk 'NF == 3 && $2 != "U" { print $3 }' "$out/nm.txt" | sort -u \
    > "$out/def.txt"
comm -23 "$out/undef.txt" "$out/def.txt" > "$out/need.txt"
# __os_* is the seam and embk_* is this OS's ABI -- both expected. The rest
# is libgcc, named one by one rather than by a pattern so that a NEW
# external dependency cannot hide behind a wildcard: complex multiply and
# divide are calls on every compiler, not instructions.
grep -Ev "^(__os_|embk_)|^__(div|mul)[dsx]c3$" \
    "$out/need.txt" > "$out/extra.txt" || true
[ ! -s "$out/extra.txt" ] || {
    cat "$out/extra.txt"
    echo "FAIL: the library needs symbols nothing provides"; exit 1; }
echo "the archive is closed: it needs the backend, the kernel ABI and
libgcc's helpers, and nothing else"
