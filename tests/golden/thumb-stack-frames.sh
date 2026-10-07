#!/bin/sh
# Stack frames on ARMv7-M, where an RTOS gives every task a small stack.
#
#   - A function that cannot return -- an RTOS task's `for (;;)`, no
#     IR_RET, no tail call, a last instruction that jumps -- saves
#     nothing: no caller is ever resumed. FreeRTOS's prvIdleTask pushed
#     r3 and lr and kept an 8-byte frame to do it; clang's has none.
#   - A void function that falls off its end still returns: it has no
#     IR_RET either, and treating it as one that cannot return dropped its
#     push and its `pop {..., pc}` (vListInsert ran into the next
#     function).
#   - A parameter nothing reads -- `(void) pvParameters;` -- gets no slot
#     and no store. After mem2reg a parameter is read as an operand, not
#     only through LDVAR; counting only LDVARs dropped live parameters.
#   - 64-bit temporaries that do not get a register share eight-byte
#     slots as the four-byte ones share theirs: strtol's conv reserved
#     96 bytes, a slot per temporary, and now 72.
# FreeRTOS's frames went from 1.15 times clang's to 1.09.
set -u
echo "TEST-MARKER thumb-stack-frames"
. "$(dirname "$0")/../lib.sh"

OD=${EMBCC_LLVM_OBJDUMP:-llvm-objdump}
command -v "$OD" >/dev/null 2>&1 || { echo "SKIP: llvm-objdump not found"; exit 0; }
EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/thumb-stack-frames
rm -rf "$out"; mkdir -p "$out"

cat > "$out/f.c" <<'EOT'
extern void work(int);
extern volatile int flag;
void task(void *pv)
{
    (void)pv;
    for (;;) {
        work(1);
        if (flag)
            work(2);
    }
}
struct node { struct node *next, *prev; int v; };
void insert(struct node *at, struct node *n)
{
    work(n->v);
    n->next = at->next;
    n->prev = at;
    at->next->prev = n;
    at->next = n;
}
int unused(int pv, int x)
{
    int buf[4];
    (void)pv;
    for (int i = 0; i < 4; i++)
        buf[i] = x + i;
    work(buf[x & 3]);
    return buf[1];
}
EOT
for o in -O1 -O2 -Os; do
    "$EMBCC" --target=thumbv7em-none-eabi $o -c "$out/f.c" -o "$out/f$o.o" || {
        echo "FAIL: could not compile at $o"; exit 1; }
    "$OD" -d --no-show-raw-insn "$out/f$o.o" > "$out/f$o.dis"
    body() {
        awk -v f="<$1>:" '$2 == f { on = 1; next } /^[0-9a-f]+ <.*>:$/ { on = 0 }
                          on && /^ *[0-9a-f]+:/' "$out/f$o.dis"
    }
    fail() { echo "FAIL: $o: $1:"; body "$2"; exit 1; }
    [ "$(body task | grep -cE 'push|sub[^,]*sp|vpush')" = 0 ] ||
        fail "a function that cannot return should save nothing" task
    [ "$(body insert | grep -cE 'pop.*pc|bx[[:space:]]+lr|b(\.w)?[[:space:]]')" -ge 1 ] ||
        fail "a void function that falls off its end must still return" insert
    [ "$(body unused | grep -cE 'str[^,]*r0, \[sp')" = 0 ] ||
        fail "an unread parameter should not be stored" unused
done
echo "a task loop saves nothing; a void function still returns; an unread parameter is not stored"

"$EMBCC" --target=thumbv7em-none-eabi -Os -Ilib/libc/include \
    -c lib/libc/src/stdlib/strtol.c -o "$out/strtol.o" || {
    echo "FAIL: strtol.c"; exit 1; }
"$OD" -d --no-show-raw-insn "$out/strtol.o" |
    awk '$2 == "<conv>:" { on = 1; next } /^[0-9a-f]+ <.*>:$/ { on = 0 }
         on && /sub[[:space:]]+sp, #/ { sub(/.*#/, ""); print; exit }' > "$out/conv-sub"
n=$(cat "$out/conv-sub"); n=$((n))
[ "$n" -le 72 ] || { echo "FAIL: strtol's conv reserves $n bytes, more than 72"; exit 1; }
echo "strtol's conv: $n bytes of frame (its 64-bit temporaries share slots)"
