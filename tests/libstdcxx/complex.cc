// std::complex through libstdc++'s GNU __complex__ implementation:
// arithmetic, comparisons, abs/arg/norm/conj, sqrt/exp/pow (newlib's
// <complex.h> functions), real/imag access, and complex<float> <-> double.
#include <complex>
#include <cstdio>

int main()
{
    std::complex<double> a(1.0, 2.0), b(3.0, -4.0);
    std::complex<double> c = a * b + 1.0;       // (1+2i)(3-4i) + 1 = 12 + 2i
    std::complex<double> d = c / std::complex<double>(0.0, 1.0);
    c.real(c.real() + 0.5);
    std::complex<float> f(d);
    std::printf("%.3f %.3f | %.3f %.3f | %.3f %.3f\n", c.real(), c.imag(),
                d.real(), d.imag(), (double)f.real(), (double)f.imag());
    std::printf("%.3f %.3f %.3f | %.3f %.3f\n", std::abs(b), std::arg(b),
                std::norm(b), std::conj(b).imag(), (-a).real());
    std::complex<double> s = std::sqrt(std::complex<double>(-4.0, 0.0));
    std::complex<double> e = std::exp(std::complex<double>(0.0, 0.0));
    std::complex<double> p = std::pow(a, 2);
    std::printf("%.3f %.3f | %.3f | %.3f %.3f | %d %d\n", s.real(), s.imag(),
                e.real(), p.real(), p.imag(), a == a, a != b);
    return 0;
}
