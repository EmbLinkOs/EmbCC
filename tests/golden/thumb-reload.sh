#!/bin/sh
# Values left in memory on ARMv7-M, run on the board under register pools
# of every size: tests/exec/spill-reload.c, whose functions keep more
# values live across calls than there are registers.
#
# Two things in the backend act only on those values. The RELOAD CACHE
# (rc_try in src/arch/thumb/codegen.c) keeps a spilled value in a free low
# register for a stretch of reads -- across labels whose every jump is in
# the stretch, and out to a loop's back edge -- and keeps that register
# from every scratch and role meanwhile. REMATERIALIZATION (t_remat_ok)
# makes a spilled constant again at each read instead of storing it and
# loading it back. Both are wrong in ways only a run shows: a register
# taken from under the cache, a stretch entered from outside, a loop's
# next trip reading what the body left. The default pool barely spills
# here, so the run is repeated with EMBCC_RA_MAXPOOL shrinking it; and the
# test first checks that each of the two still changes this program's
# code, so it does not go on passing after they stop acting on it.
set -u
echo "TEST-MARKER thumb-reload"
. "$(dirname "$0")/../lib.sh"

QEMU=${EMBCC_QEMU_ARM:-qemu-system-arm}
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "SKIP: $QEMU not found (set EMBCC_QEMU_ARM)"; exit 0; }

T=thumbv7m-none-eabi
H=tests/harness/thumb
src=tests/exec/spill-reload.c
out=tests/golden/out/thumb-reload
rm -rf "$out"; mkdir -p "$out"

export EMBCC_THUMB_HARNESS="$PWD/$out"
for f in boot io; do
    "$EMBCC" --target=$T -c "$H/$f.c" -o "$out/$f.o" || {
        echo "the harness does not compile for $T"; exit 1; }
done
cat > "$out/drv.c" <<'EOF'
extern void puts_(const char *s);
extern void putn(long v);
int prog_main(void);
int main(void)
{
    puts_("result ");
    putn(prog_main());
    puts_("\n==END==\n");
    return 0;
}
EOF
"$EMBCC" --target=$T -O1 -c "$out/drv.c" -o "$out/drv.o" || {
    echo "the driver does not compile"; exit 1; }

# Each of the two acts on this program at a small pool.
for knob in EMBCC_T_NORC EMBCC_T_NOREMAT; do
    EMBCC_RA_MAXPOOL=4 "$EMBCC" --target=$T -Os -Dmain=prog_main -c "$src" \
        -o "$out/on.o" || { echo "$src does not compile"; exit 1; }
    env "$knob=1" EMBCC_RA_MAXPOOL=4 "$EMBCC" --target=$T -Os \
        -Dmain=prog_main -c "$src" -o "$out/off.o" || exit 1
    if cmp -s "$out/on.o" "$out/off.o"; then
        echo "$knob=1 changes nothing in $src at a pool of 4: the test no"
        echo "longer reaches what it is for"
        exit 1
    fi
done

n=0
for pool in default 3 4 5 6 8; do
    for opt in -Os -O2; do
        tag=p$pool$opt
        if [ $pool = default ]; then
            "$EMBCC" --target=$T $opt -Dmain=prog_main -c "$src" \
                -o "$out/$tag.o"
        else
            EMBCC_RA_MAXPOOL=$pool "$EMBCC" --target=$T $opt \
                -Dmain=prog_main -c "$src" -o "$out/$tag.o"
        fi || { echo "$tag: $src does not compile"; exit 1; }
        sh "$H/link.sh" "$out/$tag.elf" "$out/drv.o" "$out/$tag.o" || {
            echo "$tag: embld could not link the image"; exit 1; }
        sh "$H/run.sh" "$out/$tag.elf" > "$out/$tag.txt" 2>&1
        if ! grep -Eq '^result 0[^0-9]*$' "$out/$tag.txt" ||
           ! grep -q '==END==' "$out/$tag.txt"; then
            echo "$tag: spill-reload.c did not come out 0:"
            sed -n '1,5p' "$out/$tag.txt"
            exit 1
        fi
        n=$((n + 1))
    done
done
echo "thumb-reload: spill-reload.c right in $n builds (pools default, 3-8; -Os, -O2), and both the reload cache and rematerialization act on it"
