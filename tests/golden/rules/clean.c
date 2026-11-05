/* MISRA-clean code: --misra must say nothing about it */
#include <stdint.h>
static uint32_t total(const uint32_t *v, uint32_t n)
{
    uint32_t s = 0U;
    for (uint32_t i = 0U; i < n; i++) {
        s += v[i];
    }
    return s;
}
uint32_t use(void)
{
    static const uint32_t data[4] = { 1U, 2U, 3U, 4U };
    uint32_t r = total(data, 4U);
    if (r > 5U) {
        r = 5U;
    } else {
        r = 0U;
    }
    return r;
}
