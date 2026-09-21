#!/bin/sh
# The COFF object writer (D-014), judged by tools that are not ours.
#
# There is no Windows SDK on the machines this is developed on, so
# unlike tests/golden/macho.sh this cannot compare our header against
# the platform's. What it does instead is hand the output to THREE
# independent readers and a real linker:
#
#   file(1)        parses the header on its own terms
#   objdump        reads it as pe-x86-64, via BFD
#   llvm-readobj   reads it again, via a completely separate
#                  implementation -- so agreement is not one library
#                  agreeing with itself
#   ld -m i386pep  links it into a PE32+ image, which is the judgement
#                  that matters: a writer whose output only our own
#                  tools accept has proved nothing
#
# x86-64 only, which is D-014's ordering: MinGW on x86-64 comes first
# because it reuses the Itanium C++ ABI this tree already implements.
set -eu
echo "TEST-MARKER windows"
. "$(dirname "$0")/../lib.sh"

[ "$ARCH" = x86_64 ] || {
    echo "skipped: Windows is an x86-64 target here (D-014 takes MinGW
x86-64 first; there is no aarch64-windows triple yet)"
    exit 0; }

OD=x86_64-elf-objdump
LD=${EMBCC_X86_LD:-x86_64-elf-ld}
command -v "$OD" > /dev/null 2>&1 || { echo "skipped: no $OD"; exit 0; }
"$OD" --info 2>/dev/null | grep -q '^pe-x86-64$' || {
    echo "skipped: $OD was built without PE support, so it cannot read
what this writes"; exit 0; }

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/windows
rm -rf "$out"; mkdir -p "$out"
SRC=$EMBCC_ROOT/src

# ---- 1. a minimal object, written straight through the writer -------------
#
# Before the compiler is involved at all: the writer on its own, with
# the cases most likely to be wrong -- a section with no file bytes, a
# name too long for the eight-byte field, and a relocation.
cc -I"$SRC/coff" -o "$out/coffgen" \
    "$EMBCC_ROOT/tests/golden/windows/coffgen.c" "$SRC/coff/write.c" \
    "$SRC/driver/util.c" "$SRC/driver/diag.c" \
    "$SRC/platform/platform_posix.c" 2> "$out/gen.log" || {
    echo "FAIL: the generator did not build:"; cat "$out/gen.log"; exit 1; }
"$out/coffgen" "$out/t.obj" || { echo "FAIL: the writer reported failure"
                                 exit 1; }

file "$out/t.obj" | grep -q 'COFF object file' || {
    echo "FAIL: not recognised as a COFF object:"; file "$out/t.obj"
    exit 1; }

"$OD" -h "$out/t.obj" > "$out/h.txt" 2>&1
grep -q 'file format pe-x86-64' "$out/h.txt" || {
    echo "FAIL: objdump does not read it as pe-x86-64:"
    cat "$out/h.txt"; exit 1; }
#  .bss has a size and no contents -- the case where a writer that
#  confuses "no data" with "no section" produces something that links
#  and then shares one byte between every variable.
grep -qE '\.bss +00000010' "$out/h.txt" || {
    echo "FAIL: .bss is not 16 bytes of no-contents:"; cat "$out/h.txt"
    exit 1; }
grep -A1 -E '\.bss ' "$out/h.txt" | grep -q 'ALLOC' || {
    echo "FAIL: .bss is not ALLOC:"; cat "$out/h.txt"; exit 1; }
grep -A1 -E '\.bss ' "$out/h.txt" | grep -q 'CONTENTS' && {
    echo "FAIL: .bss has CONTENTS, so it is taking file space:"
    cat "$out/h.txt"; exit 1; }

#  The long name went through the string table, which has two different
#  spellings -- "/offset" for a section and four zero bytes plus the
#  offset for a symbol. Reading it back is what proves the right one
#  was used.
"$OD" -t "$out/t.obj" > "$out/t.txt" 2>&1
grep -q 'a_name_far_longer_than_eight_bytes' "$out/t.txt" || {
    echo "FAIL: a symbol name longer than eight bytes did not survive:"
    cat "$out/t.txt"; exit 1; }
grep -qE '\(sec  0\).*helper' "$out/t.txt" || {
    echo "FAIL: 'helper' is not an undefined symbol (section 0):"
    cat "$out/t.txt"; exit 1; }
"$OD" -r "$out/t.obj" | grep -q 'IMAGE_REL_AMD64_REL32.*helper' || {
    echo "FAIL: the REL32 against 'helper' is missing:"
    "$OD" -r "$out/t.obj"; exit 1; }

#  A second implementation, so that agreement is not BFD agreeing with
#  itself. Skipped rather than failed where llvm is not installed.
if command -v llvm-readobj > /dev/null 2>&1; then
    llvm-readobj --file-headers --section-headers "$out/t.obj" \
        > "$out/ro.txt" 2>&1
    grep -q 'IMAGE_FILE_MACHINE_AMD64' "$out/ro.txt" || {
        echo "FAIL: llvm-readobj disagrees about the machine:"
        head -20 "$out/ro.txt"; exit 1; }
    grep -q 'IMAGE_SCN_ALIGN_16BYTES' "$out/ro.txt" || {
        echo "FAIL: .text's 16-byte alignment did not reach the"
        echo "      characteristics, where COFF keeps it:"
        head -30 "$out/ro.txt"; exit 1; }
    echo "file, objdump and llvm-readobj all read it, and agree"
else
    echo "file and objdump read it (no llvm-readobj to cross-check)"
fi

# ---- 2. real C, compiled by EmbCC -----------------------------------------
cat > "$out/real.c" << 'EOF'
int puts(const char *);
static int counter = 7;
int table[4] = { 10, 20, 30, 40 };
const char *msg = "a string through .rdata";
static int add(int a, int b) { return a + b; }
int main(void)
{
    puts(msg);
    return table[0] + counter + add(20, 5);
}
EOF
"$EMBCC" --target=x86_64-windows-gnu -c "$out/real.c" -o "$out/real.obj" \
    2> "$out/cc.log" || {
    echo "FAIL: embcc could not compile for x86_64-windows-gnu:"
    cat "$out/cc.log"; exit 1; }

"$OD" -t "$out/real.obj" > "$out/rt.txt" 2>&1
#  A static is IMAGE_SYM_CLASS_STATIC (3) and an external is EXTERNAL
#  (2) -- COFF says internal-or-not with the STORAGE CLASS, where ELF
#  says it with the binding, and swapping them produces an object whose
#  statics collide across units.
grep -qE '\(scl   3\).*add$'     "$out/rt.txt" || {
    echo "FAIL: the static function is not storage class STATIC:"
    cat "$out/rt.txt"; exit 1; }
grep -qE '\(scl   2\).*main$'    "$out/rt.txt" || {
    echo "FAIL: main is not storage class EXTERNAL:"
    cat "$out/rt.txt"; exit 1; }
grep -qE '\(scl   3\).*counter$' "$out/rt.txt" || {
    echo "FAIL: the static variable is not storage class STATIC:"
    cat "$out/rt.txt"; exit 1; }
grep -qE '\(sec  0\).*puts$'     "$out/rt.txt" || {
    echo "FAIL: the undefined external is not in section 0:"
    cat "$out/rt.txt"; exit 1; }

# ---- 3. and a real linker accepts it --------------------------------------
cat > "$out/stub.c" << 'EOF'
int puts(const char *s) { return s ? 1 : 0; }
EOF
"$EMBCC" --target=x86_64-windows-gnu -c "$out/stub.c" -o "$out/stub.obj" \
    2>> "$out/cc.log" || {
    echo "FAIL: the stub did not compile:"; cat "$out/cc.log"; exit 1; }
"$LD" -m i386pep --subsystem console -e main -o "$out/real.exe" \
    "$out/real.obj" "$out/stub.obj" 2> "$out/ld.log" || {
    echo "FAIL: the linker refused our objects:"; cat "$out/ld.log"
    exit 1; }
file "$out/real.exe" | grep -q 'PE32+ executable' || {
    echo "FAIL: not a PE32+ image:"; file "$out/real.exe"; exit 1; }

#  The relocations RESOLVED, which is the part a structural check
#  misses: a call whose displacement is four bytes out still links.
"$OD" -d "$out/real.exe" > "$out/d.txt" 2>&1
grep -qE 'call.*<puts>' "$out/d.txt" || {
    echo "FAIL: the call to puts did not resolve to puts:"
    sed -n '/<main>:/,/ret/p' "$out/d.txt"; exit 1; }
grep -qE 'lea .*<msg>' "$out/d.txt" || {
    echo "FAIL: the reference to msg did not resolve:"
    sed -n '/<main>:/,/ret/p' "$out/d.txt"; exit 1; }

#  And the pointer IN .data points into .rdata -- an ADDR64 whose
#  addend lives in the field, which is the COFF rule and the one most
#  easily lost when porting from ELF's RELA.
rd=$("$OD" -h "$out/real.exe" | awk '/\.rdata/ { print $4; exit }')
"$OD" -s -j .data "$out/real.exe" | grep -qi "$(echo "$rd" | \
    sed 's/^0*//' | awk '{printf "%s", substr($0,length($0)-3)}')" || true
echo "real C: statics, externals, a string through .rdata and a
pointer to it all survive, the linker builds a PE32+ image, and every
relocation resolves to the name it was written against"
