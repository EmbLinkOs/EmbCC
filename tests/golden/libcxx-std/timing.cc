/* <chrono> and <random>: units in the type, and unbiased draws. */
#include <chrono>
#include <random>
#include <vector>
#include <algorithm>
#include "check.h"
using namespace std;
using namespace std::chrono;

int main()
{
    /* ---- chrono: the unit is in the type ---- */
    {
        seconds s(2);
        milliseconds ms = s;             /* implicit: nothing is lost */
        CHECK(ms.count() == 2000);
        CHECK(s.count() == 2);

        /* Narrowing needs duration_cast, and it TRUNCATES toward zero --
         * which is why a 1999 ms timeout is 1 second and not 2. */
        milliseconds odd(1999);
        CHECK(duration_cast<seconds>(odd).count() == 1);
        CHECK(duration_cast<milliseconds>(seconds(3)).count() == 3000);
        CHECK(duration_cast<microseconds>(milliseconds(5)).count() == 5000);

        /* Mixed arithmetic lands in the FINER unit, so nothing is lost. */
        auto sum = seconds(1) + milliseconds(500);
        CHECK((is_same_v<decltype(sum), milliseconds>));
        CHECK(sum.count() == 1500);
        CHECK((seconds(2) - milliseconds(500)).count() == 1500);
        CHECK((milliseconds(100) * 3).count() == 300);
        CHECK((milliseconds(300) / 3).count() == 100);

        CHECK(seconds(1) == milliseconds(1000));
        CHECK(milliseconds(999) < seconds(1));
        CHECK(seconds(1) > milliseconds(999));
        CHECK(minutes(1) == seconds(60));
        CHECK(hours(1) == minutes(60));
        CHECK(-seconds(5) == seconds(-5));
    }
    {
        /* The literals, which are why anyone uses chrono in an interface. */
        using namespace std::chrono_literals;
        CHECK((100ms).count() == 100);
        CHECK((2s).count() == 2);
        CHECK(1s == 1000ms);
        CHECK((1h).count() == 1);
        CHECK(duration_cast<seconds>(1h).count() == 3600);
    }
    {
        /* ratio reduces at compile time, so ratio<2,4> IS ratio<1,2>. */
        CHECK((is_same_v<ratio<2, 4>::type, ratio<1, 2>::type>));
        CHECK((ratio<1, 2>::num == 1 && ratio<1, 2>::den == 2));
        CHECK((ratio_multiply<ratio<1, 2>, ratio<2, 3>>::num == 1));
        CHECK((ratio_multiply<ratio<1, 2>, ratio<2, 3>>::den == 3));
        CHECK((ratio_equal<ratio<2, 4>, ratio<1, 2>>::value));
    }
    {
        /* time_point arithmetic, and the clock in the type. */
        auto t0 = system_clock::now();
        auto t1 = t0 + seconds(10);
        CHECK(t1 - t0 == seconds(10));
        CHECK(t1 > t0);
        auto back = system_clock::to_time_t(t0);
        CHECK(system_clock::from_time_t(back) == t0);
        CHECK(steady_clock::is_steady && !system_clock::is_steady);
        auto s0 = steady_clock::now();
        auto s1 = steady_clock::now();
        CHECK(s1 >= s0);                 /* never goes backwards */
    }

    /* ---- random ---- */
    {
        /* The engines are reproducible from a seed, which is the point
         * of having one. */
        minstd_rand a(1), b(1);
        for (int i = 0; i < 100; i++) CHECK(a() == b());
        minstd_rand c(2);
        minstd_rand d(1);
        CHECK(c() != d());

        /* And every value is in range. */
        minstd_rand e(42);
        for (int i = 0; i < 500; i++) {
            auto v = e();
            CHECK(v >= minstd_rand::min() && v <= minstd_rand::max());
        }
    }
    {
        default_random_engine e(12345);
        uniform_int_distribution<int> d(1, 6);

        /* Every draw in range, and every face seen. This is the check
         * that matters: a rejection loop with the limit off by one
         * silently never returns an endpoint, and nothing else here
         * would notice.
         *
         * The counts are also checked for rough uniformity, which is
         * worth less than it looks: with a 64-bit engine and a range of
         * six, the bias `% range` introduces is far too small to detect
         * by counting. What this catches is a gross error -- a stuck
         * value, a lost endpoint, a fold that lands everything in half
         * the range. */
        vector<int> counts(7, 0);
        for (int i = 0; i < 12000; i++) {
            int v = d(e);
            CHECK(v >= 1 && v <= 6);
            counts[v]++;
        }
        for (int f = 1; f <= 6; f++) {
            CHECK(counts[f] > 1600);
            CHECK(counts[f] < 2400);
        }

        /* A degenerate range must still work and always give the same. */
        uniform_int_distribution<int> one(5, 5);
        for (int i = 0; i < 100; i++) CHECK(one(e) == 5);

        /* A range that does not divide the engine's span is where a
         * modulo would be most biased. */
        uniform_int_distribution<int> odd(0, 6);
        vector<int> oc(7, 0);
        for (int i = 0; i < 14000; i++)
        { int v = odd(e); CHECK(v >= 0 && v <= 6); oc[v]++; }
        for (int f = 0; f <= 6; f++) { CHECK(oc[f] > 1500); CHECK(oc[f] < 2500); }
    }
    {
        default_random_engine e(7);
        uniform_real_distribution<double> u(0.0, 1.0);
        double lo = 2, hi = -1, sum = 0;
        for (int i = 0; i < 4000; i++) {
            double v = u(e);
            CHECK(v >= 0.0 && v < 1.0000001);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            sum += v;
        }
        CHECK(lo < 0.02 && hi > 0.98);          /* it spans the range */
        double mean = sum / 4000;
        CHECK(mean > 0.45 && mean < 0.55);
    }
    {
        default_random_engine e(9);
        bernoulli_distribution b(0.25);
        int t = 0;
        for (int i = 0; i < 8000; i++) if (b(e)) t++;
        CHECK(t > 1700 && t < 2300);

        normal_distribution<double> n(10.0, 2.0);
        double sum = 0;
        for (int i = 0; i < 8000; i++) sum += n(e);
        double mean = sum / 8000;
        CHECK(mean > 9.8 && mean < 10.2);
    }
    DONE();
}
