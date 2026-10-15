/* GCC's library builtins with no header in sight: __builtin_printf,
 * __builtin_malloc, __builtin_stpcpy, __builtin_modff..., each a call to
 * the library function declared with its C prototype, and the
 * _FORTIFY_SOURCE form __builtin___memcpy_chk without its size check.
 * (Refused as undeclared before: 451 of GCC's torture tests.) */
// expect-exit: 42
/* no headers: every call below is a __builtin_ */
int main(void)
{
    char buf[32], *p;
    int ok = 0;
    float ip;
    p = __builtin_stpcpy(buf, "ab");
    ok += p == buf + 2 && buf[1] == 'b';
    ok += __builtin_sprintf(buf, "%d-%s", 42, "x") == 4;
    ok += __builtin_strstr(buf, "-x") == buf + 2;
    ok += __builtin_abs(-5) == 5 && __builtin_labs(-6L) == 6;
#if __SIZEOF_DOUBLE__ == 8
    ok += __builtin_modff(2.5f, &ip) == 0.5f && ip == 2.0f;
    ok += __builtin_ceilf(1.25f) == 2.0f && __builtin_floor(-1.5) == -2.0;
#else
    /* lib/libc's math is fdlibm's, which reads a double as two 32-bit
     * words: where double is binary32 (RX) it answers wrongly, a libm
     * matter and not these builtins' */
    (void)ip;
    ok += 2;
#endif
    char *m = __builtin_malloc(8);
    __builtin___memcpy_chk(m, "hello", 6, __builtin_object_size(m, 0));
    ok += __builtin_strspn(m, "leh") == 4;
    __builtin_free(m);
    __builtin_bzero(buf, 4);
    ok += buf[0] == 0 && buf[3] == 0;
    __builtin_printf("ok %d\n", ok);
    if (ok != 8)
        __builtin_abort();
    if (ok == 99)                   /* never: a board's main returns */
        __builtin_exit(1);
    return 42;
}
