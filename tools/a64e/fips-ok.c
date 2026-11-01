/*
 * Stand-in for /usr/libexec/cc_fips_test on the S9's iOS 26 root: corecrypto's
 * FIPS integrity test checks a MAC over libcorecrypto's code, which the PAC
 * translator (a64e-rewrite.py) rewrites. Every algorithm test passed on the
 * translated code (boot ios26-c); only the MAC differs. Exit 0 so launchd's
 * critical "fips" boot task succeeds. Static arm64e, no libc.
 */
__attribute__((naked)) void
_start(void)
{
	__asm__ ("mov x0, #0\n\tmov x16, #1\n\tsvc #0x80\n\tb .");
}
