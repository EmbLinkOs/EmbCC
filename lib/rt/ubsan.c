/* -fsanitize=undefined's report, on a hosted target: a failed check calls
 * this instead of trapping (src/ir/irgen.c's san_fail_if), so the program
 * says what went wrong and where, as clang's and GCC's runtime do:
 *
 *   t.c:3:14: runtime error: signed integer overflow
 *
 * and then aborts -- or, under -fsanitize-recover=undefined, returns and
 * the program goes on with the wrapped value. On a board, and with
 * -fsanitize-trap, the check traps and none of this is linked.
 *
 * Nothing here includes a header: the C library under it may be EmbCC's,
 * glibc or libSystem, and write and abort are the same in all of them. */
#if defined(__linux__) || defined(__APPLE__)

typedef __SIZE_TYPE__ usize;
long write(int, const void *, usize);
void abort(void);

#define NOI __attribute__((no_instrument_function,                     \
                           no_profile_instrument_function,             \
                           no_sanitize("address", "thread", "memory",  \
                                       "undefined")))

static NOI void put(const char *s)
{
    usize n = 0;
    while (s[n])
        n++;
    while (n) {
        long w = write(2, s, n);
        if (w <= 0)
            return;
        s += w;
        n -= (usize)w;
    }
}

static NOI void num(unsigned long v)
{
    char b[24];
    int k = 23;
    b[k] = 0;
    do
        b[--k] = (char)('0' + v % 10);
    while ((v /= 10) && k);
    put(b + k);
}

/* why: 1 signed overflow, 2 division by zero, 3 the most negative value
 * divided by -1, 4 a shift count out of range, 5 an index out of bounds,
 * 6 negation of the most negative value. recover: go on. */
__attribute__((weak)) NOI void __embcc_ubsan_fail(unsigned long why,
                                                  const char *file,
                                                  unsigned long line,
                                                  unsigned long col,
                                                  unsigned long recover)
{
    static const char *const what[] = {
        "undefined behavior",
        "signed integer overflow",
        "division by zero",
        "division of the most negative value by -1 overflows",
        "shift count is negative or not less than the operand's width",
        "index out of bounds",
        "negation of the most negative value overflows",
    };
    put(file ? file : "?");
    put(":");
    num(line);
    if (col) {
        put(":");
        num(col);
    }
    put(": runtime error: ");
    put(what[why < sizeof what / sizeof *what ? why : 0]);
    put("\n");
    if (!recover)
        abort();
}

#else
typedef int ubsan_hosted_only;
#endif
