#include <stdio.h>
#include <stdlib.h>
struct w { unsigned len; short text[] __attribute__((counted_by(len))); };
int main(void){ struct w *y = malloc(64); y->len = 5; printf("%zu %zu\n", __builtin_dynamic_object_size(y->text, 0), __builtin_object_size(y->text, 0)); return 0; }
