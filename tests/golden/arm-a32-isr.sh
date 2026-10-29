#!/bin/sh
# ARMv7-A interrupt handlers: __attribute__((interrupt("IRQ"))) and GCC's
# other kinds -- FIQ, ABORT, SWI, UNDEF -- in ARM state. A handler pushes
# r0-r3, r12 and lr with the callee-saved registers it uses (an even
# count, so the mode's 8-aligned stack stays so), IRQ, FIQ and ABORT
# first take 4 off lr, and every one returns with ldm sp!, {..., pc}^,
# which restores CPSR from the mode's SPSR as it takes pc.
#
#   THE BAD FORMS are refused by name: parameters, a result, an argument
#   that names no kind, one that is not a string, and a handler built for
#   an FPU (its VFP registers and FPSCR would not be saved; GCC asks for
#   -mgeneral-regs-only there).
#
#   THE SHAPE: each kind's lr adjustment, the registers pushed, the
#   exception return, no bx lr and no tail call.
#
#   THE RUN, on QEMU's virt (tests/golden/arm-a32-isr/main.c): the
#   generic timer's virtual interrupt, through the GICv2, interrupts a loop
#   that keeps a pattern in r0-r12 and a C computation, with a handler
#   that counts and one that calls, at -O0 to -Os; time counted in
#   instructions (-icount), so hundreds of interrupts land per phase.
set -u
echo "TEST-MARKER arm-a32-isr"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-./embld}
T=armv7a-none-eabi
out=tests/golden/out/arm-a32-isr
rm -rf "${out:?}"; mkdir -p "$out"
export EMBCC_VERIFY=1
fail() { echo "FAIL: $*"; exit 1; }

# ---- the bad forms -----------------------------------------------------------
refc() {            # refc WHAT PATTERN SOURCE [FLAGS]
    printf '%s\n' "$3" > "$out/bad.c"
    # shellcheck disable=SC2086
    if "$EMBCC" --target=$T ${4:-} -c "$out/bad.c" -o /dev/null 2> "$out/bad.err"; then
        fail "$1 was accepted"
    fi
    grep -q -- "$2" "$out/bad.err" || {
        cat "$out/bad.err"; fail "$1 was refused, but not by name"; }
}
refc "a handler with parameters" "takes parameters" \
    'void __attribute__((interrupt("IRQ"))) h(int x) { (void)x; }'
refc "a handler with a result" "returns a value" \
    'int __attribute__((interrupt("FIQ"))) h(void) { return 1; }'
refc 'interrupt("NMI")' 'interrupt wants "IRQ", "FIQ", "ABORT", "SWI" or "UNDEF"' \
    'void __attribute__((interrupt("NMI"))) h(void) { }'
refc "interrupt(1)" "interrupt's argument is a string on ARM" \
    'void __attribute__((interrupt(1))) h(void) { }'
refc "a handler with an FPU" "would not save the VFP registers" \
    'void __attribute__((interrupt)) h(void) { }' "-mfpu=vfpv3-d16 -mfloat-abi=hard"
echo "arm-a32-isr: parameters, a result, an unknown kind, a number and an FPU are refused by name"

# ---- the shape ---------------------------------------------------------------
cat > "$out/shape.c" <<'EOF'
volatile int count;
extern void work(int);
void __attribute__((interrupt("IRQ"))) irq(void) { count++; }
void __attribute__((interrupt)) dflt(void) { count += 2; }
void __attribute__((interrupt("FIQ"))) fiq(void) { long long x = count; count = (int)(x * x / 7); }
void __attribute__((interrupt("ABORT"))) abt(void) { work(count); }
void __attribute__((interrupt("SWI"))) swi(void) { work(count); }
void __attribute__((interrupt("UNDEF"))) und(void) { count = 0; }
EOF
for O in -O0 -O2 -Os; do
    "$EMBCC" --target=$T $O -c "$out/shape.c" -o "$out/shape$O.o" ||
        fail "$O: the handlers do not compile"
    llvm-objdump -d "$out/shape$O.o" > "$out/shape$O.dis" ||
        fail "llvm-objdump does not read the handlers"
    for fn in irq dflt fiq abt swi und; do
        b=$(sed -n "/<$fn>:/,/^\$/p" "$out/shape$O.dis")
        first=$(echo "$b" | sed -n 2p)
        case $fn in
        swi|und)
            echo "$first" | grep -q 'sub[[:space:]]*lr, lr, #4' &&
                { echo "$b"; fail "$O: $fn takes 4 off lr"; } ;;
        *)
            echo "$first" | grep -q 'sub[[:space:]]*lr, lr, #4' ||
                { echo "$b"; fail "$O: $fn does not begin with sub lr, lr, #4"; } ;;
        esac
        push=$(echo "$b" | grep -m1 'push' | sed 's/.*{\(.*\)}.*/\1/')
        for r in r0 r1 r2 r3 r12 lr; do
            echo "$push" | tr -d ' ' | tr ',' '\n' | grep -qx "$r" ||
                { echo "$b"; fail "$O: $fn does not push $r"; }
        done
        n=$(echo "$push" | tr ',' '\n' | wc -l | tr -d ' ')
        [ $((n % 2)) = 0 ] || fail "$O: $fn pushes an odd count ($push)"
        last=$(echo "$b" | awk 'NF { l = $0 } END { print l }')
        echo "$last" | grep -qE 'ldm[[:space:]]+sp!, \{.*pc\} \^' ||
            { echo "$b"; fail "$O: $fn does not return with ldm sp!, {..., pc}^"; }
        echo "$b" | grep -qE '[[:space:]](bx[[:space:]]+lr|b[[:space:]]+[0-9a-f]+ <(work)>)' &&
            { echo "$b"; fail "$O: $fn returns another way, or tail-calls"; }
    done
done
echo "arm-a32-isr: IRQ/FIQ/ABORT take 4 off lr and SWI/UNDEF do not; r0-r3, r12 and lr pushed, an even count; ldm ...^; -O0 -O2 -Os"

# ---- the run -----------------------------------------------------------------
QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || { echo "arm-a32-isr: run skipped: $QEMU not found"; exit 0; }
EMBCC="$EMBCC" sh tools/build-rt.sh $T "$out/lib" > "$out/rt.log" 2>&1 ||
    fail "lib/rt does not build for $T"
EMBCC="$EMBCC" sh tools/build-libc.sh $T "$out/lib" > "$out/libc.log" 2>&1 ||
    fail "lib/libc does not build for $T"
"$EMBCC" --target=$T -O1 -DHARNESS_LIBC -c tests/harness/arm-a32/boot.c -o "$out/boot.o" &&
"$EMBCC" --target=$T -O1 -c tests/harness/arm-a32/io.c -o "$out/io.o" ||
    fail "the harness does not compile"
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=$T $O -c tests/golden/arm-a32-isr/main.c -o "$out/m$O.o" ||
        fail "main.c does not compile at $O"
    EMBLD="$EMBLD" EMBCC_A32_HARNESS="$out" sh tests/harness/arm-a32/link.sh \
        "$out/m$O.elf" "$out/m$O.o" "$out/lib/libc.a" "$out/lib/librt.a" ||
        fail "main.c does not link at $O"
    tests/harness/qrun.sh "${EMBCC_QEMU_TIMEOUT:-30}" --until "==EXIT [0-9]* ==" \
        "$QEMU" -M virt -cpu cortex-a15 -m 128 -nographic -monitor none \
        -semihosting -icount shift=0 -kernel "$out/m$O.elf" \
        > "$out/m$O.out" 2>/dev/null
    s=$(sed -n 's/.*==EXIT \([0-9]*\) ==.*/\1/p' "$out/m$O.out" | tail -1)
    [ "$s" = 42 ] || { cat "$out/m$O.out"; fail "the run at $O exited ${s:-without the sentinel}"; }
    echo "  $O: $(grep '^leaf' "$out/m$O.out" | sed 's/, torture.*//'); $(grep '^calls' "$out/m$O.out" | sed 's/, torture.*//')"
done
echo "arm-a32-isr: the timer interrupts r0-r12 and a computation unharmed, at -O0 to -Os"
