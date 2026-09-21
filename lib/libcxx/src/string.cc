/* <string>'s out-of-line half: the numeric conversions, and the throwers.
 *
 * Every conversion goes through the C library's, which is where the hard
 * parts already are -- correct rounding on the way out, and on the way in
 * a locale-free parse that reports both "nothing was converted" and "it
 * does not fit". Reimplementing either here would be a second place for
 * those to be wrong.
 */
#include <string>
#include <stdexcept>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

namespace std {

void __throw_string_out_of_range()
{
    throw out_of_range("basic_string: index out of range");
}

void __throw_string_too_long()
{
    throw length_error("basic_string: requested size exceeds max_size()");
}

namespace {

/* 512 bytes covers every conversion here: the widest integer is 20
 * digits and a %f of a long double's exponent range is under 5000 --
 * which snprintf would truncate, so the result is clamped rather than
 * read past the buffer. */
template <class V>
string fmt(const char *spec, V v)
{
    char buf[512];
    int n = snprintf(buf, sizeof buf, spec, v);
    if (n < 0)
        return string();
    if ((size_t)n >= sizeof buf)
        n = (int)sizeof buf - 1;
    return string(buf, (size_t)n);
}

/* The two failures strtol-family conversions have to report, and which a
 * bare cast would silently swallow: nothing parsed at all, and a value
 * outside the target's range. */
[[noreturn]] void bad_no_conversion(const char *who)
{
    throw invalid_argument(who);
}
[[noreturn]] void bad_out_of_range(const char *who)
{
    throw out_of_range(who);
}

}  // namespace

string to_string(int v) { return fmt("%d", v); }
string to_string(long v) { return fmt("%ld", v); }
string to_string(long long v) { return fmt("%lld", v); }
string to_string(unsigned v) { return fmt("%u", v); }
string to_string(unsigned long v) { return fmt("%lu", v); }
string to_string(unsigned long long v)
{ return fmt("%llu", v); }
string to_string(double v) { return fmt("%f", v); }
string to_string(long double v) { return fmt("%Lf", v); }

/* errno is cleared FIRST and tested after: the conversion functions set
 * it on overflow and do not clear it otherwise, so a stale ERANGE from
 * anywhere earlier in the program would otherwise be reported as this
 * string's fault. */
int stoi(const string &s, size_t *pos, int base)
{
    char *end;
    errno = 0;
    long v = strtol(s.c_str(), &end, base);
    if (end == s.c_str())
        bad_no_conversion("stoi: no conversion");
    if (errno == ERANGE || v > 2147483647L || v < -2147483647L - 1)
        bad_out_of_range("stoi: out of range");
    if (pos)
        *pos = (size_t)(end - s.c_str());
    return (int)v;
}

long stol(const string &s, size_t *pos, int base)
{
    char *end;
    errno = 0;
    long v = strtol(s.c_str(), &end, base);
    if (end == s.c_str())
        bad_no_conversion("stol: no conversion");
    if (errno == ERANGE)
        bad_out_of_range("stol: out of range");
    if (pos)
        *pos = (size_t)(end - s.c_str());
    return v;
}

long long stoll(const string &s, size_t *pos, int base)
{
    char *end;
    errno = 0;
    long long v = strtoll(s.c_str(), &end, base);
    if (end == s.c_str())
        bad_no_conversion("stoll: no conversion");
    if (errno == ERANGE)
        bad_out_of_range("stoll: out of range");
    if (pos)
        *pos = (size_t)(end - s.c_str());
    return v;
}

unsigned long stoul(const string &s, size_t *pos, int base)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s.c_str(), &end, base);
    if (end == s.c_str())
        bad_no_conversion("stoul: no conversion");
    if (errno == ERANGE)
        bad_out_of_range("stoul: out of range");
    if (pos)
        *pos = (size_t)(end - s.c_str());
    return v;
}

unsigned long long stoull(const string &s, size_t *pos, int base)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s.c_str(), &end, base);
    if (end == s.c_str())
        bad_no_conversion("stoull: no conversion");
    if (errno == ERANGE)
        bad_out_of_range("stoull: out of range");
    if (pos)
        *pos = (size_t)(end - s.c_str());
    return v;
}

float stof(const string &s, size_t *pos)
{
    char *end;
    errno = 0;
    float v = strtof(s.c_str(), &end);
    if (end == s.c_str())
        bad_no_conversion("stof: no conversion");
    if (errno == ERANGE)
        bad_out_of_range("stof: out of range");
    if (pos)
        *pos = (size_t)(end - s.c_str());
    return v;
}

double stod(const string &s, size_t *pos)
{
    char *end;
    errno = 0;
    double v = strtod(s.c_str(), &end);
    if (end == s.c_str())
        bad_no_conversion("stod: no conversion");
    if (errno == ERANGE)
        bad_out_of_range("stod: out of range");
    if (pos)
        *pos = (size_t)(end - s.c_str());
    return v;
}

long double stold(const string &s, size_t *pos)
{
    char *end;
    errno = 0;
    long double v = strtold(s.c_str(), &end);
    if (end == s.c_str())
        bad_no_conversion("stold: no conversion");
    if (errno == ERANGE)
        bad_out_of_range("stold: out of range");
    if (pos)
        *pos = (size_t)(end - s.c_str());
    return v;
}

}  // namespace std
