/* One eight-byte atomic per function, each with its own memory orders,
 * for tests/golden/atomic8.sh to compare with clang: the routine called,
 * and at RV32 the order arguments passed. */
typedef unsigned long long u64;
u64 d;
double df;
u64 ld_rlx(void) { return __atomic_load_n(&d, __ATOMIC_RELAXED); }
u64 ld_acq(void) { return __atomic_load_n(&d, __ATOMIC_ACQUIRE); }
u64 ld_sc(void) { return __atomic_load_n(&d, __ATOMIC_SEQ_CST); }
void st_rlx(u64 v) { __atomic_store_n(&d, v, __ATOMIC_RELAXED); }
void st_rel(u64 v) { __atomic_store_n(&d, v, __ATOMIC_RELEASE); }
u64 xc_ar(u64 v) { return __atomic_exchange_n(&d, v, __ATOMIC_ACQ_REL); }
int ce_s(u64 *e, u64 v) { return __atomic_compare_exchange_n(&d, e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE); }
int ce_w(u64 *e, u64 v) { return __atomic_compare_exchange_n(&d, e, v, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED); }
u64 fadd(u64 v) { return __atomic_fetch_add(&d, v, __ATOMIC_RELAXED); }
u64 fsub(u64 v) { return __atomic_fetch_sub(&d, v, __ATOMIC_ACQUIRE); }
u64 fand(u64 v) { return __atomic_fetch_and(&d, v, __ATOMIC_RELEASE); }
u64 f_or(u64 v) { return __atomic_fetch_or(&d, v, __ATOMIC_ACQ_REL); }
u64 fxor(u64 v) { return __atomic_fetch_xor(&d, v, __ATOMIC_SEQ_CST); }
u64 fnand(u64 v) { return __atomic_fetch_nand(&d, v, __ATOMIC_RELAXED); }
u64 addf(u64 v) { return __atomic_add_fetch(&d, v, __ATOMIC_ACQUIRE); }
u64 subf(u64 v) { return __atomic_sub_fetch(&d, v, __ATOMIC_SEQ_CST); }
u64 nandf(u64 v) { return __atomic_nand_fetch(&d, v, __ATOMIC_RELEASE); }
u64 s_add(u64 v) { return __sync_fetch_and_add(&d, v); }
u64 s_subf(u64 v) { return __sync_sub_and_fetch(&d, v); }
u64 s_val(u64 a, u64 b) { return __sync_val_compare_and_swap(&d, a, b); }
int s_bool(u64 a, u64 b) { return __sync_bool_compare_and_swap(&d, a, b); }
u64 s_tas(u64 v) { return __sync_lock_test_and_set(&d, v); }
void s_rel(void) { __sync_lock_release(&d); }
double g_ld(void) { double r; __atomic_load(&df, &r, __ATOMIC_ACQUIRE); return r; }
void g_st(double v) { __atomic_store(&df, &v, __ATOMIC_RELEASE); }
double g_xc(double v) { double r; __atomic_exchange(&df, &v, &r, __ATOMIC_SEQ_CST); return r; }
int g_ce(double *e, double v) { return __atomic_compare_exchange(&df, e, &v, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
