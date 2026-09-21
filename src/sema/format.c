/* -Wformat: a printf or scanf format string checked against the
 * arguments beside it, where the call is written.
 *
 * The bug this catches is specific and unusually nasty. A variadic call
 * has no prototype for its tail, so `printf("%d\n", 1L)` and
 * `printf("%s\n", 42)` compile with nothing to say -- and then read the
 * wrong number of bytes, or a pointer that is not one, at run time. The
 * type information exists at the call; only nobody was comparing it.
 *
 * WHICH functions to check is not hard-coded here. A function says so
 * itself with __attribute__((format(printf, n, m))), which is how GCC
 * has spelled it for thirty years and is already on every declaration
 * in a real <stdio.h>. The compiler knowing the names of the standard
 * library would be knowledge duplicated in two places, and wrong the
 * first time someone writes their own logging function.
 *
 * WHAT is compared is the PROMOTED type, because that is what actually
 * lands in the variadic tail: a float arrives as a double and a short
 * as an int, so `printf("%f", 1.0f)` and `printf("%d", (short)x)` are
 * both correct and neither is reported. sema has already applied those
 * promotions by the time this runs, so it reads the argument types as
 * they will be passed rather than as they were written.
 *
 * What is deliberately NOT reported is a signedness mismatch --
 * `printf("%d", 3u)` and `printf("%x", 3)`. Every such pair has the
 * same size and the same representation for the values that reach it;
 * GCC reports them, and in a codebase of any size the result is noise
 * that trains the reader to ignore the category. What is reported is a
 * difference the machine can see: a wrong SIZE, a wrong CLASS, a
 * pointer where a number goes, or a count that does not match.
 */
#include "format.h"

#include "type.h"
#include "../driver/util.h"

#include <string.h>

/* What a conversion wants of its argument, after the promotions. */
enum want {
    W_NONE,        /* %% and a suppressed scanf field: no argument */
    W_INT,         /* an integer of `size` bytes */
    W_DOUBLE,
    W_LDOUBLE,
    W_PTR,         /* any pointer (%p, %n, and every scanf field) */
    W_STR,         /* a pointer to characters (%s) */
};

struct conv {
    enum want w;
    int size;              /* W_INT, or a scanf field: bytes read/written */
    /* What a scanf field writes through its pointer: 1 an integer, 2 a
     * floating value, 0 "do not ask". Zero is the right answer for %s,
     * %c and %[, whose argument is a character BUFFER rather than a
     * pointer to one object -- checking its pointee against a size is
     * how a checker reports `sscanf(s, "%s", buf)` and becomes useless. */
    int pclass;
    int stars;             /* width/precision `*`, each an int argument */
    char c;                /* the conversion character, for the message */
    int bad;               /* an unknown conversion */
};

/* Read one conversion, starting just past its '%'. Returns the position
 * after it. */
static const char *read_conv(const char *p, const char *end, int scanf_like,
                             struct conv *cv)
{
    memset(cv, 0, sizeof *cv);
    cv->size = 4;

    if (p < end && *p == '%') {           /* %% is a literal percent */
        cv->w = W_NONE;
        cv->c = '%';
        return p + 1;
    }
    int suppress = 0;
    if (scanf_like) {
        if (p < end && *p == '*') { suppress = 1; p++; }
    } else {
        while (p < end && strchr("-+ #0'", *p))
            p++;
    }
    /* Width, then precision. A `*` takes an int argument of its own --
     * which is the part a hand-written checker forgets, and it shifts
     * every argument after it. */
    if (!scanf_like && p < end && *p == '*') { cv->stars++; p++; }
    else while (p < end && *p >= '0' && *p <= '9') p++;
    if (p < end && *p == '.') {
        p++;
        if (!scanf_like && p < end && *p == '*') { cv->stars++; p++; }
        else while (p < end && *p >= '0' && *p <= '9') p++;
    }

    /* Length modifiers. long and long long are one type on both targets,
     * so they want the same size and are not distinguished. */
    int len_long = 0, len_ldouble = 0, len_short = 0;
    for (;;) {
        if (p + 1 < end && p[0] == 'h' && p[1] == 'h') { len_short = 2; p += 2; }
        else if (p < end && *p == 'h') { len_short = 1; p++; }
        else if (p + 1 < end && p[0] == 'l' && p[1] == 'l') { len_long = 1; p += 2; }
        else if (p < end && *p == 'l') { len_long = 1; p++; }
        else if (p < end && (*p == 'j' || *p == 'z' || *p == 't')) { len_long = 1; p++; }
        else if (p < end && *p == 'L') { len_ldouble = 1; len_long = 1; p++; }
        else break;
    }
    if (p >= end) { cv->bad = 1; cv->c = '?'; return p; }

    char c = *p++;
    cv->c = c;
    switch (c) {
    case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'b':
        cv->w = W_INT;
        /* Promotion has already widened hh and h to an int; only the
         * long forms change what the tail reads. */
        cv->size = len_long ? 8 : 4;
        if (scanf_like) {
            cv->w = W_PTR;
            cv->pclass = 1;
            cv->size = len_long ? 8 : len_short == 2 ? 1 : len_short ? 2 : 4;
        }
        break;
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
    case 'a': case 'A':
        cv->w = scanf_like ? W_PTR : (len_ldouble ? W_LDOUBLE : W_DOUBLE);
        /* scanf is the mirror of printf here: there is no promotion, so
         * plain %f writes a FLOAT and %lf a double. Passing &a_double to
         * %f is the classic version of this bug. */
        cv->size = len_ldouble ? 16 : len_long ? 8 : 4;
        if (scanf_like)
            cv->pclass = 2;
        break;
    case 'c':
        cv->w = scanf_like ? W_PTR : W_INT;
        cv->size = 4;
        break;
    case 's':
        cv->w = scanf_like ? W_PTR : W_STR;
        break;
    case 'p': case 'n':
        cv->w = W_PTR;
        break;
    case '[':
        if (!scanf_like) { cv->bad = 1; break; }
        cv->w = W_PTR;
        if (p < end && *p == '^') p++;
        if (p < end && *p == ']') p++;        /* a leading ] is a member */
        while (p < end && *p != ']') p++;
        if (p < end) p++;
        break;
    case 'm':
        cv->w = W_NONE;                        /* GNU %m: strerror(errno) */
        break;
    default:
        cv->bad = 1;
        break;
    }
    if (suppress)
        cv->w = W_NONE;
    return p;
}

static int is_char_ptr(const struct type *t)
{
    return t->kind == TY_PTR && t->pointee &&
           (t->pointee->kind == TY_CHAR || t->pointee->kind == TY_VOID);
}

static int is_int_like(const struct type *t)
{
    switch (t->kind) {
    case TY_BOOL: case TY_CHAR: case TY_SHORT: case TY_INT:
    case TY_LONG: case TY_INT128:
        return 1;
    default:
        return 0;
    }
}

/* The name for a message: `%` then the conversion, with its modifier
 * folded away -- "%ld" reads better than "the conversion at offset 7". */
static const char *conv_name(char c, char *buf)
{
    buf[0] = '%';
    buf[1] = c;
    buf[2] = 0;
    return buf;
}

void format_check(const char *file, struct expr *call, const struct func *fn)
{
    if (!fn->fmt_kind || fn->fmt_idx <= 0)
        return;
    int scanf_like = fn->fmt_kind == 2;
    int fi = fn->fmt_idx - 1;              /* the indices are 1-based */
    if (fi >= call->nargs)
        return;

    /* Only a literal can be read. A format built at run time is not a
     * defect and not checkable, so nothing is said about it. */
    struct expr *fmt = call->args[fi];
    while (fmt && fmt->kind == EXPR_CAST)
        fmt = fmt->rhs;
    if (!fmt || fmt->kind != EXPR_STR || fmt->str_width != 1 || !fmt->name)
        return;

    /* `num` counts the NUL; the bytes may contain their own. */
    const char *s = fmt->name;
    const char *end = s + (fmt->num > 0 ? fmt->num - 1 : 0);

    /* first-to-check of 0 means "this one takes a va_list", so there is
     * no argument tail here to compare against -- only the string. */
    int check_args = fn->fmt_first > 0;
    int ai = check_args ? fn->fmt_first - 1 : call->nargs;
    int unknown = 0;     /* a conversion nobody can count arguments past */
    char nb[4];

    for (const char *p = s; p < end; ) {
        if (*p != '%') { p++; continue; }
        p++;
        struct conv cv;
        p = read_conv(p, end, scanf_like, &cv);
        if (cv.bad) {
            diag_warn_opt(file, call->line, call->col, "format",
                          "'%%%c' is not a conversion this format "
                          "understands", cv.c);
            /* How many arguments it would have taken is now a guess, so
             * the count below is not reported: one diagnostic about one
             * mistake, rather than a second that follows from it. */
            unknown = 1;
            continue;
        }
        for (int k = 0; k < cv.stars; k++) {
            if (!check_args) continue;
            if (ai >= call->nargs) { ai++; continue; }
            struct type *t = call->args[ai]->ty;
            if (!is_int_like(t))
                diag_warn_opt(file, call->args[ai]->line, call->args[ai]->col,
                              "format",
                              "a '*' width or precision takes an int, "
                              "but this argument is %s", ty_name(t));
            ai++;
        }
        if (cv.w == W_NONE)
            continue;
        if (!check_args)
            continue;
        if (ai >= call->nargs) { ai++; continue; }

        struct type *t = call->args[ai]->ty;
        int line = call->args[ai]->line, col = call->args[ai]->col;
        const char *cn = conv_name(cv.c, nb);
        ai++;

        switch (cv.w) {
        case W_INT:
            if (t->kind == TY_PTR || t->kind == TY_ARRAY)
                diag_warn_opt(file, line, col, "format",
                              "%s takes an integer, but this argument is "
                              "%s", cn, ty_name(t));
            else if (!is_int_like(t))
                diag_warn_opt(file, line, col, "format",
                              "%s takes an integer, but this argument is "
                              "%s", cn, ty_name(t));
            else if (ty_size(t) != cv.size)
                /* The one that silently reads the wrong bytes off the
                 * variadic tail, and the reason this check exists. */
                diag_warn_opt(file, line, col, "format",
                              "%s reads %d bytes, but this argument is %s, "
                              "which is %d", cn, cv.size, ty_name(t),
                              ty_size(t));
            break;
        case W_DOUBLE:
            if (t->kind == TY_LDOUBLE)
                diag_warn_opt(file, line, col, "format",
                              "%s takes a double; a long double needs the "
                              "'L' modifier", cn);
            else if (t->kind != TY_DOUBLE)
                diag_warn_opt(file, line, col, "format",
                              "%s takes a double, but this argument is %s",
                              cn, ty_name(t));
            break;
        case W_LDOUBLE:
            if (t->kind != TY_LDOUBLE)
                diag_warn_opt(file, line, col, "format",
                              "%s takes a long double, but this argument "
                              "is %s", cn, ty_name(t));
            break;
        case W_STR:
            if (!is_char_ptr(t))
                diag_warn_opt(file, line, col, "format",
                              "%s takes a pointer to characters, but this "
                              "argument is %s", cn, ty_name(t));
            break;
        case W_PTR:
            if (t->kind != TY_PTR && t->kind != TY_ARRAY)
                diag_warn_opt(file, line, col, "format",
                              "%s takes a pointer, but this argument is %s",
                              cn, ty_name(t));
            else if (scanf_like && cv.pclass && t->kind == TY_PTR &&
                     t->pointee) {
                struct type *pt = t->pointee;
                int fp = pt->kind == TY_FLOAT || pt->kind == TY_DOUBLE ||
                         pt->kind == TY_LDOUBLE;
                if (cv.pclass == 1 ? !is_int_like(pt) : !fp)
                    diag_warn_opt(file, line, col, "format",
                                  "%s writes %s, but this argument points "
                                  "at %s", cn,
                                  cv.pclass == 1 ? "an integer"
                                                 : "a floating value",
                                  ty_name(pt));
                else if (ty_size(pt) != cv.size)
                    diag_warn_opt(file, line, col, "format",
                                  "%s writes %d bytes, but this argument "
                                  "points at %s, which is %d", cn, cv.size,
                                  ty_name(pt), ty_size(pt));
            }
            break;
        default:
            break;
        }
    }

    if (!check_args || unknown)
        return;
    if (ai > call->nargs)
        diag_warn_opt(file, call->line, call->col, "format",
                      "this format needs %d argument%s, but the call "
                      "passes %d", ai - (fn->fmt_first - 1),
                      ai - (fn->fmt_first - 1) == 1 ? "" : "s",
                      call->nargs - (fn->fmt_first - 1));
    else if (ai < call->nargs)
        diag_warn_opt(file, call->args[ai]->line, call->args[ai]->col,
                      "format",
                      "this format uses %d argument%s, but the call passes "
                      "%d", ai - (fn->fmt_first - 1),
                      ai - (fn->fmt_first - 1) == 1 ? "" : "s",
                      call->nargs - (fn->fmt_first - 1));
}
