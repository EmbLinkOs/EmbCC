/* The fourth round: GNU's argp, obstack, argz and envz; backtrace; the
 * resolver's own interface (name codecs, compression, the message
 * parser, res_mkquery); getaddrinfo_a; and getopt's optind while it
 * permutes, which is GNU's (lazy) and argp's state->next with it. Every
 * answer is glibc 2.36's: the program ran there as a transcript, and the
 * help texts are what glibc printed. */
// expect-exit: 42
// needs-libc: argp, obstack, argz, the res_* calls, from lib/libc
#define _GNU_SOURCE 1
#include <argp.h>
#include <argz.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <envz.h>
#include <errno.h>
#include <execinfo.h>
#include <getopt.h>
#include <netdb.h>
#include <obstack.h>
#include <stdarg.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define obstack_chunk_alloc malloc
#define obstack_chunk_free free

static int g_line;
#define CHECK(c) do { if (!(c)) { g_line = __LINE__; return 1; } } while (0)

/* the vector with its NULs as '|' */
static const char *flat(const char *a, size_t n)
{
    static char b[128];
    size_t i;
    for (i = 0; i < n && i < sizeof b - 1; i++)
        b[i] = a[i] ? a[i] : '|';
    b[i] = 0;
    return b;
}

static int t_argz(void)
{
    char *a;
    size_t n;
    CHECK(argz_create_sep("a:b::c:", ':', &a, &n) == 0 && !strcmp(flat(a, n), "a|b|c||"));
    CHECK(argz_count(a, n) == 4);
    free(a);
    CHECK(argz_create_sep("", ':', &a, &n) == 0 && !a && n == 0);
    char *argv[] = { "one", "", "three", 0 };
    argz_create(argv, &a, &n);
    argz_add(&a, &n, "four");
    argz_add_sep(&a, &n, "d,,e,", ',');
    CHECK(!strcmp(flat(a, n), "one||three|four|d|e||"));
    CHECK(argz_insert(&a, &n, a + n + 5, "x") == EINVAL);
    CHECK(argz_insert(&a, &n, strstr(a + 5, "three") + 2, "ins") == 0 &&
          !strcmp(flat(a, n), "one||ins|three|four|d|e||"));
    argz_delete(&a, &n, a);
    unsigned cnt = 0;
    /* glibc counts the entries that changed, not the replacements */
    CHECK(argz_replace(&a, &n, "e", "EE", &cnt) == 0 && cnt == 2 &&
          !strcmp(flat(a, n), "|ins|thrEEEE|four|d|EE||"));
    argz_stringify(a, n, ' ');
    CHECK(!strcmp(a, " ins thrEEEE four d EE "));
    free(a);
    CHECK(argz_next(0, 0, 0) == 0);

    char *ev[] = { "A=1", "B", "C=3=4", "DD=", 0 };
    argz_create(ev, &a, &n);
    CHECK(!strcmp(envz_get(a, n, "C"), "3=4") && !envz_get(a, n, "B") &&
          !strcmp(envz_entry(a, n, "B"), "B") && !strcmp(envz_get(a, n, "C=zz"), "3=4") &&
          !envz_get(a, n, "D"));
    envz_add(&a, &n, "A", "new");
    envz_add(&a, &n, "E", 0);
    envz_remove(&a, &n, "B");
    CHECK(!strcmp(flat(a, n), "C=3=4|DD=|A=new|E|"));
    char *b;
    size_t bn;
    char *ev2[] = { "C=over", "G=g", 0 };
    argz_create(ev2, &b, &bn);
    envz_merge(&a, &n, b, bn, 0);
    CHECK(!strcmp(flat(a, n), "C=3=4|DD=|A=new|E|G=g|"));
    envz_merge(&a, &n, b, bn, 1);
    envz_strip(&a, &n);
    CHECK(!strcmp(flat(a, n), "DD=|A=new|C=over|G=g|"));
    free(a);
    free(b);
    return 0;
}

static int t_obstack(void)
{
    struct obstack o;
    obstack_init(&o);
    CHECK(obstack_empty_p(&o));
    char *s1 = obstack_copy0(&o, "hello", 5);
    obstack_grow(&o, "wor", 3);
    obstack_1grow(&o, 'l');
    obstack_grow0(&o, "d", 1);
    CHECK(obstack_object_size(&o) == 6);
    char *s2 = obstack_finish(&o);
    CHECK(!strcmp(s1, "hello") && !strcmp(s2, "world") && !obstack_empty_p(&o));
    CHECK(((size_t)s2 - (size_t)s1) % (obstack_alignment_mask(&o) + 1) == 0);
    CHECK(obstack_printf(&o, "%d-%s", 42, "x") == 4);
    obstack_1grow(&o, 0);
    char *s3 = obstack_finish(&o);
    CHECK(!strcmp(s3, "42-x"));
    size_t one = obstack_memory_used(&o);
    char *mark = 0;
    for (int i = 0; i < 300; i++) {
        char *q = obstack_alloc(&o, 37);
        memset(q, 'a' + i % 26, 37);
        if (i == 150)
            mark = q;
    }
    CHECK(obstack_memory_used(&o) > one && mark[36] == 'a' + 150 % 26);
    for (int i = 0; i < 6000; i++)          /* grows past a chunk, and moves */
        obstack_1grow(&o, 'z');
    char *big = obstack_finish(&o);
    CHECK(big[0] == 'z' && big[5999] == 'z');
    obstack_free(&o, mark);
    obstack_free(&o, s3);
    CHECK(obstack_memory_used(&o) == one && !strcmp(s1, "hello"));
    obstack_free(&o, 0);
    struct obstack o2;
    obstack_specify_allocation(&o2, 100, 4, malloc, free);
    CHECK(obstack_chunk_size(&o2) == 100 && obstack_memory_used(&o2) == 100 &&
          obstack_alignment_mask(&o2) == 3);
    obstack_alignment_mask(&o2) = 0;
    obstack_1grow(&o2, 'a');
    char *x1 = obstack_finish(&o2);
    obstack_1grow(&o2, 'b');
    CHECK(obstack_finish(&o2) == x1 + 1);
    obstack_free(&o2, 0);
    return 0;
}

/* ---- argp --------------------------------------------------------------------- */

const char *argp_program_version = "prog 1.0";
const char *argp_program_bug_address = "<bugs@example.org>";

static char g_log[512];
static char *g_out;
static size_t g_outn;
static FILE *g_mem;

static const struct argp_option opts[] = {
    { "verbose", 'v', 0, 0, "Produce verbose output", 0 },
    { "quiet", 'q', 0, 0, "Don't produce any output", 0 },
    { "silent", 's', 0, OPTION_ALIAS, 0, 0 },
    { "output", 'o', "FILE", 0, "Output to FILE instead of standard output", 0 },
    { 0, 0, 0, 0, "Advanced options:", 7 },
    { "level", 'l', "N", OPTION_ARG_OPTIONAL,
      "A level, which is a long description that will need to wrap across more than one "
      "line of the help output to see the wrapping", 0 },
    { "long-only-option-with-a-very-long-name", 1000, "VALUE", 0,
      "Long name pushes the doc to the next line", 0 },
    { "hidden", 'h', 0, OPTION_HIDDEN, "hidden", 0 },
    { "abort", 'A', 0, 0, "Abort with an error", 0 },
    { 0, 0, 0, 0, 0, 0 },
};

static error_t parse(int key, char *arg, struct argp_state *st)
{
    char item[64];
    switch (key) {
    case ARGP_KEY_INIT:
        st->out_stream = st->err_stream = g_mem;
        st->child_inputs[0] = (void *)"to-child";
        return 0;
    case 'A':
        argp_error(st, "abort %d", 3);
        return 0;
    case ARGP_KEY_ARG:
        if (st->arg_num >= 2)
            return ARGP_ERR_UNKNOWN;
        snprintf(item, sizeof item, "arg:%s@%d ", arg, st->next);
        strcat(g_log, item);
        return 0;
    case 'v': case 'q': case 'o': case 'l': case 1000: case 'h':
        snprintf(item, sizeof item, "%c:%s@%d ", key < 256 ? key : 'L', arg ? arg : "-",
                 st->next);
        strcat(g_log, item);
        return 0;
    case ARGP_KEY_END:
        strcat(g_log, "end ");
        return 0;
    }
    return ARGP_ERR_UNKNOWN;
}

static const struct argp_option child_opts[] = {
    { "child", 'c', "C", 0, "A child option", 0 },
    { "vchild", 'v', 0, 0, "Shadowed short v", 0 },
    { 0, 0, 0, 0, 0, 0 },
};

static error_t child_parse(int key, char *arg, struct argp_state *st)
{
    if (key == ARGP_KEY_INIT || key == 'c') {
        char item[64];
        snprintf(item, sizeof item, "child%c:%s:%s ", key == 'c' ? 'c' : 'I', arg ? arg : "-",
                 (char *)st->input);
        strcat(g_log, item);
        return 0;
    }
    return ARGP_ERR_UNKNOWN;
}

static const struct argp_option merged_opts[] = {
    { "beta", 'b', "B", 0, "Sorted among the main ones", 0 },
    { 0, 0, 0, 0, 0, 0 },
};

static const struct argp child_argp = { child_opts, child_parse, "CHILDARG",
                                        "Child doc.\vChild post doc.", 0, 0, 0 };
static const struct argp merged_argp = { merged_opts, 0, 0, 0, 0, 0, 0 };
static const struct argp_child children[] = {
    { &child_argp, 0, "Child options:", 0 },
    { &merged_argp, 0, 0, 0 },
    { 0, 0, 0, 0 },
};
static const struct argp argp = {
    opts, parse, "ARG1 ARG2\n-x ARG3",
    "Pre doc text.\vPost doc text that is long enough to wrap around the right margin of "
    "seventy-nine columns, we hope.",
    children, 0, 0,
};

/* argp_parse on the words, its output in g_out */
static error_t run(unsigned flags, int *end, int argc, ...)
{
    static char *v[16];
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc; i++)
        v[i] = va_arg(ap, char *);
    v[argc] = 0;
    va_end(ap);
    g_log[0] = 0;
    free(g_out);
    g_out = 0;
    g_mem = open_memstream(&g_out, &g_outn);
    error_t e = argp_parse(&argp, argc, v, flags | ARGP_NO_EXIT, end, 0);
    fclose(g_mem);
    return e;
}

static const char help[] =
    "Usage: prog [OPTION...] ARG1 ARG2 CHILDARG\n"
    "  or:  prog [OPTION...] -x ARG3 CHILDARG\n"
    "Pre doc text.\n"
    "\n"
    "  -b, --beta=B               Sorted among the main ones\n"
    "  -o, --output=FILE          Output to FILE instead of standard output\n"
    "  -q, -s, --quiet, --silent  Don't produce any output\n"
    "  -v, --verbose              Produce verbose output\n"
    "\n"
    " Child options:\n"
    "  -c, --child=C              A child option\n"
    "      --vchild               Shadowed short v\n"
    "\n"
    " Advanced options:\n"
    "  -A, --abort                Abort with an error\n"
    "      --long-only-option-with-a-very-long-name=VALUE\n"
    "                             Long name pushes the doc to the next line\n"
    "  -l, --level[=N]            A level, which is a long description that will\n"
    "                             need to wrap across more than one line of the help\n"
    "                             output to see the wrapping\n"
    "\n"
    "  -?, --help                 Give this help list\n"
    "      --usage                Give a short usage message\n"
    "  -V, --version              Print program version\n"
    "\n"
    "Mandatory or optional arguments to long options are also mandatory or optional\n"
    "for any corresponding short options.\n"
    "\n"
    "Post doc text that is long enough to wrap around the right margin of\n"
    "seventy-nine columns, we hope.\n"
    "\n"
    "Child post doc.\n"
    "\n"
    "Report bugs to <bugs@example.org>.\n";

static const char usage[] =
    "Usage: prog [-qsvA?V] [-b B] [-o FILE] [-c C] [-l[N]] [--beta=B]\n"
    "            [--output=FILE] [--quiet] [--silent] [--verbose] [--child=C]\n"
    "            [--vchild] [--abort]\n"
    "            [--long-only-option-with-a-very-long-name=VALUE] [--level[=N]]\n"
    "            [--help] [--usage] [--version] ARG1 ARG2 CHILDARG\n"
    "  or:  prog [OPTION...] -x ARG3 CHILDARG\n";

static int t_argp(void)
{
    /* the options first (permuted), each with next where it ended; then
     * the operands; the child's input from the parent's INIT */
    CHECK(run(0, 0, 9, "prog", "-v", "a", "--output=f", "b", "-qo", "g", "--level=3", "-c1")
          == 0);
    CHECK(!strcmp(g_log, "childI:-:to-child v:-@2 o:f@4 q:-@5 o:g@7 l:3@8 childc:1:to-child "
                         "arg:a@8 arg:b@9 end "));
    CHECK(run(0, 0, 2, "prog", "--help") == 0 && !strcmp(g_out, help));
    CHECK(run(0, 0, 2, "/usr/bin/prog", "--usage") == 0 && !strcmp(g_out, usage));
    CHECK(run(0, 0, 2, "prog", "-V") == 0 && !strcmp(g_out, "prog 1.0\n"));
    CHECK(run(0, 0, 2, "prog", "-A") == 0 &&
          !strcmp(g_out, "prog: abort 3\n"
                         "Try `prog --help' or `prog --usage' for more information.\n"));
    CHECK(run(0, 0, 5, "prog", "a", "b", "c", "d") == EINVAL &&
          !strcmp(g_out, "prog: Too many arguments\n"
                         "Try `prog --help' or `prog --usage' for more information.\n"));
    int end;
    CHECK(run(0, &end, 5, "prog", "a", "b", "c", "d") == 0 && end == 3);
    CHECK(run(ARGP_IN_ORDER, 0, 4, "prog", "a", "-v", "b") == 0 &&
          !strcmp(g_log, "childI:-:to-child arg:a@2 v:-@3 arg:b@4 end "));
    CHECK(run(0, 0, 5, "prog", "-v", "--", "-q", "x") == 0 &&
          !strcmp(g_log, "childI:-:to-child v:-@2 arg:-q@4 arg:x@5 end "));
    CHECK(run(ARGP_LONG_ONLY, 0, 3, "prog", "-verbose", "-output=x") == 0 &&
          !strcmp(g_log, "childI:-:to-child v:-@2 o:x@3 end "));
    CHECK(run(ARGP_NO_ERRS, 0, 2, "prog", "--bogus") == EINVAL && !*g_out);
    /* argp_help, by itself, to a stream */
    char *hb = 0;
    size_t hn;
    FILE *m = open_memstream(&hb, &hn);
    argp_help(&merged_argp, m, ARGP_HELP_USAGE | ARGP_HELP_LONG, "direct");
    fclose(m);
    CHECK(!strcmp(hb, "Usage: direct [-b B] [--beta=B]\n"
                      "\n"
                      "  -b, --beta=B               Sorted among the main ones\n"
                      "\n"
                      "Mandatory or optional arguments to long options are also mandatory or "
                      "optional\n"
                      "for any corresponding short options.\n"));
    free(hb);
    return 0;
}

/* getopt_long's optind as it goes, where options follow operands: GNU's,
 * where the moving waits for the next call */
static int t_getopt(void)
{
    static const struct option lo[] = { { "out", required_argument, 0, 'o' }, { 0, 0, 0, 0 } };
    char *v[] = { "p", "a", "-v", "b", "--out=x", "c", 0 };
    int seen[4], k = 0, c;
    optind = 0;
    opterr = 0;
    while ((c = getopt_long(6, v, "v", lo, 0)) != -1 && k < 3)
        seen[k++] = optind;
    seen[k++] = optind;
    CHECK(k == 3 && seen[0] == 3 && seen[1] == 5 && seen[2] == 3);
    CHECK(!strcmp(v[1], "-v") && !strcmp(v[2], "--out=x") && !strcmp(v[3], "a") &&
          !strcmp(v[5], "c"));
    return 0;
}

/* ---- the resolver -------------------------------------------------------------- */

static int t_resolv(void)
{
    unsigned char w[NS_MAXCDNAME];
    char t[NS_MAXDNAME];
    CHECK(ns_name_pton("www.example.com", w, sizeof w) == 0 &&
          ns_name_ntop(w, t, sizeof t) == 16 && !strcmp(t, "www.example.com"));
    CHECK(ns_name_pton("a\\.b.c.", w, sizeof w) == 1 && w[0] == 3 &&
          ns_name_ntop(w, t, sizeof t) > 0 && !strcmp(t, "a\\.b.c"));
    CHECK(ns_name_pton(".", w, sizeof w) == 1 && w[0] == 0 &&
          ns_name_ntop(w, t, sizeof t) == 2 && !strcmp(t, "."));
    CHECK(ns_name_pton("a..b", w, sizeof w) == -1 && errno == EMSGSIZE);
    CHECK(ns_name_pton("\\256", w, sizeof w) == -1 && ns_name_pton("x\\", w, sizeof w) == -1);
    unsigned char bin[] = { 3, 'a', 0x01, '.', 2, 0x7f, ' ', 0 };
    CHECK(ns_name_ntop(bin, t, sizeof t) == 17 && !strcmp(t, "a\\001\\..\\127\\032"));
    CHECK(res_hnok("www.example.com") && !res_hnok("-bad.com") && res_hnok("a_b.c") &&
          !res_hnok("a*b") && res_ownok("*.example.com") && !res_ownok("a.*.com") &&
          res_mailok("John\\.Doe.example.com") && !res_mailok("user") && res_dnok("a\\032b"));

    /* compression: each name points back to the longest tail already
     * written, and only whole names are remembered */
    unsigned char msg[256];
    const unsigned char *dnptrs[6] = { msg, 0 };
    unsigned char *p = msg + 12;
    static const char *names[] = { "www.example.com", "mail.example.com", "WWW.EXAMPLE.COM",
                                   "a.b.mail.example.com", "x.y.z", "q.x.y.z", 0 };
    static const int lens[] = { 17, 7, 2, 6, 7, 4 };
    for (int i = 0; names[i]; i++) {
        int n = dn_comp(names[i], p, (int)(msg + sizeof msg - p), (unsigned char **)dnptrs,
                        (unsigned char **)dnptrs + 6);
        CHECK(n == lens[i]);
        p += n;
    }
    CHECK(p[-2] == 0xc0 && p[-1] == 12 + 17 + 7 + 2 + 6);     /* q -> x.y.z */
    CHECK(dnptrs[1] == msg + 12 && dnptrs[2] == msg + 29 && dnptrs[3] == msg + 38 &&
          dnptrs[4] == msg + 44 && dnptrs[5] == 0);
    CHECK(dn_expand(msg, p, msg + 36, t, sizeof t) == 2 && !strcmp(t, "www.example.com"));
    CHECK(dn_expand(msg, p, msg + 38, t, sizeof t) == 6 && !strcmp(t, "a.b.mail.example.com"));
    CHECK(dn_skipname(msg + 29, p) == 7);
    unsigned char o[8];
    CHECK(dn_comp("ab.c", o, 6, 0, 0) == 6 && dn_comp("ab.c", o, 5, 0, 0) == -1);
    unsigned char loop[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 12 };
    CHECK(dn_expand(loop, loop + sizeof loop, loop + 12, t, sizeof t) == -1);
    unsigned char root[] = { 0 };
    CHECK(dn_expand(root, root + 1, root, t, sizeof t) == 1 && !*t);

    /* a message, and the parser over it */
    unsigned char m[128];
    memset(m, 0, sizeof m);
    m[0] = 0x12; m[1] = 0x34; m[2] = 0x81; m[3] = 0x80; m[5] = 1; m[7] = 1;
    const unsigned char *dp[4] = { m, 0 };
    p = m + 12;
    p += dn_comp("Host.Example.ORG", p, 100, (unsigned char **)dp, (unsigned char **)dp + 4);
    ns_put16(ns_t_a, p); ns_put16(ns_c_in, p + 2); p += 4;
    p += dn_comp("host.example.org", p, 100, (unsigned char **)dp, (unsigned char **)dp + 4);
    ns_put16(ns_t_a, p); ns_put16(ns_c_in, p + 2); ns_put32(3600, p + 4); ns_put16(4, p + 8);
    p += 10;
    *p++ = 192; *p++ = 0; *p++ = 2; *p++ = 1;
    int len = (int)(p - m);
    ns_msg h;
    ns_rr rr;
    CHECK(ns_initparse(m, len, &h) == 0 && ns_msg_id(h) == 0x1234 &&
          ns_msg_count(h, ns_s_an) == 1 && ns_msg_getflag(h, ns_f_qr) == 1 &&
          ns_msg_getflag(h, ns_f_rd) == 1 && ns_msg_getflag(h, ns_f_ra) == 1);
    CHECK(ns_parserr(&h, ns_s_qd, 0, &rr) == 0 && !strcmp(ns_rr_name(rr), "Host.Example.ORG") &&
          ns_rr_rdata(rr) == 0);
    CHECK(ns_parserr(&h, ns_s_an, 0, &rr) == 0 && !strcmp(rr.name, "Host.Example.ORG") &&
          ns_rr_ttl(rr) == 3600 && ns_rr_rdlen(rr) == 4 && ns_rr_rdata(rr)[3] == 1);
    CHECK(ns_parserr(&h, ns_s_an, 1, &rr) == -1 && errno == ENODEV);
    CHECK(ns_initparse(m, len + 1, &h) == -1 && errno == EMSGSIZE);  /* a byte too many */

    unsigned char q[64];
    res_init();
    int n = res_mkquery(QUERY, "www.example.com", C_IN, T_MX, 0, 0, 0, q, sizeof q);
    static const unsigned char want[] = {
        1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        3, 'c', 'o', 'm', 0, 0, 15, 0, 1,
    };
    CHECK(n == 33 && !memcmp(q + 2, want, sizeof want));
    CHECK(res_mkquery(QUERY, "abc.de", C_IN, T_A, 0, 0, 0, q, 23) == -1 &&
          res_mkquery(QUERY, "abc.de", C_IN, T_A, 0, 0, 0, q, 24) == 24);
    return 0;
}

static int t_gai(void)
{
    struct addrinfo hint;
    memset(&hint, 0, sizeof hint);
    hint.ai_flags = AI_NUMERICHOST;
    hint.ai_family = AF_INET;
    struct gaicb a = { "127.0.0.1", 0, &hint, 0, 0, 0, { 0 } };
    struct gaicb b = { "not a number", 0, &hint, 0, 0, 0, { 0 } };
    struct gaicb *list[] = { &a, 0, &b };
    CHECK(getaddrinfo_a(GAI_WAIT, list, 3, 0) == 0);
    CHECK(gai_error(&a) == 0 && a.ar_result && a.ar_result->ai_family == AF_INET);
    CHECK(gai_error(&b) == EAI_NONAME && !b.ar_result);
    freeaddrinfo(a.ar_result);
    CHECK(gai_cancel(&a) == EAI_ALLDONE);
    CHECK(getaddrinfo_a(GAI_NOWAIT, list, 1, 0) == 0);
    const struct gaicb *one[] = { &a };
    struct timespec ts = { 5, 0 };
    /* 0 having waited for it; EAI_ALLDONE if it was done before the call */
    int r = gai_suspend(one, 1, &ts);
    CHECK((r == 0 || r == EAI_ALLDONE) && gai_error(&a) == 0);
    /* nothing left in progress: all done, finished ones or not */
    CHECK(gai_suspend(one, 1, &ts) == EAI_ALLDONE);
    freeaddrinfo(a.ar_result);
    const struct gaicb *none[] = { 0 };
    CHECK(gai_suspend(none, 1, &ts) == EAI_ALLDONE);
    CHECK(!strcmp(gai_strerror(EAI_INPROGRESS), "Processing request in progress"));
    return 0;
}

/* ---- backtrace ------------------------------------------------------------------ */

static void *g_bt[16];
static int g_btn;

__attribute__((noinline)) static int f3(int x)
{
    g_btn = backtrace(g_bt, 16);
    return x + 1;
}
__attribute__((noinline)) static int f2(int x) { return f3(x) * 3; }
__attribute__((noinline)) static int f1(int x) { return f2(x) + 5; }

/* the address returns into fn, a little way into it */
static int within(void *ret, int (*fn)(int))
{
    return (char *)ret > (char *)fn && (char *)ret < (char *)fn + 256;
}

static int t_backtrace(void)
{
    CHECK(f1(1) == 11);
#if defined(__x86_64__) || defined(__aarch64__)
    CHECK(g_btn >= 4 && within(g_bt[0], f3) && within(g_bt[1], f2) && within(g_bt[2], f1));
#else
    CHECK(g_btn >= 1 && within(g_bt[0], f3));
#endif
    char **s = backtrace_symbols(g_bt, g_btn);
    CHECK(s && strchr(s[0], '[') && strchr(s[g_btn - 1], ']'));
    free(s);
    return 0;
}

int main(void)
{
    if (t_argz() || t_obstack() || t_argp() || t_getopt() || t_resolv() || t_gai() ||
        t_backtrace()) {
        printf("failed at line %d\n", g_line);
        return 1;
    }
    return 42;
}
