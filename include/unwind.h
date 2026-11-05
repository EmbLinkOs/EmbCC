/* EmbCC's unwind.h: the Itanium C++ ABI's base unwinder interface (level
 * I, "Exception Handling ABI" 1.6), with the extensions GCC's libgcc
 * provides and libstdc++'s libsupc++ uses (_Unwind_GetIPInfo, the
 * relative bases, _Unwind_Resume_or_Rethrow ...). The unwinder itself is
 * libgcc's (libgcc_eh.a); this only declares it — with its layouts, so
 * code EmbCC compiles meets it: x86-64 and aarch64 (LP64, DWARF CFI). */
#ifndef _UNWIND_H
#define _UNWIND_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long _Unwind_Word;
typedef long _Unwind_Sword;
typedef unsigned long _Unwind_Ptr;
typedef unsigned long _Unwind_Internal_Ptr;
/* eight bytes everywhere, as libgcc's: "GNUCC++\0" does not fit a
 * 32-bit unsigned long */
typedef unsigned long long _Unwind_Exception_Class;
/* LEB128's decoded values (libgcc's unwind-pe.h reads them) */
typedef unsigned long _uleb128_t;
typedef long _sleb128_t;

typedef enum {
    _URC_NO_REASON = 0,
    _URC_OK = 0,
    _URC_FOREIGN_EXCEPTION_CAUGHT = 1,
    _URC_FATAL_PHASE2_ERROR = 2,
    _URC_FATAL_PHASE1_ERROR = 3,
    _URC_NORMAL_STOP = 4,
    _URC_END_OF_STACK = 5,
    _URC_HANDLER_FOUND = 6,
    _URC_INSTALL_CONTEXT = 7,
    _URC_CONTINUE_UNWIND = 8,
    _URC_FAILURE = 9                 /* EHABI's */
} _Unwind_Reason_Code;

#if defined(__ARM_EABI__) && !defined(__ARM_DWARF_EH__)
/* ---- 32-bit ARM: the Exception Handling ABI for the ARM Architecture
 * (EHABI), as lib/rt/unwind_arm.c implements it and GCC's libgcc does.
 * The exception header is the unwinder control block; a personality
 * routine is given a state, and unwinds its frame itself
 * (__gnu_unwind_frame) when it does not stop there. */
#define __EMBCC_ARM_EHABI 1
typedef int _Unwind_State;
#define _US_VIRTUAL_UNWIND_FRAME  0
#define _US_UNWIND_FRAME_STARTING 1
#define _US_UNWIND_FRAME_RESUME   2
#define _US_ACTION_MASK           3
#define _US_FORCE_UNWIND          8
#define _US_END_OF_STACK          16
typedef unsigned _uw;
struct _Unwind_Control_Block;
typedef struct _Unwind_Control_Block _Unwind_Control_Block;
struct _Unwind_Control_Block {
    char exception_class[8];
    void (*exception_cleanup)(_Unwind_Reason_Code, _Unwind_Control_Block *);
    struct { _uw reserved1, reserved2, reserved3, reserved4, reserved5; }
        unwinder_cache;
    struct { _uw sp; _uw bitpattern[5]; } barrier_cache;
    struct { _uw bitpattern[4]; } cleanup_cache;
    struct { _uw fnstart; _uw *ehtp; _uw additional; _uw reserved1; }
        pr_cache;
    long long int : 0;
} __attribute__((__aligned__(8)));
#define _Unwind_Exception _Unwind_Control_Block
typedef void (*_Unwind_Exception_Cleanup_Fn)(_Unwind_Reason_Code,
                                             _Unwind_Control_Block *);
typedef enum { _UVRSC_CORE = 0, _UVRSC_VFP = 1 } _Unwind_VRS_RegClass;
typedef enum { _UVRSD_UINT32 = 0, _UVRSD_VFPX = 1, _UVRSD_DOUBLE = 5 }
    _Unwind_VRS_DataRepresentation;
typedef enum { _UVRSR_OK = 0, _UVRSR_NOT_IMPLEMENTED = 1,
               _UVRSR_FAILED = 2 } _Unwind_VRS_Result;
struct _Unwind_Context;
typedef struct _Unwind_Context _Unwind_Context;
_Unwind_VRS_Result _Unwind_VRS_Get(_Unwind_Context *, _Unwind_VRS_RegClass,
                                   _uw, _Unwind_VRS_DataRepresentation,
                                   void *);
_Unwind_VRS_Result _Unwind_VRS_Set(_Unwind_Context *, _Unwind_VRS_RegClass,
                                   _uw, _Unwind_VRS_DataRepresentation,
                                   void *);
_Unwind_VRS_Result _Unwind_VRS_Pop(_Unwind_Context *, _Unwind_VRS_RegClass,
                                   _uw, _Unwind_VRS_DataRepresentation);
_Unwind_Reason_Code __gnu_unwind_frame(_Unwind_Control_Block *,
                                       _Unwind_Context *);
void _Unwind_Complete(_Unwind_Control_Block *);
#else
struct _Unwind_Exception;

typedef void (*_Unwind_Exception_Cleanup_Fn)(_Unwind_Reason_Code,
                                             struct _Unwind_Exception *);

/* the language-independent header of an exception object: as aligned as
 * anything (16 bytes here), as libgcc lays it out */
struct _Unwind_Exception {
    _Unwind_Exception_Class exception_class;
    _Unwind_Exception_Cleanup_Fn exception_cleanup;
    _Unwind_Word private_1;
    _Unwind_Word private_2;
} __attribute__((__aligned__));
#endif

typedef int _Unwind_Action;

#define _UA_SEARCH_PHASE    1
#define _UA_CLEANUP_PHASE   2
#define _UA_HANDLER_FRAME   4
#define _UA_FORCE_UNWIND    8
#define _UA_END_OF_STACK    16

struct _Unwind_Context;

typedef _Unwind_Reason_Code (*_Unwind_Stop_Fn)(int, _Unwind_Action,
                                               _Unwind_Exception_Class,
                                               struct _Unwind_Exception *,
                                               struct _Unwind_Context *,
                                               void *);
typedef _Unwind_Reason_Code (*_Unwind_Trace_Fn)(struct _Unwind_Context *,
                                                void *);
typedef _Unwind_Reason_Code (*_Unwind_Personality_Fn)(
    int, _Unwind_Action, _Unwind_Exception_Class,
    struct _Unwind_Exception *, struct _Unwind_Context *);

_Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception *);
_Unwind_Reason_Code _Unwind_ForcedUnwind(struct _Unwind_Exception *,
                                         _Unwind_Stop_Fn, void *);
void _Unwind_DeleteException(struct _Unwind_Exception *);
void _Unwind_Resume(struct _Unwind_Exception *);
_Unwind_Reason_Code _Unwind_Resume_or_Rethrow(struct _Unwind_Exception *);
_Unwind_Reason_Code _Unwind_Backtrace(_Unwind_Trace_Fn, void *);

_Unwind_Word _Unwind_GetGR(struct _Unwind_Context *, int);
void _Unwind_SetGR(struct _Unwind_Context *, int, _Unwind_Word);
_Unwind_Ptr _Unwind_GetIP(struct _Unwind_Context *);
_Unwind_Ptr _Unwind_GetIPInfo(struct _Unwind_Context *, int *);
void _Unwind_SetIP(struct _Unwind_Context *, _Unwind_Ptr);
_Unwind_Word _Unwind_GetCFA(struct _Unwind_Context *);
void *_Unwind_GetLanguageSpecificData(struct _Unwind_Context *);
_Unwind_Ptr _Unwind_GetRegionStart(struct _Unwind_Context *);
_Unwind_Ptr _Unwind_GetDataRelBase(struct _Unwind_Context *);
_Unwind_Ptr _Unwind_GetTextRelBase(struct _Unwind_Context *);
void *_Unwind_FindEnclosingFunction(void *);

#ifdef __cplusplus
}
#endif

#endif /* _UNWIND_H */
