/* The standard stream objects.
 *
 * They are defined here rather than in the header because there must be
 * exactly one of each, and they have to outlive every static object in
 * the program that might write to them during its destructor.
 *
 * The lifetime problem is real and is why the standard specifies
 * ios_base::Init: a static object in another translation unit may write
 * to cout from its constructor or its destructor, and static
 * initialisation order across translation units is unspecified. The
 * answer here is the same one libstdc++ uses -- the objects live in raw
 * storage that is never destroyed, so "after cout's destructor" cannot
 * happen.
 */
#include <iostream>

namespace std {

namespace __detail {
__filebuf __cin_buf(stdin);
__filebuf __cout_buf(stdout);
__filebuf __cerr_buf(stderr);
}

/* Raw storage, constructed in place by the first __ios_init and NEVER
 * destroyed. A destructor here would run at some point during exit, and
 * anything writing to cout after that point -- another static object's
 * destructor -- would be writing to a dead stream. Leaking three objects
 * at process exit is the accepted trade, and is what every
 * implementation does. */
alignas(istream) static unsigned char __cin_store[sizeof(istream)];
alignas(ostream) static unsigned char __cout_store[sizeof(ostream)];
alignas(ostream) static unsigned char __cerr_store[sizeof(ostream)];
alignas(ostream) static unsigned char __clog_store[sizeof(ostream)];

istream &cin = *reinterpret_cast<istream *>(__cin_store);
ostream &cout = *reinterpret_cast<ostream *>(__cout_store);
ostream &cerr = *reinterpret_cast<ostream *>(__cerr_store);
ostream &clog = *reinterpret_cast<ostream *>(__clog_store);

int __ios_init::__count = 0;

__ios_init::__ios_init()
{
    if (__count++ != 0)
        return;
    new (__cin_store) istream(&__detail::__cin_buf);
    new (__cout_store) ostream(&__detail::__cout_buf);
    new (__cerr_store) ostream(&__detail::__cerr_buf);
    new (__clog_store) ostream(&__detail::__cerr_buf);
    /* cerr is unit-buffered: an error message that is still sitting in a
     * buffer when the program dies is an error message nobody sees. */
    cerr.setf(ios_base::unitbuf);
}

__ios_init::~__ios_init()
{
    if (--__count != 0)
        return;
    /* Flush, but do not destroy: see above. */
    cout.flush();
    cerr.flush();
    clog.flush();
}

}  // namespace std
