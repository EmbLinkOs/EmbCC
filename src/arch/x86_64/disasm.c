/* x86-64 instruction decoding — the inverse of src/arch/x86_64/emit.c.
 *
 * It lives here, beside the encoder, because it is compiler knowledge and
 * two things need it: `embcc -S`, which renders the bytes the backend
 * emitted (src/driver/asmout.c), and EmbDBG, which shows a person what a
 * program became. One decoder, as there is one encoder (R1) — a second
 * would disagree, and the case where it disagreed would be the one someone
 * was trying to understand.
 *
 * Lengths are the load-bearing property: get one wrong and every later
 * instruction desyncs, so unknown bytes stop as `.byte` and are never
 * guessed at. AT&T syntax, to read like objdump.
 */
#include "disasm.h"

#include <stdio.h>
#include <string.h>

static const char *REG8[16] = {"al","cl","dl","bl","spl","bpl","sil","dil",
    "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"};
static const char *REG16[16] = {"ax","cx","dx","bx","sp","bp","si","di",
    "r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"};
static const char *REG32[16] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi",
    "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
static const char *REG64[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
    "r8","r9","r10","r11","r12","r13","r14","r15"};
static const char *regname(int sz, int r)
{ return sz==1?REG8[r]:sz==2?REG16[r]:sz==4?REG32[r]:REG64[r]; }

static long rd_s32(const unsigned char *c)
{ return (long)(int)((unsigned)c[0]|(c[1]<<8)|(c[2]<<16)|((unsigned)c[3]<<24)); }

/* Decode a ModRM (and any SIB/displacement) at c[*i] into an AT&T operand in
 * rm[]; return the reg field (0-15, REX.R-extended). isxmm makes a register
 * r/m an xmm. rip-relative and disp formatting follow objdump. */
static int modrm(const unsigned char *c, int *i, int rex, int sz, int isxmm, char *rm)
{
    unsigned char b = c[(*i)++];
    int mod = b >> 6, reg = ((b >> 3) & 7) | ((rex & 4) ? 8 : 0), rmf = b & 7;
    if (mod == 3) {
        int r = rmf | ((rex & 1) ? 8 : 0);
        if (isxmm) sprintf(rm, "%%xmm%d", r);
        else sprintf(rm, "%%%s", regname(sz, r));
        return reg;
    }
    long disp = 0; int base = rmf | ((rex & 1) ? 8 : 0), index = -1, scale = 1;
    int havebase = 1, riprel = 0;
    if (rmf == 4) {                              /* SIB */
        unsigned char s = c[(*i)++];
        scale = 1 << (s >> 6);
        index = ((s >> 3) & 7) | ((rex & 2) ? 8 : 0);
        base  = (s & 7) | ((rex & 1) ? 8 : 0);
        if (((s >> 3) & 7) == 4) index = -1;     /* rsp = no index */
        if ((s & 7) == 5 && mod == 0) { havebase = 0; disp = rd_s32(c + *i); *i += 4; }
    } else if (rmf == 5 && mod == 0) {           /* rip-relative */
        riprel = 1; disp = rd_s32(c + *i); *i += 4;
    }
    if (mod == 1) { disp = (signed char)c[(*i)++]; }
    else if (mod == 2) { disp = rd_s32(c + *i); *i += 4; }

    if (riprel) { sprintf(rm, "0x%lx(%%rip)", disp & 0xffffffffUL); return reg; }
    char ds[24] = "";
    if (disp < 0) sprintf(ds, "-0x%lx", -disp);
    else if (disp > 0 || !havebase) sprintf(ds, "0x%lx", disp);
    char inner[48] = "";
    int p = 0;
    inner[p++] = '(';
    if (havebase) p += sprintf(inner + p, "%%%s", REG64[base]);
    if (index >= 0) p += sprintf(inner + p, ",%%%s,%d", REG64[index], scale);
    inner[p++] = ')'; inner[p] = 0;
    if (havebase || index >= 0) sprintf(rm, "%s%s", ds, inner);
    else sprintf(rm, "%s", ds);                  /* absolute disp32, no base */
    return reg;
}

static const char *CC[16] = {"o","no","b","ae","e","ne","be","a",
                             "s","ns","p","np","l","ge","le","g"};
static const char *GRP1[8] = {"add","or","adc","sbb","and","sub","xor","cmp"};
static const char *GRP2[8] = {"rol","ror","rcl","rcr","shl","shr","sal","sar"};

/* Decode one instruction at code[0..n) with runtime address `addr`. Writes the
 * AT&T text to `out` and returns the byte length (>=1; 1 for a `.byte` stop). */
int embdbg_decode_one(const unsigned char *code, int n, unsigned long addr,
                      char *out)
{
    int i = 0, rex = 0, opsz = 4, pfx66 = 0, rep = 0;
    /* prefixes */
    while (i < n) {
        unsigned char b = code[i];
        if (b == 0x66) { pfx66 = 1; opsz = 2; i++; }
        else if (b == 0xf2 || b == 0xf3) { rep = b; i++; }
        else if (b == 0x67 || b == 0x2e || b == 0x3e || b == 0x26
                 || b == 0x64 || b == 0x65 || b == 0x36) { i++; }
        else break;
    }
    while (i < n && (code[i] & 0xf0) == 0x40) { rex = code[i]; i++; }  /* REX */
    if (rex & 8) opsz = 8;
    if (i >= n) { sprintf(out, ".byte 0x%02x", code[0]); return 1; }

    char rm[64], reg[16];
    unsigned char op = code[i++];

    /* ---- ALU r/m<->reg family: add/or/adc/sbb/and/sub/xor/cmp/mov/test ---- */
    struct { unsigned char base; const char *mn; } alu[] = {
        {0x00,"add"},{0x08,"or"},{0x10,"adc"},{0x18,"sbb"},
        {0x20,"and"},{0x28,"sub"},{0x30,"xor"},{0x38,"cmp"} };
    for (unsigned a = 0; a < sizeof alu / sizeof alu[0]; a++) {
        unsigned char bs = alu[a].base;
        if (op == bs + 0 || op == bs + 1 || op == bs + 2 || op == bs + 3) {
            int byte = !(op & 1), dir = (op >> 1) & 1, sz = byte ? 1 : opsz;
            int r = modrm(code, &i, rex, sz, 0, rm);
            sprintf(reg, "%%%s", regname(sz, r));
            if (dir) sprintf(out, "%s    %s,%s", alu[a].mn, rm, reg);
            else     sprintf(out, "%s    %s,%s", alu[a].mn, reg, rm);
            return i;
        }
    }
    if (op == 0x88 || op == 0x89 || op == 0x8a || op == 0x8b) {   /* mov */
        int byte = !(op & 1), dir = (op >> 1) & 1, sz = byte ? 1 : opsz;
        int r = modrm(code, &i, rex, sz, 0, rm);
        sprintf(reg, "%%%s", regname(sz, r));
        if (dir) sprintf(out, "mov    %s,%s", rm, reg);
        else     sprintf(out, "mov    %s,%s", reg, rm);
        return i;
    }
    if (op == 0x84 || op == 0x85) {                              /* test */
        int sz = (op & 1) ? opsz : 1;
        int r = modrm(code, &i, rex, sz, 0, rm);
        sprintf(out, "test   %%%s,%s", regname(sz, r), rm);
        return i;
    }
    if (op == 0x8d) {                                            /* lea */
        int r = modrm(code, &i, rex, opsz, 0, rm);
        sprintf(out, "lea    %s,%%%s", rm, regname(opsz, r));
        return i;
    }
    if (op == 0x63) {                                           /* movslq */
        int r = modrm(code, &i, rex, 4, 0, rm);
        sprintf(out, "movslq %s,%%%s", rm, regname(8, r));
        return i;
    }
    if (op == 0x80 || op == 0x81 || op == 0x83) {              /* grp1 imm */
        int sz = (op == 0x80) ? 1 : opsz;
        int r = modrm(code, &i, rex, sz, 0, rm);
        long imm; 
        if (op == 0x81) { imm = rd_s32(code + i); i += 4; }
        else { imm = (signed char)code[i]; i += 1; }
        sprintf(out, "%s    $0x%lx,%s", GRP1[r & 7], imm & 0xffffffffUL, rm);
        return i;
    }
    if (op == 0xc0 || op == 0xc1 || op == 0xd0 || op == 0xd1
        || op == 0xd2 || op == 0xd3) {                         /* grp2 shift */
        int sz = (op & 1) ? opsz : 1;
        int r = modrm(code, &i, rex, sz, 0, rm);
        if (op == 0xc0 || op == 0xc1) { int sh = code[i++]; sprintf(out, "%s    $0x%x,%s", GRP2[r&7], sh, rm); }
        else if (op == 0xd0 || op == 0xd1) sprintf(out, "%s    %s", GRP2[r&7], rm);
        else sprintf(out, "%s    %%cl,%s", GRP2[r&7], rm);
        return i;
    }
    if (op == 0xc6 || op == 0xc7) {                            /* mov imm */
        int sz = (op == 0xc6) ? 1 : opsz;
        int r = modrm(code, &i, rex, sz, 0, rm); (void)r;
        long imm; 
        if (op == 0xc6) { imm = code[i]; i += 1; }
        else { imm = rd_s32(code + i); i += 4; }
        sprintf(out, "mov    $0x%lx,%s", imm & 0xffffffffUL, rm);
        return i;
    }
    if (op >= 0xb8 && op <= 0xbf) {                            /* mov imm -> reg */
        int r = (op - 0xb8) | ((rex & 1) ? 8 : 0);
        if (rex & 8) { unsigned long lo = (unsigned)rd_s32(code + i) & 0xffffffffUL;
            unsigned long hi = (unsigned)rd_s32(code + i + 4) & 0xffffffffUL; i += 8;
            sprintf(out, "movabs $0x%lx,%%%s", lo | (hi << 32), REG64[r]); }
        else { long imm = (unsigned)rd_s32(code + i) & 0xffffffffUL; i += 4;
            sprintf(out, "mov    $0x%lx,%%%s", imm, regname(opsz, r)); }
        return i;
    }
    if (op >= 0xb0 && op <= 0xb7) {                            /* mov imm8 -> r8 */
        int r = (op - 0xb0) | ((rex & 1) ? 8 : 0); int imm = code[i++];
        sprintf(out, "mov    $0x%x,%%%s", imm, REG8[r]);
        return i;
    }
    if (op == 0xf6 || op == 0xf7) {                           /* grp3 */
        int sz = (op == 0xf6) ? 1 : opsz;
        const char *g3[8] = {"test","test","not","neg","mul","imul","div","idiv"};
        int r = modrm(code, &i, rex, sz, 0, rm);
        if ((r & 7) <= 1) { long imm; if (op==0xf6){imm=code[i];i++;} else {imm=rd_s32(code+i);i+=4;}
            sprintf(out, "test   $0x%lx,%s", imm & 0xffffffffUL, rm); }
        else sprintf(out, "%-6s %s", g3[r & 7], rm);
        return i;
    }
    if (op == 0xfe || op == 0xff) {                           /* grp4/5 */
        int sz = (op == 0xfe) ? 1 : opsz;
        int save = i; unsigned char mb = code[i];
        int r = (mb >> 3) & 7;
        const char *g5[8] = {"inc","dec","call","callf","jmp","jmpf","push","?"};
        int rr = modrm(code, &i, rex, r >= 2 ? 8 : sz, 0, rm); (void)rr; (void)save;
        if (r == 2 || r == 4) sprintf(out, "%s   *%s", g5[r], rm);
        else sprintf(out, "%-6s %s", g5[r], rm);
        return i;
    }
    if (op >= 0x50 && op <= 0x57) { sprintf(out, "push   %%%s", REG64[(op-0x50)|((rex&1)?8:0)]); return i; }
    if (op >= 0x58 && op <= 0x5f) { sprintf(out, "pop    %%%s", REG64[(op-0x58)|((rex&1)?8:0)]); return i; }
    if (op == 0x68) { long imm = rd_s32(code + i); i += 4; sprintf(out, "push   $0x%lx", imm & 0xffffffffUL); return i; }
    if (op == 0x6a) { int imm = (signed char)code[i++]; sprintf(out, "push   $0x%x", imm); return i; }
    if (op == 0x69 || op == 0x6b) {                           /* imul r,rm,imm */
        int r = modrm(code, &i, rex, opsz, 0, rm);
        long imm; if (op == 0x69) { imm = rd_s32(code + i); i += 4; } else { imm = (signed char)code[i]; i++; }
        sprintf(out, "imul   $0x%lx,%s,%%%s", imm & 0xffffffffUL, rm, regname(opsz, r));
        return i;
    }
    if (op == 0xe8 || op == 0xe9) {                           /* call/jmp rel32 */
        long rel = rd_s32(code + i); i += 4;
        sprintf(out, "%s   0x%lx", op == 0xe8 ? "call" : "jmp ", addr + i + rel);
        return i;
    }
    if (op == 0xeb) { long rel = (signed char)code[i++]; sprintf(out, "jmp    0x%lx", addr + i + rel); return i; }
    if (op >= 0x70 && op <= 0x7f) { long rel = (signed char)code[i++];
        sprintf(out, "j%-5s 0x%lx", CC[op - 0x70], addr + i + rel); return i; }
    if (op == 0xc3) { sprintf(out, "ret"); return i; }
    if (op == 0xc2) { int imm = code[i]|(code[i+1]<<8); i+=2; sprintf(out, "ret    $0x%x", imm); return i; }
    if (op == 0xc9) { sprintf(out, "leave"); return i; }
    if (op == 0x90) { sprintf(out, "nop"); return i; }
    if (op == 0x98) { sprintf(out, rex & 8 ? "cltq" : "cwtl"); return i; }
    if (op == 0x99) { sprintf(out, rex & 8 ? "cqto" : "cltd"); return i; }
    if (op == 0xcc) { sprintf(out, "int3"); return i; }
    if (op == 0xcd) { int imm = code[i++]; sprintf(out, "int    $0x%x", imm); return i; }
    if (op == 0xf4) { sprintf(out, "hlt"); return i; }

    /* ---- two-byte 0F opcodes ---- */
    if (op == 0x0f) {
        unsigned char o2 = code[i++];
        if (o2 >= 0x80 && o2 <= 0x8f) { long rel = rd_s32(code + i); i += 4;
            sprintf(out, "j%-5s 0x%lx", CC[o2 - 0x80], addr + i + rel); return i; }
        if (o2 >= 0x90 && o2 <= 0x9f) { int r = modrm(code, &i, rex, 1, 0, rm);
            (void)r; sprintf(out, "set%-3s %s", CC[o2 - 0x90], rm); return i; }
        if (o2 == 0xb6 || o2 == 0xb7 || o2 == 0xbe || o2 == 0xbf) {  /* movzx/movsx */
            int srcsz = (o2 & 1) ? 2 : 1;
            int r = modrm(code, &i, rex, srcsz, 0, rm);
            const char *mn = (o2 < 0xbe) ? "movz" : "movs";
            char suf = srcsz == 1 ? 'b' : 'w';
            char dst = (opsz == 8) ? 'q' : 'l';
            sprintf(out, "%s%c%c %s,%%%s", mn, suf, dst, rm, regname(opsz, r));
            return i;
        }
        if (o2 == 0xaf) { int r = modrm(code, &i, rex, opsz, 0, rm);   /* imul */
            sprintf(out, "imul   %s,%%%s", rm, regname(opsz, r)); return i; }
        if (o2 == 0x1f) { int r = modrm(code, &i, rex, opsz, 0, rm); (void)r;
            sprintf(out, "nop    %s", rm); return i; }
        if (o2 == 0xa2) { sprintf(out, "cpuid"); return i; }
        if (o2 == 0xc7) { int r = modrm(code, &i, rex, opsz, 0, rm); (void)r;
            sprintf(out, "rdrand %s", rm); return i; }
        /* SSE scalar/packed — reg is xmm; rm is xmm or memory */
        /* SSE scalar/packed. `tail` is what follows the s/d the prefix
         * selects, and `gpr_dst` marks the two conversions whose
         * destination is a general register, not an xmm — `cvttss2si
         * %xmm0,%eax`. Both were wrong before: 0x10/0x11 printed `movs`,
         * which is a STRING instruction, and the cvt forms lost their
         * `2si`/`2sd` tail, so the text named an instruction that either
         * does not exist or is a different one. */
        /* 0x11 and 0x29 are the STORE forms: the xmm is the SOURCE, so in
         * AT&T order it comes first. Printing them like their load
         * counterparts reversed every SSE store -- in `-S` output that is a
         * different program, and in a debugger's disassembly it is a lie
         * about which way the data moved. */
        struct { unsigned char o; int rmxmm; const char *base;
                 const char *tail; int gpr_dst; } sse[] = {
            {0x10,1,"movs","",0},   {0x11,1,"movs","",0},
            {0x28,1,"movap","",0},  {0x29,1,"movap","",0},
            {0x2a,0,"cvtsi2s","",0},
            {0x2c,1,"cvtts","2si",1}, {0x2d,1,"cvts","2si",1},
            {0x2e,1,"ucomis","",0}, {0x2f,1,"comis","",0},
            {0x51,1,"sqrts","",0},
            {0x58,1,"adds","",0},   {0x59,1,"muls","",0},
            {0x5c,1,"subs","",0},   {0x5e,1,"divs","",0},
            {0x5a,1,"cvts","2sd",0},
            {0x54,1,"andp","",0},   {0x57,1,"xorp","",0},
            {0xef,1,"pxor","",0} };
        for (unsigned k = 0; k < sizeof sse / sizeof sse[0]; k++) {
            if (o2 != sse[k].o) continue;
            char suf = rep == 0xf2 ? 'd' : rep == 0xf3 ? 's' : pfx66 ? 'd' : 's';
            int rmxmm = sse[k].rmxmm;
            int srcsz = (o2 == 0x2a) ? opsz : 4;   /* cvtsi2sd takes a GPR src */
            int r = modrm(code, &i, rex, srcsz, rmxmm, rm);
            /* cvtss2sd converts TO the other width, so its tail names the
             * destination: f3 (ss) -> 2sd, f2 (sd) -> 2ss. */
            const char *tail = sse[k].tail;
            if (o2 == 0x5a)
                tail = suf == 's' ? "2sd" : "2ss";
            if (sse[k].gpr_dst)
                sprintf(out, "%s%c%s   %s,%%%s", sse[k].base, suf, tail, rm,
                        regname(opsz == 8 ? 8 : 4, r));
            else if (o2 == 0x11 || o2 == 0x29)      /* a store */
                sprintf(out, "%s%c%s   %%xmm%d,%s", sse[k].base, suf, tail,
                        r, rm);
            else
                sprintf(out, "%s%c%s   %s,%%xmm%d", sse[k].base, suf, tail,
                        rm, r);
            return i;
        }
        sprintf(out, ".byte 0x0f,0x%02x", o2);
        return i;
    }

    sprintf(out, ".byte 0x%02x", op);
    return 1;
}
