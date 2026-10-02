#!/bin/sh
# A const OBJECT is read-only data, and only a const object is.
#
# gcc puts `const int t[] = {...}` in .rodata: on a microcontroller that
# is flash, where it is read in place; under an MMU, a page that cannot
# be written. EmbCC put every object in .data -- a lookup table cost its
# size in RAM, copied there at startup -- because its types carried no
# const. The parser now decides whether the declared object itself is
# const, and the declarators below are the ways to get that wrong. Each
# name says where it must land; one that is written through (`p`, the
# pointers to const) placed in .rodata would be a write that faults, or
# on a flash part a write that silently does nothing.
set -u
echo "TEST-MARKER rodata-const"
. "$(dirname "$0")/../lib.sh"
out=tests/golden/out/rodata-const
rm -rf "$out"; mkdir -p "$out"
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: no llvm-objdump"; exit 0; }

cat > "$out/r.c" <<'CEOF'
struct cfg { int a, b; };
typedef const struct cfg cfg_t;
typedef char *str;
typedef const int cint;
const int RO_tbl[4] = { 1, 2, 3, 4 };
int const RO_post = 5;
char *const RO_cptr = 0;
const char *const RO_ccptr = "x";
int *const RO_cptrs[2] = { 0, 0 };
const int (RO_paren)[3] = { 1 };
int (*const RO_fp)(void) = 0;
cfg_t RO_cfg = { 1, 2 };
const str RO_cstr = 0;
struct cfg const RO_spost = { 3, 4 };
int const *const RO_both = 0;
const char *const RO_names[2] = { "a", "b" };
const int RO_dim[sizeof(const char *)] = { 1 };
const int RO_z;
const int RO_m1 = 1, *RW_m2 = 0;
const char *RW_p = "y";
const int *RW_ptrs[2];
const int (*RW_parr)[3];
const int (*RW_fps[2])(void);
const str *RW_pstr;
cint *RW_pcint;
int RW_plain = 1;
const volatile int RW_cv = 3;
int use(void) { static const int RO_local[3] = { 7, 8, 9 }; static int RW_local = 1;
                return RO_local[RW_local] + RO_tbl[0]; }
CEOF

fail=0
for T in x86_64-elf aarch64-elf thumbv7m-none-eabi riscv32-unknown-elf avr; do
    "$EMBCC" --target=$T -O2 -c "$out/r.c" -o "$out/r-$T.o" || {
        echo "FAIL $T: does not compile"; fail=1; continue; }
    "$OD" -t "$out/r-$T.o" > "$out/r-$T.sym"
    n=0
    for name in $(grep -o '\(RO\|RW\)_[a-z0-9]*' "$out/r.c" | sort -u); do
        sec=$(awk -v n="$name" '$NF == n || $NF ~ ("\\." n "$") { for (i = 1; i <= NF; i++) if ($i ~ /^\./) { print $i; exit } }' "$out/r-$T.sym")
        case $name:$sec in
            RO_*:.rodata | RW_*:.data | RW_*:.bss) ;;
            RO_*:*) echo "FAIL $T: $name is in '${sec:-?}', not .rodata"; fail=1 ;;
            *) echo "FAIL $T: $name is in '${sec:-?}', not a writable section"; fail=1 ;;
        esac
        n=$((n + 1))
    done
    echo "  $T: $n objects where they belong"
done
[ "$fail" -eq 0 ] || exit 1
echo "const objects are .rodata and everything written through stays
writable -- pointers to const, a const typedef behind a pointer, a const
volatile -- across the declarator shapes, on x86-64, aarch64, Thumb,
RISC-V and AVR"
