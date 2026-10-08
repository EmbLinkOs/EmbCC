/* The shapes tests/golden/riscv-hf-abi-{caller,callee}.c pass between
 * them under RISC-V's hardware floating-point calling convention
 * (ilp32f/ilp32d/lp64f/lp64d), so one side may be compiled by EmbCC and
 * the other by clang and the two must still agree.
 *
 * Every rule of the psABI's "Hardware Floating-point Calling Convention"
 * that a caller and a callee could read differently is here, each in a
 * shape that puts it at its edge: floats in fa0-fa7 and what happens
 * past fa7, doubles under the single-float ABI (integer registers), the
 * struct flattening -- one float, two, a float and an integer in either
 * order, arrays and nested structs opened up, complex numbers -- and the
 * shapes that do NOT flatten (three floats, a union, a pointer, two
 * integers), a struct that would flatten but finds too few registers
 * left, the zero-width bit-field quirk, variadic floats, and results. A
 * callee prints what it received as bits; a caller prints what came
 * back. */
typedef struct { float x, y; } ff;
typedef struct { double x, y; } dd;
typedef struct { float f; int i; } fi;
typedef struct { int i; float f; } iff;
typedef struct { char c; double d; } cd;
typedef struct { double d; long long l; } dl;
typedef struct { float a[2]; } fa2;
typedef struct { struct { float x; } in; float y; } nest;
typedef struct { float x, y, z; } f3;
typedef union { float f; int i; } uf;
typedef struct { float f; int *p; } fp_;
typedef struct { float f; } f1;
typedef struct { double d; float f; } df;
typedef struct { float f; int : 0; } fz;
typedef struct { float f; int : 0; float g; } fzf;
typedef struct { float f; int bf : 5; } fbf;
typedef struct { short s; double d; } sd;

void s_scalars(float a, int b, float c, double d, int e, double f);
void s_tenf(float a, float b, float c, float d, float e, float f, float g,
            float h, float i, float j, int k);
void s_tend(double a, double b, double c, double d, double e, double f,
            double g, double h, double i, double j, int k);
void s_structs(ff a, fi b, iff c, cd d, fa2 e, nest f, f1 g);
void s_noflat(f3 a, uf b, fp_ c, df d, dl e, sd f);
void s_wide(dd a, int k, dd b);
void s_bits(fz a, fzf b, fbf c);
void s_cplx(float _Complex a, double _Complex b, int k);
/* seven floats leave one f register: a two-float struct then takes the
 * integer rules, and the float after it still gets fa7 */
void s_lastf(float a, float b, float c, float d, float e, float f, float g,
             ff h, float i);
/* eight integers leave no a register: a float-and-int struct then goes
 * whole on the stack, and the float after it still gets fa0 */
void s_lastx(int a, int b, int c, int d, int e, int f, int g, int h,
             fi s, float x);
void s_var(int n, ...);

float r_float(float a, float b);
double r_double(double a, int b);
ff r_ff(float a);
dd r_dd(double a);
fi r_fi(int k);
iff r_iff(int k);
f3 r_f3(int k);
float _Complex r_cf(float a);
double _Complex r_cd(double a);
cd r_cd2(int k);

typedef float (*fn_ff)(float, float);
fn_ff r_ptr(void);
