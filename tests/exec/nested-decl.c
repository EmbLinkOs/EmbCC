/* Declarators nested in parentheses around a function declarator:
 * functions returning pointers to functions and to arrays (C11 6.7.6) —
 * the classic `void (*signal(int, void (*)(int)))(int)` — declared,
 * defined and called; and deeper nests (a pointer to a function returning
 * a function pointer, an array of them). The C++ front end emits these
 * for std::forward<const char (&)[N]>. gcc referees every value. */
// expect-exit: 42

typedef void (*handler_t)(int);

static int last;
static void h1(int v) { last = v; }
static void h2(int v) { last = v * 2; }

static handler_t current = h1;

/* returns the previous handler */
void (*set_handler(int which, void (*fn)(int)))(int)
{
    void (*old)(int) = current;
    if (which)
        current = fn;
    return old;
}

static char name[6] = "hello";
static int grid[3][4];

/* a pointer to an array of 6 chars, and to a row of 4 ints */
char (*name_ptr(void))[6] { return &name; }
int (*row(int i))[4] { return &grid[i]; }

/* prototypes only, defined below */
static int (*pick(int k))(int, int);
static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static int (*pick(int k))(int, int) { return k ? mul : add; }

/* a pointer to a function returning a function pointer */
static int (*(*chooser)(int))(int, int) = pick;
/* an array of those */
static int (*(*choosers[2])(int))(int, int) = { pick, pick };

int main(void)
{
    handler_t prev = set_handler(1, h2);
    if (prev != h1)
        return 1;
    current(5);
    if (last != 10)
        return 2;
    if (set_handler(0, 0) != h2)
        return 3;

    char (*np)[6] = name_ptr();
    if ((*np)[1] != 'e' || sizeof *np != 6)
        return 4;
    (*row(2))[3] = 7;
    if (grid[2][3] != 7 || sizeof *row(0) != 4 * sizeof(int))
        return 5;

    if (pick(0)(3, 4) != 7 || pick(1)(3, 4) != 12)
        return 6;
    if (chooser(1)(6, 7) != 42 || choosers[1](0)(40, 2) != 42)
        return 7;
    return 42;
}
