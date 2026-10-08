#!/bin/sh
# __attribute__((section("name"))) on a static local: firmware keeps a
# reset counter in .noinit, a version tag in its own read-only section,
# a buffer in core-coupled RAM -- declared where they are used, inside a
# function. GCC and clang place them; EmbCC refused every one ("section
# attribute on block-scope 'x' is not supported"). A static local is an
# object of static storage and now goes where its attribute says. An
# automatic variable, which lives on the stack, is still refused, as GCC
# refuses it.
#
#   - on Cortex-M, RV32 and x86-64, each static local is in its named
#     section, as a local symbol named function.variable, as clang names
#     it, with either spelling of the attribute;
#   - on the Cortex-M3 board, an initialised one in its own data section
#     keeps its value across calls and starts from its initialiser, and a
#     const one in its own section reads back.
set -u
echo "TEST-MARKER static-local-section"
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/static-local-section
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
export EMBCC_VERIFY=1

cat > "$out/s.c" <<'EOF'
int f(void)
{
    static int boot __attribute__((section(".noinit")));
    __attribute__((section(".data.mine"))) static int counted = 5;
    static const char tag[] __attribute__((section(".rodata.tag"))) = "v1";
    return ++boot + ++counted + tag[0];
}
EOF
for T in thumbv7m-none-eabi riscv32-unknown-elf x86_64-elf; do
    "$EMBCC" --target=$T -O2 -c "$out/s.c" -o "$out/s.o" > "$out/s.err" 2>&1 ||
        fail "$T: $(head -2 "$out/s.err")"
    llvm-readelf -S -s "$out/s.o" > "$out/s.txt"
    for pair in "f.boot .noinit" "f.counted .data.mine" "f.tag .rodata.tag"; do
        set -- $pair
        sec=$(sed -n 's/^ *\[ *\([0-9]*\)\] \([^ ]*\) .*/\1 \2/p' "$out/s.txt" |
              awk -v n="$2" '$2 == n { print $1 }')
        [ -n "$sec" ] || fail "$T: no section $2: $(grep '\[' "$out/s.txt" | head -12)"
        awk -v n="$1" -v x="$sec" '$NF == n && $5 == "LOCAL" && $7 == x { f = 1 } END { exit !f }' \
            "$out/s.txt" || fail "$T: $1 is not a local symbol in $2 (section $sec): $(grep "$1" "$out/s.txt")"
    done
done
echo "static locals go to their sections on Cortex-M, RV32 and x86-64, named as clang names them"

printf 'int g(void) { int a __attribute__((section(".x"))) = 1; return a; }\n' > "$out/auto.c"
"$EMBCC" --target=thumbv7m-none-eabi -c "$out/auto.c" -o "$out/auto.o" \
    2> "$out/auto.err" && fail "a section on an automatic variable was accepted"
grep -q "only a static local can be placed in a section" "$out/auto.err" ||
    fail "the refusal of an automatic one does not say why: $(cat "$out/auto.err")"
echo "and an automatic variable with a section is refused by name"

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
if command -v "$QEMU" >/dev/null 2>&1; then
    T=thumbv7m-none-eabi
    d=$out/m3
    mkdir -p "$d"
    cat > "$d/prog.c" <<'EOF'
void putn(long v);
void puts_(const char *s);
__attribute__((noinline)) static int tick(void)
{
    __attribute__((section(".data.mine"))) static int counted = 5;
    static const char tag[] __attribute__((section(".rodata.tag"))) = "v1";
    return ++counted * 100 + tag[1];
}
int main(void)
{
    puts_("ticks "); putn(tick()); putn(tick()); putn(tick()); puts_("\n");
    return 42;
}
EOF
    for f in boot io; do
        "$EMBCC" --target=$T -O1 -c tests/harness/thumb/$f.c -o "$d/$f.o" || fail "$f.c"
    done
    for O in -O0 -O2 -Os; do
        "$EMBCC" --target=$T $O -c "$d/prog.c" -o "$d/prog.o" || fail "prog.c $O"
        EMBCC_THUMB_HARNESS=$d sh tests/harness/thumb/link.sh "$d/prog.elf" "$d/prog.o" \
            > "$d/link.log" 2>&1 || fail "the M3 image does not link: $(head -3 "$d/link.log")"
        sh tests/harness/qrun.sh 10 "$QEMU" -M lm3s6965evb -cpu cortex-m3 -nographic \
            -kernel "$d/prog.elf" > "$d/console$O.log" 2>&1
        # '1' is 49: 649, 749, 849
        tr -d '\r' < "$d/console$O.log" | tr -s ' ' | grep -q "^ticks 649 749 849 *$" ||
            fail "at $O the static local did not count from 5: $(head -3 "$d/console$O.log")"
    done
    echo "on the Cortex-M3 it starts from its initialiser and keeps its value, at three levels"
else
    echo "SKIP: the board run ($QEMU not found)"
fi
echo "ok static-local-section"
