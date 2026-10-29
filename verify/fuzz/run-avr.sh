#!/bin/sh
# verify/fuzz/run-avr.sh -- differential fuzzing of EmbCC's AVR backend
# against clang's, on EmbSim's uno (the ATmega328P).
#
#   sh verify/fuzz/run-avr.sh [-O "OPTS"] FIRST LAST
#   sh verify/fuzz/run-avr.sh --selftest
#
# run.sh's reference is the host's compiler, whose int is 32 bits; AVR's
# is 16 and its double four bytes, so the same program means something
# else there. Here gen2.py --avr writes the program (i32 is long, a double
# is mixed through its four bytes, and it is small enough for 32 KB), clang
# builds it for the ATmega328P at -O2 and at -O1, and their runs on EmbSim
# print the checksum: the reference, when the two agree. Not clang's -O0,
# which LLVM 23 miscompiles on AVR -- a (u64) of an i16 came out with
# 0x01010101 for its high half, of a u16 with stack bytes. EmbCC then builds the program with that checksum
# built in, at each of OPTS (default "-O0 -O2 -Os"), and its run must print
# 42. Both link against EmbCC's harness and runtime -- clang's object
# calls avr-gcc's routines, __udivmodsi4 and the rest (lib/rt/avrdivmod.c).
# A program that disagrees with itself under two stack fillings reads
# uninitialised memory: the generator's bug, reported and skipped. One
# whose image does not fit the part is skipped and counted.
#
# Run from the tree root, after `make embcc embld embsim rt-embedded
# libc-embedded` (memcpy and memset are the C library's).
# Every program that fails is kept in $FUZZ_OUT (default verify/fuzz/out)
# as avr-SEED.c, ready for reduce.py. A line per failure, then `seed N
# done`; EMBCC_VERIFY=1 is set, as the test suite sets it.
opts="-O0 -O2 -Os"
selftest=0
while [ $# -gt 0 ]; do
    case $1 in
    -O) opts=$2; shift 2 ;;
    --selftest) selftest=1; shift ;;
    -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "run-avr.sh: unknown option $1" >&2; exit 2 ;;
    *) break ;;
    esac
done
if [ $selftest = 0 ] && [ $# -ne 2 ]; then
    echo "usage: run-avr.sh [-O \"OPTS\"] FIRST LAST | --selftest" >&2
    exit 2
fi

ROOT=$(pwd)
[ -f "$ROOT/verify/fuzz/gen2.py" ] || { echo "run-avr.sh: run it from the tree root" >&2; exit 2; }
F=$ROOT/verify/fuzz
EMBCC=${EMBCC:-$ROOT/embcc}; EMBLD=${EMBLD:-$ROOT/embld}; EMBSIM=${EMBSIM:-$ROOT/embsim}
export EMBLD EMBCC_VERIFY=1
CLANG=${FUZZ_CC:-clang}
OUT=${FUZZ_OUT:-$F/out}
mkdir -p "$OUT"
W=$(mktemp -d "${TMPDIR:-/tmp}/embfuzz-avr.XXXXXX") || exit 2
trap 'rm -rf "$W"' EXIT INT TERM

# the harness, once: boot and io, the runtime, and two drivers -- one
# printing the checksum (the reference's), one printing main's result
H=$W/h; mkdir -p "$H"
cat > "$W/ckdrv.c" <<'EOF'
int prog_main(void);
void putn(long v);
void puts_(const char *s);
extern unsigned long long ck;
int main(void)
{
    prog_main();
    putn((long)(unsigned long)(ck >> 32)); puts_(" ");
    putn((long)(unsigned long)ck); puts_("\n==END==\n");
    for (;;) ;
}
EOF
cat > "$W/rdrv.c" <<'EOF'
int prog_main(void);
void putn(long v);
void puts_(const char *s);
int main(void)
{
    putn((long)prog_main()); puts_("\n==END==\n");
    for (;;) ;
}
EOF
"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$H/boot.o" &&
"$EMBCC" --target=avr -Os -c tests/harness/avr/io.c -o "$H/io.o" &&
"$EMBCC" --target=avr -O1 -c "$W/ckdrv.c" -o "$W/ckdrv.o" &&
"$EMBCC" --target=avr -O1 -c "$W/rdrv.c" -o "$W/rdrv.o" &&
sh tools/build-rt.sh avr "$H" > "$H/rt.log" 2>&1 ||
    { echo "run-avr.sh: the harness does not build"; exit 2; }

# run ELF: what it printed before ==END==, or why nothing
run() {
    "$EMBSIM" "$1" --board uno --until '==END==' --max-insns 200000000 > "$1.out" 2>&1
    head -1 "$1.out"
}

# link OBJ DRIVER ELF: 0 when it links, 3 when the image is too big
link() {
    EMBCC_AVR_HARNESS="$H" sh tests/harness/avr/link.sh "$3" "$1" "$2" "$H/librt.a" \
        "$ROOT/build/libc/avr/libc.a" "$H/librt.a" \
        > "$3.lerr" 2>&1 && return 0
    grep -q 'rom-limit\|flash' "$3.lerr" && return 3
    return 1
}

# the reference: the checksum, as hi and lo, or nothing
ref_ck() {   # ref_ck FILE OPT [CLANG FLAGS]
    f=$1; shift
    "$CLANG" --target=avr -mmcu=atmega328p -w -ffp-contract=off "$@" \
        -Dmain=prog_main -c "$f" -o "$W/r.o" 2>/dev/null || return 1
    link "$W/r.o" "$W/ckdrv.o" "$W/r.elf"; st=$?
    [ $st = 0 ] || return $st
    run "$W/r.elf"
}

# one level: "result N", or why there is none
emb() {   # emb OPT FILE
    "$EMBCC" --target=avr $1 -Dmain=prog_main -c "$2" -o "$W/e.o" ||
        { echo "compile failed"; return; }
    link "$W/e.o" "$W/rdrv.o" "$W/e.elf"; st=$?
    [ $st = 3 ] && { echo "too big"; return; }
    [ $st = 0 ] || { echo "link failed: $(head -1 "$W/e.elf.lerr")"; return; }
    echo "result $(run "$W/e.elf" | tr -d ' ')"
}

# "hi lo" -> the 64-bit value in decimal
ck64() { python3 -c 'import sys; print(int(sys.argv[1]) % 2**32 * 2**32 + int(sys.argv[2]) % 2**32)' $1; }

if [ $selftest = 1 ]; then
    python3 "$F/gen2.py" 2 --avr > "$W/p.c"
    hl=$(ref_ck "$W/p.c" -O2) || { echo "selftest: the reference does not build or run"; exit 1; }
    ck=$(ck64 "$hl")
    python3 "$F/gen2.py" 2 "$ck" --avr > "$W/good.c"
    wrong=$(python3 -c 'import sys; print((int(sys.argv[1]) + 1) % 2**64)' "$ck")
    python3 "$F/gen2.py" 2 "$wrong" --avr > "$W/bad.c"
    r=$(emb -O0 "$W/good.c"); [ "$r" = "result 42" ] || { echo "selftest: the true checksum gave '$r'"; exit 1; }
    r=$(emb -O0 "$W/bad.c"); [ "$r" != "result 42" ] || { echo "selftest: a wrong checksum passed"; exit 1; }
    echo "selftest: the runner tells a right checksum from a wrong one on the uno"
    exit 0
fi

big=0 nore=0
for seed in $(seq "$1" "$2"); do
    python3 "$F/gen2.py" "$seed" --avr > "$W/p.c"
    hl=$(ref_ck "$W/p.c" -O2); st=$?
    [ $st = 3 ] && { big=$((big + 1)); echo "seed $seed: too big for the part, skipped"; continue; }
    [ $st = 0 ] && [ -n "$hl" ] || { echo "seed $seed: the reference failed"; continue; }
    hl1=$(ref_ck "$W/p.c" -O1)
    [ "$hl1" = "$hl" ] || { nore=$((nore + 1)); echo "seed $seed: clang's -O1 and -O2 disagree, no reference"; continue; }
    hlp=$(ref_ck "$W/p.c" -O2 -ftrivial-auto-var-init=pattern)
    [ "$hlp" = "$hl" ] || { echo "seed $seed: GENERATOR reads uninitialised memory"; continue; }
    python3 "$F/gen2.py" "$seed" "$(ck64 "$hl")" --avr > "$W/e.c"
    for o in $opts; do
        r=$(emb $o "$W/e.c")
        case $r in
        "result 42") ;;
        "too big") big=$((big + 1)); echo "seed $seed $o: too big for the part, skipped" ;;
        *) echo "seed $seed avr $o: $r"; cp "$W/e.c" "$OUT/avr-$seed.c" ;;
        esac
    done
    echo "seed $seed done"
done
echo "run-avr: $big builds too big for the part, $nore seeds with no reference"
