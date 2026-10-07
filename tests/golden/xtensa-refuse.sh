#!/bin/sh
# The Xtensa target: what it is, and what it refuses BY NAME.
#
# The triples and -dumpmachine, the object's header (EM_XTENSA, ELF32,
# little-endian, e_flags 0x300 = XT_INSN | XT_LIT, RELA relocations:
# R_XTENSA_SLOT0_OP on a call8, R_XTENSA_32 on a literal-pool word), a
# function's symbol at its entry after its literal pool, the machine
# options an ESP-IDF build passes -- the one configuration EmbCC emits is
# accepted, every other one refused -- and the constructs the backend does
# not lower, each with a message naming it rather than code that does
# something else.
set -u
echo "TEST-MARKER xtensa-refuse"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
T=xtensa-none-elf
out=tests/golden/out/xtensa-refuse
rm -rf "$out"; mkdir -p "$out"

# ---- the target ---------------------------------------------------------
for t in xtensa-none-elf xtensa-esp32-elf xtensa-esp32s3-elf xtensa-esp-elf \
         xtensa-elf xtensa; do
    m=$("$EMBCC" --target=$t -dumpmachine) || { echo "--target=$t refused"; exit 1; }
    [ "$m" = xtensa-none-elf ] || { echo "--target=$t is '$m'"; exit 1; }
done
if "$EMBCC" --target=xtensaeb-none-elf -dumpmachine > /dev/null 2>&1; then
    echo "big-endian xtensaeb-none-elf was accepted"; exit 1
fi
"$EMBCC" --target=$T --dump-predef > "$out/predef.txt"
for m in '__xtensa__ 1' '__XTENSA__ 1' '__XTENSA_EL__ 1' \
         '__XTENSA_WINDOWED_ABI__ 1' '__CHAR_UNSIGNED__ 1' \
         '__SIZEOF_WCHAR_T__ 2' '__SIZEOF_LONG__ 4' '__SIZEOF_POINTER__ 4' \
         '__SIZEOF_LONG_DOUBLE__ 8' '__BIGGEST_ALIGNMENT__ 16'; do
    grep -q "^#define $m\$" "$out/predef.txt" || {
        echo "the predefined macros lack '#define $m'"; exit 1; }
done
for m in __XTENSA_CALL0_ABI__ __XTENSA_EB__ __LP64__ __SIZEOF_INT128__; do
    if grep -q "^#define $m " "$out/predef.txt"; then
        echo "$m is predefined, and the target is not that"; exit 1
    fi
done
cat > "$out/f.c" <<'CEOF'
extern int g;
int h(int);
int f(int x) { return h(x + 1) + g + 0x12345678; }
CEOF
"$EMBCC" --target=$T -O2 -c "$out/f.c" -o "$out/f.o" || { echo "-c failed"; exit 1; }
if command -v "$RE" >/dev/null 2>&1; then
    "$RE" -h -S -r -s "$out/f.o" > "$out/f.hdr" 2>/dev/null
    grep -q 'Class:.*ELF32' "$out/f.hdr" &&
    grep -q 'Data:.*little endian' "$out/f.hdr" &&
    grep -qE 'Machine:.*(Xtensa|94)' "$out/f.hdr" &&
    grep -q 'Flags:.*0x300' "$out/f.hdr" &&
    grep -q '\.rela\.text *RELA ' "$out/f.hdr" || {
        echo "the object header is not ELF32 LE EM_XTENSA, e_flags 0x300, RELA:"
        grep -E 'Class|Data|Machine|Flags|rela' "$out/f.hdr"; exit 1; }
    grep -qE 'R_XTENSA_SLOT0_OP|00000014 ' "$out/f.hdr" &&
    grep -qE 'R_XTENSA_32|00000001 ' "$out/f.hdr" || {
        echo "the call and the literal are not SLOT0_OP and R_XTENSA_32:"
        grep -A8 'rela.text' "$out/f.hdr"; exit 1; }
    # the pool (the constant and &g, 8 bytes) comes first: f is at 8
    grep -qE '0+8 +[0-9]+ FUNC +GLOBAL +DEFAULT +[0-9]+ f$' "$out/f.hdr" || {
        echo "f's symbol is not at its entry, after its literal pool:"
        grep ' f$' "$out/f.hdr"; exit 1; }
    echo "an ELF32 little-endian EM_XTENSA object, e_flags 0x300, RELA, f at its entry"
fi

# ---- the options -----------------------------------------------------------
for o in -mlongcalls -mno-longcalls -mtext-section-literals -mabi=windowed \
         -mlittle-endian -mserialize-volatile -mno-serialize-volatile \
         -mforce-no-pic -mstrict-align -mtarget-align -mno-target-align \
         -mauto-litpools -mdynconfig=xtensa_esp32.so; do
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
refopt -mabi=call0 '-mabi=call0 is not supported'
refopt -mbig-endian 'little-endian'
refopt -mfix-esp32-psram-cache-issue 'PSRAM workaround'
refopt -mconst16 'is not supported for xtensa-none-elf'
refopt -mdynconfig=xtensa_esp32s2.so 'is not supported for xtensa-none-elf'
refopt -S 'is not supported for xtensa-none-elf yet'
for o in -funwind-tables -fasynchronous-unwind-tables -fexceptions; do
    refopt $o 'unwind tables are not supported for xtensa-none-elf yet'
done
echo "the windowed/little-endian/text-literal flags are accepted, others refused"

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
refc "a 1-byte atomic" 'an atomic narrower than four bytes' \
    'char c; int f(void){ return __atomic_fetch_add(&c, 1, 5); }' -O1
refc "a 2-byte compare-and-swap" 'an atomic narrower than four bytes' \
    'short s; int f(void){ short e = 0; return __atomic_compare_exchange_n(&s, &e, 1, 0, 5, 5); }'
refc "an 8-byte atomic read-modify-write" 'an atomic wider than a register' \
    'long long x; long long f(void){ return __atomic_fetch_add(&x, 1, 5); }'
refc "an 8-byte atomic load" 'an atomic access of 8 bytes is not one access' \
    'long long x; long long f(void){ return __atomic_load_n(&x, 5); }'
refc "a computed goto" 'a computed goto' \
    'int f(int i){ void *t[2]; t[0] = &&a; t[1] = &&b; goto *t[i]; a: return 1; b: return 2; }'
refc "__builtin_return_address" '__builtin_frame_address or __builtin_return_address' \
    'void *f(void){ return __builtin_return_address(0); }'
refc "__builtin_frame_address" '__builtin_frame_address or __builtin_return_address' \
    'void *f(void){ return __builtin_frame_address(0); }'
refc "__int128" '__int128 does not exist on this target' \
    '__int128 x;'
refc "an interrupt handler" '__attribute__((interrupt)) is not supported' \
    'void __attribute__((interrupt)) f(void){}'
refc "inline assembly" 'inline assembly is not supported for xtensa-none-elf' \
    'int f(int x){ __asm__ volatile("nop"); return x; }'
refc "a naked function" 'inline assembly is not supported for xtensa-none-elf' \
    'void __attribute__((naked)) f(void){ __asm__("retw"); }'
refc "a file-scope instruction" 'file-scope asm instruction' \
    '__asm__(".globl x\nx: nop");'
refc "a 32-aligned scalar local" 'needs 32-byte alignment and the stack only guarantees 16' \
    'int g(int *); int f(void){ _Alignas(32) int x = 1; return g(&x); }'
printf 'int f(int x) { return x; }\n' > "$out/c.cc"
if "$EMBCC" --target=$T -c "$out/c.cc" -o /dev/null 2> "$out/cxx.err"; then
    echo "C++ was accepted"; exit 1
fi
grep -q 'C++ is not yet supported for xtensa-none-elf' "$out/cxx.err" || {
    echo "C++ was refused, but not by name:"; cat "$out/cxx.err"; exit 1; }
echo "narrow and 8-byte atomics, computed goto, the frame and return address,"
echo "__int128, interrupt and naked functions, inline and file-scope assembly,"
echo "a scalar aligned beyond the 16-byte stack and C++ are each refused by name"
