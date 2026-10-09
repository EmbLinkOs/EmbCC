#!/bin/sh
# Every tool in `make all` must link.
#
# This exists because embas did not, for two commits and nobody noticed.
# src/elf/write.c started asking target_ptr_size() to choose between
# ELF32 and ELF64 when the RISC-V backend landed, and embas links
# write.c without src/arch/target.c -- so `make all` failed while the
# whole test suite stayed green. It stayed green because `make test`
# depends on embcc, embread, embld, embdbg and embls, and reaches the
# assembler only through the kernel build, which is opt-in.
#
# A tool that does not build is not a subtle failure, so this is not a
# subtle test: build each one into a private directory and say which.
set -u
echo "TEST-MARKER tools-build"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/tools-build
rm -rf "$out"; mkdir -p "$out"

# The names `all` builds. Read from the Makefile rather than listed
# here, so a tool added to `all` is covered without editing this file.
# the `all` target, its backslash-continued lines joined: it outgrew one
# line, and reading only the first gave a tool named `\` and lost the rest
tools=$(awk '/^all:/ { f = 1 }
             f { l = l " " $0; if ($0 !~ /\\$/) { print l; exit } }' \
            "$EMBCC_ROOT/Makefile" | sed 's/\\//g; s/^ *all: *//')
[ -n "$tools" ] || { echo "FAIL: could not read the `all` target"; exit 1; }

# The link commands come from the Makefile so there is one source of
# truth, but they must NOT be run as-is: `make embcc` writes ./embcc in
# the working tree, and this test runs CONCURRENTLY with every other
# golden test, all of which are using that binary. So the recipe is
# taken with `make -n` and its output path rewritten into $out. -B
# forces make to print the recipe even when the tool is up to date.
fail=0
for t in $tools; do
    ( cd "$EMBCC_ROOT" && make -Bn BUILD="$out/b" "$t" ) 2>/dev/null         | grep -v '^make' > "$out/$t.cmd"
    [ -s "$out/$t.cmd" ] || {
        printf 'FAIL %s: no recipe came back from make\n' "$t"; fail=1
        continue; }
    # -o <tool> becomes -o $out/<tool>; nothing else is touched.
    sed "s| -o $t | -o $out/$t |" "$out/$t.cmd" > "$out/$t.sh"
    grep -q " -o $out/$t " "$out/$t.sh" || {
        printf 'FAIL %s: could not redirect the output path\n' "$t"; fail=1
        continue; }
    if ( cd "$EMBCC_ROOT" && sh "$out/$t.sh" ) > "$out/$t.log" 2>&1; then
        printf '  %s links\n' "$t"
    else
        printf 'FAIL %s does not link:\n' "$t"
        grep -iE 'undefined|error' "$out/$t.log" | head -3 | sed 's/^/     | /'
        fail=1
    fi
done

# embsim is built optimized (EMBSIM_OPT, -O2): a simulator's speed is
# every test's that runs one, and without -O it ran a busy loop about
# three times slower. Still with -g, as every tool here is.
if [ -s "$out/embsim.cmd" ]; then
    grep -Eq -- ' -O[1-3s]? ' "$out/embsim.cmd" ||
        { echo "FAIL embsim: its recipe has no -O:"; cut -c1-100 "$out/embsim.cmd"; fail=1; }
    grep -q -- ' -g ' "$out/embsim.cmd" ||
        { echo "FAIL embsim: its recipe dropped -g"; fail=1; }
fi
# ...and rebuilt when the Makefile changes, or an ./embsim linked before
# -O2 was the default (or with another EMBSIM_OPT) is kept: the rule's
# prerequisites, from make's own database (-p), must name the Makefile
( cd "$EMBCC_ROOT" && make -pn embsim 2> /dev/null ) |
    awk '/^embsim:/ { for (i = 2; i <= NF; i++) if ($i == "Makefile") ok = 1 }
         END { exit !ok }' ||
    { echo "FAIL embsim: its rule does not depend on the Makefile, so EMBSIM_OPT changes leave ./embsim as it was"; fail=1; }

[ "$fail" -eq 0 ] || exit 1
echo "  every tool in \`make all\` builds ($(echo $tools | wc -w | tr -d ' ') of them)"
