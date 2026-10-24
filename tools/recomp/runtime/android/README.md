The C library headers the code library is compiled against for Android (`build.py --android`,
`build_game.py --android`), so that any clang, Zig's or llvm-mingw's included, builds it without
the Android NDK: the generated code only calls a few functions of the C library (memcpy, sqrt,
trunc, nearbyint, fesetround), which Android's own libc.so and libm.so give it at run time. They
declare what C says, with AArch64's values where there are some (the rounding modes).
