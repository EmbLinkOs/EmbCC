/* EmbIR's textual form, read back — the other half of §9.1.
 *
 *     print -> parse -> identical IR
 *
 * which is what makes a pass testable text-in/text-out (§9.1, §30): hand it
 * IR, run it, compare the IR that comes out, with no C source and no
 * backend in the loop.
 *
 * ---- what "identical" means here ----
 *
 * The check this supports is that **printing a parsed unit reproduces the
 * text it was parsed from**, byte for byte. That is the honest formulation:
 * it proves the textual form is a lossless encoding of everything it
 * claims to carry, and it fails the moment the printer emits something the
 * parser cannot read back, which is the drift the requirement exists to
 * prevent.
 *
 * It does NOT claim to reconstruct the parts of struct ir_func that EmbIR
 * deliberately does not carry: `src` is NULL in a parsed unit, so a parsed
 * IR can be printed, analysed and transformed, but not handed to the DWARF
 * emitter, which wants the type graph (see irprint.c). Code generation
 * needs none of that — the ABI answers travel in the IR — but this parser
 * is for testing passes, so it stops at the boundary rather than pretending.
 *
 * ---- shape ----
 *
 * One line at a time, because the printer emits one thing per line. No
 * lookahead, no backtracking: an error names the line and what was expected,
 * since the most likely reader of that message is someone who just changed
 * the printer.
 */
#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../parse/ast.h"

struct p {
    const char *file;
    char *text;          /* mutable: lines are NUL-terminated in place */
    char *cur;           /* the rest of the current line */
    int line;
    struct ir_unit *u;
    struct ir_func *fn;  /* the function being filled, or NULL */
};

static void perr(struct p *p, const char *what)
{
    diag_fatal(p->file, p->line, "EmbIR: expected %s", what);
}

/* ---- lexing one line ---- */

static void skip_ws(struct p *p)
{
    while (*p->cur == ' ' || *p->cur == '\t')
        p->cur++;
}

/* The next whitespace-delimited word, NUL-terminated in place, or NULL at
 * the end of the line. A trailing `; comment` ends the line. */
static char *word(struct p *p)
{
    skip_ws(p);
    if (!*p->cur)
        return NULL;
    char *s = p->cur;
    while (*p->cur && *p->cur != ' ' && *p->cur != '\t')
        p->cur++;
    if (*p->cur)
        *p->cur++ = 0;
    return s;
}

static int eat(struct p *p, const char *lit)
{
    char *save = p->cur;
    int saveline = p->line;
    char *w = word(p);
    if (w && !strcmp(w, lit))
        return 1;
    p->cur = save;
    p->line = saveline;
    return 0;
}

/* `key=value` on the current line, anywhere in it. The printer emits these
 * in a fixed order, but reading them by name means a new one can be added
 * without the parser caring where it went. */
static int kv(const char *line, const char *key, long *out)
{
    size_t kl = strlen(key);
    for (const char *s = line; (s = strstr(s, key)) != NULL; s += kl) {
        if (s != line && s[-1] != ' ')
            continue;                  /* `align=` must not match `user_align=` */
        if (s[kl] != '=')
            continue;
        *out = strtol(s + kl + 1, NULL, 10);
        return 1;
    }
    return 0;
}

static int has_flag(const char *line, const char *flag)
{
    size_t fl = strlen(flag);
    for (const char *s = line; (s = strstr(s, flag)) != NULL; s += fl) {
        if (s != line && s[-1] != ' ' && s[-1] != '\t')
            continue;
        char after = s[fl];
        /* A TAB counts: the printer separates an instruction from its
         * location comment with one, so the last flag on a line is followed
         * by a tab and nothing else. */
        if (!after || after == ' ' || after == '\t' || after == '\n')
            return 1;
    }
    return 0;
}

/* ---- operands ---- */

/* `%12` -> 12, `-1` where absent. */
static int vreg(struct p *p, char *w)
{
    if (!w || *w != '%')
        perr(p, "a %vreg operand");
    return (int)strtol(w + 1, NULL, 10);
}

/* `v3` -> 3 (a frame slot, not a temp). */
static int slot(struct p *p, char *w)
{
    if (!w || *w != 'v')
        perr(p, "a vN frame slot");
    return (int)strtol(w + 1, NULL, 10);
}

/* `L7` or `L7:` -> 7. */
static int labelno(struct p *p, char *w)
{
    if (!w || *w != 'L')
        perr(p, "an LN label");
    return (int)strtol(w + 1, NULL, 10);
}

/* `[%7]` -> 7 */
static int addr_operand(struct p *p, char *w)
{
    if (!w || w[0] != '[' || w[1] != '%')
        perr(p, "a [%vreg] address");
    return (int)strtol(w + 2, NULL, 10);
}

/* A mnemonic's suffixes: `.8:4sfv` -> w=8 size=4 sign flt vol. The opcode
 * name is everything before the first '.' or ':'. */
static void split_mnemonic(char *m, char **base, struct ir_ins *in)
{
    *base = m;
    char *q = m;
    while (*q && *q != '.' && *q != ':')
        q++;
    /* The separator is part of the suffix, so the suffix is copied out
     * before the mnemonic is truncated -- writing the NUL first would eat
     * the very character that says whether the number is a width or a
     * memory size. */
    char suf[32];
    snprintf(suf, sizeof suf, "%s", q);
    *q = 0;
    for (char *s = suf; *s; s++) {
        if (*s == '.')      { in->w = (int)strtol(s + 1, &s, 10); s--; }
        else if (*s == ':') { in->size = (int)strtol(s + 1, &s, 10); s--; }
        else if (*s == 's') in->sign = 1;
        else if (*s == 'f') in->flt = 1;
        else if (*s == 'v') in->vol = 1;
    }
}

/* ---- the unit ---- */

static struct ir_func *new_func(struct ir_unit *u)
{
    u->funcs = xrealloc(u->funcs, (size_t)(u->nfuncs + 1) * sizeof *u->funcs);
    struct ir_func *f = &u->funcs[u->nfuncs++];
    memset(f, 0, sizeof *f);
    return f;
}

static struct ir_ins *push(struct ir_func *f)
{
    if (f->nins == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 32;
        f->ins = xrealloc(f->ins, (size_t)f->cap * sizeof *f->ins);
    }
    struct ir_ins *i = &f->ins[f->nins++];
    memset(i, 0, sizeof *i);
    i->dst = i->a = i->b = i->c = -1;
    i->label = -1;
    i->callee_sym = i->glob_sym = -1;
    /* Zero, not irgen's 4: the printer omits a width it does not have, so
     * "absent in the text" has to mean the same thing coming back in, or
     * reprinting invents a suffix that was never there. */
    i->w = 0;
    i->size = 0;
    return i;
}

static int sym_index(struct p *p, const char *name)
{
    for (int i = 0; i < p->u->nsyms; i++)
        if (!strcmp(p->u->syms[i].name, name))
            return i;
    perr(p, "a symbol declared earlier in the unit");
    return -1;
}

static void decl_sym(struct p *p, int is_func, char *rest)
{
    char *nm = word(p);
    if (!nm || *nm != '@')
        perr(p, "@name after func/data");
    p->u->syms = xrealloc(p->u->syms,
                          (size_t)(p->u->nsyms + 1) * sizeof *p->u->syms);
    struct ir_sym *y = &p->u->syms[p->u->nsyms++];
    memset(y, 0, sizeof *y);
    y->name = xstrndup(nm + 1, strlen(nm + 1));
    y->is_func = is_func;
    y->defined = has_flag(rest, "defined");
    y->is_weak = has_flag(rest, "weak");
    y->is_varargs = has_flag(rest, "varargs");
    y->sret_first = has_flag(rest, "sret");
    y->is_nothrow = has_flag(rest, "nothrow");
}

/* `; 12:5`, `; line 12`, or `; compiler-synthesized` at the end of an
 * instruction — provenance (R3), which round-trips like everything else. */
static void trailing_location(struct ir_ins *in, const char *rest)
{
    const char *c = strchr(rest, ';');
    if (!c)
        return;
    c++;
    while (*c == ' ')
        c++;
    if (!strncmp(c, "compiler-synthesized", 20)) {
        in->synth = 1;
        return;
    }
    if (!strncmp(c, "line ", 5)) {
        in->line = (int)strtol(c + 5, NULL, 10);
        return;
    }
    char *end;
    long l = strtol(c, &end, 10);
    if (end != c && *end == ':') {
        in->line = (int)l;
        in->col = (int)strtol(end + 1, NULL, 10);
    }
}

static void parse_local(struct p *p, const char *rest)
{
    char *w = word(p);                 /* vN */
    int idx = slot(p, w);
    if (idx < 0 || idx >= p->fn->nvars)
        perr(p, "a frame slot within nvars");
    struct ir_local *L = &p->fn->locals[idx];
    long v;
    if (kv(rest, "size", &v))       L->size = (int)v;
    if (kv(rest, "align", &v))      L->align = (int)v;
    if (kv(rest, "user_align", &v)) L->user_align = (int)v;
    L->is_volatile          = has_flag(rest, "volatile");
    L->is_ldouble           = has_flag(rest, "ldouble");
    L->is_int128            = has_flag(rest, "int128");
    L->is_int_or_ptr        = has_flag(rest, "intptr");
    L->is_scalar_int_or_ptr = has_flag(rest, "scalar");
}

static void parse_func_header(struct p *p, const char *rest)
{
    char *nm = word(p);
    if (!nm || *nm != '@')
        perr(p, "@name after func");
    struct ir_func *f = new_func(p->u);
    f->name = xstrndup(nm + 1, strlen(nm + 1));
    f->is_static  = has_flag(rest, "static");
    f->is_varargs = has_flag(rest, "varargs");
    f->has_alloca = has_flag(rest, "alloca");
    f->has_i128   = has_flag(rest, "i128");
    long v;
    if (kv(rest, "nparams", &v))  f->nparams = (int)v;
    if (kv(rest, "nvars", &v))    f->nvars = (int)v;
    if (kv(rest, "vregs", &v))    f->nvregs = (int)v;
    if (kv(rest, "labels", &v))   f->nlabels = (int)v;
    if (kv(rest, "scratch", &v))  f->scratch_bytes = (int)v;
    if (kv(rest, "outgoing", &v)) f->outgoing_bytes = (int)v;
    if (f->nvars > 0)
        f->locals = xcalloc((size_t)f->nvars, sizeof *f->locals);
    if (f->nparams > 0)
        f->param_abi = xcalloc((size_t)f->nparams, sizeof *f->param_abi);
    p->fn = f;
}

/* ---- one instruction ---- */

static void parse_ins(struct p *p, char *first, const char *rest)
{
    struct ir_func *f = p->fn;
    if (!f)
        perr(p, "an instruction inside a func");

    int dst = -1;
    char *m = first;
    if (*first == '%') {
        /* `%3 = op ...`, or `%3, %4 = landing` */
        dst = vreg(p, first);
        char *nx = word(p);
        int second = -1;
        if (nx && *nx == '%') {        /* the two-result form */
            second = vreg(p, nx);
            nx = word(p);
        }
        if (!nx || strcmp(nx, "="))
            perr(p, "'=' after a destination");
        m = word(p);
        if (!m)
            perr(p, "an opcode");
        struct ir_ins *in = push(f);
        in->dst = dst;
        if (second >= 0)
            in->b = second;
        char *base;
        split_mnemonic(m, &base, in);
        int op = ir_op_from_name(base);
        if (op < 0)
            perr(p, "a known opcode");
        in->op = (enum ir_op)op;
        trailing_location(in, rest);

        switch (in->op) {
        case IR_CONST:
            in->imm = strtol(word(p), NULL, 10);
            break;
        case IR_MOV: case IR_NEG: case IR_BNOT: case IR_BSWAP:
        case IR_SQRT:
            in->a = vreg(p, word(p));
            break;
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR: {
            char *wa = word(p);
            wa[strlen(wa) - 1] = 0;    /* the comma */
            in->a = vreg(p, wa);
            char *wb = word(p);
            if (*wb == '#') { in->imm_b = 1; in->imm = strtol(wb + 1, NULL, 10); }
            else            { in->b = vreg(p, wb); }
            break;
        }
        case IR_CMP: {
            int pr = ir_pred_from_name(word(p));
            if (pr < 0)
                perr(p, "a comparison predicate");
            in->pred = (enum binop)pr;
            char *wa = word(p);
            wa[strlen(wa) - 1] = 0;
            in->a = vreg(p, wa);
            char *wb = word(p);
            if (*wb == '#') { in->imm_b = 1; in->imm = strtol(wb + 1, NULL, 10); }
            else            { in->b = vreg(p, wb); }
            break;
        }
        case IR_LDVAR:
            in->a = slot(p, word(p));
            break;
        case IR_LOAD:
            in->a = addr_operand(p, word(p));
            break;
        case IR_ADDR:
            in->a = slot(p, word(p));
            break;
        case IR_STRADDR: {
            char *w = word(p);
            if (strncmp(w, "str", 3))
                perr(p, "a strN operand");
            in->label = (int)strtol(w + 3, NULL, 10);
            break;
        }
        case IR_GADDR: case IR_FADDR: {
            char *w = word(p);
            if (!w || *w != '@')
                perr(p, "an @symbol");
            int y = sym_index(p, w + 1);
            if (in->op == IR_GADDR) in->glob_sym = y;
            else                    in->callee_sym = y;
            break;
        }
        case IR_EXT: case IR_I2F: case IR_F2I: case IR_F2F:
            in->a = vreg(p, word(p));
            break;
        case IR_ALLOCA:
            in->a = vreg(p, word(p));
            break;
        case IR_SPSAVE: case IR_FRAMEADDR: case IR_LANDING:
            break;
        case IR_LABELADDR:
            in->label = labelno(p, word(p));
            break;
        case IR_XCHG: case IR_XADD: case IR_CAS: case IR_CAS16:
        case IR_ARMW: case IR_CMPXCHG: {
            char *wa = word(p);
            wa[strlen(wa) - 1] = 0;
            in->a = addr_operand(p, wa);
            char *wb = word(p);
            size_t bl = strlen(wb);
            if (bl && wb[bl - 1] == ',') wb[bl - 1] = 0;
            if (*wb == '[') in->b = addr_operand(p, wb);
            else            in->b = vreg(p, wb);
            char *wc = word(p);
            if (wc && *wc == '%')
                in->c = vreg(p, wc);
            break;
        }
        case IR_CALL: {
            char *w = word(p);         /* `@name(%1,` or `[%2](%1,` ... */
            if (!w)
                perr(p, "a call target");
            char *lp = strchr(w, '(');
            if (!lp)
                perr(p, "'(' after a call target");
            *lp = 0;
            if (*w == '@')
                in->callee_sym = sym_index(p, w + 1);
            else if (w[0] == '[' && w[1] == '%') {
                in->indirect = 1;
                in->a = (int)strtol(w + 2, NULL, 10);
            } else {
                perr(p, "@name or [%vreg] as the call target");
            }
            /* the argument list, `%1, %2)` possibly split across words */
            char *arg = lp + 1;
            for (;;) {
                while (*arg == ' ' || *arg == ',')
                    arg++;
                if (!*arg || *arg == ')')
                    break;
                if (*arg != '%')
                    perr(p, "a %vreg argument");
                in->argv[in->nargs++].vreg = (int)strtol(arg + 1, &arg, 10);
                while (*arg == ',' || *arg == ' ')
                    arg++;
                if (*arg == ')' || !*arg) {
                    if (*arg == ')')
                        break;
                    arg = word(p);     /* the list continued past a space */
                    if (!arg)
                        break;
                }
            }
            /* "varargs(N)": N is how many parameters are NAMED, which
             * decides where the rest go on Darwin. Printed with the flag
             * rather than beside it so a round-trip cannot keep one and
             * lose the other -- §9.1 requires print/parse to give back
             * identical IR, and a dropped field is identical text over
             * different meaning. */
            {
                const char *va = strstr(rest, "varargs");
                in->call_varargs = va != NULL;
                in->call_nfixed = 0;
                if (va && va[7] == '(')
                    in->call_nfixed = atoi(va + 8);
            }
            in->sret_first = has_flag(rest, "sret");
            break;
        }
        case IR_VLOAD: case IR_VSPLAT: case IR_VREDADD: {
            char *w0 = word(p);
            in->a = *w0 == '[' ? addr_operand(p, w0) : vreg(p, w0);
            break;
        }
        case IR_VWIDEN: {
            char *h = word(p);              /* "lo" or "hi" */
            in->c = h && h[0] == 'h';
            in->a = vreg(p, word(p));
            break;
        }
        case IR_VBIN: {
            char *op = word(p);             /* the lane-wise operator */
            in->imm = (unsigned char)op[0];
            char *wa = word(p);
            size_t al = strlen(wa);
            if (al && wa[al - 1] == ',') wa[al - 1] = 0;
            in->a = vreg(p, wa);
            char *wb = word(p);
            if (wb && *wb == '#') { in->c = atoi(wb + 1); in->b = -1; }
            else                  in->b = vreg(p, wb);
            break;
        }
        case IR_SELECT: {
            /* `%d = select.W %c ? %a : %b` -- the condition, then the
             * two arms, with the punctuation the printer put between
             * them. §9.1 wants print and parse to give back identical
             * IR, so a new opcode has to be taught to BOTH; this one
             * was not, and tests/golden/ir-roundtrip.sh said so. */
            char *w0 = word(p);
            in->a = vreg(p, w0);
            char *q = word(p);              /* "?" */
            (void)q;
            char *w1 = word(p);
            size_t l1 = strlen(w1);
            if (l1 && w1[l1 - 1] == ':') w1[l1 - 1] = 0;
            in->b = vreg(p, w1);
            char *w2 = word(p);
            if (w2 && *w2 == ':') w2 = word(p);
            in->c = vreg(p, w2);
            break;
        }
        default:
            /* An opcode whose operand syntax this parser does not know is
             * refused by NAME, so the message says which one to teach it. */
            diag_fatal(p->file, p->line,
                       "EmbIR: '%s' has a destination but no operand form "
                       "here — teach irparse.c about it", ir_opname(in->op));
        }
        return;
    }

    /* No destination: a statement-shaped instruction. */
    char *base;
    struct ir_ins *in = push(f);
    split_mnemonic(m, &base, in);
    int op = ir_op_from_name(base);
    if (op < 0)
        perr(p, "a known opcode");
    in->op = (enum ir_op)op;
    trailing_location(in, rest);

    switch (in->op) {
    case IR_STVAR: {
        char *wa = word(p);
        wa[strlen(wa) - 1] = 0;
        in->dst = slot(p, wa);
        in->a = vreg(p, word(p));
        break;
    }
    case IR_STORE: case IR_VSTORE: {
        char *wa = word(p);
        wa[strlen(wa) - 1] = 0;
        in->a = addr_operand(p, wa);
        in->b = vreg(p, word(p));
        break;
    }
    case IR_RET: {
        char *w = word(p);
        if (w && *w == '%')
            in->a = vreg(p, w);
        break;
    }
    case IR_JMP:
        in->label = labelno(p, word(p));
        break;
    case IR_BRZ: case IR_BRNZ:
        in->a = vreg(p, word(p));
        if (!eat(p, "->"))
            perr(p, "'->' before a branch target");
        in->label = labelno(p, word(p));
        break;
    case IR_IGOTO:
        in->a = addr_operand(p, word(p));
        break;
    case IR_MEMCPY: {
        char *wa = word(p);
        wa[strlen(wa) - 1] = 0;
        in->a = addr_operand(p, wa);
        in->b = addr_operand(p, word(p));
        break;
    }
    case IR_MEMZERO:
        in->a = addr_operand(p, word(p));
        break;
    case IR_SPRESTORE:
        in->a = vreg(p, word(p));
        break;
    case IR_VA_START:
        in->a = addr_operand(p, word(p));
        break;
    case IR_FENCE: case IR_UD2:
        break;
    default:
        diag_fatal(p->file, p->line,
                   "EmbIR: '%s' has no operand form here — teach "
                   "irparse.c about it", ir_opname(in->op));
    }
}

/* ---- the driver ---- */

struct ir_unit *ir_parse(const char *file, char *text)
{
    struct p p;
    memset(&p, 0, sizeof p);
    p.file = file;
    p.text = text;
    p.u = xcalloc(1, sizeof *p.u);

    char *s = text;
    p.line = 0;
    while (*s) {
        char *nl = strchr(s, '\n');
        if (nl) *nl = 0;
        p.line++;
        char *raw = s;
        s = nl ? nl + 1 : s + strlen(s);

        /* a copy of the line for the key=value and flag lookups, since the
         * word scanner NUL-terminates in place as it goes */
        char keep[2048];
        snprintf(keep, sizeof keep, "%s", raw);
        p.cur = raw;

        skip_ws(&p);
        if (!*p.cur || *p.cur == ';')
            continue;                   /* blank, or `; EmbIR` */

        char *w = word(&p);
        if (!w)
            continue;

        if (!strcmp(w, "}")) { p.fn = NULL; continue; }
        if (!strcmp(w, "local")) { parse_local(&p, keep); continue; }
        if (!strncmp(w, "str", 3) && w[3] >= '0' && w[3] <= '9') {
            /* strN = "...": the bytes, unescaped */
            const char *q = strchr(keep, '"');
            if (!q)
                perr(&p, "a quoted string after strN =");
            struct outbuf b = { NULL, 0, 0 };
            for (const char *c = q + 1; *c && *c != '"'; c++) {
                if (*c != '\\') { ob_ch(&b, *c); continue; }
                c++;
                ob_ch(&b, *c == 'n' ? '\n' : *c == 't' ? '\t' : *c);
            }
            /* ir_intern_string keeps the POINTER, so the bytes have to
             * outlive this line -- the buffer is handed over, not freed. */
            ob_ch(&b, 0);
            ir_intern_string(p.u, b.p, (int)b.n);
            continue;
        }
        if (!strcmp(w, "data")) { decl_sym(&p, 0, keep); continue; }
        if (!strcmp(w, "func")) {
            /* `func @name ;decl` declares; `func @name ... {` defines. */
            if (strstr(keep, ";decl"))
                decl_sym(&p, 1, keep);
            else
                parse_func_header(&p, keep);
            continue;
        }
        if (*w == 'L' && w[strlen(w) - 1] == ':') {
            struct ir_ins *in = push(p.fn);
            in->op = IR_LABEL;
            w[strlen(w) - 1] = 0;
            in->label = labelno(&p, w);
            continue;
        }
        parse_ins(&p, w, keep);
    }
    return p.u;
}
