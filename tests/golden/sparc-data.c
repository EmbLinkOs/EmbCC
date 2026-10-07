/* SPARC's own layouts, compared byte for byte with clang's by
 * tests/golden/sparc-data.sh beside tests/golden/be-data.c: what SPARC's
 * data model makes different from the other big-endian targets -- a
 * binary128 long double that is sixteen bytes but aligned to EIGHT (the
 * cap of __BIGGEST_ALIGNMENT__ 8), in structures, arrays, unions and
 * _Complex; long long and double aligned to 8 after a char; and the
 * special values of the quad format, most significant byte first. */
struct cld { char c; long double v; char d; };
struct cd8 { char c; double d; };
struct cll { char c; long long l; short s; };
struct ldarr { int n; long double v[3]; };
union uld { char c[3]; long double v; };
struct cpx { char c; _Complex long double z; _Complex double w; };

struct cld s_cld = { 1, 2.5L, 3 };
struct cd8 s_cd8 = { 4, -0.125 };
struct cll s_cll = { 5, 0x0102030405060708LL, -2 };
struct ldarr s_ldarr = { 3, { 1.0L / 3, -0.0L, 1e4000L } };
union uld s_uld = { { 7, 8, 9 } };
struct cpx s_cpx = { 6, 1.5L + 2.5Li, -3.0 + 0.5i };
long double ld_vals[] = {
    0.1L, -1.0L, 3.141592653589793238462643383279502884L,
    1.18973149535723176508575932662800702e4932L,   /* LDBL_MAX */
    3.36210314311209350626267781732175260e-4932L,  /* LDBL_MIN */
    6.47517511943802511092443895822764655e-4966L,  /* the smallest */
    __builtin_infl(), -__builtin_infl(),
};
char after_ld[] = { 'x' };
long double *p_ld = &ld_vals[3];
struct cld *p_cld = &s_cld;
