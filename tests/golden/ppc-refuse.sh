#!/bin/sh
# The PowerPC target: what it is, and what it refuses BY NAME.
#
# The triples and -dumpmachine, the object's header (EM_PPC, ELF32,
# big-endian, e_flags 0, RELA relocations), -S that llvm-mc reassembles
# into the same code and relocations, the machine options an e500/e200
# build passes -- the one configuration EmbCC emits is accepted, every other
# one refused -- and the constructs the backend does not lower, each with a
# message naming it rather than code that does something else; and EmbLD's
# refusal of a little-endian PowerPC object.
set -u
echo "TEST-MARKER ppc-refuse"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
MC=${EMBCC_LLVM_MC:-llvm-mc}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
T=powerpc-none-eabi
out=tests/golden/out/ppc-refuse
rm -rf "$out"; mkdir -p "$out"

# ---- the target ---------------------------------------------------------
for t in powerpc-none-eabi powerpc-unknown-eabi powerpc-eabi powerpc ppc; do
    m=$("$EMBCC" --target=$t -dumpmachine) || { echo "--target=$t refused"; exit 1; }
    [ "$m" = powerpc-none-eabi ] || { echo "--target=$t is '$m'"; exit 1; }
done
printf 'int f(int x) { return x + 1; }\n' > "$out/f.c"
"$EMBCC" --target=$T -c "$out/f.c" -o "$out/f.o" || { echo "-c failed"; exit 1; }
if command -v "$RE" >/dev/null 2>&1; then
    "$RE" -h -S "$out/f.o" > "$out/f.hdr" || exit 1
    grep -q 'Class:.*ELF32' "$out/f.hdr" &&
    grep -q 'Data:.*big endian' "$out/f.hdr" &&
    grep -q 'Machine:.*PowerPC' "$out/f.hdr" &&
    grep -q 'Flags:.*0x0$' "$out/f.hdr" || {
        echo "the object header is not ELF32 big-endian PowerPC, e_flags 0:"
        grep -E 'Class|Data|Machine|Flags' "$out/f.hdr"; exit 1; }
    printf 'extern int g; int f(void) { return g; }\n' > "$out/r.c"
    "$EMBCC" --target=$T -c "$out/r.c" -o "$out/r.o" &&
    "$RE" -S "$out/r.o" | grep -q '\.rela\.text *RELA ' || {
        echo "the relocations are not RELA (.rela.text), as the SVR4 ABI's are"
        exit 1; }
    echo "an ELF32 big-endian EM_PPC object, e_flags 0, RELA"
fi

# ---- -S, reassembled ------------------------------------------------------
if command -v "$MC" >/dev/null 2>&1 && command -v "$OD" >/dev/null 2>&1 &&
   command -v "$RE" >/dev/null 2>&1; then
    cat > "$out/s.c" <<'C'
extern int g; int arr[20000]; static const char *msg = "hi";
int ext(int);
long long ll(long long a, long long b) { return a / b + (a << (b & 63)); }
int f(int x) { return ext(x) + g + arr[15000] + msg[1] + (int)ll(x, 3); }
int sw(int k) { switch (k) { case 0: return 4; case 1: return 9; case 2: return 1;
    case 3: return 7; case 4: return 2; case 5: return 8; default: return 0; } }
double d(double a) { return a * 2.5; }
C
    for opt in -O0 -O2; do
        "$EMBCC" --target=$T $opt -S "$out/s.c" -o "$out/s$opt.s" &&
        "$EMBCC" --target=$T $opt -c "$out/s.c" -o "$out/s$opt.o" || {
            echo "-S or -c failed at $opt"; exit 1; }
        "$MC" --triple=$T -filetype=obj "$out/s$opt.s" -o "$out/s$opt-mc.o" \
            2> "$out/mc.err" || {
            echo "llvm-mc does not assemble -S at $opt:"; head -4 "$out/mc.err"
            exit 1; }
        for o in s$opt s$opt-mc; do
            "$OD" -d "$out/$o.o" | sed 1,3d > "$out/$o.dis"
            "$RE" -r "$out/$o.o" | awk '/R_PPC/ { print $1, $3, $5, $6, $7 }' |
                sort > "$out/$o.rel"
        done
        cmp -s "$out/s$opt.dis" "$out/s$opt-mc.dis" &&
        cmp -s "$out/s$opt.rel" "$out/s$opt-mc.rel" || {
            echo "-S at $opt reassembles into something other than -c's object:"
            diff "$out/s$opt.dis" "$out/s$opt-mc.dis" | head -6
            diff "$out/s$opt.rel" "$out/s$opt-mc.rel" | head -6
            exit 1; }
    done
    echo "-S reassembles with llvm-mc into -c's code and relocations"
fi

# ---- the options -----------------------------------------------------------
for o in -mcpu=e500 -mcpu=8548 -mcpu=e200z4 -mcpu=ppc -msoft-float -mno-spe \
         -mlong-double-64 -meabi -msdata=none -G0 -mbig-endian \
         -mfloat-abi=soft -fno-asynchronous-unwind-tables; do
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
refopt -mcpu=pwr9 'not a 32-bit PowerPC core'
refopt -mcpu=e5500 'not a 32-bit PowerPC core'
refopt -mspe '-mspe is not supported'
refopt -mvle '-mvle is not supported'
refopt -mhard-float '-mhard-float is not supported'
refopt -mfloat-abi=hard 'soft-float PowerPC'
refopt -mlong-double-128 '-mlong-double-128 is not supported'
refopt -msdata=eabi 'small-data'
refopt -G8 'small-data'
refopt -mno-eabi 'embedded ABI'
refopt -mlittle-endian 'big-endian'
for o in -funwind-tables -fasynchronous-unwind-tables -fexceptions; do
    refopt $o 'unwind tables are not supported for powerpc-none-eabi yet'
done
echo "the e500/e200 soft-float EABI flags are accepted, others refused by name"

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
# An 8-byte atomic is a call to libatomic's sized routine, as GCC's and
# clang's are; lib/rt/atomic8.c defines them (it used to be refused).
printf 'long long x;
long long f(void){ return __atomic_fetch_add(&x, 1, 5); }
long long g(void){ return __atomic_load_n(&x, 5); }
' > "$out/at8.c"
"$EMBCC" --target=$T -O1 -c "$out/at8.c" -o "$out/at8.o" 2> "$out/at8.err" || {
    echo "an 8-byte atomic was refused:"; cat "$out/at8.err"; exit 1; }
for s in __atomic_fetch_add_8 __atomic_load_8; do
    "${EMBCC_LLVM_READELF:-llvm-readelf}" -s "$out/at8.o" | grep -q " $s\$" || {
        echo "an 8-byte atomic is not a call to $s"; exit 1; }
done
refc "__builtin_return_address(1)" 'only level 0' \
    'void *f(void){ return __builtin_return_address(1); }'
refc "__builtin_frame_address(1)" 'only level 0' \
    'void *f(void){ return __builtin_frame_address(1); }'
refc "__int128" '__int128 does not exist on this target' \
    '__int128 x;'
refc "an interrupt handler" '__attribute__((interrupt)) is not supported' \
    'void __attribute__((interrupt)) f(void){}'
# Assembly is src/arch/ppc/asm.c's (tests/golden/ppc-asm.sh referees it):
# inline asm with operands, a naked function, a file-scope block, a
# compiler barrier and a .s file all compile; what is outside the
# vocabulary is refused by name.
for src in 'int f(void){ int r; __asm__("li %0, 1" : "=r"(r)); return r; }' \
           'void __attribute__((naked)) f(void){ __asm__("blr"); }' \
           '__asm__(".globl foo\nfoo: blr");' \
           'int g; int f(void){ g = 1; __asm__ volatile("" ::: "memory"); return g; }'; do
    printf '%s\n' "$src" > "$out/asm.c"
    "$EMBCC" --target=$T -O2 -c "$out/asm.c" -o /dev/null 2> "$out/asm.err" || {
        echo "assembly in C was refused: $src"; cat "$out/asm.err"; exit 1; }
done
refc "a floating-point instruction" 'EmbCC compiles soft float' \
    'void f(void){ __asm__ volatile("fadd 1, 2, 3"); }'
refc "an AltiVec instruction" 'AltiVec instruction, which the e500 does not have' \
    'void f(void){ __asm__ volatile("vaddubm 1, 2, 3"); }'
refc "r1 clobbered" "clobbers 'r1', which holds the stack pointer" \
    'void f(void){ __asm__ volatile("nop" ::: "r1"); }'
printf 'int f(int x) { return x; }\n' > "$out/c.cc"
# C++ compiles here without exceptions (tests/golden/cxx-embedded.sh runs
# it); exceptions, on by default, are refused by name: there are no
# unwind tables for this target
if "$EMBCC" --target=$T -c "$out/c.cc" -o /dev/null 2> "$out/cxx.err"; then
    echo "C++ with exceptions was accepted"; exit 1
fi
grep -q 'C++ exceptions are not supported for powerpc-none-eabi' "$out/cxx.err" || {
    echo "C++ exceptions were refused, but not by name:"; cat "$out/cxx.err"; exit 1; }
"$EMBCC" --target=$T -fno-exceptions -c "$out/c.cc" -o /dev/null || {
    echo "C++ with -fno-exceptions does not compile"; exit 1; }
printf '\tblr\n' > "$out/a.s"
"$EMBCC" --target=$T -c "$out/a.s" -o /dev/null || {
    echo "an assembly file was refused"; exit 1; }
echo "narrow atomics, the frame and return address above level 0,"
echo "__int128, interrupt functions, an over-aligned scalar, floating-point"
echo "and AltiVec assembly and C++ exceptions are each refused by name"

# ---- EmbLD -------------------------------------------------------------------
# A little-endian PowerPC object (clang's powerpcle), which embld must refuse
# rather than read as a big-endian one.
CLANG=${EMBCC_REF_CLANG_PPC:-clang}
if command -v "$CLANG" >/dev/null 2>&1 &&
   "$CLANG" --target=powerpcle-none-eabi -c "$out/f.c" -o "$out/le.o" \
       2>/dev/null; then
    if "$EMBLD" -e f "$out/le.o" -o "$out/le.elf" 2> "$out/ld.err"; then
        echo "embld linked a little-endian PowerPC object"; exit 1
    fi
    grep -q 'little-endian PowerPC object' "$out/ld.err" || {
        echo "embld refused the little-endian object, but not by name:"
        cat "$out/ld.err"; exit 1; }
    echo "embld refuses a little-endian PowerPC object by name"
fi
