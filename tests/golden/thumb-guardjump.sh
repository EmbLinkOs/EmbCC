#!/bin/sh
# -Os on ARM: a rotated loop is entered at its test (pass_guardjump in
# src/opt/opt.c) instead of through the guard rotation left in front of it,
# and only when that is the same computation.
#
# Not in an inner loop either: there the jump in is a taken branch on
# every trip of the outer one, and tools/bench's -Os cycles grew by it.
#
# Text in, text out (EmbIR's form, optimized by `inspect ir -Os`): one loop
# the pass must rewrite -- its guard gone, a jump to a label before the
# latch's copy of the test -- and one loop per legality rule that it must
# leave alone, each a shape where entering at the test would compute
# something else. Then C: the string walk in tests/exec/size-shapes.c loads
# its byte once, at -Os, on ARMv7-M and ARMv6-M, and twice at -O2, where
# rotation keeps its guard. The program's VALUES are checked on the boards
# by exec-boards.sh and thumb-v6m-exec.sh.
set -u
echo "TEST-MARKER thumb-guardjump"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
out=tests/golden/out/thumb-guardjump
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
opt() { "$EMBCC" inspect ir -Os --target=thumbv7em-none-eabi "$out/$1.ir"             > "$out/$1.out" 2>&1 || { cat "$out/$1.out"; fail "$1: inspect"; }; }

# pos: the guard is the copy: entered at the test
cat > "$out/pos.ir" <<'EOF'
; EmbIR
func @pos nparams=2 nvars=2 vregs=8 labels=2 {
  %6 = const.4 0
  %2 = load.4:4 [%0]
  brz.4s %2 -> L1
L0:
  %6 = add.4 %6, #1
  %0 = add.4 %0, #4
  %3 = load.4:4 [%0]
  brnz.4s %3 -> L0
L1:
  ret %6
}
EOF

# g1: the guard loads through a different pointer
cat > "$out/g1.ir" <<'EOF'
; EmbIR
func @g1 nparams=2 nvars=2 vregs=8 labels=2 {
  %6 = const.4 0
  %2 = load.4:4 [%1]
  brz.4s %2 -> L1
L0:
  %6 = add.4 %6, #1
  %0 = add.4 %0, #4
  %3 = load.4:4 [%0]
  brnz.4s %3 -> L0
L1:
  ret %6
}
EOF

# g2: the guard's value is read after the loop
cat > "$out/g2.ir" <<'EOF'
; EmbIR
func @g2 nparams=2 nvars=2 vregs=8 labels=2 {
  %6 = const.4 0
  %2 = load.4:4 [%0]
  brz.4s %2 -> L1
L0:
  %6 = add.4 %6, #1
  %0 = add.4 %0, #4
  %3 = load.4:4 [%0]
  brnz.4s %3 -> L0
L1:
  %7 = add.4 %6, %2
  ret %7
}
EOF

# g3: a name the copy writes also holds a value from before the loop
cat > "$out/g3.ir" <<'EOF'
; EmbIR
func @g3 nparams=2 nvars=2 vregs=8 labels=2 {
  %6 = const.4 0
  %4 = mov.4 %1
  %2 = load.4:4 [%0]
  brz.4s %2 -> L1
L0:
  %6 = add.4 %6, %4
  %0 = add.4 %0, #4
  %4 = load.4:4 [%0]
  brnz.4s %4 -> L0
L1:
  ret %6
}
EOF

# g5: the guard branches on another value
cat > "$out/g5.ir" <<'EOF'
; EmbIR
func @g5 nparams=2 nvars=2 vregs=8 labels=2 {
  %6 = const.4 0
  %2 = load.4:4 [%0]
  brz.4s %1 -> L1
L0:
  %6 = add.4 %6, #1
  %0 = add.4 %0, #4
  %2 = load.4:4 [%0]
  brnz.4s %2 -> L0
L1:
  ret %6
}
EOF

# d1: the set-up code writes what the test reads
cat > "$out/d1.ir" <<'EOF'
; EmbIR
func @d1 nparams=2 nvars=2 vregs=8 labels=2 {
  %6 = const.4 0
  %2 = load.4:4 [%0]
  brz.4s %2 -> L1
  %0 = add.4 %0, #4
L0:
  %6 = add.4 %6, #1
  %0 = add.4 %0, #4
  %2 = load.4:4 [%0]
  brnz.4s %2 -> L0
L1:
  ret %6
}
EOF

# d2: the set-up code reads the value the guard computed
cat > "$out/d2.ir" <<'EOF'
; EmbIR
func @d2 nparams=2 nvars=2 vregs=8 labels=2 {
  %6 = const.4 0
  %2 = load.4:4 [%0]
  brz.4s %2 -> L1
  %7 = mov.4 %2
L0:
  %6 = add.4 %6, %7
  %0 = add.4 %0, #4
  %2 = load.4:4 [%0]
  brnz.4s %2 -> L0
L1:
  ret %6
}
EOF

# d3: what the set-up code writes is read after the loop
cat > "$out/d3.ir" <<'EOF'
; EmbIR
func @d3 nparams=2 nvars=2 vregs=8 labels=2 {
  %6 = const.4 0
  %7 = mov.4 %1
  %2 = load.4:4 [%0]
  brz.4s %2 -> L1
  %7 = add.4 %1, #2
L0:
  %6 = add.4 %6, %7
  %0 = add.4 %0, #4
  %2 = load.4:4 [%0]
  brnz.4s %2 -> L0
L1:
  %5 = add.4 %6, %7
  ret %5
}
EOF

# d4: code after the loop jumps back into the body
cat > "$out/d4.ir" <<'EOF'
; EmbIR
func @d4 nparams=2 nvars=2 vregs=8 labels=3 {
  %6 = const.4 0
  %7 = mov.4 %1
  %2 = load.4:4 [%0]
  brz.4s %2 -> L1
  %7 = add.4 %6, #2
L0:
  %6 = add.4 %6, %7
  %0 = add.4 %0, #4
  %2 = load.4:4 [%0]
  brnz.4s %2 -> L0
L1:
  brz.4s %1 -> L2
  %1 = const.4 0
  jmp L0
L2:
  ret %6
}
EOF

# n1: an inner loop (each entry would pay the jump: speed)
cat > "$out/n1.ir" <<'EOF'
; EmbIR
func @n1 nparams=2 nvars=2 vregs=8 labels=3 {
  %6 = const.4 0
L2:
  %2 = load.4:4 [%0]
  brz.4s %2 -> L1
L0:
  %6 = add.4 %6, #1
  %0 = add.4 %0, #4
  %3 = load.4:4 [%0]
  brnz.4s %3 -> L0
L1:
  %0 = add.4 %0, #4
  %1 = sub.4 %1, #1
  brnz.4s %1 -> L2
  ret %6
}
EOF

opt pos
grep -q 'brz' "$out/pos.out" && { cat "$out/pos.out"; fail "pos: the guard is still there"; }
grep -q 'jmp L' "$out/pos.out" || { cat "$out/pos.out"; fail "pos: no jump to the test"; }
n=0
for c in g1 g2 g3 g5 d1 d2 d3 d4 n1; do
    opt $c
    grep -q '^  brz.4s %[0-9]* -> L1' "$out/$c.out" ||
        { cat "$out/$c.out"; fail "$c: entered at the test where it must not be"; }
    n=$((n + 1))
done
echo "IR: the guard dropped where it is the test, kept in $n shapes where it is not"

command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "SKIP the C half: no $OBJDUMP"; exit 0; }
for cfg in "thumbv7em-none-eabi -Os 1" "thumbv6m-none-eabi -Os 1" "thumbv7em-none-eabi -O2 2"; do
    set -- $cfg
    "$EMBCC" --target=$1 $2 -c tests/exec/size-shapes.c -o "$out/s.o" || fail "$1 $2: compile"
    k=$("$OBJDUMP" -d --triple=${1%%-*} "$out/s.o" | sed -n '/<slen>:/,/^$/p' | grep -c 'ldrb')
    [ "$k" = "$3" ] || { "$OBJDUMP" -d --triple=${1%%-*} "$out/s.o" | sed -n '/<slen>:/,/^$/p'
                         fail "$1 $2: slen loads its byte $k times, want $3"; }
done
echo "C: a string walk loads its byte once at -Os (ARMv7-M, ARMv6-M), twice at -O2"
