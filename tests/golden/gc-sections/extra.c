/* Linked beside ldscript/prog.c into the STM32-shaped image: nothing
 * here is reachable but what -u names. */
int unused_counter = 5;
const char unused_table[256] = { 1, 2, 3 };
int unused_zero[64];
int unused_fn(int x)
{
    return x * unused_counter + unused_table[x & 255] + unused_zero[x & 63];
}
int unused_caller(int x) { return unused_fn(x) + 1; }

int kept_data = 3;
int kept_by_u(int x) { return x + kept_data; }   /* -u kept_by_u */
