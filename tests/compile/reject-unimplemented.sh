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

check() { # name source expected-message-grep [extra flags]
    src="$out_dir/$1.c"
    printf '%s\n' "$2" > "$src"
    # -Werror so that a case whose diagnostic is a WARNING still fails
    # the compile: an attribute EmbCC does not know is warned about,
    # not refused, and this file's whole shape is "it did not compile".
    # shellcheck disable=SC2086
    if err=$("$EMBCC" -Werror ${4:-} -c "$src" -o "$out_dir/$1.o" 2>&1); then
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
    "'case' outside of a switch"
check default-outside-switch \
    'int main(void) { { default: return 0; } }' \
    "'default' outside of a switch"
# a label in a nested block is the switch's (tests/exec/case-nested.c), so
# its duplicates are found across the nesting too
check duplicate-case-nested \
    'int main(void) { int x = 1; switch (x) { case 1: { case 1: return 1; } } return 0; }' \
    "duplicate case label 1"
check two-defaults-nested \
    'int main(void) { int x = 1; switch (x) { default: if (x) { default: return 1; } } return 0; }' \
    "only one .default."
# an aligned typedef is the type's alignment (tests/exec/aligned-typedef.c);
# an array of one aligned beyond its size could not align its second
# element, and GCC and clang refuse it
check aligned-typedef-array \
    'typedef int A8 __attribute__((aligned(8))); A8 arr[3];' \
    "is not a multiple of its alignment"
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
# main alone may reach its closing brace (it returns 0, C99 5.1.2.2.3)
check fallthrough \
    'static int f(void) { int x = 1; }
int main(void) { return f(); }' \
    "must end in a return"
check fallthrough-if \
    'static int f(void) { if (1) return 1; }
int main(void) { return f(); }' \
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
# for a reason that will not go away by itself: a board image needs the
# board's memory map, which embld takes and the driver has no default
# for. The message once named "aarch64" for every such target -- a Thumb
# build was told it needed aarch64 -- so each target is named as itself.
printf 'int main(void) { return 0; }\n' > "$out_dir/nolink.c"
for t in aarch64-elf thumbv7em-none-eabi riscv32-unknown-elf mipsel-none-elf; do
    if err=$("$EMBCC" --target=$t "$out_dir/nolink.c" \
             -o "$out_dir/nolink.bin" 2>&1); then
        echo "case nolink $t: linked in one step without a memory map"
        exit 1
    fi
    # a board image needs the board's memory map, which has no default;
    # an AArch64 object is one embld does not read
    want="linking a $t image needs its memory map"
    [ "$t" = aarch64-elf ] &&
        want="cannot link for $t: embld does not read AArch64 objects"
    echo "$err" | grep -q "$want" || {
        echo "case nolink $t: wrong diagnostic:"; echo "$err"; exit 1; }
    # MIPS (like AVR) has no linker-script layout in embld: not offered
    if [ "$t" = mipsel-none-elf ] && echo "$err" | grep -q 'linker script'; then
        echo "case nolink $t: offered a linker script embld refuses:"; echo "$err"; exit 1
    fi
done
echo "case nolink: a board image with no memory map, or an AArch64 one, is refused by name"

# C++ follows the target's data model and C++ ABI on every 32-bit target
# but AVR (tests/golden/cxx-embedded.sh runs it on ARM, RV32, MIPS, Xtensa,
# TriCore and the rest); on AVR -- two-byte int and pointers, which the C++
# lowering does not handle -- code generation is refused by name, except
# for a check that writes nothing. Elsewhere exceptions are refused by
# name: there are no unwind tables for them.
printf 'long f(long x) { return x + (long)sizeof(long); }\n' > "$out_dir/ilp.cpp"
for t in avr; do
    if err=$("$EMBCC" --target=$t -fno-exceptions -c "$out_dir/ilp.cpp" \
             -o "$out_dir/ilp.o" 2>&1); then
        echo "case cxx-not-lp64 $t: compiled C++ for an unchecked C++ ABI"; exit 1
    fi
    echo "$err" | grep -q "C++ is not yet supported for $t" || {
        echo "case cxx-not-lp64 $t: wrong diagnostic:"; echo "$err"; exit 1; }
done
for t in thumbv7em-none-eabi riscv32-unknown-elf mipsel-none-elf \
         xtensa-none-elf tricore-none-elf; do
    if err=$("$EMBCC" --target=$t -c "$out_dir/ilp.cpp" \
             -o "$out_dir/ilp.o" 2>&1); then
        echo "case cxx-not-lp64 $t: compiled C++ with exceptions"; exit 1
    fi
    echo "$err" | grep -q "C++ exceptions are not supported for $t" || {
        echo "case cxx-not-lp64 $t: wrong diagnostic:"; echo "$err"; exit 1; }
    "$EMBCC" --target=$t -fno-exceptions -c "$out_dir/ilp.cpp" \
        -o "$out_dir/ilp.o" || {
        echo "case cxx-not-lp64 $t: C++ with -fno-exceptions was refused"
        exit 1; }
done
"$EMBCC" --target=avr -fsyntax-only "$out_dir/ilp.cpp" || {
    echo "case cxx-not-lp64: -fsyntax-only, which writes nothing, was refused"
    exit 1; }
echo "case cxx-not-lp64: C++ on AVR, and exceptions on the other 32-bit targets, are refused by name"

# File-scope asm: AArch64's blocks are read by the x86-64 vocabulary's
# assembler (src/arch/x86_64/topasm.c), so an instruction in one is
# refused by name; `ret` once became 0xc3 there. The embedded targets'
# blocks are read by their own assembler (src/as/gas.c), so `ret` is
# THEIR ret -- RISC-V's and AVR's -- and on Thumb, which spells it
# `bx lr`, it is refused by that assembler. A block written as data
# assembles everywhere.
printf '__asm__(".globl f\\nf:\\n ret\\n");\nint g(void) { return 1; }\n' \
    > "$out_dir/topasm.c"
printf '__asm__(".globl tbl\\ntbl:\\n .long 1\\n");\nint g(void) { return 1; }\n' \
    > "$out_dir/topdata.c"
for t in aarch64-elf thumbv7em-none-eabi riscv32-unknown-elf riscv64-unknown-elf avr; do
    case $t in
    aarch64*) want='file-scope asm instruction "ret"' ;;
    thumb*) want='"ret" is not in the ARMv7-M vocabulary' ;;
    *) want= ;;
    esac
    if [ -n "$want" ]; then
        if err=$("$EMBCC" --target=$t -c "$out_dir/topasm.c" \
                 -o "$out_dir/topasm.o" 2>&1); then
            echo "case topasm $t: an instruction of another machine went into the object"
            exit 1
        fi
        echo "$err" | grep -q "$want" || {
            echo "case topasm $t: wrong diagnostic:"; echo "$err"; exit 1; }
    else
        "$EMBCC" --target=$t -c "$out_dir/topasm.c" -o "$out_dir/topasm.o" || {
            echo "case topasm $t: the target's own ret was refused"; exit 1; }
        # the bytes at f: RISC-V's ret (jalr x0, 0(ra), or c.jr ra with
        # the C extension), AVR's ret
        b=$(llvm-objdump -d --no-show-raw-insn "$out_dir/topasm.o" 2>/dev/null |
            sed -n '/<f>:/,$p' | sed -n 2p)
        echo "$b" | grep -Eq '(ret|jr[[:space:]]+ra)$' || {
            echo "case topasm $t: f is not the target's ret: $b"; exit 1; }
    fi
    "$EMBCC" --target=$t -c "$out_dir/topdata.c" -o "$out_dir/topdata.o" || {
        echo "case topasm $t: a data-only block was refused"; exit 1; }
done
echo "case topasm: a block's instructions are the target's, or refused by name"
check generic-duplicate-type \
    'int main(void) { return _Generic(1, int: 1, int: 2, default: 3); }' \
    "more than one _Generic association matches"
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
check va-arg-struct-windows \
    'typedef char *va_list;
struct P { int x; int y; };
int f(int n, ...) { va_list ap; __builtin_va_start(ap, n);
     struct P p = __builtin_va_arg(ap, struct P); __builtin_va_end(ap);
     return p.x; }
int main(void) { return 0; }' \
    "va_arg of a struct is not supported for a Windows target" \
    --target=x86_64-windows-gnu
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
# constructor(N) is honoured (tests/golden/ctor-priority.sh); a priority
# past GCC's range is not one
check attr-constructor-priority \
    '__attribute__((constructor(70000))) static void f(void) { }' \
    "a priority is 0 to 65535"

# An attribute EmbCC has never heard of is warned about and ignored,
# which is GCC's behaviour and the thing that would have caught
# __attribute__((constructor)) going unimplemented. -Werror makes the
# warning an error, which is what lets this file check it at all.
check attr-unknown \
    '__attribute__((no_such_attribute_anywhere)) int f(void) { return 0; }' \
    "is not one EmbCC knows"

# A block-scope function declaration must agree with the unit's: calls
# use the unit's, so a different one would be silently overruled.
check block-fn-conflict \
    'int g(void) { extern long f(int); return (int)f(1); } int f(int x) { return x; }' \
    "conflicting declaration of 'f'"
check block-fn-redeclared \
    'int g(void) { int f = 1; extern int f(int); return f; } int f(int x) { return x; }' \
    "redeclared as a different kind of symbol"

# An unsized array's size, when its initializer leaves braces out, is
# worked out at parse time from the syntax (sizeof may fold it there)
# and again in sema from the types. Here the syntax cannot tell that
# `1 ? 2 : 3` is an int and not a struct value: rather than give the
# object one size and sizeof another, the declaration is refused.
check elision-size-guess \
    'struct pt { int x, y; }; static struct pt g[] = { 1 ? 2 : 3, 4 }; int main(void) { return sizeof g; }' \
    "cannot size 'g' from its initializer"
# A union initializer takes one member; a second one is not stored over
# it (it was, at the same offset).
check union-excess-init \
    'union u { int i; int j; }; int main(void) { union u v = { 1, 2 }; return v.i; }' \
    "a union takes one"
# `[i] =` names an array element; in a struct's list it was taken as
# positional and stored into the first member.
check array-designator-in-struct \
    'struct s { int a, b; }; struct s x = { [1] = 2 };' \
    "array designator"

# UTF-8 is read in identifiers, for the letters C11 Annex D lists; any
# other non-ASCII character outside a literal is named, not skipped.
check non-ascii-operator \
    "int main(void) { int a = 2 $(printf '\303\227') 3; return a; }" \
    "is not part of a character C allows"

# An automatic object has no room past its type's size, so its flexible
# array member cannot be initialized (gcc refuses it too); the elements
# were stored past the object, over the rest of the frame.
check fam-init-automatic \
    'struct fam { int n; int d[]; }; int main(void) { struct fam l = { 3, { 10 } }; return l.n; }' \
    "flexible array member 'd' cannot be initialized"
check fam-init-compound-literal \
    'struct fam { int n; int d[]; }; int main(void) { struct fam *p = &(struct fam){ 3, { 10 } }; return p->n; }' \
    "flexible array member 'd' cannot be initialized"

# A GNU range designator with more steps after it, `[0 ... 1][1] = 5`,
# would have to repeat a path; it is refused rather than guessed.
check desig-range-path \
    'int m[3][2] = { [0 ... 1][1] = 5 };' \
    "range designator"

# One level of braces around a scalar (C11 6.7.9p11); gcc refuses more.
check scalar-double-braces \
    'int main(void) { int q = { { 4 } }; return q; }' \
    "take one level"

# #pragma pack is honoured; the forms it is not are refused by name: a
# labelled push or pop (gcc's identifier argument, which a macro there
# also is -- gcc does not expand them), a change inside a struct body, a
# pop with no push, and a value that is not 1, 2, 4, 8 or 16.
check pack-push-label \
    '#pragma pack(push, hdrs, 1)
struct s { char c; int i; };' \
    "pack(push, name) is not supported"
check pack-in-struct \
    'struct s { char c;
#pragma pack(1)
int i; };' \
    "inside a struct body is not supported"
check pack-pop-unmatched \
    '#pragma pack(pop)
struct s { char c; int i; };' \
    "without a matching push"
check pack-bad-value \
    '#pragma pack(3)
struct s { char c; int i; };' \
    "wants 1, 2, 4, 8 or 16"

# C23 constexpr takes an exactly representable integer constant; EmbCC
# keeps integer ones, and says so for the rest.
check constexpr-not-exact \
    'constexpr unsigned char c = 300;' \
    "does not fit"
check constexpr-floating \
    'constexpr double d = 1.5;' \
    "takes integer constants"

# A function in a section of its own is laid out apart from .text, and a
# data object cannot share a section that holds code. (-g with one is
# supported: tests/golden/gc-sections.sh.)
check fn-section-shared-with-data \
    '__attribute__((section(".ramfunc"))) int f(void) { return 1; }
__attribute__((section(".ramfunc"))) int v = 2;' \
    "cannot share it"

# Enumerators past what any integer type holds. gcc refuses these
# ("overflow in enumeration values"); clang warns and wraps, and EmbCC
# wrapped without a word.
check enum-past-llong-max \
    'enum { A = 0x7fffffffffffffff, B };' \
    "one past LLONG_MAX"
check enum-past-ullong-max \
    'enum { A = ~0ULL, B };' \
    "one past ULLONG_MAX"
check enum-negative-and-huge \
    'enum { A = -1, B = 0x8000000000000000ULL };' \
    "no integer type holds"
check enum-fixed-type-range \
    'enum e : unsigned char { A = 255, B };' \
    "cannot represent"
check constexpr-ullong-max-in-long-long \
    'constexpr long long k = 0xffffffffffffffffULL;' \
    "does not fit"

# Enumerators and constexprs share one list per unit, which sema reads
# after the locals and before the globals. Where that would resolve a
# name to the wrong declaration, the program is refused instead.
check enum-hides-local \
    'int f(void) { int N = 5; { enum { N = 3 }; return N; } }' \
    "would hide the local"
check block-enum-names-global \
    'int f(void) { enum { N = 3 }; return N; }
int N = 7;
int g(void) { return N; }' \
    "declared in a function"
check enum-redeclares-function \
    'enum { g = 1 };
int g(void) { return 0; }' \
    "different kind of symbol"
check constexpr-shadows-enumerator \
    'enum { A = 1 };
int f(void) { constexpr int A = 2; return A; }' \
    "already a named constant"

# A const object, or one reached through a pointer to const, is not
# assigned: C11 6.5.16p2 makes each of these a constraint violation.
# const was not in the type, and every one compiled -- the file-scope
# ones into a store to .rodata.
check const-assign 'const int k = 1; void f(void) { k = 2; }' "read-only 'k'"
check const-through-pointer \
    'void f(const char *s) { *s = 1; }' "read-only location"
check const-pointer-itself \
    'char b[2]; char *const p = b; void f(void) { p = 0; }' "read-only 'p'"
check const-member \
    'struct S { const int a; }; void f(struct S *s) { s->a = 1; }' "read-only 'a'"
check const-struct-member \
    'struct T { int a; }; const struct T t = { 1 }; void f(void) { t.a = 2; }' \
    "read-only 'a'"
check const-member-whole \
    'struct S { const int a; }; void f(struct S *x, struct S *y) { *x = *y; }' \
    "has a const member"
check const-array-element \
    'const int arr[2] = { 1, 2 }; void f(void) { arr[0] = 3; }' "read-only location"
check const-increment 'void f(void) { const int x = 1; x++; }' "increment of read-only"
check const-compound 'void f(void) { const int x = 1; x += 2; }' "read-only 'x'"
check const-asm-output \
    'void f(void) { const int x = 1; __asm__("" : "=r"(x)); }' "an asm output of read-only"
# a parameter `int a[const 4]` is `int *const a`
check const-array-parameter \
    'int g(int a[const 4]) { a = 0; return 0; }' "read-only 'a'"
# the storage classes a parameter cannot have, and a bound's `static`
# where it is not a parameter's
check parameter-storage-class 'int f(static int x);' "no storage class but 'register'"
check parameter-storage-class-late 'int f(int extern x);' "no storage class but 'register'"
check array-static-not-parameter \
    'void f(void) { int a[static 3]; (void)a; }' "expected an expression, got 'static'"

# The COFF writer has no named data sections. A variable with one was
# put at offset 0 of .data, on top of the first variable there.
check coff-section-variable \
    'int a = 1; int b __attribute__((section(".mydata"))) = 2;' \
    "variable's section attribute is not supported for COFF" \
    --target=x86_64-windows-gnu

# The frame builtins above level 0 are refused by NAME where the code
# keeps no frame chain (level 0 works everywhere: tests/exec/
# frame-builtins.c). On RISC-V and AVR the eight-byte check once met them
# first (their w is a host pointer's) and said "this operation at 64 bits".
for t in riscv32-unknown-elf riscv64-unknown-elf avr; do
    check "frame-builtins-$t" \
        'void *f(void) { return __builtin_frame_address(1); }
void *g(void) { return __builtin_return_address(1); }
int main(void) { return 0; }' \
        "__builtin_frame_address(1) is not supported on" \
        "--target=$t"
done
