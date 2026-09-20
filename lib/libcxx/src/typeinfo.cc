/* std::type_info and the ABI's type_info hierarchy.
 *
 * Defining each class's destructor out of line is not a style choice: it
 * makes this translation unit the KEY FUNCTION of the class, so the vtable
 * is emitted here and exactly once. Those vtable symbols are what the
 * compiler's RTTI writes into every type_info object it emits
 * (`_ZTVN10__cxxabiv120__si_class_type_infoE + 16`), so if this file did
 * not exist nothing would link -- and if it emitted them twice, two
 * `catch` sites could disagree about whether a type matches.
 *
 * The +16 is the vtable's address point: two words of prefix
 * (offset-to-top, typeinfo) before the first virtual. It is the same
 * arithmetic on both targets.
 */
#include "abi.h"

#include <exception>
#include <new>
#include <string.h>

namespace std {

type_info::~type_info() {}

/* FNV-1a over the name, so equal types hash equally -- which is the only
 * thing [type.info] requires, and the reason this cannot hash the pointer:
 * two type_info objects for the same type may exist in different units. */
size_t type_info::hash_code() const noexcept
{
    size_t h = 1469598103934665603UL;
    for (const char *p = __name; *p; p++) {
        h ^= static_cast<unsigned char>(*p);
        h *= 1099511628211UL;
    }
    return h;
}

exception::~exception() noexcept {}
const char *exception::what() const noexcept { return "std::exception"; }

bad_exception::~bad_exception() noexcept {}
const char *bad_exception::what() const noexcept
{ return "std::bad_exception"; }

bad_cast::~bad_cast() noexcept {}
const char *bad_cast::what() const noexcept { return "std::bad_cast"; }

bad_typeid::~bad_typeid() noexcept {}
const char *bad_typeid::what() const noexcept { return "std::bad_typeid"; }

}  // namespace std

namespace __cxxabiv1 {

__fundamental_type_info::~__fundamental_type_info() {}
__array_type_info::~__array_type_info() {}
__function_type_info::~__function_type_info() {}
__enum_type_info::~__enum_type_info() {}
__class_type_info::~__class_type_info() {}
__si_class_type_info::~__si_class_type_info() {}
__vmi_class_type_info::~__vmi_class_type_info() {}
__pbase_type_info::~__pbase_type_info() {}
__pointer_type_info::~__pointer_type_info() {}
__pointer_to_member_type_info::~__pointer_to_member_type_info() {}

/* Which kind of type_info is this?
 *
 * By comparing the object's vptr against the known vtables, rather than by
 * calling a virtual function on it. Virtual dispatch would work too, and
 * is what libsupc++ does -- but it makes every query depend on the exact
 * SLOT LAYOUT of these vtables agreeing between the compiler that built
 * the RTTI and the runtime that reads it. The vptr value is written
 * literally by the compiler (`_ZTV... + 16`), so comparing it depends on
 * nothing but the symbol, and a mismatch is a link error rather than a
 * jump through the wrong slot.
 */
static const void *vptr_of(const type_info *t)
{
    return *reinterpret_cast<const void *const *>(t);
}

/* The address point of a class's vtable: two prefix words in. */
#define ADDR_POINT(sym) \
    (reinterpret_cast<const void *>(reinterpret_cast<const char *>(sym) + 16))

extern "C" {
extern void *_ZTVN10__cxxabiv117__class_type_infoE[];
extern void *_ZTVN10__cxxabiv120__si_class_type_infoE[];
extern void *_ZTVN10__cxxabiv121__vmi_class_type_infoE[];
extern void *_ZTVN10__cxxabiv119__pointer_type_infoE[];
}

ti_kind ti_kind_of(const type_info *t)
{
    const void *v = vptr_of(t);
    if (v == ADDR_POINT(_ZTVN10__cxxabiv117__class_type_infoE))
        return TI_CLASS;
    if (v == ADDR_POINT(_ZTVN10__cxxabiv120__si_class_type_infoE))
        return TI_SI_CLASS;
    if (v == ADDR_POINT(_ZTVN10__cxxabiv121__vmi_class_type_infoE))
        return TI_VMI_CLASS;
    if (v == ADDR_POINT(_ZTVN10__cxxabiv119__pointer_type_infoE))
        return TI_POINTER;
    return TI_OTHER;
}

bool ti_is_class(const type_info *t)
{
    ti_kind k = ti_kind_of(t);
    return k == TI_CLASS || k == TI_SI_CLASS || k == TI_VMI_CLASS;
}

}  // namespace __cxxabiv1
