#!/bin/sh
# The Mach-O object writer (D-014), judged by the platform's own tools.
#
# A format writer is one of the few things that can be checked against an
# absolute authority rather than against itself: the SDK declares the
# structures, and ld either accepts the file or does not. Both are used
# here, and nothing in this file trusts our own header.
#
# macOS only, and skipped elsewhere -- it needs the SDK to compare
# against and the system linker to be judged by.
set -eu
echo "TEST-MARKER macho"
. "$(dirname "$0")/../lib.sh"

case "$(uname -s)" in
    Darwin) ;;
    *) echo "skipped: the Mach-O writer is checked against the macOS SDK"
       exit 0 ;;
esac

out=$EMBCC_ROOT/tests/golden/out/macho-$ARCH
rm -rf "$out"; mkdir -p "$out"
D=$EMBCC_ROOT/tests/golden/darwin
SRC="$EMBCC_ROOT/src"

# 1. Our header against the SDK's. Every constant, every struct size,
#    every offset the writer depends on. A format is a contract with
#    someone else's parser, so the numbers are not ours to decide.
cc -I"$SRC/macho" -o "$out/machochk" "$D/machochk.c" 2> "$out/chk.log" || {
    echo "FAIL: the SDK conformance check did not build:"
    cat "$out/chk.log"; exit 1; }
"$out/machochk" > "$out/chk.txt" || {
    echo "FAIL: our Mach-O header disagrees with the SDK:"
    cat "$out/chk.txt"; exit 1; }
cat "$out/chk.txt"

# 2. A minimal object, written by the writer and read by the system.
cc -o "$out/machogen" "$D/machogen.c" "$SRC/macho/write.c" \
    "$SRC/driver/util.c" "$SRC/driver/diag.c" \
    "$SRC/platform/platform_posix.c" 2> "$out/gen.log" || {
    echo "FAIL: the generator did not build:"; cat "$out/gen.log"; exit 1; }

host=$(uname -m)
case "$host" in
    arm64|aarch64) cpu=arm64 ;;
    *)             cpu=x86_64 ;;
esac

"$out/machogen" "$cpu" "$out/t.o" || {
    echo "FAIL: the writer reported failure"; exit 1; }

# file(1) parses the header independently of otool.
file "$out/t.o" | grep -q "Mach-O 64-bit object $cpu" || {
    echo "FAIL: not recognised as a Mach-O object:"; file "$out/t.o"; exit 1; }

# otool reads the load commands: the three we emit, and no more.
otool -l "$out/t.o" > "$out/lc.txt" 2>&1
for c in LC_SEGMENT_64 LC_SYMTAB LC_BUILD_VERSION; do
    grep -q "$c" "$out/lc.txt" || {
        echo "FAIL: $c missing from the load commands:"
        head -30 "$out/lc.txt"; exit 1; }
done

# nm reads the symbol table, and proves the platform's leading
# underscore was applied: the C name is `main`, the symbol is `_main`.
nm "$out/t.o" > "$out/nm.txt" 2>&1
grep -q '^0*0 T _main$' "$out/nm.txt" || {
    echo "FAIL: _main is not a defined text symbol:"; cat "$out/nm.txt"; exit 1; }
echo "file, otool and nm all read it; the symbol carries the platform's underscore"

# 3. The judgement that matters: the system linker accepts it, and the
#    program runs. A writer whose output only our own tools like has
#    proved nothing.
cc -o "$out/prog" "$out/t.o" 2> "$out/link.log" || {
    echo "FAIL: the system linker refused our object:"
    cat "$out/link.log"; exit 1; }
set +e
"$out/prog"
rc=$?
set -e
[ "$rc" = 42 ] || { echo "FAIL: the linked program exited $rc, wanted 42"; exit 1; }
echo "the system linker accepts it and the program runs, exiting 42"

# 4. The whole point: REAL C, compiled by embcc, linked by the system,
#    calling into libSystem. Everything before this proves the
#    container; this proves the compiler can fill it.
EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
triple=$([ "$cpu" = arm64 ] && echo aarch64-apple-darwin \
                            || echo x86_64-apple-darwin)
cat > "$out/real.c" << 'EOF'
int puts(const char *);
int printf(const char *, ...);
static int counter = 7;
int table[4] = { 10, 20, 30, 40 };
const char *msg = "a string through __const";
static int add(int a, int b) { return a + b; }
int main(void)
{
    puts("compiled by EmbCC, linked by the system");
    printf("%s c=%d t2=%d sum=%d\n", msg, counter, table[2], add(40, 2));
    return table[0] + counter;
}
EOF
"$EMBCC" --target="$triple" -c "$out/real.c" -o "$out/real.o" 2> "$out/cc.log" || {
    echo "FAIL: embcc could not compile for $triple:"; cat "$out/cc.log"; exit 1; }
cc -o "$out/real" "$out/real.o" 2> "$out/rlink.log" || {
    echo "FAIL: the system linker refused embcc's object:"
    cat "$out/rlink.log"; exit 1; }
set +e
"$out/real" > "$out/real.txt" 2>&1
rc=$?
set -e
[ "$rc" = 17 ] || { echo "FAIL: the program exited $rc, wanted 17"
                    cat "$out/real.txt"; exit 1; }
grep -qx 'compiled by EmbCC, linked by the system' "$out/real.txt" || {
    echo "FAIL: wrong output:"; cat "$out/real.txt"; exit 1; }
#    Every value here goes through a different path: the string through
#    a __const anchor symbol, the two globals through __data symbols,
#    the sum through a local call -- and all four are VARIADIC
#    arguments, which Darwin passes on the stack where AAPCS64 would
#    use registers.
grep -qx 'a string through __const c=7 t2=30 sum=42' "$out/real.txt" || {
    echo "FAIL: wrong values -- check the Darwin variadic rule:"
    cat "$out/real.txt"; exit 1; }
echo "real C: strings, globals, a local call and Darwin's stack-passed
varargs all correct, linked against libSystem"

# 5. Darwin's va_arg. Its variadic arguments arrive on the stack, so
#    the va_list is a plain pointer walking them -- no register save
#    area, no 32-byte record. Defining a variadic function was refused
#    until this worked, because the AAPCS64 walk would have read
#    registers no caller fills.
#
#    Every type goes through a different path: an int takes eight
#    bytes, a double arrives promoted, a pointer is read as itself, and
#    the last line nests all three as arguments to a fourth.
cat > "$out/va.c" << 'EOF'
#include <stdarg.h>
int printf(const char *, ...);
static int isum(int n, ...)
{
    va_list a; va_start(a, n);
    int t = 0; while (n--) t += va_arg(a, int);
    va_end(a); return t;
}
static double dsum(int n, ...)
{
    va_list a; va_start(a, n);
    double t = 0; while (n--) t += va_arg(a, double);
    va_end(a); return t;
}
static const char *last(int n, ...)
{
    va_list a; va_start(a, n);
    const char *s = 0; while (n--) s = va_arg(a, const char *);
    va_end(a); return s;
}
int main(void)
{
    printf("i=%d d=%.2f s=%s\n", isum(5, 1, 2, 3, 4, 5),
           dsum(3, 1.5, 2.25, 0.25), last(3, "a", "b", "z"));
    return isum(3, 10, 20, 12);
}
EOF
"$EMBCC" --target="$triple" -c "$out/va.c" -o "$out/va.o" 2> "$out/va.log" || {
    echo "FAIL: embcc could not compile a variadic function for $triple:"
    cat "$out/va.log"; exit 1; }
cc -o "$out/va" "$out/va.o" 2>> "$out/va.log" || {
    echo "FAIL: linking the variadic test:"; cat "$out/va.log"; exit 1; }
set +e
"$out/va" > "$out/va.txt" 2>&1
rc=$?
set -e
[ "$rc" = 42 ] || { echo "FAIL: the variadic program exited $rc, wanted 42"
                    cat "$out/va.txt"; exit 1; }
grep -qx 'i=15 d=4.00 s=z' "$out/va.txt" || {
    echo "FAIL: va_arg read the wrong values:"; cat "$out/va.txt"; exit 1; }
echo "va_arg walks the stack as Darwin requires: ints, doubles and
pointers, and a variadic call whose own arguments are variadic calls"

# 6. And the frame says so. With every variadic argument on the stack
#    there is nothing to save, so Darwin must NOT emit the sixteen
#    register stores AAPCS64 opens a variadic function with. Compared
#    against the same source built for the freestanding target, which
#    must still have them.
"$EMBCC" --target=aarch64-elf -c "$out/va.c" -o "$out/va-elf.o" 2>/dev/null
d_size=$(wc -c < "$out/va.o")
e_size=$(wc -c < "$out/va-elf.o")
[ "$d_size" -lt "$e_size" ] || {
    echo "FAIL: the Darwin object ($d_size) is not smaller than the"
    echo "      freestanding one ($e_size) -- the register save area"
    echo "      and its stores are probably still being emitted"
    exit 1; }
echo "and the register save area is gone: $d_size bytes against $e_size"
