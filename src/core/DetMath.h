#pragma once

namespace ex::core::det {

// Deterministic replacements for the libm functions used by the simulation
// state (blast model, scene generation, collapse, physics set-up). libm results
// differ between MSVC's UCRT and glibc in the last bits; these use only IEEE
// +, -, *, / and sqrt plus exact scaling (frexp/ldexp/floor), so Windows and
// Linux produce bit-identical values when the code is compiled without fast-math
// or FMA contraction. They are accurate to about 1e-15 relative, not correctly
// rounded. Domain: finite arguments; sin/cos expect |x| < 1e5.

double Sin(double x);
double Cos(double x);
double Atan2(double y, double x);
double Exp(double x);
double Log(double x);
double Pow(double base, double exponent);
double Cbrt(double x);

// Float overloads evaluate in double and round once, like the double versions.
float Sin(float x);
float Cos(float x);
float Exp(float x);
float Cbrt(float x);
float Atan2(float y, float x);
float Pow(float base, float exponent);

} // namespace ex::core::det
