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
typedef unsigned long _Unwind_Exception_Class;
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
    _URC_CONTINUE_UNWIND = 8
} _Unwind_Reason_Code;

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
