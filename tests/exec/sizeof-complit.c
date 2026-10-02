/* sizeof and _Alignof of a compound literal, `sizeof (T){ ... }`: the
 * parenthesized type is followed by a brace, so the operand is the
 * literal (an expression), not the type. An unsized array literal takes
 * its size from the list, brace elision included. */
// expect-exit: 42
struct pt { int x, y; };
int main(void)
{
    int bad = 0;
    if (sizeof (struct pt[]){ 1, 2, 3 } != 2 * sizeof(struct pt)) bad |= 1;
    if (sizeof (int[]){ 1, 2, 3 } != 3 * sizeof(int)) bad |= 2;
    if (sizeof (struct pt){ 1 } != sizeof(struct pt)) bad |= 4;
    if (sizeof (char[]){ "abcd" } != 5) bad |= 8;
    if (_Alignof (double){ 1.0 } != _Alignof(double)) bad |= 16;

    return bad ? bad : 42;
}
