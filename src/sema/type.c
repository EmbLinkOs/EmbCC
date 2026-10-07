#include "type.h"

#include <stdio.h>
#include <string.h>

#include "../driver/util.h"
#include "../arch/target.h"

/* [kind][is_unsigned] — TY_PTR/TY_ARRAY/TY_STRUCT handled separately.
 * Designated initializers so this table survives struct type growing.
 * _Bool is unsigned in both slots (it holds only 0 or 1). */
static struct type bases[10][2] = {
    { { .kind = TY_VOID }, { .kind = TY_VOID } },
    { { .kind = TY_BOOL, .is_unsigned = 1 },
      { .kind = TY_BOOL, .is_unsigned = 1 } },
    { { .kind = TY_CHAR }, { .kind = TY_CHAR, .is_unsigned = 1 } },
    { { .kind = TY_SHORT }, { .kind = TY_SHORT, .is_unsigned = 1 } },
    { { .kind = TY_INT }, { .kind = TY_INT, .is_unsigned = 1 } },
    { { .kind = TY_LONG }, { .kind = TY_LONG, .is_unsigned = 1 } },
    { { .kind = TY_FLOAT }, { .kind = TY_FLOAT } },   /* never unsigned */
    { { .kind = TY_DOUBLE }, { .kind = TY_DOUBLE } },
    /* long double: x87 80-bit extended in 16 bytes (x86-64), IEEE binary128
     * (aarch64) — 16 bytes, 16-aligned, on both */
    { { .kind = TY_LDOUBLE }, { .kind = TY_LDOUBLE } },
    /* GNU __int128: two eightbytes, 16-aligned (both ABIs) */
    { { .kind = TY_INT128 }, { .kind = TY_INT128, .is_unsigned = 1 } },
};

/* `long long`, which shares TY_LONG's kind and differs only in width on
 * a target where long is not already eight bytes. Its own rows because
 * ty_base() hands out pointers INTO the table above and every caller
 * compares those pointers by identity. */
static struct type llongs[2] = {
    { .kind = TY_LONG, .is_llong = 1 },
    { .kind = TY_LONG, .is_llong = 1, .is_unsigned = 1 },
};

/* Plain `char` has the kind and signedness of one of the other two, but
 * is a distinct TYPE (C11 6.2.5p15), which only _Generic observes. Its own
 * nodes, so ty_is_plain_char tells it apart by identity. */
static struct type plain_chars[2] = {
    { .kind = TY_CHAR }, { .kind = TY_CHAR, .is_unsigned = 1 }
};

struct type *ty_plain_char(void)
{
    return &plain_chars[target_char_unsigned() ? 1 : 0];
}

int ty_is_plain_char(const struct type *t)
{
    const struct type *c = t->canon ? t->canon : t;
    return c == &plain_chars[0] || c == &plain_chars[1];
}

/* Two types the same for _Generic, which is stricter than ty_equal:
 * `long` is not `long long`, plain `char` is neither signed nor unsigned
 * char, and a pointee's const, volatile and _Atomic count. */
int ty_generic_same(const struct type *a, const struct type *b)
{
    if (a->kind != b->kind || a->is_unsigned != b->is_unsigned ||
        a->is_llong != b->is_llong || a->is_volatile != b->is_volatile ||
        a->is_atomic != b->is_atomic || a->is_const != b->is_const ||
        a->is_flash != b->is_flash)
        return 0;
    if (a->kind == TY_CHAR && ty_is_plain_char(a) != ty_is_plain_char(b))
        return 0;
    if (a->kind == TY_PTR)
        return ty_generic_same(a->pointee, b->pointee);
    if (a->kind == TY_ARRAY)
        return (a->count == b->count || a->vla_len || b->vla_len) &&
               ty_generic_same(a->pointee, b->pointee);
    if (a->kind == TY_FUNC) {
        if (a->nptypes != b->nptypes || a->is_varargs != b->is_varargs ||
            !ty_generic_same(a->ret, b->ret))
            return 0;
        for (int i = 0; i < a->nptypes; i++)
            if (!ty_equal(a->ptypes[i], b->ptypes[i]))
                return 0;
        return 1;
    }
    return ty_equal(a, b);
}

struct type *ty_wchar(void)
{
    /* int-sized, but for Xtensa's 16-bit unsigned short (xtensa/elf.h) */
    if (target_wchar_size() == 2 && target_int_size() != 2)
        return ty_base(TY_SHORT, target_wchar_unsigned());
    return ty_base(target_long_size_types() ? TY_LONG : TY_INT,
                   target_wchar_unsigned());
}

struct type *ty_llong(int is_unsigned)
{
    return &llongs[is_unsigned ? 1 : 0];
}

struct type *ty_size_t(void)
{
    return ty_base(target_int_size() == target_ptr_size() &&
                   !target_long_size_types() ? TY_INT : TY_LONG, 1);
}

struct type *ty_ptrdiff_t(void)
{
    return ty_base(target_int_size() == target_ptr_size() &&
                   !target_long_size_types() ? TY_INT : TY_LONG, 0);
}

struct type *ty_int_of_size(int size, int is_unsigned)
{
    switch (size) {
    case 1:  return ty_base(TY_CHAR, is_unsigned);
    case 2:  return ty_base(TY_SHORT, is_unsigned);
    case 4:  return target_int_size() == 4 ? ty_base(TY_INT, is_unsigned)
                                           : ty_base(TY_LONG, is_unsigned);
                                       /* AVR's int is two bytes */
    case 8:  return target_long_size() == 8 ? ty_base(TY_LONG, is_unsigned)
                                            : ty_llong(is_unsigned);
    case 16: return target_has_int128() ? ty_base(TY_INT128, is_unsigned)
                                        : NULL;
    default: return NULL;
    }
}

struct type *ty_base(enum ty_kind kind, int is_unsigned)
{
    return &bases[kind][is_unsigned ? 1 : 0];
}

/* Record a qualified copy of a struct with its original (see qcopies). */
static void note_qcopy(struct type *c)
{
    c->qcopies = NULL;
    if (c->kind != TY_STRUCT || !c->canon)
        return;
    c->qnext = c->canon->qcopies;
    c->canon->qcopies = c;
}

struct type *ty_volatile(struct type *t)
{
    if (!t || t->is_volatile)
        return t;
    struct type *v = xcalloc(1, sizeof *v);
    *v = *t;                 /* a non-interned copy */
    v->is_volatile = 1;
    v->canon = t->canon ? t->canon : t;  /* struct equality follows this */
    note_qcopy(v);
    return v;
}

struct type *ty_const(struct type *t)
{
    if (!t || t->is_const)
        return t;
    struct type *c = xcalloc(1, sizeof *c);
    *c = *t;                 /* a non-interned copy */
    c->is_const = 1;
    c->canon = t->canon ? t->canon : t;
    note_qcopy(c);
    /* A qualified array is an array of qualified elements (C11
     * 6.7.3p9): `const A x` with A an int[3] typedef cannot have x[0]
     * assigned. */
    if (t->kind == TY_ARRAY)
        c->pointee = ty_const(t->pointee);
    return c;
}

struct type *ty_unqual(struct type *t)
{
    if (t && t->canon &&
        (t->is_const || t->is_volatile || t->is_atomic || t->is_flash))
        return t->canon;
    return t;
}

/* AVR's `__flash`: a copy in program memory. An array of it is an array
 * of __flash elements, as with const, so that t[i] reads flash. */
struct type *ty_flash(struct type *t)
{
    if (!t || t->is_flash)
        return t;
    struct type *c = xcalloc(1, sizeof *c);
    *c = *t;
    c->is_flash = 1;
    c->canon = t->canon ? t->canon : t;
    note_qcopy(c);
    if (t->kind == TY_ARRAY)
        c->pointee = ty_flash(t->pointee);
    return c;
}

/* `_Atomic T`: volatile as well (never merged or removed), and every
 * access goes through the atomic paths in irgen. */
struct type *ty_atomic(struct type *t)
{
    if (!t || t->is_atomic)
        return t;
    struct type *a = ty_volatile(t);
    if (a == t) {                /* already volatile: copy it again */
        a = xcalloc(1, sizeof *a);
        *a = *t;
        a->canon = t->canon ? t->canon : t;
        note_qcopy(a);
    }
    a->is_atomic = 1;
    return a;
}

struct type *ty_ptr(struct type *pointee)
{
    struct type *t = xcalloc(1, sizeof *t);
    t->kind = TY_PTR;
    t->pointee = pointee;
    return t;
}

struct type *ty_array(struct type *elem, int count)
{
    struct type *t = xcalloc(1, sizeof *t);
    t->kind = TY_ARRAY;
    t->pointee = elem;
    t->count = count;
    return t;
}

struct type *ty_vla(struct type *elem, struct expr *len)
{
    struct type *t = ty_array(elem, 0);
    t->vla_len = len;
    t->vla_size = -1;
    return t;
}

int ty_is_vla(const struct type *t)
{
    return t && t->kind == TY_ARRAY && t->vla_len != NULL;
}

int ty_is_vm(const struct type *t)
{
    for (; t; t = t->pointee) {
        if (ty_is_vla(t))
            return 1;
        if (t->kind != TY_PTR && t->kind != TY_ARRAY)
            return 0;
    }
    return 0;
}

struct type *ty_func(struct type *ret, struct type **ptypes, int n,
                     int is_varargs)
{
    struct type *t = xcalloc(1, sizeof *t);
    t->kind = TY_FUNC;
    t->ret = ret;
    for (int i = 0; i < n; i++)
        t->ptypes[i] = ptypes[i];
    t->nptypes = n;
    t->is_varargs = is_varargs;
    return t;
}

struct type *ty_struct(const char *tag, int is_union)
{
    struct type *t = xcalloc(1, sizeof *t);
    t->kind = TY_STRUCT;
    t->tag = tag;
    t->is_union = is_union;
    return t;
}

/* `packed` drops every member's alignment to 1 (no inter-member padding,
 * struct align 1); `user_align`, from __attribute__((aligned(N))), raises
 * the struct's alignment to at least N. `pack`, from `#pragma pack(N)`
 * (0 for none), caps every member's alignment at N -- even one a member's
 * own aligned(M) raised, as gcc's maximum_field_alignment does -- and a
 * bit-field's alignment unit with it. */
int ty_bf_mempos(int bit_off, int bit_width, int unit_bits)
{
    return target_big_endian() ? unit_bits - bit_off - bit_width : bit_off;
}

/* The Microsoft bit-field layout, as GCC's place_field computes it when
 * TARGET_MS_BITFIELD_LAYOUT_P is true (RX, for a struct that is not
 * packed): a run of bit-fields whose declared types have the same SIZE
 * shares a storage unit of that type while the bits last; any other
 * field ends the run, the rest of whose unit is then skipped; a new unit
 * starts aligned for its type; a `:0` ends a run and is otherwise
 * ignored; and a bit-field's type raises the struct's alignment, named or
 * not (a `:0` only right after a nonzero bit-field). */
static void ms_struct_layout(struct type *t, struct member *members, int n,
                             int pack, int *align_out, long *bits_out)
{
    long bitpos = 0, remaining = 0, unit_start = 0;
    int align = 1;
    struct member *prev = NULL;      /* the run's latest bit-field */
    for (int i = 0; i < n; i++) {
        struct member *m = &members[i];
        int isbf = m->is_bitfield;
        long tsize = 8L * ty_size(m->ty);
        long width = isbf ? m->bit_width : tsize;
        int ma = ty_align(m->ty);
        struct member *prev_saved = prev;
        m->bf_bytes = 0;
        if (m->user_align > ma)
            ma = m->user_align;
        if (pack && ma > pack)
            ma = pack;
        if (!isbf || width != 0 || (prev && prev->bit_width != 0))
            if (ma > align)
                align = ma;
        if (prev) {
            if (isbf && width && prev->bit_width &&
                tsize == 8L * ty_size(prev->ty)) {
                if (remaining < width) {         /* out of bits */
                    bitpos += remaining;
                    unit_start = bitpos;
                    prev = m;
                    remaining = tsize < width ? 0 : tsize - width;
                } else {
                    remaining -= width;
                }
            } else {
                if (prev->bit_width)
                    bitpos += remaining;         /* use up the unit */
                else
                    prev_saved = NULL;
                if (!isbf || width == 0)
                    prev = NULL;
            }
        }
        if (!isbf || (prev_saved ? tsize != 8L * ty_size(prev_saved->ty)
                                 : width != 0)) {
            long ta = 8L * ma;
            remaining = tsize < width ? 0 : tsize - width;
            bitpos = (bitpos + ta - 1) / ta * ta;
            unit_start = bitpos;
            prev = NULL;
        }
        if (isbf) {
            m->off = (int)(unit_start / 8);
            m->bit_off = (int)(bitpos - unit_start);
        } else {
            m->off = (int)(bitpos / 8);
        }
        if (!prev && isbf)
            prev = m;
        bitpos += width;
        if (isbf && width && i == n - 1)
            bitpos += remaining;
    }
    (void)t;
    *align_out = align;
    *bits_out = bitpos;
}

void ty_struct_layout(struct type *t, struct member *members, int n,
                      int packed, int user_align, int pack)
{
    int align = 1;
    if (target_ms_bitfields() && !packed && !t->is_union) {
        long bits = 0;
        int bytes;
        ms_struct_layout(t, members, n, pack, &align, &bits);
        t->nat_align = align;
        if (user_align > align)
            align = user_align;
        bytes = (int)((bits + 7) / 8);
        t->members = members;
        t->nmembers = n;
        t->align = align;
        t->size = (bytes + align - 1) & ~(align - 1);
        t->complete = 1;
        for (struct type *q = t->qcopies; q; q = q->qnext) {
            struct type keep = *q;
            *q = *t;
            q->is_const = keep.is_const;
            q->is_volatile = keep.is_volatile;
            q->is_atomic = keep.is_atomic;
            q->canon = keep.canon;
            q->qnext = keep.qnext;
            q->qcopies = NULL;
        }
        return;
    }
    /* Non-bitfields track a byte offset; bitfields a bit position. The two
     * share one running cursor kept in bits (bitpos), rounded up to a byte
     * when a plain member intervenes — gcc's layout (a field never crosses
     * a boundary of its declared type). The positions are in MEMORY order
     * and the same in either byte order; what the byte order changes is
     * which bits of the unit a position is, and so bit_off (type.h). */
    int bitpos = 0;   /* bits from the struct start; unions ignore it */
    int umax = 0;     /* union: largest member extent, in bytes */

    for (int i = 0; i < n; i++) {
        struct member *m = &members[i];
        m->bf_bytes = 0;          /* (members may come uncleared) */
        int ma = packed ? 1 : ty_align(m->ty);
        /* An explicit __attribute__((aligned(N))) on the member raises its
         * alignment (and, through `align` below, the struct's) — it overrides
         * even `packed`, which only lowers the *default* alignment. */
        if (m->user_align > ma)
            ma = m->user_align;
        if (pack && ma > pack)
            ma = pack;

        if (m->is_bitfield) {
            int unit = 8 * ty_size(m->ty);   /* storage-unit width, bits */
            /* The type's ALIGNMENT in bits, which is what GCC's rules are
             * written in -- and which is the size everywhere but AVR, where
             * every alignment is one byte. Using the size made a `:0` round
             * up to two bytes there instead of one, and kept a 12-bit field
             * of a 16-bit type from straddling a byte boundary as avr-gcc lets
             * it. */
            int abits = packed ? 8 : 8 * ty_align(m->ty);
            if (pack && abits > 8 * pack)
                abits = 8 * pack;
            if (t->is_union) {
                m->off = 0;
                m->bit_off = ty_bf_mempos(0, m->bit_width, unit);
                int ext = (m->bit_width + 7) / 8;
                if (ext > umax) umax = ext;
            } else if (m->bit_width == 0) {
                /* a zero-width field rounds up to the next unit boundary and
                 * names nothing — a separator, never stored or accessed. */
                if (!packed)
                    bitpos = (bitpos + abits - 1) / abits * abits;
                m->off = bitpos / 8;
                m->bit_off = 0;
            } else {
                /* GCC's excess_unit_span (stor-layout.c): a field may not span
                 * more alignment units of its type than the type itself does.
                 * Where alignment is the size, that is "within one storage
                 * unit"; on AVR it lets a field straddle bytes as avr-gcc does. */
                if (!packed) {
                    int in = bitpos % abits;
                    if ((in + m->bit_width + abits - 1) / abits > unit / abits)
                        bitpos = (bitpos + abits - 1) / abits * abits;
                }
                m->off = (bitpos / unit) * ty_size(m->ty);
                m->bit_off = bitpos - m->off * 8;
                if (m->bit_off + m->bit_width > unit) {
                    /* (packed: across its unit — bytes from its first) */
                    m->off = bitpos / 8;
                    m->bit_off = bitpos % 8;
                    m->bf_bytes = (m->bit_off + m->bit_width + 7) / 8;
                    m->bit_off = ty_bf_mempos(m->bit_off, m->bit_width,
                                              8 * m->bf_bytes);
                } else {
                    m->bit_off = ty_bf_mempos(m->bit_off, m->bit_width, unit);
                }
                bitpos += m->bit_width;
            }
        } else {
            int ms = ty_size(m->ty);
            if (t->is_union) {
                m->off = 0;
                if (ms > umax) umax = ms;
            } else {
                int bytepos = (bitpos + 7) / 8;      /* leave any open unit */
                bytepos = (bytepos + ma - 1) & ~(ma - 1);
                m->off = bytepos;
                bitpos = (bytepos + ms) * 8;
            }
        }
        /* An unnamed bit-field raises the struct's alignment only on the
         * ARM ABIs; see target_anon_bitfield_aligns. */
        if (ma > align &&
            (!m->is_bitfield || m->name || target_anon_bitfield_aligns()))
            align = ma;
    }
    t->nat_align = align;
    if (user_align > align)
        align = user_align;
    int bytes = t->is_union ? umax : (bitpos + 7) / 8;
    t->members = members;
    t->nmembers = n;
    t->align = align;
    t->size = (bytes + align - 1) & ~(align - 1);
    t->complete = 1;
    /* Every qualified copy made before this body takes it too, keeping
     * its own qualifiers: `const struct unit *u` declared ahead of the
     * struct was otherwise "incomplete here" for ever. */
    for (struct type *q = t->qcopies; q; q = q->qnext) {
        struct type keep = *q;
        *q = *t;
        q->is_const = keep.is_const;
        q->is_volatile = keep.is_volatile;
        q->is_atomic = keep.is_atomic;
        q->is_flash = keep.is_flash;
        q->canon = keep.canon;
        q->qnext = keep.qnext;
        q->qcopies = NULL;
    }
}

struct member *ty_find_member(struct type *t, const char *name)
{
    for (int i = 0; i < t->nmembers; i++)
        if (t->members[i].name && strcmp(t->members[i].name, name) == 0)
            return &t->members[i];
    return NULL;
}

int ty_size(const struct type *t)
{
    switch (t->kind) {
    case TY_BOOL: return 1;
    case TY_CHAR: return 1;
    case TY_SHORT: return 2;
    /* Two on AVR, four elsewhere. On a 16-bit target int is the same
     * width as a POINTER, which is what makes the usual arithmetic
     * conversions land differently from every other target here. */
    case TY_INT: return target_int_size();
    case TY_LONG: return t->is_llong ? 8 : target_long_size();
    case TY_FLOAT: return 4;
    /* Four on AVR, eight everywhere else -- a target property like
     * long double's, not a constant. */
    case TY_DOUBLE: return target_double_size();
    case TY_LDOUBLE: return target_ldouble_size();
    case TY_INT128: return 16;
    case TY_PTR: return target_ptr_size();
    case TY_ARRAY: return t->count * ty_size(t->pointee);
    case TY_STRUCT: return t->size; /* 0 while incomplete */
    case TY_FUNC: break;            /* no size; only pointers to it */
    case TY_VOID: break;
    }
    return 0;
}

/* The ARM procedure-call standards' natural alignment (type.h). */
int ty_natural_align(const struct type *t)
{
    if (t->kind == TY_ARRAY)
        return ty_natural_align(t->pointee);
    if (t->kind == TY_STRUCT && t->complete && t->nat_align)
        return t->nat_align;
    return ty_align(t);
}

int ty_align(const struct type *t)
{
    switch (t->kind) {
    case TY_ARRAY: return ty_align(t->pointee);
    case TY_STRUCT: return t->complete ? t->align : 1;
    default: {
        /* A scalar is aligned to its size, capped where the target says so:
         * on AVR every type's alignment is 1 (see target.c's `maxal`). A
         * struct inherits it through its members, which is why the cap is
         * here and not in the struct layout. */
        int a = ty_size(t), m = target_max_scalar_align();
        return m && a > m ? m : a;
    }
    }
}

int ty_equal(const struct type *a, const struct type *b)
{
    if (a->kind != b->kind || a->is_unsigned != b->is_unsigned)
        return 0;
    /* `long` against `long long`. C says they are distinct types
     * whatever the target, but this compiler has always let them
     * interchange and on LP64 nothing could go wrong: the sizes agree,
     * so the only cost was a diagnostic it did not give. Tightening
     * that everywhere is a separate change with its own fallout, and
     * mixing it into the one that adds a 32-bit target would put a pile
     * of new errors between a real regression and a bisect.
     *
     * So the distinction is enforced exactly where it is unsound to
     * ignore: when the two spellings are different WIDTHS, as they are
     * on ILP32, where taking a `long long *` to a `long` reads eight
     * bytes out of a four-byte object. */
    if (a->is_llong != b->is_llong && ty_size(a) != ty_size(b))
        return 0;
    if (a->kind == TY_PTR)
        return ty_equal(a->pointee, b->pointee);
    if (a->kind == TY_ARRAY)   /* a VLA is compatible with any length */
        return (a->count == b->count || a->vla_len || b->vla_len) &&
               ty_equal(a->pointee, b->pointee);
    if (a->kind == TY_STRUCT) {
        /* one node per tag: identity is equality — but a volatile copy points at
         * its original via `canon`, so compare canonical nodes. */
        const struct type *ca = a->canon ? a->canon : a;
        const struct type *cb = b->canon ? b->canon : b;
        return ca == cb;
    }
    if (a->kind == TY_FUNC) {
        if (a->nptypes != b->nptypes || a->is_varargs != b->is_varargs ||
            a->sret_first != b->sret_first || !ty_equal(a->ret, b->ret))
            return 0;
        for (int i = 0; i < a->nptypes; i++)
            if (!ty_equal(a->ptypes[i], b->ptypes[i]))
                return 0;
        return 1;
    }
    return 1;
}

int ty_is_integer(const struct type *t)
{
    return t->kind == TY_BOOL || t->kind == TY_CHAR ||
           t->kind == TY_SHORT || t->kind == TY_INT || t->kind == TY_LONG ||
           t->kind == TY_INT128;
}

/* A long double with a representation of its own -- x87's 80 bits or a
 * 128-bit quad -- as opposed to one that IS a double (ARM EABI) or a
 * float (AVR). Only these need the 16-byte paths; the others are
 * lowered exactly as the type they share a format with, while staying a
 * distinct C type for _Generic, format checking and C++ mangling. */
int ty_is_xldouble(const struct type *t)
{
    return t && t->kind == TY_LDOUBLE && target_ldouble_size() > 8;
}

int ty_is_float(const struct type *t)
{
    return t->kind == TY_FLOAT || t->kind == TY_DOUBLE ||
           t->kind == TY_LDOUBLE;
}

int ty_is_arith(const struct type *t)
{
    return ty_is_integer(t) || ty_is_float(t);
}

int ty_is_scalar(const struct type *t)
{
    return ty_is_arith(t) || t->kind == TY_PTR;
}

/* 64-bit value class. Floats have their own register file, so this
 * answers width only — never "which register bank".
 *
 * By size rather than by kind, because on ILP32 a `long` and a pointer
 * are four bytes and a `long long` is eight; the kinds no longer
 * partition the way they did when both targets were LP64. */
int ty_wide(const struct type *t)
{
    return ty_is_scalar(t) && ty_size(t) == 8;
}

int ty_signed_int(const struct type *t)
{
    return ty_is_integer(t) && !t->is_unsigned;
}

/* Merge rule for two scalars landing in the same eightbyte: anything
 * non-floating makes the whole eightbyte INTEGER. */
static void class_merge(enum arg_class *slot, int *seen, enum arg_class c)
{
    if (!*seen) {
        *slot = c;
        *seen = 1;
        return;
    }
    if (c == CLASS_INTEGER)
        *slot = CLASS_INTEGER;
}

/* Walks every scalar leaf of t at byte offset `off`, classifying the
 * eightbyte each one falls in. Arrays and nested structs recurse, which
 * is what makes "all floating" mean all the way down. */
/* Returns nonzero when some field is UNALIGNED -- at an offset its own
 * type's alignment does not divide, which only packing makes -- and the
 * whole aggregate is then MEMORY (SysV 3.2.3), as gcc and clang pass it.
 * A scalar marks every eightbyte it covers: an __int128 member is both
 * halves, and marking only the first left the second "padding". */
static int classify_fields(const struct type *t, int off,
                           enum arg_class *cls, int *seen)
{
    if (t->kind == TY_STRUCT) {
        int bad = 0;
        for (int i = 0; i < t->nmembers; i++) {
            const struct member *m = &t->members[i];
            if (!m->is_bitfield && (off + m->off) % ty_align(m->ty))
                bad = 1;
            bad |= classify_fields(m->ty, off + m->off, cls, seen);
        }
        return bad;
    }
    if (t->kind == TY_ARRAY) {
        int esz = ty_size(t->pointee), bad = 0;
        for (int i = 0; i < t->count; i++)
            bad |= classify_fields(t->pointee, off + i * esz, cls, seen);
        return bad;
    }
    int sz = ty_size(t);
    for (int idx = off / 8; idx <= (off + (sz > 0 ? sz : 1) - 1) / 8; idx++)
        if (idx >= 0 && idx <= 1)   /* past that the caller said MEMORY */
            class_merge(&cls[idx], &seen[idx],
                        ty_is_float(t) ? CLASS_SSE : CLASS_INTEGER);
    return 0;
}

/* Does t contain a long double anywhere? */
static int has_ldouble(const struct type *t)
{
    if (t->kind == TY_LDOUBLE) return 1;
    if (t->kind == TY_ARRAY) return has_ldouble(t->pointee);
    if (t->kind == TY_STRUCT)
        for (int i = 0; i < t->nmembers; i++)
            if (has_ldouble(t->members[i].ty)) return 1;
    return 0;
}

static int only_ldouble(const struct type *t)
{
    if (t->kind == TY_LDOUBLE) return 1;
    if (t->kind == TY_ARRAY) return t->count > 0 && only_ldouble(t->pointee);
    if (t->kind != TY_STRUCT || t->nmembers == 0) return 0;
    for (int i = 0; i < t->nmembers; i++)
        if (!only_ldouble(t->members[i].ty)) return 0;
    return 1;
}

int ty_x87_struct(const struct type *t)
{
    return t->kind == TY_STRUCT && ty_size(t) == 16 && only_ldouble(t);
}

int ty_x87_ret(const struct type *t)
{
    if (ty_x87_struct(t)) return 1;
    if (t->kind == TY_STRUCT && t->is_complex && t->celem->kind == TY_LDOUBLE)
        return 2;
    return 0;
}

struct type *ty_complex(struct type *elem)
{
    static struct type *made[3];
    int k = elem->kind == TY_FLOAT ? 0 : elem->kind == TY_DOUBLE ? 1 : 2;
    if (made[k]) return made[k];
    struct type *t = ty_struct(NULL, 0);
    struct member *ms = xcalloc(2, sizeof *ms);
    ms[0].name = "__real__";
    ms[0].ty = elem;
    ms[1].name = "__imag__";
    ms[1].ty = elem;
    ty_struct_layout(t, ms, 2, 0, 0, 0);
    t->is_complex = 1;
    t->celem = elem;
    made[k] = t;
    return t;
}

int ty_is_complex(const struct type *t)
{
    return t && t->kind == TY_STRUCT && t->is_complex;
}

int ty_classify(const struct type *t, enum arg_class *classes)
{
    /* long double is X87 class; as an argument that means MEMORY, and a
     * struct holding one is MEMORY too (SysV 3.2.3). Where long double IS
     * a double (ARM EABI, AVR, Apple arm64) it is SSE like one: classed
     * MEMORY there, a long double argument looked like an integer to the
     * allocator, which put it in an x register -- and the aarch64 call
     * went looking for it in a v register's slot. */
    if (has_ldouble(t) && target_ldouble_size() > 8)
        return 0;
    if (t->kind == TY_INT128) {         /* two INTEGER eightbytes */
        classes[0] = classes[1] = CLASS_INTEGER;
        return 2;
    }
    if (t->kind != TY_STRUCT) {
        classes[0] = ty_is_float(t) ? CLASS_SSE : CLASS_INTEGER;
        return 1;
    }
    int size = ty_size(t);
    if (size > 16)
        return 0; /* MEMORY */

    int seen[2] = { 0, 0 };
    classes[0] = classes[1] = CLASS_INTEGER;
    if (classify_fields(t, 0, classes, seen))
        return 0; /* MEMORY: an unaligned field */
    int n = (size + 7) / 8;
    for (int i = 0; i < n; i++)
        if (!seen[i])
            classes[i] = CLASS_NONE;    /* padding only: no register */
    return n;
}

const char *ty_name(const struct type *t)
{
    /* Rotating buffers so one diagnostic can name two types — with a
     * single buffer "cannot convert char* to int" printed the SAME
     * spelling twice (found by a refusal test, of course). */
    static char bufs[4][64];
    static int which;
    char *buf = bufs[which];
    size_t bufsz = sizeof bufs[0];
    which = (which + 1) & 3;
    const char *base;
    int stars = 0;
    int dims[4];
    int ndims = 0;
    int pconst[8];          /* each pointer's own const, outermost first */

    while (t->kind == TY_PTR || t->kind == TY_ARRAY) {
        if (t->kind == TY_PTR) {
            if (stars < 8)
                pconst[stars] = t->is_const;
            stars++;
        } else {
            if (ndims < 4)
                dims[ndims] = t->vla_len ? -1 : t->count;   /* -1: [*] */
            ndims++;
        }
        t = t->pointee;
    }
    char structbuf[48];
    switch (t->kind) {
    case TY_VOID: base = "void"; break;
    case TY_BOOL: base = "_Bool"; break;
    case TY_CHAR: base = t->is_unsigned ? "unsigned char" : "char"; break;
    case TY_SHORT: base = t->is_unsigned ? "unsigned short" : "short"; break;
    case TY_INT: base = t->is_unsigned ? "unsigned int" : "int"; break;
    case TY_LONG:
        base = t->is_llong ? (t->is_unsigned ? "unsigned long long"
                                             : "long long")
                           : (t->is_unsigned ? "unsigned long" : "long");
        break;
    case TY_FLOAT: base = "float"; break;
    case TY_DOUBLE: base = "double"; break;
    case TY_LDOUBLE: base = "long double"; break;
    case TY_INT128: base = t->is_unsigned ? "unsigned __int128" : "__int128";
        break;
    case TY_STRUCT:
        if (t->is_complex) {
            snprintf(structbuf, sizeof structbuf, "%s _Complex",
                     t->celem->kind == TY_FLOAT ? "float"
                     : t->celem->kind == TY_DOUBLE ? "double" : "long double");
            base = structbuf;
            break;
        }
        snprintf(structbuf, sizeof structbuf, "%s %s",
                 t->is_union ? "union" : "struct",
                 t->tag ? t->tag : "<anonymous>");
        base = structbuf;
        break;
    case TY_FUNC:
        base = "function";
        break;
    default: base = "?"; break;
    }
    int n = snprintf(buf, bufsz, "%s%s%s", t->is_const ? "const " : "",
                     t->is_flash ? "__flash " : "", base);
    if (stars) {
        buf[n++] = ' ';
        /* innermost pointer first: `const char *const *` */
        for (int i = stars - 1; i >= 0 && n < (int)bufsz - 16; i--) {
            buf[n++] = '*';
            if (i < 8 && pconst[i])
                n += snprintf(buf + n, bufsz - (size_t)n, i ? "const " : "const");
        }
    }
    for (int i = 0; i < ndims && i < 4 && n < (int)bufsz - 16; i++)
        n += dims[i] < 0 ? snprintf(buf + n, bufsz - (size_t)n, "[*]")
                         : snprintf(buf + n, bufsz - (size_t)n, "[%d]", dims[i]);
    buf[n] = 0;
    return buf;
}

/* ---- AAPCS64 aggregate classification ----
 *
 * A property of the TYPE under that ABI, asked by two places: the aarch64
 * backend, and irgen, which records the answer on the instruction so the
 * IR need not carry the type itself.
 */
static int hfa_walk(const struct type *t, int *esz)
{
    switch (t->kind) {
    case TY_FLOAT:
    case TY_DOUBLE:
    case TY_LDOUBLE: {
        int sz = ty_size(t);
        if (*esz && *esz != sz)
            return -1;
        *esz = sz;
        return 1;
    }
    case TY_ARRAY: {
        if (t->count <= 0)
            return -1;
        int n = hfa_walk(t->pointee, esz);
        return n < 0 ? -1 : n * t->count;
    }
    case TY_STRUCT: {
        int n = 0;
        for (int m = 0; m < t->nmembers; m++) {
            const struct member *mb = &t->members[m];
            if (mb->is_bitfield)
                return -1;
            int k = hfa_walk(mb->ty, esz);
            if (k < 0)
                return -1;
            if (t->is_union) { if (k > n) n = k; }   /* the widest member */
            else n += k;
        }
        return n;
    }
    default:
        return -1;
    }
}

int ty_hfa(const struct type *t, int *esz)
{
    if (!t || t->kind != TY_STRUCT)
        return 0;
    *esz = 0;
    int n = hfa_walk(t, esz);
    if (n < 1 || n > 4 || ty_size(t) != n * *esz)
        return 0;
    return n;
}

/* A composite larger than 16 bytes that is not an HFA is passed as a POINTER
 * to a copy the caller makes (stage B.3), and returned through x8. */
int ty_aapcs64_byref(const struct type *t)
{
    int esz;
    return t && t->kind == TY_STRUCT && ty_size(t) > 16 && !ty_hfa(t, &esz);
}

/* ---- the usual conversions -------------------------------------------
 * Moved here from sema.c unchanged. They are pure functions of types,
 * and the parser needs ty_arith_common for `typeof(a - b)`. */

/* Integer promotion, C11 6.3.1.1p2: a type narrower than int becomes `int`
 * "if an int can represent all values of the original type", and `unsigned
 * int` otherwise. The second half never mattered until AVR, where short and
 * int are BOTH sixteen bits: `unsigned short` cannot fit in an int there, so it
 * promotes to unsigned int. This returned plain int for every short, so on
 * AVR `(unsigned short)1 < -1` -- true in C, because -1 converts to 65535 --
 * compiled to a signed compare and came out false. That is the ordinary
 * uint16_t-against-an-int pattern of firmware code. */
struct type *ty_promote(struct type *t)
{
    /* The value of an expression has no qualifiers: `x + 1` on a const
     * long is a long, and typeof of it can be assigned. */
    t = ty_unqual(t);
    if (t->kind == TY_CHAR || t->kind == TY_SHORT)
        return ty_base(TY_INT, t->is_unsigned &&
                               ty_size(t) >= ty_size(ty_base(TY_INT, 0)));
    return t;
}

/* Usual arithmetic conversions. On LP64 the ranks that matter are
 * int(32) and long(64), and long represents every unsigned int, so a
 * mixed int/long keeps the long's signedness.
 *
 * On ILP32 there are THREE: int(32), long(32) and long long(64). The
 * width test below still separates 64 from 32, but the answer at 64
 * has to be `long long` and not `long`, and two 32-bit operands one of
 * which is a `long` give a `long`. Returning ty_base(TY_LONG) at the
 * wide branch was right while long was always eight bytes and is a
 * silent NARROWING where it is four: `a + b` on two long longs came out
 * as a 32-bit add. */
struct type *ty_arith_common(struct type *a, struct type *b)
{
    /* Floating types outrank every integer, and long double > double >
     * float — the usual arithmetic conversions, floating half first. */
    if (a->kind == TY_LDOUBLE || b->kind == TY_LDOUBLE)
        return ty_base(TY_LDOUBLE, 0);
    if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE)
        return ty_base(TY_DOUBLE, 0);
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT)
        return ty_base(TY_FLOAT, 0);
    a = ty_promote(a);
    b = ty_promote(b);
    int qa = a->kind == TY_INT128, qb = b->kind == TY_INT128;
    if (qa || qb)       /* __int128 outranks long, holds all its values */
        return ty_base(TY_INT128, qa && qb ? a->is_unsigned || b->is_unsigned
                                           : (qa ? a : b)->is_unsigned);
    int wa = ty_wide(a), wb = ty_wide(b);
    if (wa || wb) {
        int uns;
        if (wa && wb)
            uns = a->is_unsigned || b->is_unsigned;
        else
            uns = (wa ? a : b)->is_unsigned;
        return ty_int_of_size(8, uns);
    }
    if (a->kind == TY_LONG || b->kind == TY_LONG) {
        /* Only reachable where a `long` is not wide: ILP32 and AVR.
         *
         * A signed long with an unsigned int takes the long's signedness
         * only if a long can represent EVERY unsigned int (C11 6.3.1.8) --
         * true on AVR, where long is 32 bits and int 16, and FALSE on ILP32,
         * where both are 32: there the answer is unsigned long. This took
         * the long's signedness unconditionally, so on a Cortex-M or RV32
         * `long a < unsigned b` compiled to a SIGNED compare, and -1L < 0u
         * was true. */
        int la = a->kind == TY_LONG, lb = b->kind == TY_LONG;
        int uns;
        if (la && lb) {
            uns = a->is_unsigned || b->is_unsigned;
        } else {
            struct type *l = la ? a : b, *o = la ? b : a;
            uns = l->is_unsigned ||
                  (o->is_unsigned && ty_size(o) >= ty_size(l));
        }
        return ty_base(TY_LONG, uns);
    }
    return ty_base(TY_INT, a->is_unsigned || b->is_unsigned);
}

