#!/bin/sh
# A weak definition is a DEFAULT: the link may replace it, and every call
# has to reach whichever definition the link chose. Three ways EmbCC got
# that wrong for a caller in the weak function's own file:
#
#   the call was bound to it at compile time, a displacement within the
#   section rather than a relocation -- so lib/libc's bare-metal write()
#   stayed the default, and a program's printf went nowhere;
#   the optimizer inlined the default's body into the caller;
#   it inferred from the default's body that the function writes no
#   memory, so a global read before the call was reused after it.
#
# Each check below fails on one of those, at the levels that run each pass.
set -u
echo "TEST-MARKER weak-override"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/weak-override-$ARCH
rm -rf "$out"; mkdir -p "$out"

cat > "$out/a.c" << 'EOF'
__attribute__((weak)) int hook(int x) { return x + 1; }
__attribute__((weak)) void touch(void) {}
int g;
int call(int x) { return hook(x) * 2; }
int reread(void)
{
    int a = g;
    touch();
    return a + g;
}
EOF
cat > "$out/b.c" << 'EOF'
int call(int x);
int reread(void);
extern int g;
int hook(int x) { return x + 40; }
void touch(void) { g += 10; }
int main(void)
{
    if (call(1) != 82) return 1;          /* the override, not x + 1 */
    g = 1;
    if (reread() != 12) return 2;         /* g read again after touch() */
    return 42;
}
EOF
for opt in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target="$TARGET" $opt -c "$out/a.c" -o "$out/a$opt.o" &&
    "$EMBCC" --target="$TARGET" $opt -c "$out/b.c" -o "$out/b$opt.o" || {
        echo "$opt: does not compile"; exit 1; }
    t_link "$out/p$opt" "$out/a$opt.o" "$out/b$opt.o" || {
        echo "$opt: does not link"; exit 1; }
    t_run "$out/p$opt" >/dev/null
    got=$?
    [ "$got" -eq 42 ] || {
        echo "$opt: exit $got -- 1: hook() ran the weak default; 2: g was"
        echo "    not read again after touch(), which the override writes"
        exit 1; }
done
echo "weak-override: every call reaches the definition the link chose ($TARGET)"

# The embedded backends make the same choice in their own call lowering:
# the call in call() must be a relocation against hook, for each of them.
command -v llvm-readelf >/dev/null 2>&1 || exit 0
for t in thumbv7em-none-eabi riscv32-unknown-elf riscv64-unknown-elf avr; do
    "$EMBCC" --target=$t -O2 -c "$out/a.c" -o "$out/a-$t.o" || {
        echo "$t: does not compile"; exit 1; }
    llvm-readelf -r "$out/a-$t.o" | grep -qw hook || {
        echo "$t: the call to the weak hook() is bound in the object"; exit 1; }
done
echo "weak-override: Thumb, RISC-V and AVR leave the call to the linker"
