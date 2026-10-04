/* The program the STM32-shaped script links: what each part of that
 * script promises, checked from inside the running image. */
void puts_(const char *s);
void putn(long v);

int counter = 42;                       /* .data: copied from flash */
char greeting[] = "hello from flash";   /* .data */
static int zeros[100];                  /* .bss: zeroed by the startup */
static const int primes[] = { 2, 3, 5, 7, 11, 13 };   /* .rodata */
static int ctor_ran;

__attribute__((constructor)) static void init_first(void) { ctor_ran += 1; }

/* a table in a section of its own: placed as an orphan, walked by the
 * __start_/__stop_ symbols the linker provides */
struct cmd { const char *name; int code; };
__attribute__((section("cmds"), used)) static const struct cmd c1 = { "reset", 7 };
__attribute__((section("cmds"), used)) static const struct cmd c2 = { "boot", 35 };
extern const struct cmd __start_cmds[], __stop_cmds[];

extern char end[], _ebss[], _sdata[], _edata[];

int main(void)
{
    int sum = 0, nz = 0, ncmd = 0, codes = 0;
    for (int i = 0; i < 6; i++)
        sum += primes[i];
    for (int i = 0; i < 100; i++)
        nz += zeros[i] != 0;
    for (const struct cmd *c = __start_cmds; c < __stop_cmds; c++) {
        ncmd++;
        codes += c->code;
    }
    puts_(greeting); puts_("\n");
    putn(counter); putn(sum); putn(nz); putn(ctor_ran); putn(ncmd); putn(codes);
    putn(end >= _ebss); putn((long)(_edata - _sdata) >= 21);
    puts_("\n");
    counter++;
    putn(counter);
    puts_("done\n");
    return 0;
}
