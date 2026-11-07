static int counter = 40;
static const char *const msg = "fw ok\n";
int sh_write0(const char *s) { register long x0 __asm__("x0") = 4; register const char *x1 __asm__("x1") = s;
  __asm__ volatile("hlt #0xf000" : "+r"(x0) : "r"(x1) : "memory"); return (int)x0; }
void sh_exit(int code) { long blk[2] = { 0x20026, code }; register long x0 __asm__("x0") = 0x20; register long *x1 __asm__("x1") = blk;
  __asm__ volatile("hlt #0xf000" : "+r"(x0) : "r"(x1) : "memory"); for (;;); }
void cmain(void) {
  counter += 2; sh_write0(msg); sh_exit(counter); }
