/* The remark store. See remark.h for what a remark is and why it exists.
 *
 * Deliberately the same shape as src/driver/diag.c: records are collected
 * during the compile and rendered once at the end, in whichever format was
 * asked for. Nothing here prints as it goes, because a remark that is
 * printed immediately cannot be queried, sorted, or counted — and §19's
 * whole premise is that "why" is a query over records.
 */
#include "remark.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static struct remark *g_rem;
static int g_n, g_cap;
static int g_on;

void remarks_enable(int on) { g_on = on; }
int remarks_on(void) { return g_on; }

void remark_add(const char *pass, const char *decision, const char *subject,
                const char *reason, const char *file, int line,
                const char *fmt, ...)
{
    if (!g_on)
        return;
    if (g_n == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_rem = xrealloc(g_rem, (size_t)g_cap * sizeof *g_rem);
    }
    struct remark *r = &g_rem[g_n++];
    memset(r, 0, sizeof *r);
    r->pass = pass;
    r->decision = decision;
    r->reason = reason;
    r->file = file;
    r->line = line;
    r->subject = subject ? xstrndup(subject, strlen(subject)) : NULL;
    if (fmt) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        r->detail = xstrndup(buf, strlen(buf));
    }
}

int remarks_count(void) { return g_n; }

const struct remark *remark_get(int i)
{
    return i >= 0 && i < g_n ? &g_rem[i] : NULL;
}

/* One line each, in the order the passes decided — which is the order the
 * compiler thought, and reading it top to bottom is reading the compile. */
void remarks_render_text(struct outbuf *b)
{
    for (int i = 0; i < g_n; i++) {
        const struct remark *r = &g_rem[i];
        if (r->file && r->line)
            ob_fmt(b, "%s:%d: ", r->file, r->line);
        ob_fmt(b, "remark: %s", r->decision);
        if (r->subject)
            ob_fmt(b, " '%s'", r->subject);
        if (r->detail)
            ob_fmt(b, ": %s", r->detail);
        ob_fmt(b, " [%s/%s]\n", r->pass, r->reason);
    }
}

static void json_str(struct outbuf *b, const char *s)
{
    ob_ch(b, '"');
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '"' || *p == '\\') { ob_ch(b, '\\'); ob_ch(b, *p); }
        else if (*p == '\n')         ob_str(b, "\\n");
        else if (*p == '\t')         ob_str(b, "\\t");
        else if ((unsigned char)*p < 0x20) ob_fmt(b, "\\u%04x", *p);
        else                         ob_ch(b, *p);
    }
    ob_ch(b, '"');
}

void remarks_render_json(struct outbuf *b)
{
    ob_str(b, "[\n");
    for (int i = 0; i < g_n; i++) {
        const struct remark *r = &g_rem[i];
        ob_str(b, "  {\"pass\": ");      json_str(b, r->pass);
        ob_str(b, ", \"decision\": ");   json_str(b, r->decision);
        ob_str(b, ", \"subject\": ");    json_str(b, r->subject);
        ob_str(b, ", \"reason\": ");     json_str(b, r->reason);
        if (r->detail) {
            ob_str(b, ", \"detail\": "); json_str(b, r->detail);
        }
        if (r->file && r->line) {
            ob_str(b, ", \"location\": {\"file\": ");
            json_str(b, r->file);
            ob_fmt(b, ", \"line\": %d}", r->line);
        }
        ob_fmt(b, "}%s\n", i + 1 < g_n ? "," : "");
    }
    ob_str(b, "]\n");
}

/* §19: the answer to a question, not a dump. A remark is written for a
 * reader who asked one thing, so the rendering names the decision, what it
 * was about, and the fact that settled it — and says plainly when nothing
 * was recorded, since an empty list otherwise reads as "there was no
 * reason". */
int remarks_render_why(struct outbuf *b, const char *decision,
                       const char *subject)
{
    int hits = 0;
    for (int i = 0; i < g_n; i++) {
        const struct remark *r = &g_rem[i];
        if (strcmp(r->decision, decision) != 0)
            continue;
        if (subject && (!r->subject || strcmp(r->subject, subject) != 0))
            continue;
        hits++;
        ob_fmt(b, "%s", r->subject ? r->subject : "(unnamed)");
        if (r->file && r->line)
            ob_fmt(b, " (%s:%d)", r->file, r->line);
        ob_fmt(b, ": %s\n", r->decision);
        ob_fmt(b, "  because %s", r->reason);
        if (r->detail)
            ob_fmt(b, " — %s", r->detail);
        ob_fmt(b, "\n  decided by the %s pass\n", r->pass);
    }
    return hits;
}

void remarks_free(void)
{
    for (int i = 0; i < g_n; i++) {
        free(g_rem[i].subject);
        free(g_rem[i].detail);
    }
    free(g_rem);
    g_rem = NULL;
    g_n = g_cap = 0;
}
