#include "dwarf.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../arch/target.h"

/* --- DWARF constants (only the handful this emitter uses) --- */
#define DW_TAG_compile_unit     0x11
#define DW_TAG_subprogram       0x2e
#define DW_TAG_formal_parameter 0x05
#define DW_TAG_variable         0x34
#define DW_TAG_base_type        0x24
#define DW_TAG_pointer_type     0x0f
#define DW_TAG_array_type       0x01
#define DW_TAG_structure_type   0x13
#define DW_TAG_union_type       0x17
#define DW_TAG_member           0x0d
#define DW_TAG_subrange_type    0x21
#define DW_CHILDREN_no        0x00
#define DW_CHILDREN_yes       0x01
#define DW_AT_name            0x03
#define DW_AT_byte_size       0x0b
#define DW_AT_stmt_list       0x10
#define DW_AT_low_pc          0x11
#define DW_AT_high_pc         0x12
#define DW_AT_language        0x13
#define DW_AT_comp_dir        0x1b
#define DW_AT_producer        0x25
#define DW_AT_ranges          0x55
#define DW_AT_encoding        0x3e
#define DW_AT_frame_base      0x40
#define DW_AT_type            0x49
#define DW_AT_location        0x02
#define DW_AT_bit_size        0x0d
#define DW_AT_upper_bound     0x2f
#define DW_AT_data_member_location 0x38
#define DW_AT_data_bit_offset 0x6b
#define DW_FORM_addr          0x01
#define DW_FORM_block1        0x0a
#define DW_FORM_data1         0x0b
#define DW_FORM_data2         0x05
#define DW_FORM_data4         0x06
#define DW_FORM_string        0x08
#define DW_FORM_ref4          0x13
#define DW_FORM_sec_offset    0x17
#define DW_FORM_exprloc       0x18
#define DW_FORM_udata         0x0f
#define DW_FORM_addrx         0x1b
#define DW_FORM_loclistx      0x22
#define DW_UT_skeleton        0x04
#define DW_UT_split_compile   0x05
#define DW_TAG_skeleton_unit  0x4a
#define DW_AT_dwo_name        0x76
#define DW_AT_addr_base       0x73
#define DW_OP_addrx           0xa1
#define DW_LLE_base_addressx  0x01
#define DW_UT_compile         0x01
#define DW_LNCT_path          0x1
#define DW_LNCT_directory_index 0x2
#define DW_RLE_end_of_list    0x00
#define DW_RLE_start_end      0x06    /* (0x07 is start_length; loclists' 7 is start_end) */
#define DW_LLE_end_of_list    0x00
#define DW_LLE_offset_pair    0x04
#define DW_LLE_base_address   0x06
#define DW_OP_reg0            0x50
#define DW_OP_regx            0x90
#define DW_OP_piece           0x93
#define DW_LANG_C99           0x000c
/* DW_ATE base-type encodings */
#define DW_ATE_boolean        0x02
#define DW_ATE_complex_float  0x03
#define DW_ATE_float          0x04
#define DW_ATE_signed         0x05
#define DW_ATE_signed_char    0x06
#define DW_ATE_unsigned       0x07
#define DW_ATE_unsigned_char  0x08
/* location/frame-base operations */
#define DW_OP_fbreg           0x91
#define DW_OP_addr            0x03
#define DW_OP_reg6            0x56   /* rbp — EmbCC's x86-64 frame pointer */
#define DW_OP_reg29           0x6d   /* x29 — its aarch64 frame pointer */
/* DW_OP_bregN is 0x70 + N, and takes a signed offset. The embedded
 * backends keep no frame pointer -- every slot is addressed from sp,
 * which the prologue sets once and never moves (the outgoing argument
 * area is part of the frame) -- so their frame base is sp itself. */
#define DW_OP_breg(n)         (0x70 + (n))
#define DW_REG_SP_ARM         13     /* r13 */
#define DW_REG_SP_RISCV       2      /* x2  */
#define DW_REG_FB_ARM         7      /* r7: the frame base under alloca */
#define DW_REG_FB_RISCV       8      /* x8, s0: the same */
#define DW_REG_SP_MIPS        29     /* $29 */
#define DW_REG_SP_RX          0      /* r0 */
#define DW_REG_FB_RX          13     /* r13: the frame base under alloca */
#define DW_REG_FB_MIPS        30     /* $30, fp/s8: the same */
#define DW_REG_SP_LA           3     /* $r3, sp: LoongArch numbers r0-r31 0-31 */
#define DW_REG_FB_LA          22     /* $r22, fp: the VLA frame base */
/* TriCore, as GCC for TriCore numbers its registers (unverified: D0-D15
 * are 0-15 and A0-A15 16-31): A10, the stack pointer, and A14, the frame
 * base the backend keeps under alloca. */
#define DW_REG_SP_TRICORE     26
#define DW_REG_FB_TRICORE     30
#define DW_REG_SP_XTENSA      1      /* a1 */
#define DW_REG_FB_XTENSA      7      /* a7: sp at entry, under alloca */
#define DW_REG_SP_PPC         1      /* r1 */
#define DW_REG_FB_PPC         31     /* r31: the frame base under alloca */
#define DW_REG_FP_SPARC       30     /* %i6, %fp: every SPARC slot's base */
#define DW_REG_FB_M68K        14     /* a6: every ColdFire function links it */
/* AVR numbers r0-r31 0-31 (GCC and LLVM agree; GCC's SP is 32). The frame
 * base is Y, r28:r29, which the prologue points one byte below the frame
 * and every slot is addressed from -- `ldd r24, Y+5` -- so `breg28 + 0`
 * makes each slot's DW_OP_fbreg offset the displacement the code uses. A
 * debugger reads the pointer-sized r28 as the pair, r29 the high byte. */
#define DW_REG_FB_AVR         28
/* What a data address is to a debugger on AVR. Data memory is a separate
 * space from program memory and both start at 0, so GDB, avr-gcc's
 * DWARF and QEMU's gdb stub all put data at 0x800000 up: `m800100,2`
 * reads SRAM at 0x100, where `m100,2` reads flash. A global's DW_OP_addr
 * carries the offset, and the code addresses carry none. */
#define AVR_DATA_SPACE        0x800000L

/* Abbreviation codes, shared by emit_abbrev and emit_info. Two each for
 * parameter/variable and pointer: the "with type" form carries DW_AT_type,
 * the plain form omits it (an abbrev's attribute list is fixed, so a type we
 * cannot yet describe — an aggregate, step 3 — needs its own abbrev rather
 * than a stale DW_AT_type; THE RULE: no confidently-wrong type). */
#define AB_CU            1
#define AB_SUBPROGRAM    2
#define AB_PARAM_T       3   /* formal_parameter, with type */
#define AB_PARAM         4   /* formal_parameter, no type */
#define AB_VAR_T         5   /* variable, with type */
#define AB_VAR           6   /* variable, no type */
#define AB_BASE          7   /* base_type */
#define AB_PTR_T         8   /* pointer_type, with type */
#define AB_PTR           9   /* pointer_type, no type (void *) */
#define AB_SUBPROGRAM_T 10   /* subprogram, with a return type */
#define AB_STRUCT       11   /* structure_type, named */
#define AB_STRUCT_ANON  12   /* structure_type, anonymous */
#define AB_UNION        13
#define AB_UNION_ANON   14
#define AB_MEMBER       15   /* member at a byte offset */
#define AB_MEMBER_BF    16   /* bitfield member: bit offset + width */
#define AB_ARRAY        17   /* array_type; one subrange child */
#define AB_SUBRANGE     18   /* subrange_type with its upper bound */
#define AB_SUBRANGE_NB  19   /* subrange_type, no bound ([] / a VLA) */
#define AB_CU_RANGES    20   /* compile_unit whose code is in several sections */
#define AB_PARAM_T_LL   21   /* formal_parameter, with type, location list */
#define AB_PARAM_LL     22
#define AB_VAR_T_LL     23   /* variable, with type, location list */
#define AB_VAR_LL       24

static int dw_ver = 5;

void dwarf_set_version(int v) { dw_ver = v == 4 ? 4 : 5; }
int dwarf_version(void) { return dw_ver; }

/* -gsplit-dwarf: what goes to the .dwo, and the object's address table
 * the .dwo's addresses index (each a relocation in .debug_addr) */
static const char *dw_dwo;
#define dw_split (dw_dwo != NULL)
struct dw_addr { int target; long addend; struct global *g; int end; };
static struct dw_addr *dw_addrs;
static int dw_naddr, dw_capaddr;
/* ...and where each location list starts in .debug_loclists.dwo, which
 * DW_FORM_loclistx numbers */
static int *dw_lloff;
static int dw_nll, dw_capll;

void dwarf_set_split(const char *dwo_name)
{
    dw_dwo = dwo_name;
    if (dwo_name)
        dw_ver = 5;
}

int dwarf_section_is_dwo(int sec)
{
    return sec == DWSEC_INFO_DWO || sec == DWSEC_ABBREV_DWO ||
           sec == DWSEC_LOC_DWO;
}

static int addr_index(int target, long addend, struct global *g, int end)
{
    for (int k = 0; k < dw_naddr; k++)
        if (dw_addrs[k].target == target && dw_addrs[k].addend == addend &&
            dw_addrs[k].g == g && dw_addrs[k].end == end)
            return k;
    if (dw_naddr == dw_capaddr) {
        dw_capaddr = dw_capaddr ? dw_capaddr * 2 : 16;
        dw_addrs = xrealloc(dw_addrs, (size_t)dw_capaddr * sizeof *dw_addrs);
    }
    dw_addrs[dw_naddr].target = target;
    dw_addrs[dw_naddr].addend = addend;
    dw_addrs[dw_naddr].g = g;
    dw_addrs[dw_naddr].end = end;
    return dw_naddr++;
}

const char *dwarf_section_name(int sec)
{
    switch (sec) {
    case DWSEC_ABBREV: return ".debug_abbrev";
    case DWSEC_INFO:   return ".debug_info";
    case DWSEC_LINE:   return ".debug_line";
    case DWSEC_RANGES: return dw_ver >= 5 ? ".debug_rnglists" : ".debug_ranges";
    case DWSEC_FRAME:  return ".debug_frame";
    case DWSEC_LOC:    return dw_ver >= 5 ? ".debug_loclists" : ".debug_loc";
    case DWSEC_ADDR:   return ".debug_addr";
    case DWSEC_INFO_DWO:   return ".debug_info.dwo";
    case DWSEC_ABBREV_DWO: return ".debug_abbrev.dwo";
    case DWSEC_LOC_DWO:    return ".debug_loclists.dwo";
    }
    return "";
}

/* Line-program standard opcodes */
#define DW_LNS_copy           0x01
#define DW_LNS_advance_pc     0x02
#define DW_LNS_advance_line   0x03
#define DW_LNS_const_add_pc   0x08
#define DW_LNS_set_prologue_end 0x0a
/* Extended opcodes */
#define DW_LNE_end_sequence   0x01
#define DW_LNE_set_address    0x02

/* line-program special-opcode parameters: gcc's and clang's. A special
 * opcode is one byte that advances the address by 0..17 and the line by
 * -5..+8 and appends a row -- what advance_line, advance_pc and copy say
 * in five or more (line_row). */
#define LINE_BASE   (-5)
#define LINE_RANGE  14
#define OPCODE_BASE 13
/* the address advance of opcode 255 with no line advance: const_add_pc's */
#define MAX_SPECIAL_ADDR ((255 - OPCODE_BASE) / LINE_RANGE)

/* --- a growable byte buffer, one per section --- */
struct dbuf { unsigned char *p; int len, cap; };

static void db_need(struct dbuf *b, int n)
{
    if (b->len + n > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        while (b->len + n > b->cap) b->cap *= 2;
        b->p = xrealloc(b->p, (size_t)b->cap);
    }
}
static void db_u8(struct dbuf *b, unsigned v)
{
    db_need(b, 1);
    b->p[b->len++] = (unsigned char)(v & 0xff);
}
/* Every multi-byte field in the target's byte order: DWARF is read by a
 * debugger of THAT machine (a big-endian target's sections are big-endian
 * throughout, as clang writes them). */
static void db_un(struct dbuf *b, int n, unsigned long v)
{
    db_need(b, n);
    target_put_uint(b->p + b->len, n, v);
    b->len += n;
}
static void db_u16(struct dbuf *b, unsigned v)
{
    db_un(b, 2, v & 0xffff);
}
static void db_u32(struct dbuf *b, unsigned long v)
{
    db_un(b, 4, v & 0xffffffffUL);
}
static void db_u64(struct dbuf *b, unsigned long v)
{
    db_un(b, 8, v);
}
static void db_str(struct dbuf *b, const char *s)
{
    do { db_u8(b, (unsigned char)*s); } while (*s++);
}
static void db_uleb(struct dbuf *b, unsigned long v)
{
    do {
        unsigned char byte = v & 0x7f;
        v >>= 7;
        if (v) byte |= 0x80;
        db_u8(b, byte);
    } while (v);
}
static void db_sleb(struct dbuf *b, long v)
{
    int more = 1;
    while (more) {
        unsigned char byte = (unsigned char)(v & 0x7f);
        v >>= 7;                       /* arithmetic shift (sign-propagating) */
        if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40)))
            more = 0;
        else
            byte |= 0x80;
        db_u8(b, byte);
    }
}

/* Record a relocation against a to-be-written zero field at the CURRENT end
 * of section `in_sec` (call immediately before writing the field's zeros). */
static void reloc(struct dwarf_out *out, int in_sec, int at_off, int width,
                  int target, long addend)
{
    if (out->nrelocs == out->reloccap) {
        out->reloccap = out->reloccap ? out->reloccap * 2 : 8;
        out->relocs = xrealloc(out->relocs,
                               (size_t)out->reloccap * sizeof *out->relocs);
    }
    struct dwarf_reloc *r = &out->relocs[out->nrelocs++];
    r->in_sec = in_sec;
    r->off = at_off;
    r->width = width;
    r->target = target;
    r->addend = addend;
    r->end = 0;
    r->glob = NULL;
}

/* The same for the address one past a function's code (dwarf.h). */
static void reloc_end(struct dwarf_out *out, int in_sec, int at_off,
                      int width, long addend)
{
    reloc(out, in_sec, at_off, width, DWTGT_TEXT, addend);
    out->relocs[out->nrelocs - 1].end = 1;
}

/* One abbreviation: code, tag, has-children, then (attr, form) pairs ended by
 * (0,0). The variadic-free spelling — a small (attr,form) array — keeps this
 * inside EmbCC's own subset (which compiles this file for self-hosting). */
static void one_abbrev(struct dbuf *b, int code, int tag, int children,
                       const unsigned char *pairs, int npairs)
{
    db_uleb(b, code);
    db_uleb(b, tag);
    db_u8(b, children);
    for (int i = 0; i < npairs; i++) {
        db_uleb(b, pairs[2 * i]);
        db_uleb(b, pairs[2 * i + 1]);
    }
    db_uleb(b, 0); db_uleb(b, 0);
}

/* --- .debug_abbrev: compile_unit + subprogram/param/var + base/pointer. --- */
static void emit_abbrev(struct dbuf *b)
{
    static const unsigned char cu[] = {
        DW_AT_producer,  DW_FORM_string, DW_AT_language, DW_FORM_data2,
        DW_AT_name,      DW_FORM_string, DW_AT_comp_dir, DW_FORM_string,
        DW_AT_low_pc,    DW_FORM_addr,   DW_AT_high_pc,  DW_FORM_addr,
        DW_AT_stmt_list, DW_FORM_sec_offset };
    /* the base address 0 and the list: DWARF 4's way of saying a unit's
     * code is in more than one place (2.17.3) */
    static const unsigned char cu_ranges[] = {
        DW_AT_producer,  DW_FORM_string, DW_AT_language, DW_FORM_data2,
        DW_AT_name,      DW_FORM_string, DW_AT_comp_dir, DW_FORM_string,
        DW_AT_low_pc,    DW_FORM_addr,   DW_AT_ranges,   DW_FORM_sec_offset,
        DW_AT_stmt_list, DW_FORM_sec_offset };
    unsigned char sub[] = {
        DW_AT_name,       DW_FORM_string, DW_AT_low_pc,     DW_FORM_addr,
        DW_AT_high_pc,    DW_FORM_addr,   DW_AT_frame_base, DW_FORM_exprloc };
    unsigned char sub_t[] = {
        DW_AT_name,       DW_FORM_string, DW_AT_type,       DW_FORM_ref4,
        DW_AT_low_pc,     DW_FORM_addr,   DW_AT_high_pc,    DW_FORM_addr,
        DW_AT_frame_base, DW_FORM_exprloc };
    if (dw_split) {
        /* the .dwo's: an address is an index into .debug_addr, and the
         * end is a length, which needs none */
        sub[3] = sub_t[5] = DW_FORM_addrx;
        sub[5] = sub_t[7] = DW_FORM_data4;
    }
    static const unsigned char param_t[] = {
        DW_AT_name, DW_FORM_string, DW_AT_type, DW_FORM_ref4,
        DW_AT_location, DW_FORM_exprloc };
    static const unsigned char param[] = {
        DW_AT_name, DW_FORM_string, DW_AT_location, DW_FORM_exprloc };
    static const unsigned char base[] = {
        DW_AT_name, DW_FORM_string, DW_AT_encoding, DW_FORM_data1,
        DW_AT_byte_size, DW_FORM_data1 };
    static const unsigned char ptr_t[] = {
        DW_AT_type, DW_FORM_ref4, DW_AT_byte_size, DW_FORM_data1 };
    static const unsigned char ptr[] = {
        DW_AT_byte_size, DW_FORM_data1 };
    static const unsigned char agg[] = {
        DW_AT_name, DW_FORM_string, DW_AT_byte_size, DW_FORM_data4 };
    static const unsigned char agg_anon[] = {
        DW_AT_byte_size, DW_FORM_data4 };
    static const unsigned char member[] = {
        DW_AT_name, DW_FORM_string, DW_AT_type, DW_FORM_ref4,
        DW_AT_data_member_location, DW_FORM_data4 };
    static const unsigned char member_bf[] = {
        DW_AT_name, DW_FORM_string, DW_AT_type, DW_FORM_ref4,
        DW_AT_data_bit_offset, DW_FORM_data4, DW_AT_bit_size, DW_FORM_data1 };
    static const unsigned char array[] = { DW_AT_type, DW_FORM_ref4 };
    static const unsigned char subrange[] = { DW_AT_upper_bound, DW_FORM_data4 };

    one_abbrev(b, AB_CU,         DW_TAG_compile_unit,     DW_CHILDREN_yes, cu,
               dw_split ? 4 : 7);   /* (split: its pc and lines the skeleton's) */
    one_abbrev(b, AB_SUBPROGRAM, DW_TAG_subprogram,       DW_CHILDREN_yes, sub, 4);
    one_abbrev(b, AB_PARAM_T,    DW_TAG_formal_parameter, DW_CHILDREN_no, param_t, 3);
    one_abbrev(b, AB_PARAM,      DW_TAG_formal_parameter, DW_CHILDREN_no, param, 2);
    one_abbrev(b, AB_VAR_T,      DW_TAG_variable,         DW_CHILDREN_no, param_t, 3);
    one_abbrev(b, AB_VAR,        DW_TAG_variable,         DW_CHILDREN_no, param, 2);
    one_abbrev(b, AB_BASE,       DW_TAG_base_type,        DW_CHILDREN_no, base, 3);
    one_abbrev(b, AB_PTR_T,      DW_TAG_pointer_type,     DW_CHILDREN_no, ptr_t, 2);
    one_abbrev(b, AB_PTR,        DW_TAG_pointer_type,     DW_CHILDREN_no, ptr, 1);
    one_abbrev(b, AB_SUBPROGRAM_T, DW_TAG_subprogram,     DW_CHILDREN_yes, sub_t, 5);
    one_abbrev(b, AB_STRUCT,      DW_TAG_structure_type, DW_CHILDREN_yes, agg, 2);
    one_abbrev(b, AB_STRUCT_ANON, DW_TAG_structure_type, DW_CHILDREN_yes, agg_anon, 1);
    one_abbrev(b, AB_UNION,       DW_TAG_union_type,     DW_CHILDREN_yes, agg, 2);
    one_abbrev(b, AB_UNION_ANON,  DW_TAG_union_type,     DW_CHILDREN_yes, agg_anon, 1);
    one_abbrev(b, AB_MEMBER,      DW_TAG_member,         DW_CHILDREN_no, member, 3);
    one_abbrev(b, AB_MEMBER_BF,   DW_TAG_member,         DW_CHILDREN_no, member_bf, 4);
    one_abbrev(b, AB_ARRAY,       DW_TAG_array_type,     DW_CHILDREN_yes, array, 1);
    one_abbrev(b, AB_SUBRANGE,    DW_TAG_subrange_type,  DW_CHILDREN_no, subrange, 1);
    one_abbrev(b, AB_SUBRANGE_NB, DW_TAG_subrange_type,  DW_CHILDREN_no, subrange, 0);
    one_abbrev(b, AB_CU_RANGES,   DW_TAG_compile_unit,   DW_CHILDREN_yes, cu_ranges, 7);
    unsigned char param_t_ll[] = {
        DW_AT_name, DW_FORM_string, DW_AT_type, DW_FORM_ref4,
        DW_AT_location, DW_FORM_sec_offset };
    unsigned char param_ll[] = {
        DW_AT_name, DW_FORM_string, DW_AT_location, DW_FORM_sec_offset };
    if (dw_split)
        param_t_ll[5] = param_ll[3] = DW_FORM_loclistx;
    one_abbrev(b, AB_PARAM_T_LL, DW_TAG_formal_parameter, DW_CHILDREN_no, param_t_ll, 3);
    one_abbrev(b, AB_PARAM_LL,   DW_TAG_formal_parameter, DW_CHILDREN_no, param_ll, 2);
    one_abbrev(b, AB_VAR_T_LL,   DW_TAG_variable,         DW_CHILDREN_no, param_t_ll, 3);
    one_abbrev(b, AB_VAR_LL,     DW_TAG_variable,         DW_CHILDREN_no, param_ll, 2);
    db_uleb(b, 0);                       /* end of the abbrev table */
}

/* Type DIEs are emitted once and referenced by their .debug_info offset (a
 * DW_FORM_ref4 — CU-relative, and our single CU starts at section offset 0,
 * so the section offset IS the reference). This maps each already-emitted
 * type to that offset. */
struct typemap {
    struct type **k; int *off; int n, cap;
    /* A struct whose DIE is being built (its member types first), and the
     * ref4 fields of pointers to it that wait for its offset — the only
     * cycle C types have is a struct reaching itself through a pointer. */
    struct type *open[16]; int nopen;
    struct { int at; struct type *to; } *fix; int nfix, capfix;
};

static int type_lookup(struct typemap *m, struct type *t)
{
    if (t && t->canon) t = t->canon;      /* as ensure_type records it */
    for (int i = 0; i < m->n; i++)
        if (m->k[i] == t) return m->off[i];
    return -1;
}
static void type_record(struct typemap *m, struct type *t, int off)
{
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8;
        m->k = xrealloc(m->k, (size_t)m->cap * sizeof *m->k);
        m->off = xrealloc(m->off, (size_t)m->cap * sizeof *m->off);
    }
    m->k[m->n] = t;
    m->off[m->n] = off;
    m->n++;
}

static int base_encoding(struct type *t)
{
    if (ty_is_float(t)) return DW_ATE_float;
    if (t->kind == TY_CHAR)
        return t->is_unsigned ? DW_ATE_unsigned_char : DW_ATE_signed_char;
    return t->is_unsigned ? DW_ATE_unsigned : DW_ATE_signed;
}

static void db_patch_u32(struct dbuf *b, int at, unsigned long v)
{
    target_put_uint(b->p + at, 4, v & 0xffffffffUL);
}

static int type_is_open(struct typemap *m, struct type *t)
{
    for (int i = 0; i < m->nopen; i++)
        if (m->open[i] == t) return 1;
    return 0;
}

/* Emit (once) the DIE for t and return its offset, or -1 if EmbCC has no DIE
 * for it (functions, void, an incomplete struct). A type's components are
 * emitted first so every ref4 points backward at an existing DIE — except a
 * pointer back into a struct still being built, patched when it completes. */
static int ensure_type(struct dbuf *b, struct typemap *m, struct type *t)
{
    if (!t) return -1;
    if (t->canon) t = t->canon;           /* volatile: describe the original */
    int e = type_lookup(m, t);
    if (e >= 0) return e;

    if (ty_is_integer(t) || ty_is_float(t) || ty_is_complex(t)) {
        /* a complex is a base type to DWARF (DW_ATE_complex_float), which
         * is what makes a debugger print it as re + im i */
        int off = b->len;
        db_uleb(b, AB_BASE);
        db_str(b, ty_name(t));           /* "int", "double _Complex", ... */
        db_u8(b, ty_is_complex(t) ? DW_ATE_complex_float : base_encoding(t));
        db_u8(b, ty_size(t));
        type_record(m, t, off);
        return off;
    }
    /* A pointer's byte_size is the target's: it was 8 everywhere, so on
     * Cortex-M and RV32 a debugger read a pointer variable's four bytes
     * and the four after them as one address. */
    if (t->kind == TY_PTR) {
        struct type *pt = t->pointee && t->pointee->canon ? t->pointee->canon
                                                          : t->pointee;
        if (pt && pt->kind == TY_STRUCT && type_is_open(m, pt)) {
            /* struct node { struct node *next; }: its offset comes later */
            int off = b->len;
            db_uleb(b, AB_PTR_T);
            if (m->nfix == m->capfix) {
                m->capfix = m->capfix ? m->capfix * 2 : 8;
                m->fix = xrealloc(m->fix, (size_t)m->capfix * sizeof *m->fix);
            }
            m->fix[m->nfix].at = b->len;
            m->fix[m->nfix].to = pt;
            m->nfix++;
            db_u32(b, 0);
            db_u8(b, (unsigned char)target_ptr_size());
            type_record(m, t, off);
            return off;
        }
        int pe = ensure_type(b, m, t->pointee);
        int off = b->len;
        if (pe >= 0) {
            db_uleb(b, AB_PTR_T);
            db_u32(b, (unsigned long)pe);
            db_u8(b, (unsigned char)target_ptr_size());
        } else {
            db_uleb(b, AB_PTR);          /* pointer to a type we can't name yet */
            db_u8(b, (unsigned char)target_ptr_size());
        }
        type_record(m, t, off);
        return off;
    }
    if (t->kind == TY_ARRAY) {
        int el = ensure_type(b, m, t->pointee);
        if (el < 0) return -1;
        int off = b->len;
        db_uleb(b, AB_ARRAY);
        db_u32(b, (unsigned long)el);
        if (t->count > 0 && !t->vla_len) {
            db_uleb(b, AB_SUBRANGE);
            db_u32(b, (unsigned long)(t->count - 1));
        } else {
            db_uleb(b, AB_SUBRANGE_NB);   /* [] or a VLA: no static bound */
        }
        db_u8(b, 0);                      /* end of the array's children */
        type_record(m, t, off);
        return off;
    }
    if (t->kind == TY_STRUCT && t->complete &&
        m->nopen < (int)(sizeof m->open / sizeof m->open[0])) {
        /* members' types first, so the member DIEs below refer back */
        m->open[m->nopen++] = t;
        for (int i = 0; i < t->nmembers; i++)
            if (t->members[i].name)
                (void)ensure_type(b, m, t->members[i].ty);
        m->nopen--;
        int off = b->len;
        db_uleb(b, t->tag ? (t->is_union ? AB_UNION : AB_STRUCT)
                          : (t->is_union ? AB_UNION_ANON : AB_STRUCT_ANON));
        if (t->tag) db_str(b, t->tag);
        db_u32(b, (unsigned long)t->size);
        for (int i = 0; i < t->nmembers; i++) {
            struct member *mb = &t->members[i];
            int mt = mb->name ? type_lookup(m, mb->ty->canon ? mb->ty->canon
                                                             : mb->ty) : -1;
            if (mt < 0) continue;          /* unnamed padding, or no DIE */
            if (mb->is_bitfield) {
                db_uleb(b, AB_MEMBER_BF);
                db_str(b, mb->name);
                db_u32(b, (unsigned long)mt);
                db_u32(b, (unsigned long)(mb->off * 8 +
                    ty_bf_mempos(mb->bit_off, mb->bit_width,
                                 8 * (mb->bf_bytes ? mb->bf_bytes
                                                   : ty_size(mb->ty)))));
                db_u8(b, mb->bit_width);
            } else {
                db_uleb(b, AB_MEMBER);
                db_str(b, mb->name);
                db_u32(b, (unsigned long)mt);
                db_u32(b, (unsigned long)mb->off);
            }
        }
        db_u8(b, 0);                      /* end of the members */
        type_record(m, t, off);
        for (int i = 0; i < m->nfix; i++)
            if (m->fix[i].to == t) {
                db_patch_u32(b, m->fix[i].at, (unsigned long)off);
                m->fix[i].to = NULL;
            }
        return off;
    }
    return -1;
}

/* DW_AT_location = DW_OP_fbreg(off): the slot at frame_base(=rbp) + off. */
/* DWARF address width. A 32-bit target's .debug_info and .debug_line
 * carry four-byte addresses and say so in the CU header's
 * address_size; emitting eight everywhere was fine while every target
 * was 64-bit and is simply wrong on ARMv7-M and RV32 -- a debugger
 * reads the next field out of the second half of the address. */
/* AVR's pointer is two bytes, and its DWARF addresses are four, as
 * avr-gcc writes them: a code address is a BYTE address in program
 * memory, past 64K on the larger parts (an ATmega2560 has 256K of
 * flash), and a data address is 0x800000 up (AVR_DATA_SPACE), so neither
 * fits the pointer. Two-byte addresses were also simply wrong here: this
 * wrote EIGHT bytes for anything that was not four, so an AVR CU said
 * address_size 2 and carried eight-byte fields, and a reader lost the
 * thread at the first one -- the whole unit decoded as empty. */
static int addr_bytes(void)
{
    if (target_get() == TARGET_AVR) return 4;
    return target_ptr_size();
}

static void db_addr(struct dbuf *b, unsigned long v)
{
    if (addr_bytes() == 2)      db_u16(b, (unsigned)v);
    else if (addr_bytes() == 4) db_u32(b, v);
    else                        db_u64(b, v);
}

static void loc_fbreg(struct dbuf *b, long off)
{
    struct dbuf e = { 0, 0, 0 };
    /* No location: an EMPTY expression, which DWARF defines as "present
     * in the source, not in the object code" (DWARF 4, 2.6.1.1.4) -- an
     * optimized-out variable, and not a slot nothing writes. */
    if (off == IR_VAR_NO_LOC) {
        db_uleb(b, 0);
        return;
    }
    db_u8(&e, DW_OP_fbreg);
    db_sleb(&e, off);
    db_uleb(b, (unsigned long)e.len);
    for (int i = 0; i < e.len; i++) db_u8(b, e.p[i]);
    free(e.p);
}

/* The unit's global variables, as CU-level DW_TAG_variable DIEs located
 * by DW_OP_addr -- a relocation against the object's own symbol, so the
 * address is wherever the linker puts it. Without them a debugger had a
 * global's address from the symbol table and nothing to say what it was.
 *
 * Only what this unit DEFINES (an extern is described by the unit that
 * owns it), not thread-local (its address is per thread, an expression
 * this writer does not have), and only a type this writer can describe
 * -- the rule ensure_type keeps: no confidently-wrong type. A function's
 * static local is a global too, under a compiler-made name with a dot,
 * and is left out: its name is not one the programmer could ask for. */
static void emit_globals(struct dwarf_out *out, struct dbuf *b,
                         struct typemap *tm, struct ir_unit *iu)
{
    if (!iu->src)
        return;
    for (struct global *g = iu->src->globals; g; g = g->next) {
        if (g->absorbed || !g->defined || g->is_tls || !g->name ||
            strchr(g->name, '.'))
            continue;
        int toff = ensure_type(b, tm, g->ty);
        if (toff < 0)
            continue;
        db_uleb(b, AB_VAR_T);
        db_str(b, g->name);
        db_u32(b, (unsigned long)toff);
        long dsp = target_get() == TARGET_AVR ? AVR_DATA_SPACE : 0;
        if (dw_split) {
            struct dbuf e = { 0, 0, 0 };
            db_u8(&e, DW_OP_addrx);
            db_uleb(&e, (unsigned long)addr_index(DWTGT_GLOBAL, dsp, g, 0));
            db_uleb(b, (unsigned long)e.len);
            for (int k = 0; k < e.len; k++) db_u8(b, e.p[k]);
            free(e.p);
            continue;
        }
        db_uleb(b, (unsigned long)(1 + addr_bytes()));
        db_u8(b, DW_OP_addr);
        reloc(out, DWSEC_INFO, b->len, addr_bytes(), DWTGT_GLOBAL, dsp);
        out->relocs[out->nrelocs - 1].glob = g;
        db_addr(b, 0);
    }
}

/* --- .debug_info: a compile_unit with children — the type DIEs, then one
 * subprogram DIE per function carrying its parameters and locals. --- */
/* One location's expression: a register, a frame-base offset, or a
 * register pair in pieces. */
static void loc_expr(struct dbuf *e, const struct ir_dbgloc *l)
{
    int r[2] = { l->val, l->val2 };
    switch (l->kind) {
    case IR_LOC_FB:
        db_u8(e, DW_OP_fbreg);
        db_sleb(e, l->val);
        break;
    case IR_LOC_REGRUN:
        for (int k = 0; k < l->val2; k++) {
            int rk = l->val + k;
            if (rk < 32) {
                db_u8(e, DW_OP_reg0 + (unsigned)rk);
            } else {
                db_u8(e, DW_OP_regx);
                db_uleb(e, (unsigned long)rk);
            }
            if (l->val2 > 1) {
                db_u8(e, DW_OP_piece);
                db_uleb(e, (unsigned long)l->piece);
            }
        }
        break;
    case IR_LOC_REG:
    case IR_LOC_REGPAIR:
        for (int k = 0; k < (l->kind == IR_LOC_REGPAIR ? 2 : 1); k++) {
            if (r[k] < 32) {
                db_u8(e, DW_OP_reg0 + (unsigned)r[k]);
            } else {
                db_u8(e, DW_OP_regx);
                db_uleb(e, (unsigned long)r[k]);
            }
            if (l->kind == IR_LOC_REGPAIR) {
                db_u8(e, DW_OP_piece);
                db_uleb(e, (unsigned long)l->piece);
            }
        }
        break;
    }
}

/* A variable's location list, at the end of `b` (.debug_loclists or
 * .debug_loc): the function's start as the base, then each range as
 * offsets from it. */
static void emit_loclist(struct dwarf_out *out, struct dbuf *b,
                         const struct ir_dbgloc *l, int n, long base)
{
    if (dw_split) {
        db_u8(b, DW_LLE_base_addressx);
        db_uleb(b, (unsigned long)addr_index(DWTGT_TEXT, base, NULL, 0));
    } else if (dw_ver >= 5) {
        db_u8(b, DW_LLE_base_address);
        reloc(out, DWSEC_LOC, b->len, addr_bytes(), DWTGT_TEXT, base);
        db_addr(b, 0);
    } else {
        /* a base address selection entry: the largest address, then
         * the base */
        db_addr(b, addr_bytes() == 8 ? ~0UL : addr_bytes() == 4
                                            ? 0xffffffffUL : 0xffffUL);
        reloc(out, DWSEC_LOC, b->len, addr_bytes(), DWTGT_TEXT, base);
        db_addr(b, 0);
    }
    for (int k = 0; k < n; k++) {
        struct dbuf e = { 0, 0, 0 };
        loc_expr(&e, &l[k]);
        if (dw_ver >= 5) {
            db_u8(b, DW_LLE_offset_pair);
            db_uleb(b, (unsigned long)(l[k].lo - base));
            db_uleb(b, (unsigned long)(l[k].hi - base));
            db_uleb(b, (unsigned long)e.len);
        } else {
            db_addr(b, (unsigned long)(l[k].lo - base));
            db_addr(b, (unsigned long)(l[k].hi - base));
            db_u16(b, (unsigned)e.len);
        }
        for (int i = 0; i < e.len; i++)
            db_u8(b, e.p[i]);
        free(e.p);
    }
    if (dw_ver >= 5) {
        db_u8(b, DW_LLE_end_of_list);
    } else {
        db_addr(b, 0);
        db_addr(b, 0);
    }
}

/* .debug_rnglists' and .debug_loclists' header (7.28, 7.29): length,
 * version, address size, no segment selector, no offset table. */
static int list_header(struct dbuf *b)
{
    int at = b->len;
    db_u32(b, 0);                        /* unit_length — backpatched */
    db_u16(b, 5);
    db_u8(b, (unsigned char)addr_bytes());
    db_u8(b, 0);
    db_u32(b, 0);
    return at;
}

static void emit_info(struct dwarf_out *out, struct dbuf *b,
                      struct dbuf *loc,
                      struct ir_unit *iu, const char *filename,
                      long text_lo, long text_hi, int split)
{
    int len_at = b->len;
    db_u32(b, 0);                        /* unit_length — backpatched */
    int after_len = b->len;
    db_u16(b, (unsigned)dw_ver);         /* version */
    if (dw_ver >= 5) {
        /* DWARF 5: the unit's type, then address_size before the
         * abbreviations' offset (7.5.1.1) */
        db_u8(b, dw_split ? DW_UT_split_compile : DW_UT_compile);
        db_u8(b, (unsigned char)addr_bytes());
    }
    if (!dw_split)
        reloc(out, DWSEC_INFO, b->len, 4, DWTGT_ABBREV, 0);
    db_u32(b, 0);                        /* debug_abbrev_offset (reloc) */
    if (dw_ver < 5)
        db_u8(b, (unsigned char)addr_bytes());   /* address_size */
    if (dw_split)
        db_u64(b, 0);                    /* dwo_id, patched (emit_all) */

    db_uleb(b, split && !dw_split ? AB_CU_RANGES : AB_CU);
    db_str(b, "EmbCC");                  /* DW_AT_producer */
    db_u16(b, DW_LANG_C99);              /* DW_AT_language */
    db_str(b, filename);                 /* DW_AT_name */
    db_str(b, ".");                      /* DW_AT_comp_dir (deterministic) */
    if (dw_split) {
        /* (the skeleton has the unit's extent and its line table) */
    } else if (split) {
        db_addr(b, 0);                    /* DW_AT_low_pc: base 0 */
        /* (.debug_rnglists' list is past its 12-byte header) */
        reloc(out, DWSEC_INFO, b->len, 4, DWTGT_RANGES, dw_ver >= 5 ? 12 : 0);
        db_u32(b, 0);                     /* DW_AT_ranges (reloc) */
    } else {
        reloc(out, DWSEC_INFO, b->len, addr_bytes(), DWTGT_TEXT, text_lo);
        db_addr(b, 0);                    /* DW_AT_low_pc (reloc) */
        reloc_end(out, DWSEC_INFO, b->len, addr_bytes(), text_hi);
        db_addr(b, 0);                    /* DW_AT_high_pc (reloc) */
    }
    if (!dw_split) {
        reloc(out, DWSEC_INFO, b->len, 4, DWTGT_LINE, 0);
        db_u32(b, 0);                    /* DW_AT_stmt_list (reloc) */
    }

    /* Every variable's type, emitted up front as CU children so the
     * subprogram DIEs below can reference them (a DIE can't be emitted in the
     * middle of another DIE's child list). */
    struct typemap tm;
    memset(&tm, 0, sizeof tm);
    for (int n = 0; n < iu->nfuncs; n++) {
        (void)ensure_type(b, &tm, iu->funcs[n].src->ret_ty);
        for (int v = 0; v < iu->funcs[n].ndbgvars; v++)
            (void)ensure_type(b, &tm, iu->funcs[n].dbgvars[v].ty);
    }
    emit_globals(out, b, &tm, iu);

    for (int n = 0; n < iu->nfuncs; n++) {
        struct ir_func *fn = &iu->funcs[n];
        if (fn->src->code_len <= 0) continue;
        /* the function starts at its entry, past an Xtensa literal pool
         * (code_entry, 0 elsewhere) */
        long lo = fn->src->code_off + fn->src->code_entry;
        long hi = fn->src->code_off + fn->src->code_len;

        int rtoff = type_lookup(&tm, fn->src->ret_ty);
        db_uleb(b, rtoff >= 0 ? AB_SUBPROGRAM_T : AB_SUBPROGRAM);
        db_str(b, fn->name);
        if (rtoff >= 0) db_u32(b, (unsigned long)rtoff);  /* return type */
        if (dw_split) {
            db_uleb(b, (unsigned long)addr_index(DWTGT_TEXT, lo, NULL, 0));
            db_u32(b, (unsigned long)(hi - lo));     /* high_pc: a length */
        } else {
            reloc(out, DWSEC_INFO, b->len, addr_bytes(), DWTGT_TEXT, lo);
            db_addr(b, 0);               /* low_pc */
            reloc_end(out, DWSEC_INFO, b->len, addr_bytes(), hi);
            db_addr(b, 0);               /* high_pc */
        }
        /* frame_base. x86-64 and aarch64 set a frame pointer in the
         * prologue and never move it, so it is one byte and no offset.
         *
         * ARMv7-M and RISC-V keep no frame pointer at all -- the
         * prologue subtracts the frame from sp once and everything,
         * including the outgoing argument area, is addressed from
         * there -- so their frame base is `sp + 0`, which is exact for
         * the same reason: sp does not move for the life of the body.
         * A function whose sp DOES move (alloca, a VLA, a local aligned
         * past the stack) copies sp into r7 or s0 once the frame is
         * built and addresses every slot from that copy, at the same
         * offsets, so that register is its frame base. It was sp here
         * too -- the comment claimed the backend refused such a function
         * at -g, and nothing did -- so every location was wrong from
         * the first allocation on. */
        {
            enum target_arch a = target_get();
            if (a == TARGET_AVR) {
                /* Y, which the AVR backend always uses as its frame
                 * pointer (a VLA it refuses): breg28 + 0 */
                db_uleb(b, 2);
                db_u8(b, DW_OP_breg(DW_REG_FB_AVR));
                db_u8(b, 0);
            } else if (a == TARGET_COLDFIRE) {
                /* the slots are a6-relative in every function */
                db_uleb(b, 2);
                db_u8(b, DW_OP_breg(DW_REG_FB_M68K));
                db_u8(b, 0);
            } else if (a == TARGET_SPARC32) {
                /* SPARC: every slot is addressed from %fp, which never
                 * moves (src/arch/sparc/codegen.c), alloca or not */
                db_uleb(b, 2);
                db_u8(b, DW_OP_breg(DW_REG_FP_SPARC));
                db_u8(b, 0);
            } else if (a == TARGET_THUMB || a == TARGET_RISCV32 ||
                a == TARGET_RISCV64 || a == TARGET_MIPS32 ||
                a == TARGET_LOONGARCH64 || a == TARGET_TRICORE ||
                a == TARGET_XTENSA || a == TARGET_PPC32 ||
                a == TARGET_RX || a == TARGET_MIPS64) {
                struct dbuf e = { 0, 0, 0 };
                int thumb = a == TARGET_THUMB, mips = a == TARGET_MIPS32 ||
                                                a == TARGET_MIPS64;
                int la = a == TARGET_LOONGARCH64, tc = a == TARGET_TRICORE;
                int xt = a == TARGET_XTENSA, ppc = a == TARGET_PPC32;
                int rx = a == TARGET_RX;
                db_u8(&e, DW_OP_breg(fn->has_alloca
                                     ? (thumb ? DW_REG_FB_ARM
                                        : mips ? DW_REG_FB_MIPS
                                        : la ? DW_REG_FB_LA
                                        : tc ? DW_REG_FB_TRICORE
                                        : xt ? DW_REG_FB_XTENSA
                                        : ppc ? DW_REG_FB_PPC
                                        : rx ? DW_REG_FB_RX
                                             : DW_REG_FB_RISCV)
                                     : (thumb ? DW_REG_SP_ARM
                                        : mips ? DW_REG_SP_MIPS
                                        : la ? DW_REG_SP_LA
                                        : tc ? DW_REG_SP_TRICORE
                                        : xt ? DW_REG_SP_XTENSA
                                        : ppc ? DW_REG_SP_PPC
                                        : rx ? DW_REG_SP_RX
                                             : DW_REG_SP_RISCV)));
                db_sleb(&e, 0);
                db_uleb(b, (unsigned long)e.len);
                for (int k = 0; k < e.len; k++) db_u8(b, e.p[k]);
                free(e.p);
            } else {
                db_uleb(b, 1);
                db_u8(b, a == TARGET_AARCH64 ? DW_OP_reg29 : DW_OP_reg6);
            }
        }

        for (int v = 0; v < fn->ndbgvars; v++) {
            struct ir_dbgvar *dv = &fn->dbgvars[v];
            int toff = type_lookup(&tm, dv->ty);
            /* a variable the optimizer took out of memory: where it is,
             * range by range (ra_debug_locs) */
            int ll = fn->ndbglocs && fn->ndbglocs[v] > 0 &&
                     (fn->var_off[dv->vreg] == IR_VAR_NO_LOC || dv->moved);
            int code = dv->is_param
                ? (toff >= 0 ? (ll ? AB_PARAM_T_LL : AB_PARAM_T)
                             : (ll ? AB_PARAM_LL : AB_PARAM))
                : (toff >= 0 ? (ll ? AB_VAR_T_LL : AB_VAR_T)
                             : (ll ? AB_VAR_LL : AB_VAR));
            db_uleb(b, code);
            db_str(b, dv->name);
            if (toff >= 0) db_u32(b, (unsigned long)toff);
            if (ll && dw_split) {
                if (dw_nll == dw_capll) {
                    dw_capll = dw_capll ? dw_capll * 2 : 16;
                    dw_lloff = xrealloc(dw_lloff,
                                        (size_t)dw_capll * sizeof *dw_lloff);
                }
                dw_lloff[dw_nll] = loc->len;
                db_uleb(b, (unsigned long)dw_nll++);
                emit_loclist(out, loc, fn->dbglocs[v], fn->ndbglocs[v], lo);
            } else if (ll) {
                reloc(out, DWSEC_INFO, b->len, 4, DWTGT_LOC, loc->len);
                db_u32(b, 0);
                emit_loclist(out, loc, fn->dbglocs[v], fn->ndbglocs[v], lo);
            } else {
                loc_fbreg(b, fn->var_off[dv->vreg]);
            }
        }
        db_u8(b, 0);                     /* end of this subprogram's children */
    }
    db_u8(b, 0);                         /* end of the CU's children */

    free(tm.k); free(tm.off); free(tm.fix);

    unsigned long ulen = (unsigned long)(b->len - after_len);
    db_patch_u32(b, len_at, ulen);
}

/* ---- call frame information (.debug_frame) --------------------------
 *
 * What lets a debugger unwind: for every function, where the caller's
 * frame is (the CFA) and where the return address and each saved
 * register went, from the prologue steps the backend recorded
 * (ir_cfi_add). Without it gdb guessed by reading the prologue's
 * instructions, and lost the thread at a Cortex-M4's vpush.
 *
 * One CIE: at a function's entry the CFA is the stack pointer and the
 * return address is still in its register (the backend registry's
 * frame_sp and frame_ra). Then one FDE per function, advancing to each
 * recorded step. Version 1 (what gcc and clang write in .debug_frame),
 * code alignment 1 and data alignment -1, so every offset is in bytes. A
 * function that recorded nothing -- a leaf that saves nothing -- still has
 * an FDE: the CIE's rule is right for all of it. */
#define DW_CFA_advance_loc1     0x02
#define DW_CFA_advance_loc2     0x03
#define DW_CFA_advance_loc4     0x04
#define DW_CFA_offset_extended  0x05
#define DW_CFA_def_cfa          0x0c
#define DW_CFA_def_cfa_offset   0x0e
#define DW_CFA_offset_extended_sf 0x11
#define DW_CFA_nop              0x00

static void frame_pad(struct dbuf *b, int start)
{
    while ((b->len - start) % addr_bytes())
        db_u8(b, DW_CFA_nop);
    db_patch_u32(b, start, (unsigned long)(b->len - start - 4));
}

/* AVR's return address size: 2 bytes, 3 on avr6 (a compiler built
 * without AVR has no part to ask) */
static int avr_pc_bytes(void)
{
#if FAMILY_BUILT(avr)
    const struct avr_mcu *m = target_avr_mcu();
    return m ? m->pc_bytes : 2;
#else
    return 2;
#endif
}

static void emit_frame(struct dwarf_out *out, struct dbuf *b,
                       struct ir_unit *iu)
{
    if (!out->frame_sp)
        return;
    int cie = b->len;
    db_u32(b, 0);                        /* length, patched */
    db_u32(b, 0xffffffffUL);             /* CIE_id */
    db_u8(b, 1);                         /* version */
    db_u8(b, 0);                         /* augmentation "" */
    db_uleb(b, 1);                       /* code_alignment_factor */
    db_sleb(b, -1);                      /* data_alignment_factor */
    db_u8(b, (unsigned)out->frame_ra);   /* return_address_register */
    if (target_get() == TARGET_AVR) {
        /* AVR, as avr-gcc writes it: a call has PUSHED the return
         * address (2 bytes, 3 on avr6), so at entry the CFA is SP + that
         * many and the return address is in memory just below it --
         * column 36 at CFA - (size - 1), its high byte at the lowest
         * address. Y (r28) is the CFA's base once the prologue sets it. */
        int pc = avr_pc_bytes();
        db_u8(b, DW_CFA_def_cfa);
        db_uleb(b, (unsigned long)out->frame_sp);
        db_uleb(b, (unsigned long)pc);
        db_u8(b, 0x80 | (unsigned)out->frame_ra);    /* DW_CFA_offset */
        db_uleb(b, (unsigned long)(pc - 1));
    } else {
        db_u8(b, DW_CFA_def_cfa);        /* at entry: CFA = sp + 0 */
        db_uleb(b, (unsigned long)out->frame_sp);
        db_uleb(b, 0);
    }
    frame_pad(b, cie);

    for (int n = 0; n < iu->nfuncs; n++) {
        struct ir_func *fn = &iu->funcs[n];
        struct func *f = fn->src;
        /* ncfi -1: a function the backend cannot describe (an interrupt
         * handler's own prologue) -- no FDE, rather than a wrong one */
        if (!f || f->code_len <= 0 || fn->ncfi < 0)
            continue;
        long lo = f->code_off + f->code_entry;
        int fde = b->len;
        db_u32(b, 0);                    /* length, patched */
        reloc(out, DWSEC_FRAME, b->len, 4, DWTGT_FRAME, cie);
        db_u32(b, 0);                    /* CIE_pointer */
        reloc(out, DWSEC_FRAME, b->len, addr_bytes(), DWTGT_TEXT, lo);
        db_addr(b, 0);                   /* initial_location */
        db_addr(b, (unsigned long)(f->code_len - f->code_entry));
        long at = 0, cfa_reg = out->frame_sp, cfa = 0;
        if (target_get() == TARGET_AVR) {      /* the CIE's CFA, SP + pc */
            cfa = avr_pc_bytes();
        }
        for (int k = 0; k < fn->ncfi; k++) {
            const struct ir_cfi *c = &fn->cfi[k];
            long to = c->off - f->code_entry, d = to - at;
            if (d > 0) {
                if (d < 64)         db_u8(b, 0x40 | (unsigned)d);
                else if (d < 256)   { db_u8(b, DW_CFA_advance_loc1); db_u8(b, (unsigned)d); }
                else if (d < 65536) { db_u8(b, DW_CFA_advance_loc2);
                                      db_u8(b, (unsigned)(d & 0xff));
                                      db_u8(b, (unsigned)(d >> 8)); }
                else                { db_u8(b, DW_CFA_advance_loc4); db_u32(b, (unsigned long)d); }
                at = to;
            }
            switch (c->kind) {
            case IR_CFI_CFA_OFFSET:
                cfa = c->val;
                db_u8(b, DW_CFA_def_cfa_offset);
                db_uleb(b, (unsigned long)cfa);
                break;
            case IR_CFI_CFA_REG:
                cfa_reg = c->reg;
                cfa = c->val;
                db_u8(b, DW_CFA_def_cfa);
                db_uleb(b, (unsigned long)cfa_reg);
                db_uleb(b, (unsigned long)cfa);
                break;
            case IR_CFI_SAVED:
                /* data alignment -1: the factored offset is -val */
                if (c->val <= 0 && c->reg < 64) {
                    db_u8(b, 0x80 | (unsigned)c->reg);
                    db_uleb(b, (unsigned long)-c->val);
                } else if (c->val <= 0) {
                    db_u8(b, DW_CFA_offset_extended);
                    db_uleb(b, (unsigned long)c->reg);
                    db_uleb(b, (unsigned long)-c->val);
                } else {
                    db_u8(b, DW_CFA_offset_extended_sf);
                    db_uleb(b, (unsigned long)c->reg);
                    db_sleb(b, -c->val);
                }
                break;
            }
        }
        (void)cfa_reg;
        frame_pad(b, fde);
    }
}

/* A row at (addr, line), from the state machine at (*cur_addr, *cur_line),
 * as gcc and clang encode one (LLVM's MCDwarfLineAddr::encode): a line
 * advance outside a special opcode's -5..+8 is an advance_line of its own;
 * then one special opcode when the address advance fits in it, or
 * const_add_pc and one when it fits in the two, or advance_pc and one; a
 * row that moves neither is a copy. prologue_end is set first: the row
 * the special opcode (or copy) appends carries it, and clears it. */
static void line_row(struct dbuf *b, long *cur_addr, long *cur_line,
                     long addr, long line, int prologue_end)
{
    long dl = line - *cur_line, da = addr - *cur_addr;
    if (dl < LINE_BASE || dl >= LINE_BASE + LINE_RANGE) {
        db_u8(b, DW_LNS_advance_line);
        db_sleb(b, dl);
        dl = 0;
    }
    if (prologue_end)
        db_u8(b, DW_LNS_set_prologue_end);
    *cur_addr = addr;
    *cur_line = line;
    if (dl == 0 && da == 0) {
        db_u8(b, DW_LNS_copy);
        return;
    }
    long op = dl - LINE_BASE + OPCODE_BASE;     /* no address advance */
    if (da >= 0 && op + da * LINE_RANGE <= 255) {
        db_u8(b, (int)(op + da * LINE_RANGE));
        return;
    }
    if (da >= MAX_SPECIAL_ADDR &&
        op + (da - MAX_SPECIAL_ADDR) * LINE_RANGE <= 255) {
        db_u8(b, DW_LNS_const_add_pc);
        db_u8(b, (int)(op + (da - MAX_SPECIAL_ADDR) * LINE_RANGE));
        return;
    }
    db_u8(b, DW_LNS_advance_pc);
    db_uleb(b, (unsigned long)da);
    db_u8(b, (int)op);
}

/* One function's rows, bracketed by set_address .. end_sequence. Offsets are
 * .text-relative (row.off and code_off share that space), so pc deltas need
 * no knowledge of the final load address. */
static void emit_line_func(struct dwarf_out *out, struct dbuf *b,
                           struct ir_func *fn)
{
    long lo = fn->src->code_off + fn->src->code_entry;
    long hi = fn->src->code_off + fn->src->code_len;

    /* DW_LNE_set_address <.text address, relocated, 4 or 8 bytes> */
    db_u8(b, 0); db_uleb(b, (unsigned long)(addr_bytes() + 1));
    db_u8(b, DW_LNE_set_address);
    reloc(out, DWSEC_LINE, b->len, addr_bytes(), DWTGT_TEXT, lo);
    db_addr(b, 0);

    long cur_addr = lo, cur_line = 1;
    /* A row at the entry, with the line the function was declared on, as
     * gcc and clang write it, and `prologue_end` on the first row of the
     * body. A debugger sets `break f` past the prologue -- where the
     * parameters are where the DWARF says -- by that flag, or, an older
     * one, at the second row; with the body's first row as the first,
     * gdb scanned the prologue itself and stopped a statement late. */
    int entry_row = fn->line > 0 && (fn->nlines == 0 || fn->lines[0].off > lo);
    if (entry_row)
        line_row(b, &cur_addr, &cur_line, lo, fn->line, 0);
    int body = 0;                        /* prologue_end is not yet said */
    for (int r = 0; r < fn->nlines; r++) {
        long off = fn->lines[r].off, line = fn->lines[r].line;
        /* the body's first row: past the entry row, or the entry itself
         * when there is no prologue */
        int pe = !body && (off > lo || !entry_row);
        if (pe)
            body = 1;
        line_row(b, &cur_addr, &cur_line, off, line, pe);
    }
    /* advance to the function end and close the sequence */
    if (hi != cur_addr) {
        db_u8(b, DW_LNS_advance_pc);
        db_uleb(b, (unsigned long)(hi - cur_addr));
    }
    db_u8(b, 0); db_uleb(b, 1); db_u8(b, DW_LNE_end_sequence);
}

static void emit_line(struct dwarf_out *out, struct dbuf *b,
                      struct ir_unit *iu, const char *filename)
{
    int len_at = b->len;
    db_u32(b, 0);                        /* unit_length — backpatched */
    int after_len = b->len;
    db_u16(b, (unsigned)dw_ver);         /* version */
    if (dw_ver >= 5) {
        db_u8(b, (unsigned char)addr_bytes());  /* address_size */
        db_u8(b, 0);                     /* segment_selector_size */
    }
    int hdr_len_at = b->len;
    db_u32(b, 0);                        /* header_length — backpatched */
    int after_hdr_len = b->len;
    db_u8(b, 1);                         /* minimum_instruction_length */
    db_u8(b, 1);                         /* maximum_operations_per_instruction */
    db_u8(b, 1);                         /* default_is_stmt */
    db_u8(b, (unsigned char)LINE_BASE);  /* line_base (signed) */
    db_u8(b, LINE_RANGE);                /* line_range */
    db_u8(b, OPCODE_BASE);               /* opcode_base */
    /* standard_opcode_lengths for opcodes 1..12 */
    static const unsigned char oplen[12] =
        { 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1 };
    for (int i = 0; i < 12; i++) db_u8(b, oplen[i]);
    if (dw_ver >= 5) {
        /* DWARF 5 (6.2.4.1): each table says its entries' form. One
         * directory, the compilation's; files 0 (the unit) and 1 (what
         * the program's file register starts at) both the source, as
         * GCC writes them. */
        db_u8(b, 1);                     /* directory_entry_format_count */
        db_uleb(b, DW_LNCT_path); db_uleb(b, DW_FORM_string);
        db_uleb(b, 1);                   /* directories_count */
        db_str(b, ".");
        db_u8(b, 2);                     /* file_name_entry_format_count */
        db_uleb(b, DW_LNCT_path); db_uleb(b, DW_FORM_string);
        db_uleb(b, DW_LNCT_directory_index); db_uleb(b, DW_FORM_udata);
        db_uleb(b, 2);                   /* file_names_count */
        for (int k = 0; k < 2; k++) {
            db_str(b, filename);
            db_uleb(b, 0);
        }
    } else {
        db_u8(b, 0);                     /* include_directories: none */
        /* file_names: one entry, the source file */
        db_str(b, filename);
        db_uleb(b, 0);                   /* dir index */
        db_uleb(b, 0);                   /* mtime (deterministic) */
        db_uleb(b, 0);                   /* size */
        db_u8(b, 0);                     /* end of file_names */
    }

    /* backpatch header_length (bytes from here to end of header) */
    unsigned long hlen = (unsigned long)(b->len - after_hdr_len);
    db_patch_u32(b, hdr_len_at, hlen);

    for (int n = 0; n < iu->nfuncs; n++)
        if (iu->funcs[n].src->code_len > 0)
            emit_line_func(out, b, &iu->funcs[n]);

    unsigned long ulen = (unsigned long)(b->len - after_len);
    db_patch_u32(b, len_at, ulen);
}

/* -gsplit-dwarf: the .dwo's sections from what emit_info and emit_abbrev
 * made (ab, in, lc), and in their place the object's skeleton unit and
 * abbreviation, and .debug_addr. A dwo_id ties the two units: a hash of
 * the .dwo unit's DIEs, deterministic as everything here. */
static void emit_split(struct dwarf_out *out, struct dbuf *ab,
                       struct dbuf *in, struct dbuf *lc,
                       const char *filename, long lo, long hi, int ranges)
{
    unsigned long id = 1469598103934665603UL;
    for (int k = 20; k < in->len; k++)
        id = (id ^ in->p[k]) * 1099511628211UL;
    for (const char *q = filename; *q; q++)
        id = (id ^ (unsigned char)*q) * 1099511628211UL;
    /* unit_length 4, version 2, unit_type 1, address_size 1, abbrev 4 */
    db_patch_u32(in, 0, (unsigned long)(in->len - 4));
    struct dbuf idb = { 0, 0, 0 };
    db_u64(&idb, id);
    memcpy(in->p + 12, idb.p, 8);
    out->sec[DWSEC_INFO_DWO] = in->p;   out->seclen[DWSEC_INFO_DWO] = in->len;
    out->sec[DWSEC_ABBREV_DWO] = ab->p; out->seclen[DWSEC_ABBREV_DWO] = ab->len;
    if (dw_nll) {
        /* .debug_loclists.dwo: its header, an offset per list (from the
         * end of the offsets, which DW_FORM_loclistx numbers), the lists */
        struct dbuf l = { 0, 0, 0 };
        db_u32(&l, 0);
        db_u16(&l, 5);
        db_u8(&l, (unsigned char)addr_bytes());
        db_u8(&l, 0);
        db_u32(&l, (unsigned long)dw_nll);
        for (int k = 0; k < dw_nll; k++)
            db_u32(&l, (unsigned long)(dw_nll * 4 + dw_lloff[k]));
        for (int k = 0; k < lc->len; k++)
            db_u8(&l, lc->p[k]);
        db_patch_u32(&l, 0, (unsigned long)(l.len - 4));
        out->sec[DWSEC_LOC_DWO] = l.p;
        out->seclen[DWSEC_LOC_DWO] = l.len;
    }
    free(lc->p);
    lc->p = NULL;
    /* .debug_addr: its header (7.27), then each address, relocated */
    struct dbuf ad = { 0, 0, 0 };
    db_u32(&ad, 0);
    db_u16(&ad, 5);
    db_u8(&ad, (unsigned char)addr_bytes());
    db_u8(&ad, 0);
    for (int k = 0; k < dw_naddr; k++) {
        reloc(out, DWSEC_ADDR, ad.len, addr_bytes(), dw_addrs[k].target,
              dw_addrs[k].addend);
        out->relocs[out->nrelocs - 1].glob = dw_addrs[k].g;
        out->relocs[out->nrelocs - 1].end = dw_addrs[k].end;
        db_addr(&ad, 0);
    }
    db_patch_u32(&ad, 0, (unsigned long)(ad.len - 4));
    out->sec[DWSEC_ADDR] = ad.p;
    out->seclen[DWSEC_ADDR] = ad.len;
    /* the skeleton's abbreviation and unit */
    struct dbuf sa = { 0, 0, 0 }, sk = { 0, 0, 0 };
    unsigned char sk_pairs[] = {
        DW_AT_comp_dir, DW_FORM_string, DW_AT_dwo_name, DW_FORM_string,
        DW_AT_low_pc, DW_FORM_addr, DW_AT_high_pc, DW_FORM_addr,
        DW_AT_stmt_list, DW_FORM_sec_offset, DW_AT_addr_base,
        DW_FORM_sec_offset };
    if (ranges)
        sk_pairs[7] = DW_FORM_sec_offset, sk_pairs[6] = DW_AT_ranges;
    one_abbrev(&sa, 1, DW_TAG_skeleton_unit, DW_CHILDREN_no, sk_pairs, 6);
    db_uleb(&sa, 0);
    db_u32(&sk, 0);
    db_u16(&sk, 5);
    db_u8(&sk, DW_UT_skeleton);
    db_u8(&sk, (unsigned char)addr_bytes());
    reloc(out, DWSEC_INFO, sk.len, 4, DWTGT_ABBREV, 0);
    db_u32(&sk, 0);
    db_u64(&sk, id);
    db_uleb(&sk, 1);
    db_str(&sk, ".");
    db_str(&sk, dw_dwo);
    if (ranges) {
        db_addr(&sk, 0);
        reloc(out, DWSEC_INFO, sk.len, 4, DWTGT_RANGES, 12);
        db_u32(&sk, 0);
    } else {
        reloc(out, DWSEC_INFO, sk.len, addr_bytes(), DWTGT_TEXT, lo);
        db_addr(&sk, 0);
        reloc_end(out, DWSEC_INFO, sk.len, addr_bytes(), hi);
        db_addr(&sk, 0);
    }
    reloc(out, DWSEC_INFO, sk.len, 4, DWTGT_LINE, 0);
    db_u32(&sk, 0);
    reloc(out, DWSEC_INFO, sk.len, 4, DWTGT_ADDR, 8);
    db_u32(&sk, 0);
    db_patch_u32(&sk, 0, (unsigned long)(sk.len - 4));
    in->p = sk.p; in->len = sk.len;
    ab->p = sa.p; ab->len = sa.len;
}

static void emit_all(struct ir_unit *iu, const char *filename,
                     struct dwarf_out *out, int split)
{
    /* Bound the code: low = first function's offset (0 in practice),
     * high = the end of the last function. Covers exactly the code that can
     * carry line rows; file-scope asm (no line info) beyond it is simply not
     * attributed to this CU, which is correct. */
    long lo = 0, hi = 0;
    int seen = 0;
    for (int n = 0; n < iu->nfuncs; n++) {
        if (iu->funcs[n].src->code_len <= 0) continue;
        long f_lo = iu->funcs[n].src->code_off;
        long f_hi = f_lo + iu->funcs[n].src->code_len;
        if (!seen || f_lo < lo) lo = f_lo;
        if (!seen || f_hi > hi) hi = f_hi;
        seen = 1;
    }

    struct dbuf ab = { 0, 0, 0 }, in = { 0, 0, 0 }, ln = { 0, 0, 0 },
                rg = { 0, 0, 0 }, fr = { 0, 0, 0 }, lc = { 0, 0, 0 };
    dw_naddr = 0;
    dw_nll = 0;
    int lc_at = dw_ver >= 5 && !dw_split ? list_header(&lc) : -1;
    int lc_empty = lc.len;
    emit_abbrev(&ab);
    emit_frame(out, &fr, iu);
    emit_info(out, &in, &lc, iu, filename, lo, hi, split);
    emit_line(out, &ln, iu, filename);
    if (dw_split) {
        emit_split(out, &ab, &in, &lc, filename, lo, hi, split);
        lc.len = 0;                      /* (the .dwo's now) */
    } else if (lc.len == lc_empty) {
        lc.len = 0;                      /* no list: no section */
    } else if (lc_at >= 0) {
        db_patch_u32(&lc, lc_at, (unsigned long)(lc.len - lc_at - 4));
    }
    if (split) {
        /* the ranges: [begin, end) per function, each relocated into the
         * function's own section -- .debug_rnglists' start_end entries
         * (DWARF 5), or .debug_ranges' address pairs ended by two zeros */
        int rg_at = dw_ver >= 5 ? list_header(&rg) : -1;
        for (int n = 0; n < iu->nfuncs; n++) {
            if (iu->funcs[n].src->code_len <= 0) continue;
            long f_lo = iu->funcs[n].src->code_off;
            if (dw_ver >= 5)
                db_u8(&rg, DW_RLE_start_end);
            reloc(out, DWSEC_RANGES, rg.len, addr_bytes(), DWTGT_TEXT, f_lo);
            db_addr(&rg, 0);
            reloc_end(out, DWSEC_RANGES, rg.len, addr_bytes(),
                      f_lo + iu->funcs[n].src->code_len);
            db_addr(&rg, 0);
        }
        if (dw_ver >= 5) {
            db_u8(&rg, DW_RLE_end_of_list);
            db_patch_u32(&rg, rg_at, (unsigned long)(rg.len - rg_at - 4));
        } else {
            db_addr(&rg, 0);
            db_addr(&rg, 0);
        }
    }

    out->sec[DWSEC_ABBREV] = ab.p; out->seclen[DWSEC_ABBREV] = ab.len;
    out->sec[DWSEC_INFO]   = in.p; out->seclen[DWSEC_INFO]   = in.len;
    out->sec[DWSEC_LINE]   = ln.p; out->seclen[DWSEC_LINE]   = ln.len;
    out->sec[DWSEC_RANGES] = rg.p; out->seclen[DWSEC_RANGES] = rg.len;
    out->sec[DWSEC_FRAME]  = fr.p; out->seclen[DWSEC_FRAME]  = fr.len;
    out->sec[DWSEC_LOC]    = lc.p; out->seclen[DWSEC_LOC]    = lc.len;
}

void ir_cfi_add(struct ir_func *fn, int off, int kind, int reg, long val)
{
    if (fn->ncfi == fn->cficap) {
        fn->cficap = fn->cficap ? fn->cficap * 2 : 8;
        fn->cfi = xrealloc(fn->cfi, (size_t)fn->cficap * sizeof *fn->cfi);
    }
    struct ir_cfi *c = &fn->cfi[fn->ncfi++];
    c->off = off;
    c->kind = kind;
    c->reg = reg;
    c->val = val;
}

void dwarf_emit(struct ir_unit *iu, const char *filename,
                struct dwarf_out *out)
{
    filename = pmap_apply(filename, PMAP_DEBUG);   /* -fdebug-prefix-map */
    emit_all(iu, filename, out, 0);
}

void dwarf_emit_split(struct ir_unit *iu, const char *filename,
                      struct dwarf_out *out)
{
    emit_all(iu, filename, out, 1);
}

void dwarf_free(struct dwarf_out *out)
{
    for (int i = 0; i < DWARF_NSEC; i++) free(out->sec[i]);
    free(out->relocs);
}

/* ---- -gz: zlib (RFC 1950/1951) for the compressed debug sections ------
 *
 * One deflate block with the fixed Huffman codes: LZ77 matches found
 * through a hash of three bytes, chained back over the 32K window. Debug
 * sections are repetitive (the same attribute forms, the same few
 * opcodes), so this takes most of what a dynamic-code compressor would,
 * in a page of code -- and every reader of SHF_COMPRESSED reads it. */
struct zbits { struct dbuf b; unsigned long acc; int n; };

static void zput(struct zbits *z, unsigned long v, int n)  /* LSB first */
{
    z->acc |= v << z->n;
    z->n += n;
    while (z->n >= 8) {
        db_u8(&z->b, (unsigned)(z->acc & 0xff));
        z->acc >>= 8;
        z->n -= 8;
    }
}

static void zput_rev(struct zbits *z, unsigned v, int n)   /* a Huffman code */
{
    unsigned r = 0;
    for (int k = 0; k < n; k++)
        r |= ((v >> k) & 1u) << (n - 1 - k);
    zput(z, r, n);
}

static void zlit(struct zbits *z, int c)                   /* 0..287 */
{
    if (c < 144)      zput_rev(z, 0x30u + (unsigned)c, 8);
    else if (c < 256) zput_rev(z, 0x190u + (unsigned)(c - 144), 9);
    else if (c < 280) zput_rev(z, (unsigned)(c - 256), 7);
    else              zput_rev(z, 0xc0u + (unsigned)(c - 280), 8);
}

static const unsigned short z_lbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char z_lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
    5, 5, 5, 5, 0 };
static const unsigned short z_dbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char z_dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13 };

unsigned char *dwarf_zlib(const unsigned char *p, int n, int *outlen)
{
    enum { HBITS = 15, WIN = 32768, MAXCHAIN = 64 };
    struct zbits z;
    memset(&z, 0, sizeof z);
    db_u8(&z.b, 0x78);                   /* CMF: deflate, 32K window */
    db_u8(&z.b, 0x9c);                   /* FLG: (0x789c % 31 == 0) */
    zput(&z, 1, 1);                      /* BFINAL */
    zput(&z, 1, 2);                      /* BTYPE 01: fixed codes */
    int *head = xmalloc((size_t)(1 << HBITS) * sizeof *head);
    int *prev = xmalloc((size_t)(n ? n : 1) * sizeof *prev);
    for (int k = 0; k < (1 << HBITS); k++)
        head[k] = -1;
#define ZHASH(i) ((((unsigned)p[i] << 10) ^ ((unsigned)p[(i) + 1] << 5) ^ \
                   (unsigned)p[(i) + 2]) & ((1u << HBITS) - 1))
    int i = 0;
    while (i < n) {
        int best = 0, dist = 0;
        if (i + 2 < n) {
            unsigned h = ZHASH(i);
            int c = head[h], chain = 0;
            while (c >= 0 && i - c <= WIN && chain++ < MAXCHAIN) {
                int l = 0, max = n - i < 258 ? n - i : 258;
                while (l < max && p[c + l] == p[i + l])
                    l++;
                if (l > best) {
                    best = l;
                    dist = i - c;
                    if (l == max)
                        break;
                }
                c = prev[c];
            }
        }
        int adv = best >= 3 ? best : 1;
        if (best >= 3) {
            int lc = 0;
            while (lc < 28 && z_lbase[lc + 1] <= best)
                lc++;
            zlit(&z, 257 + lc);
            zput(&z, (unsigned long)(best - z_lbase[lc]), z_lext[lc]);
            int dc = 0;
            while (dc < 29 && z_dbase[dc + 1] <= dist)
                dc++;
            zput_rev(&z, (unsigned)dc, 5);
            zput(&z, (unsigned long)(dist - z_dbase[dc]), z_dext[dc]);
        } else {
            zlit(&z, p[i]);
        }
        for (int k = 0; k < adv; k++, i++)
            if (i + 2 < n) {
                unsigned h = ZHASH(i);
                prev[i] = head[h];
                head[h] = i;
            }
    }
#undef ZHASH
    zlit(&z, 256);                       /* end of block */
    if (z.n)
        zput(&z, 0, 8 - z.n);
    unsigned long a = 1, b = 0;          /* Adler-32, big-endian */
    for (int k = 0; k < n; k++) {
        a = (a + p[k]) % 65521;
        b = (b + a) % 65521;
    }
    unsigned long ad = (b << 16) | a;
    for (int k = 3; k >= 0; k--)
        db_u8(&z.b, (unsigned)((ad >> (8 * k)) & 0xff));
    free(head);
    free(prev);
    *outlen = z.b.len;
    return z.b.p;
}
