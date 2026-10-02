/* Aggregates by value on ARMv7-M (D-015): every shape AAPCS32 treats
 * differently.
 *
 * One byte and three (a partial word, which is assembled and taken
 * apart a byte at a time), four (returned in r0), eight and twelve and
 * twenty (returned through a hidden pointer), one with padding, one
 * containing a double, one SPLIT across r3 and the stack, and one
 * passed after the register file is spent.
 *
 * The reference is the host: what a struct's members add up to does not
 * depend on the machine. Whether they are passed the way another ARM
 * toolchain passes them is a different question, and thumb-exec.sh
 * answers it separately by linking a clang-compiled callee against this
 * caller.
 */
extern void writec(int c); extern void puts_(const char *s); extern void putn(long v);
struct s1 { char a; };
struct s3 { char a, b, c; };
struct s8 { int a, b; };
struct s12 { int a, b, c; };
struct s20 { int v[5]; };
struct mix { char c; int i; short s; };
struct dbl { double d; int i; };

static struct s1  m1(char a){ struct s1 r; r.a=a; return r; }
static struct s3  m3(int k){ struct s3 r; r.a=(char)k; r.b=(char)(k+1); r.c=(char)(k+2); return r; }
static struct s8  m8(int a,int b){ struct s8 r; r.a=a; r.b=b; return r; }
static struct s20 m20(int k){ struct s20 r; for(int i=0;i<5;i++) r.v[i]=k+i; return r; }
static struct mix mm(int k){ struct mix r; r.c=(char)k; r.i=k*2; r.s=(short)(k*3); return r; }
static struct dbl md(int k){ struct dbl r; r.d=(double)k/4.0; r.i=k; return r; }

static int u1(struct s1 s){ return s.a; }
static int u3(struct s3 s){ return s.a + s.b*10 + s.c*100; }
static int u8(struct s8 s){ return s.a*10 + s.b; }
static int u20(struct s20 s){ int t=0; for(int i=0;i<5;i++) t+=s.v[i]*(i+1); return t; }
static int umix(struct mix s){ return s.c + s.i + s.s; }
static int udbl(struct dbl s){ return (int)(s.d*8.0) + s.i; }
/* Split across r3 and the stack, and entirely on the stack. */
static int split(int a,int b,int c, struct s8 s, struct s12 t)
{ return a+b+c + s.a*10 + s.b*100 + t.a + t.b*2 + t.c*3; }
static int sret_then_args(int z, struct s20 s){ return z + s.v[0] + s.v[4]; }

int main(void)
{
    putn(u1(m1(7))); putn(u3(m3(1))); putn(u8(m8(3,4))); putn(u20(m20(10)));
    putn(umix(mm(5))); putn(udbl(md(9)));
    writec('\n');
    { struct s8 s = {1,2}; struct s12 t = {3,4,5};
      putn(split(6,7,8,s,t)); }
    { struct s20 s = m20(100); putn(sret_then_args(1, s)); }
    writec('\n');
    { struct s8 a = m8(1,2), b = a; b.a = 9;
      putn(a.a); putn(a.b); putn(b.a); putn(b.b); }
    { struct s20 v = m20(0); struct s20 w = v; w.v[2] = 77;
      putn(v.v[2]); putn(w.v[2]); }
    puts_("\n==END==\n");
    return 0;
}
