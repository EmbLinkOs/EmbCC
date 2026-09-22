#!/bin/sh
# THE RULE, as a test: everything outside the subset must FAIL with a
# diagnostic naming the construct — never compile to something else.
# Each case asserts nonzero exit AND a message, because a bare nonzero
# could be any failure. Cases graduate OUT of this file as milestones
# implement them (if/while/for left when control flow landed).
set -u
echo "TEST-MARKER reject-unimplemented"

out_dir="tests/compile/out"
mkdir -p "$out_dir"

check() { # name source expected-message-grep
    src="$out_dir/$1.c"
    printf '%s\n' "$2" > "$src"
    # -Werror so that a case whose diagnostic is a WARNING still fails
    # the compile: an attribute EmbCC does not know is warned about,
    # not refused, and this file's whole shape is "it did not compile".
    if err=$("$EMBCC" -Werror -c "$src" -o "$out_dir/$1.o" 2>&1); then
        echo "case $1: compiled instead of failing"
        exit 1
    fi
    echo "$err" | grep -q "$3" || {
        echo "case $1: wrong diagnostic:"
        echo "$err"
        exit 1
    }
    echo "$err" | grep -q "$src:" || {
        echo "case $1: diagnostic has no file:line position:"
        echo "$err"
        exit 1
    }
    echo "case $1: refused with a diagnostic"
}

check case-outside-switch \
    'int main(void) { case 1: return 0; }' \
    "directly in its switch body"
check case-nested-in-block \
    'int main(void) { int x = 1; switch (x) { { case 1: return 1; } } return 0; }' \
    "directly in its switch body"
check duplicate-case \
    'int main(void) { int x = 1; switch (x) { case 2: break; case 2: break; } return 0; }' \
    "duplicate case label 2"
check two-defaults \
    'int main(void) { int x = 1; switch (x) { default: break; default: break; } return 0; }' \
    "only one .default."
check non-constant-case \
    'int main(void) { int x = 1; int y = 2; switch (x) { case y: break; } return 0; }' \
    "integer constant"
check switch-on-pointer \
    'int main(void) { int x; int *p = &x; switch (p) { case 1: break; } return 0; }' \
    "needs an integer"
check continue-in-switch-no-loop \
    'int main(void) { int x = 1; switch (x) { case 1: continue; } return 0; }' \
    "outside of a loop"
check cond-incompatible \
    'int main(void) { int x; int *p = &x; return 1 ? p : 5; }' \
    "incompatible"
check break-outside-loop \
    'int main(void) { break; return 0; }' \
    "outside of a loop"
check deref-non-pointer \
    'int main(void) { int x = 1; return *x; }' \
    "cannot dereference int"
check deref-void-ptr \
    'int main(void) { void *p = 0; return *p; }' \
    "cannot dereference void"
check ptr-plus-ptr \
    'int main(void) { int x; int *a = &x; int *b = &x; return !(a + b); }' \
    "cannot add two pointers"
check int-to-ptr-implicit \
    'int main(void) { int *p = 42; return !p; }' \
    "without a cast"
check ptr-int-compare \
    'int main(void) { int x; int *p = &x; return p == 42; }' \
    "needs a cast"
check incompatible-ptr-assign \
    'int main(void) { int x; char *p = &x; return !p; }' \
    "without a cast"
check compound-on-rvalue \
    'int main(void) { int x = 1; (x + 1) += 2; return x; }' \
    "must be a variable, \*pointer, or member"
check compound-ptr-mul \
    'int main(void) { int a[4]; int *p = a; p *= 2; return !p; }' \
    "only += and -= apply to a pointer"
check addr-of-rvalue \
    'int main(void) { int x = 1; return !&(x + 1); }' \
    "needs a variable"
check assign-to-function \
    'static int f(void) { return 1; }
int main(void) { f = 0; return f(); }' \
    "cannot assign to a function"
check call-non-function \
    'int main(void) { int x = 1; return x(); }' \
    "called object is not a function"
check fp-type-mismatch \
    'static int f(int x) { return x; }
int main(void) { long (*fp)(int) = f; return 0; }' \
    "without a cast"
check void-variable \
    'int main(void) { void v; return 0; }' \
    "cannot have type void"
check array-assign \
    'int main(void) { int a[3]; int b[3]; a = b; return 0; }' \
    "cannot assign to an array"
check array-scalar-init \
    'int main(void) { int a[3] = 0; return 0; }' \
    "brace initializer or a string"
check too-many-initializers \
    'int main(void) { int a[2] = {1,2,3}; return a[0]; }' \
    "past the end of an array of 2"
check non-char-array-from-string \
    'int main(void) { int a[4] = "abc"; return a[0]; }' \
    "element width matches"
check array-index-past-end \
    'int a[3] = { [5] = 1 };
int main(void) { return 0; }' \
    "past the end"
check field-designator-in-array \
    'int main(void) { int a[2] = { .x = 1 }; return a[0]; }' \
    "field designator '.x' in an array"
check member-dot-on-int \
    'int main(void) { int x = 1; return x.y; }' \
    "needs a struct/union, got int"
check varargs-too-few \
    'int printf(char *fmt, ...);
int main(void) { printf(); return 0; }' \
    "needs at least 1 argument"
check global-conflicting-types \
    'int g;
long g;
int main(void) { return g; }' \
    "conflicting types"
check global-two-inits \
    'int g = 1;
int g = 2;
int main(void) { return g; }' \
    "redefinition"
check global-nonconst-init \
    'int a = 1;
int b = a;
int main(void) { return b; }' \
    "must be a constant, a string literal, or the address of a global"
check extern-with-init \
    'extern int g = 5;
int main(void) { return g; }' \
    "'extern' with an initializer"
check ptr-global-bad-init \
    'int *p = 42;
int main(void) { return !p; }' \
    "cannot convert int to int . without a cast"
check global-use-before-decl \
    'int main(void) { return g; }
int g = 42;' \
    "used before its declaration"
check global-vs-function \
    'static int f(void) { return 1; }
int f;
int main(void) { return f(); }' \
    "both a function and a variable"
check unterminated-cond \
    '#ifdef NEVER
int main(void) { return 0; }' \
    "unterminated conditional"
check error-directive \
    '#error deliberately broken
int main(void) { return 0; }' \
    "deliberately broken"
check missing-include \
    '#include "no/such/file.h"
int main(void) { return 0; }' \
    "cannot find include"
check struct-scalar-init \
    'struct P { int x; };
int main(void) { struct P p = 1; return p.x; }' \
    "cannot convert"
check struct-assign-mismatch \
    'struct P { int x; };
struct Q { int x; };
int main(void) { struct P a; struct Q b; b.x = 1; a = b; return a.x; }' \
    "cannot assign"
check struct-arg-mismatch \
    'struct P { int x; };
struct Q { int x; };
static int f(struct P p) { return p.x; }
int main(void) { struct Q q; q.x = 1; return f(q); }' \
    "cannot convert"
check incomplete-var \
    'struct Later;
int main(void) { struct Later v; return 0; }' \
    "incomplete type"
check unknown-member \
    'struct P { int x; };
int main(void) { struct P p; p.x = 1; return p.z; }' \
    "no member 'z'"
check dot-on-pointer \
    'struct P { int x; };
int main(void) { struct P p; struct P *q = &p; return q.x; }' \
    "use '->' through a pointer"
check arrow-on-struct \
    'struct P { int x; };
int main(void) { struct P p; return p->x; }' \
    "needs a pointer"
check tag-redefinition \
    'struct P { int x; };
struct P { int y; };
int main(void) { return 0; }' \
    "redefinition of 'P'"
check empty-struct \
    'struct E { };
int main(void) { return 0; }' \
    "at least one member"
check typedef-redef \
    'typedef int T;
typedef long T;
int main(void) { T v = 1; return (int)v; }' \
    "redefinition of typedef"
# <stdio.h> used to be the case here, because EmbCC shipped no headers
# and found nothing without a -I. It ships them now and finds them
# relative to its own binary (src/driver/paths.c), so the header that
# must not resolve has to be one that genuinely does not exist. What is
# being checked is unchanged: a missing include is an error NAMING the
# file, not a silently empty translation unit.
check angle-include-no-path \
    '#include <no_such_header_exists_anywhere.h>
int main(void) { return 0; }' \
    "cannot find include file"
check float-modulo \
    'int main(void) { double a = 5.0; double b = 2.0; return (int)(a % b); }' \
    "needs an integer"
check float-bitand \
    'int main(void) { double a = 5.0; return (int)(a & 1); }' \
    "needs an integer"
check float-to-pointer \
    'int main(void) { double d = 1.0; int *p = (int *)d; return !p; }' \
    "cannot convert between"
check undefined-call \
    'int main(void) { return foo(); }' \
    "not declared"
check forward-call \
    'static int a(void) { return b(); }
static int b(void) { return 1; }
int main(void) { return a(); }' \
    "before its declaration"
check proto-arity-mismatch \
    'int f(int, int);
int f(int a) { return a; }
int main(void) { return f(1); }' \
    "conflicting declaration"
check proto-type-mismatch \
    'int f(int x);
char *f(int x) { return 0; }
int main(void) { return !f(1); }' \
    "conflicting declaration"
check static-never-defined \
    'static int ghost(void);
int main(void) { return ghost(); }' \
    "called but never defined"
check nonstatic-then-static \
    'int f(void);
static int f(void) { return 1; }
int main(void) { return f(); }' \
    "static declaration of 'f' follows non-static"
check unnamed-param-in-definition \
    'int f(int) { return 1; }
int main(void) { return f(1); }' \
    "needs a name in a definition"
check fallthrough \
    'int main(void) { int x = 1; }' \
    "must end in a return"
check fallthrough-if \
    'int main(void) { if (1) return 1; }' \
    "must end in a return"
check arity \
    'static int f(int a, int b) { return a + b; }
int main(void) { return f(1); }' \
    "needs 2 arguments, got 1"
check string-as-int \
    'int main(void) { return "x"; }' \
    "converting char \* to int"
check decl-as-if-body \
    'int main(void) { if (1) int x = 1; return 0; }' \
    "wrap it in braces"
check redeclare-same-block \
    'int main(void) { int x = 1; int x = 2; return x; }' \
    "already declared in this block"
check assign-to-literal \
    'int main(void) { 5 = 6; return 0; }' \
    "assignment target must be a variable"

# And without -c: linking does not exist until M3.
# `embcc prog.c -o prog` used to be refused outright -- the integrated
# linker was M3 and had not arrived. It links in-process now, so that
# case graduated out of this file, which is what the header above says
# happens. What is left is the part still refused, and it is refused
# for a reason that will not go away by itself: EmbLD reads x86-64 ELF,
# and an image for another machine is not something it can quietly
# approximate.
printf 'int main(void) { return 0; }\n' > "$out_dir/nolink.c"
if err=$("$EMBCC" --target=aarch64-elf "$out_dir/nolink.c" \
         -o "$out_dir/nolink.bin" 2>&1); then
    echo "case nolink: linked for a machine the linker cannot read"
    exit 1
fi
echo "$err" | grep -q "integrated linker reads x86-64 ELF" || {
    echo "case nolink: wrong diagnostic:"; echo "$err"; exit 1; }
echo "case nolink: linking for another machine is refused by name"
check asm-bad-constraint \
    'int main(void) { int x; __asm__("int $0x80" : "=t"(x)); return x; }' \
    "is not supported"
check asm-bad-template \
    'int main(void) { __asm__("vzeroall"); return 0; }' \
    "not supported"
check asm-out-not-lvalue \
    'int main(void) { __asm__("int $0x80" : "=a"(1 + 2)); return 0; }' \
    "must be an lvalue"
check topasm-bad-insn \
    'extern void f(void);
__asm__(".global g\ng:\n  frobnicate %rax\n");
int main(void) { return 0; }' \
    "not supported"
check topasm-bad-jmp \
    '__asm__("jmp nowhere\n");
int main(void) { return 0; }' \
    "not a local label"
check topasm-global-no-label \
    '__asm__(".global ghost\n  ret\n");
int main(void) { return 0; }' \
    "has no label"
check va-arg-struct \
    'typedef char *va_list;
struct P { int x; int y; };
int f(int n, ...) { va_list ap; __builtin_va_start(ap, n);
     struct P p = __builtin_va_arg(ap, struct P); __builtin_va_end(ap);
     return p.x; }
int main(void) { return 0; }' \
    "struct passed by value"
check static-assert-false \
    '_Static_assert(sizeof(int) == 8, "int is not eight bytes");
int main(void) { return 0; }' \
    "static assertion failed: int is not eight bytes"
check generic-no-match \
    'int main(void) { double d = 0; return _Generic(d, int: 1, long: 2); }' \
    "no _Generic association matches"
# VLAs are local variables and parameters only (C99 6.7.5.2p2, 6.7.8p3)
check vla-file-scope \
    'int n = 3;
int g[n];' \
    "variable length array can only be a local variable or a parameter"
check vla-struct-member \
    'int f(int n) { struct s { int a[n]; } x; (void)x; return 0; }' \
    "variable length array can only be a local variable or a parameter"
check vla-static \
    'int f(int n) { static int a[n]; return a[0]; }' \
    "static 'a' cannot have a variably modified type (int\[\*\])"
check vla-initialized \
    'int f(int n) { int a[n] = { 1 }; return a[0]; }' \
    "variable length array 'a' cannot be initialized"
# the C99 complex types are floating; GNU's integer ones are refused
check complex-int \
    'int main(void) { _Complex int z; return 0; }' \
    "only float, double and long double _Complex are supported"
check imaginary-int \
    'int main(void) { double _Complex z = 2i; return 0; }' \
    "integer imaginary constant"

# ---- attributes that change code generation ------------------------------
#
# An unknown attribute is SKIPPED, and that is right: always_inline,
# pure, hot and the rest are hints, and ignoring a hint is slow rather
# than wrong. These are not hints. Each changes the code that has to be
# generated, so a program that asked for one and did not get it
# compiles, links, runs, and does something else -- which is the one
# outcome THE RULE forbids. Each was silently ignored until 2026-09-21.
check attr-naked \
    '__attribute__((naked)) void f(void) { __asm__("nop"); }' \
    "attribute__((naked)) is not supported"
check attr-interrupt \
    '__attribute__((interrupt)) void f(void *p) { (void)p; }' \
    "attribute__((interrupt)) is not supported"
# The TRAILING spelling, which is the one EmbCC's declarator parser
# reaches; the leading one is refused earlier as an unexpected token.
check attr-cleanup \
    'void c(int *p); int f(void) { int x __attribute__((cleanup(c))) = 1; return x; }' \
    "attribute__((cleanup)) is not supported"
check attr-ms-abi \
    '__attribute__((ms_abi)) int f(int a, int b) { return a + b; }' \
    "attribute__((ms_abi)) is not supported"
# constructor/destructor ARE implemented -- but only without a priority,
# which orders the array in a way one .init_array in source order cannot
# express. Accepting the number and ignoring it would run them in the
# wrong order, which is the entire reason for writing one.
check attr-constructor-priority \
    '__attribute__((constructor(101))) static void f(void) { }' \
    "cannot honour a priority"

# An attribute EmbCC has never heard of is warned about and ignored,
# which is GCC's behaviour and the thing that would have caught
# __attribute__((constructor)) going unimplemented. -Werror makes the
# warning an error, which is what lets this file check it at all.
check attr-unknown \
    '__attribute__((no_such_attribute_anywhere)) int f(void) { return 0; }' \
    "is not one EmbCC knows"
