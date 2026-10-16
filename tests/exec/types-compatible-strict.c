/* __builtin_types_compatible_p is C's compatibility, the top level's
 * qualifiers aside: `char *` and `const char *` are not compatible, nor
 * are two different enums (each IS compatible with its integer), and
 * `int[5]` goes with `int[]` (GCC's builtin-types-compatible-p.c; the
 * three answers were the other way round). */
// expect-exit: 42
typedef enum { hot, dog } dingos;
typedef enum { janette, laura } cranberry;
_Static_assert(!__builtin_types_compatible_p(char *, const char *), "ptr qual");
_Static_assert(!__builtin_types_compatible_p(dingos, cranberry), "two enums");
_Static_assert(__builtin_types_compatible_p(int[5], int[]), "unsized array");
_Static_assert(__builtin_types_compatible_p(int, const int), "top qual");
_Static_assert(__builtin_types_compatible_p(__typeof__(hot), int), "enum const");
_Static_assert(!__builtin_types_compatible_p(int[5], int[6]), "sizes");
_Static_assert(!__builtin_types_compatible_p(long, long long), "long");
_Static_assert(__builtin_types_compatible_p(int (*)(int), int (*)(const int)), "param qual");
int main(void) { return 42; }
