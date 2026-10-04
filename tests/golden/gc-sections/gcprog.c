/* Linked with --gc-sections on each board's harness (no script): what
 * main reaches runs, and what nothing reaches is not in the image. A
 * function reached only through a table in .rodata is reached. */
void puts_(const char *s);
void putn(long v);

int used_data[4] = { 1, 2, 3, 4 };
static const int used_ro[3] = { 7, 8, 9 };
int used_zero[8];
static int twice(int x) { return 2 * x; }
static int thrice(int x) { return 3 * x; }
int (*const table[2])(int) = { twice, thrice };
volatile int pick = 1;

__attribute__((noinline)) int helper(int x) { return x + used_ro[x % 3]; }

/* reached from .init_array alone (the harness does not run it: kept is
 * what is checked) */
int ctor_hits;
__attribute__((constructor)) static void gc_ctor(void) { ctor_hits++; }

int unused_data[64] = { 9 };
const char unused_ro[128] = "never read";
int unused_zero[32];
__attribute__((noinline)) int unused_fn(int x)
{
    return x + unused_data[x & 63] + unused_ro[x & 127] + unused_zero[x & 31];
}
int unused_caller(int x) { return unused_fn(x) * 3; }

int main(void)
{
    int s = 0;
    for (int i = 0; i < 4; i++)
        s += helper(used_data[i]);
    used_zero[3] = s;
    puts_("gc ");
    putn(s);
    putn(table[pick](5));
    putn(table[pick - 1](5));
    putn(used_zero[3]);
    puts_("\ndone\n");
    return 0;
}
