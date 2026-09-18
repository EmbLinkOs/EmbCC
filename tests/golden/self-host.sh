#!/bin/sh
# M3 self-hosting, host half: EmbCC compiles ALL of its own sources, and
# EmbLD links them — our compiler and our linker, end to end — into a
# well-formed, fully-resolved EmbLinkOS executable (embcc-stage1). Plus
# the property the byte-identical fixed point rests on: codegen is
# deterministic.
#
# The stage1==stage2 byte-identity check itself is an ON-OS step: stage1
# is linked against newlib for the EmbLinkOS syscall ABI, so it runs on
# the OS, not this host (glibc). The OS is the final judge (DECISIONS
# D-005), exactly as M1 and M2 were confirmed there.
#
# Skips honestly when the OS runtime / newlib are not on this machine.
set -u
echo "TEST-MARKER self-host"
. "$(dirname "$0")/../lib.sh"

EMBCC=${EMBCC:-./embcc}
EMBLD=./embld

NEWLIB_INC=$X86_NEWLIB/include   # host layout: tools/hostpaths.sh via tests/lib.sh
CRT0=$MYOS_BUILD/crt0.o
SYSCALLS=$MYOS_BUILD/syscalls.o
LIBC=$X86_NEWLIB/lib/libc.a

for f in "$NEWLIB_INC/stdio.h" "$CRT0" "$SYSCALLS" "$LIBC"; do
    [ -e "$f" ] || { echo "skipped: $f not present on this host"; exit 0; }
done

# EmbCC's own source list, taken from the Makefile rather than restated: a
# hand-kept copy here went stale the moment src/cpp/predef.c was split per
# target, and nothing noticed because the test was skipping.
SRCS=$(make -pn 2>/dev/null | sed -n 's/^SRCS := //p' | head -1)
[ -n "$SRCS" ] || { echo "could not read SRCS from the Makefile"; exit 1; }
INCS="-I include -I $NEWLIB_INC"

out=tests/golden/out/self-host
rm -rf "$out"; mkdir -p "$out"

# 1. EmbCC compiles all of EmbCC. A single failure sinks self-hosting.
n=0
for f in $SRCS; do
    o="$out/$(echo "$f" | tr / _).o"
    "$EMBCC" -c "$f" $INCS -o "$o" || {
        echo "EmbCC failed to compile its own source: $f"; exit 1; }
    n=$((n + 1))
done
echo "EmbCC compiled all $n of its own sources"

# 2. Codegen is deterministic — the fixed point is impossible otherwise.
for f in $SRCS; do
    o="$out/$(echo "$f" | tr / _).o"
    "$EMBCC" -c "$f" $INCS -o "$out/twice.o"
    cmp -s "$o" "$out/twice.o" || {
        echo "nondeterministic codegen for $f — no fixed point can exist"
        exit 1; }
done
echo "codegen is deterministic (every object byte-identical across runs)"

# 3. EmbLD links the whole compiler against the real runtime.
"$EMBLD" -o "$out/embcc-stage1.elf" "$CRT0" "$SYSCALLS" \
    $out/src_*.o "$LIBC" || {
    echo "EmbLD failed to link EmbCC-compiled EmbCC"; exit 1; }
sz=$(wc -c < "$out/embcc-stage1.elf" | tr -d " ")
echo "EmbLD linked embcc-stage1.elf ($sz bytes)"

# 4. Structural acceptance: what the EmbLinkOS loader binds.
readelf -h "$out/embcc-stage1.elf" | grep -q "EXEC (Executable file)" || {
    echo "stage1 is not ET_EXEC"; exit 1; }
und=$(readelf -sW "$out/embcc-stage1.elf" 2>/dev/null \
      | awk '$7=="UND" && $8!="" {print $8}' | grep -v '^$')
[ -z "$und" ] || { echo "stage1 has unresolved symbols:"; echo "$und"; exit 1; }
echo "stage1 is ET_EXEC with every symbol resolved"

echo "self-host host acceptance passed (stage1==stage2 fixed point: on-OS)"
