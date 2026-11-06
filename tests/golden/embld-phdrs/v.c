int g = 5; int b[100]; const int ro[4] = {1,2,3,4};
__thread int t1 = 7; __thread int t2;
__attribute__((section(".note.x"))) const int note[4] = {4, 4, 1, 0};
int _start(void){ return g + b[3] + ro[1] + t1 + t2 + note[0]; }
