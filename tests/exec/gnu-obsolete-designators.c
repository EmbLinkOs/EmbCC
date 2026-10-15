/* GNU's obsolete designators, `field: value` and `[i] value`, and the
 * address of a label declared with __label__ (refused before). */
// expect-exit: 42
int printf(const char *, ...);
struct pt { int x, y; };
struct pt p1 = { y: 2, x: 1 };
int arr[5] = { [3] 7, [1] = 4 };
static void *where;
static int lab(int n)
{
    __label__ here;
    where = &&here;
here:
    return n + 1;
}
int main(void)
{
    int ok = 0;
    ok += p1.x == 1 && p1.y == 2;
    ok += arr[3] == 7 && arr[1] == 4 && arr[0] == 0;
    ok += lab(41) == 42 && where != 0;
    printf("%d %d %d\n", ok, p1.y, arr[3]);
    return ok == 3 ? 42 : ok;
}
