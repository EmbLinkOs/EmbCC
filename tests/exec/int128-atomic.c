/* The GCC atomic builtins on an __int128: EmbCC does them inline with a
 * 16-byte compare-and-swap (x86-64's lock cmpxchg16b, aarch64's exclusive
 * pair) where gcc calls libatomic. Each form's result and the object's
 * value after it: load/store (_n and generic), exchange, the fetch-op and
 * op-fetch families (add, sub, and, or, xor, nand), compare-exchange that
 * succeeds and one that fails (expected rewritten), the __sync forms. */
// expect-exit: 42
// no-gcc-reference: gcc calls libatomic's __atomic_*_16, the toolchains have none
typedef __int128 i128;
typedef unsigned __int128 u128;

#define W(hi, lo) ((u128)(hi) << 64 | (u128)(lo))

static _Alignas(16) u128 obj;
static _Alignas(16) i128 sobj;

int main(void)
{
    u128 a = W(0x0123456789abcdefUL, 0xfedcba9876543210UL);
    u128 b = W(0xffffffffffffffffUL, 0x0000000000000001UL);
    __atomic_store_n(&obj, a, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&obj, __ATOMIC_SEQ_CST) != a || obj != a)
        return 1;
    u128 out, in = b;
    __atomic_store(&obj, &in, __ATOMIC_RELEASE);
    __atomic_load(&obj, &out, __ATOMIC_ACQUIRE);
    if (out != b)
        return 2;
    if (__atomic_exchange_n(&obj, a, __ATOMIC_SEQ_CST) != b || obj != a)
        return 3;
    in = b;
    __atomic_exchange(&obj, &in, &out, __ATOMIC_SEQ_CST);
    if (out != a || obj != b)
        return 4;
    /* b + 0xffff...ffff (low half): the carry crosses the halves */
    if (__atomic_fetch_add(&obj, (u128)~0UL, __ATOMIC_SEQ_CST) != b ||
        obj != W(0, 0))
        return 5;
    if (__atomic_sub_fetch(&obj, 1, __ATOMIC_SEQ_CST) != W(~0UL, ~0UL))
        return 6;
    obj = a;
    if (__atomic_and_fetch(&obj, W(0xff00ff00ff00ff00UL, 0x0f0f0f0f0f0f0f0fUL),
                           __ATOMIC_SEQ_CST) !=
        W(0x010045008900cd00UL, 0x0e0c0a0806040200UL))
        return 7;
    if (__atomic_fetch_or(&obj, W(1UL << 63, 0), __ATOMIC_SEQ_CST) !=
            W(0x010045008900cd00UL, 0x0e0c0a0806040200UL) ||
        obj != W(0x810045008900cd00UL, 0x0e0c0a0806040200UL))
        return 8;
    if (__atomic_xor_fetch(&obj, obj, __ATOMIC_SEQ_CST) != 0)
        return 9;
    if (__atomic_nand_fetch(&obj, a, __ATOMIC_SEQ_CST) != W(~0UL, ~0UL))
        return 10;
    /* compare-exchange: a hit swaps; a miss swaps nothing and says what
     * it saw */
    u128 exp = W(~0UL, ~0UL);
    if (!__atomic_compare_exchange_n(&obj, &exp, a, 0, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST) || obj != a ||
        exp != W(~0UL, ~0UL))
        return 11;
    exp = b;
    if (__atomic_compare_exchange_n(&obj, &exp, 7, 1, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST) || obj != a || exp != a)
        return 12;
    u128 des = b;
    exp = a;
    if (!__atomic_compare_exchange(&obj, &exp, &des, 0, __ATOMIC_SEQ_CST,
                                   __ATOMIC_SEQ_CST) || obj != b)
        return 13;
    /* __sync: signed, with the value converted from a narrower one */
    sobj = -5;
    if (__sync_fetch_and_add(&sobj, 3) != -5 || sobj != -2)
        return 14;
    if (__sync_val_compare_and_swap(&sobj, -2, (i128)1 << 100) != -2 ||
        sobj != (i128)1 << 100)
        return 15;
    if (__sync_bool_compare_and_swap(&sobj, 0, 1) || sobj != (i128)1 << 100)
        return 16;
    if (__sync_lock_test_and_set(&sobj, -1) != (i128)1 << 100 || sobj != -1)
        return 17;
    __sync_lock_release(&sobj);
    if (sobj != 0 || __sync_sub_and_fetch(&sobj, 1) != -1)
        return 18;
    return 42;
}
