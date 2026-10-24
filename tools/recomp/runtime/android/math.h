/* Android code library (README.md): C's math functions, from Android's libm.so at run time
 * (the compiler turns most of them into single instructions anyway). */
#ifndef RC_ANDROID_MATH_H
#define RC_ANDROID_MATH_H
#define NAN (__builtin_nanf(""))
#define INFINITY (__builtin_inff())
#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
double sqrt(double x);
float sqrtf(float x);
double fabs(double x);
float fabsf(float x);
double trunc(double x);
float truncf(float x);
double floor(double x);
float floorf(float x);
double ceil(double x);
float ceilf(float x);
double round(double x);
float roundf(float x);
double rint(double x);
float rintf(float x);
double nearbyint(double x);
float nearbyintf(float x);
double fma(double x, double y, double z);
float fmaf(float x, float y, float z);
double fmin(double x, double y);
float fminf(float x, float y);
double fmax(double x, double y);
float fmaxf(float x, float y);
double copysign(double x, double y);
float copysignf(float x, float y);
double fmod(double x, double y);
float fmodf(float x, float y);
double ldexp(double x, int e);
float ldexpf(float x, int e);
#endif
