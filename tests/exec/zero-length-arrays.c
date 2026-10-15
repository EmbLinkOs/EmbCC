/* GNU C's zero-length arrays as objects: `int a[0]` and `int a[] = {}`,
 * local and global, are complete at size 0 and have addresses, as do
 * arrays of empty structs. `int y[] = {};` in a function was an internal
 * error, `int z[0];` an incomplete type. */
// expect-exit: 42
int printf(const char *, ...);
struct E { };
int g0[0];
int gi[] = { };
struct E ge[3];
__attribute__((noinline)) static int addr_ok(void *a, void *b) { return a != 0 && b != 0; }
int main(void)
{
    int y[] = { };
    int z[0];
    struct E e3[3];
    int after = 7;
    int ok = 0;
    ok += sizeof y == 0 && sizeof z == 0 && sizeof g0 == 0 && sizeof gi == 0;
    ok += sizeof e3 == 0 && sizeof ge == 0 && sizeof(int[0]) == 0;
    ok += addr_ok(y, z) && addr_ok(g0, gi) && addr_ok(e3, ge);
    ok += after == 7;
    printf("%d %d %d\n", ok, (int)sizeof y, (int)sizeof(int[0][4]));
    return ok == 4 ? 42 : ok;
}
