/* A macro whose expansion names itself -- `#define m a.m`, which is how
 * glibc's <signal.h> gives siginfo_t's fields (si_pid is
 * _sifields._kill.si_pid) -- expands once and never again (C11
 * 6.10.3.4p2), however many macros the text is an argument of. EmbCC's
 * preprocessor expanded it again at each level: `R(s.m)` through
 * `R(x) Q(x)` read `s.a.a.a.m`. */
// expect-exit: 42
#include <string.h>

struct inner { int m; };
struct outer { struct inner a; int b; };

#define m a.m
#define b_alias b
#define Q(x) (x)
#define R(x) Q(x)
#define S(x) R(x)
#define V(...) (__VA_ARGS__)
#define STR(x) #x
#define XSTR(x) STR(x)

/* the siginfo shape: a member reached through a union inside */
struct info2 { int code; union { struct { int si_pid, uid; } kill; } fields; };
#define si_pid fields.kill.si_pid

int main(void)
{
    struct outer s;
    s.m = 7;
    s.b = 3;
    int r = 0;
    r += Q(s.m) + R(s.m) + S(s.m) + V(s.m) + Q(R(S(s.m))); /* 7 * 5 = 35 */
    r += S(s.b_alias);                             /* 3, so 38 */
    struct info2 si;
    si.si_pid = 4;
    r += V(si.si_pid);                             /* 42 */
    if (strcmp(STR(s.m), "s.m") || strcmp(XSTR(s.m), "s.a.m"))
        return 1;
    return r;
}
