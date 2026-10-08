/* One atomic per function, at every size, for tests/golden/avr-atomics.sh
 * to read off the disassembly: each must touch the object (through Z)
 * only between `in r0, SREG; cli` and `out SREG, r0`, call nothing and
 * never `sei`. */
#define OPS(n, T)                                                              \
  T n##_add(T *p, T v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }  \
  T n##_sub(T *p, T v) { return __atomic_sub_fetch(p, v, __ATOMIC_RELAXED); }  \
  T n##_and(T *p, T v) { return __atomic_fetch_and(p, v, __ATOMIC_ACQUIRE); }  \
  T n##_or(T *p, T v) { return __atomic_fetch_or(p, v, __ATOMIC_RELEASE); }    \
  T n##_xor(T *p, T v) { return __atomic_fetch_xor(p, v, __ATOMIC_ACQ_REL); }  \
  T n##_nand(T *p, T v) { return __atomic_fetch_nand(p, v, __ATOMIC_SEQ_CST); }\
  T n##_xchg(T *p, T v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }\
  int n##_cas(T *p, T *e, T v)                                                 \
  { return __atomic_compare_exchange_n(p, e, v, 0, __ATOMIC_SEQ_CST,           \
                                       __ATOMIC_SEQ_CST); }                    \
  T n##_vcas(T *p, T e, T v) { return __sync_val_compare_and_swap(p, e, v); }

OPS(b, unsigned char)
OPS(h, unsigned short)
OPS(w, unsigned long)
OPS(d, unsigned long long)

#define LS(n, T)                                                               \
  T n##_load(T *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }            \
  void n##_store(T *p, T v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

LS(h, unsigned short)
LS(w, unsigned long)
LS(d, unsigned long long)

_Bool tas(unsigned char *p) { return __atomic_test_and_set(p, __ATOMIC_SEQ_CST); }
void inc(_Atomic long long *p) { ++*p; }
void mul(_Atomic short *p) { *p *= 3; }
