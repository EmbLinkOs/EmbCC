/* Brackets inside a pack expansion.
 *
 * The two scanners that find packs (src/cxx/template.c: expansion_at and
 * packs_in) keep a stack of the brackets they are inside, so they can
 * tell a template argument list's `<` from a less-than. Both stored the
 * TOKEN KIND in a char and used ASCII '<' -- 60 -- as the marker for the
 * template case. TOK_LBRACE is 60. So a `{` inside the scanned range was
 * popped as if it were an angle bracket, the stack went out of step, and
 * the pack that followed was reported as needing a `...` it already had:
 *
 *     template <class... Ts> int f(Ts... xs) { return g(P{xs, xs*2}...); }
 *     error: parameter pack 'xs' must be expanded with '...'
 *
 * Nothing ever read the stored kind -- every use asked only "was it
 * '<'" -- so both now keep a flag, which cannot collide with anything.
 * The same latent collision would have hit '(' the moment any two
 * tokens were added to the enum ahead of TOK_LPAREN, and did.
 */
// expect-exit: 42

struct P { int a; int b; };
static int sum3(P x, P y, P z) { return x.a + x.b + y.a + y.b + z.a + z.b; }

/* A braced initializer in the pattern: the `{` the scanner mistook. */
template <class... Ts> int braced(Ts... xs) { return sum3(P{xs, xs * 2}...); }

/* A lambda body in the pattern -- a `{` that is not an initializer, and
 * that contains a `<` which is a comparison and not a template. */
template <class... Ts> int lam(Ts... xs)
{
    return sum3(P{[] { return 1 < 2 ? 1 : 0; }(), xs}...);
}

/* Every other bracket in a pattern, together, with a real template
 * argument list among them so the flag is exercised both ways. */
template <class T> struct Id { typedef T type; };
template <class... Ts> int mixed(Ts... xs)
{
    int arr[] = { xs... };
    return sum3(P{arr[0], (typename Id<int>::type)(xs)}...);
}

int main()
{
    /* (1+2) + (2+4) + (3+6) */
    if (braced(1, 2, 3) != 18) return 1;
    /* (1+1) + (1+2) + (1+3) */
    if (lam(1, 2, 3) != 9) return 2;
    /* (1+1) + (1+2) + (1+3) */
    if (mixed(1, 2, 3) != 9) return 3;
    return 42;
}
