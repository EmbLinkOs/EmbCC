// Side A of cxx-abi.sh (compiled by embcc): defines the class and some of
// the functions; calls everything side B (g++) defines.
#include "abi.h"
#include <stdio.h>

namespace abi {

int Account::open_count = 0;

Account::Account(int start) : total(start), owner("anon") { open_count++; }
Account::Account(const char *name, int start) : total(start), owner(name)
{
    open_count++;
}
Account::~Account() { open_count--; }
void Account::deposit(int n) { total += n; }
int Account::balance() const { return total; }
int Account::opened() { return open_count; }

int counter = 40;
const char *tag = "side-a";

int deep_call() { return counter + 2; }

namespace detail {
int mix(int a, long b, unsigned c, char d, signed char e, unsigned char f)
{
    return a + (int)b + (int)c + d + e + f;
}
Pod make_pod(int k)
{
    Pod p = { k, k * 2L, (char)(k + 1) };
    return p;
}
}

}

extern "C" int c_fn(int x) { return x * 3; }

static int sq(int x) { return x * x; }
static void nothing() {}
static int podcmp(abi::Pod &a, abi::Pod &b) { return a.a - b.a; }

int main()
{
    using namespace abi;
    using namespace abi::detail;
    int fails = 0;
#define CHECK(what, cond) do { int ok_ = (cond); fails += !ok_; \
        printf("%s %s\n", ok_ ? "ok  " : "FAIL", what); } while (0)

    CHECK("mix #2 (g++)", mix((short)1, (unsigned short)2, 3LL, 4ULL, true) == 11);
    CHECK("floats", fl(1.5f, 2.5, 3.0L) == 7.0);
    const char *list[] = { "x", "yz" };
    char buf[4] = "abc";
    CHECK("strings", str("hello", buf, list) == 5 + 3 + 3);
    int i = 1, j = 2;
    Pod p = { 1, 2, 3 }, q = { 4, 5, 6 };
    CHECK("references", refs(i, j, 30, p, q) == 1 + 2 + 30 + 1 + 4 && i == 11);
    int *pi = &i;
    CHECK("pointers", ptrs(&i, &pi, &j, &pi, &j, &i) == 6);
    CHECK("substitutions", same(&p, &q, p, &q) == 1 + 4 + 1 + 4);
    CHECK("function pointers", fn(sq, nothing, podcmp) == 9 + 1 + (1 - 4));
    int a4[4] = { 1, 2, 3, 4 }, a3[3] = { 5, 6, 7 };
    CHECK("arrays", arr(&a4, a3) == 10 + 18);
    Color c = Blue;
    CHECK("enums", en(Green, Small::B, &c) == 1 + 200 + 2);
    CHECK("nullptr_t", nul(nullptr) == 42);
    CHECK("character types", chars(L'a', u'b', U'c', (char8_t)'d') ==
                             'a' + 'b' + 'c' + 'd');
    CHECK("variadic", var(3, 10, 20, 30) == 60);
    Account::Entry e2 = { 5, nullptr }, e1 = { 7, &e2 };
    CHECK("nested class", nested(&e1, e2) == 7 + 5 + 5);
    CHECK("class by value", pod_sum(make_pod(10)) == 10 + 20 + 11);
    CHECK("std::", std::std_name(41) == 42);
    CHECK("global namespace", global_fn(&p, &q) == 5);
    CHECK("g++ built Accounts with embcc's constructors", deep_call_b() == 41);
    CHECK("... and destroyed them", Account::opened() == 0);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
