// Bit-fields in classes the Itanium way when C cannot say it: after a
// base with data (non-POD: its tail padding used), in a dynamic class
// (after the vptr), after an empty base — laid out as g++ lays them out
// (the bytes printed, compared with g++'s build), fields crossing their
// type's unit, signed and bool ones, and ++/+= on them.
// expect-exit: 42
#include <stdio.h>
#include <string.h>
struct Base { int id = 7; char tag = 'b'; Base() {} };      // non-POD: tail padding reusable
struct D1 : Base { unsigned a : 3; unsigned b : 13; int s : 9; bool f : 1; };
struct V { virtual ~V() {} virtual int get() const { return 1; } };
struct D2 : V { unsigned short x : 5; unsigned y : 20; long long z : 40; };
struct E {};
struct D3 : E { unsigned a : 4; unsigned b : 4; };
template <class T> static void dump(const char *n, const T &v)
{
    unsigned char raw[64];
    memcpy(raw, (const void *)&v, sizeof v);
    printf("%s %zu:", n, sizeof v);
    for (size_t i = 0; i < sizeof v; i++)
        if (!(i < 8 && __is_polymorphic(T)))          // (not the vptr)
            printf(" %02x", raw[i]);
    printf("\n");
}
int main()
{
    D1 d1;
    memset((char *)&d1 + 5, 0, sizeof d1 - 5);
    d1.a = 5; d1.b = 4000; d1.s = -100; d1.f = true;
    D2 d2;
    memset((char *)&d2 + 8, 0, sizeof d2 - 8);
    d2.x = 17; d2.y = 999999; d2.z = -123456789012LL;
    D3 d3;
    memset((void *)&d3, 0, sizeof d3);
    d3.a = 3; d3.b = 12;
    dump("D1", d1);
    dump("D2", d2);
    dump("D3", d3);
    d1.b += 100;
    d2.y++;
    printf("%u %u %d %d | %u %u %lld | %u %u\n", d1.a, d1.b, d1.s, (int)d1.f, d2.x, d2.y, (long long)d2.z,
           d3.a, d3.b);
    return d1.b == 4100 && d1.s == -100 && d2.z == -123456789012LL ? 42 : 1;
}
