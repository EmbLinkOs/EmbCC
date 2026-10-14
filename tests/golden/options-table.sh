#!/bin/sh
# The option table (src/driver/options.def) is the one description of
# EmbCC's command line: --help, --help=gcc and the generated option
# reference are printed from it. This holds the driver to it:
#
#   1. every spelling the driver's argument loop tests (each strcmp and
#      strncmp of an argument in main.c's main) is in the table -- as
#      EmbCC's spelling, as the other spelling of one, or as a GCC-only
#      one -- so no option exists that the documentation does not name.
#      A family counts only where the table says so (a trailing '*', or a
#      value after '='): -f<pass> does not excuse every -f flag;
#   2. every EmbCC spelling that is rewritten into another is accepted
#      (no "unknown argument") -- the rewrite templates are well formed;
#   3. EmbCC's spellings and GCC's build the same thing: one source with
#      a header and an enum, at -Os for ARMv6-M, with sections, short
#      enums, warnings and a dependency file -- the objects, the .d files
#      and the diagnostics are identical, and so with the value given as
#      the next argument (--cpu cortex-m4);
#   4. the reference (--help=markdown) has a row for every EmbCC spelling.
set -u
echo "TEST-MARKER options-table"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
ROOT=${EMBCC_ROOT:-.}
out=tests/golden/out/options-table
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

"$EMBCC" --help=spellings > "$out/table.txt" || fail "--help=spellings"
[ -s "$out/table.txt" ] || fail "the table is empty"

# ---- 1. the driver names nothing the table does not -----------------------
# main()'s text with adjacent string literals joined ("-Werror-implicit-"
# "function-declaration" is one spelling), then each literal that a
# strcmp/strncmp of argv[i], a or arg compares against, with its kind.
awk '/^int main\(/ { on = 1 } on { printf "%s ", $0 }' "$ROOT/src/driver/main.c" |
    sed 's/"[[:space:]]*"//g' |
    grep -oE 'str(n?)cmp\((argv\[i\]|a|arg)[[:space:]]*(\+[[:space:]]*[0-9]+)?[[:space:]]*,[[:space:]]*"-[^"]*"' |
    sed -E 's/^str(n?)cmp.*"(-[^"]*)"$/\1 \2/' |
    awk '{ print ($1 == "n" ? "prefix" : "exact"), ($1 == "n" ? $2 : $1) }' |
    sort -u > "$out/driver.txt"
n=$(wc -l < "$out/driver.txt" | tr -d ' ')
[ "$n" -ge 150 ] || { cat "$out/driver.txt"; fail "only $n spellings found in main(): the extraction is broken"; }
awk '
    NR == FNR {
        s = $2
        if (s ~ /[*%]$/) {
            f = substr(s, 1, length(s) - 1)
            if (f != "-f" && f != "-fno-" && f != "-W" && f != "-Wno-") fam[f] = 1
        } else {
            ex[s] = 1
            if (s ~ /=$/) fam[s] = 1
        }
        next
    }
    {
        kind = $1; s = $2
        if (s == "-" || s in ex) next
        if (s == "-D" || s == "-U" || s == "-I" || s == "-O" || s == "-l" ||
            s == "-L" || s == "-j" || s == "-x" || s == "-o") next
        for (f in fam) if (index(s, f) == 1) next
        if (kind == "prefix") {
            if (s in fam) next
            for (e in ex) if (index(e, s) == 1) next
        }
        print "  " kind " " s
        bad++
    }
    END { exit bad > 0 }' "$out/table.txt" "$out/driver.txt" > "$out/missing.txt" ||
    { cat "$out/missing.txt"
      fail "the driver accepts spellings src/driver/options.def does not name"; }
echo "options-table: all $n spellings the driver tests are in the table"

# ---- 2. every rewritten EmbCC spelling is accepted ------------------------
printf 'int x;\n' > "$out/t.c"
: > "$out/unknown.txt"
"$EMBCC" --help=markdown > "$out/ref.md" || fail "--help=markdown"
# (in $out: --deps=1 writes a dependency file named 1)
E=$EMBCC; case $E in /*) ;; *) E=$PWD/$E ;; esac
awk '$1 == "ours" { o = $2; getline; if ($1 == "alias") print o }' "$out/table.txt" |
(cd "$out" && while read -r s; do
    a=$s
    case $s in                  # a value each accepts
    --keep-temps=) a="${s}obj" ;;
    --pass=|--no-pass=) a="${s}licm" ;;
    --fp-contract=) a="${s}off" ;;
    *=) a="${s}1" ;;
    esac
    "$E" --target=x86_64-elf --syntax-only "$a" t.c > one.txt 2>&1
    grep -q "unknown argument" one.txt && echo "$a" >> unknown.txt
done)
[ ! -s "$out/unknown.txt" ] ||
    { cat "$out/unknown.txt"; fail "EmbCC spellings the driver does not accept"; }
echo "options-table: every EmbCC spelling that stands for another is accepted"

# ---- 3. the same build either way ------------------------------------------
cd "$out" || fail "cd"
printf '#include "h.h"\nenum e { A, B };\nint f(enum e x) { return x == A ? (int)sizeof(enum e) : g(); }\nint k;\nint u(int y) { int z; if (y) z = 1; return z; }\n' > s.c
printf 'int g(void);\n' > h.h
"$EMBCC" --target=thumbv6m-none-eabi -Os -fshort-enums -ffunction-sections \
    -fdata-sections -Wall -fno-common -MMD -MF g.d -c s.c -o g.o 2> g.err ||
    fail "the GCC spellings do not build"
"$EMBCC" --target=thumbv6m-none-eabi --opt=size --enums=short \
    --sections=functions,data --warn=all --no-common --deps=n.d -c s.c -o n.o 2> n.err ||
    fail "EmbCC's spellings do not build"
cmp -s g.o n.o || fail "--opt=size --enums=short --sections=functions,data --no-common build another object than GCC's spellings"
sed 's/g\.o/X/' g.d > g2.d; sed 's/n\.o/X/' n.d > n2.d
cmp -s g2.d n2.d || fail "--deps=FILE writes another rule than -MMD -MF FILE"
[ -s n.err ] || fail "--warn=all gave no warning for the uninitialized z"
cmp -s g.err n.err || fail "--warn=all warns differently from -Wall"
"$EMBCC" --target=thumbv7em-none-eabihf --cpu cortex-m4 --fpu=fpv4-sp-d16 \
    --float-abi=hard --opt=speed -c s.c -o n2.o 2>/dev/null &&
"$EMBCC" --target=thumbv7em-none-eabihf -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 \
    -mfloat-abi=hard -O2 -c s.c -o g2.o 2>/dev/null || fail "the Cortex-M4F builds"
cmp -s g2.o n2.o || fail "--cpu cortex-m4 --fpu= --float-abi= --opt=speed build another object"
cd - > /dev/null || fail "cd back"
echo "options-table: EmbCC's spellings and GCC's build identical objects, rules and diagnostics"

# ---- 4. the reference names every EmbCC spelling ---------------------------
awk '$1 == "ours" { print $2 }' "$out/table.txt" | while read -r s; do
    grep -qF "| \`$s" "$out/ref.md" || echo "$s"
done > "$out/unlisted.txt"
[ ! -s "$out/unlisted.txt" ] ||
    { cat "$out/unlisted.txt"; fail "spellings missing from --help=markdown"; }
echo "options-table: the reference lists every EmbCC spelling"
