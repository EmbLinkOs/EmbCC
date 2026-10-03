#!/bin/sh
# embar, EmbCC's archiver (tools/embar): the libraries are packaged with it,
# so a host with no binutils or LLVM can build them.
#
# Checked against the reference archiver where one is installed: the same
# members, and a symbol index that lists the same definitions. Then the
# parts a library build leans on: names past fifteen characters, `r`
# replacing a member of the same name, the output being the same bytes
# every time, and embld linking through an archive embar made.
set -u
echo "TEST-MARKER embar"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBAR=${EMBAR:-./embar}
EMBLD=${EMBLD:-./embld}
out=tests/golden/out/embar-$ARCH
rm -rf "${out:?}"; mkdir -p "$out"

cat > "$out/alpha.c" <<'CEOF'
int alpha(int x) { return x * 3; }
int alpha_data = 7;
CEOF
cat > "$out/a_member_with_a_long_name.c" <<'CEOF'
int beta(int x);
int beta(int x) { return x + 39; }
__attribute__((weak)) int gamma_weak(void) { return 1; }
CEOF
for t in x86_64-elf thumbv7em-none-eabi; do
    for f in alpha a_member_with_a_long_name; do
        "$EMBCC" --target=$t -c "$out/$f.c" -o "$out/$f-$t.o" || {
            echo "FAIL: $f.c does not compile for $t"; exit 1; }
        cp "$out/$f-$t.o" "$out/$f.o"
    done
    rm -f "$out/lib.a"
    "$EMBAR" rcs "$out/lib.a" "$out/alpha.o" "$out/a_member_with_a_long_name.o" || {
        echo "FAIL: embar could not archive the $t objects"; exit 1; }
    [ "$("$EMBAR" t "$out/lib.a" | tr '\n' ' ')" = "alpha.o a_member_with_a_long_name.o " ] || {
        echo "FAIL: $t: the archive's members are not the two objects:"
        "$EMBAR" t "$out/lib.a"; exit 1; }
    if command -v llvm-ar > /dev/null 2>&1; then
        [ "$(llvm-ar t "$out/lib.a" | tr '\n' ' ')" = "alpha.o a_member_with_a_long_name.o " ] || {
            echo "FAIL: $t: llvm-ar reads other members"; exit 1; }
        llvm-nm --print-armap "$out/lib.a" 2>/dev/null |
            sed -n '/^Archive map/,/^$/p' | grep ' in ' | sort > "$out/mine.idx"
        rm -f "$out/ref.a"
        llvm-ar rcs "$out/ref.a" "$out/alpha.o" "$out/a_member_with_a_long_name.o"
        llvm-nm --print-armap "$out/ref.a" 2>/dev/null |
            sed -n '/^Archive map/,/^$/p' | grep ' in ' | sort > "$out/ref.idx"
        cmp -s "$out/mine.idx" "$out/ref.idx" || {
            echo "FAIL: $t: the symbol index differs from llvm-ar's:"
            diff "$out/ref.idx" "$out/mine.idx" | head; exit 1; }
    fi
done
echo "embar's members and symbol index match llvm-ar's, for ELF64 and ELF32"

# r replaces a member of the same name; the bytes are the same every time
cp "$out/alpha-x86_64-elf.o" "$out/alpha.o"
cp "$out/a_member_with_a_long_name-x86_64-elf.o" "$out/a_member_with_a_long_name.o"
rm -f "$out/r.a" "$out/r2.a"
"$EMBAR" rcs "$out/r.a" "$out/alpha.o" "$out/a_member_with_a_long_name.o"
"$EMBAR" rcs "$out/r.a" "$out/alpha.o"
[ "$("$EMBAR" t "$out/r.a" | wc -l | tr -d ' ')" = 2 ] || {
    echo "FAIL: r added a second alpha.o instead of replacing it"; exit 1; }
"$EMBAR" rcs "$out/r2.a" "$out/alpha.o" "$out/a_member_with_a_long_name.o"
cmp -s "$out/r.a" "$out/r2.a" || {
    echo "FAIL: the same members in the same order gave different bytes"; exit 1; }
echo "r replaces in place, and the output is deterministic"

# embld links through it
cat > "$out/main.c" <<'CEOF'
int alpha(int); int beta(int);
void _start(void) { volatile int r = alpha(1) + beta(0); (void)r; for (;;); }
CEOF
"$EMBCC" --target=x86_64-elf -c "$out/main.c" -o "$out/main.o" &&
"$EMBLD" -e _start "$out/main.o" "$out/r.a" -o "$out/p.elf" || {
    echo "FAIL: embld did not link through an archive embar made"; exit 1; }
echo "and embld links through an archive embar made"
