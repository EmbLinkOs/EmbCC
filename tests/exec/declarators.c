/* Three spellings of a declaration that the standard allows, that real
 * code uses, and that EmbCC did not accept. Two of the three did not
 * merely fail -- they took the compiler down.
 *
 * 1. A REDUNDANTLY PARENTHESIZED declarator: `int (f)(int);`. C11
 *    §6.7.6 lets a declarator be parenthesized anywhere, and the reason
 *    a header does it is to suppress a function-like macro of the same
 *    name -- `(isdigit)(c)` calls the function even where <ctype.h> has
 *    defined the macro. EmbCC handled `(` only when a `*` followed, so
 *    the pointer form `int (*fp)(int)` worked and the plain one was
 *    "expected a name before '('". The DEFINITION was worse: the outer
 *    parameter list was parsed by a path that discards parameter
 *    names, so the body was compiled against whatever the previous
 *    declaration had left in the name array -- a stale pointer, and a
 *    segfault.
 *
 * 2. `_Alignas` BEFORE the storage class: `_Alignas(16) static char a[1];`
 *    C11 §6.7 makes declaration specifiers unordered, so this is the
 *    same declaration as `static _Alignas(16) char a[1];`. Only the
 *    second parsed. The first crashed: the type-specifier parser
 *    consumed the alignment, found `static` where a type belonged,
 *    returned NULL, and the caller dereferenced it. And once it
 *    parsed, the alignment was still dropped on the way from the local
 *    to the global it becomes -- accepted, and then not done, which is
 *    the failure that does not announce itself.
 *
 * 3. The address of an element of an array of arrays, in a STATIC
 *    initializer: `int *row = &g[1][0];`. `a[i]` is built as `*(a + i)`,
 *    so this arrives as `&*(*(g+1) + 0)`, and the constant-address
 *    resolver had no case for a dereference that yields an array --
 *    which loads nothing, because an array lvalue decays straight back
 *    to the address just computed. A table of rows naming its own first
 *    element was rejected as "not a constant".
 *
 * The numbers below are offsets in elements, checked against what
 * clang computes for the same file.
 */
// expect-exit: 42

int (f)(int);                      /* 1: parenthesized declaration */
int (f)(int x) { return x; }       /*    ... and definition */
int (*fp)(int) = f;

int g[3][4];
int *row1 = &g[1][0];              /* 3: the case that failed */
int *row2 = g[2];                  /*    the same shape, written as a decay */
int *mid  = &g[1][2];
int *back = &g[2][3] - 1;

struct s { int a; int b[4]; } st;
int *smem = &st.b[2];              /*    and through a member */

/* The declarator that keeps the fix honest: <signal.h>'s, where the
 * parenthesized part is itself a function declarator with named
 * parameters, and the suffix after it belongs to the RETURN type. The
 * first version of the fix wrote the suffix's names into the parser's
 * one shared name array, and parsing `handler`'s own `(int)` then
 * overwrote `sig` with nothing -- so the real libc's signal() stopped
 * compiling with "parameter 1 needs a name in a definition". */
typedef void (*sighandler)(int);
static sighandler installed;
static int bumped;
static void bump(int n) { bumped += n; }
void (*setsig(int sig, void (*handler)(int)))(int)
{
    sighandler prev = installed;
    installed = sig ? handler : 0;
    return prev;
}

static int aligned_statics(void)
{
    _Alignas(64) static char a[1];        /* 2: alignment first */
    static _Alignas(64) char b[1];        /*    storage first */
    _Alignas(64) static char c[1] = { 1 };/*    and in .data */
    unsigned long bad = ((unsigned long)a | (unsigned long)b |
                         (unsigned long)c) & 63;
    return bad == 0 && c[0] == 1;
}

int main(void)
{
    int (y) = 40;                  /* 1: at block scope too */
    static int (z) = 2;

    if ((f)(y) != 40 || fp(z) != 2)
        return 1;
    if (row1 - &g[0][0] != 4)  return 2;
    if (row2 - &g[0][0] != 8)  return 3;
    if (mid  - &g[0][0] != 6)  return 4;
    if (back - &g[0][0] != 10) return 5;
    if (smem - &st.a != 3)     return 6;
    if (!aligned_statics())    return 7;

    /* setsig's parameters both have to be reachable in its body */
    if (setsig(1, bump) != 0)  return 8;
    installed(5);                  /* the handler the first call kept */
    if (setsig(0, 0) != bump)  return 9;
    if (bumped != 5)           return 10;
    installed = 0;

    return y + z;                  /* 42 */
}
