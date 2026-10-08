#!/bin/sh
# -Os on ARM: one copy of a repeated block end (pass_tailmerge in
# src/opt/opt.c), and only where the copies compute the same thing.
#
# Text in, text out (EmbIR, optimized by `inspect ir -Os`): one function
# whose two blocks end in the same run must keep one copy of it; four
# where a rule says no -- different successors, a different operand, a
# value the run defines read after the block, a different variable
# written -- must keep both; and so must a loop's blocks where the path
# through the shared copy would take a branch more each trip (speed).
# Then C: scanf's store_int, five `case`s storing through
# `va_arg(ap, long *)`, has one such store left at -Os. Corpus programs on
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

# ops: the same instructions, one reading another value
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

copies() { grep -c 'mul.4s %0' "$out/$1.out"; }
opt pos
[ "$(copies pos)" = 1 ] || { cat "$out/pos.out"; fail "pos: the run is still there twice"; }
for c in exit ops local def loop; do
    opt $c
    [ "$(copies $c)" = 2 ] || { cat "$out/$c.out"; fail "$c: merged where the runs differ"; }
done
echo "IR: one copy where the runs are the same, two in five shapes where they are not"

command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "SKIP the C half: no $OBJDUMP"; exit 0; }
"$EMBCC" --target=thumbv7em-none-eabi -Os -Ilib/libc/include \
    -c lib/libc/src/stdio/scan.c -o "$out/scan.o" || fail "scan.c: compile"
"$OBJDUMP" -d --triple=thumbv7em "$out/scan.o" |
    sed -n '/<store_int>:/,/^$/p' > "$out/store_int.dis"
n=$(grep -cE '^ +[0-9a-f]+:.*[[:space:]]str(\.w)?[[:space:]]' "$out/store_int.dis")
[ "$n" -le 6 ] || { cat "$out/store_int.dis"; fail "store_int: $n word stores, the cases are not merged"; }
echo "C: store_int's identical cases share one copy ($n word stores)"
