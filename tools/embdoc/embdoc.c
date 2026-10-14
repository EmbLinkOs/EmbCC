/* embdoc -- EmbCC's documentation, as a website, made by EmbCC's own tool.
 *
 *   embdoc site [--out DIR] [--base-url /EmbCC/] [--repo-url URL] [--root DIR]
 *   embdoc check [--root DIR]
 *   embdoc md FILE [--repo-url URL] [--root DIR]
 *
 * The documents are README.md, every .md under docs/ and each
 * tools/X/README.md, in the repository whose top is --root (the current
 * directory by default). md.c renders each one's Markdown as GitHub does,
 * and this file makes the site around them:
 *
 *   site   writes DIR (build/site by default): a page per document, X.md
 *          as X.html and a README.md as its directory's index.html, so
 *          README.md is the site's front page; each page with the
 *          navigation, an "On this page" list of its h2 and h3 headings,
 *          the path of its source and a link to edit it on GitHub; the
 *          stylesheet and script all pages share (embdoc.css, embdoc.js,
 *          from assets.c); search-index.json, which the script searches in
 *          the browser; and 404.html, whose links start with --base-url
 *          because GitHub Pages serves it at whatever address was missing.
 *          Broken links are counted on stderr, not fatal: that is check's.
 *   check  validates every relative link and every #anchor of every page,
 *          prints each broken one as `file:line: link -> reason`, and
 *          exits 1 when there is one.
 *   md     writes one file's rendered body to stdout, as the site would
 *          have it: what tests/golden/embdoc.sh reads.
 *
 * A link to another document becomes a relative link to its page, its
 * #anchor kept; anchors are GitHub's heading slugs, so a link that works
 * on github.com works here and check proves both. A link to any other file
 * of the repository (src/..., tests/..., Makefile) goes to that file on
 * GitHub, --repo-url/blob/main/PATH.
 *
 * The navigation: the introduction and the contents page, then the
 * Manual, the Tools and the Internals, each in the order docs/README.md
 * lists them (the rest after, by title), and the *-plan.md porting
 * diaries last, as "Porting history".
 *
 * ISO C, so EmbCC compiles it with its own headers; listing and making
 * directories, which ISO C cannot, is host.c's. No other dependency, and
 * no font, script or style from anywhere else. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "embdoc.h"

static const char *root = ".";
static const char *repo_url = "https://github.com/EmbLinkOs/EmbCC";
static const char *base_url = "/";

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "embdoc: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

/* ---- files ------------------------------------------------------------ */

/* root/rel, malloc'd */
static char *in_root(const char *rel)
{
    struct buf b = { 0 };
    buf_printf(&b, "%s/%s", root, rel);
    return b.p;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    struct buf b = { 0 };
    char tmp[65536];
    size_t got;
    buf_add(&b, "", 0);
    while ((got = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_add(&b, tmp, got);
    fclose(f);
    *len = b.n;
    return b.p;
}

/* 1 a regular file, 2 a directory, 0 nothing (a path under root) */
static int file_kind(const char *rel)
{
    char *p = in_root(rel);
    int r = host_kind(p);
    free(p);
    return r;
}

/* Make every directory on the way to path, the file's own name aside. */
static void make_dirs(const char *path)
{
    char *p = xstrdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/')
            continue;
        *s = 0;
        if (host_mkdir(p) != 0)
            die("cannot make the directory %s", p);
        *s = '/';
    }
    free(p);
}

static void write_file(const char *path, const char *data, size_t n)
{
    make_dirs(path);
    FILE *f = fopen(path, "wb");
    if (!f)
        die("cannot write %s", path);
    if (fwrite(data, 1, n, f) != n || fclose(f) != 0)
        die("cannot write %s", path);
}

static int ends_with(const char *s, const char *tail)
{
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && !strcmp(s + n - m, tail);
}

static int starts_with(const char *s, const char *head)
{
    return !strncmp(s, head, strlen(head));
}

/* ---- the pages -------------------------------------------------------- */

enum { G_START, G_MANUAL, G_TOOLS, G_INTERNALS, G_HISTORY, G_OTHER, NGROUP };
static const char *const group_name[NGROUP] = {
    "Start", "Manual", "Tools", "Internals", "Porting history", "More",
};

/* What a link resolved to. */
enum { L_EXT, L_SAME, L_PAGE, L_REPO, L_BAD };

struct link {
    int line;
    char *dest;         /* as written */
    int kind;
    int page;           /* L_PAGE: the page it goes to */
    char *path;         /* L_PAGE, L_REPO: the repository path */
    char *frag;         /* its #anchor, percent-decoded; NULL when none */
    const char *why;    /* L_BAD */
};

struct page {
    char *src;          /* repository path: docs/manual/invoking.md */
    char *out;          /* site path: docs/manual/invoking.html */
    char *title;        /* the first h1's text */
    char *label;        /* in the navigation */
    int group, rank;
    int rendered;
    struct md_page md;
    struct link *links;
    int nlinks, caplinks;
};

static struct page *pages;
static int npages, cappages;

static int find_page(const char *src)
{
    for (int k = 0; k < npages; k++)
        if (!strcmp(pages[k].src, src))
            return k;
    return -1;
}

/* X.md is X.html, and a README.md is its directory's index.html. */
static char *out_path(const char *src)
{
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    struct buf b = { 0 };
    if (!strcmp(base, "README.md")) {
        buf_add(&b, src, (size_t)(base - src));
        buf_puts(&b, "index.html");
    } else {
        size_t n = strlen(src);
        buf_add(&b, src, ends_with(src, ".md") ? n - 3 : n);
        buf_puts(&b, ".html");
    }
    return b.p;
}

static int add_page(const char *src)
{
    if (npages == cappages) {
        cappages = cappages ? cappages * 2 : 64;
        pages = xrealloc(pages, (size_t)cappages * sizeof *pages);
    }
    struct page *p = &pages[npages];
    memset(p, 0, sizeof *p);
    p->src = xstrdup(src);
    p->out = out_path(src);
    return npages++;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The names in a directory under root, sorted; dot files left out. */
static char **list_dir(const char *rel, int *n)
{
    char *full = in_root(rel);
    char **names = host_list(full, n);
    free(full);
    if (*n)
        qsort(names, (size_t)*n, sizeof *names, cmp_str);
    return names;
}

/* Every .md under the repository directory rel. */
static void walk(const char *rel)
{
    int n;
    char **names = list_dir(rel, &n);
    for (int k = 0; k < n; k++) {
        struct buf p = { 0 };
        buf_printf(&p, "%s/%s", rel, names[k]);
        int kind = file_kind(p.p);
        if (kind == 2)
            walk(p.p);
        else if (kind == 1 && ends_with(names[k], ".md"))
            add_page(p.p);
        buf_free(&p);
        free(names[k]);
    }
    free(names);
}

static void find_pages(void)
{
    if (file_kind("README.md") == 1)
        add_page("README.md");
    walk("docs");
    int n;
    char **names = list_dir("tools", &n);
    for (int k = 0; k < n; k++) {
        struct buf p = { 0 };
        buf_printf(&p, "tools/%s/README.md", names[k]);
        if (file_kind(p.p) == 1)
            add_page(p.p);
        buf_free(&p);
        free(names[k]);
    }
    free(names);
    if (npages == 0)
        die("no documents under %s (README.md, docs/, tools/*/README.md)", root);
}

/* ---- links ------------------------------------------------------------ */

/* dir/rel with . and .. resolved, without a trailing /; NULL when it
 * climbs out of the repository. A rel starting with / starts at the
 * repository's top, as on GitHub. */
static char *resolve(const char *dir, const char *rel)
{
    struct buf in = { 0 };
    if (rel[0] == '/')
        buf_puts(&in, rel + 1);
    else if (*dir)
        buf_printf(&in, "%s/%s", dir, rel);
    else
        buf_puts(&in, rel);
    buf_add(&in, "", 0);
    /* the components, .. popping one */
    char **part = xmalloc((in.n + 1) * sizeof *part);
    int np = 0, bad = 0;
    for (char *s = strtok(in.p, "/"); s; s = strtok(NULL, "/")) {
        if (!strcmp(s, "."))
            continue;
        if (!strcmp(s, "..")) {
            if (np == 0)
                bad = 1;
            else
                np--;
            continue;
        }
        part[np++] = s;
    }
    struct buf out = { 0 };
    buf_add(&out, "", 0);
    for (int k = 0; k < np; k++)
        buf_printf(&out, k ? "/%s" : "%s", part[k]);
    free(part);
    buf_free(&in);
    if (bad) {
        buf_free(&out);
        return NULL;
    }
    return out.p;
}

/* The relative URL from the site page from to the site path to. */
static char *rel_href(const char *from, const char *to)
{
    size_t k = 0, cut = 0;
    while (from[k] && from[k] == to[k]) {
        if (from[k] == '/')
            cut = k + 1;
        k++;
    }
    struct buf b = { 0 };
    buf_add(&b, "", 0);
    for (const char *p = from + cut; *p; p++)
        if (*p == '/')
            buf_puts(&b, "../");
    buf_puts(&b, to + cut);
    return b.p;
}

/* The way from a page up to the site's top: "../../" for docs/manual/x. */
static char *up_to_root(const char *out)
{
    struct buf b = { 0 };
    buf_add(&b, "", 0);
    for (; *out; out++)
        if (*out == '/')
            buf_puts(&b, "../");
    return b.p;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' :
           c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* %XX decoded, as a browser does before it looks the anchor up. */
static char *pct_decode(const char *s)
{
    char *r = xmalloc(strlen(s) + 1), *o = r;
    for (; *s; s++) {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *o++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
    return r;
}

static int has_scheme(const char *s)
{
    const char *p = s;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (p > s && ((*p >= '0' && *p <= '9') || *p == '+' || *p == '.' || *p == '-')))
        p++;
    return p - s >= 2 && *p == ':';
}

/* md.c's link callback: where a destination goes, recorded for check. */
static char *on_link(void *arg, const char *dest, int line)
{
    struct page *pg = arg;
    if (pg->nlinks == pg->caplinks) {
        pg->caplinks = pg->caplinks ? pg->caplinks * 2 : 32;
        pg->links = xrealloc(pg->links, (size_t)pg->caplinks * sizeof *pg->links);
    }
    struct link *l = &pg->links[pg->nlinks++];
    memset(l, 0, sizeof *l);
    l->line = line;
    l->dest = xstrdup(dest);
    l->page = -1;

    if (has_scheme(dest)) {
        l->kind = L_EXT;
        return xstrdup(dest);
    }
    const char *hash = strchr(dest, '#');
    if (hash)
        l->frag = pct_decode(hash + 1);
    if (dest[0] == '#') {
        l->kind = L_SAME;
        return xstrdup(dest);
    }
    size_t plen = strcspn(dest, "#?");
    char *path = xstrndup(dest, plen);
    char *dir = xstrdup(pg->src);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        dir[0] = 0;
    char *rp = plen ? resolve(dir, path) : NULL;
    free(dir);
    free(path);
    l->kind = L_BAD;
    if (!plen) {
        l->why = "an empty path";
        return xstrdup(dest);
    }
    if (!rp) {
        l->why = "leaves the repository";
        return xstrdup(dest);
    }
    l->path = rp;
    int target = find_page(rp);
    if (target < 0 && file_kind(rp) == 2) {
        /* a directory with a README on the site: its index page */
        struct buf r = { 0 };
        buf_printf(&r, "%s%sREADME.md", rp, *rp ? "/" : "");
        target = find_page(r.p);
        buf_free(&r);
    }
    struct buf h = { 0 };
    buf_add(&h, "", 0);
    if (target >= 0) {
        l->kind = L_PAGE;
        l->page = target;
        if (strcmp(pages[target].out, pg->out) != 0) {
            char *r = rel_href(pg->out, pages[target].out);
            buf_puts(&h, r);
            free(r);
        } else if (!hash) {
            const char *b = strrchr(pg->out, '/');
            buf_puts(&h, b ? b + 1 : pg->out);
        }
    } else if (file_kind(rp)) {
        l->kind = L_REPO;
        buf_printf(&h, "%s/%s/main/%s", repo_url,
                   file_kind(rp) == 2 ? "tree" : "blob", rp);
    } else {
        l->why = "no such file";
        buf_free(&h);
        return xstrdup(dest);
    }
    if (hash)
        buf_puts(&h, hash);
    return h.p;
}

/* ---- rendering -------------------------------------------------------- */

static void render_page(int k)
{
    struct page *p = &pages[k];
    if (p->rendered)
        return;
    char *path = in_root(p->src);
    size_t len;
    char *src = slurp(path, &len);
    if (!src)
        die("cannot read %s", path);
    free(path);
    p->md.link = on_link;
    p->md.arg = p;
    md_render(&p->md, src, len);
    free(src);
    p->rendered = 1;
    for (int h = 0; h < p->md.nh; h++)
        if (p->md.h[h].level == 1) {
            p->title = xstrdup(p->md.h[h].text);
            break;
        }
    if (!p->title) {
        const char *b = strrchr(p->src, '/');
        p->title = xstrdup(b ? b + 1 : p->src);
    }
}

/* The navigation's group and order: docs/README.md's order of mention,
 * and the pages it does not mention after, by title. */
enum { UNLISTED = 1 << 20 };

static void classify(void)
{
    int contents = find_page("docs/README.md");
    for (int k = 0; k < npages; k++) {
        struct page *p = &pages[k];
        const char *s = p->src;
        if (!strcmp(s, "README.md") || !strcmp(s, "docs/README.md"))
            p->group = G_START;
        else if (starts_with(s, "docs/manual/tools/") ||
                 (starts_with(s, "tools/") && ends_with(s, "/README.md")))
            p->group = G_TOOLS;
        else if (starts_with(s, "docs/manual/"))
            p->group = G_MANUAL;
        else if (starts_with(s, "docs/internals/") && ends_with(s, "-plan.md"))
            p->group = G_HISTORY;
        else if (starts_with(s, "docs/internals/"))
            p->group = G_INTERNALS;
        else
            p->group = G_OTHER;
        p->rank = !strcmp(s, "README.md") ? -2 :
                  !strcmp(s, "docs/README.md") ? -1 : UNLISTED;

        /* the label: the title up to " — " or ": " */
        const char *t = p->title, *cut = strstr(t, " \xe2\x80\x94 ");
        const char *colon = strstr(t, ": ");
        if (!cut || (colon && colon < cut))
            cut = colon;
        if (!strcmp(s, "README.md"))
            p->label = xstrdup("Introduction");
        else if (!strcmp(s, "docs/README.md"))
            p->label = xstrdup("Contents");
        else
            p->label = cut ? xstrndup(t, (size_t)(cut - t)) : xstrdup(t);
    }
    if (contents >= 0) {
        struct page *c = &pages[contents];
        for (int k = 0; k < c->nlinks; k++) {
            int t = c->links[k].page;
            if (t >= 0 && pages[t].rank == UNLISTED)
                pages[t].rank = k;
        }
    }
}

static int cmp_nav(const void *a, const void *b)
{
    const struct page *p = &pages[*(const int *)a], *q = &pages[*(const int *)b];
    if (p->group != q->group)
        return p->group - q->group;
    if (p->rank != q->rank)
        return p->rank < q->rank ? -1 : 1;
    return strcmp(p->title, q->title);
}

static int *nav;        /* the pages in navigation order */

static void order_nav(void)
{
    nav = xmalloc((size_t)npages * sizeof *nav);
    for (int k = 0; k < npages; k++)
        nav[k] = k;
    qsort(nav, (size_t)npages, sizeof *nav, cmp_nav);
}

/* ---- check ------------------------------------------------------------ */

static int has_anchor(const struct md_page *m, const char *frag)
{
    for (int k = 0; k < m->nh; k++)
        if (!strcmp(m->h[k].id, frag))
            return 1;
    for (int k = 0; k < m->nids; k++)
        if (!strcmp(m->ids[k], frag))
            return 1;
    return 0;
}

/* A Markdown file that is not on the site, rendered for its anchors only. */
static int other_md(const char *rel)
{
    int k = find_page(rel);
    if (k < 0)
        k = add_page(rel);
    render_page(k);
    return k;
}

/* Every broken link of page k, to out; their number. */
static int check_page(int k, FILE *out)
{
    struct page *p = &pages[k];
    int bad = 0;
    for (int j = 0; j < p->nlinks; j++) {
        struct link *l = &p->links[j];
        struct buf why = { 0 };
        if (l->kind == L_BAD) {
            buf_puts(&why, l->why);
        } else if (l->frag && *l->frag && l->kind == L_SAME) {
            if (!has_anchor(&p->md, l->frag))
                buf_printf(&why, "no anchor #%s on this page", l->frag);
        } else if (l->frag && *l->frag && l->kind == L_PAGE) {
            render_page(l->page);
            if (!has_anchor(&pages[l->page].md, l->frag))
                buf_printf(&why, "no anchor #%s in %s", l->frag, pages[l->page].src);
        } else if (l->frag && *l->frag && l->kind == L_REPO &&
                   ends_with(l->path, ".md")) {
            int t = other_md(l->path);
            p = &pages[k];      /* pages may have moved */
            l = &p->links[j];
            if (!has_anchor(&pages[t].md, l->frag))
                buf_printf(&why, "no anchor #%s in %s", l->frag, pages[t].src);
        }
        if (why.n) {
            fprintf(out, "%s:%d: %s -> %s\n", p->src, l->line, l->dest, why.p);
            bad++;
        }
        buf_free(&why);
    }
    return bad;
}

static int cmd_check(void)
{
    int n = npages, bad = 0, links = 0;
    for (int k = 0; k < n; k++)
        render_page(k);
    for (int k = 0; k < n; k++) {
        bad += check_page(k, stdout);
        links += pages[k].nlinks;
    }
    fprintf(stderr, "embdoc: %d links in %d documents, %d broken\n", links, n, bad);
    return bad ? 1 : 0;
}

/* ---- the site --------------------------------------------------------- */

/* The icons, drawn on a 24-unit grid. Sized in the markup too, so a page
 * read without its stylesheet does not show them a screen wide. */
#define SVG_OPEN \
    "<svg viewBox=\"0 0 24 24\" width=\"20\" height=\"20\" aria-hidden=\"true\" " \
    "fill=\"none\" "

static const char svg_chip[] =
    SVG_OPEN
    "stroke=\"currentColor\" stroke-width=\"1.6\" stroke-linecap=\"round\">"
    "<rect x=\"6\" y=\"6\" width=\"12\" height=\"12\" rx=\"1.5\"/>"
    "<path d=\"M9.5 2.5V6M14.5 2.5V6M9.5 18v3.5M14.5 18v3.5M2.5 9.5H6M2.5 "
    "14.5H6M18 9.5h3.5M18 14.5h3.5\"/>"
    "<circle cx=\"9\" cy=\"9\" r=\"1.1\" fill=\"currentColor\" stroke=\"none\"/>"
    "</svg>";

/* the same chip, as the tab's icon */
static const char favicon[] =
    "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' "
    "viewBox='0 0 24 24' fill='none' stroke='%230a7552' stroke-width='2' "
    "stroke-linecap='round'%3E%3Crect x='6' y='6' width='12' height='12' "
    "rx='1.5'/%3E%3Cpath d='M9.5 2.5V6M14.5 2.5V6M9.5 18v3.5M14.5 18v3.5M2.5 "
    "9.5H6M2.5 14.5H6M18 9.5h3.5M18 14.5h3.5'/%3E%3C/svg%3E";

static const char svg_menu[] =
    SVG_OPEN
    "stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\">"
    "<path d=\"M4 7h16M4 12h16M4 17h16\"/></svg>";

static const char svg_search[] =
    SVG_OPEN
    "stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\">"
    "<circle cx=\"10.5\" cy=\"10.5\" r=\"6\"/><path d=\"M15 15l5 5\"/></svg>";

static const char svg_theme[] =
    SVG_OPEN
    "stroke=\"currentColor\" stroke-width=\"1.8\">"
    "<circle cx=\"12\" cy=\"12\" r=\"8\"/>"
    "<path d=\"M12 4a8 8 0 0 1 0 16z\" fill=\"currentColor\" stroke=\"none\"/>"
    "</svg>";

static void put_esc(struct buf *b, const char *s)
{
    buf_html(b, s, strlen(s));
}

/* The page's head and top bar; up is the way to the site's top. The ids
 * embdoc gives its own elements -- Search, Results, Nav, Content -- are
 * capitalised because a heading's slug never is: the search box was "q"
 * until embar's page grew a heading `q`. */
static void page_head(struct buf *b, const char *title, const char *up)
{
    buf_puts(b, "<!doctype html>\n<html lang=\"en\" data-root=\"");
    put_esc(b, up);
    buf_puts(b, "\">\n<head>\n<meta charset=\"utf-8\">\n"
                "<meta name=\"viewport\" "
                "content=\"width=device-width, initial-scale=1\">\n"
                "<meta name=\"color-scheme\" content=\"light dark\">\n"
                "<meta name=\"generator\" content=\"embdoc\">\n<title>");
    put_esc(b, title);
    buf_puts(b, "</title>\n<link rel=\"icon\" href=\"");
    buf_puts(b, favicon);
    buf_puts(b, "\">\n<link rel=\"stylesheet\" href=\"");
    put_esc(b, up);
    buf_puts(b, "embdoc.css\">\n<script>");
    buf_puts(b, embdoc_theme_boot);
    buf_puts(b, "</script>\n</head>\n<body>\n"
                "<a class=\"skip\" href=\"#Content\">Skip to the content</a>\n"
                "<header class=\"bar\">\n"
                "<button class=\"menu\" type=\"button\" "
                "aria-label=\"Show the navigation\" aria-expanded=\"false\" "
                "aria-controls=\"Nav\">");
    buf_puts(b, svg_menu);
    buf_puts(b, "</button>\n<a class=\"brand\" href=\"");
    put_esc(b, up);
    buf_puts(b, "index.html\">");
    buf_puts(b, svg_chip);
    buf_puts(b, "<span class=\"name\">EmbCC</span><span class=\"sub\">docs</span></a>\n"
                "<div class=\"search\" role=\"search\">");
    buf_puts(b, svg_search);
    buf_puts(b, "<input id=\"Search\" type=\"search\" "
                "placeholder=\"Search the documentation\" autocomplete=\"off\" "
                "spellcheck=\"false\" aria-label=\"Search the documentation\" "
                "aria-controls=\"Results\"><kbd>/</kbd>\n"
                "<div id=\"Results\" class=\"results\" role=\"listbox\" hidden>"
                "</div></div>\n"
                "<a class=\"repo\" href=\"");
    put_esc(b, repo_url);
    buf_puts(b, "\">Repository</a>\n"
                "<button class=\"theme\" type=\"button\" aria-label=\"Dark theme\" "
                "aria-pressed=\"false\">");
    buf_puts(b, svg_theme);
    buf_puts(b, "</button>\n</header>\n");
}

static void page_nav(struct buf *b, int cur, const char *up)
{
    buf_puts(b, "<nav id=\"Nav\" class=\"side\" aria-label=\"Documentation\">\n");
    int open = -1;
    for (int k = 0; k < npages; k++) {
        const struct page *p = &pages[nav[k]];
        if (p->group != open) {
            if (open >= 0)
                buf_puts(b, "</ul></div>\n");
            open = p->group;
            buf_printf(b, "<div class=\"group\"><h2>%s</h2><ul>\n", group_name[open]);
        }
        buf_puts(b, "<li><a href=\"");
        put_esc(b, up);
        put_esc(b, p->out);
        buf_puts(b, nav[k] == cur ? "\" aria-current=\"page\">" : "\">");
        put_esc(b, p->label);
        buf_puts(b, "</a></li>\n");
    }
    if (open >= 0)
        buf_puts(b, "</ul></div>\n");
    buf_puts(b, "</nav>\n");
}

/* "On this page": the h2 and h3 headings. Empty when there are fewer
 * than two. */
static void page_toc(struct buf *b, const struct md_page *m)
{
    int n = 0;
    for (int k = 0; k < m->nh; k++)
        n += m->h[k].level == 2 || m->h[k].level == 3;
    if (n < 2)
        return;
    buf_puts(b, "<aside class=\"toc\" aria-label=\"On this page\"><details>"
                "<summary>On this page</summary><ul>\n");
    for (int k = 0; k < m->nh; k++) {
        const struct md_heading *h = &m->h[k];
        if (h->level != 2 && h->level != 3)
            continue;
        buf_printf(b, "<li class=\"h%d\"><a href=\"#", h->level);
        put_esc(b, h->id);
        buf_puts(b, "\">");
        /* the heading's own markup -- its code in code -- less any link
         * or anchor in it, which cannot sit inside this link */
        for (const char *s = h->html; *s;) {
            if (!strncmp(s, "<a ", 3) || !strncmp(s, "</a>", 4)) {
                const char *e = strchr(s, '>');
                s = e ? e + 1 : s + strlen(s);
                continue;
            }
            buf_putc(b, *s++);
        }
        buf_puts(b, "</a></li>\n");
    }
    buf_puts(b, "</ul></details></aside>\n");
}

static void pager_link(struct buf *b, int k, const char *cls, const char *dir,
                       const char *up)
{
    buf_printf(b, "<a class=\"%s\" href=\"", cls);
    put_esc(b, up);
    put_esc(b, pages[k].out);
    buf_printf(b, "\"><span class=\"dir\">%s</span>", dir);
    put_esc(b, pages[k].label);
    buf_puts(b, "</a>");
}

static void page_html(struct buf *b, int pos)
{
    int cur = nav[pos];
    const struct page *p = &pages[cur];
    char *up = up_to_root(p->out);
    struct buf title = { 0 };
    buf_add(&title, "", 0);
    if (!strcmp(p->src, "README.md"))
        buf_puts(&title, p->title);
    else
        buf_printf(&title, "%s \xc2\xb7 EmbCC", p->title);
    page_head(b, title.p, up);
    buf_free(&title);
    buf_puts(b, "<div class=\"shell\">\n");
    page_nav(b, cur, up);
    page_toc(b, &p->md);
    buf_printf(b, "<main id=\"Content\">\n<div class=\"meta\">"
                  "<span class=\"group\">%s</span><a class=\"src\" href=\"",
               group_name[p->group]);
    put_esc(b, repo_url);
    buf_puts(b, "/blob/main/");
    put_esc(b, p->src);
    buf_puts(b, "\" title=\"The source of this page\">");
    put_esc(b, p->src);
    buf_puts(b, "</a><a class=\"edit\" href=\"");
    put_esc(b, repo_url);
    buf_puts(b, "/edit/main/");
    put_esc(b, p->src);
    buf_puts(b, "\">Edit on GitHub</a></div>\n<article class=\"doc\">\n");
    buf_add(b, p->md.html.p, p->md.html.n);
    buf_puts(b, "</article>\n");
    if (npages > 1) {
        buf_puts(b, "<nav class=\"pager\" aria-label=\"Previous and next\">");
        if (pos > 0)
            pager_link(b, nav[pos - 1], "prev", "Previous", up);
        if (pos + 1 < npages)
            pager_link(b, nav[pos + 1], "next", "Next", up);
        buf_puts(b, "</nav>\n");
    }
    buf_puts(b, "<footer class=\"foot\">Generated by embdoc from <code>");
    put_esc(b, p->src);
    buf_puts(b, "</code>.</footer>\n</main>\n</div>\n<script src=\"");
    put_esc(b, up);
    buf_puts(b, "embdoc.js\"></script>\n</body>\n</html>\n");
    free(up);
}

/* GitHub Pages serves 404.html for any address with nothing behind it,
 * so its links cannot be relative: they start at --base-url. */
static void page_404(struct buf *b)
{
    page_head(b, "Page not found \xc2\xb7 EmbCC", base_url);
    buf_puts(b, "<div class=\"shell\">\n");
    page_nav(b, -1, base_url);
    buf_puts(b, "<main id=\"Content\">\n<article class=\"doc\">\n"
                "<h1>Page not found</h1>\n<p>Nothing is published at this address. "
                "Search for it above, or start from the <a href=\"");
    put_esc(b, base_url);
    buf_puts(b, "index.html\">introduction</a> or the <a href=\"");
    put_esc(b, base_url);
    buf_puts(b, "docs/index.html\">contents</a>.</p>\n</article>\n</main>\n</div>\n"
                "<script src=\"");
    put_esc(b, base_url);
    buf_puts(b, "embdoc.js\"></script>\n</body>\n</html>\n");
}

/* Text for the search index: one line, white space collapsed. */
static void put_text(struct buf *b, const char *html, size_t n)
{
    char *t = html_text(html, n);
    int space = 1;
    size_t start = b->n;
    for (const char *s = t; *s; s++) {
        if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') {
            if (!space)
                buf_putc(b, ' ');
            space = 1;
        } else {
            buf_putc(b, *s);
            space = 0;
        }
    }
    while (b->n > start && b->p[b->n - 1] == ' ')
        b->p[--b->n] = 0;
    free(t);
}

/* search-index.json: the pages, and each one's sections -- the text
 * before its first h2, and every h2 and h3 with what follows it to the
 * next -- as {"p": page, "h": heading, "a": anchor, "x": text}. */
static void search_index(struct buf *b)
{
    buf_puts(b, "{\"pages\":[\n");
    for (int k = 0; k < npages; k++) {
        const struct page *p = &pages[nav[k]];
        buf_puts(b, k ? ",\n{\"t\":\"" : "{\"t\":\"");
        buf_json(b, p->title, strlen(p->title));
        buf_puts(b, "\",\"u\":\"");
        buf_json(b, p->out, strlen(p->out));
        buf_puts(b, "\"}");
    }
    buf_puts(b, "\n],\"sections\":[\n");
    int first = 1;
    for (int k = 0; k < npages; k++) {
        const struct md_page *m = &pages[nav[k]].md;
        const char *html = m->html.p;
        size_t start = 0;
        const char *head = pages[nav[k]].title, *anchor = "";
        int s = 0;
        for (;;) {
            /* the section runs to the next h1..h3 */
            while (s < m->nh && m->h[s].level > 3)
                s++;
            size_t end = s < m->nh ? m->h[s].at : m->html.n;
            struct buf x = { 0 };
            buf_add(&x, "", 0);
            if (end > start)
                put_text(&x, html + start, end - start);
            if (x.n || *anchor) {
                buf_puts(b, first ? "{\"p\":" : ",\n{\"p\":");
                first = 0;
                buf_printf(b, "%d,\"h\":\"", k);
                buf_json(b, head, strlen(head));
                buf_puts(b, "\",\"a\":\"");
                buf_json(b, anchor, strlen(anchor));
                buf_puts(b, "\",\"x\":\"");
                buf_json(b, x.p, x.n);
                buf_puts(b, "\"}");
            }
            buf_free(&x);
            if (s >= m->nh)
                break;
            /* the next section starts after its heading's </hN> */
            const char *close = strstr(html + m->h[s].at, "</h");
            start = close ? (size_t)(close - html) + 5 : m->h[s].at;
            if (m->h[s].level == 1) {
                head = pages[nav[k]].title;
                anchor = "";
            } else {
                head = m->h[s].text;
                anchor = m->h[s].id;
            }
            s++;
        }
    }
    buf_puts(b, "\n]}\n");
}

static char *out_file(const char *dir, const char *rel)
{
    struct buf b = { 0 };
    buf_printf(&b, "%s/%s", dir, rel);
    return b.p;
}

static void write_buf(const char *dir, const char *rel, struct buf *b)
{
    char *path = out_file(dir, rel);
    write_file(path, b->p ? b->p : "", b->n);
    free(path);
    buf_free(b);
}

static void write_parts(const char *dir, const char *rel, const char *const *parts)
{
    struct buf b = { 0 };
    for (int k = 0; parts[k]; k++)
        buf_puts(&b, parts[k]);
    write_buf(dir, rel, &b);
}

static int cmd_site(const char *dir)
{
    int n = npages, bad = 0;
    for (int k = 0; k < n; k++)
        render_page(k);
    classify();
    order_nav();
    FILE *devnull = fopen("/dev/null", "w");
    for (int k = 0; k < n; k++)
        bad += check_page(k, devnull ? devnull : stderr);
    if (devnull)
        fclose(devnull);
    npages = n;         /* documents check read for anchors are not pages */
    for (int k = 0; k < npages; k++) {
        struct buf b = { 0 };
        page_html(&b, k);
        write_buf(dir, pages[nav[k]].out, &b);
    }
    struct buf b = { 0 };
    page_404(&b);
    write_buf(dir, "404.html", &b);
    search_index(&b);
    write_buf(dir, "search-index.json", &b);
    write_parts(dir, "embdoc.css", embdoc_css);
    write_parts(dir, "embdoc.js", embdoc_js);
    fprintf(stderr, "embdoc: %d pages in %s", npages, dir);
    if (bad)
        fprintf(stderr, ", %d broken links (embdoc check lists them)", bad);
    fputc('\n', stderr);
    return 0;
}

static int cmd_md(const char *file)
{
    /* the file's repository path */
    const char *rel = file;
    size_t rl = strlen(root);
    if (!strcmp(root, ".")) {
        while (starts_with(rel, "./"))
            rel += 2;
    } else if (!strncmp(rel, root, rl) && rel[rl] == '/') {
        rel += rl + 1;
    }
    int k = find_page(rel);
    if (k < 0)
        k = add_page(rel);
    struct page *p = &pages[k];
    size_t len;
    char *src = slurp(file, &len);
    if (!src)
        die("cannot read %s", file);
    p->md.link = on_link;
    p->md.arg = p;
    md_render(&p->md, src, len);
    free(src);
    fwrite(p->md.html.p, 1, p->md.html.n, stdout);
    for (int j = 0; j < p->nlinks; j++)
        if (p->links[j].kind == L_BAD)
            fprintf(stderr, "embdoc: %s:%d: %s -> %s\n", p->src, p->links[j].line,
                    p->links[j].dest, p->links[j].why);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: embdoc site [--out DIR] [--base-url /PATH/] [--repo-url URL]\n"
            "                   [--root DIR]\n"
            "       embdoc check [--root DIR]\n"
            "       embdoc md FILE [--repo-url URL] [--root DIR]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : NULL, *out = "build/site", *file = NULL;
    if (!cmd || !strcmp(cmd, "-h") || !strcmp(cmd, "--help"))
        usage();
    for (int k = 2; k < argc; k++) {
        const char *a = argv[k];
        const char *v = k + 1 < argc ? argv[k + 1] : NULL;
        if (!strcmp(a, "--out") && v) {
            out = v;
            k++;
        } else if (!strcmp(a, "--base-url") && v) {
            base_url = v;
            k++;
        } else if (!strcmp(a, "--repo-url") && v) {
            repo_url = v;
            k++;
        } else if (!strcmp(a, "--root") && v) {
            root = v;
            k++;
        } else if (a[0] != '-' && !file) {
            file = a;
        } else {
            usage();
        }
    }
    /* --base-url ends with a /, --repo-url does not */
    if (!ends_with(base_url, "/")) {
        struct buf b = { 0 };
        buf_printf(&b, "%s/", base_url);
        base_url = b.p;
    }
    if (ends_with(repo_url, "/") && strlen(repo_url) > 1)
        repo_url = xstrndup(repo_url, strlen(repo_url) - 1);
    find_pages();
    if (!strcmp(cmd, "site") && !file)
        return cmd_site(out);
    if (!strcmp(cmd, "check") && !file)
        return cmd_check();
    if (!strcmp(cmd, "md") && file)
        return cmd_md(file);
    usage();
    return 2;
}
