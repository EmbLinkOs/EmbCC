#!/bin/sh
# EmbCC on a host with nothing but a C library (docs/internals/porting.md).
#
# `make PLATFORM=iso` builds the compiler against src/platform/platform_iso.c,
# which asks the host nothing ISO C cannot -- no <unistd.h>, no /proc, no
# _NSGetExecutablePath. That is what lets a hobby or non-POSIX operating
# system run EmbCC with only its own C library. Checked three ways: the
# binary references no POSIX call, it compiles exactly what the POSIX build
# compiles, and it finds itself from argv[0].
set -u
echo "TEST-MARKER host-iso"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=$PWD/tests/golden/out/host-iso-$ARCH
rm -rf "${out:?}"; mkdir -p "$out"

make PLATFORM=iso BUILD="$out/obj" "$out/obj/embcc" > "$out/build.log" 2>&1 || {
    echo "FAIL: the compiler does not build with PLATFORM=iso:"
    tail -5 "$out/build.log"; exit 1; }
E="$out/obj/embcc"

# 1. Nothing POSIX: the host layer's own calls and the ones that tend to
#    creep in. (A whitelist of ISO names would trip on what each host's
#    compiler adds -- stack protectors, fortify wrappers.)
nm -u "$E" | sed 's/^ *U *//; s/^_//' > "$out/undef.txt"
for f in isatty readlink realpath NSGetExecutablePath strncasecmp \
         strcasecmp fork execv execvp popen fileno getcwd stat lstat \
         open close read write unlink mkstemp; do
    if grep -qx "$f" "$out/undef.txt" || grep -qx "_$f" "$out/undef.txt"; then
        echo "FAIL: the PLATFORM=iso compiler calls $f, which ISO C does not have"
        exit 1
    fi
done
echo "the PLATFORM=iso compiler calls nothing outside ISO C"

# 2. The host layer changes nothing the compiler produces.
cat > "$out/f.c" <<'CEOF'
static int tab[4] = { 1, 2, 3, 5 };
int f(int i) { return tab[i & 3] * 7 + i; }
CEOF
"$EMBCC" --target=x86_64-elf -O2 -c "$out/f.c" -o "$out/posix.o" &&
"$E" --target=x86_64-elf -O2 -c "$out/f.c" -o "$out/iso.o" || {
    echo "FAIL: a compile failed"; exit 1; }
cmp -s "$out/posix.o" "$out/iso.o" || {
    echo "FAIL: the PLATFORM=iso compiler emits different bytes"; exit 1; }
echo "and emits the same bytes as the POSIX build"

# 3. With no OS answer, where the program is comes from argv[0].
"$E" --print-search-dirs > "$out/dirs.txt" 2>&1
grep -q "^self: $E\$" "$out/dirs.txt" || {
    echo "FAIL: the PLATFORM=iso compiler did not take its path from argv[0]:"
    cat "$out/dirs.txt"; exit 1; }
echo "and finds itself from argv[0]"
