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
# Variadic functions too: Windows' va_list is a pointer walking eight-byte
# slots from the home area, which the callee fills from rcx..r9 -- EmbCC
# built the System V record instead, and read "r9" for the fifth and
# sixth slots, even calling itself.
#
# And what a Windows callee must preserve beyond System V's set -- rsi,
# rdi and all 128 bits of xmm6-15: gcc's caller keeps values there, and
# an assembly caller plants a canary in each around a callee that copies
# a block (rep movsq: rsi and rdi) and keeps doubles live across a call.
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
#include <stdarg.h>
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
/* variadic: ints past the four home slots, doubles among them, va_copy */
long long vints(int n, ...)
{ va_list ap; long long t = 0; va_start(ap, n);
  for (int i = 0; i < n; i++) t = t * 10 + va_arg(ap, int);
  va_end(ap); return t; }
long long vmixd(int n, ...)
{ va_list ap; long long t = 0; va_start(ap, n);
  for (int i = 0; i < n; i++)
      t = t * 10 + (i & 1 ? (long long)(va_arg(ap, double) * 2) : va_arg(ap, long long));
  va_end(ap); return t; }
long long vcopy(int n, ...)
{ va_list ap, aq; long long a = 0, b = 0; va_start(ap, n); va_copy(aq, ap);
  for (int i = 0; i < n; i++) a = a * 10 + va_arg(ap, int);
  for (int i = 0; i < n; i++) b = b * 10 + va_arg(aq, int);
  va_end(aq); va_end(ap); return a * 1000000 + b; }
/* va_arg of structures and of __int128: an 8-byte one in its slot, a
 * 12-byte one and the __int128 by reference to the caller's copy (the low
 * half only: a shift of it would call a helper, a relocation) */
struct s8 { int a, b; };
struct s12 { int a, b, c; };
long long vstruct(int n, ...)
{ va_list ap; va_start(ap, n);
  struct s8 s = va_arg(ap, struct s8);
  struct s12 t = va_arg(ap, struct s12);
  __int128 q = va_arg(ap, __int128);
  va_end(ap);
  return n + s.a * 10 + s.b * 100 + t.c * 1000 + (long long)q * 10000; }
/* ...and EmbCC as the CALLER of someone else's variadic function, through
 * a pointer so that no relocation is needed: a double among the first
 * four arguments must travel in its integer register as well */
long long callv(long long (*f)(int, ...), int k)
{ double h = k * 0.5; return f(4, k, h, k + 1, h * 3); }
/* everything a Win64 callee must give back, used: a block copy, and
 * doubles enough to reach the high half of the pool (no constants: those
 * would be relocations) */
struct blk { long long w[24]; };
long long clob(long long n, long long (*g)(long long))
{ struct blk a, b; for (int i = 0; i < 24; i++) a.w[i] = n + i;
  b = a;
  double x = n, y = x + x, z = y + x, u = z * y, v = u - x, w = v / y,
         p = w * u, q = p + z, r = q - y;
  long long k = g(n);
  return b.w[7] + k + (long long)(x + y + z + u + v + w + p + q + r); }
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
typedef long long MS (*vints_t)(int, ...);
typedef long long MS (*callv_t)(long long MS (*)(int, ...), int);
typedef long long MS (*clob_t)(long long, long long MS (*)(long long));
MS long long ident(long long n) { return n; }
/* check(fn, n): calls fn(n, ident) the Microsoft way with a canary in
 * rsi, rdi and both halves of xmm6-15; 0 when every one came back, else
 * a bit per register lost (1 rsi, 2 rdi, 4 << k xmm(6+k)) */
long long check(void *fn, long long n);
__asm__(".text\n.globl check\ncheck:\n"
        "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\n"
        "sub $56, %rsp\n"
        "mov %rdi, %r12\nmov %rsi, %r13\n"
        "movabs $0x5151515151515151, %rsi\nmovabs $0x7d7d7d7d7d7d7d7d, %rdi\n"
#define CAN(k) "movabs $0x0" #k "0" #k "0" #k "0" #k "0" #k "0" #k "0" #k "0" #k ", %rax\n" \
               "movq %rax, %xmm" #k "\npunpcklqdq %xmm" #k ", %xmm" #k "\n"
        CAN(6) CAN(7) CAN(8) CAN(9)
        "movabs $0x0a0a0a0a0a0a0a0a, %rax\nmovq %rax, %xmm10\npunpcklqdq %xmm10, %xmm10\n"
        "movabs $0x0b0b0b0b0b0b0b0b, %rax\nmovq %rax, %xmm11\npunpcklqdq %xmm11, %xmm11\n"
        "movabs $0x0c0c0c0c0c0c0c0c, %rax\nmovq %rax, %xmm12\npunpcklqdq %xmm12, %xmm12\n"
        "movabs $0x0d0d0d0d0d0d0d0d, %rax\nmovq %rax, %xmm13\npunpcklqdq %xmm13, %xmm13\n"
        "movabs $0x0e0e0e0e0e0e0e0e, %rax\nmovq %rax, %xmm14\npunpcklqdq %xmm14, %xmm14\n"
        "movabs $0x0f0f0f0f0f0f0f0f, %rax\nmovq %rax, %xmm15\npunpcklqdq %xmm15, %xmm15\n"
        "mov %r13, %rcx\nlea ident(%rip), %rdx\ncall *%r12\n"
        "xor %ebx, %ebx\n"
        "movabs $0x5151515151515151, %rax\ncmp %rax, %rsi\nje 1f\nor $1, %ebx\n1:\n"
        "movabs $0x7d7d7d7d7d7d7d7d, %rax\ncmp %rax, %rdi\nje 1f\nor $2, %ebx\n1:\n"
#define CHK(k, bit) "movabs $0x0" #k "0" #k "0" #k "0" #k "0" #k "0" #k "0" #k "0" #k ", %rax\n" \
                    "movq %xmm" #k ", %rcx\ncmp %rax, %rcx\njne 2f\n" \
                    "pshufd $0x4e, %xmm" #k ", %xmm0\nmovq %xmm0, %rcx\ncmp %rax, %rcx\nje 1f\n" \
                    "2:\nor $" #bit ", %ebx\n1:\n"
        CHK(6, 4) CHK(7, 8) CHK(8, 16) CHK(9, 32)
#define CHKX(k, c, bit) "movabs $0x" c c c c c c c c ", %rax\n" \
                    "movq %xmm" #k ", %rcx\ncmp %rax, %rcx\njne 2f\n" \
                    "pshufd $0x4e, %xmm" #k ", %xmm0\nmovq %xmm0, %rcx\ncmp %rax, %rcx\nje 1f\n" \
                    "2:\nor $" #bit ", %ebx\n1:\n"
        CHKX(10, "0a", 64) CHKX(11, "0b", 128) CHKX(12, "0c", 256)
        CHKX(13, "0d", 512) CHKX(14, "0e", 1024) CHKX(15, "0f", 2048)
        "mov %rbx, %rax\nadd $56, %rsp\n"
        "pop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n");
/* gcc's own Microsoft-convention variadic function, for EmbCC to call */
MS long long gvar(int n, ...)
{ __builtin_ms_va_list ap; long long t = 0; __builtin_ms_va_start(ap, n);
  for (int i = 0; i < n; i++)
      t = t * 10 + (i & 1 ? (long long)(__builtin_va_arg(ap, double) * 2)
                          : __builtin_va_arg(ap, int));
  __builtin_ms_va_end(ap); return t; }
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
T t8(void) { return FN(vints_t, vints)(7, 1, 2, 3, 4, 5, 6, 7) != 1234567; }
T t9(void) { return FN(vints_t, vmixd)(6, 1LL, 1.0, 3LL, 2.0, 5LL, 3.0) != 123456; }
T t10(void) { return FN(vints_t, vcopy)(6, 1, 2, 3, 4, 5, 6) != 123456123456LL; }
T t11(void) { return FN(callv_t, callv)(gvar, 3) != 3 * 1000 + 3 * 100 + 4 * 10 + 9; }
T t12(void) { return check((void *)FN(clob_t, clob), 5) != 0; }
T t13(void) { return FN(clob_t, clob)(5, ident) != 6901; }
struct s8 { int a, b; };
struct s12 { int a, b, c; };
T t14(void) { struct s8 s = { 1, 2 }; struct s12 t = { 0, 0, 3 };
              __int128 q = ((__int128)5 << 64) | 4;
              return FN(vints_t, vstruct)(9, s, t, q) != 9 + 10 + 200 + 3000 + 40000; }
int main(void)
{
    int bad = t1() | t2() << 1 | t3() << 2 | t4() << 3 | t5() << 4 |
              t6() << 5 | t7() << 6;
    int va = t8() | t9() << 1 | t10() << 2 | t11() << 3 | t14() << 4;
    int keep = t12() | t13() << 1;
    if (!bad && !va && keep)
        bad = 120 + keep;
    if (!bad && va)
        bad = 100 + va;
    return bad ? bad : 42;
}
E


for opt in -O0 -O1 -O2; do
    b=$out/O${opt#-O}; mkdir -p "$b"
    "$EMBCC" --target=x86_64-windows-gnu $opt -c "$out/callee.c" \
        -o "$b/c.obj" 2> "$b/cc.log" || {
        echo "FAIL $opt: embcc could not compile the callee:"
        grep -v 'warn=windows-abi' "$b/cc.log"; exit 1; }
    # (.pdata's relocations, against .text and .xdata, are the unwind
    # table's, which the test image does not need)
    if llvm-objdump -r -j .text "$b/c.obj" | grep -q IMAGE_REL; then
        echo "FAIL $opt: the callee has relocations; its bytes cannot be"
        echo "      moved into the test image as they are"; exit 1
    fi
    llvm-objcopy --dump-section .text="$b/c.bin" "$b/c.obj" "$b/junk.obj"
    defs=$(llvm-nm "$b/c.obj" |
           awk '$2 == "T" { printf " -DOFF_%s=0x%s", $3, $1 }')
    # shellcheck disable=SC2086
    x86_gcc_c "$out/caller.c" -o "$b/caller.o" -O1 -mno-red-zone \
        -DBLOB="\"$b/c.bin\"" $defs || {
        echo "FAIL $opt: the gcc caller does not compile"; exit 1; }
    x86_link "$b/run" "$b/caller.o" > "$b/ln.log" 2>&1 || {
        echo "FAIL $opt: the test image does not link:"; cat "$b/ln.log"
        exit 1; }
    x86_run "$b/run" > /dev/null 2>&1
    rc=$?
    [ "$rc" = 42 ] || {
        echo "FAIL $opt: exit $rc (a bit per wrong call, 42 is all right)"
        echo "      bits: 1 swap2, 2 rot3, 4 byref2, 8 byref5, 16 odd3,"
        echo "      32 fstack, 64 mixed; or 100 + bits: 1 vints, 2 vmixd,"
        echo "      4 vcopy, 8 callv; or 120 + bits: 1 a callee-saved"
        echo "      register lost across clob, 2 clob's value"; exit 1; }
done
echo "EmbCC's Win64 code at -O0, -O1 and -O2 takes its arguments where"
echo "gcc's ms_abi calls put them: float permutations, by-reference"
echo "aggregates in registers and on the stack, a 3-byte one, float"
echo "arguments past the fourth, variadic functions (va_list a pointer"
echo "into the home area, va_copy) -- and calls gcc's variadic one"
