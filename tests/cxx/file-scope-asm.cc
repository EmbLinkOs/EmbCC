// A file-scope asm block in C++: passed to the C the front end writes, as
// g++ passes it to the assembler, and assembled as a C unit's. A label it
// defines is a function the C++ code calls (extern "C").
// expect-exit: 42
extern "C" int forty_two(void);
#if defined(__x86_64__)
asm(".globl forty_two\nforty_two:\n call the_answer\n ret\n");
#else
asm(".globl forty_two\nforty_two:\n b the_answer\n");
#endif
extern "C" int the_answer(void) { return 42; }
int main() { return forty_two(); }
