/* Allocation that cannot fail quietly, and the diagnostic API.
 *
 * A diagnostic is a record (src/driver/diag.c, docs/tools/diagnostics.md): severity,
 * location and source range, the option that controls it, notes under it,
 * and fix-its. They are held and rendered once — as GCC's caret text, or as
 * GCC's JSON for an editor — so the API here builds them rather than
 * printing. diag_at and diag_fatal still stop the compile; recovery (T2)
 * will give the front ends a way not to.
 */
#ifndef EMBCC_DRIVER_UTIL_H
#define EMBCC_DRIVER_UTIL_H

/* The calls below that never come back say so, so that a caller's control
 * flow analysis -- ours and the host compiler's -- knows a switch arm that
 * ends in one needs no return. EmbCC understands the attribute too, which
 * matters when it compiles itself. */
#if defined(__GNUC__) || defined(__clang__) || defined(__EMBCC__)
#define EMBCC_NORETURN __attribute__((noreturn))
#else
#define EMBCC_NORETURN
#endif

#include <stddef.h>

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
void *xcalloc(size_t n, size_t size);
char *xstrndup(const char *s, size_t n);

/* Register a file's text so diagnostics naming it can print its source lines.
 * `text` must outlive every diagnostic (the whole compile). */
void diag_register_source(const char *file, const char *text);

/* Record that `macro` expanded at file:line, so an error there notes it.
 * `file`/`macro` must outlive the compile. */
void diag_register_expansion(const char *file, int line, const char *macro);

/* "embcc: FILE:LINE: error: ..." then the source line and a caret, then
 * exit(1). line 0 omits the line; a registered source adds the line + caret. */
EMBCC_NORETURN void diag_fatal(const char *file, int line, const char *fmt, ...);

/* As diag_fatal, but with a column for the caret. */
EMBCC_NORETURN void diag_at(const char *file, int line, int col, const char *fmt, ...);

/* An "error:" that does NOT exit — the primary of an error+note pair. Follow it
 * with diag_note_at(s) and then exit(1) yourself. */
void diag_error_at(const char *file, int line, int col, const char *fmt, ...);

/* A non-fatal "note:" tied to a location — a previous declaration, a macro
 * expansion site. Does not exit. */
void diag_note_at(const char *file, int line, int col, const char *fmt, ...);

/* A "warning:" at a location. Does not exit. */
void diag_warn_at(const char *file, int line, int col, const char *fmt, ...);

/* A warning that a -W option controls: silent unless that option is on,
 * and printed with "[-Wname]". */
void diag_warn_opt(const char *file, int line, int col, const char *name,
                   const char *fmt, ...);
int diag_warning_enabled(const char *name);
/* Warnings from this file are dropped (a system header), unless
 * -Wsystem-headers. */
void diag_mark_system(const char *file);
void diag_set_warn_system(int on);
void diag_enable_warning(const char *name, int on);   /* -Wname / -Wno-name */
void diag_enable_group(int wall, int wextra);         /* -Wall / -Wextra */
int diag_warning_count(void);                         /* for --help */
const char *diag_warning_name(int i);
int diag_warning_group(int i);                        /* 0 none, 1 all, 2 extra */

/* An error whose arguments a front end already gathered (parse.c builds its
 * own error on top of this one). Needs <stdarg.h> included first. */
#ifdef va_arg
void diag_verror_at(const char *file, int line, int col, const char *fmt,
                    va_list ap);
#endif

/* ---- the diagnostic engine (diag.c) ---- */

enum { DIAG_ERROR, DIAG_WARNING, DIAG_NOTE };
enum { DIAG_TEXT, DIAG_JSON };

/* Widen the last diagnostic's caret to end at `end_col` (exclusive). Without
 * one, a caret underlines the identifier or number it lands on. */
void diag_range(int end_col);

/* Tag the diagnostic just raised with an explain id ("E0001"), which is
 * printed with it and understood by `embcc --explain`. */
void diag_set_id(const char *id);

/* src/driver/explain.c */
int explain_print(const char *id);      /* NULL/"" lists them */
const char *explain_title(const char *id);
/* The standard header that declares this name, or NULL: a name that is not
 * declared is most often a missing #include. */
const char *header_declaring(const char *name);

/* Attach a fix-it to the last diagnostic (or to its last note, which is
 * where "did you mean 'x'?" carries it): replace [col, end_col) of `line`
 * with `text`. Inserting is col == end_col; deleting is text "". */
void diag_fixit_at(const char *file, int line, int col, int end_col,
                   const char *text);

/* The same, for a token the front end can name but not locate: replaces
 * the first `find` at or after (line, from_col) in the registered source.
 * 0 (and nothing attached) if it is not there. */
int diag_fixit_find(const char *file, int line, int from_col,
                    const char *find, const char *text);

void diag_set_format(int format);      /* DIAG_TEXT (default) or DIAG_JSON */
void diag_set_color(int mode);         /* -1 auto (a terminal), 0 no, 1 yes */
void diag_set_max_errors(int n);       /* stop after n errors; 0 = no limit */
void diag_set_werror(int on);          /* warnings become errors */
void diag_set_no_warnings(int on);     /* -w: drop them */
int  diag_error_count(void);           /* for the driver's exit status */
void diag_set_parseable_fixits(int on); /* GCC's fix-it: lines */
/* --fix: do the edits the fix-its describe, in the files they name.
 * Returns how many were applied. */
int diag_apply_fixits(void);
void diag_flush(void);                 /* render everything held (atexit) */
/* The closing "compilation terminated: N errors" (text only: in JSON the
 * array is the whole output). */
void diag_terminated(int nerrors);

/* ---- the fatal boundary (R6: stages are libraries) ----
 *
 * A library must not end the process. `embcc` may; a language server holding
 * the same libraries in-process may not, and neither may a future in-process
 * build server -- on EmbLinkOS there is no fork/exec to isolate them behind
 * (ARCHITECTURE §1), so a backend that calls exit() takes the whole host with
 * it.
 *
 * So the driver installs a boundary and the libraries unwind to it:
 *
 *     jmp_buf boundary;
 *     if (setjmp(boundary) == 0) {
 *         fatal_set_boundary(&boundary);
 *         ... compile ...
 *     } else {
 *         ... the unit failed; the diagnostic is already recorded ...
 *     }
 *     fatal_set_boundary(NULL);
 *
 * With no boundary installed, fatal_error exits as before, so a tool that
 * has not adopted the pattern still behaves correctly.
 *
 * `internal_error` is for an impossible state -- a compiler bug, not the
 * user's mistake. It says so, because a reader who is told "internal error"
 * knows to report it rather than to edit their program.
 */
void fatal_set_boundary(void *jmp_buf_ptr);
EMBCC_NORETURN void internal_error(const char *fmt, ...);

/* The diagnostic is already recorded and this unit cannot continue: leave
 * for the boundary. This is what the front ends' "no recovery point" paths
 * call instead of exit(1). */
EMBCC_NORETURN void fatal_unwind(void);

/* ---- a growable text buffer ----
 *
 * So a stage can BUILD its output and hand it over as bytes, instead of
 * streaming to a FILE* it opened itself. That is what lets writing go
 * through the platform layer (§16), and what lets the same dump be sent to
 * a file, to stdout, or to a caller in memory without three code paths.
 */
struct outbuf { char *p; size_t n, cap; };

void ob_add(struct outbuf *b, const char *s, size_t n);
void ob_str(struct outbuf *b, const char *s);
void ob_ch(struct outbuf *b, char c);
/* Returns the number of characters appended, so callers can track a column. */
int ob_fmt(struct outbuf *b, const char *fmt, ...);
void ob_free(struct outbuf *b);

#endif
