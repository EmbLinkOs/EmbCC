/* <charconv>, <complex>, <memory_resource>.
 *
 * Three headers with one thing in common: each replaces something that
 * already worked, and the reason is never "it is faster".
 *
 *   charconv replaces sprintf and strtol because those consult the
 *     LOCALE and report failure through errno -- so the same program
 *     produces different bytes on different machines, and "no digits"
 *     is told apart from "converted zero" only by clearing errno first
 *     and comparing pointers after.
 *
 *   complex replaces C's _Complex because a complex<float> should cost
 *     two floats and because the layout is guaranteed, so an array can
 *     be handed to an FFT written in C.
 *
 *   memory_resource replaces the allocator template parameter because
 *     that parameter is part of the TYPE: vector<int> and
 *     vector<int, MyAlloc> are different types and a function taking
 *     one cannot take the other.
 */
#include "check.h"
#include <charconv>
#include <complex>
#include <cstring>
#include <memory_resource>
#include <string>
#include <system_error>

using namespace std;

static string chars_of(const char *b, const char *e)
{ return string(b, (size_t)(e - b)); }

int main()
{
    char buf[64];

    /* ---- to_chars: integers ------------------------------------------ */
    {
        auto r = to_chars(buf, buf + sizeof buf, 42);
        CHECK(r.ec == errc());
        CHECK(chars_of(buf, r.ptr) == "42");
        /* NOT terminated: ptr is one past the last character, and
         * nothing wrote a NUL. A caller that forgets is the reason this
         * is worth checking. */
        CHECK(r.ptr == buf + 2);

        r = to_chars(buf, buf + sizeof buf, -42);
        CHECK(chars_of(buf, r.ptr) == "-42");
        r = to_chars(buf, buf + sizeof buf, 0);
        CHECK(chars_of(buf, r.ptr) == "0");

        /* The most negative value, which is where negating in the
         * signed domain would be undefined rather than large. */
        r = to_chars(buf, buf + sizeof buf, (long long)(-9223372036854775807LL - 1));
        CHECK(chars_of(buf, r.ptr) == "-9223372036854775808");
        r = to_chars(buf, buf + sizeof buf, (int)(-2147483647 - 1));
        CHECK(chars_of(buf, r.ptr) == "-2147483648");

        r = to_chars(buf, buf + sizeof buf, 18446744073709551615ull);
        CHECK(chars_of(buf, r.ptr) == "18446744073709551615");
    }

    /* Other bases print the VALUE, not a sign: to_chars(-1, 16) is the
     * bit pattern, which is what the standard says and is easy to get
     * wrong by reusing the base-10 path. */
    {
        auto r = to_chars(buf, buf + sizeof buf, 255, 16);
        CHECK(chars_of(buf, r.ptr) == "ff");
        r = to_chars(buf, buf + sizeof buf, 255, 2);
        CHECK(chars_of(buf, r.ptr) == "11111111");
        r = to_chars(buf, buf + sizeof buf, 8, 8);
        CHECK(chars_of(buf, r.ptr) == "10");
        r = to_chars(buf, buf + sizeof buf, 35, 36);
        CHECK(chars_of(buf, r.ptr) == "z");
        r = to_chars(buf, buf + sizeof buf, (int)-1, 16);
        CHECK(chars_of(buf, r.ptr) == "ffffffff");
        r = to_chars(buf, buf + sizeof buf, 1, 37);
        CHECK(r.ec == errc::invalid_argument);
    }

    /* Too small a buffer writes NOTHING -- a partial number would be a
     * different number -- and ptr is the end, so a caller can size a
     * new one. */
    {
        char tiny[2];
        for (size_t i = 0; i < sizeof tiny; i++)
            tiny[i] = '#';
        auto r = to_chars(tiny, tiny + 2, 12345);
        CHECK(r.ec == errc::value_too_large);
        CHECK(r.ptr == tiny + 2);
        CHECK(tiny[0] == '#' && tiny[1] == '#');
        /* Exactly enough is enough. */
        char exact[3];
        auto e = to_chars(exact, exact + 3, 123);
        CHECK(e.ec == errc() && e.ptr == exact + 3);
    }

    /* ---- from_chars: integers ----------------------------------------- */
    {
        int v = 0;
        const char *s = "42rest";
        auto r = from_chars(s, s + strlen(s), v);
        CHECK(r.ec == errc() && v == 42);
        /* ptr is where parsing STOPPED, so a caller continues from
         * there without searching. */
        CHECK(r.ptr == s + 2 && *r.ptr == 'r');

        s = "-7";
        r = from_chars(s, s + 2, v);
        CHECK(r.ec == errc() && v == -7);

        /* NO whitespace is skipped and NO '+' is accepted. strtol does
         * both; from_chars deliberately does neither, because a parser
         * that silently swallows spaces cannot tell " 1" from "1". */
        s = " 1";
        r = from_chars(s, s + 2, v);
        CHECK(r.ec == errc::invalid_argument && r.ptr == s);
        s = "+1";
        r = from_chars(s, s + 2, v);
        CHECK(r.ec == errc::invalid_argument && r.ptr == s);

        /* Nothing parsed at all: ptr is the START, which is how this is
         * told apart from a value that happened to be zero. */
        s = "abc";
        r = from_chars(s, s + 3, v);
        CHECK(r.ec == errc::invalid_argument && r.ptr == s);
        s = "0";
        r = from_chars(s, s + 1, v);
        CHECK(r.ec == errc() && v == 0 && r.ptr == s + 1);

        /* Out of range: the digits WERE valid, so ptr is past them and
         * the caller can carry on reading its input. */
        s = "99999999999999999999";
        r = from_chars(s, s + strlen(s), v);
        CHECK(r.ec == errc::result_out_of_range);
        CHECK(r.ptr == s + strlen(s));

        /* The most negative value must PARSE: it is one further from
         * zero than the most positive, so a limit taken from the
         * accumulator rather than the type rejects it. */
        long long big = 0;
        s = "-9223372036854775808";
        r = from_chars(s, s + strlen(s), big);
        CHECK(r.ec == errc() && big == (-9223372036854775807LL - 1));
        /* One more is not representable. */
        s = "-9223372036854775809";
        r = from_chars(s, s + strlen(s), big);
        CHECK(r.ec == errc::result_out_of_range);

        unsigned long long u = 0;
        s = "18446744073709551615";
        r = from_chars(s, s + strlen(s), u);
        CHECK(r.ec == errc() && u == 18446744073709551615ull);

        /* An unsigned type rejects a sign rather than wrapping. */
        unsigned uu = 1;
        s = "-1";
        r = from_chars(s, s + 2, uu);
        CHECK(r.ec == errc::invalid_argument);

        /* Bases, and a digit out of range ends the number. */
        s = "ff";
        r = from_chars(s, s + 2, v, 16);
        CHECK(r.ec == errc() && v == 255);
        s = "129";
        r = from_chars(s, s + 3, v, 8);
        CHECK(r.ec == errc() && v == 10 && r.ptr == s + 2);   /* "12" */
        s = "z";
        r = from_chars(s, s + 1, v, 36);
        CHECK(r.ec == errc() && v == 35);
    }

    /* A narrow type rejects what does not fit it, not merely what does
     * not fit a long long. */
    {
        signed char c = 0;
        const char *s = "127";
        auto r = from_chars(s, s + 3, c);
        CHECK(r.ec == errc() && c == 127);
        s = "128";
        r = from_chars(s, s + 3, c);
        CHECK(r.ec == errc::result_out_of_range);
        s = "-128";
        r = from_chars(s, s + 4, c);
        CHECK(r.ec == errc() && c == -128);
    }

    /* Round trip, which is the property the pair exists for. */
    {
        long long values[] = {0, 1, -1, 12345, -98765, 4294967296LL,
                              -9223372036854775807LL - 1,
                              9223372036854775807LL};
        for (long long x : values) {
            auto w = to_chars(buf, buf + sizeof buf, x);
            CHECK(w.ec == errc());
            long long back = 0;
            auto rd = from_chars(buf, w.ptr, back);
            CHECK(rd.ec == errc() && rd.ptr == w.ptr && back == x);
        }
    }

    /* ---- charconv: floating point --------------------------------------- */
    {
        auto r = to_chars(buf, buf + sizeof buf, 1.5, chars_format::fixed, 2);
        CHECK(chars_of(buf, r.ptr) == "1.50");
        r = to_chars(buf, buf + sizeof buf, 3.14159, chars_format::fixed, 3);
        CHECK(chars_of(buf, r.ptr) == "3.142");
        r = to_chars(buf, buf + sizeof buf, 1234.5,
                     chars_format::scientific, 2);
        CHECK(chars_of(buf, r.ptr) == "1.23e+03");
        /* No locale: the separator is a point, always. */
        CHECK(chars_of(buf, r.ptr).find(',') == string::npos);

        double d = 0;
        const char *s = "1.5rest";
        auto f = from_chars(s, s + strlen(s), d);
        CHECK(f.ec == errc() && d == 1.5 && *f.ptr == 'r');

        s = "abc";
        f = from_chars(s, s + 3, d);
        CHECK(f.ec == errc::invalid_argument && f.ptr == s);
        /* Whitespace and '+' are refused here too. */
        s = " 1.0";
        f = from_chars(s, s + 4, d);
        CHECK(f.ec == errc::invalid_argument);

        /* `fixed` forbids an exponent: "1e5" read as fixed is 1
         * followed by "e5", not 100000. */
        s = "1e5";
        f = from_chars(s, s + 3, d, chars_format::fixed);
        CHECK(f.ec == errc() && d == 1.0 && f.ptr == s + 1);
        f = from_chars(s, s + 3, d, chars_format::general);
        CHECK(f.ec == errc() && d == 100000.0 && f.ptr == s + 3);

        /* A float round trip through an explicit precision. */
        float g = 0;
        s = "2.5";
        f = from_chars(s, s + 3, g);
        CHECK(f.ec == errc() && g == 2.5f);
    }

    /* ---- complex --------------------------------------------------------- */
    {
        complex<double> z(3.0, 4.0);
        CHECK(z.real() == 3.0 && z.imag() == 4.0);
        CHECK(real(z) == 3.0 && imag(z) == 4.0);

        /* The layout the standard guarantees: two T in order, so an
         * array is interleaved exactly as C's is and can be handed to
         * an FFT written in C. */
        static_assert(sizeof(complex<double>) == 2 * sizeof(double));
        static_assert(sizeof(complex<float>) == 2 * sizeof(float));
        complex<double> arr[2] = {{1.0, 2.0}, {3.0, 4.0}};
        const double *raw = reinterpret_cast<const double *>(arr);
        CHECK(raw[0] == 1.0 && raw[1] == 2.0 && raw[2] == 3.0 && raw[3] == 4.0);

        /* abs is NOT sqrt(x*x + y*y): squaring overflows for any part
         * above about 1e154 in a double, which is why it scales. */
        CHECK(abs(z) == 5.0);
        CHECK(norm(z) == 25.0);
        complex<double> huge(1e200, 1e200);
        double a = abs(huge);
        CHECK(a > 1e200 && a < 2e200);      /* finite, not inf */
        CHECK(abs(complex<double>(0.0, 0.0)) == 0.0);
        CHECK(abs(complex<double>(-3.0, 0.0)) == 3.0);
        CHECK(abs(complex<double>(0.0, -4.0)) == 4.0);

        CHECK(conj(z) == complex<double>(3.0, -4.0));

        /* Arithmetic. */
        complex<double> w(1.0, 2.0);
        CHECK(z + w == complex<double>(4.0, 6.0));
        CHECK(z - w == complex<double>(2.0, 2.0));
        /* (3+4i)(1+2i) = 3+6i+4i-8 = -5+10i */
        CHECK(z * w == complex<double>(-5.0, 10.0));
        complex<double> q = z / w;
        /* (3+4i)/(1+2i) = (3+4i)(1-2i)/5 = (11-2i)/5 */
        CHECK(q.real() > 2.19 && q.real() < 2.21);
        CHECK(q.imag() > -0.41 && q.imag() < -0.39);

        /* The compound forms must use the OLD real part for the new
         * imaginary one; a missing temporary is the classic bug. */
        complex<double> m(3.0, 4.0);
        m *= complex<double>(1.0, 2.0);
        CHECK(m == complex<double>(-5.0, 10.0));

        /* Mixed with a scalar. */
        CHECK(z * 2.0 == complex<double>(6.0, 8.0));
        CHECK(2.0 * z == complex<double>(6.0, 8.0));
        CHECK(z + 1.0 == complex<double>(4.0, 4.0));
        CHECK(1.0 - z == complex<double>(-2.0, -4.0));
        CHECK(-z == complex<double>(-3.0, -4.0));

        /* Assigning a scalar zeroes the imaginary part. */
        complex<double> s(1.0, 1.0);
        s = 5.0;
        CHECK(s == complex<double>(5.0, 0.0));
        CHECK(s == 5.0);
        CHECK(complex<double>(5.0, 1.0) != 5.0);
    }

    /* The identities, each checked rather than assumed. */
    {
        complex<double> i(0.0, 1.0);
        /* i*i == -1, the definition. */
        CHECK(i * i == complex<double>(-1.0, 0.0));
        /* sqrt(-1) == i */
        complex<double> r = sqrt(complex<double>(-1.0, 0.0));
        CHECK(r.real() > -1e-12 && r.real() < 1e-12);
        CHECK(r.imag() > 0.999999 && r.imag() < 1.000001);
        /* exp(0) == 1 */
        CHECK(exp(complex<double>(0.0, 0.0)) == complex<double>(1.0, 0.0));
        /* Euler: exp(i*pi) == -1 */
        complex<double> e = exp(i * 3.141592653589793);
        CHECK(e.real() < -0.999999 && e.real() > -1.000001);
        CHECK(e.imag() > -1e-9 && e.imag() < 1e-9);
        /* log(exp(z)) == z for a z in the principal branch */
        complex<double> z(0.5, 0.25);
        complex<double> back = log(exp(z));
        CHECK(back.real() > 0.4999 && back.real() < 0.5001);
        CHECK(back.imag() > 0.2499 && back.imag() < 0.2501);
        /* sin^2 + cos^2 == 1, over the complex numbers too */
        complex<double> t(0.3, 0.4);
        complex<double> one = sin(t) * sin(t) + cos(t) * cos(t);
        CHECK(one.real() > 0.999999 && one.real() < 1.000001);
        CHECK(one.imag() > -1e-9 && one.imag() < 1e-9);
        /* polar and abs/arg invert each other */
        complex<double> p = polar(2.0, 0.5);
        CHECK(abs(p) > 1.999999 && abs(p) < 2.000001);
        CHECK(arg(p) > 0.4999 && arg(p) < 0.5001);
        /* sqrt(z)^2 == z */
        complex<double> sq = sqrt(t);
        complex<double> again = sq * sq;
        CHECK(again.real() > 0.2999 && again.real() < 0.3001);
        CHECK(again.imag() > 0.3999 && again.imag() < 0.4001);
    }

    /* complex<float> is two floats and stays float. */
    {
        complex<float> f(3.0f, 4.0f);
        CHECK(abs(f) == 5.0f);
        static_assert(is_same_v<decltype(f.real()), float>);
        complex<float> g = f * 2.0f;
        CHECK(g.real() == 6.0f);
        /* Converting widens. */
        complex<double> d(f);
        CHECK(d.real() == 3.0 && d.imag() == 4.0);
    }

    /* ---- memory_resource ------------------------------------------------- */
    {
        /* A buffer the caller owns, on the stack: nothing is allocated
         * at all until it is exhausted, which is the form that makes
         * this worth using. */
        char arena[512];
        pmr::monotonic_buffer_resource r(arena, sizeof arena,
                                         pmr::null_memory_resource());
        pmr::polymorphic_allocator<int> a(&r);
        int *p = a.allocate(4);
        for (int i = 0; i < 4; i++)
            p[i] = i * 10;
        CHECK(p[3] == 30);
        /* Inside the caller's buffer, so upstream was never touched --
         * and upstream THROWS, so this would have failed if it had
         * been. */
        CHECK((char *)p >= arena && (char *)p < arena + sizeof arena);

        /* deallocate is a no-op: the memory comes back when the
         * resource dies, and that is the whole point. */
        int *q = a.allocate(4);
        CHECK(q != p);
        a.deallocate(p, 4);
        int *s = a.allocate(4);
        CHECK(s != p);              /* not reused: nothing was freed */
        (void)q;
        (void)s;

        /* Alignment is honoured. */
        pmr::polymorphic_allocator<double> ad(&r);
        double *dp = ad.allocate(1);
        CHECK(((size_t)dp % alignof(double)) == 0);
    }

    /* Exhausting the buffer goes upstream, and release() gives it all
     * back at once. */
    {
        char small[32];
        pmr::monotonic_buffer_resource r(small, sizeof small);
        pmr::polymorphic_allocator<long> a(&r);
        long *first = a.allocate(1);
        CHECK((char *)first >= small && (char *)first < small + sizeof small);
        /* More than the buffer holds: this must come from upstream
         * rather than run off the end. */
        long *big = a.allocate(200);
        big[0] = 1;
        big[199] = 2;
        CHECK(big[0] == 1 && big[199] == 2);
        CHECK(!((char *)big >= small && (char *)big < small + sizeof small));
        r.release();
    }

    /* null_memory_resource: every allocation throws. Not a joke -- it
     * is how a program proves a path it believes allocation-free really
     * is. */
    {
        pmr::polymorphic_allocator<int> a(pmr::null_memory_resource());
        int threw = 0;
        try { (void)a.allocate(1); } catch (const bad_alloc &) { threw = 1; }
        CHECK(threw);
    }

    /* The default resource is settable, and set_default_resource hands
     * back the previous one so it can be put back. */
    {
        pmr::memory_resource *before = pmr::get_default_resource();
        CHECK(before == pmr::new_delete_resource());
        char arena[128];
        pmr::monotonic_buffer_resource r(arena, sizeof arena);
        pmr::memory_resource *prev = pmr::set_default_resource(&r);
        CHECK(prev == before);
        CHECK(pmr::get_default_resource() == &r);
        /* A default-constructed allocator now uses it. */
        pmr::polymorphic_allocator<char> a;
        CHECK(a.resource() == &r);
        char *p = a.allocate(8);
        CHECK(p >= arena && p < arena + sizeof arena);
        pmr::set_default_resource(prev);
        CHECK(pmr::get_default_resource() == before);
    }

    /* A pool: many small blocks of a few sizes, and freeing really does
     * return them -- unlike the monotonic resource. */
    {
        pmr::unsynchronized_pool_resource pool;
        pmr::polymorphic_allocator<int> a(&pool);
        int *p = a.allocate(1);
        *p = 7;
        a.deallocate(p, 1);
        int *q = a.allocate(1);
        /* The freed block came back, which is the difference from the
         * monotonic one. */
        CHECK(q == p);
        a.deallocate(q, 1);

        /* Something too big for any size class goes upstream and must
         * still be released rather than leaked. */
        pmr::polymorphic_allocator<char> ac(&pool);
        char *big = ac.allocate(4096);
        big[0] = 'x';
        big[4095] = 'y';
        CHECK(big[0] == 'x' && big[4095] == 'y');
        ac.deallocate(big, 4096);
        pool.release();
    }

    /* Two resources are equal only to themselves: a resource that
     * claimed equality with another would let a container hand its
     * blocks to something that does not own them. */
    {
        char a1[64], a2[64];
        pmr::monotonic_buffer_resource r1(a1, sizeof a1);
        pmr::monotonic_buffer_resource r2(a2, sizeof a2);
        CHECK(r1 == r1);
        CHECK(r1 != r2);
        pmr::polymorphic_allocator<int> p1(&r1), p2(&r2), p3(&r1);
        CHECK(p1 == p3);
        CHECK(p1 != p2);
        /* A container copied from an arena does NOT take the arena
         * with it -- the arena's lifetime is its phase. */
        CHECK(p1.select_on_container_copy_construction().resource() ==
              pmr::get_default_resource());
    }

    DONE();
}
