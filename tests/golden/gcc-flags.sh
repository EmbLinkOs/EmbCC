#!/bin/sh
# The flags an arm-none-eabi-gcc Makefile or CMake toolchain file passes,
# and what EmbCC does with each: accepts it (a hint, a permission, or a
# promise it already keeps), implements it, or refuses it by name. Each
# used to stop the driver at "unknown argument", so a build written for
# GCC did not start.
#
# Accepting is only half of it. A flag that PROMISES something about the
# code -- no jump tables, zero-initialized data in .data, a volatile
# bit-field read at its declared width -- is checked here against the
# object, on every target where the promise means something, so the
# acceptance cannot quietly become a lie when the code generator moves:
#
#   1. every accepted flag compiles a file on the targets it applies to,
#      with nothing on stderr (and the few that say something, say it)
#   2. every refused flag fails, names itself, and writes no object
#   3. -fno-jump-tables leaves no table on any backend, and a dense
#      switch built with it RUNS to the same answers (Cortex-M, RV32)
#   4. -fno-inline-functions keeps a non-inline callee a call at -O2
#   5. -fcommon makes tentative definitions COMMON, embld merges them,
#      and the merged objects work on the board
#   6. -fsingle-precision-constant changes types and values as GCC's does
#   7. -save-temps writes the .i and .s where GCC 11 does
#   8. the promises EmbCC already kept: -fno-zero-initialized-in-bss,
#      -fstrict-volatile-bitfields, -fno-delete-null-pointer-checks,
#      -fno-tree-loop-distribute-patterns, -fno-short-enums,
#      -mlittle-endian, -Werror=implicit-function-declaration, -Og, -Ofast
set -u
echo "TEST-MARKER gcc-flags"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
RE=${EMBCC_LLVM_READELF:-llvm-readelf}
command -v "$OD" >/dev/null 2>&1 && command -v "$RE" >/dev/null 2>&1 || {
    echo "SKIP: llvm-objdump/llvm-readelf not found"; exit 0; }
out=tests/golden/out/gcc-flags
rm -rf "$out"; mkdir -p "$out"
EMBLD=${EMBLD:-./embld}
case $EMBCC in /*) EMBCC_ABS=$EMBCC ;; *) EMBCC_ABS=$PWD/$EMBCC ;; esac

fail() { echo "FAIL: $*"; exit 1; }

ALL="thumbv7em-none-eabi thumbv7m-none-eabi thumbv8m.main-none-eabi
     riscv32-unknown-elf riscv64-unknown-elf avr x86_64-elf aarch64-elf"
THUMB="thumbv7em-none-eabi thumbv7m-none-eabi thumbv8m.main-none-eabi"

printf 'int f(int x) { return x + 1; }\n' > "$out/t.c"

# ---- 1. accepted, silently ------------------------------------------------
# accept TARGETS FLAG...: each flag alone, on each target, exit 0, no stderr
accept() {
    for t in $1; do
        for fl in $2; do
            rm -f "$out/t.o"
            err=$("$EMBCC" --target=$t -c "$out/t.c" -o "$out/t.o" "$fl" 2>&1)
            st=$?
            [ $st = 0 ] || fail "$fl on $t: exit $st: $err"
            [ -z "$err" ] || fail "$fl on $t said something: $err"
            [ -s "$out/t.o" ] || fail "$fl on $t: no object"
        done
    done
}
EVERYWHERE="-fno-inline-functions -finline-functions -finline-small-functions
 -fno-inline-small-functions -finline-limit=600 -fcommon -fno-common
 -fno-short-enums -fno-math-errno -ffast-math -fsingle-precision-constant
 -funsafe-math-optimizations -fno-signed-zeros -fno-trapping-math
 -ffinite-math-only -fassociative-math -freciprocal-math
 -fmessage-length=0 -fdiagnostics-color=always -fdiagnostics-color=never
 -fdiagnostics-color=auto -fverbose-asm -pipe -fno-pic -fno-pie
 -fno-delete-null-pointer-checks -fno-tree-loop-distribute-patterns
 -fno-zero-initialized-in-bss -fmerge-constants -fno-jump-tables
 -fjump-tables -fno-strict-overflow -fstrict-volatile-bitfields
 -fno-strict-volatile-bitfields -fno-builtin-memcpy -fno-builtin-printf
 -specs=nano.specs --specs=nosys.specs
 -fno-isolate-erroneous-paths-dereference -fno-move-loop-invariants
 -fno-ipa-sra -fno-lto -Og -Ofast -mlittle-endian
 -Werror=implicit-function-declaration -Wimplicit-function-declaration
 -Werror-implicit-function-declaration"
accept "$ALL" "$EVERYWHERE"
accept "$THUMB" "-mabi=aapcs -mabi=aapcs-linux -mthumb-interwork
 -mno-thumb-interwork -munaligned-access -mslow-flash-data"
accept riscv32-unknown-elf -mabi=ilp32
accept riscv64-unknown-elf -mabi=lp64
echo "accepted silently: $(echo $EVERYWHERE | wc -w | tr -d ' ') flags on" \
     "every target, the ARM and RISC-V ABI and machine flags on theirs"

# The two that do speak, once, and still succeed.
err=$("$EMBCC" --target=thumbv7em-none-eabi -c "$out/t.c" -o "$out/t.o" \
      -fanalyzer 2>&1) || fail "-fanalyzer was refused: $err"
[ "$(echo "$err" | wc -l | tr -d ' ')" = 1 ] &&
    echo "$err" | grep -q 'no static analyzer' ||
    fail "-fanalyzer should warn once that it checks nothing: $err"
printf 'void _start(void) { for (;;); }\n' > "$out/s.c"
err=$("$EMBCC" --target=x86_64-elf "$out/s.c" -nostdlib -o "$out/s.elf" \
      -specs=nano.specs 2>&1) || fail "-specs= stopped a link: $err"
[ "$(echo "$err" | wc -l | tr -d ' ')" = 1 ] &&
    echo "$err" | grep -q 'specs=nano.specs is ignored' ||
    fail "-specs= should be noted once at the link: $err"
echo "-fanalyzer warns, and -specs= is noted at the link, once each"

# ---- 2. refused, by name ----------------------------------------------------
# refuse TARGET SUBSTRING FLAG... (FILE: t.c unless the last flag is one)
refuse() {
    t=$1; want=$2; shift 2
    src="$out/t.c"
    rm -f "$out/r.o"
    if err=$("$EMBCC" --target=$t -c "$src" -o "$out/r.o" "$@" 2>&1); then
        fail "$* on $t was accepted"
    fi
    echo "$err" | grep -qF -- "$want" ||
        fail "$* on $t refused without saying '$want': $err"
    [ ! -e "$out/r.o" ] || fail "$* on $t refused but wrote an object"
}
for t in $ALL; do
    refuse $t "which is little-endian" -mbig-endian
    refuse $t "dumps GCC's internal representation" -fdump-rtl-expand
    refuse $t "dumps GCC's internal representation" -fdump-tree-all
    refuse $t "dumps GCC's internal representation" -fcallgraph-info=da
done
for t in $THUMB; do
    for abi in apcs-gnu atpcs iwmmxt; do
        refuse $t "-mabi=$abi is not supported: EmbCC emits the AAPCS" \
               -mabi=$abi
    done
    refuse $t "-mno-unaligned-access is not supported" -mno-unaligned-access
done
refuse riscv32-unknown-elf "-march=rv32imac has no D extension" -mabi=ilp32d
refuse riscv32-unknown-elf "-mabi=ilp32e is not supported" -mabi=ilp32e
refuse riscv64-unknown-elf "-march=rv64imac has no D extension" -mabi=lp64d
refuse riscv32-unknown-elf "has no F extension" -march=rv32imac -mabi=ilp32f
refuse riscv32-unknown-elf "the D extension needs F" -march=rv32imadc
refuse riscv32-unknown-elf "the 'v' extension is not supported" -march=rv32imafcv
refuse riscv32-unknown-elf "the 'zba' extension is not supported" -march=rv32imac_zba
refuse riscv32-unknown-elf "EmbCC needs the M extension" -march=rv32iac
refuse riscv32-unknown-elf "use --target=riscv64-unknown-elf" -march=rv64gc
refuse riscv64-unknown-elf "is a 32-bit ABI" -march=rv64gc -mabi=ilp32d
refuse riscv32-unknown-elf "is an ARM option" -mthumb-interwork
refuse x86_64-elf "is an ARM option" -mslow-flash-data
refuse x86_64-apple-darwin "-fcommon is not supported for" -fcommon
refuse x86_64-windows-gnu "-fcommon is not supported for" -fcommon
printf 'int x;\n' > "$out/c.cc"
if err=$("$EMBCC" --target=x86_64-elf -c "$out/c.cc" -o "$out/r.o" \
         -fsingle-precision-constant 2>&1); then
    fail "-fsingle-precision-constant was accepted for C++"
fi
echo "$err" | grep -q 'supported for C, not C++' ||
    fail "-fsingle-precision-constant refused for C++ without saying why: $err"
echo "refused by name: -mbig-endian, GCC's dumps, the non-AAPCS -mabi,"
echo "-mno-unaligned-access, the F/D/E RISC-V ABIs, -fcommon off ELF, and"
echo "-fsingle-precision-constant in C++"

# ---- 3. -fno-jump-tables ----------------------------------------------------
# A switch dense enough that every backend that has tables uses one.
cat > "$out/sw.c" <<'EOF'
extern int g;
int sw(int x)
{
    switch (x) {
    case 0: return 11;  case 1: return g;   case 2: return 37;
    case 3: return 41;  case 4: return 53;  case 5: return 67;
    case 6: return 79;  case 7: return 83;  case 8: return 97;
    case 9: return 101; case 10: return 103; case 11: return 107;
    }
    return -1;
}
EOF
# the table's dispatch, per backend: an indirect jump through it
table_re() {
    case $1 in
    x86_64*)  echo 'jmpq?[[:space:]]+\*' ;;
    aarch64*) echo '[[:space:]]br[[:space:]]+x' ;;
    thumb*)   echo '[[:space:]]tb[bh][[:space:]]' ;;
    riscv*)   echo '[[:space:]]jr[[:space:]]+[ast][0-9]' ;;
    avr)      echo '[[:space:]]e?ijmp' ;;
    esac
}
ntables() {   # ntables TARGET OPT FLAG...
    t=$1; o=$2; shift 2
    "$EMBCC" --target=$t $o -c "$out/sw.c" -o "$out/sw.o" "$@" ||
        fail "the dense switch does not compile for $t $o $*"
    "$OD" -d "$out/sw.o" | grep -cE "$(table_re $t)"
}
for t in $ALL x86_64-linux-gnu aarch64-linux-gnu; do
    for o in -O0 -O2 -Os; do
        n=$(ntables $t $o)
        # AVR has no tables (target_jump_tables), so there is nothing to
        # remove there; everywhere else the switch IS a table without the
        # flag, or the check below would prove nothing.
        if [ $t = avr ]; then
            [ "$n" = 0 ] || fail "$t $o: a jump table on AVR"
        else
            [ "$n" -ge 1 ] || fail "$t $o: the dense switch is no table" \
                "without -fno-jump-tables, so this test checks nothing"
        fi
        n=$(ntables $t $o -fno-jump-tables)
        [ "$n" = 0 ] || {
            "$OD" -d "$out/sw.o" | sed -n '1,40p'
            fail "$t $o -fno-jump-tables: still $n table dispatch(es)"; }
        # and -fjump-tables, later on the line, gives them back
        n=$(ntables $t $o -fno-jump-tables -fjump-tables)
        [ $t = avr ] || [ "$n" -ge 1 ] ||
            fail "$t $o: -fjump-tables after -fno-jump-tables did not restore it"
    done
done
echo "-fno-jump-tables: no table on x86-64, AArch64, ARMv7-M/v7E-M/v8-M,"
echo "RV32, RV64 (and none on AVR either way) at -O0, -O2 and -Os"

# The switch with no table computes what it did with one, on the boards.
cat > "$out/swrun.c" <<'EOF'
extern void puts_(const char *s);
extern void putn(long v);
int g = 13;
int sw(int x);
int main(void)
{
    long sum = 0;
    for (int i = -3; i < 16; i++) {
        putn(sw(i));
        sum = sum * 3 + sw(i);
    }
    putn(sum);
    puts_("\n==END==\n");
    return 0;
}
EOF
QEMU_ARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
QEMU_RV=${EMBCC_QEMU_RISCV:-qemu-system-riscv32}
EXPECT="-1 -1 -1 11 13 37 41 53 67 79 83 97 101 103 107 -1 -1 -1 -1"
runsw() {   # runsw NAME TARGET HARNESS QEMU-ARGS... (flags in $SWFLAGS)
    name=$1; t=$2; H=$3; shift 3
    d="$out/$name"; mkdir -p "$d"
    for f in boot io; do
        "$EMBCC" --target=$t -c tests/harness/$H/$f.c -o "$d/$f.o" ||
            fail "$name: the harness does not compile"
    done
    "$EMBCC" --target=$t -O2 -c "$out/swrun.c" -o "$d/main.o" ||
        fail "$name: the driver program does not compile"
    "$EMBCC" --target=$t $SWFLAGS -c "$out/sw.c" -o "$d/sw.o" ||
        fail "$name: the switch does not compile"
    if [ $H = thumb ]; then
        "$EMBLD" -e reset -Ttext 0x0 -Tdata 0x20000000 "$d/boot.o" \
            "$d/io.o" "$d/main.o" "$d/sw.o" -o "$d/img.elf"
    else
        "$EMBLD" -e _start -Ttext 0x80000000 -Tstack 0x80800000 \
            "$d/boot.o" "$d/io.o" "$d/main.o" "$d/sw.o" -o "$d/img.elf"
    fi || fail "$name: embld could not link it"
    sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-10}" --until '==END==' \
        "$@" -kernel "$d/img.elf" > "$d/out.txt" 2>/dev/null
    grep -q '==END==' "$d/out.txt" ||
        fail "$name: the image did not finish: $(head -c 300 "$d/out.txt")"
    tr -d '\r' < "$d/out.txt" | sed -n 1p
}
ran=0
for o in -O0 -O2 -Os; do
    if command -v "$QEMU_ARM" >/dev/null 2>&1; then
        a=$(SWFLAGS="$o" runsw arm$o-t thumbv7m-none-eabi thumb \
            "$QEMU_ARM" -M lm3s6965evb -cpu cortex-m3 -nographic) || {
            echo "$a"; exit 1; }
        b=$(SWFLAGS="$o -fno-jump-tables" runsw arm$o-n thumbv7m-none-eabi \
            thumb "$QEMU_ARM" -M lm3s6965evb -cpu cortex-m3 -nographic) || {
            echo "$b"; exit 1; }
        case "$a" in "$EXPECT "*) ;; *) fail "Cortex-M $o table: $a" ;; esac
        [ "$a" = "$b" ] || fail "Cortex-M $o: with a table '$a', without '$b'"
        ran=1
    fi
    if command -v "$QEMU_RV" >/dev/null 2>&1; then
        a=$(SWFLAGS="$o" runsw rv$o-t riscv32-unknown-elf riscv \
            "$QEMU_RV" -M virt -bios none -nographic -m 8) || {
            echo "$a"; exit 1; }
        b=$(SWFLAGS="$o -fno-jump-tables" runsw rv$o-n riscv32-unknown-elf \
            riscv "$QEMU_RV" -M virt -bios none -nographic -m 8) || {
            echo "$b"; exit 1; }
        case "$a" in "$EXPECT "*) ;; *) fail "RV32 $o table: $a" ;; esac
        [ "$a" = "$b" ] || fail "RV32 $o: with a table '$a', without '$b'"
        ran=1
    fi
done
if [ $ran = 1 ]; then
    echo "and the switch without a table runs to the same answers on QEMU"
else
    echo "(QEMU not found: the dense switch was not run)"
fi

# ---- 4. -fno-inline-functions ----------------------------------------------
cat > "$out/inl.c" <<'EOF'
static int helper(int x) { return x * 3 + 1; }
static inline int ihelper(int x) { return x * 5 + 2; }
int f(int a) { return helper(a) + ihelper(a); }
int g(int a) { return helper(a + 1) + ihelper(a + 1); }
int h(int a) { return helper(a + 2) + ihelper(a + 2); }
EOF
# calls TARGET OPT FLAG...: how many instructions name <NAME> as a target
calls() {
    t=$1; o=$2; name=$3; shift 3
    "$EMBCC" --target=$t $o -c "$out/inl.c" -o "$out/inl.o" "$@" ||
        fail "inl.c does not compile for $t $o $*"
    # a call names it in the disassembly, or (AVR) in its relocation
    "$OD" -dr "$out/inl.o" | grep -v ':$' |
        grep -cE "<$name>([[:space:]]|\$)|R_[A-Z0-9_]+[[:space:]]+$name\$"
}
for t in thumbv7em-none-eabi riscv32-unknown-elf x86_64-elf aarch64-elf avr; do
    for o in -O2 -Os; do
        # AVR's -Os budget copies nothing this size (src/opt/inline.c), so there is
        # no inlining there for the flag to stop
        [ $t = avr ] && [ $o = -Os ] && continue
        [ "$(calls $t $o helper)" = 0 ] ||
            fail "$t $o: helper() was not inlined without the flag, so this" \
                 "test checks nothing"
        [ "$(calls $t $o helper -fno-inline-functions)" -ge 3 ] ||
            fail "$t $o -fno-inline-functions: helper() was inlined"
        [ "$(calls $t $o ihelper -fno-inline-functions)" = 0 ] ||
            fail "$t $o -fno-inline-functions: the inline function was not" \
                 "inlined"
        [ "$(calls $t $o helper -fno-inline-functions -finline-functions)" \
            = 0 ] || fail "$t $o: -finline-functions did not restore it"
    done
done
echo "-fno-inline-functions: a static function not declared inline stays a"
echo "call at -O2 and -Os; an inline one is still inlined"

# ---- 5. -fcommon ------------------------------------------------------------
cat > "$out/cm.c" <<'EOF'
int a; static int b; const int c; int d[10]; __thread int e;
int f __attribute__((section(".mysec"))); int g __attribute__((weak));
int h __attribute__((aligned(64))); int ini = 0;
int *use(void) { return &b + c; }
EOF
# symbols TARGET FLAG...: "name:ndx" for each OBJECT/TLS symbol
symbols() {
    t=$1; shift
    "$EMBCC" --target=$t -c "$out/cm.c" -o "$out/cm.o" "$@" ||
        fail "cm.c does not compile for $t $*"
    "$RE" -s "$out/cm.o" | awk '$4 == "OBJECT" || $4 == "TLS" {
        v = $2; sub(/^0+/, "", v); printf "%s:%s:%s ", $8, $7, v }'
}
for t in thumbv7em-none-eabi riscv32-unknown-elf riscv64-unknown-elf avr \
         x86_64-elf aarch64-elf x86_64-linux-gnu; do
    s=$(symbols $t -fcommon)
    # COMMON's value is the alignment: aligned(64) says 0x40
    for want in "a:COM:" "c:COM:" "d:COM:" "h:COM:40 "; do
        case " $s" in *" $want"*) ;;
        *) fail "$t -fcommon: no '$want' in $s" ;; esac
    done
    for not in b e f g ini; do
        case " $s" in *" $not:COM:"*) fail "$t -fcommon: '$not' became COMMON: $s" ;; esac
    done
    s=$(symbols $t)
    case "$s" in *":COM:"*) fail "$t: COMMON without -fcommon: $s" ;; esac
    s=$(symbols $t -fcommon -fno-common)
    case "$s" in *":COM:"*) fail "$t: -fno-common after -fcommon: $s" ;; esac
done
"$EMBCC" --target=thumbv7em-none-eabi -fcommon -S -o - "$out/cm.c" |
    grep -q '^[[:space:]]*\.comm[[:space:]]*d,40,4' ||
    fail "-fcommon -S has no .comm for d"
# embld merges them: the largest size wins, a real definition wins, and
# without -fcommon the same two units are a multiple definition.
printf 'int buf[10]; int val; int *pa(void) { return buf; }\nvoid _start(void) { for (;;); }\n' > "$out/ca.c"
printf 'int buf[20]; int val = 5; int *pb(void) { return buf; }\n' > "$out/cb.c"
"$EMBCC" --target=x86_64-elf -fcommon "$out/ca.c" "$out/cb.c" -nostdlib \
    -o "$out/cm.elf" || fail "-fcommon units did not link"
"$RE" -s "$out/cm.elf" | awk '$8 == "buf" && $3 == 80 { f = 1 } END { exit !f }' ||
    fail "the merged COMMON buf is not 80 bytes: $("$RE" -s "$out/cm.elf" | grep buf)"
if err=$("$EMBCC" --target=x86_64-elf "$out/ca.c" "$out/cb.c" -nostdlib \
         -o "$out/cm2.elf" 2>&1); then
    fail "without -fcommon two tentative 'buf's linked"
fi
echo "$err" | grep -q "multiple definition of 'buf'" ||
    fail "without -fcommon: $err"
# and on the board: two units' counters are one object, zeroed by the
# startup, the larger array's size
if command -v "$QEMU_ARM" >/dev/null 2>&1; then
    d="$out/cmrun"; mkdir -p "$d"
    cat > "$d/a.c" <<'EOF'
int shared[4]; int hits;
void bump(void) { hits++; shared[3] = 7; }
EOF
    cat > "$d/b.c" <<'EOF'
extern void puts_(const char *s);
extern void putn(long v);
int shared[8]; int hits;
void bump(void);
int main(void)
{
    bump(); bump();
    putn(hits); putn(shared[3]); putn(shared[7]);
    putn((long)sizeof shared);
    puts_("\n==END==\n");
    return 0;
}
EOF
    T=thumbv7m-none-eabi
    for f in boot io; do
        "$EMBCC" --target=$T -c tests/harness/thumb/$f.c -o "$d/$f.o" ||
            fail "the harness does not compile"
    done
    "$EMBCC" --target=$T -O2 -fcommon -c "$d/a.c" -o "$d/a.o" &&
    "$EMBCC" --target=$T -O2 -fcommon -c "$d/b.c" -o "$d/b.o" ||
        fail "the -fcommon units do not compile"
    "$EMBLD" -e reset -Ttext 0x0 -Tdata 0x20000000 "$d/boot.o" "$d/io.o" \
        "$d/a.o" "$d/b.o" -o "$d/img.elf" || fail "embld: -fcommon image"
    sh tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-10}" --until '==END==' \
        "$QEMU_ARM" -M lm3s6965evb -cpu cortex-m3 -nographic \
        -kernel "$d/img.elf" > "$d/out.txt" 2>/dev/null
    got=$(tr -d '\r' < "$d/out.txt" | sed -n 1p)
    [ "$got" = "2 7 0 32 " ] || fail "-fcommon on the board: '$got'"
    echo "-fcommon: tentative definitions are COMMON, embld merges them, and"
    echo "the merged objects are shared on the board"
else
    echo "-fcommon: tentative definitions are COMMON and embld merges them"
fi

# ---- 6. -fsingle-precision-constant -----------------------------------------
cat > "$out/sp.c" <<'EOF'
_Static_assert(sizeof(1.0) == SZ, "an unsuffixed constant");
_Static_assert(sizeof(0x1p-3) == SZ, "a hex one");
_Static_assert(sizeof(1.0L) > 4 || sizeof(long double) == 4, "L stays");
_Static_assert(_Generic(0.5, float: 1, default: 0) == (SZ == 4), "type");
double d = 0.1;
EOF
for t in thumbv7em-none-eabi riscv32-unknown-elf x86_64-elf avr; do
    "$EMBCC" --target=$t -DSZ=4 -fsingle-precision-constant -c "$out/sp.c" \
        -o "$out/sp.o" || fail "$t: 1.0 is not a float under the flag"
    # avr's double IS float, so its sizes cannot tell the two apart
    [ $t = avr ] ||
        "$EMBCC" --target=$t -DSZ=8 -c "$out/sp.c" -o "$out/sp8.o" ||
        fail "$t: 1.0 is not a double without the flag"
done
# the VALUE is the float's: 0.1f widened, 0x3fb99999a0000000
for t in thumbv7em-none-eabi x86_64-elf; do
    "$EMBCC" --target=$t -DSZ=4 -fsingle-precision-constant -c "$out/sp.c" \
        -o "$out/sp.o" || fail "$t: sp.c"
    "$OD" -s -j .data "$out/sp.o" | grep -q '000000a0 9999b93f' ||
        fail "$t: double d = 0.1 does not hold 0.1f: $("$OD" -s -j .data "$out/sp.o")"
done
# and the arithmetic is single: x * 0.1 is a float multiply
printf 'float m(float x) { return x * 0.1; }\n' > "$out/spm.c"
helpers() {
    "$EMBCC" --target=thumbv7em-none-eabi -O2 -c "$out/spm.c" -o "$out/spm.o" "$@" ||
        fail "spm.c"
    "$RE" -s "$out/spm.o" | awk '$7 == "UND" && $8 != "" { printf "%s ", $8 }'
}
[ "$(helpers -fsingle-precision-constant)" = "__mulsf3 " ] ||
    fail "x * 0.1 under the flag calls: $(helpers -fsingle-precision-constant)"
case "$(helpers)" in *__muldf3*) ;; *) fail "x * 0.1 without the flag calls: $(helpers)" ;; esac
printf 'double big = 1e300;\n' > "$out/spw.c"
err=$("$EMBCC" --target=x86_64-elf -fsingle-precision-constant -c \
      "$out/spw.c" -o "$out/spw.o" 2>&1) || fail "1e300 refused: $err"
echo "$err" | grep -q "exceeds the range of 'float'" ||
    fail "1e300 became infinity without a warning: $err"
echo "-fsingle-precision-constant: 1.0 is a float -- its size, its type, its"
echo "value (0.1 is 0.1f) and its arithmetic -- and 1e300 is said to overflow"

# ---- 7. -save-temps -----------------------------------------------------------
st="$out/st"; mkdir -p "$st/build"
printf '#define TWICE(x) ((x) * 2)\nint tw(int x) { return TWICE(x); }\n' > "$st/m.c"
"$EMBCC" --target=thumbv7em-none-eabi -save-temps -c "$st/m.c" \
    -o "$st/build/m.o" || fail "-save-temps -c"
[ -s "$st/build/m.o" ] || fail "-save-temps wrote no object"
grep -q '((x) \* 2)' "$st/build/m.i" || fail "build/m.i is not preprocessed"
grep -q 'TWICE' "$st/build/m.i" && fail "build/m.i still has the macro"
grep -q '^tw:' "$st/build/m.s" || fail "build/m.s is not tw's assembly"
"$EMBCC" --target=thumbv7em-none-eabi -save-temps -S "$st/m.c" \
    -o "$st/build/n.s" || fail "-save-temps -S"
[ -s "$st/build/n.i" ] && [ -s "$st/build/n.s" ] || fail "-save-temps -S: n.i, n.s"
"$EMBCC" --target=x86_64-elf -save-temps "$st/m.c" "$out/s.c" -nostdlib \
    -o "$st/build/prog.elf" || fail "-save-temps with a link"
for f in prog-m.i prog-m.s prog-s.i prog-s.s prog.elf; do
    [ -s "$st/build/$f" ] || fail "-save-temps link: no build/$f"
done
( cd "$st" && "$EMBCC_ABS" --target=x86_64-elf -save-temps=cwd -c m.c \
      -o build/q.o ) || fail "-save-temps=cwd"
[ -s "$st/q.i" ] && [ -s "$st/q.s" ] && [ -s "$st/build/q.o" ] ||
    fail "-save-temps=cwd: q.i and q.s not in the current directory"
"$EMBCC" --target=x86_64-elf -E "$st/m.c" -o "$st/e.i" || fail "-E -o"
grep -q '((x) \* 2)' "$st/e.i" || fail "-E -o FILE did not write FILE"
echo "-save-temps: the .i and .s beside the object, named after the image and"
echo "the source for a link, in the current directory with =cwd"

# ---- 8. promises EmbCC already kept ------------------------------------------
# -fno-zero-initialized-in-bss: an explicit zero is .data, as the flag asks
# (a bootloader that does not clear .bss relies on it); only an object
# with no initializer is .bss.
cat > "$out/zb.c" <<'EOF'
int x = 0; static int y = 0; int arr[4] = { 0 }; int z;
int *get(void) { static int sl = 0; return &sl + y; }
EOF
for t in $ALL; do
    "$EMBCC" --target=$t -O2 -fno-zero-initialized-in-bss -c "$out/zb.c" \
        -o "$out/zb.o" || fail "zb.c on $t"
    tab=$("$OD" -t "$out/zb.o")
    for v in x y arr get.sl; do
        echo "$tab" | grep -E "[[:space:]]\.data[[:space:]].*[[:space:]]$v\$" \
            >/dev/null || fail "$t: '$v' (= 0) is not in .data:
$tab"
    done
    echo "$tab" | grep -E "[[:space:]]\.bss[[:space:]].*[[:space:]]z\$" \
        >/dev/null || fail "$t: 'z' (no initializer) is not in .bss"
done
echo "-fno-zero-initialized-in-bss: = 0 is .data on every target"

# -fstrict-volatile-bitfields: one access of the DECLARED type's width,
# the AAPCS rule, even where a narrower one would reach the field.
cat > "$out/vb.c" <<'EOF'
struct R { volatile unsigned int a : 8; volatile unsigned int b : 24; };
struct H { volatile unsigned short c : 4; volatile unsigned short d : 12; };
struct C { volatile unsigned char e : 3; volatile unsigned char f : 5; };
struct P { volatile unsigned char k; volatile unsigned int m : 8; };
unsigned r_a(struct R *p) { return p->a; }
unsigned r_b(struct R *p) { return p->b; }
void w_a(struct R *p, unsigned v) { p->a = v; }
unsigned r_c(struct H *p) { return p->c; }
unsigned r_d(struct H *p) { return p->d; }
void w_c(struct H *p, unsigned v) { p->c = v; }
unsigned r_f(struct C *p) { return p->f; }
unsigned r_m(struct P *p) { return p->m; }
void w_m(struct P *p, unsigned v) { p->m = v; }
EOF
# accesses FUNC: its loads and stores that are not to the frame or a pool
accesses() {
    sed -n "/<$1>:/,/^\$/p" "$out/vb.s" | awk -F'\t' '
        $2 ~ /^(ldr|str)/ && $3 !~ /(sp|pc)[],]/ {
            sub(/\.w$/, "", $2); printf "%s,", $2 }'
}
for t in $THUMB; do
    for o in -O0 -O2 -Os; do
        "$EMBCC" --target=$t $o -fstrict-volatile-bitfields -c "$out/vb.c" \
            -o "$out/vb.o" || fail "vb.c on $t $o"
        "$OD" -d --no-show-raw-insn --triple=thumbv7em "$out/vb.o" > "$out/vb.s"
        # r_a and r_c are the telling ones: their field is all in the
        # first byte, which an ldrb would reach
        for fw in r_a=ldr, r_b=ldr, w_a=ldr,str, r_c=ldrh, r_d=ldrh, \
                  w_c=ldrh,strh, r_f=ldrb, r_m=ldr, w_m=ldr,str,; do
            fn=${fw%%=*}
            got=$(accesses $fn)
            [ "$fn=$got" = "$fw" ] || {
                sed -n "/<$fn>:/,/^\$/p" "$out/vb.s"
                fail "$t $o $fn: accesses '$got', not '${fw#*=}' -- the" \
                     "declared type's width, once"; }
        done
    done
done
echo "-fstrict-volatile-bitfields: a volatile bit-field is one access of its"
echo "declared width on ARMv7-M, ARMv7E-M and ARMv8-M"

# -fno-delete-null-pointer-checks: a check after a dereference, or of an
# object's address, stays.
cat > "$out/nn.c" <<'EOF'
extern int ext;
int after(int *p) { int v = *p; if (!p) return -7; return v; }
int addr(void) { return &ext != 0; }
int zero(void) { return *(volatile int *)0; }
EOF
for t in thumbv7em-none-eabi riscv32-unknown-elf x86_64-elf; do
    ir=$("$EMBCC" inspect ir "$out/nn.c" --target=$t -O2 \
         -fno-delete-null-pointer-checks) || fail "nn.c on $t"
    echo "$ir" | sed -n '/@after/,/^}/p' | grep -q 'const.4s -7' ||
        fail "$t: the null check after *p was deleted:
$ir"
    echo "$ir" | sed -n '/@addr/,/^}/p' | grep -qE 'cmp\.[48] ne' ||
        fail "$t: &ext != 0 was folded"
    echo "$ir" | sed -n '/@zero/,/^}/p' | grep -q 'load' ||
        fail "$t: the load from address 0 was dropped"
done
echo "-fno-delete-null-pointer-checks: no check is deleted, no address folded"

# -fno-tree-loop-distribute-patterns: no loop becomes a CALL. A hand-written
# memset, and loops the idiom pass recognises, compile to no reference to
# memset or memcpy on any target -- IR_MEMZERO/IR_MEMCPY are inline code.
cat > "$out/ms.c" <<'EOF'
typedef __SIZE_TYPE__ size_t;
void *memset(void *s, int c, size_t n)
{
    unsigned char *p = s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}
static unsigned tab[64], src[64];
void clear(void) { for (int i = 0; i < 64; i++) tab[i] = 0; }
void copy(void) { for (int i = 0; i < 64; i++) tab[i] = src[i]; }
struct big { int w[64]; };
void scopy(struct big *d, const struct big *s) { *d = *s; }
EOF
for t in $ALL; do
    for o in -O2 -Os; do
        "$EMBCC" --target=$t $o -fno-tree-loop-distribute-patterns \
            -c "$out/ms.c" -o "$out/ms.o" || fail "ms.c on $t $o"
        "$RE" -r "$out/ms.o" | grep -E 'mem(set|cpy|move)' &&
            fail "$t $o: a loop or a copy became a call"
        "$RE" -s "$out/ms.o" | awk '$7 == "UND" && $8 != "" { print $8 }' |
            grep . && fail "$t $o: an undefined symbol appeared"
    done
done
echo "-fno-tree-loop-distribute-patterns: no loop or copy becomes a call"

# -fno-short-enums and -mlittle-endian describe every target here as it is
# (each is little-endian; mips-none-elf, the big-endian one, is
# mips-refuse.sh's, where -mlittle-endian is refused and -mbig-endian
# accepted).
cat > "$out/en.c" <<'EOF'
enum e { A, B };
_Static_assert(sizeof(enum e) == sizeof(int), "an enum is int-sized");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little-endian");
EOF
for t in $ALL; do
    "$EMBCC" --target=$t -fno-short-enums -mlittle-endian -c "$out/en.c" \
        -o "$out/en.o" || fail "$t: an enum is not int, or not little-endian"
done
echo "-fno-short-enums and -mlittle-endian: what every target here already is"

# -Werror=implicit-function-declaration: an implicit declaration is an
# error anyway, and the flag no longer claims the warning is missing.
printf 'int f(void) { return undeclared(3); }\n' > "$out/imp.c"
for fl in -Werror=implicit-function-declaration -Wimplicit-function-declaration; do
    if err=$("$EMBCC" --target=thumbv7em-none-eabi -c "$out/imp.c" \
             -o "$out/imp.o" $fl 2>&1); then
        fail "$fl: an implicit declaration compiled"
    fi
    echo "$err" | grep -q "'undeclared' is not declared" ||
        fail "$fl: not the implicit-declaration error: $err"
    echo "$err" | grep -qE 'not a warning EmbCC has|names no warning' &&
        fail "$fl: said the warning does not exist: $err"
done
echo "-Werror=implicit-function-declaration: an error, as it always was"

# -Og is -O1 and -Ofast is -O3 (no fast-math): the same object.
for t in thumbv7em-none-eabi x86_64-elf; do
    "$EMBCC" --target=$t -Og -c "$out/inl.c" -o "$out/og.o" &&
    "$EMBCC" --target=$t -O1 -c "$out/inl.c" -o "$out/o1.o" &&
    cmp -s "$out/og.o" "$out/o1.o" || fail "$t: -Og is not -O1"
    "$EMBCC" --target=$t -Ofast -c "$out/inl.c" -o "$out/of.o" &&
    "$EMBCC" --target=$t -O3 -c "$out/inl.c" -o "$out/o3.o" &&
    cmp -s "$out/of.o" "$out/o3.o" || fail "$t: -Ofast is not -O3"
done
"$EMBCC" --target=x86_64-elf -Ofast -ffast-math --dump-predef |
    grep -q __FAST_MATH__ && fail "-ffast-math defined __FAST_MATH__"
echo "-Og is -O1, -Ofast is -O3, and -ffast-math defines no __FAST_MATH__"

# ---- GCC flags a real embedded build line passes --------------------------
# -march=/-mtune= on a Cortex-M (CMake toolchain files), the CubeMX
# Makefile's assembler listing, -E -dM from standard input (how build
# systems read a compiler's macros), and flags EmbCC satisfies already.
accept thumbv7em-none-eabi "-mtune=cortex-m4 -mtune=cortex-m0plus -fno-ident
 -fident -fno-reorder-functions -freorder-functions -ffp-contract=off
 -ffp-contract=on -ffp-contract=fast -funroll-loops -fno-unroll-loops"
accept armv7a-none-eabi "-march=armv7-a -mtune=cortex-a7"
refuse thumbv7em-none-eabi "is not a Cortex-M core" -mtune=pentium
refuse thumbv7em-none-eabi "is not an architecture EmbCC emits" -march=armv9-a
refuse thumbv7em-none-eabi "the extension '+mve'" -march=armv7e-m+mve
refuse armv7a-none-eabi "EmbCC emits ARMv7-A code" -march=armv8-a
# -march= selects the level, as -mcpu= does, and +fp/+fp.dp the unit
macros() { "$EMBCC" --target=$1 $2 -E -dM - </dev/null; }
for c in "armv6-m|__ARM_ARCH_6M__ 1" "armv7-m|__ARM_ARCH_7M__ 1" \
         "armv8-m.base|__ARM_ARCH_8M_BASE__ 1" \
         "armv8-m.main|__ARM_ARCH_8M_MAIN__ 1"; do
    m=${c%%|*}; want=${c#*|}
    macros thumbv7m-none-eabi -march=$m | grep -q "^#define $want\$" ||
        fail "-march=$m does not define $want"
done
macros thumbv7m-none-eabi "-march=armv7e-m+fp -mfloat-abi=hard" |
    grep -q "^#define __ARM_FP 0x4$" || fail "-march=armv7e-m+fp is not FPv4-SP"
macros thumbv7m-none-eabi "-march=armv7e-m+fp.dp -mfloat-abi=hard" |
    grep -q "^#define __ARM_FP 0xc$" || fail "-march=armv7e-m+fp.dp is not FPv5-D16"
macros thumbv7m-none-eabi "-march=armv7e-m+fp -mfpu=fpv5-d16 -mfloat-abi=hard" |
    grep -q "^#define __ARM_FP 0xc$" || fail "-mfpu= did not override -march='s +fp"
macros thumbv7m-none-eabi "-march=armv8-m.main+fp -mfloat-abi=hard" |
    grep -q "^#define __ARM_FP 0x4$" || fail "-march=armv8-m.main+fp is not FPv5-SP"
echo "-march= and -mtune= on Cortex-M and ARMv7-A"
# -E -dM: every macro at the end of the file, a #define each, also from stdin
printf '#define SQ(x) ((x)*(x))\n#define V(f, ...) g(f, __VA_ARGS__)\n#define E\n' > "$out/dm.c"
"$EMBCC" --target=thumbv7em-none-eabi -E -dM "$out/dm.c" > "$out/dm.out" ||
    fail "-E -dM"
for l in "#define SQ(x) ((x)*(x))" "#define V(f,...) g(f, __VA_ARGS__)" \
         "#define E" "#define __ARM_ARCH_7EM__ 1"; do
    grep -qxF "$l" "$out/dm.out" || fail "-E -dM lacks: $l"
done
printf 'int x;\n' | "$EMBCC" --target=riscv32-unknown-elf -E -dM - |
    grep -q "^#define __riscv 1$" || fail "-E -dM - (standard input)"
printf 'int y(void) { return 7; }\n' | "$EMBCC" --target=riscv32-unknown-elf \
    -x c -c - -o "$out/stdin.o" || fail "-x c -c - (standard input)"
printf 'int x;\n' | "$EMBCC" --target=riscv32-unknown-elf -c - -o /dev/null \
    2> "$out/stdin.err" && fail "standard input without -E or -x was taken"
grep -q "\-E or -x required when input is from standard input" "$out/stdin.err" ||
    fail "standard input without -E or -x: $(cat "$out/stdin.err")"
echo "-E -dM, and standard input with -E or -x"
# -Wa,-a...=FILE: the listing, which is the -S text, beside the object
rm -f "$out/t.lst" "$out/t.o"
"$EMBCC" --target=thumbv7em-none-eabi -O2 -c "$out/t.c" -o "$out/t.o" \
    -Wa,-a,-ad,-alms="$out/t.lst" || fail "-Wa,-a,-ad,-alms="
[ -s "$out/t.o" ] || fail "-Wa,-alms= left no object"
"$EMBCC" --target=thumbv7em-none-eabi -O2 -S "$out/t.c" -o "$out/t.s" &&
    cmp -s "$out/t.s" "$out/t.lst" || fail "the listing is not the -S text"
refuse thumbv7em-none-eabi "is not one the integrated assembler has" -Wa,-z
echo "-Wa,-a...=FILE writes the listing"

# -x assembler-with-cpp: a CubeMX Makefile's way to assemble its
# startup_*.s, preprocessed though the suffix is lowercase; -x assembler
# does not preprocess; an object beside it is still an object
printf '\t.syntax unified\n\t.thumb\n\t.text\n\t.globl s\n\t.type s, %%function\ns:\n#define SEVEN 7\n\tmovs r0, #SEVEN\n\tbx lr\n' > "$out/start.s"
"$EMBCC" --target=thumbv7em-none-eabi -x assembler-with-cpp -c "$out/start.s" \
    -o "$out/start.o" || fail "-x assembler-with-cpp on a .s"
"${EMBCC_LLVM_OBJDUMP:-llvm-objdump}" -d "$out/start.o" | grep -q "movs.*r0, #0x7" ||
    fail "-x assembler-with-cpp did not preprocess the .s"
"$EMBCC" --target=thumbv7em-none-eabi -x assembler -c "$out/start.s" \
    -o "$out/start2.o" 2>/dev/null &&
    fail "-x assembler preprocessed the .s: SEVEN was defined"
"$EMBCC" --target=thumbv7em-none-eabi -c "$out/start.s" -o "$out/start3.o" \
    2>/dev/null && fail "a plain .s was preprocessed"
printf 'int main(void) { return 0; }\n' > "$out/xm.c"
"$EMBCC" --target=thumbv7em-none-eabi -c "$out/xm.c" -o "$out/xm.o" || fail "xm.c"
"$EMBCC" --target=thumbv7em-none-eabi -x assembler-with-cpp -c "$out/start.s" \
    "$out/xm.o" -o "$out/start4.o" > "$out/xm.err" 2>&1 ||
    fail "-x assembler-with-cpp assembled the object beside it: $(cat "$out/xm.err")"
grep -q "linker input unused" "$out/xm.err" ||
    fail "the object beside -x assembler-with-cpp was not a linker input: $(cat "$out/xm.err")"
refuse thumbv7em-none-eabi "unknown language 'assemblr'" -x assemblr
echo "-x assembler-with-cpp preprocesses a .s, -x assembler does not, and an object stays one"
