#!/bin/sh
# -O2 register allocation differential: every exec/ program is compiled at -O2
# (which turns on allocation of vregs to callee-saved registers) and must exit
# with — and print — exactly what the host gcc does. A register-allocation bug
# (a value's register clobbered while still live, a wrong extension, a callee
# reg not preserved across a call) shows up here as a divergence from gcc. -O0
# and -O1 are unaffected by -O2 codegen, so this is purely the allocator's net.
set -u
echo "TEST-MARKER regalloc-O2"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
out_dir="tests/golden/out/regalloc-O2-$ARCH"
rm -rf "$out_dir"; mkdir -p "$out_dir"

n=0
for c in tests/exec/*.c; do
    [ -e "$c" ] || continue
    name=$(basename "$c" .c)
    pinned_elsewhere "$c" && continue
    no_gcc_reference "$c" && continue
    "$EMBCC" --target="$TARGET" -O2 -c "$c" -o "$out_dir/$name.o" || {
        echo "$name: embcc -O2 failed to compile"; exit 1; }
    t_link "$out_dir/$name.embcc" "$out_dir/$name.o" || {
        echo "$name: link of -O2 object failed"; exit 1; }
    t_gcc_c "$c" -std=c11 -o "$out_dir/$name.gcc.o" || {
        echo "$name: not valid C11 (needed for the gcc reference)"; exit 1; }
    t_link "$out_dir/$name.gcc" "$out_dir/$name.gcc.o" || {
        echo "$name: link of the gcc object failed"; exit 1; }
    out_a=$(t_run "$out_dir/$name.embcc"); a=$?
    out_b=$(t_run "$out_dir/$name.gcc"); b=$?
    if [ "$a" -ne "$b" ]; then
        echo "$name: embcc -O2 exits $a, gcc exits $b — register-alloc miscompile"
        exit 1
    fi
    if [ "$out_a" != "$out_b" ]; then
        echo "$name: stdout differs at -O2:"
        printf 'embcc: %s\ngcc:   %s\n' "$out_a" "$out_b"
        exit 1
    fi
    n=$((n + 1))
done

echo "all $n exec programs agree with gcc when built -O2 (register allocation)"
echo "regalloc-O2 acceptance passed"
