#!/bin/sh
# verify/fuzz/run4.sh -- long double (binary128) fuzzing on RISC-V.
#
#   sh verify/fuzz/run4.sh [-O "OPTS"] 32|64 FIRST LAST
#
# gen4.py adds `long double` to gen 3. On RISC-V that is IEEE binary128 in
# software, which the host's long double is not, so the reference is
# clang's build of the same program run on the same QEMU board (`virt`):
# the program prints its checksum there, and EmbCC's build, given that
# checksum, must return 42 at each of OPTS (default "-O0 -O2 -Os"). At
# RV32 there is no __int128, and long double travels by reference.
#
# Needs clang with the RISC-V target and qemu-system-riscv32/64. Run from
# the tree root after `make embcc embld rt-embedded`. Failing programs are
# kept in $FUZZ_OUT (default verify/fuzz/out) as e4-XLEN-SEED.c.
set -u
opts="-O0 -O2 -Os"
[ "${1:-}" = -O ] && { opts=$2; shift 2; }
[ $# -eq 3 ] || { echo "usage: run4.sh [-O \"OPTS\"] 32|64 FIRST LAST" >&2; exit 2; }
X=$1
case $X in 32|64) ;; *) echo "run4.sh: XLEN is 32 or 64" >&2; exit 2 ;; esac
ROOT=$(pwd)
[ -f "$ROOT/verify/fuzz/gen4.py" ] || { echo "run4.sh: run it from the tree root" >&2; exit 2; }
F=$ROOT/verify/fuzz
EMBCC=${EMBCC:-$ROOT/embcc}; EMBLD=${EMBLD:-$ROOT/embld}
case $EMBCC in /*) ;; *) EMBCC=$ROOT/$EMBCC ;; esac
case $EMBLD in /*) ;; *) EMBLD=$ROOT/$EMBLD ;; esac
export EMBLD EMBCC_VERIFY=1
OUT=${FUZZ_OUT:-$F/out}; mkdir -p "$OUT"
T=riscv$X-unknown-elf
if [ $X = 32 ]; then M="-march=rv32im -mabi=ilp32"; export GEN4_NO128=1
else M="-march=rv64im -mabi=lp64"; fi
W=$(mktemp -d "${TMPDIR:-/tmp}/embfuzz4.XXXXXX") || exit 2
trap 'rm -rf "$W"' EXIT INT TERM
export EMBCC_RISCV_HARNESS=$W
"$EMBCC" --target=$T -c "$F/rvboot.c" -o "$W/boot.o" &&
"$EMBCC" --target=$T -c tests/harness/riscv/io.c -o "$W/io.o" || exit 1
# clang's freestanding build calls these; lib/rt has none
printf '%s\n' 'void *memcpy(void *d, const void *s, __SIZE_TYPE__ n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }' \
    'void *memset(void *d, int c, __SIZE_TYPE__ n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }' > "$W/mem.c"
"$EMBCC" --target=$T -O1 -c "$W/mem.c" -o "$W/mem.o" || exit 1

run() {     # run ELF: the board's console
    sh tests/harness/qrun.sh 60 qemu-system-riscv$X -M virt -bios none -nographic -m 8 \
        -kernel "$1" 2>/dev/null
}
ref() {     # ref EXTRA-CFLAGS: clang's checksum on the board
    clang -target $T $M -mcmodel=medany -ffreestanding -fno-builtin -w -O0 \
        -ffp-contract=off $1 -c "$W/p.c" -o "$W/r.o" 2>/dev/null || return 1
    sh tests/harness/riscv/link.sh "$W/r.elf" "$W/r.o" "$W/mem.o" build/libc/$T/librt.a \
        > /dev/null 2>&1 || return 1
    run "$W/r.elf" | grep -m1 '^[0-9a-f]\{16\}$'
}
for seed in $(seq "$2" "$3"); do
    python3 "$F/gen4.py" "$seed" > "$W/p.c"
    ck=$(ref "") || { echo "seed $seed: reference failed"; continue; }
    ckp=$(ref "-ftrivial-auto-var-init=pattern")
    [ -n "$ck" ] && [ "$ck" = "$ckp" ] || { echo "seed $seed: reference unstable ($ck/$ckp)"; continue; }
    python3 "$F/gen4.py" "$seed" "$ck" > "$W/e.c"
    for o in $opts; do
        "$EMBCC" --target=$T $o -Ilib/libc/include -c "$W/e.c" -o "$W/p.o" ||
            { echo "seed $seed rv$X $o: compile failed"; cp "$W/e.c" "$OUT/e4-$X-$seed.c"; continue; }
        sh tests/harness/riscv/link.sh "$W/p.elf" "$W/p.o" build/libc/$T/librt.a > /dev/null 2>&1 ||
            { echo "seed $seed rv$X $o: link failed"; cp "$W/e.c" "$OUT/e4-$X-$seed.c"; continue; }
        run "$W/p.elf" > /dev/null; st=$?
        [ $st = 42 ] || { echo "seed $seed rv$X $o: exit $st"; cp "$W/e.c" "$OUT/e4-$X-$seed.c"; }
    done
    echo "seed $seed done"
done
