/* UTF-8 letters in identifiers (C11 Annex D), as gcc and clang take
 * them: locals, a global and a function (symbols carry the bytes), a
 * macro name, and a struct member. */
// expect-exit: 42
#define DOUBLÉ(x) ((x) * 2)
#define STR(x) #x
int compteur_été = 5;
struct π { int δ; };
static int größe(struct π p) { return p.δ + 1; }
int main(void)
{
    int palא = 10, ñ = 3;
    struct π q = { 7 };
    const char *s = STR(café);
    int len = 0;
    while (s[len]) len++;              /* "café" is 5 bytes */
    return DOUBLÉ(palא) + ñ + compteur_été + größe(q) - len + 11;  /* 20+3+5+8-5+11 */
}
