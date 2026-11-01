#include "core/DetMath.h"

#include <cmath>
#include <limits>

namespace ex::core::det {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLn2Hi = 6.93147180369123816490e-01;
constexpr double kLn2Lo = 1.90821492927058770002e-10;
constexpr double kInvLn2 = 1.44269504088896338700e+00;

// pi/2 split in three pieces (fdlibm), each short enough that k * piece is
// exact for |k| < 2^20.
constexpr double kPio2Hi = 1.57079632673412561417e+00;
constexpr double kPio2Mid = 6.07710050630396597660e-11;
constexpr double kPio2Lo = 2.02226624871116645580e-21;
constexpr double kPio2LoTail = 8.47842766036889956997e-32;
constexpr double kTwoOverPi = 6.36619772367581382433e-01;

// sin(r) and cos(r) for |r| <= pi/4 by Taylor series (Horner form).
double SinKernel(double r) {
    const double r2 = r * r;
    double p = 1.0 / 51090942171709440000.0;  // 1/21!
    p = p * r2 - 1.0 / 121645100408832000.0;  // 1/19!
    p = p * r2 + 1.0 / 355687428096000.0;     // 1/17!
    p = p * r2 - 1.0 / 1307674368000.0;       // 1/15!
    p = p * r2 + 1.0 / 6227020800.0;          // 1/13!
    p = p * r2 - 1.0 / 39916800.0;            // 1/11!
    p = p * r2 + 1.0 / 362880.0;              // 1/9!
    p = p * r2 - 1.0 / 5040.0;                // 1/7!
    p = p * r2 + 1.0 / 120.0;                 // 1/5!
    p = p * r2 - 1.0 / 6.0;                   // 1/3!
    return r + r * r2 * p;
}

double CosKernel(double r) {
    const double r2 = r * r;
    double p = -1.0 / 2432902008176640000.0;  // -1/20!
    p = p * r2 + 1.0 / 6402373705728000.0;    // 1/18!
    p = p * r2 - 1.0 / 20922789888000.0;      // -1/16!
    p = p * r2 + 1.0 / 87178291200.0;         // 1/14!
    p = p * r2 - 1.0 / 479001600.0;           // -1/12!
    p = p * r2 + 1.0 / 3628800.0;             // 1/10!
    p = p * r2 - 1.0 / 40320.0;               // -1/8!
    p = p * r2 + 1.0 / 720.0;                 // 1/6!
    p = p * r2 - 1.0 / 24.0;                  // -1/4!
    p = p * r2 + 0.5;                         // 1/2!
    return 1.0 - r2 * p;
}

// Reduces x to r in [-pi/4, pi/4] and returns the quadrant k mod 4.
int ReduceQuadrant(double x, double& r) {
    // Beyond 1e5 rad the multiples of pi/2 of the three-part reduction are no
    // longer exact, so the argument is folded by full turns first. fmod is exact;
    // the only error is the representation of 2 pi (about 2.4e-16 rad per turn,
    // 5e-9 rad at |x| = 1e8).
    if (std::fabs(x) >= 1.0e5) {
        x = std::fmod(x, 2.0 * kPi);
    }
    const double k = std::floor(x * kTwoOverPi + 0.5);
    r = ((x - k * kPio2Hi) - k * kPio2Mid) - k * kPio2Lo - k * kPio2LoTail;
    long long q = static_cast<long long>(k);
    return int(((q % 4) + 4) % 4);
}

// atan for |t| <= 1 via three argument halvings and a short series.
double AtanUnit(double t) {
    double x = t;
    for (int i = 0; i < 3; ++i) {
        x = x / (1.0 + std::sqrt(1.0 + x * x));
    }
    const double x2 = x * x;
    // atan(x) = x * (1 - x2/3 + x2^2/5 - ... - x2^9/19)
    double p = -1.0 / 19.0;
    p = p * x2 + 1.0 / 17.0;
    p = p * x2 - 1.0 / 15.0;
    p = p * x2 + 1.0 / 13.0;
    p = p * x2 - 1.0 / 11.0;
    p = p * x2 + 1.0 / 9.0;
    p = p * x2 - 1.0 / 7.0;
    p = p * x2 + 1.0 / 5.0;
    p = p * x2 - 1.0 / 3.0;
    p = p * x2 + 1.0;
    return 8.0 * (x * p);
}

} // namespace

double Sin(double x) {
    if (!std::isfinite(x)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double r = 0.0;
    switch (ReduceQuadrant(x, r)) {
    case 0: return SinKernel(r);
    case 1: return CosKernel(r);
    case 2: return -SinKernel(r);
    default: return -CosKernel(r);
    }
}

double Cos(double x) {
    if (!std::isfinite(x)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double r = 0.0;
    switch (ReduceQuadrant(x, r)) {
    case 0: return CosKernel(r);
    case 1: return -SinKernel(r);
    case 2: return -CosKernel(r);
    default: return SinKernel(r);
    }
}

double Atan2(double y, double x) {
    if (x == 0.0 && y == 0.0) {
        return 0.0;
    }
    const double ax = std::fabs(x);
    const double ay = std::fabs(y);
    double angle;
    if (ay <= ax) {
        angle = AtanUnit(ay / ax);
    } else {
        angle = 0.5 * kPi - AtanUnit(ax / ay);
    }
    if (x < 0.0) {
        angle = kPi - angle;
    }
    return y < 0.0 ? -angle : angle;
}

double Exp(double x) {
    if (x > 709.0) {
        return std::numeric_limits<double>::infinity();
    }
    if (x < -745.0) {
        return 0.0;
    }
    const double k = std::floor(x * kInvLn2 + 0.5);
    const double r = (x - k * kLn2Hi) - k * kLn2Lo;  // |r| <= ln2 / 2
    double p = 1.0 / 87178291200.0;                  // 1/14!
    p = p * r + 1.0 / 6227020800.0;                  // 1/13!
    p = p * r + 1.0 / 479001600.0;                   // 1/12!
    p = p * r + 1.0 / 39916800.0;                    // 1/11!
    p = p * r + 1.0 / 3628800.0;                     // 1/10!
    p = p * r + 1.0 / 362880.0;                      // 1/9!
    p = p * r + 1.0 / 40320.0;                       // 1/8!
    p = p * r + 1.0 / 5040.0;                        // 1/7!
    p = p * r + 1.0 / 720.0;                         // 1/6!
    p = p * r + 1.0 / 120.0;                         // 1/5!
    p = p * r + 1.0 / 24.0;                          // 1/4!
    p = p * r + 1.0 / 6.0;                           // 1/3!
    p = p * r + 0.5;                                 // 1/2!
    p = p * r + 1.0;
    p = p * r + 1.0;
    return std::ldexp(p, int(k));
}

double Log(double x) {
    if (x <= 0.0) {
        return x == 0.0 ? -std::numeric_limits<double>::infinity()
                        : std::numeric_limits<double>::quiet_NaN();
    }
    int exponent = 0;
    double m = std::frexp(x, &exponent);  // m in [0.5, 1)
    if (m < 0.70710678118654752440) {
        m *= 2.0;
        exponent -= 1;
    }
    const double s = (m - 1.0) / (m + 1.0);
    const double s2 = s * s;
    double p = 1.0 / 23.0;
    p = p * s2 + 1.0 / 21.0;
    p = p * s2 + 1.0 / 19.0;
    p = p * s2 + 1.0 / 17.0;
    p = p * s2 + 1.0 / 15.0;
    p = p * s2 + 1.0 / 13.0;
    p = p * s2 + 1.0 / 11.0;
    p = p * s2 + 1.0 / 9.0;
    p = p * s2 + 1.0 / 7.0;
    p = p * s2 + 1.0 / 5.0;
    p = p * s2 + 1.0 / 3.0;
    p = p * s2 + 1.0;
    const double logM = 2.0 * s * p;
    const double e = double(exponent);
    return (e * kLn2Hi + logM) + e * kLn2Lo;
}

double Pow(double base, double exponent) {
    if (exponent == 0.0) {
        return 1.0;
    }
    if (base == 0.0) {
        return 0.0;
    }
    return Exp(exponent * Log(base));
}

double Cbrt(double x) {
    if (x == 0.0) {
        return 0.0;
    }
    const double a = std::fabs(x);
    double y = Exp(Log(a) / 3.0);
    // One Newton step removes the error of the exp/log round trip.
    y = y - (y * y * y - a) / (3.0 * y * y);
    return x < 0.0 ? -y : y;
}

float Sin(float x) { return float(Sin(double(x))); }
float Cos(float x) { return float(Cos(double(x))); }
float Exp(float x) { return float(Exp(double(x))); }
float Cbrt(float x) { return float(Cbrt(double(x))); }
float Atan2(float y, float x) { return float(Atan2(double(y), double(x))); }
float Pow(float base, float exponent) { return float(Pow(double(base), double(exponent))); }

} // namespace ex::core::det
