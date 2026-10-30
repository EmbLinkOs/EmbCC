/* wordexp and wordfree: 105 strings and what each expands to. Every
 * answer is glibc 2.36's for the same call, except the rows marked G,
 * where glibc departs from POSIX and the shell (bash --posix gave the
 * answer written) -- a glibc run skips those. The command substitutions
 * need a /bin/sh: under the Linux harness that is this program, which
 * as `sh -c` runs the few commands the table uses, and as `sh -n -c`
 * checks one parses (glibc asks that when a command fails). */
// expect-exit: 42
// needs-libc: <wordexp.h>, from lib/libc
#define _GNU_SOURCE 1
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wordexp.h>
#include <sys/stat.h>

#define G 8                    /* glibc answers otherwise */

struct w {
    const char *in;
    int flags;
    const char *ifs;           /* 0: unset */
    int group;                 /* 0 anywhere, 1 a filesystem, 2 a shell */
    const char *want;          /* status:count:words joined by | */
};

#define W(in, f, ifs, g, want) { in, f, ifs, g, want }
static const struct w g_w[] = {
    W("a b  c", 0, 0, 0, "0:3:a|b|c"),
    W("'a b' c", 0, 0, 0, "0:2:a b|c"),
    W("\"a  b\"", 0, 0, 0, "0:1:a  b"),
    W("a\\ b", 0, 0, 0, "0:1:a b"),
    W("\"\"", 0, 0, 0, "0:1:"),
    W("''x''", 0, 0, 0 | G, "0:1:x"), /* glibc: two words, "" and x */
    W("", 0, 0, 0, "0:0:"),
    W("   ", 0, 0, 0, "0:0:"),
    W("$V", 0, 0, 0, "0:2:one|two"),
    W("\"$V\"", 0, 0, 0, "0:1:one  two"),
    W("x$V'y'", 0, 0, 0, "0:2:xone|twoy"),
    W("${V}", 0, 0, 0, "0:2:one|two"),
    W("${#V}", 0, 0, 0, "0:1:8"),
    W("${#U}", 0, 0, 0, "0:1:0"),
    W("${U:-def ault}", 0, 0, 0, "0:2:def|ault"),
    W("\"${U:-def ault}\"", 0, 0, 0, "0:1:def ault"),
    W("${U-x}", 0, 0, 0, "0:1:x"),
    W("${E-x}", 0, 0, 0, "0:0:"),
    W("${E:-x}", 0, 0, 0, "0:1:x"),
    W("\"${E}\"", 0, 0, 0 | G, "0:1:"), /* glibc: WRDE_NOSPACE */
    W("${E}", 0, 0, 0, "0:0:"),
    W("a${E}b", 0, 0, 0, "0:1:ab"),
    W("${V:+set}", 0, 0, 0, "0:1:set"),
    W("${U:+set}", 0, 0, 0, "0:0:"),
    W("${E+set}", 0, 0, 0, "0:1:set"),
    W("${E:+set}", 0, 0, 0, "0:0:"),
    W("${P%.c}", 0, 0, 0, "0:1:file.tar"),
    W("${P%%.*}", 0, 0, 0, "0:1:file"),
    W("${P#*.}", 0, 0, 0, "0:1:tar.c"),
    W("${P##*.}", 0, 0, 0, "0:1:c"),
    W("${P%x}", 0, 0, 0, "0:1:file.tar.c"),
    W("${P#\"file\"}", 0, 0, 0, "0:1:.tar.c"),
    W("$((1+2*3))", 0, 0, 0, "0:1:7"),
    W("$(( (1+2)*3 ))", 0, 0, 0, "0:1:9"),
    W("$((7/2)) $((7%3)) $((1<<4)) $((5>3)) $((2>3?10:20))", 0, 0, 0 | G, "0:5:3|1|16|1|20"), /* glibc: 3|7|1|5|2, stopping at an operator it has not got */
    W("$((N+1))", 0, 0, 0 | G, "0:1:42"), /* glibc reads no name in arithmetic */
    W("$(($N*2))", 0, 0, 0, "0:1:82"),
    W("$((-3/2))", 0, 0, 0, "0:1:-1"),
    W("$((0x10 + 010))", 0, 0, 0, "0:1:24"),
    W("$((!0 && 3 || 0)) $((~0)) $((6&3|8^1))", 0, 0, 0 | G, "0:3:1|-1|11"), /* glibc has no && or || */
    W("$((1/0))", 0, 0, 0, "5:0:"),
    W("$((1+))", 0, 0, 0, "5:0:"),
    W("a|b", 0, 0, 0, "2:0:"),
    W("a;b", 0, 0, 0, "2:0:"),
    W("a&b", 0, 0, 0, "2:0:"),
    W("a<b", 0, 0, 0, "2:0:"),
    W("(x)", 0, 0, 0, "2:0:"),
    W("{a}", 0, 0, 0, "2:0:"),
    W("a\nb", 0, 0, 0, "2:0:"),
    W("'a|b;c'", 0, 0, 0, "0:1:a|b;c"),
    W("\"a(b)c\"", 0, 0, 0, "0:1:a(b)c"),
    W("a\\;b", 0, 0, 0, "0:1:a;b"),
    W("'abc", 0, 0, 0, "5:0:"),
    W("\"abc", 0, 0, 0, "5:0:"),
    W("${V", 0, 0, 0, "5:0:"),
    W("$((1+2)", 0, 0, 0, "5:0:"),
    W("$U", WRDE_UNDEF, 0, 0, "3:0:"),
    W("${U}", WRDE_UNDEF, 0, 0, "3:0:"),
    W("${U-ok}", WRDE_UNDEF, 0, 0 | G, "0:1:ok"), /* glibc: WRDE_BADVAL, though the word supplies the value (set -u agrees) */
    W("$V", WRDE_UNDEF, 0, 0, "0:2:one|two"),
    W("$(ls)", WRDE_NOCMD, 0, 0, "4:0:"),
    W("`ls`", WRDE_NOCMD, 0, 0, "4:0:"),
    W("'$(ls)'", WRDE_NOCMD, 0, 0, "0:1:$(ls)"),
    W("\"$(ls)\"", WRDE_NOCMD, 0, 0, "4:0:"),
    W("$Q", 0, ":", 0, "0:3:a||b"),
    W("$R", 0, " :", 0, "0:4:a|b||c"),
    W("$V", 0, "", 0, "0:1:one  two"),
    W("$V", 0, "o", 0, "0:2:|ne  tw"),
    W("~", 0, 0, 0, "0:1:/home/me"),
    W("~/x y", 0, 0, 0, "0:2:/home/me/x|y"),
    W("a~", 0, 0, 0, "0:1:a~"),
    W("\"~\"", 0, 0, 0, "0:1:~"),
    W("~\"/x\"", 0, 0, 0 | G, "0:1:~/x"), /* glibc: WRDE_SYNTAX */
    W("~nosuchuser/x", 0, 0, 0, "0:1:~nosuchuser/x"),
    W("$", 0, 0, 0, "0:1:$"),
    W("a$ b", 0, 0, 0, "0:2:a$|b"),
    W("$1", 0, 0, 0, "0:0:"),
    W("$#", 0, 0, 0, "0:1:0"),
    W("$?", 0, 0, 0 | G, "0:1:0"), /* glibc: $? kept as it is */
    W("$@", 0, 0, 0, "0:0:"),
    W("${W:=new} $W", 0, 0, 0, "0:2:new|new"),
    W("/nonexist/*.c", 0, 0, 0, "0:1:/nonexist/*.c"),
    W("'*'", 0, 0, 0, "0:1:*"),
    W("\\*", 0, 0, 0, "0:1:*"),
    W("a\\\\b", 0, 0, 0, "0:1:a\\b"),
    W("\"a\\$b\\\"c\\\\d\\e\"", 0, 0, 0, "0:1:a$b\"c\\d\\e"),
    W("$S", 0, 0, 0, "0:2:/nonexist/*|z"),
    W("\"$S\"", 0, 0, 0, "0:1:/nonexist/* z"),
    W("/tmp/wt/*.c", 0, 0, 1, "0:2:/tmp/wt/a.c|/tmp/wt/b.c"),
    W("\"/tmp/wt/*.c\"", 0, 0, 1, "0:1:/tmp/wt/*.c"),
    W("/tmp/wt/\\*.c", 0, 0, 1, "0:1:/tmp/wt/*.c"),
    W("$G", 0, 0, 1 | G, "0:1:/tmp/wt/c.h"), /* glibc does not glob an expansion */
    W("\"$G\"", 0, 0, 1, "0:1:/tmp/wt/*.h"),
    W("/tmp/wt/[ab].c x", 0, 0, 1, "0:3:/tmp/wt/a.c|/tmp/wt/b.c|x"),
    W("/tmp/wt/*.none", 0, 0, 1, "0:1:/tmp/wt/*.none"),
    W("$(echo hi there)", WRDE_SHOWERR, 0, 2, "0:2:hi|there"),
    W("\"$(echo hi there)\"", WRDE_SHOWERR, 0, 2, "0:1:hi there"),
    W("`echo a  b`", WRDE_SHOWERR, 0, 2, "0:2:a|b"),
    W("$(printf 'x\\n\\n')", WRDE_SHOWERR, 0, 2, "0:1:x"),
    W("x$(echo)y", WRDE_SHOWERR, 0, 2, "0:1:xy"),
    W("$(echo $V)", WRDE_SHOWERR, 0, 2, "0:2:one|two"),
    W("$(echo \"(\")", WRDE_SHOWERR, 0, 2, "0:1:("),
    W("$(exit 3)z", WRDE_SHOWERR, 0, 2 | G, "0:1:z"), /* glibc: WRDE_NOSPACE for a command that fails */
    W("$(if)", WRDE_SHOWERR, 0, 2 | G, "5:0:"), /* glibc: WRDE_NOSPACE for a command that fails */
    W("$(echo err >&2)k", 0, 0, 2 | G, "0:1:k"), /* glibc: WRDE_NOSPACE with no /dev/null, as in the harness's root */
};

#ifdef EMBCC_TEST_HAS_FS
/* ---- the shell the table's commands need ----------------------------------- */

static void say(int fd, const char *s) { write(fd, s, strlen(s)); }

/* words of one line: '...' and "..." taken whole, $NAME replaced (and,
 * outside quotes, split on spaces); a word >&2 sends output to fd 2 */
static int as_shell(const char *cmd)
{
    char line[256];
    for (const char *p = cmd; *p;) {
        size_t n = strcspn(p, "\n");
        if (n >= sizeof line)
            return 2;
        memcpy(line, p, n);
        line[n] = 0;
        p += n + (p[n] == '\n');
        char *av[16], buf[512];
        int ac = 0, out = 1;
        size_t b = 0;
        for (char *s = line; *s;) {
            while (*s == ' ')
                s++;
            if (!*s)
                break;
            if (!strncmp(s, ">&2", 3)) {
                out = 2;
                s += 3;
                continue;
            }
            av[ac++] = buf + b;
            while (*s && *s != ' ') {
                char q = *s == '\'' || *s == '"' ? *s++ : 0;
                for (; *s && (q ? *s != q : *s != ' ' && *s != '\'' && *s != '"'); s++) {
                    if (*s == '$' && q != '\'') {
                        char nm[32];
                        size_t k = 0;
                        for (s++; k < 31 && (s[0] == '_' || (s[0] >= 'A' && s[0] <= 'Z')); s++)
                            nm[k++] = *s;
                        nm[k] = 0;
                        s--;
                        const char *v = getenv(nm);
                        for (; v && *v; v++) {
                            if (!q && *v == ' ') {
                                if (buf[b - 1]) {
                                    buf[b++] = 0;
                                    av[ac++] = buf + b;
                                }
                                continue;
                            }
                            buf[b++] = *v;
                        }
                        continue;
                    }
                    buf[b++] = *s;
                }
                if (q && *s)
                    s++;
            }
            buf[b++] = 0;
        }
        if (!ac)
            continue;
        if (!strcmp(av[0], "exit"))
            return ac > 1 ? atoi(av[1]) : 0;
        if (!strcmp(av[0], "echo")) {
            for (int i = 1; i < ac; i++) {
                if (i > 1)
                    say(out, " ");
                say(out, av[i]);
            }
            say(out, "\n");
        } else if (!strcmp(av[0], "printf") && ac > 1) {
            for (char *s = av[1]; *s; s++)
                if (s[0] == '\\' && s[1] == 'n') {
                    say(out, "\n");
                    s++;
                } else {
                    write(out, s, 1);
                }
        } else {
            return 127;
        }
    }
    return 0;
}

static void put(const char *p)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);
}
#endif

/* ---- the table ---------------------------------------------------------------- */

static int table(int fs, int shell)
{
    setenv("V", "one  two", 1);
    unsetenv("U");
    setenv("E", "", 1);
    setenv("P", "file.tar.c", 1);
    setenv("N", "41", 1);
    setenv("HOME", "/home/me", 1);
    setenv("Q", "a::b:", 1);
    setenv("R", " a : b :: c ", 1);
    setenv("G", "/tmp/wt/*.h", 1);
    setenv("S", "/nonexist/* z", 1);
    unsetenv("W");
    int bad = 0;
    for (size_t i = 0; i < sizeof g_w / sizeof g_w[0]; i++) {
        const struct w *k = &g_w[i];
        int group = k->group & ~G;
        if ((group == 1 && !fs) || (group == 2 && !shell))
            continue;
#ifdef __GLIBC__
        if (k->group & G)
            continue;
#endif
        if (k->ifs)
            setenv("IFS", k->ifs, 1);
        else
            unsetenv("IFS");
        wordexp_t we;
        memset(&we, 0, sizeof we);
        int rc = wordexp(k->in, &we, k->flags);
        char got[512];
        int n = snprintf(got, sizeof got, "%d:%zu:", rc,
                         rc ? (size_t)0 : we.we_wordc);
        for (size_t j = 0; !rc && j < we.we_wordc; j++)
            n += snprintf(got + n, sizeof got - n, "%s%s", j ? "|" : "",
                          we.we_wordv[j]);
        if (!rc)
            wordfree(&we);
        if (strcmp(got, k->want)) {
            printf("wordexp(%s): %s, want %s\n", k->in, got, k->want);
            bad++;
        }
    }
    unsetenv("IFS");
    return bad;
}

/* ---- the flags that shape the vector ---------------------------------------------- */

static int vector(void)
{
    wordexp_t we;
    we.we_offs = 2;
    if (wordexp("a b", &we, WRDE_DOOFFS) || we.we_wordc != 2 ||
        we.we_offs != 2 || we.we_wordv[0] || we.we_wordv[1] ||
        strcmp(we.we_wordv[2], "a") || strcmp(we.we_wordv[3], "b") ||
        we.we_wordv[4])
        return 1;
    if (wordexp("c", &we, WRDE_DOOFFS | WRDE_APPEND) || we.we_wordc != 3 ||
        we.we_wordv[0] || strcmp(we.we_wordv[2], "a") ||
        strcmp(we.we_wordv[4], "c") || we.we_wordv[5])
        return 2;
    wordfree(&we);
    if (we.we_wordv)
        return 3;
    if (wordexp("x", &we, 0) || wordexp("y z", &we, WRDE_REUSE) ||
        we.we_wordc != 2 || strcmp(we.we_wordv[0], "y") || we.we_wordv[2])
        return 4;
    wordfree(&we);
    /* an error leaves no words to free */
    memset(&we, 0, sizeof we);
    if (wordexp("a|b", &we, 0) != WRDE_BADCHAR || we.we_wordc)
        return 5;
    wordfree(&we);
#ifndef __GLIBC__
    /* glibc returns 0 here, with no words, and does not survive a
     * second such call; POSIX makes it an error, as the shell does */
    unsetenv("U");
    int saved = dup(2), null = open("/dev/null", O_WRONLY);
    if (null >= 0)
        dup2(null, 2);
    int rc = wordexp("${U:?gone}", &we, 0);
    int rc2 = wordexp("a ${U:?} b", &we, 0);
    if (saved >= 0)
        dup2(saved, 2);
    if (rc != WRDE_BADVAL || rc2 != WRDE_BADVAL)
        return 6;
    setenv("U", "here", 1);
    if (wordexp("${U:?gone}", &we, 0) || we.we_wordc != 1 ||
        strcmp(we.we_wordv[0], "here"))
        return 7;
    wordfree(&we);
#endif
    return 0;
}

int main(int argc, char **argv)
{
    int fs = 0, shell = 0;
#ifdef EMBCC_TEST_HAS_FS
    if (argc >= 3 && !strcmp(argv[1], "-c"))
        return as_shell(argv[2]);
    /* sh -n -c: does it parse? Only a bare `if` does not */
    if (argc >= 4 && !strcmp(argv[1], "-n"))
        return strcmp(argv[3], "if") ? 0 : 2;
    mkdir("/tmp", 0777);
    mkdir("/tmp/wt", 0755);
    put("/tmp/wt/a.c");
    put("/tmp/wt/b.c");
    put("/tmp/wt/c.h");
    fs = 1;
    shell = access("/bin/sh", X_OK) == 0;
#endif
    (void)argc;
    (void)argv;
    int r = vector();
    if (r)
        return r;
    if (table(fs, shell))
        return 50;
    return 42;
}
