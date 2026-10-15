/* `int B[], Q[];` at file scope, sized by nothing: tentative definitions
 * of one element each (C17 6.9.2p5), with GCC's warning; and one a later
 * declaration sizes. */
// expect-exit: 42
int B[], Q[];
int S[];
int S[3] = { 1, 2, 3 };
int main(void)
{
    B[0] = 20;
    Q[0] = 19;
    return B[0] + Q[0] + S[2] == 42 && sizeof S == 3 * sizeof(int) ? 42 : 1;
}
