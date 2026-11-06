#!/bin/sh
# AVX-512 in the x86-64 assembler: the EVEX forms (and the opmask
# instructions' VEX ones) src/arch/x86_64/evex.inc holds, which
# tools/evex-isa learns from llvm-mc, against llvm-mc -- zmm and the
# registers past 15, masks and {z}, broadcasts, rounding and {sae},
# displacements on both sides of the disp8*N boundary, gathers (a vector
# index), comparisons whose predicate is in the mnemonic, the opmask
# instructions, and an AVX-512 mnemonic's VEX form where no operand needs
# EVEX (llvm-mc's choice). And a misuse is refused by name.
EMBCC=${EMBCC:-./embcc}
MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
out=tests/golden/out/x86-avx512-asm
rm -rf "$out"; mkdir -p "$out"
command -v "$MC" >/dev/null 2>&1 || { echo "SKIP: no llvm-mc"; exit 0; }

cat > "$out/a.s" <<'EOF'
	.text
	vaddps	%zmm2, %zmm1, %zmm0
	vaddps	%zmm18, %zmm25, %zmm31 {%k3} {z}
	vaddps	(%rax){1to16}, %zmm1, %zmm2 {%k1}
	vaddpd	{rn-sae}, %zmm2, %zmm1, %zmm0
	vmaxss	{sae}, %xmm2, %xmm1, %xmm17
	vmovdqu64	8128(%rdi), %zmm3
	vmovdqu64	8192(%rdi), %zmm3
	vmovdqu64	-8192(%rdi,%r9,8), %zmm3
	vmovdqu8	%zmm1, 64(%rsp) {%k2}
	vpbroadcastq	%rax, %zmm0
	vpbroadcastd	%eax, %ymm17
	vpbroadcastb	%xmm3, %xmm20 {%k1} {z}
	vpternlogd	$0xe8, %zmm2, %zmm1, %zmm0
	vpcmpleuq	%zmm1, %zmm2, %k1 {%k2}
	vpcmpnltuq	(%rax), %zmm2, %k3
	vcmpltsd	-16(%rbp), %xmm1, %k1
	vptestmq	%zmm1, %zmm1, %k0
	vpgatherdd	(%rax,%zmm1,4), %zmm0 {%k1}
	vscatterqpd	%zmm5, 16(%rbx,%zmm9,8) {%k2}
	vpmovqd	%zmm1, %ymm2
	vpminuq	%xmm1, %xmm2, %xmm3
	vcvtusi2sd	%rax, %xmm1, %xmm2
	vfixupimmpd	$23, {sae}, %zmm27, %zmm24, %zmm4 {%k4}
	vpdpbusd	%xmm1, %xmm2, %xmm3
	vgf2p8mulb	%ymm1, %ymm2, %ymm3
	vpermt2q	%zmm3, %zmm2, %zmm1
	vcompresspd	%zmm1, (%rax) {%k1}
	vpandq	-64(%rip), %zmm1, %zmm2
	kmovw	%k1, %eax
	kmovq	%rax, %k2
	kshiftlb	$3, %k1, %k2
	korb	%k1, %k2, %k3
	kunpckbw	%k1, %k2, %k3
	kortestw	%k1, %k2
EOF
"$EMBCC" --target=x86_64-elf -c "$out/a.s" -o "$out/a.o" 2> "$out/a.err" || {
    echo "EmbCC refused an AVX-512 statement:"; head -5 "$out/a.err"; exit 1; }
"$MC" -triple=x86_64 -mattr=+avx512f,+avx512bw,+avx512dq,+avx512vl,+avx512vnni,+gfni \
    -filetype=obj "$out/a.s" -o "$out/a.ref.o" || { echo "llvm-mc refused the test"; exit 1; }
for f in a a.ref; do
    "$OBJDUMP" -d "$out/$f.o" | grep -E '^ +[0-9a-f]+:' > "$out/$f.txt"
done
cmp -s "$out/a.txt" "$out/a.ref.txt" || {
    echo "AVX-512 statements encode otherwise than llvm-mc's:"
    diff "$out/a.txt" "$out/a.ref.txt" | head -12; exit 1; }
echo "$(grep -c . "$out/a.ref.txt") AVX-512 and opmask instructions encode as llvm-mc's"

refs() {
    printf '\t%s\n' "$2" > "$out/bad.s"
    if "$EMBCC" --target=x86_64-elf -c "$out/bad.s" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$3" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refs "zeroing without a mask" 'vaddps %zmm1, %zmm2, %zmm3 {z}' "{z} wants a mask"
refs "a gather without a mask" 'vpgatherdd (%rax,%zmm1,4), %zmm0' "wants a mask"
refs "a mask on a form without one" 'vptestmq %zmm1, %zmm1, %k0 {%k1} {z}' "takes no zeroing"
refs "a broadcast of the wrong count" 'vaddps (%rax){1to8}, %zmm1, %zmm2' "vaddps"
echo "four misuses are refused by name"
