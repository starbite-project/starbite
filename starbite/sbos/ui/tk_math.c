/* SPDX-License-Identifier: MIT
 * sbtk math without libm (the kernel has none): enough precision for drawing (about 1e-6 relative). */
#include "sbtk.h"

float tk_sqrtf(float x) { return x > 0 ? __builtin_sqrtf(x) : 0; }
float tk_floorf(float x) { return __builtin_floorf(x); }
float tk_fabsf(float x) { return x < 0 ? -x : x; }
float tk_fmodf(float x, float y) { return x - y * (float)(int64_t)(x / y); }

static float ldexpf_(float x, int e)
{
    union { float f; uint32_t u; } k;
    if (e < -126) return 0;
    if (e > 127) e = 127;
    k.u = (uint32_t)(e + 127) << 23;
    return x * k.f;
}

float tk_expf(float x)
{
    if (x < -87.0f) return 0;
    if (x > 88.0f) x = 88.0f;
    float n = __builtin_floorf(x * 1.44269504f + 0.5f);
    float r = x - n * 0.693145752f - n * 1.42860677e-6f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.166666672f + r * (0.0416666679f + r * (0.00833333377f + r * 0.00138888892f)))));
    return ldexpf_(p, (int)n);
}

static float logf_(float x)
{
    union { float f; uint32_t u; } k = { x };
    int e = (int)((k.u >> 23) & 255) - 127;
    k.u = (k.u & 0x7fffff) | 0x3f800000;     /* m in [1, 2) */
    float m = k.f;
    if (m > 1.41421356f) { m *= 0.5f; e++; }
    float t = (m - 1) / (m + 1), t2 = t * t;
    float s = 2 * t * (1 + t2 * (0.333333343f + t2 * (0.2f + t2 * (0.142857149f + t2 * 0.111111112f))));
    return s + (float)e * 0.693147181f;
}

float tk_powf(float x, float y)
{
    if (x <= 0) return x == 0 ? (y == 0 ? 1 : 0) : 0;
    return tk_expf(y * logf_(x));
}

float tk_sinf(float x)
{
    const float pi = 3.14159265f;
    x = x - 2 * pi * __builtin_floorf((x + pi) / (2 * pi));     /* [-pi, pi) */
    if (x > pi / 2) x = pi - x;
    else if (x < -pi / 2) x = -pi - x;
    float x2 = x * x;
    return x * (1 + x2 * (-0.166666667f + x2 * (0.00833333333f + x2 * (-0.000198412698f + x2 * 2.75573192e-6f))));
}

float tk_cosf(float x) { return tk_sinf(x + 1.57079633f); }

float tk_atan2f(float y, float x)
{
    const float pi = 3.14159265f;
    float ax = tk_fabsf(x), ay = tk_fabsf(y);
    if (ax == 0 && ay == 0) return 0;
    float a = ax > ay ? ay / ax : ax / ay, s = a * a;
    float r = a * (0.99997726f + s * (-0.33262347f + s * (0.19354346f + s * (-0.11643287f + s * (0.05265332f - s * 0.01172120f)))));
    if (ay > ax) r = pi / 2 - r;
    if (x < 0) r = pi - r;
    return y < 0 ? -r : r;
}

float tk_acosf(float x) { return tk_atan2f(tk_sqrtf(1 - x * x), x); }

size_t tk_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int tk_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
void tk_memcpy(void *d, const void *s, size_t n) { uint8_t *o = d; const uint8_t *i = s; while (n--) *o++ = *i++; }
void tk_memset(void *d, int v, size_t n) { uint8_t *o = d; while (n--) *o++ = (uint8_t)v; }
