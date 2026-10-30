/* POSIX's string, environment and search functions, and the small
 * modules: strtok_r and strsep, strlcpy and strlcat, memmem and its
 * neighbours, strerror_r (XSI's), basename and dirname, fnmatch, getopt
 * and getopt_long (which permute, as glibc's and newlib's do), qsort_r,
 * the random and *rand48 generators (glibc's sequences, which programs
 * print and tests compare), tsearch and hsearch, setenv and its family.
 *
 * Every expected value here was what glibc 2.36 printed for the same
 * calls, except where a comment says otherwise. */
// expect-exit: 42
// needs-libc: POSIX functions the harnesses' newlib has not got, or gives other sequences for
#define _GNU_SOURCE 1
#include <errno.h>
#include <fnmatch.h>
#include <getopt.h>
#include <libgen.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int strings(void)
{
    char buf[32], *save, *t, out[64] = "";
    strcpy(buf, ",,a,b,,c,");
    for (t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
        strcat(strcat(strcat(out, "<"), t), ">");
    if (strcmp(out, "<a><b><c>") != 0)
        return 1;
    strcpy(buf, ",,a,b,,c,");
    out[0] = 0;
    char *sp = buf;
    while ((t = strsep(&sp, ",")))
        strcat(strcat(strcat(out, "<"), t), ">");
    if (strcmp(out, "<><><a><b><><c><>") != 0)
        return 2;

    /* BSD's answers: the length it tried for, so >= size means cut */
    char small[6];
    if (strlcpy(small, "hello world", sizeof small) != 11 ||
        strcmp(small, "hello") != 0)
        return 3;
    strcpy(small, "ab");
    if (strlcat(small, "cdefgh", sizeof small) != 8 ||
        strcmp(small, "abcde") != 0 || strlcat(small, "xyz", 3) != 6)
        return 4;

    const char *hay = "abcabcabd";
    if ((char *)memmem(hay, 9, "abd", 3) - hay != 6 ||
        memmem(hay, 9, "", 0) != hay || memmem(hay, 9, "abe", 3) ||
        memmem(hay, 2, "abc", 3))
        return 5;
    if (strcmp(strcasestr("Hello World", "WORLD"), "World") != 0 ||
        (char *)memrchr(hay, 'a', 9) - hay != 6 ||
        strchrnul(hay, 'z') - hay != 9 || strchrnul(hay, 'c') - hay != 2)
        return 6;

    char eb[8], eb2[64];
    if (strerror_r(ENOENT, eb, sizeof eb) != ERANGE ||
        strcmp(eb, "No such") != 0 ||
        strerror_r(12345, eb2, sizeof eb2) != EINVAL ||
        strcmp(eb2, "Unknown error 12345") != 0 ||
        strerror_r(ENOENT, eb2, sizeof eb2) != 0)
        return 7;
    char secret[8] = "hunter2";
    explicit_bzero(secret, sizeof secret);
    for (int i = 0; i < 8; i++)
        if (secret[i])
            return 8;
    return 0;
}

static int paths(void)
{
    static const char *const in[] = { "/usr/lib/", "usr", "/", "//", "a/b//",
                                      "", "///a", "a//b", ".", "..", "//a" };
    static const char *const base[] = { "lib", "usr", "/", "/", "b", ".",
                                        "a", "b", ".", "..", "a" };
    static const char *const dir[] = { "/usr", ".", "/", "//", "a", ".", "/",
                                       "a", ".", ".", "//" };
    for (size_t i = 0; i < sizeof in / sizeof *in; i++) {
        char a[16], b[16];
        strcpy(a, in[i]);
        strcpy(b, in[i]);
        if (strcmp(basename(a), base[i]) != 0)
            return 10 + (int)i;
        if (strcmp(dirname(b), dir[i]) != 0)
            return 30 + (int)i;
    }
    if (strcmp(basename(NULL), ".") != 0 || strcmp(dirname(NULL), ".") != 0)
        return 49;
    return 0;
}

static int globs(void)
{
    static const struct { const char *p, *s; int f, want; } t[] = {
        { "*.c", "main.c", 0, 0 },
        { "*.c", "dir/main.c", FNM_PATHNAME, 1 },
        { "*/*.c", "dir/main.c", FNM_PATHNAME, 0 },
        { "*", ".hidden", FNM_PERIOD, 1 },
        { ".*", ".hidden", FNM_PERIOD, 0 },
        { "*", "a/.b", FNM_PERIOD, 0 },
        { "*/*", "a/.b", FNM_PERIOD | FNM_PATHNAME, 1 },
        { "?", "/", FNM_PATHNAME, 1 },
        { "[a-c]x", "bx", 0, 0 }, { "[!a-c]x", "bx", 0, 1 },
        { "[]]", "]", 0, 0 }, { "[!]]", "a", 0, 0 },
        { "[[:digit:]]*", "7up", 0, 0 }, { "[[:alpha:][:digit:]]", "_", 0, 1 },
        { "\\*", "*", 0, 0 }, { "\\*", "\\*", FNM_NOESCAPE, 0 },
        { "a[", "a[", 0, 0 }, { "a[b", "ab", 0, 1 },
        { "ABC", "abc", FNM_CASEFOLD, 0 }, { "[A-C]", "b", FNM_CASEFOLD, 0 },
        { "dir", "dir/file", FNM_LEADING_DIR, 0 },
        /* one backtracking point: linear, where a recursive matcher
         * takes exponential time on this */
        { "*a*a*a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          0, 1 },
        { "a*b*c", "aXbYc", 0, 0 }, { "a*b*c", "aXbYcZ", 0, 1 },
        { "", "", 0, 0 }, { "[a-]", "-", 0, 0 }, { "[-a]", "-", 0, 0 },
        { "[\\]]", "]", 0, 0 }, { "x[[.a.]]", "xa", 0, 0 },
        { "[[=b=]]", "b", 0, 0 },
        { "*\\", "a\\", 0, 1 },           /* a trailing '\' matches nothing */
        { "[[:upper:]]", "a", FNM_CASEFOLD, 1 },  /* classes do not fold */
        { "a/*", "a/b/c", FNM_PATHNAME, 1 }, { "a/*/c", "a/b/c", FNM_PATHNAME, 0 },
        { "**", "a/b", FNM_PATHNAME, 1 },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if ((fnmatch(t[i].p, t[i].s, t[i].f) != 0) != t[i].want)
            return 50 + (int)i;
    return 0;
}

/* Runs one option string over an argument vector and records what was
 * seen, as "c:arg" items, then "|" and the operands in argv order. */
static char seen[256];
static void parse(int argc, const char *const *av, const char *opts,
                  const struct option *lo)
{
    char *argv[16];
    for (int i = 0; i < argc; i++)
        argv[i] = (char *)av[i];
    argv[argc] = NULL;
    optind = 0;
    opterr = 0;
    seen[0] = 0;
    int c;
    while ((c = lo ? getopt_long(argc, argv, opts, lo, NULL)
                   : getopt(argc, argv, opts)) != -1) {
        char item[32];
        snprintf(item, sizeof item, "%c:%s ", c > 32 ? c : '0' + c,
                 optarg ? optarg : (c == '?' || c == ':') ?
                 (char[]){ (char)optopt, 0 } : "");
        strcat(seen, item);
    }
    strcat(seen, "|");
    for (int i = optind; i < argc; i++)
        strcat(strcat(seen, " "), argv[i]);
}

static int options(void)
{
    static const char *const a1[] = { "p", "-ab", "-c", "arg", "-dval", "--",
                                      "-x", "f" };
    parse(8, a1, "abc:d:", NULL);
    if (strcmp(seen, "a: b: c:arg d:val | -x f") != 0)
        return 90;
    static const char *const a2[] = { "p", "file", "-a", "-c", "x", "g" };
    parse(6, a2, "ac:", NULL);             /* permuted, as glibc */
    if (strcmp(seen, "a: c:x | file g") != 0)
        return 91;
    parse(6, a2, "+ac:", NULL);            /* POSIX's: stop at "file" */
    if (strcmp(seen, "| file -a -c x g") != 0)
        return 92;
    static const char *const a3[] = { "p", "-z", "-c" };
    parse(3, a3, "c:", NULL);
    if (strcmp(seen, "?:z ?:c |") != 0)
        return 93;
    parse(3, a3, ":c:", NULL);
    if (strcmp(seen, "?:z ::c |") != 0)
        return 94;

    static int flag;
    static const struct option lo[] = {
        { "verbose", no_argument, NULL, 'v' },
        { "output", required_argument, NULL, 'o' },
        { "opt", optional_argument, NULL, 'p' },
        { "flag", no_argument, &flag, 7 },
        { "verify", no_argument, NULL, 'V' },
        { 0, 0, 0, 0 } };
    static const char *const a4[] = {
        "p", "in", "--verbose", "--output=x", "--out", "y", "--opt", "--opt=z",
        "--flag", "--ver", "--verb", "--nope", "-ab", "tail", "--output" };
    parse(15, a4, "vab", lo);
    /* --ver is ambiguous (verbose, verify); --verb is not */
    if (strcmp(seen, "v: o:x o:y p: p:z 0: ?: v: ?: a: b: ?:o | in tail") != 0)
        return 95;
    if (flag != 7)
        return 96;
    static const char *const a5[] = { "p", "x", "-a", "y", "--", "-b", "z" };
    parse(7, a5, "ab", lo);
    if (strcmp(seen, "a: | x y -b z") != 0)
        return 97;
    parse(7, a5, "-ab", lo);               /* operands in order, as code 1 */
    if (strcmp(seen, "1:x a: 1:y | -b z") != 0)
        return 98;
    static const char *const a6[] = { "p", "f1", "-abc", "f2" };
    parse(4, a6, "abc", lo);
    if (strcmp(seen, "a: b: c: | f1 f2") != 0)
        return 99;
    return 0;
}

static int icmp(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static int icmp_r(const void *a, const void *b, void *dir)
{
    return *(int *)dir * (*(const int *)a - *(const int *)b);
}

static int generators(void)
{
    int v[] = { 5, 3, 9, 1, 7 }, dir = -1;
    qsort_r(v, 5, sizeof *v, icmp_r, &dir);
    if (v[0] != 9 || v[1] != 7 || v[2] != 5 || v[3] != 3 || v[4] != 1)
        return 100;

    srandom(1);
    if (random() != 1804289383 || random() != 846930886 ||
        random() != 1681692777)
        return 101;
    static char st[5][256];
    static const size_t sizes[] = { 8, 32, 64, 128, 256 };
    static const long first[5][2] = {
        { 1406932606, 654583775 }, { 2129646341, 1708620114 },
        { 1848518023, 594153704 }, { 542256677, 294305677 },
        { 155396598, 489107431 } };
    for (int k = 0; k < 5; k++) {
        initstate(12345u + k, st[k], sizes[k]);
        if (random() != first[k][0] || random() != first[k][1])
            return 102 + k;
    }
    random();
    random();
    setstate(st[1]);                       /* resumes where it was left */
    if (random() != 1729007179 || random() != 670702363)
        return 107;
    /* still the 32-byte state's generator; seed 0 is taken as 1 */
    srandom(0);
    long z = random();
    srandom(1);
    if (z != 964237963 || random() != z)
        return 108;
    srandom(4000000000u);
    if (random() != 519042614)
        return 109;

    srand48(7);
    if (drand48() != 0.2664441967654092 || lrand48() != 1464659504 ||
        mrand48() != 1140273416)
        return 110;
    unsigned short xs[3] = { 1, 2, 3 };
    if (erand48(xs) != 0.44199632268870914 || nrand48(xs) != 565063343 ||
        jrand48(xs) != -1485464893)
        return 111;
    unsigned short sd[3] = { 9, 8, 7 };
    unsigned short *old = seed48(sd);
    if (old[0] != 10787 || old[1] != 12552 || old[2] != 17399 ||
        lrand48() != 575374558)
        return 112;
    unsigned short pp[7] = { 1, 2, 3, 4, 5, 6, 7 };
    lcong48(pp);
    if (lrand48() != 917510 || mrand48() != 15925355)
        return 113;
    unsigned seed = 99;
    if (rand_r(&seed) != 2063792036 || rand_r(&seed) != 780411989)
        return 114;
    return 0;
}

static int visited[64], nvisited, deepest;
static void collect(const void *n, VISIT v, int depth)
{
    if (v == postorder || v == leaf)
        visited[nvisited++] = **(int *const *)n;
    if (depth > deepest)
        deepest = depth;
}

/* AVL's own invariant, from twalk alone: each node's subtree heights,
 * rebuilt from the order of its visits -- a child seen before the node's
 * postorder visit is its left, after it its right -- may differ by one
 * at most. */
static int side[64], lh[64], rh[64], unbalanced;
static void report(int depth, int h)
{
    if (depth >= 0)
        *(side[depth] ? &rh[depth] : &lh[depth]) = h;
}
static void balance(const void *n, VISIT v, int depth)
{
    (void)n;
    if (v == preorder) {
        side[depth] = lh[depth] = rh[depth] = 0;
    } else if (v == postorder) {
        side[depth] = 1;
    } else {
        int h = 1;
        if (v == endorder) {
            int d = lh[depth] - rh[depth];
            if (d > 1 || d < -1)
                unbalanced = 1;
            h += lh[depth] > rh[depth] ? lh[depth] : rh[depth];
        }
        report(depth - 1, h);
    }
}

static int tables(void)
{
    /* inserted in an order that unbalances a plain binary tree, and
     * walked in order; then every third key deleted */
    void *root = NULL;
    static int keys[40];
    for (int i = 0; i < 40; i++) {
        keys[i] = i;
        if (*(int **)tsearch(&keys[i], &root, icmp) != &keys[i])
            return 120;
    }
    int dup = 7;
    if (*(int **)tsearch(&dup, &root, icmp) != &keys[7])
        return 121;
    nvisited = deepest = 0;
    twalk(root, collect);
    if (nvisited != 40)
        return 122;
    for (int i = 0; i < 40; i++)
        if (visited[i] != i)
            return 123;
    if (deepest > 7)                       /* AVL: 1.44 log2(41), rounded */
        return 124;
    void **held = tfind(&keys[20], &root, icmp);
    for (int i = 0; i < 40; i += 3)
        if (!tdelete(&keys[i], &root, icmp))
            return 125;
    int absent = 3;
    if (tdelete(&absent, &root, icmp) || tfind(&absent, &root, icmp))
        return 126;
    /* a node a caller holds keeps naming its key across deletions */
    if (*(int **)held != &keys[20])
        return 127;
    nvisited = 0;
    twalk(root, collect);
    if (nvisited != 26)
        return 128;
    for (int i = 1; i < nvisited; i++)
        if (visited[i] <= visited[i - 1] || visited[i] % 3 == 0)
            return 129;
    tdestroy(root, NULL);

    /* random inserts and deletes against a model of which keys are in:
     * deletion is where a balanced tree's rotations go wrong */
    root = NULL;
    unsigned char in[64] = { 0 };
    unsigned lcg = 12345;
    for (int op = 0; op < 400; op++) {
        lcg = lcg * 1103515245u + 12345u;
        int k = (int)(lcg >> 16) % 64;
        if ((lcg >> 8) & 1) {
            tsearch(&keys[k % 40], &root, icmp);
            in[k % 40] = 1;
        } else {
            void *d = tdelete(&keys[k % 40], &root, icmp);
            if ((d != NULL) != in[k % 40])
                return 134;
            in[k % 40] = 0;
        }
        if (op % 50 == 49) {
            nvisited = deepest = 0;
            twalk(root, collect);
            int want = 0;
            for (int i = 0; i < 40; i++)
                if (in[i] && visited[want++] != i)
                    return 135;
            if (nvisited != want || deepest > 7)
                return 136;
            unbalanced = 0;
            twalk(root, balance);
            if (unbalanced)
                return 137;
        }
    }
    tdestroy(root, NULL);

    /* hcreate(4), then forty: the table grows (glibc's fills and refuses) */
    if (!hcreate(4))
        return 130;
    static char names[40][8];
    for (int i = 0; i < 40; i++) {
        sprintf(names[i], "k%d", i);
        if (!hsearch((ENTRY){ names[i], (void *)(long)(i * i) }, ENTER))
            return 131;
    }
    ENTRY *e = hsearch((ENTRY){ "k17", 0 }, FIND);
    if (!e || (long)e->data != 289 || hsearch((ENTRY){ "nope", 0 }, FIND))
        return 132;
    hdestroy();

    int arr[8] = { 4, 2 };
    size_t n = 2;
    int key = 9;
    lsearch(&key, arr, &n, sizeof(int), icmp);
    key = 2;
    if (n != 3 || arr[2] != 9 ||
        (int *)lsearch(&key, arr, &n, sizeof(int), icmp) != &arr[1] ||
        lfind(&(int){ 5 }, arr, &n, sizeof(int), icmp))
        return 133;
    return 0;
}

static int environment(void)
{
    extern char **environ;
    if (clearenv() != 0 || environ)
        return 140;
    if (setenv("A", "1", 1) || setenv("B", "2", 1) || setenv("A", "3", 0))
        return 141;
    if (strcmp(getenv("A"), "1") != 0 || strcmp(getenv("B"), "2") != 0)
        return 142;
    setenv("A", "4", 1);
    static char pe[] = "C=5";
    putenv(pe);
    pe[2] = '6';                           /* the caller's string IS the entry */
    if (strcmp(getenv("A"), "4") != 0 || strcmp(getenv("C"), "6") != 0)
        return 143;
    if (unsetenv("A") || getenv("A"))
        return 144;
    errno = 0;
    if (setenv("X=Y", "1", 1) != -1 || errno != EINVAL ||
        unsetenv("") != -1 || setenv(NULL, "1", 1) != -1)
        return 145;
    int count = 0;
    for (char **p = environ; *p; p++)
        count++;
    if (count != 2)
        return 146;
    /* many, so the vector is reallocated under the strings */
    for (int i = 0; i < 100; i++) {
        char name[8], val[8];
        sprintf(name, "V%d", i);
        sprintf(val, "%d", i * 3);
        if (setenv(name, val, 1))
            return 147;
    }
    if (strcmp(getenv("V77"), "231") != 0 || strcmp(getenv("C"), "6") != 0)
        return 148;
    clearenv();
    return 0;
}

int main(void)
{
    int r;
    if ((r = strings()) || (r = paths()) || (r = globs()) ||
        (r = options()) || (r = generators()) || (r = tables()) ||
        (r = environment()))
        return r;
    return 42;
}
