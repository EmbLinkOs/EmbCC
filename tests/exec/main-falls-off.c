/* Reaching the } that ends main returns 0 (C99 5.1.2.2.3) -- the one
 * function where falling off the end is defined. EmbCC refused it as it
 * refuses every other non-void function that can fall off its end. */
// expect-exit: 0
static volatile int x = 1;
int main(void)
{
    if (x == 2)
        return 5;
}
