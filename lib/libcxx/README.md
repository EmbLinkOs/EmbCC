# `lib/libcxx` — our C++ runtime

*The Itanium C++ ABI, on top of libgcc's unwinder. This is what
`libsupc++` is; the standard library sits above it.*

Built with EmbCC itself: `make libcxx-x86_64`, `make libcxx-aarch64`.

## What is here

| | |
|---|---|
| `new.cc` | `operator new`/`delete` — all twenty spellings, two of which do the work |
| `guard.cc` | static-local guards and the pure-virtual trap (`__cxa_atexit` is in the C library, beside `exit`) |
| `typeinfo.cc` | `std::type_info` and the ABI's type_info hierarchy |
| `dyncast.cc` | `__dynamic_cast`, and the base-class search catch matching shares |
| `eh.cc` | the exception object, the `__cxa_*` layer, and `__gxx_personality_v0` |
| `terminate.cc` | `std::terminate` and its handler |

## The split with libgcc

Walking the stack — decoding `.eh_frame`, restoring callee-saved
registers frame by frame, transferring control — is the **unwinder**, and
it stays libgcc's. It is language-neutral: every language on the platform
shares it, and `include/unwind.h` is EmbCC's declaration of it.

What is not neutral is the **policy**: given a frame, does this exception
match one of its catch clauses, and what address does the handler get?
That is a question about C++ types, and the unwinder answers it by calling
back into the personality routine here.

So: libgcc owns the machine, this owns the meaning.

## The standard library

`include/` also holds the beginning of the standard library proper:
`<type_traits>`, `<utility>`, `<limits>` and the `<c*>` wrappers. See
`docs/language/libcxx.md`.

## Status

The runtime is complete enough to run C++ programs with virtual dispatch,
RTTI, `dynamic_cast`, static locals, `new`/`delete` and exceptions with no
`libsupc++` at all, on both targets — `tests/golden/libcxx.sh`. The
standard library above it is next; see `docs/language/libcxx.md`.
