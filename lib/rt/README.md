# lib/rt — the compiler runtime

The routines the **backend** calls: operations a machine has no
instruction for, which codegen turns into a call instead of into code.
`__multi3` for a 128-bit multiply, `__udivti3` for a 128-bit divide,
`__muldc3` for a complex multiply, and the conversions between integers
wider than a register and floating-point types.

## Why this is not `lib/libc`

Two different jobs. A C library implements what a **program** asks for
by name — `printf`, `malloc`, `strlen` — and every one of those names is
in a standard a programmer reads. Nothing in a program ever writes
`__multi3`. These are the compiler's own, and a program picks them up
because of how it was compiled rather than because of what it asked for.

Keeping them apart has a practical consequence as well as a conceptual
one: the freestanding targets link somebody else's `libgcc`, and giving
them a second definition of `__multi3` would be an error rather than a
convenience. `librt.a` ships only where it is wanted.

## Why the names are libgcc's

So an object of ours links beside one of theirs. That is not a
hypothetical — `tests/golden/rt.sh` does exactly that, linking a gcc-
compiled program against our `libc.a` and gcc's `libgcc.a`, which only
works because both sides agree on what these routines are called and
what they do.

## The constraint that shapes every file

**These routines may not use the operations they implement.** `__multi3`
cannot multiply two `__int128`s, because that is a call to `__multi3`;
`__ashlti3` cannot shift one. So a 128-bit value is only ever taken
apart and put back together through the union in `rt.h`, and everything
in between is 64-bit arithmetic the machine really has.

The same rule is why `ldouble.c` is `#ifdef __x86_64__`: there
`long double` is the x87 80-bit format and the hardware does it, while
on aarch64 it is IEEE binary128 with no instruction behind it, so even
`a + b` would be a call to a `__addtf3` that does not exist.

## What is here

| file | what |
|---|---|
| `rt.h` | the 128-bit halves union, and why |
| `int128.c` | multiply, divide, remainder, the three shifts, negate |
| `fp128.c` | 128-bit integers ↔ `float` and `double` |
| `complex.c` | `__mulsc3`/`__muldc3`/`__divsc3`/`__divdc3` |
| `ldouble.c` | the x87 128-bit conversions (x86-64), and complex `long double` for both |
| `softtf.c` | IEEE binary128 from the bits up (aarch64) |
| `unwind.c` | the DWARF CFI interpreter and the `_Unwind_*` API |

`softtf.c` is the largest of them and the one under the most pressure
from the rule above: it implements `long double` for a machine that has
no instruction for it, so it may not use `long double` for anything but
the parameter and return types the ABI requires. It is checked against
libgcc's own soft-float over 5362 lines and matches all of them.

`unwind.c` is here for the same reason as the rest: the compiler emits
a call to `_Unwind_Resume` in every landing pad it generates, so it is
a routine the backend calls even though a programmer never writes it.
It finds its tables through the linker's bracket symbols, refuses the
DWARF-expression CFA rules it cannot evaluate rather than guessing, and
is checked against libgcc's unwinder over a program whose output says
what happened -- see `tests/golden/unwind.sh` for why libgcc had to be
reached through the bare-metal harness to serve as that oracle.

## What is NOT here

`.eh_frame_hdr`, the sorted search table a dynamic loader uses to find
an FDE quickly. The linear scan is O(n) per frame, which is fine for a
static image and wrong to replace with a binary search until something
sorts the table.

## How it is checked

`tests/golden/rt.sh`, on a real kernel, against two independent oracles:

- The **integer** half against the host's own compiler and runtime:
  4343 lines of multiply, divide, remainder, shifts and conversions,
  required to be **byte-identical**.
- The **complex** half against **gcc's libgcc**, because these are
  libgcc's names and libgcc is the implementation whose behaviour ours
  has to be defensible against. Multiply is byte-identical on all 10683
  lines including every one of the 2401 combinations of zero, infinity
  and NaN. Division is allowed one unit in the last place, because C
  fixes the special values (which do match exactly) and leaves the
  rounding of the rest to the implementation — and three implementations
  round differently.

What is deliberately **not** compared is anything C leaves undefined:
a float too large for the integer type it is converted to. The host is
not an oracle there, because at any optimisation level it folds such a
conversion in the compiler rather than calling its own runtime, and it
answers inconsistently between two such values. `lib/rt` saturates, and
the test states that as its own behaviour rather than as agreement.
