#!/bin/sh
# The Arduino Mega's parts: the ATmega1280 (avr51) and the ATmega2560
# (avr6, whose calls push a 3-byte PC).
#
#   - what depends on the PC's size -- stack arguments, a variadic function,
#     setjmp/longjmp across frames, a constructor, an interrupt, .data and
#     .rodata copied in -- on QEMU's mega and mega2560, at -O0, -Os, -O2;
#   - a program past 128 KiB on the 2560 (calls and returns beyond what a
#     word address reaches, the function pointers through embld's
#     trampolines, in code and in data), printing the hash Python computes;
#   - an avr5 object refused in an avr6 link (their frames differ);
#   - where avr-gcc is installed: its code calling EmbCC's and EmbCC's
#     calling its, with stack arguments, on all three parts; and
#     <avr/iomxx0_1.h>'s every register, bit and vector against avr-libc's.
set -u
EMBCC=${EMBCC:-./embcc}
echo "TEST-MARKER avr-mega"
out=tests/golden/out/avr-mega
rm -rf "$out"; mkdir -p "$out"
fail() { echo "FAIL: $*"; exit 1; }
here=tests/golden/avr-mega
run() { # MACHINE ELF: its console until <<END>>
    EMBCC_AVR_MACHINE=$1 EMBCC_QEMU_UNTIL='<<END>>' EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-60} \
        sh tests/harness/avr/run.sh "$2" 2>&1 | tr -d '\r'
}
qemu=1
command -v qemu-system-avr > /dev/null 2>&1 || { qemu=0; echo "  (no qemu-system-avr: built, not run)"; }

want="many=385 vsum=1234 ctor=7 data=1234 rodata copied
longjmp gave 42
return address nonzero
interrupts: 5"
for pm in atmega1280:mega atmega2560:mega2560; do
    p=${pm%%:*} m=${pm#*:}
    for O in -O0 -Os -O2; do
        "$EMBCC" --target=avr -mmcu=$p $O "$here/pc.c" -o "$out/pc-$p$O.elf" > "$out/cc.log" 2>&1 ||
            fail "$p $O does not build: $(head -3 "$out/cc.log")"
        [ $qemu = 1 ] || continue
        run $m "$out/pc-$p$O.elf" | sed '/<<END>>/,$d' > "$out/pc-$p$O.out"
        [ "$(cat "$out/pc-$p$O.out")" = "$want" ] || { diff "$out/pc-$p$O.out" - <<E
$want
E
            fail "$p $O on QEMU's $m"; }
    done
done
[ $qemu = 1 ] && echo "  stack arguments, varargs, setjmp, a constructor, an interrupt, .data: the 1280 and 2560, three -O"

python3 $here/far.py "$out" || fail "far.py"
"$EMBCC" --target=avr -mmcu=atmega2560 -Os "$out/far.c" -o "$out/far.elf" > "$out/far.log" 2>&1 ||
    fail "the 2560 program past 128 KiB does not build: $(head -3 "$out/far.log")"
python3 - "$out/far.elf" <<'PY' || fail "the far program: not past 128 KiB, or no trampolines"
import struct, sys
d = open(sys.argv[1], "rb").read()
shoff, = struct.unpack_from("<I", d, 32); shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 46)
secs = [struct.unpack_from("<IIIIII", d, shoff + i * shentsize) for i in range(shnum)]
names = secs[shstrndx][4]
name = lambda s: d[names + s[0]:d.index(b"\0", names + s[0])].decode()
by = {name(s): s for s in secs}
assert by[".text"][3] + by[".text"][5] > 0x20000, hex(by[".text"][3] + by[".text"][5])
assert by[".vectors"][5] > 57 * 4, "no .trampolines after the vectors"
PY
if [ $qemu = 1 ]; then
    run mega2560 "$out/far.elf" | sed '/<<END>>/,$d' > "$out/far.out"
    cmp -s "$out/far.out" "$out/far.expect" || { diff "$out/far.out" "$out/far.expect"; fail "the far program on QEMU's mega2560"; }
    echo "  past 128 KiB on the 2560: calls, returns and pointers through trampolines, the hash Python computes"
fi

echo 'int f(void) { return 1; }' > "$out/five.c"
"$EMBCC" --target=avr -mmcu=atmega328p -c "$out/five.c" -o "$out/five.o" || fail "the avr5 object"
"$EMBCC" --target=avr -mmcu=atmega2560 "$here/pc.c" "$out/five.o" -o "$out/mix.elf" > "$out/mix.log" 2>&1 &&
    fail "an avr5 object was linked into an avr6 image"
grep -q "frames differ" "$out/mix.log" || fail "the mix: $(cat "$out/mix.log")"
echo "  an avr5 object in an avr6 link is refused"

if command -v avr-gcc > /dev/null 2>&1; then
    for pm in atmega328p:uno atmega1280:mega atmega2560:mega2560; do
        p=${pm%%:*} m=${pm#*:}
        avr-gcc -mmcu=$p -Os -c "$here/caller.c" -o "$out/caller-$p.o" &&
            "$EMBCC" --target=avr -mmcu=$p -Os -c "$here/callee.c" -o "$out/callee-$p.o" &&
            "$EMBCC" --target=avr -mmcu=$p "$out/caller-$p.o" "$out/callee-$p.o" \
                "$(avr-gcc -mmcu=$p -print-libgcc-file-name)" -o "$out/x-$p.elf" ||
            fail "the avr-gcc and EmbCC objects for $p do not link"
        [ $qemu = 1 ] || continue
        run $m "$out/x-$p.elf" | grep -q "gcc calls embcc: 385 (385); embcc calls gcc: 220 (220)" ||
            fail "avr-gcc and EmbCC disagree on $p's calls: $(run $m "$out/x-$p.elf")"
    done
    [ $qemu = 1 ] && echo "  avr-gcc's code and EmbCC's call each other, stack arguments and all, on the 328P, 1280 and 2560"
    printf '#include <avr/io.h>\n' > "$out/io.c"
    for p in atmega1280 atmega2560; do
        avr-gcc -mmcu=$p -dM -E "$out/io.c" | sort > "$out/al-$p.txt"
        "$EMBCC" --target=avr -mmcu=$p -dM -E "$out/io.c" | sort > "$out/em-$p.txt"
        python3 - "$out/al-$p.txt" "$out/em-$p.txt" <<'PY' || fail "<avr/iomxx0_1.h> differs from avr-libc's for $p"
import re, sys
def defs(p):
    d = {}
    for l in open(p):
        m = re.match(r"#define (\w+)(?:\([^)]*\))?\s*(.*)", l)
        if m:
            d[m.group(1)] = re.sub(r"\s+", "", m.group(2)).lower()
    return d
a, e = defs(sys.argv[1]), defs(sys.argv[2])
reg = lambda v: re.match(r"_sfr_(io|mem)(8|16)\(|_vector\(|^\d+$|^0x[0-9a-f]+$", v)
def num(v):
    try:
        return int(v, 0)
    except (TypeError, ValueError):
        return None
same = lambda x, y: x == y or (num(x) is not None and num(x) == num(y))
bad = [(k, a[k], e.get(k)) for k in a if reg(a[k]) and not k.startswith("_") and
       not re.match(r"(FUSE|LOCK|BLB|LB_|[LHE]FUSE|SLEEP_MODE|INT|UINT|SIZE|PTRDIFF|SIG_|WCHAR|WINT|XRAM)", k) and
       (k not in e or not same(a[k], e[k]))]
if bad:
    print(len(bad), "registers, bits or vectors differ:", bad[:8])
    sys.exit(1)
PY
    done
    echo "  <avr/iomxx0_1.h>: every register, bit and vector avr-libc's, the 1280's and the 2560's"
else
    echo "  (no avr-gcc: the cross-calls and avr-libc's registers are not compared)"
fi
echo "PASS: avr-mega"
