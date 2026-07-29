#include "verscmp.h"
#include <ctype.h>

// Pull the next numeric component from *s, advancing past it. Skips any
// non-digits first (so a leading 'v' or the '.' separators are ignored).
// Returns -1 when the string is exhausted.
static long next_part(const char **s)
{
    while (**s && !isdigit((unsigned char)**s)) (*s)++;
    if (!**s) return -1;
    long v = 0;
    while (isdigit((unsigned char)**s)) { v = v * 10 + (**s - '0'); (*s)++; }
    return v;
}

int version_cmp(const char *a, const char *b)
{
    for (;;) {
        long na = next_part(&a);
        long nb = next_part(&b);
        if (na < 0 && nb < 0) return 0;     // both exhausted -> equal
        if (na < 0) na = 0;                 // "1" vs "1.2": missing reads as 0
        if (nb < 0) nb = 0;
        if (na != nb) return (na < nb) ? -1 : 1;
    }
}
