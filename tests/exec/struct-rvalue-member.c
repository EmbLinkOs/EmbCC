// A member of a struct value that is not an lvalue: a call's result (in
// registers, and through memory), `?:`, a comma expression, an assignment
// — C11 6.5.2.3 allows `f().m`.
// expect-exit: 42
struct pair { int *at; int *fn; };
struct small { int a, b; };
struct big { long a, b, c, d; };

static int x = 5, y = 7;
static struct pair get(void) { struct pair p = { &x, &y }; return p; }
static struct small gets(int k) { struct small s = { k, k * 2 }; return s; }
static struct big getb(long k) { struct big b = { k, k + 1, k + 2, k + 3 }; return b; }

int main(void)
{
    int fails = 0;
    fails += *get().fn != 7 || *get().at != 5;
    fails += gets(4).b != 8 || gets(3).a != 3;
    fails += getb(10).c != 12 || getb(1).d != 4;
    struct small s1 = { 1, 2 }, s2 = { 3, 4 }, s3;
    int k = 0;
    fails += (k ? s1 : s2).a != 3 || (!k ? s1 : s2).b != 2;
    fails += (k++, s2).b != 4 || k != 1;
    fails += (s3 = s1).b != 2 || s3.a != 1;
    long sum = 0;
    for (int i = 0; i < 3; i++)
        sum += getb(i).b + gets(i).a;
    fails += sum != (1 + 2 + 3) + (0 + 1 + 2);
    return fails ? fails : 42;
}
