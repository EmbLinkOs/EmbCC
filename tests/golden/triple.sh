#!/bin/sh
# The target triple: architecture x operating system x object format
# (D-014). Before it, --target= chose an architecture and the object
# format was assumed; a hosted platform needs all three.
#
# The first check is the one that matters most: everything that worked
# before D-014 must mean exactly what it did. A refactor of the target
# selector is the kind of change that can silently retarget a whole
# build.
set -eu
echo "TEST-MARKER triple"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out=$EMBCC_ROOT/tests/golden/out/triple-$ARCH
rm -rf "$out"; mkdir -p "$out"
printf 'int main(void) { return 0; }\n' > "$out/t.c"

pd() { "$EMBCC" --target="$1" --dump-predef 2>/dev/null; }

# 1. The default is untouched: no --target= still means freestanding
#    x86_64-elf, and its macro table is the generated one, entry for
#    entry. predef.sh checks the contents; this checks that adding two
#    dimensions did not disturb them.
"$EMBCC" --dump-predef > "$out/default.txt" 2>/dev/null
pd x86_64-elf > "$out/explicit.txt"
cmp -s "$out/default.txt" "$out/explicit.txt" || {
    echo "FAIL: the default target is no longer x86_64-elf"; exit 1; }
grep -q '^#define __ELF__ 1$' "$out/default.txt" || {
    echo "FAIL: the freestanding table lost __ELF__"; exit 1; }
for m in __linux__ __APPLE__ _WIN32; do
    grep -q "$m" "$out/default.txt" && {
        echo "FAIL: a freestanding target claims $m"; exit 1; }
done
echo "the default target and its macro table are unchanged"

# 2. Every triple the compiler lists is one it accepts. The list in the
#    error message and the table it parses from cannot drift apart,
#    because a name printed as supported and then refused is worse than
#    no list at all.
"$EMBCC" --target=definitely-not-a-target -c "$out/t.c" -o "$out/x.o" \
    2> "$out/list.txt" && { echo "FAIL: an unknown triple was accepted"; exit 1; }
grep -q "unknown target 'definitely-not-a-target'" "$out/list.txt" || {
    echo "FAIL: no diagnostic naming the triple:"; cat "$out/list.txt"; exit 1; }
n=0
for t in $(sed -n 's/^embcc:   \(.*\)$/\1/p' "$out/list.txt"); do
    pd "$t" > /dev/null 2>&1 || {
        echo "FAIL: '$t' is listed as a target but not accepted"; exit 1; }
    n=$((n + 1))
done
[ "$n" -ge 9 ] || { echo "FAIL: only $n targets listed"; exit 1; }
echo "$n triples listed, and every one of them parses"

# 3. A hosted target gets the macros its platform's headers read, and a
#    freestanding one does not. This is what a system header branches on
#    to decide which declarations exist.
pd x86_64-linux-gnu > "$out/linux.txt"
for m in __linux__ __gnu_linux__ __unix__; do
    grep -q "^#define $m 1$" "$out/linux.txt" || {
        echo "FAIL: linux target lacks $m"; exit 1; }
done
grep -q '^#define __x86_64__ 1$' "$out/linux.txt" || {
    echo "FAIL: the OS overlay dropped the architecture's own macros"; exit 1; }
pd aarch64-apple-darwin > "$out/darwin.txt"
for m in __APPLE__ __MACH__; do
    grep -q "^#define $m 1$" "$out/darwin.txt" || {
        echo "FAIL: darwin target lacks $m"; exit 1; }
done
pd x86_64-windows-gnu > "$out/win.txt"
for m in _WIN32 _WIN64 __MINGW64__; do
    grep -q "^#define $m 1$" "$out/win.txt" || {
        echo "FAIL: windows target lacks $m"; exit 1; }
done
echo "each hosted target defines what its own headers read"

# 3b. EmbLinkOS is a target in its own right, not "no OS". It is the
#     primary product target, it has syscalls and a libc, and code built
#     for it should be able to ASK -- rather than infer the OS from the
#     absence of __linux__.
pd x86_64-emblink > "$out/emblink.txt"
for m in __emblink__ __EmbLinkOS__; do
    grep -q "^#define $m 1$" "$out/emblink.txt" || {
        echo "FAIL: emblink target lacks $m"; exit 1; }
done
#     Its objects are ELF: `embld --embx` makes the native image at LINK
#     time (D-003), so the object format is not where EMBX lives.
grep -q '^#define __ELF__ 1$' "$out/emblink.txt" || {
    echo "FAIL: emblink objects are ELF and should keep __ELF__"; exit 1; }
"$EMBCC" --target=x86_64-emblink -c "$out/t.c" -o "$out/emb.o" || {
    echo "FAIL: x86_64-emblink did not produce an object"; exit 1; }
#     And it is NOT "hosted": its libc and linker are ours, not the
#     platform's, which is the distinction target_is_hosted() draws.
#     The observable consequence is that it claims no platform macros.
for m in __linux__ __APPLE__ _WIN32 __unix__; do
    grep -q "$m" "$out/emblink.txt" && {
        echo "FAIL: emblink claims $m, which is another platform's"; exit 1; }
done
echo "EmbLinkOS is its own OS, with ELF objects and none of anyone else's macros"

# 4. __ELF__ is a claim about the OBJECT FORMAT, and it lives in the
#    generated architecture tables because those came from the *-elf
#    compilers. On a target that is not ELF it is false, and a header
#    that reads it would be told the wrong thing -- so it is dropped.
grep -q '^#define __ELF__ 1$' "$out/linux.txt" || {
    echo "FAIL: linux is ELF and should keep __ELF__"; exit 1; }
for f in darwin win; do
    grep -q '__ELF__' "$out/$f.txt" && {
        echo "FAIL: a non-ELF target still claims __ELF__ ($f)"; exit 1; }
done
echo "__ELF__ follows the format, not the architecture"

# 5. THE RULE. A target whose object writer does not exist is refused,
#    by name, and writes nothing -- rather than quietly emitting ELF and
#    calling it a COFF object. Darwin was on this list until its writer
#    was built; that it had to be taken off is the check working.
for t in x86_64-windows-gnu; do
    rm -f "$out/o.o"
    if "$EMBCC" --target="$t" -c "$out/t.c" -o "$out/o.o" 2> "$out/err.txt"; then
        echo "FAIL: $t emitted an object it has no writer for"; exit 1
    fi
    grep -q "$t" "$out/err.txt" || {
        echo "FAIL: the refusal does not name the triple:"
        cat "$out/err.txt"; exit 1; }
    [ -e "$out/o.o" ] && {
        echo "FAIL: $t was refused but still wrote a file"; exit 1; }
done
echo "a format with no writer is refused by name, and writes no file"

# 6. ...and the parts that DO work for such a target keep working, which
#    is why the triple is in the table before its writer exists: the
#    macros and the preprocessor are how the port gets developed.
"$EMBCC" --target=x86_64-windows-gnu -E "$out/t.c" > "$out/pp.txt" 2>&1 || {
    echo "FAIL: -E refused for a target whose writer is missing"; exit 1; }
grep -q 'int main' "$out/pp.txt" || {
    echo "FAIL: -E produced nothing useful"; cat "$out/pp.txt"; exit 1; }
echo "-E still works for a target whose object writer is not built yet"

# 6b. Darwin HAS a writer now, and it produces a Mach-O rather than an
#     ELF wearing the name. Checked here on any host -- writing the
#     format needs nothing from the platform; only linking it does, and
#     macho.sh does that where it can.
"$EMBCC" --target=aarch64-apple-darwin -c "$out/t.c" -o "$out/d.o" || {
    echo "FAIL: aarch64-apple-darwin did not produce an object"; exit 1; }
#     0xfeedfacf little-endian, and CPU_TYPE_ARM64 = 0x0100000c.
od -An -tx1 -N8 "$out/d.o" | tr -d ' \n' > "$out/magic.txt"
grep -qi '^cffaedfe0c000001$' "$out/magic.txt" || {
    echo "FAIL: not a Mach-O arm64 header: $(cat "$out/magic.txt")"; exit 1; }
"$EMBCC" --target=x86_64-apple-darwin -c "$out/t.c" -o "$out/d64.o" || {
    echo "FAIL: x86_64-apple-darwin did not produce an object"; exit 1; }
od -An -tx1 -N8 "$out/d64.o" | tr -d ' \n' > "$out/magic64.txt"
grep -qi '^cffaedfe07000001$' "$out/magic64.txt" || {
    echo "FAIL: not a Mach-O x86-64 header: $(cat "$out/magic64.txt")"; exit 1; }
echo "both Darwin triples emit a Mach-O object with their own cputype"

# 7. The one hosted target that is complete enough to emit: Linux is ELF
#    and System V, both of which already existed, so it produces a real
#    relocatable object today.
"$EMBCC" --target=x86_64-linux-gnu -c "$out/t.c" -o "$out/lin.o" || {
    echo "FAIL: x86_64-linux-gnu did not produce an object"; exit 1; }
if command -v x86_64-elf-readelf > /dev/null 2>&1; then
    x86_64-elf-readelf -h "$out/lin.o" > "$out/hdr.txt" 2>&1
    grep -q 'REL (Relocatable file)' "$out/hdr.txt" || {
        echo "FAIL: not a relocatable object:"; cat "$out/hdr.txt"; exit 1; }
    grep -q 'X86-64' "$out/hdr.txt" || {
        echo "FAIL: wrong machine:"; cat "$out/hdr.txt"; exit 1; }
    echo "x86_64-linux-gnu emits a genuine ELF64 relocatable object"
else
    echo "x86_64-linux-gnu emits an object (readelf absent, header unchecked)"
fi
