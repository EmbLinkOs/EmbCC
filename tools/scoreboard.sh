#!/bin/sh
# tools/scoreboard.sh [--quick] [OUT.md] -- where EmbCC stands against clang.
#
# One page of numbers, rerun every round of work, so that "competitive"
# is a measurement rather than a feeling. Each section says what it
# compares and how; lower is better for every ratio (EmbCC / clang).
#
#   1. Speed: tools/bench/run.sh, estimated cycles on the Cortex-M4 and
#      RV32 boards at -O2 and -Os (geomean over its kernels).
#   2. Size: lib/libc compiled by both compilers at -Os for every target
#      clang also has (llvm-nm -S symbol sizes, not .text, which would
#      count read-only data -- docs/internals/testing.md), two ways: per
#      OBJECT, every function of each file both compile, and per FUNCTION,
#      the functions both define. clang gets -fno-inline-functions; EmbCC
#      still inlines a static with one caller, and then the per-function
#      join pairs the caller's whole body with clang's wrapper and drops
#      clang's static -- after printf's engine became one such static,
#      the M4 read 1.40 per function and 1.13 per object. The per-object
#      ratio is the one to steer by. Math (fdlibm, msun) is reported apart
#      because floating-point helpers dominate it.
#   3. Compile speed: lib/libc compiled for x86_64-elf at -O2, one file
#      at a time, wall clock, EmbCC against clang.
#   4. Limitations: distinct refusal messages in src/ -- the constructs
#      EmbCC names instead of compiling. The goal is zero.
#
# --quick skips the bench (minutes of QEMU) and reports sections 2-4.
# Needs clang and llvm-nm; the bench also needs QEMU and the icount plugin
# (tools/bench/run.sh). Run from the tree root after `make embcc`.
set -u
QUICK=0
[ "${1:-}" = --quick ] && { QUICK=1; shift; }
OUT=${1:-/dev/stdout}
EMBCC=${EMBCC:-$PWD/embcc}
W=${TMPDIR:-/tmp}/scoreboard.$$
mkdir -p "$W"
command -v clang >/dev/null 2>&1 && command -v llvm-nm >/dev/null 2>&1 || {
    echo "scoreboard: needs clang and llvm-nm" >&2; exit 1; }
RES=$(clang -print-resource-dir 2>/dev/null)

{
echo "# EmbCC scoreboard"
echo
echo "$(git log --oneline -1 2>/dev/null) -- $(date -u +%Y-%m-%d) -- reference: $(clang --version | head -1)"
echo
echo "Every ratio is EmbCC / clang: below 1.00, EmbCC is ahead."
echo

# ---- 1. speed -------------------------------------------------------------
echo "## 1. Speed (estimated cycles, tools/bench)"
echo
if [ $QUICK = 1 ]; then
    echo "(skipped: --quick)"
else
    echo "| Board | -O2 | -Os |"
    echo "|---|---|---|"
    for O in -O2 -Os; do
        sh tools/bench/run.sh $O m4 rv32 > "$W/bench$O.txt" 2>&1
    done
    for b in m4 rv32; do
        r2=$(awk -v b="$b" '$1 == "==" && $2 == b {on=1} on && $1 == "geomean" {print $4; exit}' "$W/bench-O2.txt")
        rs=$(awk -v b="$b" '$1 == "==" && $2 == b {on=1} on && $1 == "geomean" {print $4; exit}' "$W/bench-Os.txt")
        echo "| $b | ${r2:-?} | ${rs:-?} |"
    done
fi
echo

# ---- 2. size --------------------------------------------------------------
echo "## 2. Code size (lib/libc at -Os)"
echo
echo "Per object: every function of each file both compilers build. Per"
echo "function: the functions both define, which a static EmbCC inlines into"
echo "its one caller skews (see the script)."
echo
echo "| Target | per object | without math | per function | without math | objects |"
echo "|---|---|---|---|---|---|"
for t in thumbv7em-none-eabi thumbv6m-none-eabi riscv32-unknown-elf \
         riscv64-unknown-elf x86_64-elf aarch64-elf mipsel-none-elf \
         powerpc-none-eabi sparc-none-elf loongarch64-unknown-elf avr; do
    ct=$t
    case $t in
        x86_64-elf) ct=x86_64-unknown-elf ;;
        aarch64-elf) ct=aarch64-unknown-elf ;;
        mipsel-none-elf) ct=mipsel-unknown-elf ;;
        powerpc-none-eabi) ct=powerpc-unknown-eabi ;;
        sparc-none-elf) ct=sparc-unknown-elf ;;
    esac
    cflags=
    case $t in
        mips*) cflags="-mcpu=mips32r2 -msoft-float -mno-abicalls -fno-pic -G0" ;;
        powerpc*) cflags="-msoft-float" ;;
        sparc*) cflags="-mcpu=leon3 -msoft-float" ;;   # EmbCC is soft float here
        loongarch*) cflags="-mabi=lp64s -msoft-float" ;;
        thumbv7em*|thumbv6m*) cflags="-mfloat-abi=soft" ;;
        avr) cflags="-mmcu=atmega328p" ;;
        riscv32*) cflags="-march=rv32imac -mabi=ilp32" ;;
        riscv64*) cflags="-march=rv64imac -mabi=lp64 -mcmodel=medany" ;;
    esac
    d=$W/sz-$t; mkdir -p "$d/e" "$d/c"
    for f in lib/libc/src/*/*.c lib/libc/src/math/fdlibm/*.c lib/libc/src/math/msun/*.c; do
        [ -f "$f" ] || continue
        b=$(echo "$f" | sed 's|lib/libc/src/||; s|/|_|g; s|\.c$||')
        "$EMBCC" --target=$t -Os -Ilib/libc/include -Ilib/libc/src/math \
            -c "$f" -o "$d/e/$b.o" 2>/dev/null || continue
        # shellcheck disable=SC2086
        clang --target=$ct $cflags -Os -ffreestanding -fno-builtin \
            -fno-inline-functions -nostdinc -isystem "$RES/include" \
            -Ilib/libc/include -Ilib/libc/src/math -w -c "$f" \
            -o "$d/c/$b.o" 2>/dev/null || { rm -f "$d/e/$b.o"; continue; }
    done
    for k in e c; do
        for o in "$d/$k"/*.o; do
            [ -f "$o" ] || continue
            m=0; case $o in *math*) m=1 ;; esac
            # keyed by file AND name: a static `conv` is in strtol.c and in
            # strtod.c, and joining on the name alone paired one file's
            # with the other's
            llvm-nm -S --defined-only "$o" 2>/dev/null |
                awk -v m=$m -v f="$(basename "$o" .o)" 'NF == 4 && ($3 == "T" || $3 == "t") {
                    printf "%s:%s %d %d\n", f, $4, strtonum_hex($2), m }
                    function strtonum_hex(h,  i, v, c) { v = 0
                        for (i = 1; i <= length(h); i++) {
                            c = index("0123456789abcdef", tolower(substr(h, i, 1))) - 1
                            v = v * 16 + c }
                        return v }'
        done | sort > "$d/$k.sizes"
    done
    # per object: each file's functions summed, then the files both have
    for k in e c; do
        awk '{ split($1, a, ":"); tot[a[1]] += $2; m[a[1]] = $3 }
             END { for (f in tot) print f, tot[f], m[f] }' "$d/$k.sizes" |
            sort > "$d/$k.objs"
    done
    po=$(join "$d/e.objs" "$d/c.objs" | awk '
        { ea += $2; ca += $4; n++; if ($3 == 0) { eo += $2; co += $4 } }
        END { if (n == 0 || ca == 0) { print "? ? 0"; exit }
              printf "%.3f %.3f %d\n", ea / ca, (co ? eo / co : 0), n }')
    pf=$(join "$d/e.sizes" "$d/c.sizes" | awk '
        { ea += $2; ca += $4; n++; if ($3 == 0) { eo += $2; co += $4 } }
        END { if (n == 0 || ca == 0) { print "? ?"; exit }
              printf "%.3f %.3f\n", ea / ca, (co ? eo / co : 0) }')
    set -- $po $pf
    printf "| %s | %s | %s | %s | %s | %s |\n" "$t" "$1" "$2" "$4" "$5" "$3"
done
echo

# ---- 3. compile speed -----------------------------------------------------
echo "## 3. Compile speed (x86_64-elf at -O2, one file at a time, wall clock)"
echo
now() { perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }
# time_corpus NAME EMBCC-FLAGS CLANG-FLAGS FILE...
time_corpus() {
    name=$1 ef=$2 cf=$3; shift 3
    s=$(now)
    for f in "$@"; do
        # shellcheck disable=SC2086
        "$EMBCC" --target=x86_64-elf -O2 $ef -c "$f" -o "$W/ce.o" 2>/dev/null
    done
    m=$(now)
    for f in "$@"; do
        # shellcheck disable=SC2086
        clang --target=x86_64-unknown-elf -O2 $cf -w -c "$f" -o "$W/cc.o" 2>/dev/null
    done
    e=$(now)
    perl -e 'printf "| %s | %d | %.1f s | %.1f s | %.2f |\n", $ARGV[3], $ARGV[4], $ARGV[1]-$ARGV[0], $ARGV[2]-$ARGV[1], ($ARGV[1]-$ARGV[0])/($ARGV[2]-$ARGV[1])' \
        "$s" "$m" "$e" "$name" "$#"
}
echo "| Corpus | files | EmbCC | clang | ratio |"
echo "|---|---|---|---|---|"
LIBC_I="-Ilib/libc/include -Ilib/libc/src/math"
time_corpus "lib/libc" "$LIBC_I" \
    "-ffreestanding -fno-builtin -nostdinc -isystem $RES/include $LIBC_I" \
    lib/libc/src/*/*.c lib/libc/src/math/fdlibm/*.c lib/libc/src/math/msun/*.c
# EmbCC's own sources: the large files (src/opt/opt.c, the backends)
# are where a slow pass or the allocator would show. Both compilers read
# lib/libc's headers, which is what EmbCC uses for this target -- clang
# without them fails most files at once and looks fast. The POSIX
# platform files need a host's headers and are left out of both.
SRCS=$(ls src/*/*.c src/arch/*/*.c | grep -v '^src/platform/p')
# shellcheck disable=SC2086
time_corpus "EmbCC's src/" "-Isrc" \
    "-ffreestanding -nostdinc -isystem $RES/include -Ilib/libc/include -Isrc" $SRCS
echo

# ---- 4. limitations -------------------------------------------------------
echo "## 4. Limitations (distinct refusals in src/)"
echo
# A backend's refusal is `<arch>_refuse(F, i, "what")` or a_refuse; the
# driver's are "... is not supported ..." / "... not yet supported ...".
grep -rhoE '[a-z_]*refuse\([^"]*"[^"]{4,}"' src/arch 2>/dev/null |
    sed 's/^[^"]*"//; s/"$//' | sort -u > "$W/refusals.txt"
grep -rhoE '"[^"]*(is not supported|not yet supported|is not yet supported)[^"]*"' \
    src/driver src/sema src/parse 2>/dev/null | sort -u > "$W/driver.txt"
echo "| Backend refusals | Front-end and driver refusals |"
echo "|---|---|"
echo "| $(wc -l < "$W/refusals.txt" | tr -d ' ') | $(wc -l < "$W/driver.txt" | tr -d ' ') |"
echo
echo "Backend refusals by target:"
echo
for a in src/arch/*/codegen.c; do
    n=$(grep -hoE '[a-z_]*refuse\([^"]*"[^"]{4,}"' "$a" | sort -u | wc -l | tr -d ' ')
    printf -- '- %s: %s\n' "$(basename "$(dirname "$a")")" "$n"
done
} > "$OUT"
case "$W" in */scoreboard.*) rm -rf "${W:?}" ;; esac
