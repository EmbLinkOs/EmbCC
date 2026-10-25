/* The callee half of tests/golden/float16-abi.sh: functions that only
 * MOVE binary16 values -- no arithmetic, so no conversion routine is
 * called, and an object of any compiler links without a runtime. Each
 * argument list puts a half among the others in a way that would show a
 * misplaced one: after an int, between floats, as the ninth and tenth FP
 * argument (the stack, and Apple's packed slots), in a struct. */
typedef _Float16 h16;

h16 x_id(h16 a) { return a; }

h16 x_pick(int i, h16 a, float f, h16 b, double d, h16 c)
{
    (void)f;
    (void)d;
    return i == 0 ? a : i == 1 ? b : c;
}

float x_after(h16 a, float f) { (void)a; return f; }
int x_int(h16 a, int i, h16 b) { (void)a; (void)b; return i; }

h16 x_nth(int k, h16 a0, h16 a1, h16 a2, h16 a3, h16 a4, h16 a5, h16 a6,
          h16 a7, h16 a8, h16 a9)
{
    switch (k) {
    case 0: return a0; case 1: return a1; case 2: return a2;
    case 3: return a3; case 4: return a4; case 5: return a5;
    case 6: return a6; case 7: return a7; case 8: return a8;
    default: return a9;
    }
}

#ifdef WITH_STRUCTS
struct p2 { h16 a, b; };
struct mx { h16 a; int i; h16 b; };
struct p2 x_swap(struct p2 p) { struct p2 r; r.a = p.b; r.b = p.a; return r; }
struct mx x_mx(struct mx m) { struct mx r; r.a = m.b; r.i = m.i + 1; r.b = m.a; return r; }
#endif
