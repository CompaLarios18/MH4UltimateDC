/* Android code library (README.md): the string functions, from Android's libc.so at run time. */
#ifndef RC_ANDROID_STRING_H
#define RC_ANDROID_STRING_H
#include <stddef.h>
void *memcpy(void *restrict dest, const void *restrict src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#endif
