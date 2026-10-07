// Classes, constructors and destructors, references, namespaces and
// operator overloading: the C++ an RTOS wrapper is made of. Each line
// printed is one property; the host's clang++ build prints the same.
#include <stdio.h>

namespace rtos {
namespace detail {
int created, destroyed;
}

// A lock guard: RAII, the reason C++ is wanted at all.
class Lock {
public:
    explicit Lock(int &owner, int id) : owner_(owner), prev_(owner) {
        owner_ = id;
        detail::created++;
        printf("lock %d (was %d)\n", id, prev_);
    }
    ~Lock() {
        printf("unlock %d\n", owner_);
        owner_ = prev_;
        detail::destroyed++;
    }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
private:
    int &owner_;
    int prev_;
};

// A fixed-capacity queue, as a message queue wrapper would hold.
template <typename T, int N>
class Queue {
public:
    bool push(const T &v) {
        if (n_ == N)
            return false;
        buf_[(head_ + n_++) % N] = v;
        return true;
    }
    bool pop(T &out) {
        if (n_ == 0)
            return false;
        out = buf_[head_];
        head_ = (head_ + 1) % N;
        n_--;
        return true;
    }
    int size() const { return n_; }
private:
    T buf_[N];
    int head_ = 0, n_ = 0;
};
}  // namespace rtos

struct Vec {
    long x, y;
    Vec(long a = 0, long b = 0) : x(a), y(b) {}
    Vec operator+(const Vec &o) const { return Vec(x + o.x, y + o.y); }
    Vec operator-() const { return Vec(-x, -y); }
    Vec &operator+=(const Vec &o) { x += o.x; y += o.y; return *this; }
    bool operator==(const Vec &o) const { return x == o.x && y == o.y; }
    long operator[](int i) const { return i ? y : x; }
    explicit operator bool() const { return x || y; }
};

Vec operator*(long k, const Vec &v) { return Vec(k * v.x, k * v.y); }

struct Counter {
    static int live;
    int id;
    Counter(int i) : id(i) { live++; printf("Counter(%d) live=%d\n", id, live); }
    Counter(const Counter &o) : id(o.id + 100) { live++; printf("copy %d\n", id); }
    Counter(Counter &&o) : id(o.id + 1000) { o.id = -1; live++; printf("move %d\n", id); }
    ~Counter() { live--; printf("~Counter(%d) live=%d\n", id, live); }
};
int Counter::live;

Counter make(int i) { return Counter(i); }

void swap_refs(int &a, int &b) { int t = a; a = b; b = t; }
const long &bigger(const long &a, const long &b) { return a > b ? a : b; }

struct Bits {
    unsigned ready : 1;
    unsigned prio : 5;
    unsigned count : 10;
    signed delta : 6;
};

int main()
{
    int owner = 0;
    {
        rtos::Lock a(owner, 1);
        {
            rtos::Lock b(owner, 2);
            printf("owner %d\n", owner);
        }
        printf("owner %d\n", owner);
    }
    printf("owner %d created %d destroyed %d\n", owner,
           rtos::detail::created, rtos::detail::destroyed);

    rtos::Queue<long, 4> q;
    for (long i = 1; i <= 5; i++)
        printf("push %ld %d\n", i * 10, q.push(i * 10));
    long v;
    while (q.pop(v))
        printf("pop %ld size %d\n", v, q.size());

    Vec a(1, 2), b(30, 40);
    Vec c = a + b;
    c += -a;
    Vec d = 3L * c;
    printf("vec %ld %ld eq %d idx %ld %ld bool %d %d\n", d.x, d.y,
           c == Vec(30, 40), d[0], d[1], (bool)d, (bool)Vec());

    {
        Counter x(1);
        Counter y = make(2);
        Counter z(x);
        Counter w(static_cast<Counter &&>(y));
        printf("ids %d %d %d %d live %d\n", x.id, y.id, z.id, w.id,
               Counter::live);
    }
    printf("live %d\n", Counter::live);

    int i = 3, j = 4;
    swap_refs(i, j);
    long p = 7, r = 9;
    printf("swap %d %d bigger %ld\n", i, j, bigger(p, r));

    Bits bits = {};
    bits.ready = 1;
    bits.prio = 17;
    bits.count = 1000;
    bits.delta = -5;
    printf("bits %u %u %u %d\n", bits.ready, bits.prio, bits.count,
           (int)bits.delta);

    printf("==END==\n");
    fflush(stdout);
    return 0;
}
