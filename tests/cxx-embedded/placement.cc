// Placement new into static storage (an RTOS's object pools: no heap),
// explicit destructor calls, new and delete of objects and arrays over
// malloc (array cookies: the count destroyed is the count built), and
// aligned storage. Each line printed is one property; the host's clang++
// build prints the same.
#include <stdio.h>
#include <new>

static int built, destroyed;

struct Tcb {
    int id;
    char name[8];
    long stack_top;
    Tcb(int i) : id(i), stack_top(1000L * i) {
        for (int k = 0; k < 7; k++)
            name[k] = (char)('a' + (i + k) % 26);
        name[7] = 0;
        built++;
    }
    ~Tcb() { destroyed++; }
};

// A pool: storage for N objects, constructed in place on demand.
template <typename T, int N>
class Pool {
    alignas(T) unsigned char store_[N][sizeof(T)];
    bool used_[N] = {};
public:
    template <typename... A> T *make(A... a) {
        for (int i = 0; i < N; i++)
            if (!used_[i]) {
                used_[i] = true;
                return new (store_[i]) T(a...);
            }
        return nullptr;
    }
    void free(T *p) {
        for (int i = 0; i < N; i++)
            if (reinterpret_cast<T *>(store_[i]) == p) {
                p->~T();
                used_[i] = false;
            }
    }
    int slot(T *p) const {
        for (int i = 0; i < N; i++)
            if (reinterpret_cast<const T *>(store_[i]) == p)
                return i;
        return -1;
    }
};

static Pool<Tcb, 3> pool;

struct alignas(16) Vec4 { float v[4]; };

struct Elem {
    static int alive;
    int v;
    Elem() : v(alive++) {}
    ~Elem() { alive--; }
};
int Elem::alive;

struct Wide {
    long long x;
    Wide() : x(7) {}
    ~Wide() { x = 0; }
};

int main()
{
    Tcb *a = pool.make(1), *b = pool.make(2), *c = pool.make(3);
    printf("pool %d %d %d full %d\n", pool.slot(a), pool.slot(b), pool.slot(c),
           pool.make(4) == nullptr);
    printf("tcb %d %s %ld\n", b->id, b->name, b->stack_top);
    pool.free(b);
    Tcb *d = pool.make(9);
    printf("reused %d id %d name %s built %d destroyed %d\n", pool.slot(d),
           d->id, d->name, built, destroyed);

    alignas(Tcb) unsigned char raw[sizeof(Tcb)];
    Tcb *e = new (raw) Tcb(5);
    printf("raw %d same %d\n", e->id, (void *)e == (void *)raw);
    e->~Tcb();

    Tcb *h = new Tcb(11);
    printf("heap %d %s\n", h->id, h->name);
    delete h;

    Elem *arr = new Elem[5];
    printf("array alive %d values %d %d\n", Elem::alive, arr[0].v, arr[4].v);
    delete[] arr;
    printf("after delete[] alive %d\n", Elem::alive);
    Wide *w = new Wide[3];
    printf("wide %lld %lld\n", w[0].x, w[2].x);
    delete[] w;
    int *ints = new int[4]();
    ints[3] = 8;
    printf("ints %d %d\n", ints[0], ints[3]);
    delete[] ints;

    Vec4 *v = new Vec4;
    v->v[2] = 2.5f;
    printf("vec4 aligned %d %.1f\n",
           (int)(reinterpret_cast<unsigned long>(v) % alignof(Vec4) == 0 ||
                 alignof(Vec4) > __STDCPP_DEFAULT_NEW_ALIGNMENT__),
           v->v[2]);
    delete v;
    printf("built %d destroyed %d\n", built, destroyed);
    printf("==END==\n");
    fflush(stdout);
    return 0;
}
