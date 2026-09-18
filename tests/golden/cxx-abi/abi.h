// Declarations shared by the two halves of cxx-abi.sh: side A is compiled by
// embcc, side B by the reference g++, and each calls into the other — so
// every name below must mangle identically, and every class must have the
// same layout and be passed the same way, or the link or the run fails.
#ifndef ABI_H
#define ABI_H

namespace abi {

enum Color { Red, Green, Blue };
enum class Small : unsigned char { A = 1, B = 200 };

struct Pod {
    int a;
    long b;
    char c;
};

class Account {
public:
    Account(int start);
    Account(const char *name, int start);
    ~Account();
    void deposit(int n);
    int balance() const;
    static int open_count;
    static int opened();
    struct Entry {
        int amount;
        Entry *next;
    };
private:
    int total;
    const char *owner;
};

namespace detail {
int mix(int, long, unsigned, char, signed char, unsigned char);
int mix(short, unsigned short, long long, unsigned long long, bool);
double fl(float, double, long double);
int str(const char *, char *, const char *const *);
int refs(int &, const int &, int &&, Pod &, const Pod &);
int ptrs(int *, int **, const int *, int *const *, void *, const void *);
int same(Pod *, Pod *, Pod &, const Pod *);              // substitutions
int fn(int (*)(int), void (*)(), int (*)(Pod &, Pod &));
int arr(int (*)[4], int (&)[3]);
int en(Color, Small, Color *);
int nul(decltype(nullptr));
int chars(wchar_t, char16_t, char32_t, char8_t);
int var(int, ...);
int nested(Account::Entry *, Account::Entry &);
Pod make_pod(int);
long pod_sum(Pod);
}

int deep_call();                     // side B calls back into side A
int deep_call_b();                   // ... and reports what it saw
extern int counter;
extern const char *tag;

}

namespace std {
int std_name(int);                   // St, not N3std...E
}

int global_fn(abi::Pod *, abi::Pod *);
extern "C" int c_fn(int);

#endif
