/* See Copyright Notice in LICENSE.txt */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/time.h>

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("CRITICAL ERROR: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(1);
}

double time_delta(struct timeval *before, struct timeval *after) {
    double delta_seconds = after->tv_sec - before->tv_sec;
    double delta_milliseconds = (after->tv_usec - before->tv_usec) / 1000;

    if (delta_milliseconds < 0) {
        delta_milliseconds += 1000;
        delta_seconds--;
    }

    return delta_seconds * 1000 + delta_milliseconds;
}

void *xmalloc(size_t size) {
    void *ptr = calloc(1, size);
    if (!ptr) die("cannot malloc");
    return ptr;
}

