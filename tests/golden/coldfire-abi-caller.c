#include "coldfire-abi.h"
extern void writec(int c); extern void puts_(const char *s); extern void putn(long v);
static void p64(long long v) { putn((long)(unsigned)v); putn((long)(unsigned)(v >> 32)); }
int main(void){
  static char buf[16];
  putn(narrow(-5, 200, -300, 60000, 7)); putn(narrow(127, 0, 32767, 1, -1));
  { struct c1 a = { 9 }; struct c2 b = { 1, 2 }; struct c3 c = { 3, 4, 5 };
    putn(smalls(a, 10, b, c, 20)); }
  { struct c5 a = { 2, 100000 }; struct c7 b = { -3, 1, 2, 3, 4, 5 };
    struct c10 c = { 6, 123456789012LL, 7 };
    p64(mids(a, b, -99999999999LL, c)); }
  writec('\n');
  { struct c3 s = r3(40); putn(s.a); putn(s.b); putn(s.c); }
  { struct c5 s = { 3, 1000 }; s = r5(s, 7); putn(s.a); putn(s.b); }
  { struct c10 s = r10(1000000000001LL); putn(s.a); p64(s.b); putn(s.c); }
  { _Complex float z = cf(1.5f, 2.5f); putn((long)(__real__ z * 10)); putn((long)(__imag__ z * 10)); }
  { int a = 11, b = 13, c = 17, d = 19;
    _Complex double z = cd(2.25, 8.5, 4);
    putn((long)(__real__ z * 100)); putn((long)(__imag__ z * 100));
    putn(keep_across(a, b, c, d) + a + b + c + d); }
  putn(ptr_ret(buf, 5) - buf);
  putn((long)dmix(-2, 1.5, -7, 0.25f, 1000000000000LL / 1000000));
  writec('\n');
  { struct c3 s3 = { 1, 2, 3 }; struct c5 s5 = { 4, 5 }; struct c3 t3 = { 6, 7, 8 };
    struct c5 t5 = { 9, 10 };
    p64(vstructs(2, s3, 50000000000LL, s5, 2.5, -1, t3, 7LL, t5, -0.125, 3)); }
  puts_("\n==END==\n"); return 0; }
