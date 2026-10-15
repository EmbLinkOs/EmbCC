/* va_end does nothing to the list, but its argument is an expression
 * and is evaluated: `va_end(**p++)` advances p (GCC's va-arg-21.c). It
 * was dropped, and the next va_start/va_arg went through the null slot
 * p should have stepped over. */
// expect-exit: 42
#include <stdarg.h>
#include <stddef.h>

__attribute__((noinline)) static int doit(int n, ...)
{
    va_list l1, l2, *arr[3], **p = arr;
    arr[0] = &l1;
    arr[1] = NULL;
    arr[2] = &l2;
    va_start(*arr[0], n);
    int a = va_arg(**p, int);
    va_end(**p++);
    p++;
    va_start(*arr[2], n);
    int b = va_arg(**p, int);
    va_end(**p);
    return a + b + (int)(p - arr);
}

int main(void)
{
    return doit(1, 20);
}
