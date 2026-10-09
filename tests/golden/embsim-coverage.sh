#!/bin/sh
# embsim --coverage: the lines and functions a run executed, mapped
# through the image's line table.
#
# tests/golden/embsim-an/cov.c marks every line that has code with the
# times it runs (`=N`, `=0` for never): a branch taken and one not, a
# loop, a function called from it and one never called. Built at -O0 -g
# on the three cores -- the Cortex-M3 (lm3s6965evb), RV32 (virt) and the
# AVR (uno), each in its harness -- the report must give exactly those
# counts, line for line, and no code on an unmarked line:
#   - the text report (gcov's layout): each line of cov.c;
#   - the lcov tracefile: DA for exactly the marked lines, FN and FNDA
#     for the three functions, LF/LH and FNF/FNH that add up, and every
#     record closed;
#   - the summary lines;
#   - an image without -g: the functions alone, by the symbol table;
#   - the run itself unchanged by --coverage: its output and --count.
# And the same program built by clang, whose line table is DWARF 5 (its
# file table in the v5 form, the directory apart) and uses the special
# opcodes EmbCC's does not: the counts must be the marks again, where
# clang puts a function's prologue on its `{` line rather than on the
# line of its name.
set -u
echo "TEST-MARKER embsim-coverage"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=${EMBLD:-$PWD/embld}
EMBSIM=${EMBSIM:-$PWD/embsim}
CLANG=${EMBCC_REF_GCC_THUMB:-clang}
export EMBLD
out=tests/golden/out/embsim-coverage
rm -rf "${out:?}"; mkdir -p "$out"
src=tests/golden/embsim-an/cov.c
fail=0
[ -x "$EMBSIM" ] || { echo "FAIL: $EMBSIM is not built (make embsim)"; exit 1; }

# the marks: "LINE COUNT" for each line that has code; and clang's, with
# a function's mark on the `{` line after it
awk '{ if (match($0, /\/\* =[0-9]+ \*\//)) {
         v = substr($0, RSTART + 4, RLENGTH - 7); print NR, v + 0 } }' \
    "$src" > "$out/expect"
[ "$(wc -l < "$out/expect")" -ge 15 ] || { echo "FAIL: cov.c has lost its marks"; exit 1; }
awk '{ line[NR] = $0 }
     END { for (i = 1; i <= NR; i++)
             if (match(line[i], /\/\* =[0-9]+ \*\//)) {
                 v = substr(line[i], RSTART + 4, RLENGTH - 7)
                 print (line[i + 1] == "{" ? i + 1 : i), v + 0 } }' \
    "$src" > "$out/expect-clang"

# build TAG: the harness and cov.c, as $out/TAG.elf (and TAG-nog.elf
# without -g)
build() {
    tag=$1; h=$out/$tag; mkdir -p "$h"
    case $tag in
    m3) t=thumbv7m-none-eabi
        "$EMBCC" --target=$t -O0 -c tests/harness/thumb/boot.c -o "$h/boot.o" &&
        "$EMBCC" --target=$t -O0 -c tests/harness/thumb/io.c -o "$h/io.o" ;;
    rv32) t=riscv32-unknown-elf
        "$EMBCC" --target=$t -O0 -c tests/harness/riscv/boot.c -o "$h/boot.o" &&
        "$EMBCC" --target=$t -O0 -c tests/harness/riscv/io.c -o "$h/io.o" ;;
    avr) t=avr
        "$EMBCC" --target=$t -c tests/harness/avr/boot.S -o "$h/boot.o" &&
        "$EMBCC" --target=$t -Os -c tests/harness/avr/io.c -o "$h/io.o" &&
        sh tools/build-rt.sh avr "$h" > "$h/rt.log" 2>&1 ;;
    esac || { echo "FAIL $tag: the harness does not build"; return 1; }
    "$EMBCC" --target=$t -O0 -g -c "$src" -o "$h/cov.o" &&
    "$EMBCC" --target=$t -O0 -c "$src" -o "$h/cov-nog.o" || {
        echo "FAIL $tag: cov.c does not build"; return 1; }
    link "$tag" "$out/$tag.elf" "$h/cov.o" &&
    link "$tag" "$out/$tag-nog.elf" "$h/cov-nog.o" ||
        { echo "FAIL $tag: the image does not link"; return 1; }
}

# link TAG OUT.elf OBJ: in TAG's harness
link() {
    case $1 in
    m3) H=EMBCC_THUMB_HARNESS; l=thumb ;;
    rv32) H=EMBCC_RISCV_HARNESS; l=riscv ;;
    avr) H=EMBCC_AVR_HARNESS; l=avr ;;
    esac
    env "$H=$out/$1" sh tests/harness/$l/link.sh "$2" "$3" > /dev/null 2>&1
}

# verify TAG ELF EXPECT PATH FNS ARGS...: run ELF with --coverage, in
# both formats, and hold the reports to EXPECT ("LINE COUNT" lines);
# PATH is how the line table names cov.c; FNS the FN/FNDA lines it must
# have, sorted, as one line
verify() {
    tag=$1; e=$2; exp=$3; path=$4; fns=$5; shift 5
    "$EMBSIM" "$e" "$@" --max-insns 1000000 --count "$out/$tag.plain.count" \
        > "$out/$tag.plain.out" 2> /dev/null
    "$EMBSIM" "$e" "$@" --max-insns 1000000 --count "$out/$tag.count" \
        --coverage "$out/$tag.cov" > "$out/$tag.out" 2> "$out/$tag.err"
    "$EMBSIM" "$e" "$@" --max-insns 1000000 --coverage "$out/$tag.info" \
        --coverage-format=lcov > /dev/null 2>&1
    cmp -s "$out/$tag.plain.out" "$out/$tag.out" &&
    cmp -s "$out/$tag.plain.count" "$out/$tag.count" || {
        echo "FAIL $tag: --coverage changed the run (its output or --count)"; fail=1; }
    [ "$(tr -d ' ' < "$out/$tag.out")" = 2 ] || {
        echo "FAIL $tag: the program printed '$(cat "$out/$tag.out")', not 2"; fail=1; return; }

    # the text report: cov.c's section, each line "COUNT:LINE:source"
    awk -v f="$path" '
        index($0, f ": ") == 1 { on = 1; next }
        on && /^$/ { on = 0 }
        on && /^ *([0-9]+|#####|-): *[0-9]+:/ {
            split($0, p, ":"); c = p[1]; gsub(/ /, "", c); n = p[2] + 0
            if (c == "-") next
            print n, (c == "#####" ? 0 : c) }' "$out/$tag.cov" > "$out/$tag.got"
    if ! cmp -s "$exp" "$out/$tag.got"; then
        echo "FAIL $tag: the text report's lines are not cov.c's marks (expected, got):"
        diff "$exp" "$out/$tag.got" | head -10 | sed 's/^/     | /'
        fail=1
    fi
    grep -qF "$path: 14 of 17 lines (82.4%), 2 of 3 functions (66.7%)" "$out/$tag.cov" || {
        echo "FAIL $tag: the summary line of cov.c"; grep -F "$path:" "$out/$tag.cov"; fail=1; }

    # the lcov tracefile: cov.c's record
    awk -v f="$path" '
        /^SF:/ { on = substr($0, 4) == f; next }
        on && /^DA:/ { split(substr($0, 4), p, ","); print p[1], p[2] }' \
        "$out/$tag.info" > "$out/$tag.lcov-got"
    cmp -s "$exp" "$out/$tag.lcov-got" || {
        echo "FAIL $tag: lcov's DA lines are not cov.c's marks"
        diff "$exp" "$out/$tag.lcov-got" | head -10 | sed 's/^/     | /'
        fail=1; }
    rec=$(awk -v f="$path" '
        /^SF:/ { on = substr($0, 4) == f; next }
        on && /^(FN|FNDA|FNF|FNH|LF|LH):/ { print }' "$out/$tag.info" | sort | tr '\n' ' ')
    [ "$rec" = "$fns" ] || { echo "FAIL $tag: lcov's function and line totals:"
        echo "     | got  $rec"; echo "     | want $fns"; fail=1; }
    # every record well formed: TN first, SF..end_of_record, and LF, LH,
    # FNF and FNH what its DA and FNDA lines add up to
    awk 'NR == 1 && $0 != "TN:" { bad = "no TN: first" }
        /^SF:/ { if (in_rec) bad = "SF inside a record"; in_rec = 1; da = dh = fa = fh = 0; next }
        /^DA:/ { split(substr($0, 4), p, ","); da++; dh += p[2] > 0; next }
        /^FNDA:/ { split(substr($0, 6), p, ","); fa++; fh += p[1] > 0; next }
        /^LF:/ { if (substr($0, 4) + 0 != da) bad = "LF is not the DA lines" }
        /^LH:/ { if (substr($0, 4) + 0 != dh) bad = "LH is not the DA lines hit" }
        /^FNF:/ { if (substr($0, 5) + 0 != fa) bad = "FNF is not the FNDA lines" }
        /^FNH:/ { if (substr($0, 5) + 0 != fh) bad = "FNH is not the FNDA lines hit" }
        /^end_of_record$/ { if (!in_rec) bad = "end_of_record outside a record"; in_rec = 0 }
        END { if (in_rec) bad = "a record not closed"; if (bad != "") { print bad; exit 1 } }' \
        "$out/$tag.info" > "$out/$tag.lcov-check" 2>&1 || {
        echo "FAIL $tag: the lcov tracefile is malformed: $(cat "$out/$tag.lcov-check")"; fail=1; }
}

fns_embcc="FN:17,never_called FN:22,classify FN:32,main FNDA:0,never_called FNDA:1,main FNDA:5,classify FNF:3 FNH:2 LF:17 LH:14 "
check() {
    tag=$1; shift
    f0=$fail
    build "$tag" || { fail=1; return; }
    verify "$tag" "$out/$tag.elf" "$out/expect" "$src" "$fns_embcc" "$@"
    for fn in "never_called (line 17): 0" "classify (line 22): 5" "main (line 32): 1"; do
        grep -q "^  function $fn$" "$out/$tag.cov" || {
            echo "FAIL $tag: no '  function $fn' in the report"; fail=1; }
    done
    # no line table: the functions alone, from the symbol table
    "$EMBSIM" "$out/$tag-nog.elf" "$@" --max-insns 1000000 \
        --coverage "$out/$tag-nog.cov" > /dev/null 2> "$out/$tag-nog.err"
    grep -q 'has no line table' "$out/$tag-nog.err" &&
    grep -q '^  function never_called: 0$' "$out/$tag-nog.cov" &&
    grep -q '^  function classify: 5$' "$out/$tag-nog.cov" &&
    grep -q '^  function main: 1$' "$out/$tag-nog.cov" || {
        echo "FAIL $tag: without -g, the functions' counts (or the warning)"; fail=1; }
    [ $fail = "$f0" ] && echo "  $tag: cov.c's 17 lines and 3 functions as marked, text and lcov"
}

check m3 --board lm3s6965evb
check rv32 --board virt --ram-size 8M
check avr --board uno

if command -v "$CLANG" >/dev/null 2>&1 &&
   "$CLANG" --target=thumbv7m-none-eabi -mcpu=cortex-m3 -O0 -g -ffreestanding \
       -c "$src" -o "$out/clang.o" 2> /dev/null &&
   link m3 "$out/clang.elf" "$out/clang.o"; then
    f0=$fail
    verify clang "$out/clang.elf" "$out/expect-clang" "$PWD/$src" \
        "FN:18,never_called FN:23,classify FN:33,main FNDA:0,never_called FNDA:1,main FNDA:5,classify FNF:3 FNH:2 LF:17 LH:14 " \
        --board lm3s6965evb
    [ $fail = "$f0" ] && echo "  clang (DWARF 5, special opcodes): cov.c's lines and functions as marked"
else
    echo "  SKIP the clang build: $CLANG cannot build cov.c for thumbv7m"
fi

"$EMBSIM" "$out/m3.elf" --coverage "$out/x" --coverage-format=html > "$out/bad.out" 2>&1
[ $? = 2 ] && grep -q 'text or lcov' "$out/bad.out" || {
    echo "FAIL: --coverage-format=html is not refused"; fail=1; }
[ $fail = 0 ] && echo "embsim --coverage: every line and function of cov.c counted exactly, on the Cortex-M, RISC-V and AVR"
exit $fail
