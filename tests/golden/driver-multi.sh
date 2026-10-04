#!/bin/sh
# Several sources in one command -- `embcc a.c b.c -o prog`, `embcc -c
# a.c b.c`, `-j N` -- which is how every Makefile and CMake build calls a
# compiler. The compiler's state is per process, so the driver runs
# itself once per source (src/platform/platform.h: on a host that can
# run a program) and links the objects in command-line order.
#
# What would go wrong is checked by what it would change: the image is
# RUN on QEMU, and must be byte-identical to the one built a file at a
# time, at -j1 and at -j3; the link order is the command line's; a
# source that does not compile leaves no image and no temporaries.
set -u
echo "TEST-MARKER driver-multi"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
E=$(cd "$(dirname "$EMBCC")" && pwd)/$(basename "$EMBCC")
NM=${EMBCC_LLVM_NM:-llvm-nm}
QARM=${EMBCC_QEMU_ARM:-qemu-system-arm}
L=tests/golden/ldscript
H=tests/harness/thumb
out=tests/golden/out/driver-multi
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
T=--target=thumbv7em-none-eabi
want='hello from flash\n42 41 0 1 2 42 1 1 \n43 done\n'
run_m3() {
    command -v "$QARM" >/dev/null 2>&1 || return 0
    sh tests/harness/qrun.sh 10 --until done "$QARM" -M lm3s6965evb \
        -cpu cortex-m3 -nographic -kernel "$1" > "$1.txt" 2>/dev/null
    tr -d '\r' < "$1.txt" | head -3 > "$1.got"
    printf "$want" | cmp -s - "$1.got" || { cat "$1.txt"; fail "$1 did not run"; }
}
tmps() { ls "$out" | grep -c 'embcc-tmp' ; }

# 1. three sources, one command, linked and run
"$EMBCC" $T -O2 -T "$L/stm32.ld" "$L/startup.c" "$L/prog.c" "$H/io.c" \
    -o "$out/one.elf" || fail "three sources in one command"
run_m3 "$out/one.elf"
[ "$(tmps)" = 0 ] || fail "temporary objects were left behind"
# ...the same image as a file at a time
for f in "$L/startup.c" "$L/prog.c" "$H/io.c"; do
    b=$(basename "$f" .c)
    "$EMBCC" $T -O2 -c "$f" -o "$out/$b.o" || fail "$f"
done
"$EMBCC" $T -T "$L/stm32.ld" "$out/startup.o" "$out/prog.o" "$out/io.o" \
    -o "$out/sep.elf" || fail "the objects"
cmp -s "$out/one.elf" "$out/sep.elf" ||
    fail "one command and one command per file built different images"
# ...and at -j3, three at once
"$EMBCC" $T -O2 -j3 -T "$L/stm32.ld" "$L/startup.c" "$L/prog.c" "$H/io.c" \
    -o "$out/j3.elf" || fail "-j3"
cmp -s "$out/one.elf" "$out/j3.elf" || fail "-j3 built a different image"
"$EMBCC" $T -O2 -j -T "$L/stm32.ld" "$L/startup.c" "$L/prog.c" "$H/io.c" \
    -o "$out/jn.elf" || fail "-j"
cmp -s "$out/one.elf" "$out/jn.elf" || fail "-j built a different image"
echo "three sources in one command: runs, and is the image built a file at a time, at -j1, -j3 and -j"

# 2. command-line order, objects and sources interleaved
"$EMBCC" $T -O2 -T "$L/stm32.ld" "$L/prog.c" "$out/startup.o" "$H/io.c" \
    -o "$out/order.elf" || fail "sources among objects"
addr() { "$NM" "$out/order.elf" | awk -v s="$1" '$3 == s { print $1 }'; }
m=$(addr main); r=$(addr Reset_Handler); p=$(addr puts_)
[ $((0x$m)) -lt $((0x$r)) ] && [ $((0x$r)) -lt $((0x$p)) ] ||
    fail "not linked in command-line order: main $m, Reset_Handler $r, puts_ $p"
run_m3 "$out/order.elf"
echo "sources among objects: linked in command-line order"

# 3. -c with several sources: NAME.o for each, in the working directory
mkdir -p "$out/c"
(cd "$out/c" && "$E" $T -O2 -c ../../../ldscript/prog.c ../../../../harness/thumb/io.c) ||
    fail "-c with two sources"
[ -f "$out/c/prog.o" ] && [ -f "$out/c/io.o" ] || { ls "$out/c"; fail "-c did not write prog.o and io.o"; }
(cd "$out/c" && "$E" $T -O2 -c ../../../ldscript/prog.c -o prog1.o)
cmp -s "$out/c/prog.o" "$out/c/prog1.o" || fail "-c a.c b.c made a different prog.o"
(cd "$out/c" && "$E" $T -O2 -S ../../../ldscript/prog.c ../../../../harness/thumb/io.c) ||
    fail "-S with two sources"
[ -f "$out/c/prog.s" ] && [ -f "$out/c/io.s" ] || fail "-S did not write prog.s and io.s"
printf '#define ONE first\nONE\n' > "$out/c/e1.c"
printf '#define TWO second\nTWO\n' > "$out/c/e2.c"
"$EMBCC" -E "$out/c/e1.c" "$out/c/e2.c" > "$out/c/e.txt" || fail "-E with two sources"
grep -v '^#' "$out/c/e.txt" | grep -v '^$' | tr '\n' ' ' | grep -qx 'first second ' ||
    { cat "$out/c/e.txt"; fail "-E did not print both, in order"; }
echo "-c, -S and -E with several sources: one output each, as one at a time"

# 4. refused, and failed
if "$EMBCC" $T -c "$L/prog.c" "$H/io.c" -o "$out/x.o" 2> "$out/r1.txt"; then
    fail "-c -o with two sources should be refused"
fi
grep -q -- '-o names one output' "$out/r1.txt" || { cat "$out/r1.txt"; fail "its message"; }
printf 'int f(void) { return undeclared_thing; }\n' > "$out/bad.c"
rm -f "$out/bad.elf"
if "$EMBCC" $T -O2 -j3 -T "$L/stm32.ld" "$L/startup.c" "$out/bad.c" "$L/prog.c" \
        "$H/io.c" -o "$out/bad.elf" 2> "$out/r2.txt"; then
    fail "a source that does not compile should fail the command"
fi
grep -q 'bad.c:1:.*undeclared_thing' "$out/r2.txt" || { cat "$out/r2.txt"; fail "the error is not bad.c's"; }
# ...and nothing is linked after it: no word from the linker
if grep -q 'embld\|embcc-tmp' "$out/r2.txt"; then
    cat "$out/r2.txt"; fail "the link was attempted although a source failed"
fi
[ ! -e "$out/bad.elf" ] || fail "an image was written although a source failed"
[ "$(tmps)" = 0 ] || { ls "$out"; fail "temporary objects were left behind after a failure"; }
echo "-o with -c and two sources refused; a failing source fails the command and leaves nothing"

# 5. EmbLinkOS cannot run a program: its build is PROCESS=none, and its
#    manifest says so
make -pn PROCESS=none 2>/dev/null | sed -n 's/^SRCS := //p' | head -1 |
    grep -q 'process_none\.c' || fail "PROCESS=none does not select process_none.c"
grep -q 'make -pn PROCESS=none' tools/gen-embbuild-manifest.sh ||
    fail "the EmbLinkOS manifest is not generated with PROCESS=none"
grep -q 'process_none' build.ebm || fail "build.ebm does not build process_none.c"
echo "a host that cannot run a program builds process_none.c (EmbLinkOS's manifest does)"
