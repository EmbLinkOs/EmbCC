// CX8, what the rest of libstdc++ needed: a union's destructor leaves its
// members alone (std::variant's storage); a namespace alias in a block;
// an alias template's requires-clause (substitution fails when it does
// not hold), and a parameter whose template parameters are all given
// explicitly taking the argument by conversion (nullptr); a member function and a member template of one signature
// are two functions (bitset::to_string); a partial specialization's
// members defined outside it are not the primary template's; a member
// function whose address is taken is instantiated for it; a class built
// only through a constructor template has its vtable's functions; and
// __builtin_source_location (std::source_location::current as a default
// argument: the caller's place).
// expect-exit: 42
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

// ---- a union's destructor ----
static int destroyed;
struct Noisy { int v; ~Noisy() { destroyed++; } };
union Slot {
    int i;
    Noisy n;
    Slot() : i(1) {}
    ~Slot() {}                        // the members are not destroyed
};

// ---- a namespace alias in a block ----
namespace outer { namespace inner { static int value() { return 7; } } }

// ---- alias template constraints ----
template <class T> concept is_class = __is_class(T);
template <class T> requires is_class<T> using member_t = typename T::type;
template <class T> constexpr int pick(member_t<T> *) { return 1; }
template <class T> constexpr int pick(...) { return 2; }
struct HasType { using type = int; };

// ---- a function and a template of one signature ----
struct Str {
    template <class C> int to() const { return (int)sizeof(C); }
    int to() const { return 100; }
};

// ---- a partial specialization's members defined outside ----
template <class T, class U> struct Box { int size() const; };
template <class U> struct Box<bool, U> { int size() const; };
template <class T, class U> int Box<T, U>::size() const { return (int)sizeof(T); }
template <class U> int Box<bool, U>::size() const { return 1000; }

// ---- a member function's address ----
template <class T> struct Scanner {
    int (Scanner::*step)() const;
    Scanner(bool b) : step(b ? &Scanner::ecma : &Scanner::posix) {}
    int run() const { return (this->*step)(); }
    int ecma() const;
    int posix() const;
};
template <class T> int Scanner<T>::ecma() const { return 11; }
template <class T> int Scanner<T>::posix() const { return 22; }

// ---- a vtable of a class built by a constructor template ----
struct Counted { virtual ~Counted() {} virtual int get() const = 0; };
template <class T> struct Inplace : Counted {
    T value;
    template <class... A> Inplace(A... a) : value(a...) {}
    int get() const override { return (int)value; }
};
static Counted *make(int v) { return new Inplace<int>(v); }

// ---- __builtin_source_location ----
namespace std {               // (what g++'s builtin wants declared)
struct source_location {
    struct __impl {
        const char *_M_file_name;
        const char *_M_function_name;
        unsigned _M_line;
        unsigned _M_column;
    };
};
}
struct Where { const char *file, *func; unsigned line, col; };
static unsigned line_of(const void *p = __builtin_source_location())
{
    return ((const Where *)p)->line;
}

int main()
{
    {
        Slot s;
        check("a union's destructor leaves its members", s.i == 1);
    }
    check("... (none destroyed)", destroyed == 0);
    namespace oi = outer::inner;
    check("a namespace alias in a block", oi::value() == 7);
    check("an alias template's requires-clause", pick<HasType>(nullptr) == 1 &&
                                                 pick<int>(nullptr) == 2);
    Str st;
    check("a function beside a template of its signature", st.to() == 100 &&
                                                           st.to<char>() == 1);
    Box<long, int> bl;
    Box<bool, int> bb;
    check("members of a partial specialization", bl.size() == 8 && bb.size() == 1000);
    check("a member function whose address is taken", Scanner<char>(true).run() == 11 &&
                                                      Scanner<char>(false).run() == 22);
    Counted *c = make(42);
    check("a vtable of a class built by a template", c->get() == 42);
    delete c;
    unsigned here = __LINE__ + 1;
    unsigned there = line_of();
    const Where *w = (const Where *)__builtin_source_location();
    check("__builtin_source_location", there == here && w->line == here + 1 &&
                                       strcmp(w->func, "int main()") == 0);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
