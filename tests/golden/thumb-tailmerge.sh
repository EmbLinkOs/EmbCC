#!/bin/sh
# -Os on ARM: one copy of a repeated block end (pass_tailmerge in
# src/opt/opt.c), and only where the copies compute the same thing.
#
# Text in, text out (EmbIR, optimized by `inspect ir -Os`): one function
# whose two blocks end in the same run must keep one copy of it, and so
# must one whose runs read one value differently (a merge temp each run
# sets carries it in); where a rule says no -- different successors, a
# value the run defines read after the block, a different variable
# written, a merge temp the label's other path does not set, a differing
# value the shared copy also reads where both runs read it -- both copies
# stay; and so do a loop's blocks where the path through the shared copy
# would take a branch more each trip for less than a call's worth.
# Then C: scanf's store_int, five `case`s storing through
# `va_arg(ap, long *)`, has one such store left at -Os; a strftime-like
# switch in a loop, six `case`s calling f with differing arguments, keeps
# three calls and runs to the value C says on x86-64. Corpus programs on
# the boards (exec-boards.sh, thumb-v6m-exec.sh) check the values.
set -u
echo "TEST-MARKER thumb-tailmerge"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
out=tests/golden/out/thumb-tailmerge
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
opt() { "$EMBCC" inspect ir -Os --target=thumbv7em-none-eabi "$out/$1.ir" \
            > "$out/$1.out" 2>&1 || { cat "$out/$1.out"; fail "$1: inspect"; }; }

# pos: two blocks end in the same three instructions and the same join: one copy
cat > "$out/pos.ir" <<'EOF'
; EmbIR
func @pos nparams=3 nvars=3 vregs=16 labels=3 {
  brz.4s %2 -> L0
  %4 = add.4s %0, #7
  store:4s [%1], %4
  %5 = mul.4s %0, %4
  store:4s [%1], %5
  jmp L2
L0:
  %6 = add.4s %0, #7
  store:4s [%1], %6
  %7 = mul.4s %0, %6
  store:4s [%1], %7
L2:
  ret %0
}
EOF

# exit: the same instructions, but the blocks go on to different places
cat > "$out/exit.ir" <<'EOF'
; EmbIR
func @exit nparams=3 nvars=3 vregs=16 labels=5 {
  brnz.4s %1 -> L2
  brnz.4s %0 -> L3
  brz.4s %2 -> L0
  %4 = add.4s %0, #7
  store:4s [%1], %4
  %5 = mul.4s %0, %4
  store:4s [%1], %5
  jmp L3
L0:
  %6 = add.4s %0, #7
  store:4s [%1], %6
  %7 = mul.4s %0, %6
  store:4s [%1], %7
  jmp L2
L2:
  %9 = add.4s %0, %1
  %10 = mul.4s %9, %1
  ret %10
L3:
  %11 = sub.4s %0, %1
  %12 = mul.4s %11, %1
  ret %12
}
EOF

# ops: the same instructions, one reading another value: the adds stay,
# each writing the merge temp, and the rest is one copy
cat > "$out/ops.ir" <<'EOF'
; EmbIR
func @ops nparams=3 nvars=3 vregs=16 labels=3 {
  brz.4s %2 -> L0
  %4 = add.4s %0, #7
  store:4s [%1], %4
  %5 = mul.4s %0, %4
  store:4s [%1], %5
  jmp L2
L0:
  %6 = add.4s %2, #7
  store:4s [%1], %6
  %7 = mul.4s %0, %6
  store:4s [%1], %7
L2:
  ret %0
}
EOF

# local: what the second run defines is read after the join: not the run's own
cat > "$out/local.ir" <<'EOF'
; EmbIR
func @local nparams=3 nvars=3 vregs=16 labels=5 {
  brz.4s %2 -> L0
  %4 = add.4s %0, #7
  store:4s [%1], %4
  %5 = mul.4s %0, %4
  store:4s [%1], %5
  jmp L2
L0:
  %6 = add.4s %0, #7
  store:4s [%1], %6
  %7 = mul.4s %0, %6
  store:4s [%1], %7
L2:
  brz.4s %2 -> L4
  ret %0
L4:
  ret %7
}
EOF

# loop: inside a loop, where both runs end in a jump: the path through the copy would take one more
cat > "$out/loop.ir" <<'EOF'
; EmbIR
func @loop nparams=3 nvars=3 vregs=16 labels=5 {
L4:
  brz.4s %2 -> L0
  %4 = add.4s %0, #7
  store:4s [%1], %4
  %5 = mul.4s %0, %4
  store:4s [%1], %5
  jmp L2
L0:
  %6 = add.4s %0, #7
  store:4s [%1], %6
  %7 = mul.4s %0, %6
  store:4s [%1], %7
  jmp L2
L3:
  ret %0
L2:
  %2 = sub.4s %2, #1
  brnz.4s %2 -> L4
  jmp L3
}
EOF

# def: the runs write different variables, both read after the join
cat > "$out/def.ir" <<'EOF'
; EmbIR
func @def nparams=3 nvars=3 vregs=16 labels=3 {
  %5 = const.4 0
  %7 = const.4 1
  brz.4s %2 -> L0
  %4 = add.4s %0, #7
  %5 = mul.4s %0, %4
  store:4s [%1], %4
  jmp L2
L0:
  %6 = add.4s %0, #7
  %7 = mul.4s %0, %6
  store:4s [%1], %6
L2:
  %8 = sub.4s %5, %7
  ret %8
}
EOF

# pre: the shared copy begins at a label (L1) another path jumps to
# without setting %4 (the loop's back edge): no merge temp there, so the
# second run may not set %4 and jump in -- the loop would then read its
# value instead of %0 * 3 on every later trip
cat > "$out/pre.ir" <<'EOF'
; EmbIR
func @pre nparams=3 nvars=3 vregs=16 labels=6 {
  brnz.4s %1 -> L0
  %4 = mul.4s %0, #3
L1:
  %5 = add.4s %4, #1
  %8 = mul.4s %0, %5
  store:4s [%1], %8
L2:
  %2 = sub.4s %2, #1
  brz.4s %2 -> L3
  jmp L1
L0:
  %6 = mul.4s %0, #5
  %7 = add.4s %6, #1
  %9 = mul.4s %0, %7
  store:4s [%1], %9
  jmp L2
L3:
  ret %0
}
EOF

# same: the runs read %3 and %8 where the mul is, and both read %3 at the
# first add: a merge temp for the mul would hand the second run's %8 to
# that add too (the last two instructions, alike, do share)
cat > "$out/same.ir" <<'EOF'
; EmbIR
func @same nparams=3 nvars=3 vregs=16 labels=3 {
  %3 = add.4s %0, #7
  %8 = add.4s %0, #9
  brz.4s %2 -> L0
  %12 = add.4s %3, #1
  %5 = mul.4s %0, %3
  %10 = add.4s %5, %12
  store:4s [%1], %10
  jmp L2
L0:
  %13 = add.4s %3, #1
  %7 = mul.4s %0, %8
  %11 = add.4s %7, %13
  store:4s [%1], %11
L2:
  ret %0
}
EOF

# indef: the second run's differing value, %9, is defined INSIDE the five
# instructions that would be dropped (and before the branch too, so
# nothing calls it the run's own): the move carrying it in would run
# before that definition and read the first one (fuzz seed 13408 found
# it). The four after the load share, each run keeping its load and
# moving its value in.
cat > "$out/indef.ir" <<'EOF'
; EmbIR
func @indef nparams=3 nvars=3 vregs=16 labels=3 {
  %9 = load.4:4s [%1]
  brz.4s %9 -> L0
  %9 = load.4:4s [%0]
  %4 = mul.4s %0, #3
  %5 = sub.4s %4, %1
  %10 = mul.4s %5, #5
  store:4s [%2], %10
  jmp L2
L0:
  %9 = load.4:4s [%0]
  %6 = mul.4s %0, #3
  %7 = sub.4s %6, %9
  %11 = mul.4s %7, #5
  store:4s [%2], %11
L2:
  ret %0
}
EOF

copies() { grep -c 'mul.4s %0' "$out/$1.out"; }
# ...on every target, not ARM alone: the pass was gated to TARGET_THUMB
for t in riscv32-unknown-elf x86_64-elf avr aarch64-elf; do
    "$EMBCC" inspect ir -Os --target=$t "$out/pos.ir" > "$out/pos-$t.out" 2>&1 ||
        { cat "$out/pos-$t.out"; fail "pos on $t: inspect"; }
    [ "$(grep -c 'mul.4s %0' "$out/pos-$t.out")" = 1 ] ||
        { cat "$out/pos-$t.out"; fail "pos on $t: the repeated tail was not merged"; }
done
echo "the same copy is kept once on RISC-V, x86-64, AVR and AArch64"
opt pos
[ "$(copies pos)" = 1 ] || { cat "$out/pos.out"; fail "pos: the run is still there twice"; }
opt ops
[ "$(copies ops)" = 1 ] || { cat "$out/ops.out"; fail "ops: the runs differ in one value only, the rest is not shared"; }
grep -q 'add.4s %2, #7' "$out/ops.out" || { cat "$out/ops.out"; fail "ops: the differing add is gone"; }
for c in exit local def loop same; do
    opt $c
    [ "$(copies $c)" = 2 ] || { cat "$out/$c.out"; fail "$c: merged where the runs differ"; }
done
opt pre
[ "$(grep -c 'add.4s' "$out/pre.out")" = 2 ] ||
    { cat "$out/pre.out"; fail "pre: merged into a label whose other path does not set the value"; }
opt indef
[ "$(grep -c 'load.4' "$out/indef.out")" = 3 ] ||
    { cat "$out/indef.out"; fail "indef: a value the dropped run defines was carried in from before it"; }
[ "$(copies indef)" = 1 ] ||
    { cat "$out/indef.out"; fail "indef: the four instructions after the load are not shared"; }
echo "IR: one copy where the runs are the same or differ in a value, two in seven shapes where they are not"

# C: a strftime-like switch in a loop; the cases differ in an argument
# (a field, a product, a parameter) and share one call through a merge
# temp; 'e', two values apart, joins them through two moves; 'f', three
# apart, keeps its call -- two calls, on three targets, and the right sum
cat > "$out/sw.c" <<'EOF'
__attribute__((noinline)) static int f(int *p, int v, int w, int c)
{
    return v * w + c + p[0];
}
__attribute__((noinline)) static int run(int *p, int k, const char *s)
{
    int acc = 0;
    while (*s) {
        const char *q = s + 1;
        switch (*s) {
        case 'a': acc += f(p, p[1], 2, 48); break;
        case 'b': acc += f(p, p[2], 2, 48); break;
        case 'c': acc += f(p, p[3] * 7, 2, 48); break;
        case 'd': acc += f(p, k, 2, 48); break;
        case 'e': acc += f(p, p[1], 3, 48); break;
        case 'f': acc += f(p, k, 2, 32); break;
        default: acc += 1;
        }
        s = q;
    }
    return acc;
}
int main(void)
{
    int p[4] = { 1, 2, 3, 4 };
    return run(p, 5, "abcdefx") == 371 ? 42 : 1;
}
EOF
for t in thumbv7em-none-eabi riscv32-unknown-elf x86_64-elf; do
    "$EMBCC" inspect ir -Os --target=$t "$out/sw.c" > "$out/sw-$t.out" 2>&1 ||
        { cat "$out/sw-$t.out"; fail "sw.c on $t: inspect"; }
    n=$(sed -n '/^func @run/,/^}/p' "$out/sw-$t.out" | grep -c 'call @f')
    [ "$n" = 2 ] || { cat "$out/sw-$t.out"; fail "sw.c on $t: $n calls of f, not 2"; }
done
"$EMBCC" --target=x86_64-elf -Os -c "$out/sw.c" -o "$out/sw.o" || fail "sw.c: compile"
tests/harness/x86_64/link.sh -o "$out/sw" "$out/sw.o" > "$out/sw-ln.log" 2>&1 || fail "sw.c: link"
tests/harness/x86_64/run.sh "$out/sw" > /dev/null 2>&1
rc=$?
[ "$rc" = 42 ] || fail "sw.c exits $rc, not 42"
echo "C: the switch's cases share one call through a merge temp, and the sum is C's"

command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "SKIP the C half: no $OBJDUMP"; exit 0; }
"$EMBCC" --target=thumbv7em-none-eabi -Os -Ilib/libc/include \
    -c lib/libc/src/stdio/scan.c -o "$out/scan.o" || fail "scan.c: compile"
"$OBJDUMP" -d --triple=thumbv7em "$out/scan.o" |
    sed -n '/<store_int>:/,/^$/p' > "$out/store_int.dis"
n=$(grep -cE '^ +[0-9a-f]+:.*[[:space:]]str(\.w)?[[:space:]]' "$out/store_int.dis")
[ "$n" -le 6 ] || { cat "$out/store_int.dis"; fail "store_int: $n word stores, the cases are not merged"; }
echo "C: store_int's identical cases share one copy ($n word stores)"
