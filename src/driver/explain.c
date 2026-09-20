/* `embcc --explain E0004` — what a diagnostic means, why it fired, and the
 * smallest edit that fixes it (docs/TOOLING.md T6).
 *
 * A C compiler tells you what is wrong at the place it noticed. That is not
 * the same as telling you what the rule is. Rust's error index is the thing
 * worth stealing: a stable identifier per diagnostic, and an entry that
 * shows the mistake and the fix side by side. The identifier is printed
 * with the diagnostic, so the next step is always in front of the reader:
 *
 *     error: 'heigth' is not declared in 'area' [E0001]
 *     ...
 *     $ embcc --explain E0001
 *
 * An entry earns its place by being about a rule, not about a message. The
 * table starts small and honest: the diagnostics people actually hit.
 */
#include "util.h"

#include <stdio.h>
#include <string.h>

struct explain {
    const char *id;
    const char *title;
    const char *text;
};

static const struct explain g_explains[] = {
{ "E0001", "a name is used that was never declared",
"C has no global namespace of known names: every function and variable must\n"
"be declared before the line that uses it, either by a definition or by a\n"
"declaration (usually from a header).\n"
"\n"
"This fires for a misspelling, for a missing #include, and for a function\n"
"defined lower down in the file with no prototype above.\n"
"\n"
"    int area(int w, int h)\n"
"    {\n"
"        int height = h;\n"
"        return w * heigth;      /* E0001: 'heigth' */\n"
"    }\n"
"\n"
"The fix is whichever the case is: correct the spelling (EmbCC suggests the\n"
"nearest name in scope, and `embcc --fix` will make that edit), include the\n"
"header that declares it, or move the definition up — or put a prototype\n"
"above the first use:\n"
"\n"
"    int helper(int);            /* declared here, defined below */\n"
"\n"
"C99 6.5.1p2. (C89 let a call declare its function implicitly; C99 removed\n"
"that, and EmbCC follows C99.)"
},
{ "E0002", "a statement or declaration is missing its ';'",
"Every declaration and every expression statement ends in a semicolon. The\n"
"compiler notices at the FOLLOWING token, so the reported line is usually\n"
"the line after the one to edit — which is why the diagnostic also points\n"
"at where the ';' belongs.\n"
"\n"
"    int x = 5                   /* the ';' belongs here */\n"
"    return x;                   /* E0002 is reported here */\n"
"\n"
"EmbCC carries on as if the semicolon were there, so a file missing several\n"
"reports them all in one run, and `embcc --fix` inserts them all at once.\n"
"\n"
"C99 6.7 (declarations) and 6.8.3 (expression statements)."
},
{ "E0003", "a value of one type is used where another is required",
"C converts between arithmetic types on its own, but it will not invent a\n"
"conversion between a pointer and a struct, between unrelated struct types,\n"
"or between pointers to unrelated types.\n"
"\n"
"    struct Point p;\n"
"    int n = strlen(p);          /* E0003: struct Point is not char * */\n"
"\n"
"Read the diagnostic as 'the expression on the right is X, the place it is\n"
"going wants Y'. The fix is to produce a Y: pass the member you meant\n"
"(p.label), take an address (&p), or call the function that converts.\n"
"\n"
"A cast silences this, and is the wrong answer unless you can say why the\n"
"bytes are valid as the other type: the compiler has just told you it has\n"
"no reason to believe they are.\n"
"\n"
"C99 6.5.16.1 (assignment) and 6.5.2.2p7 (arguments)."
},
{ "E0004", "a struct or union has no such member",
"The member name is looked up in the type of the expression to the left of\n"
"the `.` or `->`, and that type has no member of that name.\n"
"\n"
"    struct Point { int x; int y; };\n"
"    int f(struct Point p) { return p.z; }   /* E0004 */\n"
"\n"
"Causes, in the order they are usually true: a misspelling; the wrong\n"
"variable; a type that has a member of that name in ANOTHER definition (two\n"
"structs with the same tag in different headers); or `.` where the value is\n"
"a pointer and `->` was meant.\n"
"\n"
"In an editor with embls running, completion after the `.` lists exactly\n"
"the members this type has.\n"
"\n"
"C99 6.5.2.3."
},
{ "E0005", "an object is declared of a type whose size is not known",
"A definition needs the size; a declaration or a pointer does not. A struct\n"
"that has only been named — `struct S;` — or one whose definition is in a\n"
"header that was not included, has no size here.\n"
"\n"
"    struct Missing m;           /* E0005: no definition in sight */\n"
"    struct Missing *p;          /* fine: a pointer needs no size */\n"
"\n"
"The fix is to include the header that defines it, or to keep a pointer.\n"
"An opaque type is a deliberate design: the library hands out pointers\n"
"precisely so the layout can change without rebuilding callers.\n"
"\n"
"C99 6.2.5p22, 6.7p7."
},
{ "E0006", "a function is called with the wrong number of arguments",
"A prototype fixes the count. A call that does not match it is an error,\n"
"not a warning: the callee will read arguments that were never passed.\n"
"\n"
"    int distance2(struct Point, struct Point);\n"
"    int d = distance2(a);       /* E0006: one argument, two wanted */\n"
"\n"
"An old-style declaration with an empty parameter list — `int f();` — is\n"
"NOT a prototype and promises nothing; write `int f(void)` for a function\n"
"that takes nothing.\n"
"\n"
"C99 6.5.2.2p2."
},
{ "E0007", "a value is assigned to something that cannot be assigned to",
"The left of an `=` must be a modifiable lvalue: an object you can name and\n"
"are allowed to change. An expression that merely computes a value is not,\n"
"and neither is a const object, an array name, or a string literal.\n"
"\n"
"    const int limit = 10;\n"
"    limit = 11;                 /* E0007: it is const */\n"
"    f(x) = 3;                   /* E0007: a call is not an object */\n"
"\n"
"If the const is the problem, decide which is true: the object should not\n"
"be const, or this code should not be writing to it. Casting away const and\n"
"writing is undefined behaviour when the object really is const.\n"
"\n"
"C99 6.3.2.1p1, 6.5.16p2."
},
{ "E0008", "control reaches the end of a function that must return a value",
"A function whose return type is not void must return a value on every path\n"
"that leaves it. EmbCC refuses rather than returning whatever happens to be\n"
"in the register, which is what makes such a bug so hard to find later.\n"
"\n"
"    int sign(int n)\n"
"    {\n"
"        if (n > 0) return 1;\n"
"        if (n < 0) return -1;   /* E0008: n == 0 falls off the end */\n"
"    }\n"
"\n"
"Either return something on that path, or say that it cannot be reached in\n"
"a way the compiler can check — an `else` that returns, or a call marked\n"
"_Noreturn (abort(), a panic()).\n"
"\n"
"C99 6.9.1p12."
},
};

/* ---- what declares a name ------------------------------------------------
 *
 * A name that is not declared is most often a missing #include, and the
 * compiler knows perfectly well which header declares printf. Saying so
 * turns "not declared" into an edit. The table is the C library's own
 * surface, which is fixed by the standard — not a guess about the
 * project's own names, which EmbCC suggests from what it has parsed.
 */
static const struct { const char *name; const char *header; } g_headers[] = {
    /* <stdio.h> */
    { "printf", "<stdio.h>" }, { "fprintf", "<stdio.h>" },
    { "sprintf", "<stdio.h>" }, { "snprintf", "<stdio.h>" },
    { "puts", "<stdio.h>" }, { "putchar", "<stdio.h>" },
    { "fputs", "<stdio.h>" }, { "fopen", "<stdio.h>" },
    { "fclose", "<stdio.h>" }, { "fread", "<stdio.h>" },
    { "fwrite", "<stdio.h>" }, { "fgets", "<stdio.h>" },
    { "scanf", "<stdio.h>" }, { "sscanf", "<stdio.h>" },
    { "perror", "<stdio.h>" }, { "fflush", "<stdio.h>" },
    { "fseek", "<stdio.h>" }, { "ftell", "<stdio.h>" },
    /* <stdlib.h> */
    { "malloc", "<stdlib.h>" }, { "calloc", "<stdlib.h>" },
    { "realloc", "<stdlib.h>" }, { "free", "<stdlib.h>" },
    { "exit", "<stdlib.h>" }, { "abort", "<stdlib.h>" },
    { "atoi", "<stdlib.h>" }, { "atol", "<stdlib.h>" },
    { "strtol", "<stdlib.h>" }, { "strtoul", "<stdlib.h>" },
    { "strtod", "<stdlib.h>" }, { "qsort", "<stdlib.h>" },
    { "bsearch", "<stdlib.h>" }, { "getenv", "<stdlib.h>" },
    { "rand", "<stdlib.h>" }, { "srand", "<stdlib.h>" },
    /* <string.h> */
    { "strlen", "<string.h>" }, { "strcpy", "<string.h>" },
    { "strncpy", "<string.h>" }, { "strcat", "<string.h>" },
    { "strncat", "<string.h>" }, { "strcmp", "<string.h>" },
    { "strncmp", "<string.h>" }, { "strchr", "<string.h>" },
    { "strrchr", "<string.h>" }, { "strstr", "<string.h>" },
    { "strdup", "<string.h>" }, { "memcpy", "<string.h>" },
    { "memmove", "<string.h>" }, { "memset", "<string.h>" },
    { "memcmp", "<string.h>" }, { "memchr", "<string.h>" },
    /* <math.h> */
    { "sqrt", "<math.h>" }, { "pow", "<math.h>" }, { "fabs", "<math.h>" },
    { "sin", "<math.h>" }, { "cos", "<math.h>" }, { "tan", "<math.h>" },
    { "log", "<math.h>" }, { "log2", "<math.h>" }, { "log10", "<math.h>" },
    { "exp", "<math.h>" }, { "floor", "<math.h>" }, { "ceil", "<math.h>" },
    { "round", "<math.h>" }, { "fmod", "<math.h>" },
    /* <ctype.h>, <assert.h>, <time.h>, <errno.h> */
    { "isalpha", "<ctype.h>" }, { "isdigit", "<ctype.h>" },
    { "isalnum", "<ctype.h>" }, { "isspace", "<ctype.h>" },
    { "isupper", "<ctype.h>" }, { "islower", "<ctype.h>" },
    { "toupper", "<ctype.h>" }, { "tolower", "<ctype.h>" },
    { "assert", "<assert.h>" },
    { "time", "<time.h>" }, { "clock", "<time.h>" },
    { "strftime", "<time.h>" }, { "localtime", "<time.h>" },
    { "errno", "<errno.h>" }, { "strerror", "<string.h>" },
    /* <unistd.h>, <fcntl.h> — POSIX, but what a program on this OS uses */
    { "write", "<unistd.h>" }, { "read", "<unistd.h>" },
    { "close", "<unistd.h>" }, { "open", "<fcntl.h>" },
    { "lseek", "<unistd.h>" }, { "unlink", "<unistd.h>" },
};

/* The header that declares `name`, or NULL. */
const char *header_declaring(const char *name)
{
    if (!name)
        return NULL;
    for (unsigned i = 0; i < sizeof g_headers / sizeof g_headers[0]; i++)
        if (!strcmp(g_headers[i].name, name))
            return g_headers[i].header;
    return NULL;
}

static const int g_nexplains =
    (int)(sizeof g_explains / sizeof g_explains[0]);

/* The title a diagnostic's id carries, for the "[E0001]" the reader sees. */
const char *explain_title(const char *id)
{
    for (int i = 0; i < g_nexplains; i++)
        if (!strcmp(g_explains[i].id, id))
            return g_explains[i].title;
    return NULL;
}

/* `embcc --explain ID`, or `--explain` for the list. Returns the exit
 * status. */
int explain_print(const char *id)
{
    if (!id || !*id) {
        printf("what embcc can explain (embcc --explain ID):\n\n");
        for (int i = 0; i < g_nexplains; i++)
            printf("  %s  %s\n", g_explains[i].id, g_explains[i].title);
        printf("\nA diagnostic prints its id, so the next step is always in\n"
               "front of you: `embcc --explain E0001`.\n");
        return 0;
    }
    char want[16];
    size_t n = 0;
    for (const char *p = id; *p && n + 1 < sizeof want; p++)
        want[n++] = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
    want[n] = 0;
    for (int i = 0; i < g_nexplains; i++)
        if (!strcmp(g_explains[i].id, want)) {
            printf("%s: %s\n\n%s\n", g_explains[i].id, g_explains[i].title,
                   g_explains[i].text);
            return 0;
        }
    fprintf(stderr, "embcc: no explanation for '%s'; `embcc --explain` lists "
                    "what there is\n", id);
    return 1;
}
