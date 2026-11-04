#include <ptrcheck.h>
#include <stdlib.h>
struct v { int n; int *__counted_by(n) items; };
struct w { unsigned len; char text[] __counted_by(len); };
int get(struct v *x, int i) { return x->items[i]; }
int ch(struct w *x, int i) { return x->text[i]; }
int main(int argc, char **argv) { int a[3] = {1,2,3}; struct v x = { 3, a }; struct w *y = malloc(sizeof *y + 2); y->len = 2; y->text[0] = 'a'; y->text[1] = 'b'; return get(&x, argc > 1 ? 3 : 2) + ch(y, argc > 2 ? 2 : 1); }
