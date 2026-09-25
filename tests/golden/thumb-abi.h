/* The shapes tests/golden/thumb-abi-{caller,callee}.c pass between
 * them, so that one may be compiled by EmbCC and the other by clang and
 * the two must still agree. */
struct s1 { char a; };
struct s3 { char a, b, c; };
struct s8 { int a, b; };
struct s12 { int a, b, c; };
struct s20 { int v[5]; };
struct mix { char c; int i; short s; };

struct s1  m1(char a);
struct s3  m3(int k);
struct s8  m8(int a, int b);
struct s20 m20(int k);
struct mix mm(int k);
int u1(struct s1 s);
int u3(struct s3 s);
int u8(struct s8 s);
int u20(struct s20 s);
int umix(struct mix s);
/* Three words placed, then a two-word composite: r3 takes its first
 * word and the stack its second. */
int split(int a, int b, int c, struct s8 s, struct s12 t);
/* An eight-byte SCALAR, which rounds the register number up to even
 * where a four-aligned composite does not. */
long long mix64(int a, long long b, int c, struct s8 s);

/* Variadic, where the ABI question is whether the callee's register
 * save area lines up with where the caller left the arguments. */
int vsum(int n, ...);
long long vmix(int n, ...);
int vafter4(int a, int b, int c, int d, ...);
