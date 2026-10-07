// The guard of a function-local static, seen from the runtime's side: the
// program defines the three guard functions itself (so the runtime's are
// not linked) and counts the calls. The compiler's inline test must read
// the guard the way the ABI says the runtime marks it -- the first byte
// non-zero (Itanium), bit 0 set (ARM) -- so that once an object is built
// its function never calls the runtime again. A test that reads the guard
// any other way still works, through acquire every time; only the count
// shows it. Each line printed is one property; the host's clang++ build
// prints the same.
#include <stdio.h>

static int acquires, releases;

// The guard's type: 32 bits on ARM (the ARM C++ ABI), 64 elsewhere.
#ifdef __ARM_EABI__
typedef int guard_type;
#else
typedef long long guard_type;
#endif

extern "C" int __cxa_guard_acquire(guard_type *g)
{
    acquires++;
    return *reinterpret_cast<unsigned char *>(g) == 0;  // 1: initialize
}

extern "C" void __cxa_guard_release(guard_type *g)
{
    releases++;
    *reinterpret_cast<unsigned char *>(g) = 1;          // bit 0 of byte 0
}

extern "C" void __cxa_guard_abort(guard_type *) {}

static int made;

struct Device {
    int id;
    explicit Device(int i) : id(i) { made++; }
};

int next_id() { return 40 + made; }

Device &device()
{
    static Device d(next_id());       // a constructor: guarded
    return d;
}

int &counter()
{
    static int n = next_id() * 2;     // a dynamic scalar: guarded
    return n;
}

inline int &shared()
{
    static int s = next_id() + 100;   // an inline function's: guarded too
    return s;
}

int main()
{
    int sum = 0;
    for (int i = 0; i < 5; i++)
        sum += device().id + counter()++ + shared();
    printf("sum %d made %d\n", sum, made);
    printf("acquires %d releases %d\n", acquires, releases);
    printf("==END==\n");
    fflush(stdout);
    return 0;
}
