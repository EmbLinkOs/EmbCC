#!/bin/sh
# The ARMv7-A (A32) target: what it is, and what it refuses BY NAME.
#
# The triples and -dumpmachine; the object (ELF32 little-endian EM_ARM,
# e_flags EF_ARM_EABI_VER5, even function symbols and an `$a` mapping
# symbol -- ARM state, no Thumb bit and no `$t` -- the A32 relocation
# types, and .ARM.attributes saying v7, profile A, the ARM ISA permitted,
# soft-float arguments); the machine options an ARMv7-A build passes -- the
# configuration EmbCC emits accepted, every other refused; and the
# constructs the backend does not lower, each with a message naming it
# rather than code that does something else.
set -u
echo "TEST-MARKER arm-a32-refuse"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
T=armv7a-none-eabi
out=tests/golden/out/arm-a32-refuse
rm -rf "$out"; mkdir -p "$out"

# ---- the target ---------------------------------------------------------
for t in armv7a-none-eabi armv7a armv7-none-eabi armv7a-unknown-none-eabi; do
    m=$("$EMBCC" --target=$t -dumpmachine) || { echo "--target=$t refused"; exit 1; }
    [ "$m" = $T ] || { echo "--target=$t is '$m'"; exit 1; }
done
for t in armv7r-none-eabi armebv7a-none-eabi; do
    if "$EMBCC" --target=$t -dumpmachine > /dev/null 2>&1; then
        echo "--target=$t was accepted"; exit 1
    fi
done
cat > "$out/f.c" <<'EOF'
extern int g;
int ext(int);
int f(int x) { return ext(x) + g; }
int (*fp(void))(int) { return f; }
EOF
"$EMBCC" --target=$T -c "$out/f.c" -o "$out/f.o" || { echo "-c failed"; exit 1; }
if command -v "$RE" >/dev/null 2>&1; then
    "$RE" -h -s -r -A "$out/f.o" > "$out/f.hdr" || exit 1
    grep -q 'Class:.*ELF32' "$out/f.hdr" &&
    grep -q 'Data:.*little endian' "$out/f.hdr" &&
    grep -q 'Machine:.*ARM' "$out/f.hdr" &&
    grep -q 'Flags:.*0x5000000' "$out/f.hdr" || {
        echo "the object header is not ELF32 LE ARM with EF_ARM_EABI_VER5:"
        grep -E 'Class|Data|Machine|Flags' "$out/f.hdr"; exit 1; }
    # f at 0: an ARM-state function's symbol is even (a Thumb one is odd)
    grep -q '00000000 .* FUNC .* f$' "$out/f.hdr" || {
        echo "f's symbol is not the even address of ARM code:"
        grep ' f$' "$out/f.hdr"; exit 1; }
    grep -q ' \$a$' "$out/f.hdr" && ! grep -q ' \$t$' "$out/f.hdr" || {
        echo "the mapping symbols are not ARM's (\$a, and no \$t)"; exit 1; }
    for r in R_ARM_CALL R_ARM_MOVW_ABS_NC R_ARM_MOVT_ABS; do
        grep -q "$r " "$out/f.hdr" || {
            echo "no $r relocation: not A32 code"; exit 1; }
    done
    if grep -q 'R_ARM_THM_' "$out/f.hdr"; then
        echo "a Thumb relocation in an ARM-state object"; exit 1
    fi
    for a in 'TagName: CPU_arch$' 'Description: ARM v7$' \
             'Description: Application' 'TagName: ARM_ISA_use' \
             'Value: 7-A'; do
        grep -q "$a" "$out/f.hdr" || {
            echo ".ARM.attributes lacks '$a'"; exit 1; }
    done
    grep -A3 'TagName: ARM_ISA_use' "$out/f.hdr" | grep -q 'Permitted' || {
        echo ".ARM.attributes does not permit the ARM ISA"; exit 1; }
    grep -A3 'TagName: ABI_VFP_args' "$out/f.hdr" | grep -q 'AAPCS$' || {
        echo ".ARM.attributes does not say base-standard (soft-float) arguments"
        exit 1; }
    echo "an ELF32 LE EM_ARM object: ARM-state symbols and \$a, R_ARM_CALL and"
    echo "  MOVW/MOVT_ABS, .ARM.attributes v7-A with the ARM ISA, soft float"
fi
# the predefined macros say ARM state, ARMv7-A, soft float, no divide
"$EMBCC" --target=$T --dump-predef > "$out/predef.txt" || exit 1
for m in '__arm__ 1' '__ARM_ARCH 7' '__ARM_ARCH_7A__ 1' "__ARM_ARCH_PROFILE 'A'" \
         '__ARM_ARCH_ISA_ARM 1' '__ARM_EABI__ 1' '__SOFTFP__ 1'; do
    grep -q "^#define $m\$" "$out/predef.txt" || {
        echo "--dump-predef lacks $m"; exit 1; }
done
for m in __thumb__ __thumb2__ __ARM_FEATURE_IDIV __ARM_FP __ARM_NEON; do
    if grep -q "^#define $m " "$out/predef.txt"; then
        echo "--dump-predef defines $m, which this code is not"; exit 1
    fi
done

# ---- the options -----------------------------------------------------------
for o in -marm -mcpu=cortex-a7 -mcpu=cortex-a8 -mcpu=cortex-a9 \
         -mcpu=cortex-a15 -mfloat-abi=soft -mfpu=none -mabi=aapcs \
         -mthumb-interwork -munaligned-access; do
    "$EMBCC" --target=$T $o -c "$out/f.c" -o /dev/null 2> "$out/opt.err" || {
        echo "$o was refused:"; cat "$out/opt.err"; exit 1; }
done
refopt() {          # refopt OPTION PATTERN
    if "$EMBCC" --target=$T "$1" -c "$out/f.c" -o /dev/null 2> "$out/opt.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/opt.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/opt.err"; exit 1; }
}
refopt -mthumb '-mthumb is not supported on armv7a-none-eabi'
refopt -mcpu=cortex-m4 '-mcpu=cortex-m4 is not supported on armv7a-none-eabi'
refopt -mcpu=cortex-r5 '-mcpu=cortex-r5 is not supported on armv7a-none-eabi'
refopt -mfpu=neon '-mfpu=neon is not supported on armv7a-none-eabi'
refopt -mfpu=fpv4-sp-d16 '-mfpu=fpv4-sp-d16 is not supported on armv7a-none-eabi'
refopt -mfloat-abi=hard '-mfloat-abi=hard needs an FPU to use'
refopt -mfloat-abi=softfp '-mfloat-abi=softfp needs an FPU to use'
refopt -mno-unaligned-access '-mno-unaligned-access is not supported'
refopt -mbig-endian 'little-endian'
echo "the ARMv7-A/ARM-state/soft-float flags are accepted, others refused"

# ---- hard float: the VFPv3/VFPv4 units, NEON's SIMD refused ----------------
m=$("$EMBCC" --target=armv7a-none-eabihf -dumpmachine)
[ "$m" = armv7a-none-eabihf ] || { echo "armv7a-none-eabihf is '$m'"; exit 1; }
m=$("$EMBCC" --target=$T -mfpu=vfpv4 -mfloat-abi=hard -dumpmachine)
[ "$m" = armv7a-none-eabihf ] || {
    echo "-mfpu=vfpv4 -mfloat-abi=hard names itself '$m'"; exit 1; }
predef() {          # predef WANT-PRESENT WANT-ABSENT FLAGS...
    w=$1; a=$2; shift 2
    "$EMBCC" "$@" --dump-predef > "$out/pd.txt" || exit 1
    for m in $w; do grep -q "^#define $m " "$out/pd.txt" || {
        echo "$*: no $m"; exit 1; }; done
    for m in $a; do ! grep -q "^#define $m " "$out/pd.txt" || {
        echo "$*: defines $m"; exit 1; }; done
}
predef "__ARM_FP __ARM_VFPV3__ __ARM_PCS_VFP" \
       "__SOFTFP__ __ARM_VFPV4__ __ARM_NEON __ARM_FPV5__" \
       --target=armv7a-none-eabihf
grep -q '^#define __ARM_FP 0xc$' "$out/pd.txt" || { echo "VFPv3's __ARM_FP is not 0xc"; exit 1; }
predef "__ARM_FP __ARM_VFPV4__ __ARM_FEATURE_FMA __ARM_PCS" \
       "__SOFTFP__ __ARM_PCS_VFP __ARM_NEON" \
       --target=$T -mfpu=vfpv4-d16 -mfloat-abi=softfp
grep -q '^#define __ARM_FP 0xe$' "$out/pd.txt" || { echo "VFPv4's __ARM_FP is not 0xe"; exit 1; }
predef "__SOFTFP__" "__ARM_FP __ARM_PCS_VFP" --target=armv7a-none-eabihf -mfloat-abi=soft
if command -v "$RE" >/dev/null 2>&1; then
    for c in "vfpv3-d16:VFPv3-D16" "vfpv3:VFPv3" "vfpv4-d16:VFPv4-D16" "vfpv4:VFPv4"; do
        u=${c%%:*}; d=${c#*:}
        "$EMBCC" --target=armv7a-none-eabihf -mfpu=$u -c "$out/f.c" \
            -o "$out/hf.o" || exit 1
        "$RE" -A "$out/hf.o" > "$out/hf.attr"
        grep -A2 'TagName: FP_arch' "$out/hf.attr" | grep -q "Description: $d\$" || {
            echo "-mfpu=$u: Tag_FP_arch is not $d"; exit 1; }
        grep -A2 'TagName: ABI_VFP_args' "$out/hf.attr" | grep -q 'AAPCS VFP' || {
            echo "-mfpu=$u -mfloat-abi=hard: Tag_ABI_VFP_args is not AAPCS VFP"
            exit 1; }
    done
fi
echo "armv7a-none-eabihf and -mfpu=vfpv3[-d16]/vfpv4[-d16]: the macros and"
echo "  attributes of each unit; NEON and the Cortex-M units refused"

# ---- the constructs ----------------------------------------------------------
refc() {            # refc WHAT PATTERN SOURCE
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
refc "an 8-byte atomic read-modify-write" 'an atomic wider than four bytes' \
    'long long x; long long f(void){ return __atomic_fetch_add(&x, 1, 5); }'
refc "an 8-byte atomic load" 'an atomic access of 8 bytes is not one access' \
    'long long x; long long f(void){ return __atomic_load_n(&x, 5); }'
refc "a computed goto" 'cannot lower a computed goto' \
    'int f(int i){ void *t[2]; t[0] = &&a; t[1] = &&b; goto *t[i]; a: return 1; b: return 2; }'
refc "__builtin_return_address" '__builtin_frame_address or __builtin_return_address' \
    'void *f(void){ return __builtin_return_address(0); }'
refc "__builtin_frame_address" '__builtin_frame_address or __builtin_return_address' \
    'void *f(void){ return __builtin_frame_address(0); }'
refc "__int128" '__int128 does not exist on this target' '__int128 x;'
refc "an interrupt handler" '__attribute__((interrupt)) is not supported' \
    'void __attribute__((interrupt("IRQ"))) f(void){}'
refc "an M-profile special register in asm" 'the M-profile special registers do not exist here' \
    'int f(void){ int r; __asm__ volatile("mrs %0, primask" : "=r"(r)); return r; }'
refc "cbz in asm" 'cbz is a Thumb instruction' \
    'int f(int x){ __asm__ volatile("cbz %0, .+8" : : "r"(x)); return x; }'
refc "a condition on a two-instruction statement" 'a condition cannot cover them' \
    'int f(int x){ __asm__ volatile("addeq %0, %0, #0x101" : "+r"(x)); return x; }'
refc ".thumb_func in file-scope asm" '.thumb_func: EmbCC assembles ARM (A32) code' \
    '__asm__(".thumb_func\nfoo:\n bx lr\n");'
refc ".thumb in file-scope asm" '.thumb: EmbCC assembles ARM (A32) code' \
    '__asm__(".thumb\nfoo:\n bx lr\n");'
# C++ compiles here (the ARM C++ ABI, tests/golden/cxx-embedded.sh), but
# not with exceptions: EmbCC writes no EHABI unwind tables
printf 'int f(int x) { return x; }\n' > "$out/c.cc"
if "$EMBCC" --target=$T -c "$out/c.cc" -o /dev/null 2> "$out/cxx.err"; then
    echo "C++ with exceptions was accepted"; exit 1
fi
grep -q 'C++ exceptions are not supported for armv7a-none-eabi' "$out/cxx.err" || {
    echo "C++ exceptions were refused, but not by name:"; cat "$out/cxx.err"
    exit 1; }
"$EMBCC" --target=$T -fno-exceptions -c "$out/c.cc" -o /dev/null || {
    echo "C++ with -fno-exceptions was refused"; exit 1; }
echo "8-byte atomics, computed goto, the frame and return address, __int128,"
echo "interrupt functions, an over-aligned scalar, Thumb and M-profile asm, a"
echo "condition on a sequence and C++ exceptions are each refused by name"

# ---- and what ARM state does accept in asm, run through llvm-mc ----------
MC=${EMBCC_LLVM_MC:-llvm-mc}
OBJCOPY=${EMBCC_LLVM_OBJCOPY:-llvm-objcopy}
if command -v "$MC" >/dev/null 2>&1 && command -v "$OBJCOPY" >/dev/null 2>&1; then
    cat > "$out/sys.s" <<'EOF'
	mrs r0, apsr
	msr cpsr_c, r1
	msr cpsr_fsxc, r2
	mrc p15, #0, r3, c1, c0, #0
	mcr p15, #0, r4, c12, c0, #0
	svc #0x123456
	bkpt #0xabcd
	udf #7
	cpsid if
	moveq r0, #1
	addne r1, r2, r3
	ldrbhi r5, [r6, #-12]
	strhlt r7, [r8, #6]
	bxle lr
	popgt {r4, r5, pc}
	ldrex r0, [r1]
	strex r2, r3, [r4]
	dmb sy
	wfi
EOF
    { printf '__asm__(\n'; sed 's/.*/"&\\n"/' "$out/sys.s"; printf ');\n'; } \
        > "$out/sys.c"
    "$EMBCC" --target=$T -c "$out/sys.c" -o "$out/sys.o" || {
        echo "ARM-state asm did not assemble"; exit 1; }
    "$MC" -triple=armv7a-none-eabi -filetype=obj "$out/sys.s" -o "$out/ref.o" &&
    "$OBJCOPY" -O binary --only-section=.text "$out/sys.o" "$out/sys.bin" &&
    "$OBJCOPY" -O binary --only-section=.text "$out/ref.o" "$out/ref.bin" || exit 1
    cmp -s "$out/sys.bin" "$out/ref.bin" || {
        echo "ARM-state asm encodes differently from llvm-mc's:"
        cmp -l "$out/sys.bin" "$out/ref.bin" | head -4; exit 1; }
    echo "the ARM-state system vocabulary and conditional statements encode as"
    echo "  llvm-mc's armv7a does"
fi
