/* SPDX-License-Identifier: GPL-2.0-only
 * Utility OS calculator: calc.c (also the host test's math checks). */
#pragma once

double cm_sqrt(double x);
double cm_exp(double x);
double cm_ln(double x);
double cm_sin(double x);
double cm_cos(double x);
double cm_pow(double a, double b);
int calc_eval(const char *s, double *out);              /* 0 ok, -1 malformed */
void calc_format(double v, char *out, unsigned cap);    /* 12 significant digits */
void ut_calc(void);
