/* C11 gives the plain operators on an _Atomic object seq_cst atomic
 * semantics: a read is an atomic load, `=` an atomic store, `++` and
 * `op=` one atomic read-modify-write. EmbCC treated _Atomic as volatile,
 * so `x++` was a load, an add and a store that an interrupt or another
 * core could split (tests/golden/atomic-operators.sh pins the
 * instructions). Here every operator's VALUE is checked, prefix and
 * postfix, on an int, an unsigned, a pointer and through a pointer to an
 * atomic object -- the word-sized ones every board does in one access. */
// expect-exit: 42
_Atomic int ai = 5;
_Atomic unsigned au = 0xF0F0u;
static int arr[8];
_Atomic(int *) ap = arr;
static int touch(_Atomic int *p) { return (*p)++ + ++*p; }   /* 1 + 3 */
int main(void)
{
    int bad = 0;
    if (ai++ != 5 || ai != 6 || ++ai != 7 || ai-- != 7 || --ai != 5) bad |= 1;
    if ((ai += 10) != 15 || (ai -= 3) != 12 || (ai *= 3) != 36 ||
        (ai /= 5) != 7 || (ai %= 4) != 3 || (ai <<= 4) != 48 ||
        (ai >>= 2) != 12) bad |= 2;
    if ((au &= 0xFF00u) != 0xF000u || (au |= 0x000Fu) != 0xF00Fu ||
        (au ^= 0xFFFFu) != 0x0FF0u) bad |= 4;
    au = 0;
    if (au-- != 0 || au != (unsigned)-1) bad |= 8;      /* wraps */
    if (ap++ != arr || ap != arr + 1 || (ap += 3) != arr + 4 ||
        (ap -= 2) != arr + 2 || --ap != arr + 1) bad |= 16;
    _Atomic int local = 1;
    if (touch(&local) != 4 || local != 3) bad |= 32;
    ai = 40;
    int sum = ai + local - 1;                          /* reads */
    return bad ? bad : sum;
}
