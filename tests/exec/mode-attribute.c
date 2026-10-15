/* __attribute__((mode(M))) among the specifiers, on an enum typedef, a
 * member, a cast and a file-scope variable (each refused before). */
// expect-exit: 42
int printf(const char *, ...);
typedef int __attribute__ ((mode (SI))) int_t;
typedef unsigned int __attribute__ ((mode (QI))) u8_t;
typedef enum { ea, eb } __attribute__((__mode__(__QI__))) small_e;
typedef enum { A, B } E;
struct S { E __attribute__ ((mode (__byte__))) a; E __attribute__ ((mode (__byte__))) b; };
__attribute__((mode(HI))) int h = 70000;
int main(void)
{
    u8_t u = 300;
    long long w = (int __attribute__((mode(DI))))-1;
    int ok = 0;
    ok += sizeof(int_t) == 4 && sizeof(u8_t) == 1 && sizeof(small_e) == 1;
    ok += sizeof(struct S) == 2 && u == 44 && sizeof h == 2 && h == 4464;
    ok += w == -1;
    printf("%d %d %d\n", ok, (int)sizeof(struct S), (int)h);
    return ok == 3 ? 42 : ok;
}
