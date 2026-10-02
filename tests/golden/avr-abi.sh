#!/bin/sh
# AVR: the calling convention, against the DOCUMENTED rules, both directions.
#
# Every other ABI check on this target compiled both sides with EmbCC, so a
# convention it read consistently wrong agreed with itself all the way through.
# Two bugs lived in exactly that gap: a call to a function returning a struct
# of more than eight bytes lost its first argument (the caller placed it in
# r25:r24, then wrote the hidden pointer on top of it -- the callee always
# knew better), and five- and six-byte structs were returned in r20 where
# avr-gcc returns them in r18.
#
# ---- the oracle is the rule, not another compiler ---------------------
#
# The natural cross-check is clang, and it is not usable here. clang 23's AVR
# struct ARGUMENTS put the first field in the highest registers, against the
# rule below; Rust's AVR backend, which copies clang's convention, documents it
# as "not binary-compatible with AVR-GCC". What avr-gcc does is written down in
# avr-libc's FAQ ("What registers are used by the compiler?"), and that is what
# this checks:
#
#   * arguments are allocated left to right, r25 down to r8, each starting in
#     an EVEN register -- an odd size leaves one free register above it -- and
#     bytes fill the block from its lowest register, like an integer;
#   * an argument is wholly in registers or wholly on the stack, and once one
#     is on the stack every later one is too, packed at its natural size;
#   * a value returned in registers is padded to the next POWER OF TWO: r24
#     for up to two bytes, r22 for four, r18 for eight;
#   * anything larger returns in memory the caller provides, as an implicit
#     first argument.
#
# The counterpart to EmbCC is hand-written assembly that puts every byte where
# those rules say, assembled by EmbCC's own assembler. The generator below
# writes it, and the expected output, from the rules alone -- neither depends
# on anything EmbCC decides.
#
# BOTH directions, because the caller's placement and the callee's are
# separate code paths in the backend, and the hidden-pointer bug was in one of
# them only: EmbCC calling assembly, and assembly calling EmbCC. For every
# struct size from one byte to eight, as the first argument, as the second
# after a byte, and as a return value; then the stack spill, and a return
# through the hidden pointer.
set -u
echo "TEST-MARKER avr-abi"
. "$(dirname "$0")/../lib.sh"

out=tests/golden/out/avr-abi
rm -rf "$out"; mkdir -p "$out"
EMBCC=${EMBCC:-./embcc}
export EMBCC
H=$out/h; mkdir -p "$H"

QEMU=${EMBCC_QEMU_AVR:-qemu-system-avr}
command -v "$QEMU" >/dev/null 2>&1 || { echo "SKIP: no $QEMU"; exit 0; }

python3 - "$out" <<'PY' || { echo "the generator failed"; exit 1; }
import sys, os
out = sys.argv[1]
even = lambda n: n + (n & 1)
def pow2(n):
    p = 1
    while p < n: p <<= 1
    return max(p, 2)
first  = lambda n: 26 - even(n)        # first argument's lowest register
second = lambda n: 24 - even(n)        # after a one-byte argument in r24
retb   = lambda n: 26 - pow2(n)        # a returned value's lowest register
S = range(1, 9)

c, a, want = [], [], []
c.append('void writec(int c); void puts_(const char *s);')
c.append('unsigned char g_buf[16], g_c;')
c.append('static void phex(unsigned char b) { static const char d[] = "0123456789abcdef"; writec(d[b >> 4]); writec(d[b & 15]); }')
c.append('static void show(const char *tag, int n) { int k; puts_(tag); writec(\':\'); for (k = 0; k < n; k++) { writec(\' \'); phex(g_buf[k]); } writec(\'\\n\'); }')
c.append('static void clear(void) { int k; for (k = 0; k < 16; k++) g_buf[k] = 0; g_c = 0; }')
a.append('\t.text')

for n in S:
    c.append('struct s%d { unsigned char b[%d]; };' % (n, n))
    # ---- assembly CALLEES, EmbCC calls them
    c.append('struct s%d a_ret%d(void);' % (n, n))
    a += ['\t.globl a_ret%d' % n, 'a_ret%d:' % n]
    a += ['\tldi r%d, 0x%02x' % (retb(n) + k, (n << 4) | k) for k in range(n)]
    a.append('\tret')
    c.append('void a_arg%d(struct s%d s);' % (n, n))
    a += ['\t.globl a_arg%d' % n, 'a_arg%d:' % n]
    a += ['\tsts g_buf+%d, r%d' % (k, first(n) + k) for k in range(n)]
    a.append('\tret')
    c.append('void a_arg2_%d(unsigned char x, struct s%d s);' % (n, n))
    a += ['\t.globl a_arg2_%d' % n, 'a_arg2_%d:' % n, '\tsts g_c, r24']
    a += ['\tsts g_buf+%d, r%d' % (k, second(n) + k) for k in range(n)]
    a.append('\tret')
    # ---- EmbCC CALLEES, assembly calls them
    c.append('void c_arg%d(struct s%d s) { int k; for (k = 0; k < %d; k++) g_buf[k] = s.b[k]; }' % (n, n, n))
    c.append('struct s%d c_ret%d(void) { struct s%d r; int k; for (k = 0; k < %d; k++) r.b[k] = (unsigned char)(0x60 + k); return r; }' % (n, n, n, n))
    c.append('void a_call_arg%d(void); void a_call_ret%d(void);' % (n, n))
    a += ['\t.globl a_call_arg%d' % n, 'a_call_arg%d:' % n]
    a += ['\tldi r%d, 0x%02x' % (first(n) + k, 0x50 + k) for k in range(n)]
    a += ['\tcall c_arg%d' % n, '\tret']
    a += ['\t.globl a_call_ret%d' % n, 'a_call_ret%d:' % n, '\tcall c_ret%d' % n]
    a += ['\tsts g_buf+%d, r%d' % (k, retb(n) + k) for k in range(n)]
    a.append('\tret')

# ---- the stack spill: two eight-byte structs fill r25..r10, so a four-byte
# value needs r9..r6 -- below r8 -- and goes on the stack, and the byte after
# it follows it there although r9:r8 are free.
c.append('void a_spill(struct s8 x, struct s8 y, unsigned long z, unsigned char w);')
a += ['\t.globl a_spill', 'a_spill:', '\tin r30, 0x3d', '\tin r31, 0x3e']
# after `call`: SP+1..SP+2 hold the return address, the first stack word is SP+3
a += ['\tldd r18, Z+%d' % (3 + k) for k in range(5)][:0]
for k in range(5):
    a += ['\tldd r18, Z+%d' % (3 + k), '\tsts g_buf+%d, r18' % k]
a.append('\tret')
c.append('unsigned char c_spill(struct s8 x, struct s8 y, unsigned long z, unsigned char w) { g_buf[0] = (unsigned char)z; g_buf[1] = (unsigned char)(z >> 8); g_buf[2] = (unsigned char)(z >> 16); g_buf[3] = (unsigned char)(z >> 24); g_buf[4] = w; g_buf[5] = x.b[0]; g_buf[6] = y.b[7]; return 0; }')
c.append('void a_call_spill(void);')
# the caller pushes the stack words highest address first, so z's low byte
# ends up lowest; x in r18..r25 and y in r10..r17 (call-saved: pushed/popped)
a += ['\t.globl a_call_spill', 'a_call_spill:',
      '\tpush r10', '\tpush r11', '\tpush r12', '\tpush r13',
      '\tpush r14', '\tpush r15', '\tpush r16', '\tpush r17',
      '\tldi r24, 0x45', '\tpush r24']                       # w
a += ['\tldi r24, 0x%02x' % (0x41 + k) for k in range(3, -1, -1)][:0]
for k in range(3, -1, -1):
    a += ['\tldi r24, 0x%02x' % (0x41 + k), '\tpush r24']   # z, high byte first
a += ['\tldi r%d, 0x%02x' % (18 + k, 0x70 + k) for k in range(8)]
a += ['\tldi r%d, 0x%02x' % (16 + k, 0x80 + k) for k in range(2)]
a += ['\tldi r24, 0x%02x' % (0x88 + k) for k in range(0)]
# r10..r15 cannot take ldi: go through r24
for k in range(6):
    a += ['\tldi r26, 0x%02x' % (0x82 + k), '\tmov r%d, r26' % (10 + k)]
a += ['\tldi r24, 0x76', '\tldi r25, 0x77']                   # x.b[6], x.b[7]
a += ['\tcall c_spill',
      '\tpop r0', '\tpop r0', '\tpop r0', '\tpop r0', '\tpop r0',
      '\tpop r17', '\tpop r16', '\tpop r15', '\tpop r14',
      '\tpop r13', '\tpop r12', '\tpop r11', '\tpop r10', '\tret']

# ---- a return through the hidden pointer: r25:r24 is the buffer, and the
# byte argument is therefore the SECOND argument, in r22.
c.append('struct s10 { unsigned char b[10]; };')
c.append('struct s10 a_big(unsigned char x);')
a += ['\t.globl a_big', 'a_big:', '\tmovw r30, r24']
for k in range(10):
    a += ['\tmov r18, r22', '\tsubi r18, %d' % (-k & 0xff), '\tst Z+, r18']
a.append('\tret')

# ---- main
m = ['int main(void)', '{', '    int k;']
for n in S:
    m.append('    { struct s%d r = a_ret%d(); for (k = 0; k < %d; k++) g_buf[k] = r.b[k]; show("ret%d", %d); }' % (n, n, n, n, n))
    want.append('ret%d: ' % n + ' '.join('%02x' % ((n << 4) | k) for k in range(n)))
    m.append('    { struct s%d s; for (k = 0; k < %d; k++) s.b[k] = (unsigned char)(0x30 + k); clear(); a_arg%d(s); show("arg%d", %d); }' % (n, n, n, n, n))
    want.append('arg%d: ' % n + ' '.join('%02x' % (0x30 + k) for k in range(n)))
    m.append('    { struct s%d s; for (k = 0; k < %d; k++) s.b[k] = (unsigned char)(0x38 + k); clear(); a_arg2_%d(0x77, s); g_buf[%d] = g_c; show("arg2_%d", %d); }' % (n, n, n, n, n, n + 1))
    want.append('arg2_%d: ' % n + ' '.join(['%02x' % (0x38 + k) for k in range(n)] + ['77']))
    m.append('    clear(); a_call_arg%d(); show("callarg%d", %d);' % (n, n, n))
    want.append('callarg%d: ' % n + ' '.join('%02x' % (0x50 + k) for k in range(n)))
    m.append('    clear(); a_call_ret%d(); show("callret%d", %d);' % (n, n, n))
    want.append('callret%d: ' % n + ' '.join('%02x' % (0x60 + k) for k in range(n)))
m.append('    { struct s8 x, y; for (k = 0; k < 8; k++) { x.b[k] = 0; y.b[k] = 0; } clear(); a_spill(x, y, 0x44434241ul, 0x45); show("spill", 5); }')
want.append('spill: 41 42 43 44 45')
m.append('    clear(); a_call_spill(); show("callspill", 7);')
want.append('callspill: 41 42 43 44 45 70 81')
m.append('    { struct s10 r = a_big(0x20); for (k = 0; k < 10; k++) g_buf[k] = r.b[k]; show("big", 10); }')
want.append('big: ' + ' '.join('%02x' % (0x20 + k) for k in range(10)))
m.append('    puts_("<<END>>\\n");')
m.append('    for (;;) ;')
m.append('}')
open(os.path.join(out, 't.c'), 'w').write('\n'.join(c + m) + '\n')
open(os.path.join(out, 'a.S'), 'w').write('\n'.join(a) + '\n')
open(os.path.join(out, 'want'), 'w').write('\n'.join(want) + '\n')
PY

"$EMBCC" --target=avr -c tests/harness/avr/boot.S -o "$H/boot.o" || exit 1
"$EMBCC" --target=avr -c "$out/a.S" -o "$H/a.o" 2> "$out/a.err" || {
    echo "the assembly counterpart did not assemble:"; head -4 "$out/a.err"; exit 1; }
for O in -O0 -O1 -O2 -Os; do
    "$EMBCC" --target=avr $O -c tests/harness/avr/io.c -o "$H/io.o" || exit 1
    "$EMBCC" --target=avr $O -c "$out/t.c" -o "$H/t.o" 2> "$out/c.err" || {
        echo "$O: did not compile:"; head -4 "$out/c.err"; exit 1; }
    "${EMBLD:-./embld}" -e __vectors -Ttext 0x0 -Tdata 0x100 \
        "$H/boot.o" "$H/io.o" "$H/t.o" "$H/a.o" -o "$H/t.elf" 2> "$out/l.err" || {
        echo "$O: link failed:"; head -4 "$out/l.err"; exit 1; }
    EMBCC_QEMU_UNTIL='<<END>>' EMBCC_QEMU_TIMEOUT=${EMBCC_QEMU_TIMEOUT:-30} \
        sh tests/harness/avr/run.sh "$H/t.elf" 2>/dev/null \
        | sed '/<<END>>/,$d' > "$out/got$O"
    cmp -s "$out/want" "$out/got$O" || {
        echo "$O: EmbCC does not follow avr-gcc's documented calling convention:"
        diff "$out/want" "$out/got$O" | head -12
        exit 1; }
done

echo "EmbCC follows avr-gcc's documented calling convention at four
optimisation levels, in both directions -- EmbCC calling hand-written assembly
and assembly calling EmbCC -- for every struct size from one byte to eight as
the first argument, the second, and the return value; for an argument that
spills to the stack and the one after it; and for a struct returned through
the caller's hidden pointer"
