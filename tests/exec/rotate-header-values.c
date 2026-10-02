/* A loop rotated to test at the bottom when the value its test computes
 * is also read by the body or after the loop: `while (*s) h ^= *s++;`
 * once value numbering has given the body the header's load. The copy
 * at the bottom writes that value under its own name, so the body and
 * the exit see this iteration's value whether they came from the guard
 * or the copy. Zero, one and many iterations; the value read after the
 * loop on both ways out; a constant bound shared with the body. */
// expect-exit: 42
typedef unsigned u32;
static volatile int vz;

__attribute__((noinline)) u32 fnv(const char *s)
{ u32 h = 2166136261u; while (*s) { h ^= (unsigned char)*s; h *= 16777619u; s++; } return h; }

/* the header's value read after the loop, on both exits */
__attribute__((noinline)) int scan(const char *p, int *last)
{
    int c, n = 0;
    while ((c = *p) != 0 && c != ',') { n += c; p++; }
    *last = c;                      /* 0 or ',' */
    return n;
}

__attribute__((noinline)) int find(const int *a, int n, int key, int *at)
{
    int i = 0, v = 0;
    while (i < n && (v = a[i]) != key) i++;
    *at = i;
    return v;                       /* the last a[i] read, or 0 */
}

__attribute__((noinline)) u32 bounded(const int *a)
{ u32 s = 0; for (int j = 0; j < 24; j++) s += (u32)a[j] ^ (u32)(j * 24); return s; }

static u32 ref_fnv(const char *s)
{ u32 h = 2166136261u; for (int i = 0; s[i]; i++) { vz = s[i]; h ^= (unsigned char)vz; h *= 16777619u; } return h; }

int main(void)
{
    int bad = 0, last;
    const char *strs[] = { "", "a", "ab", "hello, world", ",", "xyz,", "longer string without comma" };
    for (int k = 0; k < 7; k++) {
        if (fnv(strs[k]) != ref_fnv(strs[k])) bad |= 1;
        int want = 0, i = 0;
        while (strs[k][i] && strs[k][i] != ',') want += strs[k][i++];
        if (scan(strs[k], &last) != want || last != strs[k][i]) bad |= 2;
    }
    int a[24];
    for (int i = 0; i < 24; i++) a[i] = i * 7 - 30;
    int at, v;
    v = find(a, 24, a[5], &at);
    if (at != 5 || v != a[5]) bad |= 4;
    v = find(a, 24, 1000, &at);
    if (at != 24 || v != a[23]) bad |= 8;
    v = find(a, 0, 5, &at);
    if (at != 0 || v != 0) bad |= 16;
    u32 s = 0;
    for (int j = 0; j < 24; j++) { vz = a[j]; s += (u32)vz ^ (u32)(j * 24); }
    if (bounded(a) != s) bad |= 32;
    return bad ? 100 + bad : 42;
}
