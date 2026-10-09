#!/bin/sh
# RISC-V interrupt handlers: __attribute__((interrupt)), ("machine") and
# ("supervisor"), as GCC and clang define them.
#
# A handler is entered between two instructions of code that had a value in
# every register, so it must put back every register it changes -- not only
# the callee-saved ones -- and return with mret or sret. Three checks:
#
#   THE BAD FORMS are refused by name: parameters, a result, the deprecated
#   interrupt("user"), an argument that names no mode, two modes at once,
#   GCC's MIPS-only modifiers.
#
#   THE SHAPE, against clang and against the rule. A handler that calls
#   anything saves exactly the registers clang saves -- every caller-saved
#   integer register and every floating-point one a call may clobber any
#   part of -- at every float ABI (ilp32, ilp32f with F and with D, ilp32d,
#   lp64, lp64d). A handler that calls nothing saves exactly the
#   caller-saved registers its instructions write, read off llvm-objdump's
#   disassembly rather than from the compiler's own decoder. Each returns
#   with mret or sret and never with ret, and is four-aligned with the C
#   extension.
#
#   THE RUN, on QEMU's virt board (tests/golden/riscv-isr/main.c): the
#   machine timer interrupts code that keeps a pattern in every
#   caller-saved register (torture.S), and a C computation, hundreds of
#   times per phase -- a leaf handler, one that calls, one with a frame
#   past addi's reach, and in supervisor mode a handler taking a delegated
#   software interrupt the machine timer raises. At RV32 and RV64, soft and
#   hard float, -O0 to -Os.
set -u
echo "TEST-MARKER riscv-isr"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/riscv-isr
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
OBJDUMP=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
CLANG=${EMBCC_CLANG:-clang}
d=tests/golden/riscv-isr
fail() { echo "FAIL: $*"; exit 1; }

# ---- the bad forms ----------------------------------------------------
refuse() {         # refuse TRIPLE 'source' 'expected text'
    printf '%s\n' "$2" > "$out/bad.c"
    if "$EMBCC" --target=$1 -c "$out/bad.c" -o "$out/bad.o" \
        2> "$out/bad.err"; then
        fail "$1 compiled: $2"
    fi
    grep -q "$3" "$out/bad.err" || {
        echo "$1: $2"; cat "$out/bad.err"; fail "not refused with '$3'"; }
}
for t in riscv32-unknown-elf riscv64-unknown-elf; do
    refuse $t '__attribute__((interrupt)) void h(int x) { (void)x; }' \
        "interrupt handler 'h' takes parameters"
    refuse $t '__attribute__((interrupt)) void h(int x, ...) { (void)x; }' \
        "interrupt handler 'h' takes parameters"
    refuse $t '__attribute__((interrupt("supervisor"))) int h(void) { return 1; }' \
        "interrupt handler 'h' returns a value"
    refuse $t '__attribute__((interrupt("user"))) void h(void) { }' \
        'interrupt("user"))) is not supported'
    refuse $t '__attribute__((interrupt("hypervisor"))) void h(void) { }' \
        'interrupt wants "machine" or "supervisor"'
    refuse $t '__attribute__((interrupt("machine"), interrupt("supervisor"))) void h(void) { }' \
        'two different interrupt attributes'
    refuse $t '__attribute__((interrupt)) void h(void); __attribute__((interrupt("supervisor"))) void h(void) { }' \
        'different kind of interrupt handler'
    refuse $t '__attribute__((interrupt, keep_interrupts_masked)) void h(void) { }' \
        'keep_interrupts_masked)) is not supported'
done
printf 'extern "C" __attribute__((interrupt)) void h(void) { }\n' > "$out/bad.cc"
"$EMBCC" --target=riscv32-unknown-elf -fno-exceptions -c "$out/bad.cc" \
    -o "$out/bad.o" 2> "$out/bad.err" && fail "C++ compiled an interrupt handler"
grep -q 'not supported in C++ yet' "$out/bad.err" || fail "C++: $(cat "$out/bad.err")"
echo "the bad forms are refused by name, and C++ refuses the attribute"

# ---- the shape --------------------------------------------------------
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "(SKIP: no $OBJDUMP for the shape)"; OBJDUMP=; }
cat > "$out/shape.c" <<'EOF'
volatile long v[8];
volatile unsigned long cnt;
void ext(void);
__attribute__((interrupt)) void leaf1(void) { cnt++; }
__attribute__((interrupt("supervisor"))) void leaf2(void)
{
    long a = v[0], b = v[1], c = v[2], d = v[3], e = v[4], f = v[5];
    v[6] = a * b + c * d + e * f + (a ^ c) * (b - d) + e / (f | 1);
}
#ifdef __riscv_flen
volatile float fl[4];
volatile double db[4];
__attribute__((interrupt)) void fleaf(void)
{
    fl[0] = fl[1] * fl[2] + fl[3] * fl[1] - fl[2] / fl[3];
#if __riscv_flen == 64
    db[0] = db[1] * db[2] + db[3];
#endif
}
#endif
__attribute__((interrupt)) void calls(void) { ext(); cnt++; }
__attribute__((interrupt("supervisor"))) void scalls(void) { ext(); }
/* a prototype without the attribute, the definition with it */
void late(void);
__attribute__((interrupt)) void late(void) { cnt += 2; }
EOF
# The registers a function's prologue stores to sp, after its first addi:
# "ra t0 ... ft0 ...". Integer s-registers are left out, which both
# compilers save the ordinary way when they use them.
saves() {          # saves FILE.dis FUNCTION
    sed -n "/<$2>:/,/^\$/p" "$1" | awk '
        NR == 1 { next }
        NR == 2 && $2 == "addi" { next }
        $2 ~ /^f?s[wd]$/ && $4 ~ /\(sp\)$/ {
            r = $3; sub(/,/, "", r); print r; next }
        { exit }' | grep -vxE 's[0-9]+' | sort -u | tr '\n' ' '
}
# The registers a function's instructions write: the first operand of
# everything but a store, a branch or jump, and the CSR writes.
writes() {         # writes FILE.dis FUNCTION
    sed -n "/<$2>:/,/^\$/p" "$1" | awk '
        NR == 1 { next }
        $2 ~ /^f?s[bhwd]$/ { next }
        $2 ~ /^(b|j|ret|mret|sret|fence|ecall|ebreak|unimp|nop|csrw|csrs|csrc)/ { next }
        { r = $3; sub(/,.*/, "", r); print r }' | sort -u
}
for cfg in "32 rv32imac ilp32" "32 rv32imafc ilp32f" "32 rv32imafc ilp32" \
           "32 rv32imafdc ilp32f" "32 rv32imafdc ilp32d" "64 rv64imac lp64" \
           "64 rv64imafdc lp64d" "64 rv64imafdc lp64"; do
    [ -n "$OBJDUMP" ] || break
    set -- $cfg
    x=$1 m=$2 a=$3
    t=riscv$x-unknown-elf
    for O in -O0 -O2 -Os; do
        o=$out/shape-$m-$a$O
        "$EMBCC" --target=$t -march=$m -mabi=$a $O -c "$out/shape.c" \
            -o "$o.o" || fail "$m $a $O: shape.c did not compile"
        "$OBJDUMP" -d --no-show-raw-insn "$o.o" > "$o.dis"
        fns="leaf1 leaf2 calls scalls late"
        case $m in *f*) fns="$fns fleaf";; esac
        for fn in $fns; do
            body=$(sed -n "/<$fn>:/,/^\$/p" "$o.dis")
            # four-aligned, and returning with the right instruction
            addr=$(echo "$body" | head -1 | awk '{print $1}')
            case $addr in *[048c]) ;; *) fail "$m $a $O: $fn is at $addr, not four-aligned";; esac
            want=mret; case $fn in leaf2|scalls) want=sret;; esac
            echo "$body" | grep -qE "^[[:space:]]+[0-9a-f]+:[[:space:]]+$want\$" ||
                fail "$m $a $O: $fn does not return with $want"
            echo "$body" | grep -qE "^[[:space:]]+[0-9a-f]+:[[:space:]]+(ret|jr)([[:space:]]|\$)" &&
                fail "$m $a $O: $fn has an ordinary return"
            got=$(saves "$o.dis" $fn)
            case $fn in
            calls|scalls)
                # every caller-saved register, as clang saves them
                if command -v "$CLANG" >/dev/null 2>&1; then
                    [ -f "$out/clang-$m-$a.s" ] ||
                        "$CLANG" --target=$t -march=$m -mabi=$a -O2 -S \
                            -o "$out/clang-$m-$a.s" "$out/shape.c" ||
                        fail "clang did not compile shape.c for $m $a"
                    cw=$(sed -n "/^$fn:/,/ret\$/p" "$out/clang-$m-$a.s" |
                         awk '$1 ~ /^f?s[wd]$/ && $2 ~ /,$/ && $3 ~ /\(sp\)$/ {
                             r = $2; sub(/,/, "", r); print r }' |
                         grep -vxE 's[0-9]+' | sort -u | tr '\n' ' ')
                    case "$cw" in *ra*t6*) ;; *)
                        fail "clang's $fn for $m $a saves '$cw': not read";; esac
                    [ "$got" = "$cw" ] || {
                        echo "embcc: $got"; echo "clang: $cw"
                        fail "$m $a $O: $fn does not save what clang saves"; }
                fi ;;
            *)
                # exactly the caller-saved registers its instructions
                # write -- with fs0-fs11 among them where the ABI keeps
                # fewer bits of those than the registers hold
                cs='ra|t[0-6]|a[0-7]|ft[0-9]|ft1[01]|fa[0-7]'
                case "$m/$a" in
                *imafdc/ilp32f|*/ilp32|*/lp64) cs="$cs|fs[0-9]|fs1[01]" ;;
                esac
                wr=$(writes "$o.dis" $fn | grep -xE "$cs" | tr '\n' ' ')
                gs=$(echo $got | tr ' ' '\n' | grep -xE "$cs" | tr '\n' ' ')
                [ "$gs" = "$wr" ] || {
                    echo "saves:  $gs"; echo "writes: $wr"
                    fail "$m $a $O: $fn does not save exactly what it writes"; } ;;
            esac
        done
    done
done
[ -n "$OBJDUMP" ] && echo "handlers save clang's set when they call and exactly what they write when they do not, at eight ISA/ABI pairs and three levels; mret/sret, four-aligned"

# ---- the run ----------------------------------------------------------
want="A 0 0 1 1
B 0 0 1 1
D 0 0 1 1
C 0 0 1 1"
runs=0
for cfg in "32 - -" "64 - -" "32 rv32imafc ilp32f" "64 rv64imafdc lp64d"; do
    set -- $cfg
    x=$1
    flags=
    [ "$2" = - ] || flags="-march=$2 -mabi=$3"
    t=riscv$x-unknown-elf
    Q=${EMBCC_QEMU_RISCV:-qemu-system-riscv$x}
    command -v "$Q" >/dev/null 2>&1 || { echo "(SKIP: no $Q for RV$x)"; continue; }
    S=$out/run$x-$2; mkdir -p "$S"
    for f in boot io; do
        "$EMBCC" --target=$t $flags -O1 -c tests/harness/riscv/$f.c -o "$S/$f.o" ||
            fail "RV$x $flags: the harness did not compile"
    done
    "$EMBCC" --target=$t $flags -c $d/torture.S -o "$S/torture.o" ||
        fail "RV$x $flags: torture.S did not assemble"
    for O in -O0 -O1 -O2 -Os; do
        "$EMBCC" --target=$t $flags $O -c $d/main.c -o "$S/main$O.o" ||
            fail "RV$x $flags $O: main.c did not compile"
        EMBCC_RISCV_HARNESS="$S" sh tests/harness/riscv/link.sh "$S/t$O.elf" \
            "$S/main$O.o" "$S/torture.o" || fail "RV$x $flags $O: did not link"
        # -icount: mtime counts instructions, so the interrupts land in the
        # same places however busy the machine is
        tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" "$Q" -M virt \
            -bios none -nographic -m 8 -icount shift=0 \
            -kernel "$S/t$O.elf" 2>/dev/null | tr -d '\r' > "$S/t$O.out"
        got=$(head -4 "$S/t$O.out" | sed 's/ *$//')
        if [ "$got" != "$want" ]; then
            echo "RV$x $flags $O: wanted"; echo "$want"; echo "got:"
            head -c 400 "$S/t$O.out"; echo
            fail "RV$x $flags $O: a handler changed the interrupted code's registers"
        fi
        counts=$(sed -n 5p "$S/t$O.out")
        set -- $counts
        [ "${5:-x}" = 0 ] || fail "RV$x $flags $O: unexpected trap causes: $counts"
        runs=$((runs + 1))
        echo "  RV$x ${flags:-soft-float} $O: interrupts per phase A B D C(supervisor) = $1 $2 $3 $4"
    done
done
echo "$runs runs: every caller-saved register (and float, and its full width) survives hundreds of timer interrupts per phase; leaf, calling, large-frame and supervisor handlers"
