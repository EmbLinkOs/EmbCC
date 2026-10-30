#!/bin/sh
# Every backend of lib/libc defines every primitive os/backend.h names.
#
# A primitive a backend leaves out is not a missing feature, it is a
# program that fails to LINK on that target the first time it calls the
# function above it -- and nothing else would notice, since each backend
# is built only for its own targets. So the list is read out of the
# contract itself, every backend is compiled, and its object must define
# all of them: really, or as os/absent.inc's ENOSYS, but defined.
#
# The Linux backend is three files (backend.c, posix.c, thread.c), so its
# definitions are the union; EmbLinkOS's needs that OS's headers
# (EMBLINKOS=...) and is skipped without them.
set -u
echo "TEST-MARKER libc-backends"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/libc-backends
rm -rf "${out:?}"; mkdir -p "$out"
NM=${EMBCC_NM:-llvm-nm}
command -v "$NM" > /dev/null 2>&1 || NM=x86_64-elf-nm
command -v "$NM" > /dev/null 2>&1 || { echo "skipped: no nm"; exit 0; }

L=$EMBCC_ROOT/lib/libc
sed -n 's/^[a-z][a-z ]*\**\(__os_[a-z0-9_]*\)(.*/\1/p' "$L/os/backend.h" |
    sort -u > "$out/contract.txt"
n=$(wc -l < "$out/contract.txt" | tr -d ' ')
[ "$n" -gt 100 ] || { echo "FAIL: read only $n primitives out of os/backend.h"; exit 1; }

fail=0
check() {         # check NAME TRIPLE EXTRA-FLAGS FILE...
    name=$1 triple=$2 flags=$3; shift 3
    : > "$out/$name.def"
    for f in "$@"; do
        o=$out/$name-$(basename "$f" .c).o
        "$EMBCC" --target="$triple" -c -O1 -I"$L/include" -I"$L/src/math" \
            $flags "$f" -o "$o" > "$out/$name.cc" 2>&1 ||
            { echo "FAIL: $f does not compile for $triple"; head -3 "$out/$name.cc"
              fail=1; return; }
        "$NM" --defined-only "$o" | awk 'NF == 3 { print $3 }' >> "$out/$name.def"
    done
    sort -u "$out/$name.def" -o "$out/$name.def"
    comm -23 "$out/contract.txt" "$out/$name.def" > "$out/$name.missing"
    if [ -s "$out/$name.missing" ]; then
        echo "FAIL: the $name backend does not define:"
        sed 's/^/    /' "$out/$name.missing" | head -20
        fail=1
    else
        echo "$name ($triple): all $n primitives defined"
    fi
}

check baremetal thumbv7m-none-eabi "" "$L/os/baremetal/backend.c"
check baremetal-avr avr-none-elf "" "$L/os/baremetal/backend.c"
check posixlike x86_64-elf "" "$L/os/posixlike/backend.c"
for a in x86_64 aarch64; do
    check linux-$a $a-linux-gnu "" "$L/os/linux/backend.c" \
        "$L/os/linux/posix.c" "$L/os/linux/thread.c"
done
EMBLINKOS=${EMBLINKOS:-$HOME/EmbLinkOs}
if [ -f "$EMBLINKOS/user/lib/embk.h" ]; then
    check emblinkos x86_64-elf "-I$EMBLINKOS/user/lib" "$L/os/emblinkos/backend.c"
else
    echo "emblinkos: skipped (no $EMBLINKOS/user/lib/embk.h; set EMBLINKOS)"
fi
# The C++ library reaches the same primitives from its own headers, which
# cannot include os/backend.h: its copies must be the contract's, or a
# backend writes past a structure C++ allocated (a shorter __os_fileinfo
# in <filesystem> once hung every status()). Fields and prototypes,
# comments and spacing aside.
body() {          # body FILE STRUCT -> the struct's fields, one per line
    sed -n "/^struct $2 {/,/^};/p" "$1" | sed 's|/\*.*\*/||; s|^ *\*.*||; s|/\*.*||' |
        tr -s ' ' | sed 's/^ //; s/ $//' | grep -v '^$'
}
for st in __os_fileinfo; do
    for h in "$EMBCC_ROOT"/lib/libcxx/include/*; do
        grep -q "^struct $st {" "$h" || continue
        if [ "$(body "$L/os/backend.h" $st)" != "$(body "$h" $st)" ]; then
            echo "FAIL: struct $st in lib/libcxx/include/$(basename "$h") is not os/backend.h's"
            fail=1
        fi
    done
done
grep -h "^[a-z][^{=]*[ *]__os_[a-z_]*(.*);" "$EMBCC_ROOT"/lib/libcxx/include/* \
        "$EMBCC_ROOT"/lib/libcxx/src/*.cc | tr -s ' ' | sort -u |
while read -r line; do
    n=$(echo "$line" | grep -o "__os_[a-z_]*" | head -1)
    want=$(grep -h "^[a-z].*[ *]$n(" "$L/os/backend.h" | tr -s ' ' | head -1)
    [ "$line" = "$want" ] || { echo "FAIL: libcxx declares [$line], the contract [$want]"; exit 1; }
done || fail=1
[ $fail = 0 ] && echo "lib/libcxx: its copies of the contract are the contract's"
[ $fail = 0 ] || exit 1
