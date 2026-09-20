/* Allocation that cannot fail quietly, and the diagnostic API.
 *
 * A diagnostic is a record (src/driver/diag.c, docs/TOOLING.md): severity,
 * location and source range, the option that controls it, notes under it,
 * and fix-its. They are held and rendered once — as GCC's caret text, or as
 * GCC's JSON for an editor — so the API here builds them rather than
 * printing. diag_at and diag_fatal still stop the compile; recovery (T2)
 * will give the front ends a way not to.
 */
#ifndef EMBCC_DRIVER_UTIL_H
#define EMBCC_DRIVER_UTIL_H

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
void diag_fatal(const char *file, int line, const char *fmt, ...);

/* As diag_fatal, but with a column for the caret. */
void diag_at(const char *file, int line, int col, const char *fmt, ...);

/* An "error:" that does NOT exit — the primary of an error+note pair. Follow it
 * with diag_note_at(s) and then exit(1) yourself. */
void diag_error_at(const char *file, int line, int col, const char *fmt, ...);

/* A non-fatal "note:" tied to a location — a previous declaration, a macro
 * expansion site. Does not exit. */
void diag_note_at(const char *file, int line, int col, const char *fmt, ...);

/* A "warning:" at a location. Does not exit. */
void diag_warn_at(const char *file, int line, int col, const char *fmt, ...);

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

/* Attach a fix-it to the last diagnostic (or to its last note, which is
 * where "did you mean 'x'?" carries it): replace [col, end_col) of `line`
 * with `text`. Inserting is col == end_col; deleting is text "". */
void diag_fixit_at(const char *file, int line, int col, int end_col,
                   const char *text);

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

#endif
