/* Android code library (README.md): the rounding modes are FPCR.RMode's on AArch64, as in
 * Android's own fenv.h; the functions come from its libm.so at run time. */
#ifndef RC_ANDROID_FENV_H
#define RC_ANDROID_FENV_H
#define FE_TONEAREST  0x000000
#define FE_UPWARD     0x400000
#define FE_DOWNWARD   0x800000
#define FE_TOWARDZERO 0xc00000
int fegetround(void);
int fesetround(int round);
#endif
