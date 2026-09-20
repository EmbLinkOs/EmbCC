/* A program here checks itself and exits 42, so that the shell asserts
 * one thing (the status) and every expectation lives beside the code it
 * is about. A failure prints its line and the expression, because "it
 * exited 1" from a hundred checks is not a diagnosis.
 */
#ifndef LIBCXX_STD_CHECK_H
#define LIBCXX_STD_CHECK_H
#include <cstdio>
static int __failures;
#define CHECK(e)                                                             \
    do {                                                                     \
        if (!(e)) {                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #e);         \
            __failures++;                                                    \
        }                                                                    \
    } while (0)
#define DONE() return __failures == 0 ? 42 : 1
#endif
