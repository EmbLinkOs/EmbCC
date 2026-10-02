/* A designator can be a path, `.a.b =`, `[1].a.c =`, `[1][2] =` (C99),
 * and a name inside an anonymous struct or union is reached through it,
 * `.b1 =` (C11). After the designated subobject, initialization carries
 * on inside the innermost aggregate the path entered, then outward, as
 * if its braces had been elided. EmbCC took one step only and stopped at
 * "expected '=' after a field designator before '.'", and an anonymous
 * member's name was "no member". Static and automatic objects, and an
 * unsized array the path sizes. */
// expect-exit: 42
struct in { int b, c; };
struct out { struct in a; int d; int arr[3]; };
struct reg { int k; union { unsigned w; struct { unsigned char b0, b1, b2, b3; }; }; struct { int x, y; }; };
/* static storage */
static struct out g1 = { .a.b = 1, 2, 3 };                  /* b=1 c=2 d=3: the path continues */
static struct out g2[] = { [1].a.c = 9, 10, [0].arr[2] = 4 };   /* 2 elements */
static struct reg g3 = { .k = 1, .b1 = 0x22, 0x33, .y = 6 };   /* anonymous members */
int main(void)
{
    int bad = 0;
    struct out o1 = { .a.b = 1, 2, 3 };
    struct out o2 = { .a.c = 5, .arr[1] = 7, 8 };
    struct out o3[2] = { [1].a.b = 9, 10, [0].d = 4 };
    struct out o4 = { .a = { 1, 2 }, .a.c = 9 };
    struct out o5 = { .arr = { 1 }, .arr[2] = 3 };
    int m[2][3] = { [1][2] = 5, [0][1] = 2, 3 };
    struct reg r = { .k = 1, .b1 = 0x22, .x = 5, 6 };
    struct reg r2 = { .w = 0x3344u, .y = 3 };
    if (g1.a.b != 1 || g1.a.c != 2 || g1.d != 3) bad |= 1;
    if (sizeof g2 != 2 * sizeof(struct out) || g2[1].a.c != 9 || g2[1].d != 10 || g2[0].arr[2] != 4) bad |= 2;
    if (g3.k != 1 || g3.b1 != 0x22 || g3.b2 != 0x33 || g3.y != 6 || g3.b0 != 0) bad |= 4;
    if (o1.a.b != 1 || o1.a.c != 2 || o1.d != 3) bad |= 8;
    if (o2.a.c != 5 || o2.arr[1] != 7 || o2.arr[2] != 8 || o2.a.b != 0) bad |= 16;
    if (o3[1].a.b != 9 || o3[1].a.c != 10 || o3[0].d != 4 || o3[0].a.b != 0) bad |= 32;
    if (o4.a.b != 1 || o4.a.c != 9) bad |= 64;
    if (o5.arr[0] != 1 || o5.arr[1] != 0 || o5.arr[2] != 3) bad |= 128;
    if (m[1][2] != 5 || m[0][1] != 2 || m[0][2] != 3 || m[1][0] != 0) bad |= 256;
    if (r.k != 1 || r.b1 != 0x22 || r.x != 5 || r.y != 6) bad |= 512;
    if (r2.w != 0x3344u || r2.y != 3) bad |= 1024;
    return bad ? 1 + (bad & 0x3f) + (bad >> 6 ? 64 : 0) : 42;
}
