#!/bin/sh
# C++ programs that are ill-formed must be refused with a located
# diagnostic naming the problem — never compiled to something that runs
# (a deleted function called, a coroutine misused).
set -u
echo "TEST-MARKER cxx-reject"
. "$(dirname "$0")/../lib.sh"

out=$EMBCC_ROOT/tests/golden/out/cxx-reject-$ARCH
rm -rf "$out"; mkdir -p "$out"

check() { # name source expected-message-grep
    src="$out/$1.cc"
    printf '%s\n' "$2" > "$src"
    if err=$("$EMBCC" --target="$TARGET" -c "$src" -o "$out/$1.o" 2>&1); then
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

# a move constructor declared: the implicit copy assignment is deleted
# (and there is no implicit move assignment) — even for a class whose
# assignment would otherwise copy its bytes
check deleted-copy-assign \
    'struct A { A() {} A(A &&) {} };
int main() { A a, b; a = static_cast<A &&>(b); return 0; }' \
    "use of deleted function 'operator='"
check coroutine-return \
    'namespace std {
template <class R, class... A> struct coroutine_traits { using promise_type = typename R::promise_type; };
template <class P = void> struct coroutine_handle { static coroutine_handle from_address(void *) { return {}; } };
struct suspend_never { bool await_ready() { return true; } template <class H> void await_suspend(H) {} void await_resume() {} };
}
struct T {
    struct promise_type {
        T get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};
T f() { co_return; return T(); }' \
    "a coroutine returns with co_return"
check coroutine-main \
    'int main() { co_return; }' \
    "main cannot be a coroutine"
check coroutine-no-traits \
    'struct T {};
T f() { co_return; }' \
    "std::coroutine_traits is not declared"
check coroutine-no-promise \
    'namespace std { template <class R, class... A> struct coroutine_traits {}; }
int f() { co_return 1; }' \
    "has no promise_type"
# consteval (7.7): each call of an immediate function outside another must
# be a constant expression — said why when it is not
check consteval-runtime-arg \
    'consteval int sq(int x) { return x * x; }
int f(int n) { return sq(n); }' \
    "call to consteval function 'sq' is not a constant expression: the value of 'n' is not a constant"
check consteval-div-zero \
    'consteval int quot(int a, int b) { return a / b; }
int g() { return quot(1, 0); }' \
    "not a constant expression: a division by zero"
check consteval-throw \
    'consteval int pos(int x) { if (x < 0) throw 1; return x; }
int g() { return pos(-2); }' \
    "not a constant expression: an exception is thrown"
check consteval-not-constexpr \
    'void fail(const char *);
consteval int pos(int x) { if (x < 0) fail("negative"); return x; }
int g() { return pos(-2); }' \
    "'fail' is called, which is not constexpr: \"negative\""
check consteval-global \
    'consteval int one(int *p) { return *p; }
int x = 1;
int y = one(&x);' \
    "an object that is not a constant is read"
