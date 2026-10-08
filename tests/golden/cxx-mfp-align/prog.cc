struct A {
    int k;
    int a();
    int b();
};
int A::a() { return k + 1; }
int A::b() { return k + 2; }
__attribute__((noinline)) int call(A *p, int (A::*m)()) { return (p->*m)(); }
int main()
{
    A x;
    x.k = 10;
    return call(&x, &A::a) + call(&x, &A::b) == 11 + 12 ? 42 : 1;
}
