/* The Itanium C++ ABI's type_info hierarchy, §2.9.5.
 *
 * These layouts are NOT this library's choice. The compiler emits the RTTI
 * objects (src/cxx/emit.c) as arrays of words, and the runtime has to
 * describe them the same way or `catch` matches the wrong type and
 * dynamic_cast returns the wrong pointer. Each class below is annotated
 * with the words emit.c writes for it.
 */
#ifndef _LIBCXX_ABI_H
#define _LIBCXX_ABI_H

#include <cstddef>
#include <typeinfo>

namespace __cxxabiv1 {

using std::type_info;

/* { vtable+16, name } -- an int, a float, void, nullptr_t. */
class __fundamental_type_info : public type_info {
public:
    ~__fundamental_type_info() override;
};

class __array_type_info : public type_info {
public:
    ~__array_type_info() override;
};

class __function_type_info : public type_info {
public:
    ~__function_type_info() override;
};

class __enum_type_info : public type_info {
public:
    ~__enum_type_info() override;
};

/* { vtable+16, name } -- a class with no bases. */
class __class_type_info : public type_info {
public:
    ~__class_type_info() override;
};

/* { vtable+16, name, base } -- exactly one public, non-virtual base at
 * offset 0, which is the overwhelmingly common case and gets its own
 * class so the walk costs one pointer load instead of a loop. */
class __si_class_type_info : public __class_type_info {
public:
    ~__si_class_type_info() override;
    const __class_type_info *__base_type;
};

/* One entry per base: { base, offset_flags }. The low byte of
 * offset_flags is the flags; offset_flags >> 8 is the offset -- and for a
 * VIRTUAL base that is not the offset itself but the byte offset, from the
 * vptr, of the slot where the real offset lives. */
struct __base_class_type_info {
    const __class_type_info *__base_type;
    long __offset_flags;

    enum {
        __virtual_mask = 0x1,
        __public_mask  = 0x2,
        __offset_shift = 8
    };

    bool is_virtual() const { return __offset_flags & __virtual_mask; }
    bool is_public() const { return __offset_flags & __public_mask; }
    long offset() const { return __offset_flags >> __offset_shift; }
};

/* { vtable+16, name, flags | (base_count << 32), base_info... } -- the
 * third word packs two 32-bit fields, which is why emit.c writes it as one
 * long and this reads it as two ints. Little-endian, both targets. */
class __vmi_class_type_info : public __class_type_info {
public:
    ~__vmi_class_type_info() override;
    unsigned int __flags;
    unsigned int __base_count;
    __base_class_type_info __base_info[1];

    enum {
        __non_diamond_repeat_mask = 0x1,
        __diamond_shaped_mask     = 0x2
    };
};

/* { vtable+16, name, flags, pointee }. */
class __pbase_type_info : public type_info {
public:
    ~__pbase_type_info() override;
    unsigned int __flags;
    const type_info *__pointee;

    enum {
        __const_mask            = 0x1,
        __volatile_mask         = 0x2,
        __restrict_mask         = 0x4,
        __incomplete_mask       = 0x8,
        __incomplete_class_mask = 0x10,
        __transaction_safe_mask = 0x20,
        __noexcept_mask         = 0x40
    };
};

class __pointer_type_info : public __pbase_type_info {
public:
    ~__pointer_type_info() override;
};

class __pointer_to_member_type_info : public __pbase_type_info {
public:
    ~__pointer_to_member_type_info() override;
    const __class_type_info *__context;
};

/* Where a polymorphic object keeps what the runtime needs. The vptr points
 * at the address point; the two words BELOW it are the offset back to the
 * most-derived object and that object's type_info. */
struct __vtable_prefix {
    long __offset_to_top;
    const type_info *__type_info;
};

inline const __vtable_prefix *vtable_prefix_of(const void *obj)
{
    const __vtable_prefix *const *vptr =
        static_cast<const __vtable_prefix *const *>(obj);
    return *vptr - 1;
}

/* Which of the above a type_info object actually is. Answered by
 * comparing its vptr against the known vtables rather than by virtual
 * dispatch -- see src/typeinfo.cc for why. */
enum ti_kind { TI_OTHER, TI_CLASS, TI_SI_CLASS, TI_VMI_CLASS, TI_POINTER };

ti_kind ti_kind_of(const type_info *t);
bool ti_is_class(const type_info *t);

/* Adjust `p`, which points at a `from` object, to its `to` base
 * subobject. False if `to` is not an unambiguous public base -- which is
 * what makes `catch (Base &)` decline a `Derived` that inherits it
 * privately or twice. Shared by dynamic_cast and by catch matching,
 * because they are asking the same question. */
bool class_upcast(const type_info *from, const type_info *to, const void *&p);

}  // namespace __cxxabiv1

namespace abi = __cxxabiv1;

#endif
