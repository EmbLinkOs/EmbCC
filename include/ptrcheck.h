/* <ptrcheck.h>: clang's -fbounds-safety annotations, as EmbCC reads them.
 *
 * __counted_by(N) and __sized_by(N) (and their _or_null forms) name the
 * member of the same struct that holds the length of a flexible array
 * member or of a pointer member: under -fbounds-safety every subscript of
 * it is checked against that member, read when the subscript runs, and
 * traps when out of bounds (EmbCC's checks trap). The rest of clang's
 * vocabulary -- __single, __indexable, __bidi_indexable, __ended_by,
 * __terminated_by and the ABI pragmas -- is accepted and means nothing
 * here, so a header written for clang compiles: EmbCC's pointers carry no
 * bounds of their own, and an unannotated pointer is not checked. */
#ifndef __EMBCC_PTRCHECK_H
#define __EMBCC_PTRCHECK_H

#define __counted_by(N) __attribute__((counted_by(N)))
#define __counted_by_or_null(N) __attribute__((counted_by_or_null(N)))
#define __sized_by(N) __attribute__((sized_by(N)))
#define __sized_by_or_null(N) __attribute__((sized_by_or_null(N)))
#define __ended_by(P)
#define __single
#define __indexable
#define __bidi_indexable
#define __unsafe_indexable
#define __null_terminated
#define __terminated_by(T)
#define __unsafe_forge_bidi_indexable(T, P, S) ((T)(P))
#define __unsafe_forge_single(T, P) ((T)(P))
#define __unsafe_terminated_by_from_indexable(T, P, ...) (P)
#define __ptrcheck_abi_assume_single()
#define __ptrcheck_abi_assume_indexable()
#define __ptrcheck_abi_assume_bidi_indexable()
#define __ptrcheck_abi_assume_unsafe_indexable()

#endif
