/* __alignof__ of a variable counts its own aligned(N) in a constant
 * expression too, as it already did in code: an array bound and a
 * _Static_assert are folded by the parser, which measured the type
 * alone (GCC's vector-align-1.c). */
// expect-exit: 42

char x __attribute__((aligned(64)));
int f[__alignof__(x) == 64 ? 1 : -1];
_Static_assert(__alignof__(x) == 64, "x is 64-aligned");

int main(void)
{
    return sizeof f == sizeof(int) && __alignof__(x) == 64 ? 42 : 1;
}
