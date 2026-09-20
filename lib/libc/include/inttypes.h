/* <inttypes.h> — C11 §7.8.
 *
 * The format macros are spelled out for this library's actual type
 * choices (stdint.h: int64_t is long, intmax_t is long), so PRId64 is
 * "ld" and not "lld". A program that prints an int64_t with PRId64 is
 * correct; one that prints it with %lld is relying on the two being the
 * same type, which they are not.
 */
#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  "ld"
#define PRIdMAX "ld"
#define PRIdPTR "ld"
#define PRIi8   "i"
#define PRIi16  "i"
#define PRIi32  "i"
#define PRIi64  "li"
#define PRIiMAX "li"
#define PRIiPTR "li"
#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  "lu"
#define PRIuMAX "lu"
#define PRIuPTR "lu"
#define PRIo8   "o"
#define PRIo16  "o"
#define PRIo32  "o"
#define PRIo64  "lo"
#define PRIoMAX "lo"
#define PRIoPTR "lo"
#define PRIx8   "x"
#define PRIx16  "x"
#define PRIx32  "x"
#define PRIx64  "lx"
#define PRIxMAX "lx"
#define PRIxPTR "lx"
#define PRIX8   "X"
#define PRIX16  "X"
#define PRIX32  "X"
#define PRIX64  "lX"
#define PRIXMAX "lX"
#define PRIXPTR "lX"

/* The least/fast families are the same types here (stdint.h aliases
 * them), so the macros are the same strings. */
#define PRIdLEAST8   PRId8
#define PRIdLEAST16  PRId16
#define PRIdLEAST32  PRId32
#define PRIdLEAST64  PRId64
#define PRIuLEAST8   PRIu8
#define PRIuLEAST16  PRIu16
#define PRIuLEAST32  PRIu32
#define PRIuLEAST64  PRIu64
#define PRIxLEAST8   PRIx8
#define PRIxLEAST16  PRIx16
#define PRIxLEAST32  PRIx32
#define PRIxLEAST64  PRIx64
#define PRIdFAST8    PRId8
#define PRIdFAST16   PRId16
#define PRIdFAST32   PRId32
#define PRIdFAST64   PRId64
#define PRIuFAST8    PRIu8
#define PRIuFAST16   PRIu16
#define PRIuFAST32   PRIu32
#define PRIuFAST64   PRIu64
#define PRIxFAST8    PRIx8
#define PRIxFAST16   PRIx16
#define PRIxFAST32   PRIx32
#define PRIxFAST64   PRIx64

#define SCNd8   "hhd"
#define SCNd16  "hd"
#define SCNd32  "d"
#define SCNd64  "ld"
#define SCNdMAX "ld"
#define SCNdPTR "ld"
#define SCNi8   "hhi"
#define SCNi16  "hi"
#define SCNi32  "i"
#define SCNi64  "li"
#define SCNiMAX "li"
#define SCNiPTR "li"
#define SCNu8   "hhu"
#define SCNu16  "hu"
#define SCNu32  "u"
#define SCNu64  "lu"
#define SCNuMAX "lu"
#define SCNuPTR "lu"
#define SCNo8   "hho"
#define SCNo16  "ho"
#define SCNo32  "o"
#define SCNo64  "lo"
#define SCNoMAX "lo"
#define SCNoPTR "lo"
#define SCNx8   "hhx"
#define SCNx16  "hx"
#define SCNx32  "x"
#define SCNx64  "lx"
#define SCNxMAX "lx"
#define SCNxPTR "lx"

typedef struct { intmax_t quot; intmax_t rem; } imaxdiv_t;

intmax_t  imaxabs(intmax_t j);
imaxdiv_t imaxdiv(intmax_t num, intmax_t den);
intmax_t  strtoimax(const char *__restrict s, char **__restrict end, int base);
uintmax_t strtoumax(const char *__restrict s, char **__restrict end, int base);

#ifdef __cplusplus
}
#endif

#endif
