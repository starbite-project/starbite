/* SPDX-License-Identifier: MIT
 * tls/tls_check.c on the host: the offline TLS self-check must pass here as on the phone. */
#include <stdio.h>
#include <sys/time.h>
#include "../tls/tls_check.h"
static void lg(void *c, const char *l) { (void)c; printf("%s\n", l); }
static uint64_t ms(void *c)
{
    struct timeval tv;
    (void)c;
    gettimeofday(&tv, 0);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}
int main(void)
{
    int r = sbtls_selfcheck(lg, 0, ms);
    printf("tls: self-check %s (%d)\n", r ? "FAILED" : "passed", r);
    return r != 0;
}
