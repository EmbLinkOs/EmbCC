/* A static integer as wide as a pointer, initialized with an address:
 * `(uintptr_t)&stack[N]`, the way FreeRTOS's RISC-V port computes the top
 * of its interrupt stack. GCC and Clang take it -- the linker writes the
 * address -- and EmbCC refused it as not a constant. Also through a
 * cast of the array itself, with a constant added after the cast, and of
 * a function. */
// expect-exit: 42
typedef __UINTPTR_TYPE__ uptr;

static unsigned stack[64];
static void f(void) {}

static const uptr top = (uptr)&(stack[64 & ~7]);
static const uptr base = (uptr)stack;
static uptr mid = (uptr)&stack[16] + 4;
uptr fn_addr = (uptr)f;
static const uptr table[3] = { (uptr)&stack[1], 0, (uptr)&stack[2] };

int main(void)
{
    int bad = 0;
    if (top != (uptr)&stack[64]) bad |= 1;
    if (base != (uptr)&stack[0]) bad |= 2;
    if (mid != (uptr)&stack[16] + 4) bad |= 4;
    if (fn_addr != (uptr)f) bad |= 8;
    if (table[0] != (uptr)&stack[1] || table[1] != 0 ||
        table[2] != (uptr)&stack[2]) bad |= 16;
    return bad ? bad : 42;
}
