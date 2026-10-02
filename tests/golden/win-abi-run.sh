#!/bin/sh
# The Microsoft x64 convention, RUN: EmbCC's Win64 code called by gcc.
#
# win-abi.sh referees where each argument arrives, by disassembly, at
# -O1. What it cannot see is a callee that reads the right register too
# late -- after its own prologue has moved another parameter into it --
# and at -O2 three did: f(double a, double b) moved a into xmm1 before
# reading b out of it, f(long a, struct big s) put s's address in rcx
# while a was still there, and a fifth double was taken through rax into
# a stack slot it did not have. Nothing here can run a Windows program,
# but the code is x86-64 and these functions need no relocations: their
# bytes go into an ordinary x86-64 test image, and gcc calls them through
# ms_abi pointers. gcc's caller is the referee.
#
# The callee does not yet preserve what Windows says it must (rsi, rdi,
# xmm6-15 -- the -Wwindows-abi warning names them), so the caller is
# built never to keep anything there: what this checks is the argument
# passing, which EmbCC's own objects must agree on with each other.
set -u
echo "TEST-MARKER win-abi-run"
. "$(dirname "$0")/../lib.sh"

[ "$ARCH" = x86_64 ] || { echo "skipped: Win64 is an x86-64 convention"
                          exit 0; }
for t in llvm-objcopy llvm-nm llvm-objdump; do
    command -v $t > /dev/null 2>&1 || { echo "skipped: no $t"; exit 0; }
done

EMBCC=${EMBCC:-$EMBCC_ROOT/embcc}
out=$EMBCC_ROOT/tests/golden/out/win-abi-run
rm -rf "$out"; mkdir -p "$out"

cat > "$out/callee.c" <<'E'
struct big { long long x, y, z; };
struct c3 { char a, b, c; };
double swap2(double a, double b) { return b - a * 3; }
double rot3(double a, double b, double c) { return a + b * 10 + c * 100; }
long long byref2(long long a, struct big s) { return a * 3 + s.x + s.z * 7; }
long long byref5(long long a, long long b, long long c, long long d,
                 struct big s)
{ return a + b * 2 + c * 3 + d * 4 + s.x * 10 + s.y * 100 + s.z * 1000; }
int odd3(int k, struct c3 s) { return k + s.a + s.b * 10 + s.c * 100; }
double fstack(double a, double b, double c, double d, double e, double f)
{ return a - b * 2 + c * 3 - d * 4 + e * 5 - f * 6; }
long long mixed(int a, double b, long long c, float d, long long e)
{ return a + (long long)b * 10 + c * 100 + (long long)d * 1000 + e * 10000; }
E

cat > "$out/caller.c" <<'E'
struct big { long long x, y, z; };
struct c3 { char a, b, c; };
#define MS __attribute__((ms_abi))
__asm__(".section .text.winblob,\"ax\",@progbits\n"
        ".globl winblob\nwinblob:\n.incbin \"" BLOB "\"\n.previous\n");
extern const char winblob[];
#define FN(type, name) ((type)(winblob + OFF_##name))
typedef double MS (*swap2_t)(double, double);
typedef double MS (*rot3_t)(double, double, double);
typedef long long MS (*byref2_t)(long long, struct big);
typedef long long MS (*byref5_t)(long long, long long, long long, long long,
                                 struct big);
typedef int MS (*odd3_t)(int, struct c3);
typedef double MS (*fstack_t)(double, double, double, double, double,
                              double);
typedef long long MS (*mixed_t)(int, double, long long, float, long long);
#define T __attribute__((noinline)) static int
T t1(void) { return FN(swap2_t, swap2)(2, 10) != 4; }
T t2(void) { return FN(rot3_t, rot3)(1, 2, 3) != 321; }
T t3(void) { struct big s = { 5, 6, 7 };
             return FN(byref2_t, byref2)(11, s) != 33 + 5 + 49; }
T t4(void) { struct big s = { 5, 6, 7 };
             return FN(byref5_t, byref5)(1, 2, 3, 4, s) != 30 + 7650; }
T t5(void) { struct c3 s = { 1, 2, 3 };
             return FN(odd3_t, odd3)(4, s) != 4 + 1 + 20 + 300; }
T t6(void) { return FN(fstack_t, fstack)(1, 2, 3, 4, 5, 6) != -21; }
T t7(void) { return FN(mixed_t, mixed)(1, 2, 3, 4, 5) != 54321; }
int main(void)
{
    int bad = t1() | t2() << 1 | t3() << 2 | t4() << 3 | t5() << 4 |
              t6() << 5 | t7() << 6;
    return bad ? bad : 42;
}
E

# Win64 keeps rbx, rsi, rdi, r12-r15 and xmm6-15; EmbCC's callee keeps
# only the System V set, so gcc must hold nothing in the difference.
fixed="-ffixed-rsi -ffixed-rdi"
for k in 6 7 8 9 10 11 12 13 14 15; do fixed="$fixed -ffixed-xmm$k"; done

for opt in -O0 -O1 -O2; do
    b=$out/O${opt#-O}; mkdir -p "$b"
    "$EMBCC" --target=x86_64-windows-gnu $opt -c "$out/callee.c" \
        -o "$b/c.obj" 2> "$b/cc.log" || {
        echo "FAIL $opt: embcc could not compile the callee:"
        grep -v 'Wwindows-abi' "$b/cc.log"; exit 1; }
    if llvm-objdump -r "$b/c.obj" | grep -q IMAGE_REL; then
        echo "FAIL $opt: the callee has relocations; its bytes cannot be"
        echo "      moved into the test image as they are"; exit 1
    fi
    llvm-objcopy --dump-section .text="$b/c.bin" "$b/c.obj" "$b/junk.obj"
    defs=$(llvm-nm "$b/c.obj" |
           awk '$2 == "T" { printf " -DOFF_%s=0x%s", $3, $1 }')
    # shellcheck disable=SC2086
    x86_gcc_c "$out/caller.c" -o "$b/caller.o" -O1 -mno-red-zone \
        -DBLOB="\"$b/c.bin\"" $defs $fixed || {
        echo "FAIL $opt: the gcc caller does not compile"; exit 1; }
    x86_link "$b/run" "$b/caller.o" > "$b/ln.log" 2>&1 || {
        echo "FAIL $opt: the test image does not link:"; cat "$b/ln.log"
        exit 1; }
    x86_run "$b/run" > /dev/null 2>&1
    rc=$?
    [ "$rc" = 42 ] || {
        echo "FAIL $opt: exit $rc (a bit per wrong call, 42 is all right)"
        echo "      bits: 1 swap2, 2 rot3, 4 byref2, 8 byref5, 16 odd3,"
        echo "      32 fstack, 64 mixed"; exit 1; }
done
echo "EmbCC's Win64 code at -O0, -O1 and -O2 takes its arguments where"
echo "gcc's ms_abi calls put them: float permutations, by-reference"
echo "aggregates in registers and on the stack, a 3-byte one, float"
echo "arguments past the fourth"
