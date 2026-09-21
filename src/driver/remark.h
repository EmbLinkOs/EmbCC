/* Remarks — what a pass decided, and why, recorded where it decided it.
 *
 * Vision R2 / ADR-0004: "Every pass that makes a decision (inline / not
 * inline, vectorize / not, spill, fold, reject) emits a structured remark at
 * that point. 'Why?' features are queries over recorded remarks — never a
 * separate engine reconstructing reasons after the fact."
 *
 * The document is emphatic that this cannot be retrofitted, and it was
 * right: every pass written before this existed threw its reasoning away.
 * `inlinable()` returned 0 from twelve different places, each a different
 * reason, and all twelve were indistinguishable by the time anyone asked.
 * Adding the API is easy; adding it to a pass means going back through that
 * pass's decisions and naming them, which is the expensive part.
 *
 * ---- the shape of a remark ----
 *
 * A remark is data, not a sentence (§13). Rendering is the last step, so
 * `embcc why`, an editor and a build report read the same records:
 *
 *   pass      which pass decided            "inline"
 *   decision  what it decided               "not-inlined"
 *   subject   what about                    "add" / "sum:7"
 *   reason    a STABLE code for the cause   "callee-too-large"
 *   detail    the supporting facts          "24 instructions > budget 24"
 *   file/line where, when known
 *
 * `reason` is the part that must stay stable: it is what a query matches on
 * and what a person learns. `detail` is free text and may be improved at
 * will.
 *
 * ---- cost ----
 *
 * Off unless asked for (`-fremarks`), because a pass that always built
 * strings would slow every compile for a report almost nobody wants. Passes
 * therefore ask `remarks_on()` before doing any work to produce one.
 */
#ifndef EMBCC_REMARK_H
#define EMBCC_REMARK_H

struct outbuf;

struct remark {
    const char *pass;
    const char *decision;
    char *subject;
    const char *reason;
    char *detail;
    const char *file;
    int line;
};

/* Collection is off by default; the driver turns it on for -fremarks. */
void remarks_enable(int on);
int remarks_on(void);

/* Record one. `subject` and `fmt` are copied; the rest are expected to be
 * string literals, which is what keeps a remark cheap. A NULL fmt means
 * there are no supporting facts beyond the reason. */
void remark_add(const char *pass, const char *decision, const char *subject,
                const char *reason, const char *file, int line,
                const char *fmt, ...);

int remarks_count(void);
const struct remark *remark_get(int i);

/* Rendered once, at the end, like diagnostics. */
void remarks_render_text(struct outbuf *b);
void remarks_render_json(struct outbuf *b);

/* `embcc why <decision> [subject]` — the query layer (§19). Returns the
 * number of remarks that matched, so the caller can say "nothing recorded"
 * rather than printing an empty list that looks like an answer. */
int remarks_render_why(struct outbuf *b, const char *decision,
                       const char *subject);

void remarks_free(void);

#endif
