/* Switch dispatch, over the shapes that break a decision tree.
 *
 * A switch is lowered as a balanced binary tree over the sorted case
 * values rather than a compare chain (src/ir/irgen.c, switch_tree), so
 * dispatching to the last of N cases costs about log N comparisons
 * instead of N. Sorting is where such a lowering goes wrong, and it
 * goes wrong quietly -- the common cases still work.
 *
 * So: dense and sparse, values either side of zero, values either side
 * of the SIGNED boundary in an unsigned switch (0x7fffffff next to
 * 0x80000000, where a signed comparison puts them in the wrong halves
 * of the tree and the search never reaches one of them), fall-through
 * between cases, a default in the middle, and a 64-bit switch at both
 * extremes of the range.
 *
 * The answer is a hash of every result, so one wrong dispatch anywhere
 * changes it -- and the same source compiled by gcc produces the same
 * number, which tests/golden/agrees-with-gcc.sh checks.
 */
// expect-exit: 42
#include <stdio.h>
static int dense(int x){ switch(x){
 case 0:return 100; case 1:return 101; case 2:return 102; case 3:return 103;
 case 4:return 104; case 5:return 105; case 6:return 106; case 7:return 107;
 case 8:return 108; case 9:return 109; case 10:return 110; case 11:return 111;
 default:return -1; } }
static int sparse(int x){ switch(x){
 case -1000:return 1; case -7:return 2; case 0:return 3; case 5:return 4;
 case 99:return 5; case 1000:return 6; case 70000:return 7; default:return 0; } }
static int uns(unsigned x){ switch(x){
 case 0u:return 1; case 1u:return 2; case 0x7fffffffu:return 3;
 case 0x80000000u:return 4; case 0xfffffffeu:return 5; case 0xffffffffu:return 6;
 default:return 0; } }
static int fall(int x){ int n=0; switch(x){ case 1: n+=1; case 2: n+=2;
 case 3: n+=4; break; case 4: n+=8; default: n+=16; } return n; }
static int wide(long x){ switch(x){ case -9223372036854775807L-1:return 1;
 case -1:return 2; case 0:return 3; case 9223372036854775807L:return 4;
 default:return 0; } }
int main(void){
 unsigned long h=0;
 for(int i=-5;i<20;i++) h=h*31+(unsigned)dense(i);
 for(long i=-1005;i<=1005;i+=7) h=h*31+(unsigned)sparse((int)i);
 h=h*31+(unsigned)sparse(70000);
 unsigned probes[]={0,1,2,0x7ffffffeu,0x7fffffffu,0x80000000u,0x80000001u,
                    0xfffffffdu,0xfffffffeu,0xffffffffu};
 for(unsigned i=0;i<10;i++) h=h*31+(unsigned)uns(probes[i]);
 for(int i=0;i<6;i++) h=h*31+(unsigned)fall(i);
 long ws[]={-9223372036854775807L-1,-2,-1,0,1,9223372036854775807L};
 for(int i=0;i<6;i++) h=h*31+(unsigned)wide(ws[i]);
 printf("%lu\n", h);
 return 42; }
