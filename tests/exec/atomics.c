/* The GCC atomic builtins — every one, on both targets, refereed by gcc.
 *
 * Single-threaded, so this checks the VALUE semantics of each builtin (what is
 * stored, what comes back, how a narrow or signed result is extended, how an
 * operand is converted), not atomicity itself; the retry loops and barriers
 * are verified by the encoding tests and by reading them. Each check is shaped
 * so that a wrong operator, a missing re-extension or an unconverted operand
 * changes the answer.
 */
// expect-exit: 42
#define SEQ __ATOMIC_SEQ_CST

static void *where(void) { return __builtin_return_address(0); }
static void *callers_frame(void) { return __builtin_frame_address(1); }

int main(void)
{
    int x = 10;
    if (__atomic_fetch_add(&x, 5, SEQ) != 10 || x != 15) return 1;
    if (__atomic_add_fetch(&x, 5, SEQ) != 20 || x != 20) return 2;
    if (__atomic_fetch_sub(&x, 3, SEQ) != 20 || x != 17) return 3;
    if (__atomic_sub_fetch(&x, 7, SEQ) != 10) return 4;
    x = 0xF0;
    if (__atomic_fetch_and(&x, 0x3C, SEQ) != 0xF0 || x != 0x30) return 5;
    if (__atomic_and_fetch(&x, 0x10, SEQ) != 0x10) return 6;
    if (__atomic_fetch_or(&x, 0x01, SEQ) != 0x10 || x != 0x11) return 7;
    if (__atomic_or_fetch(&x, 0x100, SEQ) != 0x111) return 8;
    if (__atomic_fetch_xor(&x, 0x11, SEQ) != 0x111 || x != 0x100) return 9;
    if (__atomic_xor_fetch(&x, 0x101, SEQ) != 0x001) return 10;
    x = 0x0F;
    if (__atomic_fetch_nand(&x, 0x3C, SEQ) != 0x0F || x != ~(0x0F & 0x3C)) return 11;
    if (__atomic_nand_fetch(&x, 0xFF, SEQ) != ~(~(0x0F & 0x3C) & 0xFF)) return 12;

    /* narrow: a signed result re-extends, an unsigned one truncates */
    signed char c = -1;
    if (__atomic_fetch_add(&c, 1, SEQ) != -1 || c != 0) return 13;
    if (__atomic_sub_fetch(&c, 1, SEQ) != -1 || c != -1) return 14;
    unsigned char uc = 255;
    if (__atomic_add_fetch(&uc, 1, SEQ) != 0 || uc != 0) return 15;
    short sh = -300;
    if (__atomic_exchange_n(&sh, 7, SEQ) != -300 || sh != 7) return 16;

    /* an int operand on a long object is converted: -1 adds -1 */
    long l = 100;
    if (__atomic_fetch_add(&l, -1, SEQ) != 100 || l != 99) return 17;
    if (__atomic_fetch_and(&l, -2, SEQ) != 99 || l != 98) return 18;

    /* the __sync family */
    int s = 5;
    if (__sync_fetch_and_add(&s, 2) != 5 || s != 7) return 19;
    if (__sync_or_and_fetch(&s, 8) != 15) return 20;
    if (__sync_fetch_and_nand(&s, 6) != 15 || s != ~(15 & 6)) return 21;
    s = 15;
    if (!__sync_bool_compare_and_swap(&s, 15, 40) || s != 40) return 22;
    if (__sync_bool_compare_and_swap(&s, 15, 50) || s != 40) return 23;
    if (__sync_val_compare_and_swap(&s, 40, 41) != 40 || s != 41) return 24;
    if (__sync_val_compare_and_swap(&s, 0, 1) != 41 || s != 41) return 25;
    if (__sync_lock_test_and_set(&s, 3) != 41 || s != 3) return 26;
    __sync_lock_release(&s);
    if (s != 0) return 27;
    signed char sc = -7;
    if (!__sync_bool_compare_and_swap(&sc, -7, -8) || sc != -8) return 28;

    /* compare-exchange, by value and by pointer; a miss writes back */
    long v = 5, exp = 5;
    if (!__atomic_compare_exchange_n(&v, &exp, 9, 0, SEQ, SEQ) || v != 9) return 29;
    exp = 5;
    if (__atomic_compare_exchange_n(&v, &exp, 1, 0, SEQ, SEQ) || exp != 9) return 30;
    long des = 12;
    exp = 9;
    if (!__atomic_compare_exchange(&v, &exp, &des, 1, SEQ, SEQ) || v != 12) return 31;

    /* the generic load / store / exchange */
    long got;
    __atomic_load(&v, &got, SEQ);
    if (got != 12) return 32;
    long nv = 33;
    __atomic_store(&v, &nv, SEQ);
    if (v != 33) return 33;
    long in = 44, ret;
    __atomic_exchange(&v, &in, &ret, SEQ);
    if (ret != 33 || v != 44) return 34;
    if (__atomic_load_n(&v, __ATOMIC_ACQUIRE) != 44) return 35;
    __atomic_store_n(&v, 45, __ATOMIC_RELEASE);
    if (v != 45) return 36;

    /* test-and-set / clear work on one byte */
    _Bool flag = 0;
    if (__atomic_test_and_set(&flag, SEQ) != 0) return 37;
    if (__atomic_test_and_set(&flag, SEQ) != 1) return 38;
    __atomic_clear(&flag, SEQ);
    if (flag) return 39;

    /* fences, and the lock-free queries — the latter a constant */
    __atomic_thread_fence(SEQ);
    __atomic_signal_fence(SEQ);
    __sync_synchronize();
    _Static_assert(__atomic_always_lock_free(sizeof(long), 0), "lock-free");
    if (!__atomic_is_lock_free(4, 0) || __atomic_always_lock_free(3, 0)) return 40;

    /* a pointer object: the operand is BYTES, not elements, as gcc says */
    int arr[4];
    int *p = arr;
    if (__atomic_fetch_add(&p, sizeof(int), SEQ) != arr || p != arr + 1) return 41;

    /* the frame chain: a callee's level-1 frame is its caller's level 0,
     * and a return address lands inside the caller */
    if (callers_frame() != __builtin_frame_address(0)) return 43;
    char *ra = where();
    if (ra <= (char *)main || ra > (char *)main + 65536) return 44;
    return 42;
}
