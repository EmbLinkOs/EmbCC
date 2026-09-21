/* Tokens for the M1 subset (ARCHITECTURE §2).
 *
 * Anything C has that this enum lacks is rejected in lex.c with a
 * diagnostic naming the construct — THE RULE: unsupported must fail
 * loudly, never pass through as something else. Keywords outside the
 * subset (if, while, char, ...) deliberately lex as identifiers so the
 * parser can reject them with better context than the lexer has.
 */
#ifndef EMBCC_LEX_LEX_H
#define EMBCC_LEX_LEX_H

enum tok_kind {
    TOK_EOF,
    TOK_NUM,
    TOK_FNUM,
    TOK_STR,
    TOK_IDENT,
    TOK_ELLIPSIS,
    TOK_KW_INT,
    TOK_KW_CHAR,
    TOK_KW_SHORT,
    TOK_KW_LONG,
    TOK_KW_INT128,    /* GNU __int128 */
    TOK_KW_FLOAT,
    TOK_KW_DOUBLE,
    TOK_KW_BOOL,
    TOK_KW_COMPLEX,   /* _Complex, __complex__ */
    TOK_KW_REAL,      /* __real__ (GNU) */
    TOK_KW_IMAG,      /* __imag__ (GNU) */
    TOK_KW_STATIC_ASSERT,
    TOK_KW_GENERIC,
    TOK_KW_ALIGNOF,
    TOK_KW_ALIGNAS,
    TOK_KW_TYPEOF,
    TOK_KW_ATOMIC,
    TOK_KW_UNSIGNED,
    TOK_KW_SIGNED,
    TOK_KW_VOID,
    TOK_KW_SIZEOF,
    TOK_KW_RETURN,
    TOK_KW_STATIC,
    TOK_KW_EXTERN,
    TOK_KW_STRUCT,
    TOK_KW_UNION,
    TOK_KW_ENUM,
    TOK_KW_TYPEDEF,
    TOK_KW_CONST,
    TOK_KW_VOLATILE,
    TOK_KW_RESTRICT,
    TOK_KW_ASM,
    TOK_KW_INLINE,
    TOK_KW_NORETURN,
    TOK_KW_ATTRIBUTE,
    TOK_DOT,
    TOK_ARROW,
    TOK_KW_IF,
    TOK_KW_ELSE,
    TOK_KW_WHILE,
    TOK_KW_FOR,
    TOK_KW_BREAK,
    TOK_KW_CONTINUE,
    TOK_KW_GOTO,
    TOK_KW_DO,
    TOK_KW_SWITCH,
    TOK_KW_CASE,
    TOK_KW_DEFAULT,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_SEMI,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_AMP,
    TOK_PIPE,
    TOK_CARET,
    TOK_TILDE,
    TOK_SHL,
    TOK_SHR,
    TOK_ASSIGN,
    TOK_EQEQ,
    TOK_NEQ,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    TOK_ANDAND,
    TOK_OROR,
    TOK_BANG,
    /* compound assignment; parse desugars 'a op= b' to 'a = a op b' */
    TOK_PLUSEQ,
    TOK_MINUSEQ,
    TOK_STAREQ,
    TOK_SLASHEQ,
    TOK_PERCENTEQ,
    TOK_AMPEQ,
    TOK_PIPEEQ,
    TOK_CARETEQ,
    TOK_SHLEQ,
    TOK_SHREQ,
    TOK_PLUSPLUS,
    TOK_MINUSMINUS,
    TOK_QUESTION,
    TOK_COLON,
    /* C++ only (struct lexer.cxx): punctuators and keywords. In C these
     * spellings are identifiers or never reach the lexer, so C is untouched. */
    TOK_COLONCOLON,   /* :: */
    TOK_DOTSTAR,      /* .* */
    TOK_ARROWSTAR,    /* ->* */
    TOK_SPACESHIP,    /* <=> */
    TOK_CX_CLASS, TOK_CX_NAMESPACE, TOK_CX_USING, TOK_CX_TEMPLATE,
    TOK_CX_TYPENAME, TOK_CX_PUBLIC, TOK_CX_PRIVATE, TOK_CX_PROTECTED,
    TOK_CX_VIRTUAL, TOK_CX_FRIEND, TOK_CX_OPERATOR, TOK_CX_NEW,
    TOK_CX_DELETE, TOK_CX_THIS, TOK_CX_TRUE, TOK_CX_FALSE, TOK_CX_NULLPTR,
    TOK_CX_BOOL, TOK_CX_EXPLICIT, TOK_CX_MUTABLE, TOK_CX_CONSTEXPR,
    TOK_CX_CONSTEVAL, TOK_CX_CONSTINIT, TOK_CX_DECLTYPE, TOK_CX_AUTO,
    TOK_CX_NOEXCEPT, TOK_CX_THROW, TOK_CX_TRY, TOK_CX_CATCH, TOK_CX_TYPEID,
    TOK_CX_STATIC_CAST, TOK_CX_DYNAMIC_CAST, TOK_CX_CONST_CAST,
    TOK_CX_REINTERPRET_CAST, TOK_CX_WCHAR_T, TOK_CX_CHAR8_T,
    TOK_CX_CHAR16_T, TOK_CX_CHAR32_T, TOK_CX_CONCEPT, TOK_CX_REQUIRES,
    TOK_CX_CO_AWAIT, TOK_CX_CO_YIELD, TOK_CX_CO_RETURN, TOK_CX_EXPORT,
    TOK_CX_THREAD_LOCAL, TOK_CX_REGISTER
};

/* One decoded element of a string or character literal.
 *
 * A CHARACTER (raw = 0) is a code point — from the source text, decoded from
 * UTF-8, or from a \u / \U escape — and is ENCODED at the literal's final
 * width: UTF-8 for a narrow literal, UTF-16 for u"", UTF-32 for L"" / U"".
 * A RAW unit (raw = 1) comes from a \x or octal escape, or from a source byte
 * that is not valid UTF-8, and is placed as one code-unit VALUE, unencoded —
 * which is what makes "\xC3\xA9" two bytes but L"\xE9" one element. Adjacent
 * literals are concatenated as these, then encoded once, because the final
 * width is not known until the last one is seen ("a" L"b" is wide). */
struct litch {
    unsigned long v;
    int raw;
};

/* Decodes one element at *p — a source character or a backslash escape (with
 * *p just past the backslash if esc) — and advances *p past it. */
struct litch lit_decode(const char **p, int esc, const char *file, int line);

/* Encodes n elements at `width` bytes per unit (1, 2 or 4), little-endian,
 * followed by a NUL unit. Returns a malloc'd buffer of *nunits * width bytes,
 * *nunits counting the NUL. An escape too wide for the unit is truncated with
 * a warning, as gcc does. */
char *lit_encode(const struct litch *lc, int n, int width, long *nunits,
                 const char *file, int line);

/* The value C gives a character constant holding the one element c, under
 * encoding prefix pfx (0, 'L', 'u', 'U'). A plain constant is the byte read
 * as the target's plain char, so '\xFF' is -1 on x86-64 and 255 on aarch64,
 * exactly as gcc gives it. *uns is set when the constant's type is unsigned
 * (char32_t, and wchar_t where the target makes it so). */
long lit_char_value(struct litch c, int pfx, int *uns, const char *file,
                    int line);

struct token {
    enum tok_kind kind;
    int line;
    int col;       /* 1-based column of the token's first character */
    long num;      /* TOK_NUM; TOK_STR: element count INCLUDING the NUL */
    int str_width; /* TOK_STR: bytes per element — 1 char, 2 char16, 4 wchar/32 */
    char str_prefix; /* TOK_STR: 'L', 'U', 'u', '8' (u8 in C++: char8_t),
                      * or 0 — L"" and U"" share a
                      * width but not a type (wchar_t vs char32_t) */
    struct litch *lit; /* TOK_STR: the decoded elements, for concatenation */
    int nlit;
    int num_long;  /* TOK_NUM: type is long (L suffix or magnitude) */
    int num_llong; /* TOK_NUM: an LL suffix (long long — the same width as
                    * long, but a distinct type to C++'s overloading) */
    int char_lit;  /* TOK_NUM from a character constant; str_prefix holds its
                    * encoding prefix (C++ types 'a' as char, not int) */
    int num_uns;   /* TOK_NUM: type is unsigned (U suffix or hex range) */
    double fnum;   /* TOK_FNUM */
    int fnum_is_float; /* TOK_FNUM: an 'f' suffix -> float, else double */
    int fnum_is_imag;  /* TOK_FNUM: a GNU imaginary constant (an i/j
                        * suffix, before or after f/l): 0 + this*i */
    int fnum_is_ld;    /* TOK_FNUM: an 'l' suffix -> long double; `text`
                        * then holds the digits (no suffix), which the
                        * exact conversion needs — a double would lose them */
    char *text;    /* TOK_IDENT; TOK_STR: the bytes (may contain NULs) */
    char *ud_suffix;   /* C++: a user-defined literal's suffix (_km in 5_km),
                        * on TOK_NUM, TOK_FNUM and TOK_STR */
    char *ud_spelling; /* ... a numeric one's digits as written (for a raw
                        * literal operator) */
};

struct lexer {
    int cxx;          /* C++ mode: its keywords and punctuators (src/cxx) */
    const char *file;
    const char *src;
    const char *p;
    const char *line_start;   /* start of the current line, for columns */
    int line;
    struct token tok; /* current token */
};

void lex_init(struct lexer *lx, const char *file, const char *src);
/* lex_init, choosing C (cxx 0) or C++ (cxx 1) keywords and punctuators. */
void lex_init_mode(struct lexer *lx, const char *file, const char *src,
                   int cxx);
void lex_next(struct lexer *lx);

/* Human-readable name of a token, for diagnostics. */
const char *tok_describe(const struct token *t);

#endif
