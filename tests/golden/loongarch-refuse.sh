#!/bin/sh
# The LoongArch64 target: what it is, and what it refuses BY NAME.
#
# The triples and -dumpmachine, the object's header (EM_LOONGARCH, ELF64,
# little-endian, e_flags 0x41 = LP64S soft float | object ABI v1, RELA),
# -S that llvm-mc reassembles to the same bytes and relocations, the
# machine options a LoongArch build passes -- the one configuration EmbCC
# emits is accepted, every other one refused -- the constructs the backend
# does not lower, each with a message naming it rather than code that does
# something else, and the objects embld will not link. (Assembly is
# loongarch-asm.sh's.)
set -u
echo "TEST-MARKER loongarch-refuse"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
MC=${EMBCC_LLVM_MC:-llvm-mc}
T=loongarch64-unknown-elf
out=tests/golden/out/loongarch-refuse
rm -rf "$out"; mkdir -p "$out"

# ---- the target ---------------------------------------------------------
for t in loongarch64-unknown-elf loongarch64-none-elf loongarch64-elf \
         loongarch64; do
    m=$("$EMBCC" --target=$t -dumpmachine) || { echo "--target=$t refused"; exit 1; }
    [ "$m" = $T ] || { echo "--target=$t is '$m'"; exit 1; }
done
for t in loongarch32-unknown-elf loongarch64-linux-gnu; do
    if "$EMBCC" --target=$t -dumpmachine > /dev/null 2>&1; then
        echo "--target=$t was accepted"; exit 1
    fi
done
printf 'int f(int x) { return x + 1; }\n' > "$out/f.c"
"$EMBCC" --target=$T -c "$out/f.c" -o "$out/f.o" || { echo "-c failed"; exit 1; }
if command -v "$RE" >/dev/null 2>&1; then
    "$RE" -h -S "$out/f.o" > "$out/f.hdr" || exit 1
    grep -q 'Class:.*ELF64' "$out/f.hdr" &&
    grep -q 'Data:.*little endian' "$out/f.hdr" &&
    grep -q 'Machine:.*LoongArch' "$out/f.hdr" &&
    grep -q 'Flags:.*0x41' "$out/f.hdr" || {
        echo "the object header is not ELF64 LE LoongArch, e_flags 0x41:"
        grep -E 'Class|Data|Machine|Flags' "$out/f.hdr"; exit 1; }
    printf 'extern int g; extern int h(int);\nint f(void) { return h(g); }\n' > "$out/r.c"
    "$EMBCC" --target=$T -c "$out/r.c" -o "$out/r.o" &&
    "$RE" -S "$out/r.o" | grep -q '\.rela\.text *RELA ' &&
    "$RE" -r "$out/r.o" > "$out/r.rel" &&
    grep -q 'R_LARCH_B26 .* h' "$out/r.rel" &&
    grep -q 'R_LARCH_PCALA_HI20 .* g' "$out/r.rel" &&
    grep -q 'R_LARCH_PCALA_LO12 .* g' "$out/r.rel" || {
        echo "the relocations are not RELA B26 and the PCALA pair:"
        cat "$out/r.rel"; exit 1; }
    echo "an ELF64 little-endian EM_LOONGARCH object, e_flags 0x41, RELA,"
    echo "calls by R_LARCH_B26 and addresses by the PCALA pair"
fi

# -S, reassembled by llvm-mc: the same .text bytes, the same relocations
if command -v "$MC" >/dev/null 2>&1 && command -v "$RE" >/dev/null 2>&1; then
    for opt in -O0 -O2; do
        "$EMBCC" --target=$T $opt -S tests/exec/switch.c -o "$out/s.s" &&
        "$EMBCC" --target=$T $opt -c tests/exec/switch.c -o "$out/c.o" &&
        "$MC" -triple=loongarch64 -filetype=obj "$out/s.s" -o "$out/s.o" || {
            echo "-S $opt did not reassemble"; exit 1; }
        llvm-objcopy -O binary --only-section=.text "$out/c.o" "$out/c.bin"
        llvm-objcopy -O binary --only-section=.text "$out/s.o" "$out/s.bin"
        cmp -s "$out/c.bin" "$out/s.bin" || {
            echo "-S $opt reassembles to different .text bytes"; exit 1; }
        "$RE" -r "$out/c.o" | awk '/R_LARCH/ { print $1, $3 }' | sort \
            > "$out/c.rel"
        "$RE" -r "$out/s.o" | awk '/R_LARCH/ { print $1, $3 }' | sort \
            > "$out/s.rel"
        cmp -s "$out/c.rel" "$out/s.rel" || {
            echo "-S $opt reassembles to different relocations:"
            diff "$out/c.rel" "$out/s.rel" | head; exit 1; }
    done
    echo "-S reassembles with llvm-mc to the object's bytes and relocations"
fi

# ---- the options -----------------------------------------------------------
for o in -mabi=lp64s -msoft-float -mfpu=none -march=loongarch64 \
         -march=la64v1.0 -march=la464 -mtune=la664 -mcmodel=normal \
         -mcmodel=medium -mrelax -mno-relax -mno-strict-align -mno-lsx \
         -mno-lasx; do
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
refopt -mabi=lp64d 'soft-float LP64S convention'
refopt -mabi=lp64f 'soft-float LP64S convention'
refopt -mfpu=64 'uses no FPU'
refopt -mdouble-float 'soft-float LP64S code'
refopt -march=la32v1.0 'not an LA64 architecture'
refopt -mcmodel=extreme 'normal code model'
refopt -mstrict-align '-mstrict-align is not supported'
refopt -mlsx 'no LSX or LASX'
refopt -mlasx 'no LSX or LASX'
refopt -mbig-endian 'little-endian'
for o in -funwind-tables -fasynchronous-unwind-tables -fexceptions; do
    refopt $o "unwind tables are not supported for $T yet"
done
echo "the LP64S/soft-float/normal-model flags are accepted, others refused"

# ---- the constructs ----------------------------------------------------------
refc() {            # refc WHAT PATTERN SOURCE [FLAGS]
    printf '%s\n' "$3" > "$out/bad.c"
    if "$EMBCC" --target=$T ${4:-} -c "$out/bad.c" -o /dev/null \
           2> "$out/bad.err"; then
        echo "$1 was accepted"; exit 1
    fi
    grep -q -- "$2" "$out/bad.err" || {
        echo "$1 was refused, but not by name:"; cat "$out/bad.err"; exit 1; }
}
for O in -O0 -O2; do
    refc "a 16-byte atomic ($O)" 'a sixteen-byte atomic' \
        '__int128 x; int f(void){ __int128 e = 0; return __atomic_compare_exchange_n(&x, &e, 1, 0, 5, 5); }' $O
    refc "a computed goto ($O)" 'a computed goto' \
        'int f(int i){ void *t[2]; t[0] = &&a; t[1] = &&b; goto *t[i]; a: return 1; b: return 2; }' $O
    refc "__builtin_return_address ($O)" '__builtin_frame_address or __builtin_return_address' \
        'void *f(void){ return __builtin_return_address(0); }' $O
    refc "__builtin_frame_address ($O)" '__builtin_frame_address or __builtin_return_address' \
        'void *f(void){ return __builtin_frame_address(0); }' $O
done
refc "an interrupt handler" '__attribute__((interrupt)) is not supported' \
    'void __attribute__((interrupt)) f(void){}'
refc "a 64-aligned scalar local" 'needs 64-byte alignment and the stack only guarantees 16' \
    'int g(int *); int f(void){ _Alignas(64) int x = 1; return g(&x); }'
# C++ without exceptions is LP64 like the targets its front end lays out
# for, and compiles; with them it needs the .eh_frame EmbCC does not write.
printf 'struct A { int v; int get() const { return v * 2; } };\nint f(A a) { return a.get(); }\n' > "$out/c.cc"
"$EMBCC" --target=$T -fno-exceptions -c "$out/c.cc" -o "$out/cc.o" || {
    echo "C++ with -fno-exceptions was refused"; exit 1; }
if "$EMBCC" --target=$T -c "$out/c.cc" -o /dev/null 2> "$out/cxx.err"; then
    echo "C++ with exceptions was accepted"; exit 1
fi
grep -q "C++ without -fno-exceptions" "$out/cxx.err" || {
    echo "C++ with exceptions was refused, but not by name:"
    cat "$out/cxx.err"; exit 1; }
echo "sixteen-byte atomics, computed goto, the frame and return"
echo "address, interrupt functions, an over-aligned scalar and C++ exceptions"
echo "are each refused by name (assembly's refusals are loongarch-asm.sh's)"

# ---- what embld will not link ------------------------------------------------
CLANG=${EMBCC_REF_CLANG_LOONGARCH:-clang}
if command -v "$CLANG" >/dev/null 2>&1 &&
   "$CLANG" --target=$T -msoft-float -fsyntax-only -x c /dev/null 2>/dev/null; then
    "$CLANG" --target=$T -c "$out/f.c" -o "$out/hard.o" || exit 1
    if "$EMBLD" -e f -Ttext 0x1000000 "$out/hard.o" -o "$out/x.elf" \
           2> "$out/ld.err"; then
        echo "a double-float (lp64d) object was linked"; exit 1
    fi
    grep -q 'for the double-float ABI' "$out/ld.err" || {
        echo "an lp64d object was refused, but not by name:"
        cat "$out/ld.err"; exit 1; }
    printf 'extern int g; int h(void) { return g; }\n' > "$out/x.c"
    printf 'int g = 1;\n' > "$out/g.c"
    "$CLANG" --target=$T -msoft-float -mcmodel=extreme -c "$out/x.c" \
        -o "$out/x.o" &&
    "$EMBCC" --target=$T -c "$out/g.c" -o "$out/g.o" || exit 1
    if "$EMBLD" -e h -Ttext 0x1000000 "$out/x.o" "$out/g.o" \
           -o "$out/x.elf" 2> "$out/ld.err"; then
        echo "an extreme-code-model object was linked"; exit 1
    fi
    grep -q 'mcmodel=extreme is not linked' "$out/ld.err" || {
        echo "an extreme-model object was refused, but not by name:"
        cat "$out/ld.err"; exit 1; }
    echo "embld refuses an lp64d object and the extreme code model by name"
fi
