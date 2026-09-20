/* Compile-time `long double`: exact constant arithmetic in the TARGET's
 * format, which the host cannot do — x86-64's is x87 80-bit extended (a
 * 64-bit significand), aarch64's IEEE binary128 (113 bits), and the host's
 * own long double may be neither (on arm64 macOS it is plain double).
 *
 * A value is finite (sign * m * 2^e, m an arbitrary-precision integer),
 * infinite, or NaN. Every operation is computed EXACTLY and then rounded
 * once, round-to-nearest-even, to the format's precision and exponent
 * range — IEEE semantics per operation, which is how gcc folds too. A
 * literal is converted the same way from its exact decimal (or hex) value,
 * never through a double, so `0.1L` gets all 64 or 113 bits right.
 *
 * Used for long double literals (their byte image in .rodata) and for
 * folding static initializers.
 */
#ifndef EMBCC_SEMA_LDFLOAT_H
#define EMBCC_SEMA_LDFLOAT_H

/* The target layouts. LDF_DOUBLE rounds to IEEE double (a long double
 * constant converted to double in an initializer). */
enum ldf_fmt { LDF_X87, LDF_QUAD, LDF_DOUBLE, LDF_FLOAT };

struct ldf;   /* opaque, heap-allocated, never freed (compile-time only) */

/* The target's long double format (x87 on x86-64, binary128 on aarch64). */
enum ldf_fmt ldf_target_fmt(void);

/* A decimal or hex floating literal WITHOUT its suffix ("1.5e3", "0x1p-3"),
 * rounded to fmt. Returns NULL if the text is not a floating constant. */
struct ldf *ldf_from_text(const char *text, enum ldf_fmt fmt);
struct ldf *ldf_from_double(double d);            /* exact */
struct ldf *ldf_from_int(long v, int is_unsigned); /* exact, then rounded by use */

/* op is '+', '-', '*' or '/'; the exact result rounded to fmt. */
struct ldf *ldf_binop(int op, const struct ldf *a, const struct ldf *b,
                      enum ldf_fmt fmt);
struct ldf *ldf_neg(const struct ldf *a);
struct ldf *ldf_round(const struct ldf *a, enum ldf_fmt fmt);

/* The value's bytes in fmt, little-endian: 16 for LDF_X87 (10 significant,
 * 6 of zero padding) and LDF_QUAD, 8 for LDF_DOUBLE, 4 for LDF_FLOAT. */
void ldf_encode(const struct ldf *a, enum ldf_fmt fmt, unsigned char *out);

/* The value converted to double / truncated toward zero to an integer
 * (C's conversion; out-of-range is undefined in C, saturated here). */
double ldf_to_double(const struct ldf *a);
long ldf_to_long(const struct ldf *a);

/* IEEE comparison: -1, 0, 1, or LDF_UNORDERED when either is NaN.
 *
 * It exists because comparing through ldf_to_double() is not the same
 * question: two distinct long doubles can round to the same double, and
 * `a < b` would then answer false for values that differ. Constant
 * evaluation has to give the answer the target would give at run time. */
#define LDF_UNORDERED 2
int ldf_cmp(const struct ldf *a, const struct ldf *b);

/* Is this value zero (of either sign)? */
int ldf_is_zero(const struct ldf *a);

/* A value read back out of memory: the exact inverse of ldf_encode, for
 * the same format. Nothing is rounded -- every bit pattern in a format
 * denotes a value it holds exactly. */
struct ldf *ldf_from_bytes(const unsigned char *in, enum ldf_fmt fmt);

#endif
