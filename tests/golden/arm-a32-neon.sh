#!/bin/sh
# A32 Advanced SIMD (NEON): src/arch/thumb/neon.c's forms, which
# tools/neon-isa learns from llvm-mc, against llvm-mc itself -- every
# kind of operand a form can have (Q, D and S registers, scalars, register
# lists spaced and lane-indexed, alignments and writeback, immediates of
# each modified-immediate class, float immediates, shifts, vext's element
# scaling) and the type spellings llvm-mc takes for one another (vadd.s32
# is vadd.i32). And what no form takes is refused by name.
echo "TEST-MARKER arm-a32-neon"
EMBCC=${EMBCC:-./embcc}
MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
out=tests/golden/out/arm-a32-neon
rm -rf "$out"; mkdir -p "$out"
command -v "$MC" >/dev/null 2>&1 || { echo "SKIP: no llvm-mc"; exit 0; }

cat > "$out/n.s" <<'EOF'
	.syntax unified
	.arm
	.fpu neon-vfpv4
	.text
	vadd.i32	q0, q1, q2
	vadd.s32	q8, q9, q15
	vsub.f32	d16, d17, d31
	vmla.i32	q10, q9, q8
	vmul.f32	q0, q1, d2[1]
	vmla.f32	d0, d1, d15[1]
	vqdmulh.s16	q1, q2, d3[2]
	vld1.32	{d0, d1}, [r0]
	vld1.32	{d16-d19}, [r1:256]!
	vld1.8	{d0[3]}, [r2], r3
	vld2.16	{d0[], d1[]}, [r4:32]
	vld3.8	{d0, d2, d4}, [r5]!
	vld4.32	{d1[1], d3[1], d5[1], d7[1]}, [r6:128]
	vst1.64	{d20, d21}, [lr]!
	vst4.8	{d0, d1, d2, d3}, [r9:64], r10
	vmov.i32	q8, #0x3
	vmov.i32	d16, #0x80000000
	vmov.i32	q4, #0xff00
	vmov.i32	d1, #0x1ffff
	vmov.i16	d2, #0x200
	vmov.i8	q3, #0x7f
	vmov.i64	q5, #0xff00ff0000ff00ff
	vmvn.i32	d1, #0
	vbic.i16	q1, #0x100
	vorr.i32	d4, #0x10000
	vmov.f32	q6, #1.000000e+00
	vmov.f32	d7, #-2.5
	vshr.u32	q0, q1, #3
	vshl.i64	d0, d1, #63
	vqrshrn.s32	d0, q1, #16
	vext.8	q8, q8, q8, #8
	vext.32	d0, d1, d2, #1
	vext.16	q0, q1, q2, #6
	vdup.32	q0, r1
	vdup.8	d2, d3[7]
	vmov.32	d0[1], r0
	vmov.u8	r0, d1[7]
	vmov	r0, r1, d16
	vmov	d17, r2, r3
	vtbl.8	d0, {d1, d2, d3}, d4
	vzip.16	q0, q1
	vcvt.f32.s32	q0, q1
	vcvt.s32.f32	d0, d1, #16
	vcnt.8	q2, q3
	vpadd.i32	d0, d1, d2
	vabd.u16	q0, q1, q2
	vrecpe.f32	q8, q9
	vbsl	q0, q1, q2
	vswp	d0, d1
	vadd.f32	s0, s1, s2
EOF
"$EMBCC" --target=armv7a-none-eabihf -c "$out/n.s" -o "$out/n.o" 2> "$out/n.err" || {
    echo "EmbCC refused a NEON statement:"; head -5 "$out/n.err"; exit 1; }
"$MC" -triple=armv7a-none-eabihf -mattr=+neon,+vfp4 -filetype=obj "$out/n.s" -o "$out/n.ref.o" || {
    echo "llvm-mc refused the test's own statements"; exit 1; }
for f in n n.ref; do
    "$OBJDUMP" -d "$out/$f.o" | grep -E '^ +[0-9a-f]+:' > "$out/$f.txt"
done
n=$(wc -l < "$out/n.ref.txt" | tr -d ' ')
cmp -s "$out/n.txt" "$out/n.ref.txt" || {
    echo "NEON statements encode otherwise than llvm-mc's:"
    diff "$out/n.txt" "$out/n.ref.txt" | head -12; exit 1; }
echo "$n NEON statements encode as llvm-mc's: registers, scalars, lists,"
echo "alignment, every immediate class, vext's scaling, type synonyms"

# ---- refusals ------------------------------------------------------------------
refs() {
    printf '\t.syntax unified\n\t.arm\n\t.fpu neon\n\t%s\n' "$2" > "$out/bad.s"
    if "$EMBCC" --target=armv7a-none-eabihf -c "$out/bad.s" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$3" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refs "an immediate no modified-immediate class makes" 'vmov.i32 q0, #0x12345678' "is not one any form of vmov.i32"
refs "a shift past the element" 'vshr.u32 q0, q1, #33' "operand 3, an immediate (33)"
refs "a register list past d31" 'vld1.32 {d30, d31, d32}, [r0]' "no register d32"
echo "three statements no form takes are refused by name"
