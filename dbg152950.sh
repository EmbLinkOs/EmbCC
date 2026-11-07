#!/bin/sh
# TEMPORARY: nightly fuzz seed 152950 on the M4, every value its checksum
# mixes printed, to diff a run on one QEMU against another. Run from the
# tree root after `make embcc embld rt-embedded`; writes dbg/.
set -u
EMBCC=${EMBCC:-$PWD/embcc}
EMBLD=${EMBLD:-$PWD/embld}; export EMBLD
mkdir -p dbg
python3 verify/fuzz/gen2.py 152950 > dbg/p.c
# mix prints each value, high and low words, before mixing it in
sed 's|^static void mix(u64 v) {|void putn(long v); void puts_(const char *s);\
static void mix(u64 v) { putn((long)(u32)(v >> 32)); puts_(" "); putn((long)(u32)v); puts_("\\n");|' \
    dbg/p.c > dbg/pv.c
grep -c 'putn((long)(u32)(v >> 32))' dbg/pv.c
T=thumbv7em-none-eabi d=dbg/m4
mkdir -p $d
for f in boot io; do "$EMBCC" --target=$T -c tests/harness/thumb-m4f/$f.c -o $d/$f.o; done
"$EMBCC" --target=$T -O1 -c verify/fuzz/m4drv.c -o $d/drv.o
for O in -O0 -O1; do
    "$EMBCC" --target=$T $O -Dmain=sw_main -Ilib/libc/include -c dbg/pv.c -o $d/p$O.o
    EMBCC_THUMB_HARNESS=$d sh tests/harness/thumb-m4f/link.sh $d/p$O.elf $d/p$O.o \
        $d/drv.o build/libc/$T/librt.a
    sh tests/harness/qrun.sh 60 qemu-system-arm -M mps2-an386 -cpu cortex-m4 \
        -semihosting -nographic -kernel $d/p$O.elf > dbg/out$O.txt 2>&1
    echo "$O: exit $?, $(wc -l < dbg/out$O.txt) lines"
done
qemu-system-arm --version | head -1
shasum -a 256 $d/*.elf build/libc/$T/librt.a 2>/dev/null || sha256sum $d/*.elf build/libc/$T/librt.a
