// CX2: new and delete through a class's own operator new/delete (static
// members, found before the global ones; `::new` skips them), for single
// objects and arrays, with the size the class's operator delete asks for.
// expect-exit: 42
#include <stdio.h>
#include <stdlib.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static int news, deletes, array_news, array_deletes;
static unsigned long last_size;

struct Pooled {
    int a, b;
    Pooled() : a(1), b(2) {}
    ~Pooled() { a = -1; }
    static void *operator new(unsigned long n)
    {
        news++;
        last_size = n;
        return malloc(n);
    }
    static void operator delete(void *p, unsigned long n)
    {
        deletes++;
        last_size = n;
        free(p);
    }
    void *operator new[](unsigned long n)        // static even unsaid
    {
        array_news++;
        return malloc(n);
    }
    void operator delete[](void *p)
    {
        array_deletes++;
        free(p);
    }
};

int main()
{
    Pooled *p = new Pooled;
    check("the class's operator new", news == 1 && last_size == sizeof(Pooled)
          && p->b == 2);
    delete p;
    check("the class's sized operator delete", deletes == 1 &&
          last_size == sizeof(Pooled));
    Pooled *arr = new Pooled[3];
    check("the class's operator new[]", array_news == 1 && arr[2].a == 1);
    delete[] arr;
    check("the class's operator delete[]", array_deletes == 1);
    Pooled *g = ::new Pooled;
    ::delete g;
    check("::new and ::delete skip them", news == 1 && deletes == 1);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
