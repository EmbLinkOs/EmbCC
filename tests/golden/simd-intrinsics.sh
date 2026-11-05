#!/bin/sh
# The three SIMD intrinsic headers EmbCC writes (tools/gen-simd-headers.py)
# against the compiler whose real instructions they stand for.
#
#   arm_neon.h     every intrinsic called on fixed pseudo-random inputs,
#                  built by the host's clang (its own arm_neon.h, real
#                  Advanced SIMD) and by EmbCC (lane by lane), on an arm64
#                  host natively; a hash of each result compared
#   arm_mve.h      the same, run on QEMU's mps3-an547 (Cortex-M55, real
#                  MVE for clang's build); the _x forms' don't-care lanes
#                  are not compared
#   riscv_vector.h the same on qemu-system-riscv64 with v=true,vlen=128;
#                  a masked form's agnostic inactive elements zeroed by its
#                  own mask before the hash
#
# Each EmbCC build at -O0, -O1 and -O2. The generators in tests/golden/simd
# read the header itself, so a new intrinsic is tested by being written;
# one clang does not know is a compile error here, which is how a name or
# a signature that is not ACLE's (or the RVV spec's) is caught.
set -u
echo "TEST-MARKER simd-intrinsics"
. "$(dirname "$0")/../lib.sh"
command -v clang >/dev/null 2>&1 || { echo "skipped: no clang"; exit 0; }
cd "$EMBCC_ROOT"
EMBCC=${EMBCC:-$PWD/embcc}
EMBLD=${EMBLD:-$PWD/embld}
export EMBLD
out=$PWD/tests/golden/out/simd-intrinsics
rm -rf "$out"; mkdir -p "$out"
G=$PWD/tests/golden/simd
fail=0

# ---- NEON, natively, where the host is arm64 --------------------------------
if [ "$(uname -m)" = arm64 ] || [ "$(uname -m)" = aarch64 ]; then
    host=arm64-apple-darwin
    [ "$(uname -s)" = Linux ] && host=aarch64-linux-gnu
    python3 "$G/gen_neon_test.py" include/arm_neon.h "$out/nt.c" > /dev/null
    python3 - "$out/nt.c" "$out/ntp.c" <<'EOF'
import re, sys
s = open(sys.argv[1]).read()
s = re.sub(r'  /\* (\w+) \*/ \{(.*)\}\n', lambda m: '  /* %s */ { H = 1469598103934665603ULL; %s} printf("%%s %%016llx\\n", "%s", H);\n' % (m.group(1), m.group(2), m.group(1)), s)
open(sys.argv[2], 'w').write(s)
EOF
    clang -O1 -w -o "$out/nh" "$out/ntp.c" && "$out/nh" > "$out/nh.txt"
    for O in -O0 -O1 -O2; do
        "$EMBCC" --target=$host $O -w -o "$out/ne" "$out/ntp.c" &&
            "$out/ne" > "$out/ne$O.txt"
        d=$(diff "$out/nh.txt" "$out/ne$O.txt" | grep -c '^<')
        [ "$d" = 0 ] && [ -s "$out/nh.txt" ] || {
            echo "FAIL arm_neon.h $O: $d of $(wc -l < "$out/nh.txt") calls differ"; fail=1; }
    done
else
    echo "(arm_neon.h: not on an arm64 host)"
fi

# ---- MVE on the Cortex-M55 -----------------------------------------------
if command -v qemu-system-arm >/dev/null 2>&1; then
    T=thumbv8.1m.main-none-eabihf
    B=$out/m55; mkdir -p "$B"
    { sh tools/build-libc.sh thumbv8m.main-none-eabihf "$B" &&
      sh tools/build-rt.sh thumbv8m.main-none-eabihf "$B"; } > "$B/build.log" 2>&1
    for f in boot io; do
        "$EMBCC" --target=$T -mcpu=cortex-m55 -O1 -c tests/harness/thumb-m55/$f.c -o "$B/$f.o"
    done
    python3 "$G/gen_mve_test.py" include/arm_mve.h "$out/mt.c" > /dev/null
    python3 - "$out/mt.c" "$out/mtp.c" <<'EOF'
import re, sys
s = open(sys.argv[1]).read()
s = re.sub(r'  /\* (\w+) \*/ \{(.*)\}\n', lambda m: '  /* %s */ { H = 1469598103934665603ULL; %s} puts_("%s "); putn((long)(H & 0xffffffff)); puts_("\\n");\n' % (m.group(1), m.group(2), m.group(1)), s)
open(sys.argv[2], 'w').write(s)
EOF
    run55() {
        EMBCC_M55_HARNESS=$B sh tests/harness/thumb-m55/link.sh "$B/x.elf" "$1" \
            "$B/libc.a" "$B/librt.a" &&
        EMBCC_QEMU_UNTIL='==END==' EMBCC_QEMU_TIMEOUT=120 \
            sh tests/harness/thumb-m55/run.sh "$B/x.elf" | tr -d '\r'
    }
    clang --target=$T -mcpu=cortex-m55 -mfloat-abi=hard -O1 -w -ffreestanding \
        -isystem lib/libc/include -c "$out/mtp.c" -o "$B/mc.o" &&
        run55 "$B/mc.o" > "$out/mc.txt"
    for O in -O0 -O1 -O2; do
        "$EMBCC" --target=$T -mcpu=cortex-m55 $O -w -c "$out/mtp.c" -o "$B/me.o" &&
            run55 "$B/me.o" > "$out/me$O.txt"
        d=$(diff "$out/mc.txt" "$out/me$O.txt" | grep -c '^<')
        [ "$d" = 0 ] && [ -s "$out/mc.txt" ] || {
            echo "FAIL arm_mve.h $O: $d calls differ"; fail=1; }
    done
fi

# ---- RVV on QEMU's V ------------------------------------------------------
if command -v qemu-system-riscv64 >/dev/null 2>&1; then
    T=riscv64-unknown-elf
    B=$out/rv; mkdir -p "$B"
    { sh tools/build-libc.sh $T/lp64d "$B" && sh tools/build-rt.sh $T/lp64d "$B"; } \
        > "$B/build.log" 2>&1
    for f in boot io; do
        "$EMBCC" --target=$T -march=rv64gcv -mabi=lp64d -O2 -c tests/harness/riscv/$f.c -o "$B/$f.o"
    done
    "$EMBCC" --target=$T -march=rv64gcv -mabi=lp64d -O1 -c tests/cxx-embedded/board.c -o "$B/board.o"
    python3 "$G/gen_rvv_test.py" include/riscv_vector.h "$out/rt.c" > /dev/null
    runrv() {
        EMBCC_RISCV_HARNESS=$B sh tests/harness/riscv/link.sh "$B/x.elf" "$1" \
            "$B/board.o" "$B/libc.a" "$B/librt.a" "$B/libc.a" &&
        EMBCC_RISCV_CPU="rv64,v=true,vlen=128,elen=64" EMBCC_QEMU_TIMEOUT=120 \
            sh tests/harness/riscv/run.sh "$B/x.elf" 64 | tr -d '\r'
    }
    clang --target=$T -march=rv64gcv -mabi=lp64d -mcmodel=medany -O1 -w \
        -ffreestanding -isystem lib/libc/include -c "$out/rt.c" -o "$B/rc.o" &&
        runrv "$B/rc.o" > "$out/rc.txt"
    for O in -O0 -O1 -O2; do
        "$EMBCC" --target=$T -march=rv64gcv -mabi=lp64d $O -w -c "$out/rt.c" -o "$B/re.o" &&
            runrv "$B/re.o" > "$out/re$O.txt"
        d=$(diff "$out/rc.txt" "$out/re$O.txt" | grep -c '^<')
        [ "$d" = 0 ] && [ -s "$out/rc.txt" ] || {
            echo "FAIL riscv_vector.h $O: $d calls differ"; fail=1; }
    done
fi
[ "$fail" = 0 ] || exit 1
echo "arm_neon.h, arm_mve.h and riscv_vector.h: every intrinsic gives what"
echo "clang's real instructions give, at -O0, -O1 and -O2"
