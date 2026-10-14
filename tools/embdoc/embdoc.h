/* embdoc -- what the parts of the documentation generator share: a
 * growable byte buffer, the Markdown renderer's interface (md.c), the
 * host's directories (host.c), and the stylesheet and script every site
 * carries (assets.c). tools/embdoc/embdoc.c is the program and says what
 * it does. */
#ifndef EMBDOC_H
#define EMBDOC_H

#include <stddef.h>

/* ---- memory and text ------------------------------------------------- */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* A growable byte buffer, always NUL-terminated once anything is added. */
struct buf {
    char *p;
    size_t n, cap;
};

void buf_add(struct buf *b, const char *s, size_t n);
void buf_puts(struct buf *b, const char *s);
void buf_putc(struct buf *b, int c);
void buf_printf(struct buf *b, const char *fmt, ...);
/* s[0..n) with &, <, > and " written as entities */
void buf_html(struct buf *b, const char *s, size_t n);
/* s[0..n) as the inside of a JSON string */
void buf_json(struct buf *b, const char *s, size_t n);
void buf_free(struct buf *b);

/* The text a browser would show for a fragment of HTML: tags dropped,
 * the entities md.c writes decoded. Malloc'd. */
char *html_text(const char *html, size_t n);

/* ---- the Markdown renderer (md.c) ------------------------------------ */

struct md_heading {
    int level;          /* 1..6 */
    int line;           /* in the source */
    char *id;           /* the GitHub slug, made unique in the page */
    char *text;         /* what the heading says, as plain text */
    char *html;         /* ...and as the HTML inside its <hN> */
    size_t at;          /* where its <hN> starts in md_page.html */
};

struct md_page {
    /* in: what to do with each link's destination -- called once per
     * link, in source order, with the destination as written (escapes
     * undone) and its line; it returns the malloc'd href to write. NULL
     * writes every destination as it is. */
    char *(*link)(void *arg, const char *dest, int line);
    void *arg;

    /* out */
    struct buf html;
    struct md_heading *h;
    int nh, caph;
    char **ids;         /* explicit anchors: <a id="..."></a> */
    int nids, capids;
};

/* Render src[0..len) to pg->html, filling the headings and anchors. */
void md_render(struct md_page *pg, const char *src, size_t len);
void md_free(struct md_page *pg);

/* ---- the host (host.c: the only part that is not ISO C) ------------- */

/* 0 nothing there, 1 a regular file, 2 a directory */
int host_kind(const char *path);
/* A directory's names, unsorted, without the dot files; malloc'd. */
char **host_list(const char *dir, int *n);
/* 0 when the directory exists afterwards */
int host_mkdir(const char *path);

/* ---- the site's stylesheet and script (assets.c) -------------------- */

/* NULL-terminated lists of pieces, written one after the other. */
extern const char *const embdoc_css[];
extern const char *const embdoc_js[];
/* run in <head> before the page paints, so a dark page never flashes */
extern const char embdoc_theme_boot[];

#endif
