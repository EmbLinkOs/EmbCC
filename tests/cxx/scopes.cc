// CX1: every way out of a scope destroys what the scope built — its end,
// return, break, continue, goto — and declarations in conditions live for
// the whole statement.
// expect-exit: 42
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static char trail[256];
static void note(const char *s) { strcat(trail, s); }

struct Log {
    char tag;
    Log(char t) : tag(t) { char s[3] = { '+', t, 0 }; note(s); }
    ~Log() { char s[3] = { '-', tag, 0 }; note(s); }
};

static int early(int k)
{
    Log a('a');
    if (k == 0)
        return 10;
    Log b('b');
    if (k == 1)
        return 20;
    {
        Log c('c');
        if (k == 2)
            return 30;
    }
    return 40;
}

static int jumps()
{
    int n = 0;
    for (int i = 0; i < 5; i++) {
        Log l('0' + i);
        if (i == 1)
            continue;
        if (i == 3)
            break;
        n++;
    }
    return n;
}

static void gone()
{
    int i = 0;
again:
    {
        Log g('g');
        if (++i < 3)
            goto again;
    }
}

static int pick(int v)
{
    switch (int w = v * 2) {
    case 2: {
        Log s('s');
        return w;
    }
    case 4:
        return w + 1;
    default:
        break;
    }
    return -1;
}

static int next_val(int *p) { return (*p)--; }

int main()
{
    trail[0] = 0;
    check("return 10", early(0) == 10 && strcmp(trail, "+a-a") == 0);
    trail[0] = 0;
    check("return 20", early(1) == 20 && strcmp(trail, "+a+b-b-a") == 0);
    trail[0] = 0;
    check("return 30", early(2) == 30 && strcmp(trail, "+a+b+c-c-b-a") == 0);
    trail[0] = 0;
    check("return 40", early(3) == 40 && strcmp(trail, "+a+b+c-c-b-a") == 0);

    trail[0] = 0;
    check("break and continue", jumps() == 2 &&
          strcmp(trail, "+0-0+1-1+2-2+3-3") == 0);

    trail[0] = 0;
    gone();
    check("goto out of a scope", strcmp(trail, "+g-g+g-g+g-g") == 0);

    trail[0] = 0;
    check("switch with an init", pick(1) == 2 && pick(2) == 5 &&
          pick(3) == -1 && strcmp(trail, "+s-s") == 0);

    int total = 0;
    if (int v = 40 + 2; v > 41)
        total = v;
    check("if with an initializer", total == 42);

    int count = 3, seen = 0;
    while (int k = next_val(&count))
        seen += k;
    check("while with a declaration", seen == 6 && count == -1);

    if (int *p = &total)
        *p += 1;
    check("if with a declaration", total == 43);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
