#include "thumb-abi.h"
extern void writec(int c); extern void puts_(const char *s); extern void putn(long v);
int main(void){
  putn(u1(m1(7))); putn(u3(m3(1))); putn(u8(m8(3,4)));
  putn(u20(m20(10))); putn(umix(mm(5))); writec('\n');
  { struct s8 s={1,2}; struct s12 t={3,4,5}; putn(split(6,7,8,s,t)); }
  { struct s8 s={2,3}; long long r = mix64(1, 1000000000000LL, 4, s);
    putn((long)(unsigned)r); putn((long)(unsigned)(r >> 32)); }
  puts_("\n==END==\n"); return 0; }
