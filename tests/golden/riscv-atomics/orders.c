/* Every read-modify-write at every width and every memory order, for
 * tests/golden/riscv-atomics.sh to compare with clang's: the atomic
 * instructions of each function, aq and rl bits included, must be the
 * ones clang emits. */
#define OPS(name, T, obj, ord)                                                 \
  T name##_add(T v) { return __atomic_fetch_add(&obj, v, ord); }               \
  T name##_sub(T v) { return __atomic_sub_fetch(&obj, v, ord); }               \
  T name##_and(T v) { return __atomic_fetch_and(&obj, v, ord); }               \
  T name##_or(T v) { return __atomic_fetch_or(&obj, v, ord); }                 \
  T name##_xor(T v) { return __atomic_fetch_xor(&obj, v, ord); }               \
  T name##_nand(T v) { return __atomic_fetch_nand(&obj, v, ord); }             \
  T name##_xchg(T v) { return __atomic_exchange_n(&obj, v, ord); }             \
  int name##_cas(T *e, T v)                                                    \
  { return __atomic_compare_exchange_n(&obj, e, v, 0, ord, __ATOMIC_RELAXED); }\
  int name##_casw(T *e, T v)                                                   \
  { return __atomic_compare_exchange_n(&obj, e, v, 1, ord, __ATOMIC_RELAXED); }

#define ORDERS(p, T, obj)                       \
  OPS(p##_rlx, T, obj, __ATOMIC_RELAXED)        \
  OPS(p##_con, T, obj, __ATOMIC_CONSUME)        \
  OPS(p##_acq, T, obj, __ATOMIC_ACQUIRE)        \
  OPS(p##_rel, T, obj, __ATOMIC_RELEASE)        \
  OPS(p##_ar, T, obj, __ATOMIC_ACQ_REL)         \
  OPS(p##_sc, T, obj, __ATOMIC_SEQ_CST)

unsigned char b;
unsigned short h;
unsigned int w;
signed char sb;
ORDERS(b, unsigned char, b)
ORDERS(h, unsigned short, h)
ORDERS(w, unsigned int, w)
ORDERS(sb, signed char, sb)
#if __riscv_xlen == 64
unsigned long d;
ORDERS(d, unsigned long, d)
#endif

/* a compare-exchange's failure order strengthens its success order */
int fail_acq(unsigned short *e, unsigned short v)
{ return __atomic_compare_exchange_n(&h, e, v, 0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE); }
int fail_sc(unsigned char *e, unsigned char v)
{ return __atomic_compare_exchange_n(&b, e, v, 0, __ATOMIC_ACQ_REL, __ATOMIC_SEQ_CST); }
int fail_rlx_acq(unsigned int *e, unsigned int v)
{ return __atomic_compare_exchange_n(&w, e, v, 0, __ATOMIC_RELAXED, __ATOMIC_ACQUIRE); }

/* the generic forms, by pointer */
void gx(unsigned short *v, unsigned short *r) { __atomic_exchange(&h, v, r, __ATOMIC_ACQUIRE); }
int gc(unsigned char *e, unsigned char *v)
{ return __atomic_compare_exchange(&b, e, v, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED); }

/* the __sync forms: seq_cst, but lock_test_and_set acquires */
unsigned char s_add(unsigned char v) { return __sync_fetch_and_add(&b, v); }
unsigned short s_nand(unsigned short v) { return __sync_nand_and_fetch(&h, v); }
unsigned char s_val(unsigned char o, unsigned char n) { return __sync_val_compare_and_swap(&b, o, n); }
int s_bool(unsigned short o, unsigned short n) { return __sync_bool_compare_and_swap(&h, o, n); }
unsigned char s_tas(void) { return __sync_lock_test_and_set(&b, 1); }
unsigned int s_tasw(void) { return __sync_lock_test_and_set(&w, 1); }

/* atomic_flag, by the builtins <stdatomic.h> uses */
_Bool tas_sc(void) { return __atomic_test_and_set(&b, __ATOMIC_SEQ_CST); }
_Bool tas_acq(void) { return __atomic_test_and_set(&b, __ATOMIC_ACQUIRE); }
_Bool tas_rlx(void) { return __atomic_test_and_set(&b, __ATOMIC_RELAXED); }

/* the operators on an _Atomic object: seq_cst */
_Atomic unsigned char ab;
_Atomic short ah;
void op_inc(void) { ab += 3; }
void op_or(void) { ah |= 0x40; }
int op_post(void) { return ah--; }
