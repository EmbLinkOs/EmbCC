/* What the compiler's code calls to throw, in a runtime built without
 * exceptions (tools/build-libcxx.sh: the embedded targets).
 *
 * A failed dynamic_cast to a reference calls __cxa_bad_cast, and typeid of
 * a null pointer's object __cxa_bad_typeid; with exceptions they throw
 * std::bad_cast and std::bad_typeid (eh.cc). With none there is nothing
 * to throw and nobody to catch it, and returning would hand the program a
 * reference to nothing: they stop where the error is, as libsupc++ built
 * with -fno-exceptions does. Built with exceptions, this file is empty. */
#if !__cpp_exceptions
extern "C" {
void __cxa_bad_cast() { __builtin_trap(); }
void __cxa_bad_typeid() { __builtin_trap(); }
}
#endif
