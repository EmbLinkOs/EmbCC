#!/bin/sh
# Attributes whose effect is in the OBJECT, not in what a program
# computes.
#
# `used` keeps a symbol the compiler would otherwise drop, and
# `visibility` changes how the linker may treat one. Neither changes any
# answer, so no exec test can see them -- what sees them is the symbol
# table, read by something that is not us.
#
# The rest of the attribute story is elsewhere, and deliberately:
# tests/compile/reject-unimplemented.sh has the ones that must REFUSE
# (naked, interrupt, cleanup, ms_abi, sysv_abi, and an unknown one),
# tests/exec/attrs.c has the ones a running program can show
# (always_inline, noinline, unused), and tests/exec/ctors.c has
# constructor. Every attribute EmbCC knows has a stated disposition in
# src/parse/parse.c, which is the point: the failure this all exists to
# prevent is an attribute nobody decided about.
set -eu
echo "TEST-MARKER attrs"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/attrs-$ARCH
rm -rf "$out"; mkdir -p "$out"

case "$ARCH" in
    x86_64)  NM=x86_64-elf-nm;  RE=x86_64-elf-readelf ;;
    aarch64) NM=aarch64-elf-nm; RE=aarch64-elf-readelf ;;
    *) echo "skipped: no binutils for $ARCH"; exit 0 ;;
esac
command -v "$RE" > /dev/null 2>&1 || { echo "skipped: no $RE"; exit 0; }

cat > "$out/a.c" << 'EOF'
/* Nothing in this file calls kept(), and it is static -- so without the
 * attribute the compiler drops it and the symbol never appears. A
 * handler reached only through a table, or only from assembly, is the
 * real case: it would link and then do nothing. */
__attribute__((used)) static void kept(void) { }

/* A second static that is NOT marked: it must not survive, or the test
 * above proves only that nothing is ever dropped. */
static void dropped(void) { }

__attribute__((visibility("hidden")))    int h(void) { return 1; }
__attribute__((visibility("protected"))) int p(void) { return 2; }
__attribute__((visibility("internal")))  int i(void) { return 3; }
int plain(void) { return 4; }

__attribute__((visibility("hidden"))) int hv = 5;
int pv = 6;
EOF
"$EMBCC" --target="$TARGET" -c "$out/a.c" -o "$out/a.o" 2> "$out/cc.log" || {
    echo "FAIL: did not compile:"; cat "$out/cc.log"; exit 1; }

"$RE" -sW "$out/a.o" > "$out/syms.txt" 2>&1

# ---- used ----------------------------------------------------------------
grep -qE ' kept$' "$out/syms.txt" || {
    echo "FAIL: __attribute__((used)) did not keep 'kept':"
    cat "$out/syms.txt"; exit 1; }
! grep -qE ' dropped$' "$out/syms.txt" || {
    echo "FAIL: an unmarked unused static survived, so the 'used' check"
    echo "      above proves nothing:"; cat "$out/syms.txt"; exit 1; }
echo "used: a static nothing calls is kept, and an unmarked one is not"

# ---- visibility ----------------------------------------------------------
want_vis() {                      # want_vis SYMBOL VISIBILITY
    grep -qE "$2 +[0-9]+ $1\$" "$out/syms.txt" || {
        echo "FAIL: '$1' is not $2:"
        grep -E " $1\$" "$out/syms.txt" || cat "$out/syms.txt"
        exit 1; }
}
want_vis h     HIDDEN
want_vis p     PROTECTED
want_vis i     INTERNAL
want_vis plain DEFAULT
want_vis hv    HIDDEN
want_vis pv    DEFAULT
echo "visibility: hidden, protected, internal and default all reach
st_other, on functions and on data, and an unmarked symbol stays default"

# ---- the diagnostics -----------------------------------------------------
#
# deprecated and warn_unused_result exist to say something at the point
# of USE, so what is checked is that they fire there, that they are
# silent where the use is correct, and that each names the option that
# controls it -- a warning nobody can turn off is one people turn all of
# them off to escape.
cat > "$out/d.c" << 'EOF'
__attribute__((deprecated)) int old(void);
__attribute__((warn_unused_result)) int must(void);
int fine(void);
int main(void)
{
    old();                 /* deprecated; not warn_unused_result */
    must();                /* the result is the point: 1 warning */
    (void)must();          /* said on purpose: silent */
    int x = must();        /* used: silent */
    return fine() + x;     /* neither: silent */
}
EOF
"$EMBCC" --target="$TARGET" -c "$out/d.c" -o /dev/null > "$out/d.log" 2>&1 || {
    echo "FAIL: the diagnostics file did not compile:"
    cat "$out/d.log"; exit 1; }
n=$(grep -c 'deprecated-declarations' "$out/d.log" || true)
[ "$n" = 1 ] || { echo "FAIL: expected 1 deprecated warning, got $n:"
                  cat "$out/d.log"; exit 1; }
n=$(grep -c 'unused-result' "$out/d.log" || true)
[ "$n" = 1 ] || { echo "FAIL: expected 1 unused-result warning, got $n:"
                  cat "$out/d.log"; exit 1; }

# And each can be turned off by the name it printed.
"$EMBCC" --target="$TARGET" -c "$out/d.c" -o /dev/null \
    -Wno-deprecated-declarations -Wno-unused-result > "$out/d2.log" 2>&1
[ ! -s "$out/d2.log" ] || {
    echo "FAIL: -Wno- did not silence them:"; cat "$out/d2.log"; exit 1; }
echo "deprecated and warn_unused_result fire at the use, stay silent
where the use is correct, and each is switched off by the name it prints"
