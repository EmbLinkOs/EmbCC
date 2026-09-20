/* Exceptions: the __cxa_* layer and the personality routine.
 * Itanium C++ ABI §2.2 (the exception object) and §2.5 (unwinding).
 *
 * The division of labour is worth stating, because it is the reason this
 * file is a few hundred lines and not a few thousand. Walking the stack --
 * decoding .eh_frame, restoring callee-saved registers frame by frame,
 * transferring control -- is the UNWINDER, and it is libgcc's
 * (`unwind.h` declares it; `-lgcc` provides it). It is language-neutral
 * and every language on the platform shares it.
 *
 * What is not neutral is the POLICY: given a frame, does this exception
 * match one of its catch clauses? That question is about C++ types, and
 * the unwinder answers it by calling back into the personality routine
 * below. So: libgcc owns the machine, we own the meaning.
 *
 * The unwinder makes TWO passes, and conflating them is the classic bug.
 * Pass one only asks "is there a handler anywhere above?" and changes
 * nothing. Only when one is found does pass two run, destroying each frame
 * on the way. That is what makes an uncaught exception able to call
 * terminate() with the throwing frame still intact and inspectable, rather
 * than after the stack has already been destroyed.
 */
#include "abi.h"

#include <exception>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <unwind.h>

namespace __cxxabiv1 {

/* "GNUCC++\0" -- the ABI's identifier for a C++ exception. A personality
 * routine that sees a different class is looking at another language's
 * exception passing through: it may run cleanups, but it must not try to
 * read a type_info out of it. */
static const _Unwind_Exception_Class kCxxClass = 0x474e5543432b2b00UL;

struct __cxa_exception {
    const std::type_info *exceptionType;
    void (*exceptionDestructor)(void *);
    std::terminate_handler unexpectedHandler;
    std::terminate_handler terminateHandler;
    __cxa_exception *nextException;

    int handlerCount;
    int handlerSwitchValue;
    const unsigned char *actionRecord;
    const unsigned char *languageSpecificData;
    void *catchTemp;
    void *adjustedPtr;

    /* Last, because the unwinder is handed the address of THIS member and
     * steps back to the header by subtraction. Anything after it would be
     * invisible to that arithmetic. */
    _Unwind_Exception unwindHeader;
};

struct __cxa_eh_globals {
    __cxa_exception *caughtExceptions;
    unsigned int uncaughtExceptions;
};

/* Single-threaded, like the rest of this runtime. When threads arrive this
 * becomes thread-local and nothing else changes -- which is why the ABI
 * makes every access go through a function. */
static __cxa_eh_globals g_globals;

extern "C" __cxa_eh_globals *__cxa_get_globals() { return &g_globals; }
extern "C" __cxa_eh_globals *__cxa_get_globals_fast() { return &g_globals; }

static __cxa_exception *header_of(void *thrown)
{
    return static_cast<__cxa_exception *>(thrown) - 1;
}

static void *object_of(__cxa_exception *h)
{
    return h + 1;
}

/* ---- allocating and throwing --------------------------------------- */

extern "C" void *__cxa_allocate_exception(std::size_t n) noexcept
{
    /* The header sits immediately before the object, so one allocation
     * serves both and __cxa_free_exception can find the header from the
     * object by subtraction alone. */
    void *p = malloc(n + sizeof(__cxa_exception));
    if (!p)
        std::terminate();            /* nowhere to put it; nothing to throw */
    memset(p, 0, sizeof(__cxa_exception));
    return static_cast<char *>(p) + sizeof(__cxa_exception);
}

extern "C" void __cxa_free_exception(void *thrown) noexcept
{
    free(header_of(thrown));
}

static void exception_cleanup(_Unwind_Reason_Code, _Unwind_Exception *e)
{
    __cxa_exception *h = reinterpret_cast<__cxa_exception *>(
        reinterpret_cast<char *>(e) - offsetof(__cxa_exception, unwindHeader));
    if (h->exceptionDestructor)
        h->exceptionDestructor(object_of(h));
    free(h);
}

extern "C" void __cxa_throw(void *thrown, std::type_info *ti,
                            void (*dtor)(void *))
{
    __cxa_exception *h = header_of(thrown);
    h->exceptionType = ti;
    h->exceptionDestructor = dtor;
    h->unwindHeader.exception_class = kCxxClass;
    h->unwindHeader.exception_cleanup = exception_cleanup;
    __cxa_get_globals()->uncaughtExceptions++;

    _Unwind_RaiseException(&h->unwindHeader);

    /* Only reached when no handler exists anywhere on the stack. The
     * unwinder has NOT unwound anything -- pass one found nothing, so pass
     * two never ran -- which is exactly why the standard permits
     * terminate() to be called here with the throwing frame still live. */
    std::terminate();
}

extern "C" void __cxa_rethrow()
{
    __cxa_eh_globals *g = __cxa_get_globals();
    __cxa_exception *h = g->caughtExceptions;
    if (!h)
        std::terminate();            /* rethrow outside any handler */
    g->uncaughtExceptions++;
    /* The handler is still on the list: it is not "caught" any more, but
     * __cxa_end_catch must still run for it, so it stays until then. */
    h->handlerCount = -h->handlerCount;
    _Unwind_Resume_or_Rethrow(&h->unwindHeader);
    std::terminate();
}

/* ---- catching -------------------------------------------------------- */

extern "C" void *__cxa_begin_catch(void *exc) noexcept
{
    _Unwind_Exception *ue = static_cast<_Unwind_Exception *>(exc);
    if (ue->exception_class != kCxxClass) {
        /* A foreign exception caught by `catch (...)`. There is no C++
         * object in it, so there is nothing to hand back and nothing to
         * put on the caught list. */
        return exc;
    }
    __cxa_exception *h = reinterpret_cast<__cxa_exception *>(
        reinterpret_cast<char *>(ue) - offsetof(__cxa_exception, unwindHeader));
    __cxa_eh_globals *g = __cxa_get_globals();

    /* A negative handlerCount means the exception is mid-rethrow and the
     * handler that let it go has not run its __cxa_end_catch yet -- which
     * happens when the rethrow is caught in the SAME frame it was thrown
     * from. Negating keeps the nesting depth rather than losing it. */
    h->handlerCount = h->handlerCount < 0 ? -h->handlerCount + 1
                                          : h->handlerCount + 1;
    if (h != g->caughtExceptions) {
        h->nextException = g->caughtExceptions;
        g->caughtExceptions = h;
    }
    g->uncaughtExceptions--;
    return h->adjustedPtr;
}

extern "C" void __cxa_end_catch()
{
    __cxa_eh_globals *g = __cxa_get_globals();
    __cxa_exception *h = g->caughtExceptions;
    if (!h)
        return;                      /* a foreign exception; nothing held */

    int count = h->handlerCount;
    if (count < 0) {
        /* Negative means the exception was RETHROWN out of this handler,
         * and is in flight again. This call is the handler's own cleanup,
         * running as the unwinder passes back through the frame -- so the
         * handler is finished with it, but the UNWINDER is not.
         *
         * Popping it from the caught list is right; destroying it is not.
         * The unwinder is still carrying the object and will hand it to
         * whichever handler catches it next, which then reads freed
         * memory. It is a use-after-free that works nearly always, because
         * nothing has reused the block yet by the time the next handler
         * looks -- and that is precisely why it has to be reasoned about
         * rather than tested for. The exception is destroyed by the
         * __cxa_end_catch of the handler that finally keeps it. */
        h->handlerCount = ++count;
        if (count == 0)
            g->caughtExceptions = h->nextException;
        /* Writing the count back even when it reaches zero is the part
         * that is easy to drop, and dropping it does not double-free --
         * it LEAKS. A handlerCount left negative sends the next
         * __cxa_begin_catch down the rethrow path, which counts the
         * exception as held twice; the final end_catch then decrements to
         * one instead of zero and never destroys it. The object's
         * destructor simply never runs, which no test of the handler's
         * behaviour can see. */
        return;
    }
    if (--count == 0) {
        g->caughtExceptions = h->nextException;
        _Unwind_DeleteException(&h->unwindHeader);
    } else if (count > 0) {
        h->handlerCount = count;
    } else {
        /* More end_catches than begin_catches: the generated code and this
         * runtime disagree about the shape of a handler. */
        std::terminate();
    }
}

extern "C" std::type_info *__cxa_current_exception_type() noexcept
{
    __cxa_exception *h = __cxa_get_globals()->caughtExceptions;
    return h ? const_cast<std::type_info *>(h->exceptionType) : nullptr;
}

extern "C" void *__cxa_get_exception_ptr(void *exc) noexcept
{
    _Unwind_Exception *ue = static_cast<_Unwind_Exception *>(exc);
    if (ue->exception_class != kCxxClass)
        return exc;
    __cxa_exception *h = reinterpret_cast<__cxa_exception *>(
        reinterpret_cast<char *>(ue) - offsetof(__cxa_exception, unwindHeader));
    return h->adjustedPtr;
}

/* ---- DWARF pointer encodings ----------------------------------------
 * The exception tables are written in the same self-describing pointer
 * encodings DWARF uses: each table says how its pointers are stored
 * (absolute, relative to what, how wide) so the linker can make them
 * read-only and position-independent. Reading them is mechanical, and
 * getting the "relative to what" wrong gives a plausible-looking pointer
 * into the wrong section, which is why each base is named here.
 */
enum {
    DW_EH_PE_omit    = 0xff,
    DW_EH_PE_uleb128 = 0x01,
    DW_EH_PE_udata2  = 0x02,
    DW_EH_PE_udata4  = 0x03,
    DW_EH_PE_udata8  = 0x04,
    DW_EH_PE_sleb128 = 0x09,
    DW_EH_PE_sdata2  = 0x0a,
    DW_EH_PE_sdata4  = 0x0b,
    DW_EH_PE_sdata8  = 0x0c,
    DW_EH_PE_pcrel   = 0x10,
    DW_EH_PE_textrel = 0x20,
    DW_EH_PE_datarel = 0x30,
    DW_EH_PE_funcrel = 0x40,
    DW_EH_PE_aligned = 0x50,
    DW_EH_PE_indirect = 0x80
};

static unsigned long read_uleb(const unsigned char **p)
{
    unsigned long v = 0;
    int shift = 0;
    unsigned char b;
    do {
        b = *(*p)++;
        v |= static_cast<unsigned long>(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    return v;
}

static long read_sleb(const unsigned char **p)
{
    unsigned long v = 0;
    int shift = 0;
    unsigned char b;
    do {
        b = *(*p)++;
        v |= static_cast<unsigned long>(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40))
        v |= ~0UL << shift;          /* sign-extend */
    return static_cast<long>(v);
}

/* How wide a fixed-size encoding is, for skipping past one. */
static unsigned encoded_size(unsigned char enc)
{
    switch (enc & 0x0f) {
    case DW_EH_PE_udata2: case DW_EH_PE_sdata2: return 2;
    case DW_EH_PE_udata4: case DW_EH_PE_sdata4: return 4;
    case DW_EH_PE_udata8: case DW_EH_PE_sdata8: return 8;
    case 0x00: return sizeof(void *);            /* absptr */
    default: return 0;                           /* leb128: variable */
    }
}

static unsigned long read_encoded(const unsigned char **p, unsigned char enc,
                                  _Unwind_Context *ctx)
{
    if (enc == DW_EH_PE_omit)
        return 0;

    const unsigned char *start = *p;
    unsigned long v = 0;

    if ((enc & 0x70) == DW_EH_PE_aligned) {
        unsigned long a = reinterpret_cast<unsigned long>(*p);
        a = (a + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
        *p = reinterpret_cast<const unsigned char *>(a);
        v = *reinterpret_cast<const unsigned long *>(*p);
        *p += sizeof(void *);
        return v;
    }

    switch (enc & 0x0f) {
    case 0x00:
        memcpy(&v, *p, sizeof(void *)); *p += sizeof(void *); break;
    case DW_EH_PE_uleb128: v = read_uleb(p); break;
    case DW_EH_PE_sleb128: v = static_cast<unsigned long>(read_sleb(p)); break;
    case DW_EH_PE_udata2: { unsigned short t; memcpy(&t, *p, 2); *p += 2;
                            v = t; break; }
    case DW_EH_PE_udata4: { unsigned int t; memcpy(&t, *p, 4); *p += 4;
                            v = t; break; }
    case DW_EH_PE_udata8: { memcpy(&v, *p, 8); *p += 8; break; }
    case DW_EH_PE_sdata2: { short t; memcpy(&t, *p, 2); *p += 2;
                            v = static_cast<unsigned long>(static_cast<long>(t));
                            break; }
    case DW_EH_PE_sdata4: { int t; memcpy(&t, *p, 4); *p += 4;
                            v = static_cast<unsigned long>(static_cast<long>(t));
                            break; }
    case DW_EH_PE_sdata8: { long t; memcpy(&t, *p, 8); *p += 8;
                            v = static_cast<unsigned long>(t); break; }
    default: std::terminate();       /* an encoding this reader does not know */
    }

    if (v != 0) {
        switch (enc & 0x70) {
        case 0x00: break;                                   /* absolute */
        case DW_EH_PE_pcrel:
            v += reinterpret_cast<unsigned long>(start);
            break;
        case DW_EH_PE_textrel: v += _Unwind_GetTextRelBase(ctx); break;
        case DW_EH_PE_datarel: v += _Unwind_GetDataRelBase(ctx); break;
        case DW_EH_PE_funcrel: v += _Unwind_GetRegionStart(ctx); break;
        default: std::terminate();
        }
        if (enc & DW_EH_PE_indirect)
            v = *reinterpret_cast<const unsigned long *>(v);
    }
    return v;
}

/* ---- matching a catch clause ---------------------------------------- */

/* Can an exception of type `thrown` be caught by `catch (C)`, and if so
 * what pointer does the handler see?
 *
 * The second half is the part that bites. A match is not enough: the
 * handler has to be handed the right address, and what "right" means
 * differs by case -- the base subobject for a base-reference catch, the
 * pointer's value for a pointer catch. Both are easy to get subtly wrong
 * in a way that type-checks and then reads the wrong memory.
 */
static bool can_catch(const type_info *catch_ti, const type_info *thrown,
                      void *&adjusted)
{
    if (!catch_ti)
        return true;                 /* catch (...) */
    if (!thrown)
        return false;

    /* Pointers come FIRST, before the exact-type test, because they are
     * the one case where a match still changes what the handler receives.
     * For `catch (T *p)` the thrown object IS a pointer variable, and the
     * handler wants the VALUE in it -- not the address of the slot the
     * value is sitting in. Letting the exact-match test short-circuit here
     * hands the handler a pointer to a pointer that looks entirely valid
     * and reads the wrong memory on first use. */
    if (ti_kind_of(catch_ti) == TI_POINTER) {
        if (ti_kind_of(thrown) != TI_POINTER)
            return false;
        const __pbase_type_info *cp =
            static_cast<const __pbase_type_info *>(catch_ti);
        const __pbase_type_info *tp =
            static_cast<const __pbase_type_info *>(thrown);
        /* The caught type may ADD cv-qualification, never remove it:
         * `catch (const T*)` takes a `T*`, and not the other way round. */
        if (tp->__flags & ~cp->__flags)
            return false;

        void *val = *static_cast<void **>(adjusted);

        if (*cp->__pointee == *tp->__pointee) { adjusted = val; return true; }
        /* catch (void *) takes any object pointer. */
        if (cp->__pointee->name()[0] == 'v' && cp->__pointee->name()[1] == 0)
            { adjusted = val; return true; }
        if (!val) { adjusted = nullptr; return true; }   /* null converts */
        if (!ti_is_class(cp->__pointee) || !ti_is_class(tp->__pointee))
            return false;
        const void *p = val;
        if (!class_upcast(tp->__pointee, cp->__pointee, p))
            return false;
        adjusted = const_cast<void *>(p);
        return true;
    }

    if (*catch_ti == *thrown)
        return true;

    if (!ti_is_class(catch_ti) || !ti_is_class(thrown))
        return false;

    /* Caught by base reference or by value: hand the handler the address
     * of the BASE subobject, which with multiple inheritance is not the
     * address of the object. */
    const void *p = adjusted;
    if (!class_upcast(thrown, catch_ti, p))
        return false;
    adjusted = const_cast<void *>(p);
    return true;
}

/* ---- the personality routine ----------------------------------------- */

extern "C" _Unwind_Reason_Code
__gxx_personality_v0(int version, _Unwind_Action actions,
                     _Unwind_Exception_Class exc_class,
                     _Unwind_Exception *ue, _Unwind_Context *ctx)
{
    if (version != 1)
        return _URC_FATAL_PHASE1_ERROR;

    bool native = exc_class == kCxxClass;
    __cxa_exception *h = native
        ? reinterpret_cast<__cxa_exception *>(
              reinterpret_cast<char *>(ue) -
              offsetof(__cxa_exception, unwindHeader))
        : nullptr;

    const unsigned char *lsda = static_cast<const unsigned char *>(
        _Unwind_GetLanguageSpecificData(ctx));
    if (!lsda)
        return _URC_CONTINUE_UNWIND;     /* nothing here: keep going */

    unsigned long func_start = _Unwind_GetRegionStart(ctx);

    /* GetIPInfo, not GetIP: the return address is the instruction AFTER
     * the call, which may belong to the next call site's range. The flag
     * says whether this frame's "IP" is a return address needing the
     * adjustment or a signal frame's exact PC, which must not be adjusted. */
    int ip_before = 0;
    unsigned long ip = _Unwind_GetIPInfo(ctx, &ip_before);
    if (!ip_before)
        ip--;

    /* --- the LSDA header --- */
    unsigned char lpstart_enc = *lsda++;
    unsigned long lpstart = func_start;
    if (lpstart_enc != DW_EH_PE_omit)
        lpstart = read_encoded(&lsda, lpstart_enc, ctx);

    unsigned char ttype_enc = *lsda++;
    const unsigned char *ttype_base = nullptr;
    if (ttype_enc != DW_EH_PE_omit) {
        unsigned long off = read_uleb(&lsda);
        ttype_base = lsda + off;
    }

    unsigned char cs_enc = *lsda++;
    unsigned long cs_len = read_uleb(&lsda);
    const unsigned char *cs = lsda;
    const unsigned char *cs_end = cs + cs_len;
    const unsigned char *action_base = cs_end;

    /* --- find this IP's call site --- */
    unsigned long landing_pad = 0;
    unsigned long action = 0;
    bool found = false;
    while (cs < cs_end) {
        unsigned long start = read_encoded(&cs, cs_enc, ctx);
        unsigned long len = read_encoded(&cs, cs_enc, ctx);
        unsigned long lp = read_encoded(&cs, cs_enc, ctx);
        unsigned long act = read_uleb(&cs);
        if (ip < func_start + start)
            break;                   /* the table is sorted: no match above */
        if (ip < func_start + start + len) {
            landing_pad = lp ? lpstart + lp : 0;
            action = act;
            found = true;
            break;
        }
    }
    /* An IP with no call-site entry is code the compiler promised cannot
     * throw. Reaching here means it did. */
    if (!found)
        return _URC_CONTINUE_UNWIND;
    if (!landing_pad)
        return _URC_CONTINUE_UNWIND;     /* nothing to run in this frame */

    /* action == 0 means a CLEANUP: destructors to run, no catch. Pass one
     * must not stop for it -- that is the difference between a frame that
     * handles the exception and one that merely tidies up on the way past. */
    if (action == 0) {
        if (actions & _UA_SEARCH_PHASE)
            return _URC_CONTINUE_UNWIND;
        goto install;
    }

    {
        /* --- walk the action chain --- */
        const unsigned char *ap = action_base + action - 1;
        void *adjusted = native ? object_of(h) : static_cast<void *>(ue);
        int selector = 0;
        bool saw_cleanup = false;

        for (;;) {
            const unsigned char *here = ap;
            long filter = read_sleb(&ap);
            const unsigned char *nextp = ap;
            long next = read_sleb(&ap);

            if (filter == 0) {
                saw_cleanup = true;      /* a cleanup among the catches */
            } else if (filter > 0) {
                /* A catch clause. The types table is indexed BACKWARD from
                 * its base -- index 1 is the entry just before it. */
                const unsigned char *tp =
                    ttype_base - filter * (encoded_size(ttype_enc)
                                           ? encoded_size(ttype_enc)
                                           : sizeof(void *));
                const type_info *catch_ti = reinterpret_cast<const type_info *>(
                    read_encoded(&tp, ttype_enc, ctx));
                void *cand = adjusted;
                if (native && can_catch(catch_ti, h->exceptionType, cand)) {
                    if (actions & _UA_SEARCH_PHASE) {
                        /* Remember the decision so pass two does not have
                         * to make it again -- and, more importantly, so it
                         * cannot make a DIFFERENT one. */
                        h->handlerSwitchValue = static_cast<int>(filter);
                        h->actionRecord = here;
                        h->languageSpecificData = lsda;
                        h->catchTemp = reinterpret_cast<void *>(landing_pad);
                        h->adjustedPtr = cand;
                        return _URC_HANDLER_FOUND;
                    }
                    selector = static_cast<int>(filter);
                    adjusted = cand;
                    break;
                }
                if (!native && !catch_ti) {
                    /* catch (...) takes a foreign exception too. */
                    if (actions & _UA_SEARCH_PHASE)
                        return _URC_HANDLER_FOUND;
                    selector = static_cast<int>(filter);
                    break;
                }
            } else {
                /* An exception specification. This runtime does not
                 * implement dynamic exception specifications -- they were
                 * removed in C++17 and the compiler does not emit them. */
                std::terminate();
            }

            if (next == 0)
                break;
            ap = nextp + next;
        }

        /* No catch in this frame matched. Pass one keeps looking; pass two
         * still has to enter the landing pad if there were cleanups. */
        if (actions & _UA_SEARCH_PHASE)
            return _URC_CONTINUE_UNWIND;

        if (selector == 0 && !saw_cleanup)
            return _URC_CONTINUE_UNWIND;

        /* Pass two, and this is the handler frame: use the decision pass
         * one recorded rather than the one just computed, because the
         * unwinder chose THIS frame on the strength of it. */
        if ((actions & _UA_HANDLER_FRAME) && native) {
            landing_pad = reinterpret_cast<unsigned long>(h->catchTemp);
            selector = h->handlerSwitchValue;
            adjusted = h->adjustedPtr;
        }

        _Unwind_SetGR(ctx, __builtin_eh_return_data_regno(0),
                      reinterpret_cast<unsigned long>(ue));
        _Unwind_SetGR(ctx, __builtin_eh_return_data_regno(1),
                      static_cast<unsigned long>(selector));
        _Unwind_SetIP(ctx, landing_pad);
        return _URC_INSTALL_CONTEXT;
    }

install:
    /* A pure cleanup frame: enter the landing pad with selector 0, which
     * is what tells the generated code to run destructors and then
     * _Unwind_Resume rather than to enter a catch body. */
    _Unwind_SetGR(ctx, __builtin_eh_return_data_regno(0),
                  reinterpret_cast<unsigned long>(ue));
    _Unwind_SetGR(ctx, __builtin_eh_return_data_regno(1), 0);
    _Unwind_SetIP(ctx, landing_pad);
    return _URC_INSTALL_CONTEXT;
}

/* The compiler emits a call to this where a destructor throws during
 * unwinding: two exceptions in flight, which [except.terminate] makes
 * terminate() rather than a choice. */
extern "C" void __cxa_call_terminate(void *exc) noexcept
{
    if (exc)
        __cxa_begin_catch(exc);
    std::terminate();
}

extern "C" void __cxa_call_unexpected(void *exc)
{
    __cxa_call_terminate(exc);
}

extern "C" void __cxa_bad_cast() { throw std::bad_cast(); }
extern "C" void __cxa_bad_typeid() { throw std::bad_typeid(); }
extern "C" void __cxa_throw_bad_array_new_length()
{ throw std::bad_array_new_length(); }

}  // namespace __cxxabiv1

namespace std {

int uncaught_exceptions() noexcept
{
    return static_cast<int>(__cxxabiv1::__cxa_get_globals()->uncaughtExceptions);
}

}  // namespace std
