/* The standard exception hierarchy's out-of-line half.
 *
 * Two things here are not obvious and both are about what may happen
 * while an exception is already in flight.
 *
 * The message is COPIED. A what() returning the caller's pointer dangles
 * exactly when it is read -- during unwinding, after that buffer's scope
 * is gone -- and the bug looks like memory corruption rather than a
 * lifetime mistake.
 *
 * The copy is REFERENCE-COUNTED. `catch (std::logic_error e)` by value
 * copies the exception, and a copy constructor that allocates can throw;
 * throwing while unwinding is terminate(). Sharing the buffer makes the
 * copy constructor noexcept, which is what [exception]/2 requires of
 * these types.
 */
#include <stdexcept>

#include <stdlib.h>
#include <string.h>

namespace std {

struct __msg_exception::__rep {
    unsigned long n;        /* references */
    char text[1];           /* the message, inline after the count */
};

__msg_exception::__msg_exception(const char *w) : __r(nullptr)
{
    if (!w)
        w = "";
    size_t len = strlen(w);
    /* One allocation for the count and the text together: two would mean
     * two failure points in a constructor that is about to be thrown. */
    void *m = malloc(sizeof(__rep) + len);
    if (!m)
        return;             /* what() answers a fixed string below */
    __r = static_cast<__rep *>(m);
    __r->n = 1;
    memcpy(__r->text, w, len + 1);
}

__msg_exception::__msg_exception(const __msg_exception &o) noexcept
    : __r(o.__r)
{
    if (__r)
        __r->n++;
}

__msg_exception &
__msg_exception::operator=(const __msg_exception &o) noexcept
{
    if (this != &o) {
        /* The new reference is taken BEFORE the old one is dropped, so
         * self-assignment through two names cannot free what it keeps. */
        __rep *n = o.__r;
        if (n)
            n->n++;
        if (__r && --__r->n == 0)
            free(__r);
        __r = n;
    }
    return *this;
}

__msg_exception::~__msg_exception() noexcept
{
    if (__r && --__r->n == 0)
        free(__r);
}

const char *__msg_exception::what() const noexcept
{
    /* Honest when the message could not be stored, rather than null --
     * a what() that returns null crashes the handler printing it. */
    return __r ? __r->text : "std::exception (message unavailable)";
}

logic_error::~logic_error() noexcept {}
domain_error::~domain_error() noexcept {}
invalid_argument::~invalid_argument() noexcept {}
length_error::~length_error() noexcept {}
out_of_range::~out_of_range() noexcept {}
runtime_error::~runtime_error() noexcept {}
range_error::~range_error() noexcept {}
overflow_error::~overflow_error() noexcept {}
underflow_error::~underflow_error() noexcept {}

}  // namespace std

/* The containers' throwers, defined here so those headers need not
 * include <stdexcept> -- otherwise every use of a fixed-size array or a
 * vector drags in the whole exception hierarchy. */
namespace std {
void __throw_array_out_of_range()
{
    throw out_of_range("array::at: index out of range");
}
void __throw_vector_out_of_range()
{
    throw out_of_range("vector::at: index out of range");
}
void __throw_vector_too_long()
{
    throw length_error("vector: requested size exceeds max_size()");
}
}

/* <optional>'s failure, out of line beside the others so that header need
 * not include <stdexcept> either. */
#include <optional>

namespace std {
bad_optional_access::~bad_optional_access() noexcept {}
const char *bad_optional_access::what() const noexcept
{ return "std::bad_optional_access"; }
}
