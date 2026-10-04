/* x86-64 with unwind tables: every function has an FDE in .eh_frame,
 * and one function is collected. */
int used_leaf(int x) { return x * 3; }
int unused_leaf(int x) { return x * 5 + 1; }
int _start(void) { return used_leaf(4); }
