#!/bin/sh
# A variable declared extern inside a function and defined in another file:
#     BaseType_t xPortStartScheduler(void) { extern void *pxCurrentTCB; ...
# (FreeRTOS's PIC32MZ port). The object must name it, undefined, and
# relocate each reference against that symbol. It named nothing: the
# relocations were against symbol 0, which GNU ld takes for the address 0
# -- the program read whatever was there -- and embld refused as a local
# symbol in no section.
set -u
echo "TEST-MARKER block-extern"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
case $EMBCC in /*) ;; *) EMBCC=$PWD/$EMBCC ;; esac
out=$PWD/tests/golden/out/block-extern
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

cat > "$out/a.c" <<'EOF'
int other(void);
int main(void)
{
    extern int far_value;           /* defined in b.c */
    extern int far_array[];
    return far_value + far_array[2] + other();
}
EOF
printf 'int far_value = 30;\nint far_array[] = { 1, 2, 10 };\nint other(void) { return 2; }\n' > "$out/b.c"

for t in x86_64-elf aarch64-elf thumbv7m-none-eabi riscv32-unknown-elf \
         mipsel-none-elf; do
    for O in -O0 -O2; do
        "$EMBCC" --target=$t $O -c "$out/a.c" -o "$out/a-$t.o" ||
            fail "$t $O: a.c does not compile"
        llvm-nm "$out/a-$t.o" | grep -q ' U far_value$' ||
            fail "$t $O: the object does not name far_value"
        if llvm-objdump -r "$out/a-$t.o" | grep -q '\*ABS\*'; then
            llvm-objdump -r "$out/a-$t.o"
            fail "$t $O: a relocation against no symbol"
        fi
    done
done
echo "block-extern: each target's object names the extern, undefined, at -O0 and -O2"

if [ "$TARGET" = x86_64-elf ]; then
    "$EMBCC" -O1 -c "$out/a.c" -o "$out/a.o" && "$EMBCC" -O1 -c "$out/b.c" -o "$out/b.o" ||
        fail "the program does not compile"
    t_link "$out/p" "$out/a.o" "$out/b.o" || fail "the program does not link"
    t_run "$out/p" > /dev/null
    st=$?
    [ "$st" = 42 ] || fail "the program exited $st, not 42"
    echo "block-extern: linked with its definition, it reads that (exit 42)"
fi
