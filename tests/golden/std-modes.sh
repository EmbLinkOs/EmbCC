#!/bin/sh
# -std= and -pedantic for C. Every case of tests/std/c-corpus.txt under
# every C standard, as it is and with -pedantic-errors, must be accepted
# or refused as GCC 16 decided (tests/std/c-expect.txt, recorded with
# tools/std-check.py record; the deliberate differences, with why, in
# c-expect.txt.differs). Then what an accept/refuse verdict cannot show:
# programs that only compile under one standard, run, and the
# __STDC_VERSION__ each standard defines.
#
# The front end is the same for every target, so this runs once, for
# x86_64-elf; under tests/run.sh --target=<other> it says so and passes.
set -u
echo "TEST-MARKER std-modes"
. "$(dirname "$0")/../lib.sh"
EMBCC=${EMBCC:-./embcc}
case $EMBCC in /*) ;; *) EMBCC=$PWD/$EMBCC ;; esac
if [ "$TARGET" != x86_64-elf ]; then
    echo "std-modes: the front end's; checked under x86_64-elf only"
    exit 0
fi
out=$PWD/tests/golden/out/std-modes
rm -rf "${out:?}"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }

# ---- the corpus, against GCC's verdicts ------------------------------------
if command -v python3 >/dev/null 2>&1; then
    EMBCC=$EMBCC STD_EMBCC_FLAGS=--target=x86_64-elf \
        python3 tools/std-check.py check tests/std/c-corpus.txt \
            tests/std/c-expect.txt > "$out/corpus.log" 2>&1 ||
        { cat "$out/corpus.log"; fail "EmbCC and GCC disagree on the C corpus"; }
    echo "std-modes: $(tail -1 "$out/corpus.log")"
else
    echo "std-modes: no python3, the corpus check skipped"
fi

# ---- __STDC_VERSION__ and __STRICT_ANSI__ ----------------------------------
for pair in c89:none c90:none gnu89:none iso9899:199409:199409L c99:199901L \
            gnu99:199901L c11:201112L c17:201710L gnu17:201710L \
            c23:202311L gnu23:202311L c2y:202500L; do
    mode=${pair%:*}; want=${pair##*:}
    [ "$want" = none ] && want=__STDC_VERSION__
    got=$(printf '__STDC_VERSION__\n' | "$EMBCC" -std="$mode" -E -x c - |
          grep -v '^#' | tr -d ' \n')
    [ "$got" = "$want" ] || fail "-std=$mode: __STDC_VERSION__ is '$got', not $want"
    ansi=$(printf '__STRICT_ANSI__\n' | "$EMBCC" -std="$mode" -E -x c - |
           grep -v '^#' | tr -d ' \n')
    case $mode in
        gnu*) [ "$ansi" = __STRICT_ANSI__ ] || fail "-std=$mode defines __STRICT_ANSI__" ;;
        *)    [ "$ansi" = 1 ] || fail "-std=$mode does not define __STRICT_ANSI__" ;;
    esac
done
echo "std-modes: __STDC_VERSION__ and __STRICT_ANSI__ under each standard"

# ---- programs, run ---------------------------------------------------------
# run NAME MODE EXIT: compile $out/NAME.c under -std=MODE, run, expect EXIT
run() {
    "$EMBCC" --target=x86_64-elf -std="$2" -c "$out/$1.c" -o "$out/$1.o" \
        2> "$out/$1.err" || { cat "$out/$1.err"; fail "$1 does not compile under -std=$2"; }
    t_link "$out/$1" "$out/$1.o" || fail "$1 does not link"
    t_run "$out/$1"
    got=$?
    [ "$got" -eq "$3" ] || fail "$1 under -std=$2 exits $got, not $3"
}

# C89: implicit int (a storage class or `auto`, a function with no
# type), and a call to a function nothing declared, defined later
cat > "$out/c89.c" <<'EOF'
static x = 1;
const c = 2;
main()
{
    auto w = 4.5;           /* auto is the storage class: w is an int, 4 */
    register i;
    static s;
    for (i = 0; i < 3; i++)
        s += i;
    return x + c + w + s + twice(5) - 16;   /* 1 + 2 + 4 + 3 + 10 - 16 */
}
twice(n) { return 2 * n; }
EOF
run c89 c89 4

# trigraphs, a strict standard's before C23: `??/` at a line's end
# continues the line, inside a string too
cat > "$out/tri.c" <<'EOF'
??=define TEN 10
int main(void)
??<
    const char *s = "a??/
b";
    int a??(2??) = ??< 1, 2 ??>;
    return TEN + (s??(1??) == 'b') + a??(1??) + (??-0 == -1);   /* 14 */
??>
EOF
run tri c99 14
# ...and under GNU's dialects, three characters: `??=` is not `#`
"$EMBCC" --target=x86_64-elf -std=gnu99 -fsyntax-only "$out/tri.c" 2> /dev/null &&
    fail "-std=gnu99 read trigraphs"

# C89 strict: `//` is two slashes
printf '// c\nint x;\n' > "$out/slash.c"
"$EMBCC" --target=x86_64-elf -std=c89 -fsyntax-only "$out/slash.c" 2> /dev/null &&
    fail "-std=c89 read // as a comment"
"$EMBCC" --target=x86_64-elf -std=gnu89 -fsyntax-only "$out/slash.c" ||
    fail "-std=gnu89 did not read // as a comment"
echo "std-modes: C89's implicit int and declarations, trigraphs and // run as each standard says"

# ---- -pedantic -------------------------------------------------------------
# a warning naming the standard, an error under -pedantic-errors, and
# nothing for what __extension__ marks or a system header holds
printf 'int x = 0b101;\nint y = __extension__ 0b11;\n' > "$out/ped.c"
"$EMBCC" --target=x86_64-elf -std=c17 -pedantic -fsyntax-only "$out/ped.c" 2> "$out/ped.err" ||
    fail "-pedantic made an error"
grep -q "ped.c:1:9: warning: a binary constant is new in C23, and the standard here is C17 \[--warn=pedantic\]" "$out/ped.err" ||
    { cat "$out/ped.err"; fail "-pedantic did not report the binary constant"; }
[ "$(grep -c 'warning:' "$out/ped.err")" = 1 ] ||
    { cat "$out/ped.err"; fail "-pedantic reported what __extension__ marks"; }
"$EMBCC" --target=x86_64-elf -std=c17 -pedantic-errors -fsyntax-only "$out/ped.c" 2> /dev/null &&
    fail "-pedantic-errors did not refuse a binary constant under C17"
"$EMBCC" --target=x86_64-elf -std=c23 -pedantic-errors -fsyntax-only "$out/ped.c" ||
    fail "-pedantic-errors refused a binary constant under C23"
mkdir -p "$out/sys"
printf 'struct empty { };\n' > "$out/sys/h.h"
printf '#include <h.h>\nint x;\n' > "$out/sysuse.c"
"$EMBCC" --target=x86_64-elf -std=c17 -pedantic-errors -isystem "$out/sys" \
    -fsyntax-only "$out/sysuse.c" || fail "-pedantic-errors reported a system header"
# ...nor about what a system header's MACRO expands to in the program:
# <tgmath.h>'s sqrt is a _Generic and <stdbool.h>'s bool is _Bool, neither
# C89's. The preprocessor marks that text for the compiler only, so a
# system macro stringified (#) is still its spelling: X(INT_MAX) runs.
printf '#include <tgmath.h>\n#include <stdbool.h>\nbool f(double x) { return sqrt(x) > 1; }\n' > "$out/sysmac.c"
"$EMBCC" --target=x86_64-elf -std=c89 -pedantic-errors -fsyntax-only "$out/sysmac.c" ||
    fail "-pedantic-errors reported what a system header's macro expands to"
cat > "$out/sysstr.c" <<'EOF'
#include <limits.h>
#define S(x) #x
#define X(x) S(x)
static const char s[] = X(INT_MAX);
int main(void) { return sizeof s == 11 && s[0] == '0' && s[1] == 'x' ? 42 : 1; }
EOF
run sysstr c17 42
echo "std-modes: -pedantic warns, -pedantic-errors refuses, __extension__ and system headers and their macros are quiet"
