#!/bin/sh
# The SPARC (LEON3) target: what it is, and what it refuses BY NAME.
#
# The triples and -dumpmachine, the object's header (EM_SPARC, ELF32,
# big-endian, e_flags 0, RELA relocations), the machine options a LEON3
# build passes -- the one configuration EmbCC emits is accepted, every
# other one refused -- and the constructs the backend does not lower, each
# with a message naming it rather than code that does something else.
set -u
echo "TEST-MARKER sparc-refuse"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
T=sparc-none-elf
out=tests/golden/out/sparc-refuse
rm -rf "$out"; mkdir -p "$out"

# ---- the target ---------------------------------------------------------
for t in sparc-none-elf sparc-unknown-elf sparc-elf sparc-gaisler-elf sparc; do
    m=$("$EMBCC" --target=$t -dumpmachine) || { echo "--target=$t refused"; exit 1; }
    [ "$m" = sparc-none-elf ] || { echo "--target=$t is '$m'"; exit 1; }
done
printf 'int f(int x) { return x + 1; }\n' > "$out/f.c"
"$EMBCC" --target=$T -c "$out/f.c" -o "$out/f.o" || { echo "-c failed"; exit 1; }
if command -v "$RE" >/dev/null 2>&1; then
    printf 'extern int g; int f(void) { return g; }\n' > "$out/r.c"
    "$EMBCC" --target=$T -c "$out/r.c" -o "$out/r.o" &&
    "$RE" -h -S -r "$out/r.o" > "$out/r.hdr" || exit 1
    grep -q 'Class:.*ELF32' "$out/r.hdr" &&
    grep -q 'Data:.*big endian' "$out/r.hdr" &&
    grep -q 'Machine:.*Sparc' "$out/r.hdr" &&
    grep -q 'Flags:.*0x0' "$out/r.hdr" &&
    grep -q '\.rela\.text *RELA ' "$out/r.hdr" &&
    grep -q 'R_SPARC_HI22 .* g + 0' "$out/r.hdr" &&
    grep -q 'R_SPARC_LO10 .* g + 0' "$out/r.hdr" || {
        echo "the object is not ELF32 big-endian EM_SPARC, e_flags 0, RELA with HI22/LO10:"
        grep -E 'Class|Data|Machine|Flags|rela|R_SPARC' "$out/r.hdr"; exit 1; }
    echo "an ELF32 big-endian EM_SPARC object, e_flags 0, RELA (sethi/or: HI22, LO10)"
fi

# ---- the options -----------------------------------------------------------
for o in -mcpu=leon3 -mcpu=v8 -mcpu=leon4 -mcpu=gr712rc -march=leon3 \
         -mtune=leon3 -msoft-float -mno-fpu -mno-flat -mapp-regs -mv8 -m32 \
         -mcmodel=medlow -mbig-endian; do
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
refopt -mcpu=v9 'not a SPARC V8 core with hardware multiply and divide'
refopt -mcpu=v7 'not a SPARC V8 core with hardware multiply and divide'
refopt -mcpu=niagara 'not a SPARC V8 core'
refopt -mhard-float '-mhard-float is not supported'
refopt -mfpu '-mfpu is not supported'
refopt -mflat '-mflat is not supported'
refopt -mno-app-regs '-mno-app-regs is not supported'
refopt -m64 '-m64 is not supported'
refopt -mcmodel=medany 'absolute addresses'
refopt -mfix-ut699 'no LEON errata workarounds'
refopt -mfpu=fpv4-sp-d16 'ARM option'
refopt -mlittle-endian 'big-endian'
for o in -funwind-tables -fasynchronous-unwind-tables -fexceptions; do
    refopt $o 'unwind tables are not supported for sparc-none-elf yet'
done
"$EMBCC" --target=$T -fno-asynchronous-unwind-tables -c "$out/f.c" -o /dev/null || {
    echo "-fno-asynchronous-unwind-tables was refused"; exit 1; }
echo "the LEON3/soft-float/windowed flags are accepted, others refused"

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
# Assembly is src/arch/sparc/asm.c's (tests/golden/sparc-asm.sh referees
# it): inline asm with operands, a naked function, a file-scope block, a
# compiler barrier and a .s file all compile; what is outside the
# vocabulary is refused by name.
for src in 'int f(void){ __asm__ volatile("nop"); return 0; }' \
           'int f(int x){ int y; __asm__("add %1, 1, %0" : "=r"(y) : "r"(x)); return y; }' \
           'void __attribute__((naked)) f(void){ __asm__("retl; nop"); }' \
           '__asm__(".globl x\nx: retl\n nop");' \
           'int g; int f(void){ g = 1; __asm__ volatile("" ::: "memory"); return g; }'; do
    printf '%s\n' "$src" > "$out/asm.c"
    "$EMBCC" --target=$T -O2 -c "$out/asm.c" -o /dev/null 2> "$out/asm.err" || {
        echo "assembly in C was refused: $src"; cat "$out/asm.err"; exit 1; }
done
printf '\tretl\n\tnop\n' > "$out/a.s"
"$EMBCC" --target=$T -c "$out/a.s" -o /dev/null || {
    echo "an assembly file was refused"; exit 1; }
refc "a floating-point instruction" 'EmbCC compiles soft float' \
    'void f(void){ __asm__ volatile("faddd %f0, %f2, %f4"); }'
refc "a V9 instruction" "SPARC V9's, and the LEON3 is a V8" \
    'void f(void){ __asm__ volatile("membar 15"); }'
refc "%sp clobbered" "clobbers '%sp', which holds" \
    'void f(void){ __asm__ volatile("nop" ::: "%sp"); }'
printf 'int f(int x) { return x; }\n' > "$out/c.cc"
# C++ compiles here without exceptions (tests/golden/cxx-embedded.sh runs
# it); exceptions, on by default, are refused by name: there are no
# unwind tables for this target
if "$EMBCC" --target=$T -c "$out/c.cc" -o /dev/null 2> "$out/cxx.err"; then
    echo "C++ with exceptions was accepted"; exit 1
fi
grep -q 'C++ exceptions are not supported for sparc-none-elf' "$out/cxx.err" || {
    echo "C++ exceptions were refused, but not by name:"; cat "$out/cxx.err"; exit 1; }
"$EMBCC" --target=$T -fno-exceptions -c "$out/c.cc" -o /dev/null || {
    echo "C++ with -fno-exceptions does not compile"; exit 1; }

# ---- -S ------------------------------------------------------------------------
# The assembly EmbCC writes for SPARC is its words as .byte and its
# relocations as .reloc with SPARC's names (they once came out as x86-64's
# R_X86_64_PC32): llvm-mc assembles it into the same instructions and the
# same relocation types at the same offsets as -c's object -- against the
# same symbols, but for a string literal's, which -S names by its label.
MC=${EMBCC_LLVM_MC:-llvm-mc}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
if command -v "$MC" >/dev/null 2>&1 && command -v "$OD" >/dev/null 2>&1; then
    for f in tests/golden/sparc-abi-caller.c tests/exec/structs.c; do
        b=$(basename "$f" .c)
        "$EMBCC" --target=$T -O2 -I tests/golden -S "$f" -o "$out/$b.s" &&
        "$EMBCC" --target=$T -O2 -I tests/golden -c "$f" -o "$out/$b.o" &&
        "$MC" -triple=sparc -mcpu=leon3 -filetype=obj "$out/$b.s" \
            -o "$out/$b.re.o" || { echo "-S of $f does not reassemble"; exit 1; }
        for o in "$b.o" "$b.re.o"; do
            "$OD" -dr --no-show-raw-insn "$out/$o" | tail -n +3 |
                sed -E 's/(R_SPARC_[A-Z0-9]+)[[:space:]]+(\.LC[0-9]+|\.rodata).*/\1 (a string)/' \
                > "$out/$o.d"
        done
        cmp -s "$out/$b.o.d" "$out/$b.re.o.d" || {
            echo "-S of $f reassembles into other code than -c makes:"
            diff "$out/$b.o.d" "$out/$b.re.o.d" | head -10; exit 1; }
    done
    echo "-S reassembles with llvm-mc into -c's instructions and relocations"
fi

# ---- EmbLD -------------------------------------------------------------------
# A relocation it does not lay out -- clang's -fPIC code takes the GOT's
# base PC-relatively (R_SPARC_PC22/PC10) -- is refused by name, not
# resolved as something else. (The GOT symbol is defined so the link gets
# that far.)
EMBLD=${EMBLD:-./embld}
CLANG=${EMBCC_REF_CLANG_SPARC:-clang}
if command -v "$CLANG" >/dev/null 2>&1 &&
   "$CLANG" --target=$T -mcpu=leon3 -fsyntax-only -x c /dev/null 2>/dev/null; then
    printf 'extern int g; int h(void){ return g; }\n' > "$out/pic.c"
    "$CLANG" --target=$T -mcpu=leon3 -fPIC -O1 -c "$out/pic.c" -o "$out/pic.o" &&
    printf 'int g, _GLOBAL_OFFSET_TABLE_; void _start(void){}\n' > "$out/st.c" &&
    "$EMBCC" --target=$T -c "$out/st.c" -o "$out/st.o" || exit 1
    if "$EMBLD" -e _start -Ttext 0x40000000 "$out/st.o" "$out/pic.o" \
           -o "$out/pic.elf" 2> "$out/ld.err"; then
        echo "a GOT relocation was linked"; exit 1
    fi
    grep -q 'position-independent code' "$out/ld.err" || {
        echo "a GOT relocation was refused, but not by name:"; cat "$out/ld.err"; exit 1; }
    echo "embld refuses a SPARC object's GOT relocation by name"
fi
echo "narrow atomics, the frame and return address above level 0,"
echo "__int128, interrupt functions, floating-point and V9 assembly, an"
echo "over-aligned scalar and C++ exceptions are each refused by name"
