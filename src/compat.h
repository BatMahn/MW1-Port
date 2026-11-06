/* Small portable stand-ins for functions some C libraries lack (MinGW has neither). */
#ifndef COMPAT_H
#define COMPAT_H
#include <string.h>
#include <ctype.h>
static inline char *mw_strcasestr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    for (; *h; h++) { size_t i = 0; while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i])) i++; if (i == nl) return (char *)h; }
    return nl ? NULL : (char *)h;
}
static inline void *mw_memmem(const void *h, size_t hl, const void *n, size_t nl)
{
    const unsigned char *a = (const unsigned char *)h, *b = (const unsigned char *)n;
    if (!nl) return (void *)h;
    for (size_t i = 0; i + nl <= hl; i++) if (a[i] == b[0] && !memcmp(a + i, b, nl)) return (void *)(a + i);
    return NULL;
}
#define strcasestr mw_strcasestr
#define memmem mw_memmem
#ifdef _WIN32
#include <strings.h>
#endif
#endif
