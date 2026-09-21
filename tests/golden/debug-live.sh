#!/bin/sh
# -g, proven by DEBUGGING: the program runs in the QEMU harness with its gdb
# stub, a real gdb stops it on a source line, and what gdb then reads — the
# parameters and locals (through DW_OP_fbreg off the frame pointer), struct
# members and bitfields, an array, a pointer chased through a struct, the
# backtrace, and the value `finish` returns — must be the program's actual
# state. debug-line/debug-locals check the tables; this checks that a
# debugger using them sees the truth. Both targets; on aarch64 a function
# with a VLA addresses its frame through x19, and its locals must still read
# right.
#
# Skips honestly without gdb (with this target's architecture) or QEMU.
set -u
echo "TEST-MARKER debug-live"
. "$(dirname "$0")/../lib.sh"

command -v gdb >/dev/null 2>&1 || { echo "skipped: no gdb on this host"; exit 0; }
case "$TARGET" in
    aarch64-elf) GARCH=aarch64; QEMU=qemu-system-aarch64 ;;
    *)           GARCH=i386:x86-64; QEMU=qemu-system-x86_64 ;;
esac
gdb -batch -nx -ex "set architecture $GARCH" >/dev/null 2>&1 ||
    { echo "skipped: this gdb has no $GARCH support"; exit 0; }
command -v "$QEMU" >/dev/null 2>&1 || { echo "skipped: $QEMU absent"; exit 0; }

out=tests/golden/out/debug-live-$ARCH
rm -rf "$out"; mkdir -p "$out"

cat > "$out/live.c" <<'CEOF'
struct node { int val; struct node *next; };
struct pt { int x; long y; unsigned flags : 3; unsigned mode : 5; };

static long leaf(int a, long b, struct pt p, const char *s)
{
    long total = a + b + p.x + p.y;
    char first = s[0];
    int arr[3] = { 7, 8, 9 };
    struct node n2 = { 2, 0 };
    struct node n1 = { 1, &n2 };
    total += first + arr[2] + n1.next->val + p.flags + p.mode;
    return total;                                   /* STOP-LEAF */
}

static int vla_user(int n)
{
    int v[n];
    int k = n * 3;
    for (int i = 0; i < n; i++)
        v[i] = i + k;
    return v[n - 1] + k;                            /* STOP-VLA */
}

int main(void)
{
    struct pt p = { 3, 4000000000L, 5, 17 };
    long r = leaf(20, 22, p, "hello");
    int q = vla_user(5);
    return (int)((r + q) & 0x7f);
}
CEOF
line_of() { grep -n "$1" "$out/live.c" | cut -d: -f1; }
L_LEAF=$(line_of STOP-LEAF)
L_VLA=$(line_of STOP-VLA)

"$EMBCC" --target="$TARGET" -g -c "$out/live.c" -o "$out/live.o" ||
    { echo "embcc -g failed"; exit 1; }
if [ "$ARCH" = aarch64 ]; then
    sh "$EMBCC_ROOT/tests/harness/aarch64/link.sh" -o "$out/live.elf" "$out/live.o" ||
        { echo "harness link failed"; exit 1; }
    image="$out/live.elf"; symbols="$out/live.elf"
    qargs="-M virt -cpu cortex-a72 -semihosting -nographic -kernel $image"
else
    EMBCC_HARNESS_KEEP64=1 sh "$EMBCC_ROOT/tests/harness/x86_64/link.sh" \
        -o "$out/live.elf" "$out/live.o" || { echo "harness link failed"; exit 1; }
    # QEMU boots the ELF32 Multiboot relabel; gdb reads the 64-bit original
    image="$out/live.elf"; symbols="$out/live.elf.64"
    qargs="-cpu max -m 128M -display none -no-reboot -monitor none -serial none
           -debugcon null -device isa-debug-exit,iobase=0xf4,iosize=0x04
           -kernel $image"
fi

port=$((20000 + $$ % 20000))
# shellcheck disable=SC2086
$QEMU $qargs -S -gdb tcp:127.0.0.1:$port >/dev/null 2>&1 &
qpid=$!
trap 'kill $qpid 2>/dev/null' EXIT INT TERM
sleep 1

# Hardware breakpoints: on x86 the Multiboot loader writes the code after gdb
# attaches, which would erase a software one.
"$EMBCC_ROOT/tests/harness/qrun.sh" 60 gdb -batch -nx \
    -ex "set architecture $GARCH" -ex "file $symbols" \
    -ex "target remote 127.0.0.1:$port" \
    -ex "hbreak live.c:$L_LEAF" -ex "hbreak live.c:$L_VLA" \
    -ex continue \
    -ex "printf \"A=%d B=%ld PX=%d PY=%ld FL=%u MO=%u\\n\", a, b, p.x, p.y, p.flags, p.mode" \
    -ex "printf \"S=%s FIRST=%d ARR=%d,%d,%d NEXT=%d TOTAL=%ld\\n\", s, first, arr[0], arr[1], arr[2], n1.next->val, total" \
    -ex "bt" -ex finish \
    -ex continue \
    -ex "printf \"N=%d K=%d V4=%d\\n\", n, k, v[4]" \
    > "$out/gdb.log" 2>&1
kill $qpid 2>/dev/null

fail=0
want() {   # pattern description
    grep -qE "$1" "$out/gdb.log" || { echo "MISSING: $2"; fail=1; }
}
want "Breakpoint 1, leaf \(a=20, b=22" "stopped in leaf with its arguments"
want "A=20 B=22 PX=3 PY=4000000000 FL=5 MO=17" "parameters and struct (bitfield) members"
want "S=hello FIRST=104 ARR=7,8,9 NEXT=2 TOTAL=4000000182" "locals: string, char, array, pointer chase, long"
want "#1 .* main \(\) at .*live\.c:[0-9]+" "backtrace into main"
want "Value returned is \\\$[0-9]+ = 4000000182" "finish returns leaf's value"
want "Breakpoint 2, vla_user \(n=5\)" "stopped in the VLA function"
want "N=5 K=15 V4=19" "a VLA function's locals and its array"
if [ $fail -ne 0 ]; then
    echo "--- gdb session ---"; cat "$out/gdb.log"; exit 1
fi
echo "debug-live ($ARCH): gdb stopped the running program on source lines and read"
echo "  arguments, locals, struct/bitfield members, an array, a pointer chase,"
echo "  the backtrace, a returned value, and a VLA frame — all correct"
