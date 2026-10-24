/* The <complex.h> functions, every one at every width: values computed
 * elsewhere (mpmath at 60 digits, each part rounded once; a zero on a
 * branch cut taken as a limit from its own side, as C99 has it) at points that defeat the textbook formulas -- beside the branch
 * points, along the cuts on both signs of zero, just off the unit circle
 * where clog's real part is all cancellation -- each part within 64 ulps
 * of the target's double; the float and long double forms against the
 * double ones; and the special values C11's Annex G gives.
 *
 * The table is binary64's: where double is binary32 (RX), its arguments
 * are other numbers with other answers, and its double functions are the
 * float ones the table checks on every other target; the special values
 * are asked there too. */
// expect-exit: 42
#include <complex.h>
#include <float.h>
#include <math.h>
#include <stddef.h>

/* C11's constructors, where the C library's <complex.h> has not got them --
 * the cross gcc's newlib, for tests/golden/agrees-with-gcc.sh */
#ifndef CMPLX
#define CMPLX(x, y)  __builtin_complex((double)(x), (double)(y))
#define CMPLXF(x, y) __builtin_complex((float)(x), (float)(y))
#define CMPLXL(x, y) __builtin_complex((long double)(x), (long double)(y))
#endif

/* Classified by hand, not with <math.h>'s macros: the x86-64 QEMU harness
 * compiles with lib/libc's headers and links newlib, which has none of the
 * functions those call. */
static int is_nan(double v) { return v != v; }
static int sign_of(double v) { return __builtin_signbit(v) != 0; }

enum { CEXP, CLOG, CSQRT, CSIN, CCOS, CTAN, CSINH, CCOSH, CTANH,
       CASIN, CACOS, CATAN, CASINH, CACOSH, CATANH, NFN };

struct cref { int fn; double zr, zi, wr, wi; };
static const struct cref refs[] = {
    { CEXP, 0.5, 0.25, 1.5974665191199127, 0.40790017007835977 },
    { CEXP, -1.5, 2.0, -0.09285491028402633, 0.2028916804701695 },
    { CEXP, 3.0, -4.0, -13.128783081462158, 15.200784463067954 },
    { CEXP, 1.0, 1e-09, 2.718281828459045, 2.7182818284590456e-09 },
    { CEXP, -1.0, 1e-09, 0.36787944117144233, 3.6787944117144234e-10 },
    { CEXP, 1e-09, 1.0, 0.540302306408442, 0.8414709856493675 },
    { CEXP, 0.999999, 0.0001, 2.7182790965871804, 0.0002718279105648111 },
    { CEXP, 2.0, 0.0, 7.38905609893065, 0.0 },
    { CEXP, -2.0, 0.0, 0.1353352832366127, 0.0 },
    { CEXP, -2.0, -0.0, 0.1353352832366127, 0.0 },
    { CEXP, 1e-08, 1e-08, 1.00000001, 1.00000001e-08 },
    { CEXP, 20.0, -3.0, -480309903.06738126, -68466516.28659666 },
    { CEXP, 0.6, 0.8, 1.269482393459676, 1.3071080198026634 },
    { CEXP, 0.6, 0.80000000001, 1.2694823934466049, 1.3071080198153582 },
    { CEXP, -0.3, -0.7, 0.5666090282864079, -0.4772482007911177 },
    { CLOG, 0.5, 0.25, -0.5815754049028404, 0.4636476090008061 },
    { CLOG, -1.5, 2.0, 0.9162907318741551, 2.214297435588181 },
    { CLOG, 3.0, -4.0, 1.6094379124341003, -0.9272952180016122 },
    { CLOG, 1.0, 1e-09, 5e-19, 1e-09 },
    { CLOG, -1.0, 1e-09, 5e-19, 3.141592652589793 },
    { CLOG, 1e-09, 1.0, 5e-19, 1.5707963257948967 },
    { CLOG, 0.999999, 0.0001, -9.95000490054074e-07, 0.00010000009966676568 },
    { CLOG, 2.0, 0.0, 0.6931471805599453, 0.0 },
    { CLOG, -2.0, 0.0, 0.6931471805599453, 3.141592653589793 },
    { CLOG, -2.0, -0.0, 0.6931471805599453, -3.141592653589793 },
    { CLOG, 1e-08, 1e-08, -18.074107153672394, 0.7853981633974483 },
    { CLOG, 20.0, -3.0, 3.006857578021401, -0.14888994760949725 },
    { CLOG, 0.6, 0.8, 2.2204460492503132e-17, 0.9272952180016123 },
    { CLOG, 0.6, 0.80000000001, 8.00002286636946e-12, 0.9272952180076123 },
    { CLOG, -0.3, -0.7, -0.2723635877208361, -1.9756881130799802 },
    { CSQRT, 0.5, 0.25, 0.7276733451126774, 0.17178037486125622 },
    { CSQRT, -1.5, 2.0, 0.7071067811865476, 1.4142135623730951 },
    { CSQRT, 3.0, -4.0, 2.0, -1.0 },
    { CSQRT, 1.0, 1e-09, 1.0, 5e-10 },
    { CSQRT, -1.0, 1e-09, 5e-10, 1.0 },
    { CSQRT, 1e-09, 1.0, 0.7071067815401009, 0.7071067808329942 },
    { CSQRT, 0.999999, 0.0001, 0.9999995012498769, 5.00000249375186e-05 },
    { CSQRT, 2.0, 0.0, 1.4142135623730951, 0.0 },
    { CSQRT, -2.0, 0.0, 0.0, 1.4142135623730951 },
    { CSQRT, -2.0, -0.0, 0.0, -1.4142135623730951 },
    { CSQRT, 1e-08, 1e-08, 0.000109868411346781, 4.5508986056222736e-05 },
    { CSQRT, 20.0, -3.0, 4.484626429043822, -0.3344760201843208 },
    { CSQRT, 0.6, 0.8, 0.8944271909999159, 0.447213595499958 },
    { CSQRT, 0.6, 0.80000000001, 0.894427191002152, 0.44721359550443013 },
    { CSQRT, -0.3, -0.7, 0.4804046786753803, -0.7285524382590421 },
    { CSIN, 0.5, 0.25, 0.494485780933195, 0.22168816414957482 },
    { CSIN, -1.5, 2.0, -3.752771340479298, 0.2565539560904818 },
    { CSIN, 3.0, -4.0, 3.8537380379193773, 27.016813258003936 },
    { CSIN, 1.0, 1e-09, 0.8414709848078965, 5.403023058681397e-10 },
    { CSIN, -1.0, 1e-09, -0.8414709848078965, 5.403023058681397e-10 },
    { CSIN, 1e-09, 1.0, 1.5430806348152439e-09, 1.1752011936438014 },
    { CSIN, 0.999999, 0.0001, 0.8414704487125221, 5.4030314823935966e-05 },
    { CSIN, 2.0, 0.0, 0.9092974268256817, 0.0 },
    { CSIN, -2.0, 0.0, -0.9092974268256817, 0.0 },
    { CSIN, -2.0, -0.0, -0.9092974268256817, 0.0 },
    { CSIN, 1e-08, 1e-08, 1e-08, 1e-08 },
    { CSIN, 20.0, -3.0, 9.19122420497634, -4.088115055366117 },
    { CSIN, 0.6, 0.8, 0.7551725760865238, 0.7329854969137857 },
    { CSIN, 0.6, 0.80000000001, 0.7551725760915384, 0.7329854969248241 },
    { CSIN, -0.3, -0.7, -0.37092780393896435, -0.7247026904232854 },
    { CCOS, 0.5, 0.25, 0.9051501505596067, -0.12110879604381165 },
    { CCOS, -1.5, 2.0, 0.2661271953135458, 3.6177750739401375 },
    { CCOS, 3.0, -4.0, -27.034945603074224, 3.8511533348117775 },
    { CCOS, 1.0, 1e-09, 0.5403023058681398, -8.414709848078965e-10 },
    { CCOS, -1.0, 1e-09, 0.5403023058681398, 8.414709848078965e-10 },
    { CCOS, 1e-09, 1.0, 1.5430806348152437, -1.1752011936438015e-09 },
    { CCOS, 0.999999, 0.0001, 0.5403031500403701, -8.414704459076206e-05 },
    { CCOS, 2.0, 0.0, -0.4161468365471424, 0.0 },
    { CCOS, -2.0, 0.0, -0.4161468365471424, 0.0 },
    { CCOS, -2.0, -0.0, -0.4161468365471424, 0.0 },
    { CCOS, 1e-08, 1e-08, 1.0, -1.0000000000000001e-16 },
    { CCOS, 20.0, -3.0, 4.10843226487732, 9.145771337362248 },
    { CCOS, 0.6, 0.8, 1.1038326938102014, -0.5014623584193467 },
    { CCOS, 0.6, 0.80000000001, 1.1038326938175314, -0.5014623584268985 },
    { CCOS, -0.3, -0.7, 1.199108751098743, -0.2241768123375429 },
    { CTAN, 0.5, 0.25, 0.504500702698564, 0.3124206925025888 },
    { CTAN, -1.5, 2.0, -0.0053620609220030565, 1.036920282100185 },
    { CTAN, 3.0, -4.0, -0.00018734620462947842, -0.9993559873814731 },
    { CTAN, 1.0, 1e-09, 1.5574077246549023, 3.4255188208147598e-09 },
    { CTAN, -1.0, 1e-09, -1.5574077246549023, 3.4255188208147598e-09 },
    { CTAN, 1e-09, 1.0, 4.199743416140261e-10, 0.7615941559557649 },
    { CTAN, 0.999999, 0.0001, 1.5574042457924067, 0.0003425508056479829 },
    { CTAN, 2.0, 0.0, -2.185039863261519, 0.0 },
    { CTAN, -2.0, 0.0, 2.185039863261519, 0.0 },
    { CTAN, -2.0, -0.0, 2.185039863261519, 0.0 },
    { CTAN, 1e-08, 1e-08, 1e-08, 1e-08 },
    { CTAN, 20.0, -3.0, 0.003706132731354482, -1.003304966985037 },
    { CTAN, 0.6, 0.8, 0.31703926782658287, 0.808065172259716 },
    { CTAN, 0.6, 0.80000000001, 0.3170392678214591, 0.8080651722641914 },
    { CTAN, -0.3, -0.7, -0.18971709151908692, -0.63983593026318 },
    { CSINH, 0.5, 0.25, 0.504895714387995, 0.2789791283502615 },
    { CSINH, -1.5, 2.0, 0.8860929093625314, 2.139040009980677 },
    { CSINH, 3.0, -4.0, -6.548120040911002, 7.619231720321411 },
    { CSINH, 1.0, 1e-09, 1.1752011936438014, 1.5430806348152439e-09 },
    { CSINH, -1.0, 1e-09, -1.1752011936438014, 1.5430806348152439e-09 },
    { CSINH, 1e-09, 1.0, 5.403023058681397e-10, 0.8414709848078965 },
    { CSINH, 0.999999, 0.0001, 1.175199644687756, 0.00015430794570430226 },
    { CSINH, 2.0, 0.0, 3.6268604078470186, 0.0 },
    { CSINH, -2.0, 0.0, -3.6268604078470186, 0.0 },
    { CSINH, -2.0, -0.0, -3.6268604078470186, 0.0 },
    { CSINH, 1e-08, 1e-08, 1e-08, 1e-08 },
    { CSINH, 20.0, -3.0, -240154951.53369063, -34233258.14329833 },
    { CSINH, 0.6, 0.8, 0.4435608222125864, 0.8504006948556228 },
    { CSINH, 0.6, 0.80000000001, 0.44356082220801935, 0.850400694863882 },
    { CSINH, -0.3, -0.7, -0.23290996731262684, -0.6734255599525789 },
    { CCOSH, 0.5, 0.25, 1.0925708047319176, 0.12892104172809826 },
    { CCOSH, -1.5, 2.0, -0.9789478196465577, -1.9361483295105073 },
    { CCOSH, 3.0, -4.0, -6.580663040551157, 7.581552742746545 },
    { CCOSH, 1.0, 1e-09, 1.5430806348152437, 1.1752011936438015e-09 },
    { CCOSH, -1.0, 1e-09, 1.5430806348152437, -1.1752011936438015e-09 },
    { CCOSH, 1e-09, 1.0, 0.5403023058681398, 8.414709848078965e-10 },
    { CCOSH, 0.999999, 0.0001, 1.5430794518994244, 0.00011751996486050882 },
    { CCOSH, 2.0, 0.0, 3.7621956910836314, 0.0 },
    { CCOSH, -2.0, 0.0, 3.7621956910836314, 0.0 },
    { CCOSH, -2.0, -0.0, 3.7621956910836314, 0.0 },
    { CCOSH, 1e-08, 1e-08, 1.0, 1.0000000000000001e-16 },
    { CCOSH, 20.0, -3.0, -240154951.53369063, -34233258.14329833 },
    { CCOSH, 0.6, 0.8, 0.8259215712470895, 0.4567073249470405 },
    { CCOSH, 0.6, 0.80000000001, 0.8259215712385856, 0.45670732495147615 },
    { CCOSH, -0.3, -0.7, 0.7995189955990348, 0.1961773591614612 },
    { CTANH, 0.5, 0.25, 0.48548728102413535, 0.19805544995134952 },
    { CTANH, -1.5, 2.0, -1.0641443991765371, -0.08039101531016818 },
    { CTANH, 3.0, -4.0, 1.000709536067233, -0.00490825806749606 },
    { CTANH, 1.0, 1e-09, 0.7615941559557649, 4.199743416140261e-10 },
    { CTANH, -1.0, 1e-09, -0.7615941559557649, 4.199743416140261e-10 },
    { CTANH, 1e-09, 1.0, 3.4255188208147598e-09, 1.5574077246549023 },
    { CTANH, 0.999999, 0.0001, 0.7615937391796066, 4.19974980278302e-05 },
    { CTANH, 2.0, 0.0, 0.9640275800758169, 0.0 },
    { CTANH, -2.0, 0.0, -0.9640275800758169, 0.0 },
    { CTANH, -2.0, -0.0, -0.9640275800758169, 0.0 },
    { CTANH, 1e-08, 1e-08, 1e-08, 1e-08 },
    { CTANH, 20.0, -3.0, 1.0, 2.374112041535652e-18 },
    { CTANH, 0.6, 0.8, 0.8473188881809383, 0.561099223207919 },
    { CTANH, 0.6, 0.80000000001, 0.8473188881904469, 0.5610992232138878 },
    { CTANH, -0.3, -0.7, -0.4697051659675581, -0.72703718624352 },
    { CASIN, 0.5, 0.25, 0.5016088532755008, 0.2813960562452928 },
    { CASIN, -1.5, 2.0, -0.6065115181997548, 1.6224941488715938 },
    { CASIN, 3.0, -4.0, 0.6339838656391767, -2.305509031243477 },
    { CASIN, 1.0, 1e-09, 1.5707647040182975, 3.162277660431902e-05 },
    { CASIN, -1.0, 1e-09, -1.5707647040182975, 3.162277660431902e-05 },
    { CASIN, 1e-09, 1.0, 7.071067811865476e-10, 0.881373587019543 },
    { CASIN, 0.999999, 0.0001, 1.5607462845058986, 0.009950210199450912 },
    { CASIN, 2.0, 0.0, 1.5707963267948966, 1.3169578969248168 },
    { CASIN, -2.0, 0.0, -1.5707963267948966, 1.3169578969248168 },
    { CASIN, -2.0, -0.0, -1.5707963267948966, -1.3169578969248168 },
    { CASIN, 1e-08, 1e-08, 1e-08, 1e-08 },
    { CASIN, 20.0, -3.0, 1.4217267252464523, -3.6994199480566 },
    { CASIN, 0.6, 0.8, 0.4636476090008061, 0.8047189562170503 },
    { CASIN, 0.6, 0.80000000001, 0.4636476089983061, 0.8047189562245503 },
    { CASIN, -0.3, -0.7, -0.2457320061010797, -0.6703367429877897 },
    { CACOS, 0.5, 0.25, 1.0691874735193958, -0.2813960562452928 },
    { CACOS, -1.5, 2.0, 2.1773078449946515, -1.6224941488715938 },
    { CACOS, 3.0, -4.0, 0.9368124611557199, 2.305509031243477 },
    { CACOS, 1.0, 1e-09, 3.162277659904856e-05, -3.162277660431902e-05 },
    { CACOS, -1.0, 1e-09, 3.141561030813194, -3.162277660431902e-05 },
    { CACOS, 1e-09, 1.0, 1.5707963260877897, -0.881373587019543 },
    { CACOS, 0.999999, 0.0001, 0.01005004228899813, -0.009950210199450912 },
    { CACOS, 2.0, 0.0, 0.0, -1.3169578969248168 },
    { CACOS, -2.0, 0.0, 3.141592653589793, -1.3169578969248168 },
    { CACOS, -2.0, -0.0, 3.141592653589793, 1.3169578969248168 },
    { CACOS, 1e-08, 1e-08, 1.5707963167948966, -1e-08 },
    { CACOS, 20.0, -3.0, 0.14906960154844442, 3.6994199480566 },
    { CACOS, 0.6, 0.8, 1.1071487177940906, -0.8047189562170503 },
    { CACOS, 0.6, 0.80000000001, 1.1071487177965906, -0.8047189562245503 },
    { CACOS, -0.3, -0.7, 1.8165283328959763, 0.6703367429877897 },
    { CATAN, 0.5, 0.25, 0.4842544903299662, 0.20058661813123432 },
    { CATAN, -1.5, 2.0, -1.311223269671635, 0.3104282830771958 },
    { CATAN, 3.0, -4.0, 1.4483069952314644, -0.15899719167999918 },
    { CATAN, 1.0, 1e-09, 0.7853981633974483, 5e-10 },
    { CATAN, -1.0, 1e-09, -0.7853981633974483, 5e-10 },
    { CATAN, 1e-09, 1.0, 0.7853981636474483, 10.708206508753179 },
    { CATAN, 0.999999, 0.0001, 0.7853976658972008, 5.000004991669167e-05 },
    { CATAN, 2.0, 0.0, 1.1071487177940904, 0.0 },
    { CATAN, -2.0, 0.0, -1.1071487177940904, 0.0 },
    { CATAN, -2.0, -0.0, -1.1071487177940904, 0.0 },
    { CATAN, 1e-08, 1e-08, 1e-08, 1e-08 },
    { CATAN, 20.0, -3.0, 1.5219328731155373, -0.007317595575028303 },
    { CATAN, 0.6, 0.8, 0.7853981633974483, 0.5493061443340549 },
    { CATAN, 0.6, 0.80000000001, 0.785398163404115, 0.5493061443390549 },
    { CATAN, -0.3, -0.7, -0.48003518120284394, -0.7016804321523099 },
    { CASINH, 0.5, 0.25, 0.4926756834207706, 0.2243284526346675 },
    { CASINH, -1.5, 2.0, -1.6004100552346137, 0.8877651461839051 },
    { CASINH, 3.0, -4.0, 2.2999140408792695, -0.9176168533514787 },
    { CASINH, 1.0, 1e-09, 0.881373587019543, 7.071067811865476e-10 },
    { CASINH, -1.0, 1e-09, -0.881373587019543, 7.071067811865476e-10 },
    { CASINH, 1e-09, 1.0, 3.162277660431902e-05, 1.5707647040182975 },
    { CASINH, 0.999999, 0.0001, 0.8813728816803529, 7.071071344453992e-05 },
    { CASINH, 2.0, 0.0, 1.4436354751788103, 0.0 },
    { CASINH, -2.0, 0.0, -1.4436354751788103, 0.0 },
    { CASINH, -2.0, -0.0, -1.4436354751788103, 0.0 },
    { CASINH, 1e-08, 1e-08, 1e-08, 1e-08 },
    { CASINH, 20.0, -3.0, 3.700588641211476, -0.14871092244893852 },
    { CASINH, 0.6, 0.8, 0.7127084715353063, 0.684719203002283 },
    { CASINH, 0.6, 0.80000000001, 0.7127084715393888, 0.684719203010448 },
    { CASINH, -0.3, -0.7, -0.38559098731817487, -0.7089034409655053 },
    { CACOSH, 0.5, 0.25, 0.2813960562452928, 1.0691874735193958 },
    { CACOSH, -1.5, 2.0, 1.6224941488715938, 2.1773078449946515 },
    { CACOSH, 3.0, -4.0, 2.305509031243477, -0.9368124611557199 },
    { CACOSH, 1.0, 1e-09, 3.162277660431902e-05, 3.162277659904856e-05 },
    { CACOSH, -1.0, 1e-09, 3.162277660431902e-05, 3.141561030813194 },
    { CACOSH, 1e-09, 1.0, 0.881373587019543, 1.5707963260877897 },
    { CACOSH, 0.999999, 0.0001, 0.009950210199450912, 0.01005004228899813 },
    { CACOSH, 2.0, 0.0, 1.3169578969248168, 0.0 },
    { CACOSH, -2.0, 0.0, 1.3169578969248168, 3.141592653589793 },
    { CACOSH, -2.0, -0.0, 1.3169578969248168, -3.141592653589793 },
    { CACOSH, 1e-08, 1e-08, 1e-08, 1.5707963167948966 },
    { CACOSH, 20.0, -3.0, 3.6994199480566, -0.14906960154844442 },
    { CACOSH, 0.6, 0.8, 0.8047189562170503, 1.1071487177940906 },
    { CACOSH, 0.6, 0.80000000001, 0.8047189562245503, 1.1071487177965906 },
    { CACOSH, -0.3, -0.7, 0.6703367429877897, -1.8165283328959763 },
    { CATANH, 0.5, 0.25, 0.5003700000525311, 0.3143981432077165 },
    { CATANH, -1.5, 2.0, -0.22008968066202292, 1.2452579660726568 },
    { CATANH, 3.0, -4.0, 0.1175009073114339, -1.4099210495965755 },
    { CATANH, 1.0, 1e-09, 10.708206508753179, 0.7853981636474483 },
    { CATANH, -1.0, 1e-09, -10.708206508753179, 0.7853981636474483 },
    { CATANH, 1e-09, 1.0, 5e-10, 0.7853981633974483 },
    { CATANH, 0.999999, 0.0001, 4.9517185281429175, 0.7804233300664511 },
    { CATANH, 2.0, 0.0, 0.5493061443340549, 1.5707963267948966 },
    { CATANH, -2.0, 0.0, -0.5493061443340549, 1.5707963267948966 },
    { CATANH, -2.0, -0.0, -0.5493061443340549, -1.5707963267948966 },
    { CATANH, 1e-08, 1e-08, 1e-08, 1e-08 },
    { CATANH, 20.0, -3.0, 0.048936144281523826, -1.563443915605971 },
    { CATANH, 0.6, 0.8, 0.34657359027997264, 0.7853981633974483 },
    { CATANH, 0.6, 0.80000000001, 0.34657359027622264, 0.7853981634024483 },
    { CATANH, -0.3, -0.7, -0.19988189602962927, -0.6396697661585148 },
};

static double _Complex d_of(int fn, double _Complex z)
{
    switch (fn) {
    case CEXP: return cexp(z);     case CLOG: return clog(z);
    case CSQRT: return csqrt(z);   case CSIN: return csin(z);
    case CCOS: return ccos(z);     case CTAN: return ctan(z);
    case CSINH: return csinh(z);   case CCOSH: return ccosh(z);
    case CTANH: return ctanh(z);   case CASIN: return casin(z);
    case CACOS: return cacos(z);   case CATAN: return catan(z);
    case CASINH: return casinh(z); case CACOSH: return cacosh(z);
    default: return catanh(z);
    }
}
static float _Complex f_of(int fn, float _Complex z)
{
    switch (fn) {
    case CEXP: return cexpf(z);     case CLOG: return clogf(z);
    case CSQRT: return csqrtf(z);   case CSIN: return csinf(z);
    case CCOS: return ccosf(z);     case CTAN: return ctanf(z);
    case CSINH: return csinhf(z);   case CCOSH: return ccoshf(z);
    case CTANH: return ctanhf(z);   case CASIN: return casinf(z);
    case CACOS: return cacosf(z);   case CATAN: return catanf(z);
    case CASINH: return casinhf(z); case CACOSH: return cacoshf(z);
    default: return catanhf(z);
    }
}
static long double _Complex l_of(int fn, long double _Complex z)
{
    switch (fn) {
    case CEXP: return cexpl(z);     case CLOG: return clogl(z);
    case CSQRT: return csqrtl(z);   case CSIN: return csinl(z);
    case CCOS: return ccosl(z);     case CTAN: return ctanl(z);
    case CSINH: return csinhl(z);   case CCOSH: return ccoshl(z);
    case CTANH: return ctanhl(z);   case CASIN: return casinl(z);
    case CACOS: return cacosl(z);   case CATAN: return catanl(z);
    case CASINH: return casinhl(z); case CACOSH: return cacoshl(z);
    default: return catanhl(z);
    }
}

/* within n units of `eps` relative to want, and 0 exactly where want is */
static int near(double got, double want, double n, double eps)
{
    if (want == 0)
        return fabs(got) <= n * eps * DBL_MIN;
    return fabs(got - want) <= n * eps * fabs(want);
}

static int same(double a, double b)        /* bit for bit, a NaN any NaN */
{
    if (is_nan(a) || is_nan(b))
        return is_nan(a) && is_nan(b);
    return a == b && sign_of(a) == sign_of(b);
}

static int specials(void)
{
    double pi = 3.14159265358979323846, inf = INFINITY;
    double _Complex w;
    w = csqrt(CMPLX(-4.0, 0.0));
    if (!same(creal(w), 0.0) || !same(cimag(w), 2.0)) return 1;
    w = csqrt(CMPLX(-4.0, -0.0));
    if (!same(creal(w), 0.0) || !same(cimag(w), -2.0)) return 2;
    w = csqrt(CMPLX(3.0, inf));
    if (!same(creal(w), inf) || !same(cimag(w), inf)) return 3;
    w = clog(CMPLX(-0.0, 0.0));
    if (!same(creal(w), -inf) || !near(cimag(w), pi, 4, DBL_EPSILON)) return 4;
    w = clog(CMPLX(0.0, 0.0));
    if (!same(creal(w), -inf) || !same(cimag(w), 0.0)) return 5;
    w = cproj(CMPLX(inf, NAN));
    if (!same(creal(w), inf) || !same(cimag(w), 0.0)) return 6;
    w = cproj(CMPLX(2.0, -inf));
    if (!same(creal(w), inf) || !same(cimag(w), -0.0)) return 7;
    float _Complex f = cprojf(CMPLXF(-inf, 1.0f));
    if (!same(crealf(f), inf) || !same(cimagf(f), 0.0)) return 8;
    long double _Complex l = cprojl(CMPLXL(1.0L, -inf));
    if (!same((double)creall(l), inf) || !same((double)cimagl(l), -0.0)) return 9;
    w = catanh(CMPLX(1.0, 0.0));
    if (!same(creal(w), inf) || !same(cimag(w), 0.0)) return 10;
    w = cacosh(CMPLX(0.0, 0.0));
    if (!same(creal(w), 0.0) || !near(cimag(w), pi / 2, 4, DBL_EPSILON)) return 11;
    w = casinh(CMPLX(-0.0, 0.0));
    if (!same(creal(w), -0.0) || !same(cimag(w), 0.0)) return 12;
    w = cexp(CMPLX(0.0, 0.0));
    if (!same(creal(w), 1.0) || !same(cimag(w), 0.0)) return 13;
    w = cexp(CMPLX(-inf, 2.0));
    if (creal(w) != 0 || cimag(w) != 0) return 14;
    w = ctanh(CMPLX(inf, 1.0));
    if (!same(creal(w), 1.0) || cimag(w) != 0 || sign_of(cimag(w))) return 15;
    w = ccosh(CMPLX(0.0, 0.0));
    if (!same(creal(w), 1.0) || cimag(w) != 0) return 16;
    w = cpow(CMPLX(2.0, 0.0), CMPLX(10.0, 0.0));
    if (!near(creal(w), 1024.0, 64, DBL_EPSILON) || fabs(cimag(w)) > 1e-10) return 17;
    return 0;
}

/* Asked of lib/libc's functions, found by a weak reference to a function
 * each of its backends defines: the QEMU harness for x86-64 links newlib,
 * and a host its own libm, and neither is this library (Apple's clog
 * gives 0 for log|0.6 + 0.8i|, where it is 2.2e-17). Apple's linker
 * refuses a weak reference nothing defines, so there it is not asked. */
#if !defined(__APPLE__)
extern long __os_write(int fd, const void *buf, size_t n)
    __attribute__((weak));
#endif

int main(void)
{
#if defined(__APPLE__)
    return 42;
#else
    if (!&__os_write)
        return 42;
#endif
    double deps = DBL_EPSILON, feps = FLT_EPSILON;
    for (unsigned k = 0; DBL_MANT_DIG == 53 && k < sizeof refs / sizeof refs[0];
         k++) {
        const struct cref *r = &refs[k];
        double _Complex w = d_of(r->fn, CMPLX(r->zr, r->zi));
        if (!near(creal(w), r->wr, 64, deps) || !near(cimag(w), r->wi, 64, deps))
            return 20 + r->fn;
        /* float: against its own double, from the float arguments */
        float _Complex zf = CMPLXF((float)r->zr, (float)r->zi);
        float _Complex wf = f_of(r->fn, zf);
        double _Complex wd = d_of(r->fn, CMPLX(crealf(zf), cimagf(zf)));
        if (fabs(creal(wd)) > 1e-30 && !near(crealf(wf), creal(wd), 64, feps))
            return 40 + r->fn;
        if (fabs(cimag(wd)) > 1e-30 && !near(cimagf(wf), cimag(wd), 64, feps))
            return 60 + r->fn;
        long double _Complex wl = l_of(r->fn, CMPLXL(r->zr, r->zi));
        if (!near((double)creall(wl), creal(w), 64, deps) ||
            !near((double)cimagl(wl), cimag(w), 64, deps))
            return 80 + r->fn;
    }
    double _Complex p = cpow(CMPLX(1.5, -0.5), CMPLX(0.25, 2.0));
    double _Complex q = cexp(clog(CMPLX(1.5, -0.5)) * CMPLX(0.25, 2.0));
    if (!near(creal(p), creal(q), 1e6, deps) || !near(cimag(p), cimag(q), 1e6, deps))
        return 100;
    int s = specials();
    return s ? 110 + s : 42;
}
