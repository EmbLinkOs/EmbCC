#!/bin/sh
# EmbLD's orphan sections: a section no fixed group claims (the kernel's
# .embk_exports, a section("mytab") table) is gathered from every object
# CONTIGUOUSLY — not interleaved with other objects' .data — and its bounds
# are bracket symbols under both spellings: __NAME_start/__NAME_end for a
# dotted .NAME (what the EmbLinkOS kernel scripts define by hand), and GNU
# ld's __start_NAME/__stop_NAME for a C-identifier NAME.
#
# embld writes no symbol table, so the program records the brackets itself:
# `bounds[]` is initialized with their addresses (absolute relocations
# embld resolves), and this script reads them back out of the image, walks
# each table and checks every entry's name string — which is also a check
# that the relocations inside the orphan sections were applied.
set -u
echo "TEST-MARKER embld-sections"
. "$(dirname "$0")/../../lib.sh"

EMBLD="$(dirname "$EMBCC")/embld"
[ -x "$EMBLD" ] || { echo "embld not built"; exit 1; }

out=tests/golden/out/embld-sections
rm -rf "$out"; mkdir -p "$out"

cat > "$out/a.c" <<'EOF'
struct exp { const char *name; void *addr; };
extern const struct exp __embk_tab_start[], __embk_tab_end[];
extern const struct exp __start_mytab[], __stop_mytab[];
/* first in a.o's .data, so first in the data segment */
const void *bounds[4] = { __embk_tab_start, __embk_tab_end,
                          __start_mytab, __stop_mytab };
int f1(void) { return 1; }
const char banner[] = "some data before the table";
const struct exp ex_f1 __attribute__((used, section(".embk_tab"))) =
    { "f1", (void *)f1 };
const struct exp my_a __attribute__((section("mytab"))) = { "a", 0 };
long pad = 3;   /* a.o .data between its table entries */
const struct exp ex_banner __attribute__((section(".embk_tab"))) =
    { "banner", (void *)banner };
int _start(void) { return 0; }
EOF
cat > "$out/b.c" <<'EOF'
struct exp { const char *name; void *addr; };
char b_data[5] = "bbbb";   /* b.o .data: lands between a.o's and b.o's entries
                            * if embld interleaves by object */
int f2(void) { return 2; }
const struct exp ex_f2 __attribute__((section(".embk_tab"))) = { "f2", (void *)f2 };
const struct exp my_b __attribute__((section("mytab"))) = { "b", (void *)b_data };
EOF
for u in a b; do
    "$EMBCC" -c "$out/$u.c" -o "$out/$u.o" || { echo "embcc failed on $u.c"; exit 1; }
done
"$EMBLD" -e _start -o "$out/t.elf" "$out/a.o" "$out/b.o" ||
    { echo "embld failed"; exit 1; }

# vaddr -> file offset, from the PT_LOAD headers
$READELF -lW "$out/t.elf" | awk '$1 == "LOAD" { print $2, $3, $5 }' > "$out/loads"
foff() {
    while read -r off va fsz; do
        if [ $(($1)) -ge $((va)) ] && [ $(($1)) -lt $((va + fsz)) ]; then
            echo $(($1 - va + off)); return
        fi
    done < "$out/loads"
    echo -1
}
# the 8-byte little-endian value at a vaddr
q64() {
    o=$(foff "$1")
    [ "$o" -ge 0 ] || { echo "vaddr $1 is outside every PT_LOAD" >&2; echo 0; return; }
    printf '0x%s\n' "$(od -A n -t x8 -j "$o" -N 8 "$out/t.elf" | tr -d ' \n')"
}
# the C string at a vaddr
cstr() {
    o=$(foff "$1")
    [ "$o" -ge 0 ] || { echo "?"; return; }
    dd if="$out/t.elf" bs=1 skip="$o" count=32 2>/dev/null | tr '\0' '\n' | head -1
}

data_va=$($READELF -lW "$out/t.elf" | awk '$1 == "LOAD" && $7 ~ /W/ { print $3; exit }')
[ -n "$data_va" ] || { echo "no writable PT_LOAD"; exit 1; }
tab_start=$(q64 "$data_va");            tab_end=$(q64 $((data_va + 8)))
my_start=$(q64 $((data_va + 16)));      my_stop=$(q64 $((data_va + 24)))

fail=0
check_table() {   # label start end expected-names...
    label=$1 s=$2 e=$3; shift 3
    if [ $((s)) -eq 0 ] || [ $((e)) -eq 0 ]; then
        echo "FAIL $label: a bracket symbol resolved to 0"; fail=1; return
    fi
    if [ $((s % 8)) -ne 0 ]; then
        echo "FAIL $label: table starts misaligned at $s"; fail=1
    fi
    if [ $((e - s)) -ne $(($# * 16)) ]; then
        echo "FAIL $label: spans $((e - s)) bytes, want $(($# * 16)) ($# entries)"
        fail=1; return
    fi
    got=""
    a=$((s))
    while [ $a -lt $((e)) ]; do
        got="$got $(cstr "$(q64 $a)")"
        a=$((a + 16))
    done
    for n in "$@"; do
        case "$got " in *" $n "*) ;; *) echo "FAIL $label: entry '$n' missing (got:$got)"; fail=1 ;; esac
    done
    [ $fail -eq 0 ] && echo "$label: $# contiguous entries [$got ] between its brackets"
}
check_table ".embk_tab (__embk_tab_start/_end)" "$tab_start" "$tab_end" f1 banner f2
check_table "mytab (__start_mytab/__stop_mytab)" "$my_start" "$my_stop" a b

[ $fail -eq 0 ] || exit 1
echo "orphan sections grouped contiguously, bracketed under both spellings"
