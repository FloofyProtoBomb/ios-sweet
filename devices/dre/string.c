/* String primitives used by the freestanding read-only libfdt build. */
#include <stddef.h>

size_t strlen(const char *s)
{
    size_t i = 0;
    while (s[i]) i++;
    return i;
}

size_t strnlen(const char *s, size_t bound)
{
    size_t i = 0;
    while (i < bound && s[i]) i++;
    return i;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char *)last;
}

int memcmp(const void *a, const void *b, size_t size)
{
    const unsigned char *left = a, *right = b;
    for (size_t i = 0; i < size; i++)
        if (left[i] != right[i]) return left[i] - right[i];
    return 0;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

void *memchr(const void *s, int c, size_t size)
{
    const unsigned char *p = s;
    for (size_t i = 0; i < size; i++)
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}
