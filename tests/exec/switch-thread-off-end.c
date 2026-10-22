/* Switch threading in a function that falls off its closing brace. The
 * loop's test is then the function's last block, `brnz c -> loop` with
 * the implicit return as its other way out -- a way out the CFG lists
 * no successor for. The pass took the block for a latch (one way out),
 * and copied it without the branch; and took it into the path to the
 * switch, whose copy branches the other way to the block after it, which
 * does not exist: it read past the end of its label array and branched
 * to label 0 ("label 0 in 'f' was never placed" at -O2 on x86-64 and
 * the M4, whatever the garbage named elsewhere). From GCC's torture
 * suite, compile/920624-1.c. */
// expect-exit: 42

int B[8], Q[8];

void f(void)
{
    int s;
    for (s = 0; s <= 6; s++)
        switch (s) {
        case 2: case 3: ++B[s];
        case 4: case 5: ++Q[s];
        }
}

/* the torture file's own: no case is ever taken */
void g(void)
{
    int s;
    for (s = 0; s <= 1; s++)
        switch (s) {
        case 2: case 3: ++B[s];
        case 4: case 5: ++Q[s];
        }
}

int main(void)
{
    f();
    g();
    int sum = 0;
    for (int i = 0; i < 8; i++)
        sum += B[i] * (i + 1) + Q[i] * 10;
    /* B[2] B[3]: 3 + 4; Q[2..5]: 40 */
    return sum == 47 ? 42 : sum;
}
