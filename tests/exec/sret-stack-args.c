/* A call returning a struct through a hidden pointer, with a struct by
 * value and scalars after it: the pointer takes the first argument
 * register, and pushes arguments that would have fitted onto the stack.
 * On AVR the frame's outgoing area was sized as if it did not, so those
 * arguments landed on the caller's own slots -- the one holding the
 * result's address among them, and the 17 bytes came back to wherever
 * that pointed (found by verify/fuzz/run-avr.sh). The caller here keeps
 * addresses and values in its frame across the calls. */
// expect-exit: 42

struct big { unsigned long long x; float y; unsigned char z; long w; };

__attribute__((noinline)) static struct big make(struct big s, long k, short j)
{
    struct big r;
    r.x = s.x / 3 + (unsigned long long)k;
    r.y = s.y * 2.0f;
    r.z = (unsigned char)(s.z + j);
    r.w = s.w - k;
    return r;
}

__attribute__((noinline)) static struct big twice(long k)
{
    struct big a = { 300, 1.5f, 7, 1000 }, b;
    struct big *pa = &a;
    volatile long keep = k * 5;
    b = make(*pa, k, (short)(k + 1));
    b = make(b, keep, 3);
    return b;
}

int main(void)
{
    struct big r = twice(4);
    /* make(a, 4, 5): x = 100 + 4, y = 3, z = 12, w = 996; then
     * make(that, 20, 3): x = 104 / 3 + 20 = 54, y = 6, z = 15, w = 976 */
    if (r.x != 54 || r.y != 6.0f || r.z != 15 || r.w != 976)
        return 1;
    return 42;
}
