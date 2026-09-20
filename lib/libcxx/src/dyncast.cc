/* __dynamic_cast — Itanium C++ ABI §2.9.7.
 *
 * `dynamic_cast<Dst*>(p)` where p has static type Src*. The compiler has
 * already handled every case it can decide statically (an upcast, a cast
 * to void*, a cast within a non-polymorphic type); what reaches here needs
 * the object's actual most-derived type, which only the vtable knows.
 *
 * The rule, stated as the standard states it, because the temptation is to
 * implement something simpler that is right most of the time:
 *
 *   the cast succeeds iff the most-derived object contains EXACTLY ONE
 *   Dst subobject that is publicly reachable, and the Src subobject the
 *   caller handed us is publicly reachable from it.
 *
 * "Exactly one" is the part that is usually got wrong. With multiple
 * inheritance a class can contain two Dst subobjects; the cast is then
 * AMBIGUOUS and must return null rather than picking either. A walk that
 * returns the first match it finds looks correct on every single-
 * inheritance test and is silently wrong the first time a diamond appears.
 */
#include "abi.h"

#include <cstddef>

namespace __cxxabiv1 {

/* What the search found. `count` distinguishes the three outcomes the
 * standard cares about: none, one, or more than one. */
struct result {
    const void *addr;
    int count;
};

static void search(const type_info *cur, const void *addr,
                   const type_info *want, bool path_public, result *out);

/* Where base `b` of the object at `addr` lives.
 *
 * For a virtual base the stored offset is NOT the offset: it is the byte
 * offset, from the object's vptr, of the slot that holds the real one.
 * That indirection is what makes a virtual base's position depend on the
 * most-derived type rather than on `cur`, which is the whole point of
 * virtual inheritance and the reason this cannot be a constant. */
static const void *base_addr(const __base_class_type_info *b, const void *addr)
{
    long off = b->offset();
    if (b->is_virtual()) {
        const char *const *vptr = *reinterpret_cast<const char *const *const *>(addr);
        off = *reinterpret_cast<const long *>(
                  reinterpret_cast<const char *>(vptr) + off);
    }
    return reinterpret_cast<const char *>(addr) + off;
}

static void search(const type_info *cur, const void *addr,
                   const type_info *want, bool path_public, result *out)
{
    if (*cur == *want) {
        if (!path_public)
            return;                  /* a private base is not reachable */
        /* The SAME subobject can be reached twice through a diamond with a
         * virtual base. Reaching one address twice is not ambiguity;
         * reaching two addresses is. */
        if (out->count && out->addr == addr)
            return;
        out->count++;
        out->addr = addr;
        return;
    }

    switch (ti_kind_of(cur)) {
    case TI_SI_CLASS: {
        const __si_class_type_info *si =
            static_cast<const __si_class_type_info *>(cur);
        /* Public, non-virtual, at offset 0 -- that is what makes it an
         * __si_class_type_info in the first place. */
        search(si->__base_type, addr, want, path_public, out);
        break;
    }
    case TI_VMI_CLASS: {
        const __vmi_class_type_info *vmi =
            static_cast<const __vmi_class_type_info *>(cur);
        for (unsigned i = 0; i < vmi->__base_count; i++) {
            const __base_class_type_info *b = &vmi->__base_info[i];
            search(b->__base_type, base_addr(b, addr), want,
                   path_public && b->is_public(), out);
        }
        break;
    }
    default:
        break;                       /* no bases to walk */
    }
}

/* "Is `to` an unambiguous public base of `from`, and where is it?"
 *
 * The same search dynamic_cast does, but starting from a STATIC type
 * rather than from the most-derived object -- which is what catch matching
 * needs: `catch (Base &e)` has only the thrown object's declared type to
 * work from, and must not consult a vptr the thrown object may not have
 * (int, or a class with no virtual functions, is throwable too). */
bool class_upcast(const type_info *from, const type_info *to, const void *&p)
{
    if (*from == *to)
        return true;
    result r = { nullptr, 0 };
    search(from, p, to, true, &r);
    if (r.count != 1)
        return false;                /* absent, private, or ambiguous */
    p = r.addr;
    return true;
}

extern "C" void *__dynamic_cast(const void *sub,
                                const __class_type_info *src,
                                const __class_type_info *dst,
                                std::ptrdiff_t src2dst)
{
    if (!sub)
        return nullptr;

    /* Step back to the most-derived object. Everything else is a search
     * within it, because a subobject cannot see its siblings. */
    const __vtable_prefix *pre = vtable_prefix_of(sub);
    const void *whole = reinterpret_cast<const char *>(sub) +
                        pre->__offset_to_top;
    const type_info *whole_ti = pre->__type_info;

    /* The hint, when the compiler could compute one: src is a unique
     * public non-virtual base of dst at this offset. It does not remove
     * the need to search -- the object may still contain several dsts --
     * but it says which dst is the answer, so the search only has to
     * confirm that one is unique and public. */
    if (src2dst >= 0) {
        const void *cand = reinterpret_cast<const char *>(sub) - src2dst;
        result r = { nullptr, 0 };
        search(whole_ti, whole, dst, true, &r);
        if (r.count == 1 && r.addr == cand)
            return const_cast<void *>(cand);
        return nullptr;
    }

    result r = { nullptr, 0 };
    search(whole_ti, whole, dst, true, &r);
    if (r.count != 1)
        return nullptr;              /* none, or ambiguous */

    /* Found one Dst. It is the answer if the Src the caller gave us is
     * publicly reachable from it (a downcast) -- or, for a CROSS-CAST
     * between siblings, if Src is publicly reachable from the most-derived
     * object at exactly the address we were handed. The second test is
     * what makes `dynamic_cast<Other*>(base_ptr)` work at all: Other is
     * not below Base, so the first test can never find it. */
    result s = { nullptr, 0 };
    search(dst, r.addr, src, true, &s);
    if (s.count == 1 && s.addr == sub)
        return const_cast<void *>(r.addr);

    result c = { nullptr, 0 };
    search(whole_ti, whole, src, true, &c);
    if (c.count == 1 && c.addr == sub)
        return const_cast<void *>(r.addr);

    return nullptr;
}

}  // namespace __cxxabiv1
