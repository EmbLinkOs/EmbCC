/* One file-scope declaration that declares an object and then a function:
 * `extern int a, add(int, int), b;`. C11 6.7 lets every declarator of a
 * declaration be either, in any order. EmbCC took a function FIRST
 * (`int f(int), a;`) but not after an object: the declarator loop for
 * objects stopped at the name and the '(' was "expected ';'". Found by a
 * test that wrote `extern struct S s, k(struct S);`.
 */
// expect-exit: 42
extern int a, add(int, int), b;
static int c, *pick(int), d[3];

int add(int x, int y) { return x + y; }
static int *pick(int i) { return i ? &c : &d[1]; }

int a = 30, b = 5;

int main(void)
{
    *pick(1) = 4;
    *pick(0) = 3;
    return add(a, b) + c + d[1];
}
