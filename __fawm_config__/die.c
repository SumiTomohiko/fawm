#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void
die(const char* fmt, ...)
{
    char s[4096];
    snprintf(s, sizeof(s), "%s\n", fmt);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, s, ap);
    va_end(ap);

    exit(1);
}

/**
 * vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4
 */
