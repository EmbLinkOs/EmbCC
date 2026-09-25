#include <stdarg.h>
#include "thumb-abi.h"
struct s1  m1(char a){ struct s1 r; r.a=a; return r; }
struct s3  m3(int k){ struct s3 r; r.a=(char)k; r.b=(char)(k+1); r.c=(char)(k+2); return r; }
struct s8  m8(int a,int b){ struct s8 r; r.a=a; r.b=b; return r; }
struct s20 m20(int k){ struct s20 r; for(int i=0;i<5;i++) r.v[i]=k+i; return r; }
struct mix mm(int k){ struct mix r; r.c=(char)k; r.i=k*2; r.s=(short)(k*3); return r; }
int u1(struct s1 s){ return s.a; }
int u3(struct s3 s){ return s.a + s.b*10 + s.c*100; }
int u8(struct s8 s){ return s.a*10 + s.b; }
int u20(struct s20 s){ int t=0; for(int i=0;i<5;i++) t+=s.v[i]*(i+1); return t; }
int umix(struct mix s){ return s.c + s.i + s.s; }
int split(int a,int b,int c, struct s8 s, struct s12 t)
{ return a+b+c + s.a*10 + s.b*100 + t.a + t.b*2 + t.c*3; }
long long mix64(int a, long long b, int c, struct s8 s)
{ return (long long)a + b*3 + c + s.a*10 + s.b*100; }

int vsum(int n, ...)
{
    va_list ap; int t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) t += va_arg(ap, int);
    va_end(ap);
    return t;
}
long long vmix(int n, ...)
{
    va_list ap; long long t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        if (i & 1) t += va_arg(ap, long long);
        else       t += va_arg(ap, int);
    }
    va_end(ap);
    return t;
}
int vafter4(int a, int b, int c, int d, ...)
{
    va_list ap; int t = a + b + c + d;
    va_start(ap, d);
    t += va_arg(ap, int);
    t += va_arg(ap, int);
    va_end(ap);
    return t;
}
