# src/sema

Types, declarations, checking, diagnostics — ../../docs/ARCHITECTURE.md §2.

**The type system** lives in `type.h`/`type.c`: void, the integer types with
their unsigned variants, `_Bool`, `float`/`double`/`long double`, pointers,
arrays (including C99 variable-length ones), functions,
and the tagged types (struct/union/enum) with SysV layout — member offsets,
size, alignment, bitfield packing, and `__attribute__((aligned(N)))`. LP64.

Sema annotates every expression with a type and **materializes every implicit C
conversion as an explicit cast node**, so irgen never has to guess a width. That
one decision is why the integer-promotion and usual-arithmetic-conversion rules
live in exactly one place.

Also here: nested block scopes with shadowing, ordinary/tag/label namespaces,
prototype/definition merging under C linkage rules, `static`/`extern` (file and
block scope) and tentative definitions, lvalue and assignability checking,
constant-expression evaluation for initializers, enum and case values,
`_Static_assert` and `_Generic` resolution, all-paths-return, and
volatile-qualification tracked through to the IR so the optimizer preserves
every MMIO access.

**`_Complex`** is a type laid out, passed and returned as `struct { T re,
im; }` (its ABI on both targets), and its arithmetic is lowered here into
ordinary float operations on the parts: operands evaluated once into hidden
temps, results built in a fresh slot, `*` and `/` of two complex values
calling libgcc's `__mulXc3`/`__divXc3` for C99 Annex G, a real operand
combined part by part. Static initializers are folded exactly instead.

**`long double` constants** are computed in `ldfloat.c`: exactly, with an
arbitrary-precision integer, then rounded once per operation into the
TARGET's format — x87 80-bit extended on x86-64, IEEE binary128 on aarch64 —
which the host cannot represent (on arm64 macOS its own `long double` is a
double). Literals, static initializers and folded expressions come out bit
for bit as gcc's.

Diagnostics carry the column of the offending node, which is what gives semantic
errors a caret and not just a line number.
