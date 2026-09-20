/* random_device: the OS's entropy, or nothing.
 *
 * The one rule this file exists to keep is that it never invents
 * randomness. Falling back to a timestamp when the OS has no entropy
 * source is the traditional thing to do and is worse than failing: a
 * caller handed a predictable seed has no way to tell, and finds out
 * when someone predicts it. A caller handed an exception can decide.
 */
#include <random>
#include <stdexcept>

#include "../../libc/os/backend.h"

namespace std {

random_device::result_type random_device::operator()()
{
    result_type v;
    if (__os_getentropy(&v, sizeof v) != 0)
        throw runtime_error(
            "random_device: this target has no entropy source");
    return v;
}

/* Zero means "no entropy", which is what a source that cannot answer
 * should say -- and what a caller checking it is asking about. */
double random_device::entropy() const noexcept
{
    unsigned probe;
    return __os_getentropy(&probe, sizeof probe) == 0 ? 32.0 : 0.0;
}

}  // namespace std
