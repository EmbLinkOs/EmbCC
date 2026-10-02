/* With <stdbool.h>, bool/true/false are its macros: true is the int 1,
 * not C23's bool constant (c23-bool.c). */
// expect-exit: 42
#include <stdbool.h>
int main(void) { bool b = true; return b == 1 && sizeof(true) == sizeof(int) ? 42 : 1; }
