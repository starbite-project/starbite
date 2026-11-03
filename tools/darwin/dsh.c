/*
 * dsh: the Darwin console's whole userland. One static arm64 Mach-O with no
 * libSystem, no dyld and no iOS root: raw XNU syscalls only.
 *
 * Booted as PID 1 from a RAM disk (tools/darwin/build-darwin.sh: rd=md0;
 * mockfs serves the RAM disk itself as /sbin/launchd and devfs is at /dev),
 * it opens /dev/console as stdin/stdout/stderr, prints the system (kernel,
 * CPUs, memory, boot-args), runs an autorun script, then sits at a "darwin#"
 * prompt on the console. The kernel console goes to the framebuffer (and to
 * the serial port if there is one). With no input device a status line is
 * printed every dshtick seconds, so the screen shows a live system.
 *
 * Every command runs in a forked child (a fault in a command never takes
 * PID 1, and init dying panics the kernel). "sh" and "exec" execve() this
 * same file (/sbin/launchd) again: any other PID or argv[0] runs as a shell
 * (with arguments: one command line, then exit).
 *
 * Boot-args read from kern.bootargs: dshtick=N (status line period in
 * seconds, 0 = off; default 30), dshauto=0 (no autorun), dshrun=a,b,c
 * (autorun these commands instead of the default; ',' separates commands,
 * '+' stands for a space: dshrun=sysinfo,ls+/dev).
 *
 * Host test (macOS arm64, the syscalls are the same XNU ones):
 *   tools/darwin/build-darwin.sh --host  ->  build/darwin/dsh-host
 */
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/dirent.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/termios.h>
#include <sys/time.h>
#include <sys/signal.h>
#include <sys/wait.h>
#include <sys/fcntl.h>
#include <sys/resource.h>

/* sys/reboot.h (not in the iOS SDK) */
#define RB_AUTOBOOT 0
#define RB_HALT     0x08

#define VERSION "dsh 1.0 (Project Starbite Darwin console)"

/* ---- raw syscalls ------------------------------------------------------- */

#define SYS_exit_         1
#define SYS_fork_         2
#define SYS_read_         3
#define SYS_write_        4
#define SYS_open_         5
#define SYS_close_        6
#define SYS_wait4_        7
#define SYS_chdir_        12
#define SYS_getpid_       20
#define SYS_kill_         37
#define SYS_getppid_      39
#define SYS_sigaction_    46
#define SYS_ioctl_        54
#define SYS_reboot_       55
#define SYS_execve_       59
#define SYS_dup2_         90
#define SYS_select_       93
#define SYS_gettimeofday_ 116
#define SYS_setsid_       147
#define SYS_sysctl_       202
#define SYS_sysctlbyname_ 274
#define SYS_proc_info_    336
#define SYS_stat64_       338
#define SYS_lstat64_      340
#define SYS_getdirentries64_ 344
#define SYS_getfsstat64_  347

/* Carry set = error: return -errno like Linux, so callers test < 0. */
static long
sc(long n, long a, long b, long c, long d, long e, long f, long *x1out)
{
	register long x16 __asm__("x16") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	long carry;
	__asm__ volatile("svc #0x80\n\tcset %[cy], cs"
	    : "+r"(x0), "+r"(x1), [cy] "=r"(carry)
	    : "r"(x16), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
	    : "memory", "cc");
	if (x1out) {
		*x1out = x1;
	}
	return carry ? -x0 : x0;
}
#define S0(n)             sc(n, 0, 0, 0, 0, 0, 0, 0)
#define S1(n, a)          sc(n, (long)(a), 0, 0, 0, 0, 0, 0)
#define S2(n, a, b)       sc(n, (long)(a), (long)(b), 0, 0, 0, 0, 0)
#define S3(n, a, b, c)    sc(n, (long)(a), (long)(b), (long)(c), 0, 0, 0, 0)
#define S4(n, a, b, c, d) sc(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0, 0)
#define S6(n, a, b, c, d, e, f) sc(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f), 0)

static __attribute__((noreturn)) void
k_exit(int code)
{
	S1(SYS_exit_, code);
	for (;;) {
	}
}

/* fork: x1 is 1 in the child (x0 then holds the parent's pid), 0 in the parent. */
static long
k_fork(void)
{
	long child = 0;
	long r = sc(SYS_fork_, 0, 0, 0, 0, 0, 0, &child);
	if (r < 0) {
		return r;
	}
	return child ? 0 : r;
}

static long k_read(int fd, void *b, size_t n) { return S3(SYS_read_, fd, b, n); }
static long k_write(int fd, const void *b, size_t n) { return S3(SYS_write_, fd, b, n); }
static long k_open(const char *p, int fl) { return S3(SYS_open_, p, fl, 0); }
static long k_close(int fd) { return S1(SYS_close_, fd); }
static long k_getpid(void) { return S0(SYS_getpid_); }

static long
k_sysctl(int *mib, unsigned n, void *old, size_t *oldlen, void *new, size_t newlen)
{
	return S6(SYS_sysctl_, mib, n, old, oldlen, new, newlen);
}

static long
k_sysctlbyname(const char *name, void *old, size_t *oldlen, void *new, size_t newlen)
{
	size_t l = 0;
	while (name[l]) {
		l++;
	}
	return S6(SYS_sysctlbyname_, name, l, old, oldlen, new, newlen);
}

/* ---- memory and strings (clang may emit calls to these) ---------------- */

#ifndef DSH_HOST
void *
memcpy(void *d, const void *s, size_t n)
{
	unsigned char *a = d;
	const unsigned char *b = s;
	while (n--) {
		*a++ = *b++;
	}
	return d;
}

void *
memmove(void *d, const void *s, size_t n)
{
	unsigned char *a = d;
	const unsigned char *b = s;
	if (a < b) {
		while (n--) {
			*a++ = *b++;
		}
	} else {
		while (n--) {
			a[n] = b[n];
		}
	}
	return d;
}

void *
memset(void *d, int c, size_t n)
{
	unsigned char *a = d;
	while (n--) {
		*a++ = (unsigned char)c;
	}
	return d;
}

void
bzero(void *d, size_t n)
{
	memset(d, 0, n);
}
#endif

static size_t
slen(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
seq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int
spre(const char *s, const char *pre)
{
	while (*pre) {
		if (*s++ != *pre++) {
			return 0;
		}
	}
	return 1;
}

static void
scopy(char *d, const char *s, size_t cap)
{
	size_t i = 0;
	while (s[i] && i + 1 < cap) {
		d[i] = s[i];
		i++;
	}
	d[i] = 0;
}

static long
satol(const char *s, int *ok)
{
	long v = 0, neg = 0;
	int any = 0;
	if (*s == '-') {
		neg = 1;
		s++;
	}
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		for (; *s; s++, any = 1) {
			int d = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) >= 'a' && (*s | 32) <= 'f' ? (*s | 32) - 'a' + 10 : -1;
			if (d < 0) {
				break;
			}
			v = v * 16 + d;
		}
	} else {
		for (; *s >= '0' && *s <= '9'; s++, any = 1) {
			v = v * 10 + (*s - '0');
		}
	}
	if (ok) {
		*ok = any && !*s;
	}
	return neg ? -v : v;
}

/* ---- buffered output and a small printf -------------------------------- */

static char obuf[4096];
static size_t olen;
static int ofd = 1;

static void
flush(void)
{
	size_t off = 0;
	while (off < olen) {
		long r = k_write(ofd, obuf + off, olen - off);
		if (r <= 0) {
			break;
		}
		off += (size_t)r;
	}
	olen = 0;
}

static void
outc(char c)
{
	if (olen == sizeof(obuf)) {
		flush();
	}
	obuf[olen++] = c;
	if (c == '\n' && olen > sizeof(obuf) / 2) {
		flush();
	}
}

static void
outs(const char *s)
{
	while (*s) {
		outc(*s++);
	}
}

static void
outn(const char *s, size_t n)
{
	while (n--) {
		outc(*s++);
	}
}

static void
pad(int n, char c)
{
	while (n-- > 0) {
		outc(c);
	}
}

static int
fmt_u(char *b, unsigned long long v, int base)
{
	char t[24];
	int n = 0, i = 0;
	do {
		t[n++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	while (n) {
		b[i++] = t[--n];
	}
	b[i] = 0;
	return i;
}

/* %[-][0][width][l|ll|z](d u x s c) and %% */
static void
vpf(const char *f, va_list ap)
{
	for (; *f; f++) {
		if (*f != '%') {
			outc(*f);
			continue;
		}
		f++;
		int left = 0, zero = 0, w = 0, lng = 0;
		if (*f == '-') {
			left = 1;
			f++;
		}
		if (*f == '0') {
			zero = 1;
			f++;
		}
		while (*f >= '0' && *f <= '9') {
			w = w * 10 + (*f++ - '0');
		}
		while (*f == 'l' || *f == 'z') {
			lng++;
			f++;
		}
		char b[32];
		const char *s = b;
		int n;
		switch (*f) {
		case 'd': {
			long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
			if (v < 0) {
				b[0] = '-';
				n = 1 + fmt_u(b + 1, (unsigned long long)-v, 10);
			} else {
				n = fmt_u(b, (unsigned long long)v, 10);
			}
			break;
		}
		case 'u':
		case 'x': {
			unsigned long long v = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
			n = fmt_u(b, v, *f == 'x' ? 16 : 10);
			break;
		}
		case 'c':
			b[0] = (char)va_arg(ap, int);
			b[1] = 0;
			n = 1;
			break;
		case 's':
			s = va_arg(ap, const char *);
			if (!s) {
				s = "(null)";
			}
			n = (int)slen(s);
			break;
		case '%':
			outc('%');
			continue;
		default:
			outc('%');
			outc(*f);
			continue;
		}
		if (!left) {
			pad(w - n, zero ? '0' : ' ');
		}
		outn(s, (size_t)n);
		if (left) {
			pad(w - n, ' ');
		}
	}
}

static void __attribute__((format(printf, 1, 2)))
pf(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	vpf(f, ap);
	va_end(ap);
}

static const char *
errstr(long e)
{
	switch (e < 0 ? -e : e) {
	case EPERM: return "operation not permitted";
	case ENOENT: return "no such file or directory";
	case ESRCH: return "no such process";
	case EIO: return "input/output error";
	case ENXIO: return "device not configured";
	case E2BIG: return "argument list too long";
	case ENOEXEC: return "exec format error";
	case EBADF: return "bad file descriptor";
	case ECHILD: return "no child processes";
	case ENOMEM: return "cannot allocate memory";
	case EACCES: return "permission denied";
	case EFAULT: return "bad address";
	case EBUSY: return "resource busy";
	case EEXIST: return "file exists";
	case ENOTDIR: return "not a directory";
	case EISDIR: return "is a directory";
	case EINVAL: return "invalid argument";
	case ENOTTY: return "not a tty";
	case EROFS: return "read-only file system";
	case EAGAIN: return "resource temporarily unavailable";
	case ENOTSUP: return "operation not supported";
	case ENAMETOOLONG: return "file name too long";
	case ENOSYS: return "function not implemented";
	default: return "error";
	}
}

static void
perr(const char *what, long e)
{
	pf("%s: %s (%d)\n", what, errstr(e), (int)(e < 0 ? -e : e));
}

/* ---- time ---------------------------------------------------------------- */

static long
now_us(void)
{
	struct timeval tv = { 0, 0 };
	S3(SYS_gettimeofday_, &tv, 0, 0);
	return (long)tv.tv_sec * 1000000L + tv.tv_usec;
}

static void
sleep_ms(long ms)
{
	struct timeval tv = { ms / 1000, (int)(ms % 1000) * 1000 };
	S6(SYS_select_, 0, 0, 0, 0, &tv, 0);
}

/* "3d 04:05:06" */
static void
put_duration(long s)
{
	if (s >= 86400) {
		pf("%ldd ", s / 86400);
	}
	pf("%02ld:%02ld:%02ld", (s / 3600) % 24, (s / 60) % 60, s % 60);
}

/* UTC calendar date from the epoch (days-from-civil inverse). */
static void
put_date(long t)
{
	long days = t / 86400, rem = t % 86400;
	long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
	long doe = z - era * 146097;
	long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
	long m = mp < 10 ? mp + 3 : mp - 9;
	pf("%04ld-%02ld-%02ld %02ld:%02ld:%02ld UTC", y + (m <= 2), m, d, rem / 3600, (rem / 60) % 60, rem % 60);
}

/* ---- sysctl helpers ------------------------------------------------------ */

static char sbuf[1 << 20];        /* sysctl values, directory entries, file reads */

static long
sysctl_str(const char *name, char *out, size_t cap)
{
	size_t len = cap - 1;
	long r = k_sysctlbyname(name, out, &len, 0, 0);
	if (r < 0) {
		out[0] = 0;
		return r;
	}
	out[len < cap ? len : cap - 1] = 0;
	return 0;
}

static long long
sysctl_num(const char *name, long long dflt)
{
	union {
		int32_t i;
		int64_t q;
	} u = { .q = 0 };
	size_t len = sizeof(u);
	if (k_sysctlbyname(name, &u, &len, 0, 0) < 0) {
		return dflt;
	}
	return len == 4 ? u.i : u.q;
}

static long
boottime(void)
{
	struct timeval tv = { 0, 0 };
	size_t len = sizeof(tv);
	if (k_sysctlbyname("kern.boottime", &tv, &len, 0, 0) < 0) {
		return 0;
	}
	return tv.tv_sec;
}

static char bootargs[1024];

/* Value of boot-arg key= (or "1" for a bare -key / key), 0 when absent. */
static const char *
bootarg(const char *key, char *out, size_t cap)
{
	size_t kl = slen(key);
	const char *p = bootargs;
	while (*p) {
		while (*p == ' ') {
			p++;
		}
		const char *w = p;
		while (*p && *p != ' ') {
			p++;
		}
		if ((size_t)(p - w) >= kl && spre(w, key)) {
			if (w[kl] == '=') {
				size_t n = (size_t)(p - w) - kl - 1;
				if (n >= cap) {
					n = cap - 1;
				}
				memcpy(out, w + kl + 1, n);
				out[n] = 0;
				return out;
			}
			if (w + kl == p) {
				scopy(out, "1", cap);
				return out;
			}
		}
	}
	return 0;
}

/* ---- commands -------------------------------------------------------------- */

typedef int (*cmd_fn)(int argc, char **argv);
struct cmd {
	const char *name;
	cmd_fn fn;
	const char *help;
	int in_parent;          /* runs in the shell itself (changes its state) */
};
static const struct cmd cmds[];
static int run_line(char *line);
static int g_pid1;
static int g_fake_init;         /* host test: run the init path without being PID 1 */
static int g_depth;             /* nested shells */
static long g_started_us;

static const char *const logo[] = {
	"  ____                       _        ",
	" |  _ \\  __ _ _ ____      _(_)_ __   ",
	" | | | |/ _` | '__\\ \\ /\\ / / | '_ \\  ",
	" | |_| | (_| | |   \\ V  V /| | | | | ",
	" |____/ \\__,_|_|    \\_/\\_/ |_|_| |_| ",
	0
};

static int
c_logo(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	pf("\n");
	for (int i = 0; logo[i]; i++) {
		pf("%s\n", logo[i]);
	}
	pf("   on the Galaxy S9 - Project Starbite\n\n");
	return 0;
}

static int
c_help(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	pf("%s\ncommands:\n", VERSION);
	for (const struct cmd *c = cmds; c->name; c++) {
		pf("  %-10s %s\n", c->name, c->help);
	}
	pf("separate commands with ';'. Every command runs in a child process.\n");
	return 0;
}

static int
c_echo(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		pf(i > 1 ? " %s" : "%s", argv[i]);
	}
	pf("\n");
	return 0;
}

static int
c_uname(int argc, char **argv)
{
	char os[64], host[128], rel[64], ver[512], mach[64];
	sysctl_str("kern.ostype", os, sizeof(os));
	sysctl_str("kern.hostname", host, sizeof(host));
	sysctl_str("kern.osrelease", rel, sizeof(rel));
	sysctl_str("kern.version", ver, sizeof(ver));
	sysctl_str("hw.machine", mach, sizeof(mach));
	if (argc > 1 && seq(argv[1], "-a")) {
		pf("%s %s %s %s %s\n", os, host, rel, ver, mach);
	} else if (argc > 1 && seq(argv[1], "-r")) {
		pf("%s\n", rel);
	} else if (argc > 1 && seq(argv[1], "-m")) {
		pf("%s\n", mach);
	} else if (argc > 1 && seq(argv[1], "-v")) {
		pf("%s\n", ver);
	} else {
		pf("%s\n", os);
	}
	return 0;
}

static void
put_mib(const char *label, long long bytes)
{
	pf("%s%lld MiB\n", label, bytes >> 20);
}

static int
c_mem(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	long long page = sysctl_num("hw.pagesize", 16384);
	long long total = sysctl_num("hw.memsize", 0);
	long long freep = sysctl_num("vm.page_free_count", -1);
	long long spec = sysctl_num("vm.page_speculative_count", 0);
	long long wire = sysctl_num("vm.page_wire_count", -1);
	put_mib("memory      ", total);
	pf("page size   %lld\n", page);
	if (freep >= 0) {
		put_mib("free        ", (freep + spec) * page);
	}
	if (wire >= 0) {
		put_mib("wired       ", wire * page);
	}
	long long pressure = sysctl_num("kern.memorystatus_level", -1);
	if (pressure >= 0) {
		pf("available   %lld %%\n", pressure);
	}
	return 0;
}

static int
c_cpus(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	pf("cpus        %lld (active %lld, physical %lld)\n", sysctl_num("hw.ncpu", 0),
	    sysctl_num("hw.activecpu", 0), sysctl_num("hw.physicalcpu", 0));
	pf("cputype     0x%llx subtype %lld family 0x%llx\n", sysctl_num("hw.cputype", 0),
	    sysctl_num("hw.cpusubtype", 0), (unsigned long long)sysctl_num("hw.cpufamily", 0) & 0xffffffffULL);
	long long levels = sysctl_num("hw.nperflevels", 0);
	for (long long i = 0; i < levels && i < 4; i++) {
		char k[64], name[64];
		scopy(k, "hw.perflevel0.name", sizeof(k));
		k[12] = (char)('0' + i);
		sysctl_str(k, name, sizeof(name));
		scopy(k, "hw.perflevel0.logicalcpu", sizeof(k));
		k[12] = (char)('0' + i);
		pf("perflevel%lld  %s: %lld cpus\n", i, name[0] ? name : "?", sysctl_num(k, 0));
	}
	long long tb = sysctl_num("hw.tbfrequency", 0);
	if (tb) {
		pf("timebase    %lld Hz\n", tb);
	}
	return 0;
}

static int
c_uptime(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	long bt = boottime(), t = now_us() / 1000000L;
	struct loadavg la = { { 0, 0, 0 }, 1 };
	size_t len = sizeof(la);
	k_sysctlbyname("vm.loadavg", &la, &len, 0, 0);
	pf("up ");
	put_duration(bt ? t - bt : 0);
	long scale = la.fscale ? la.fscale : 1;
	pf("  load %ld.%02ld %ld.%02ld %ld.%02ld\n",
	    (long)la.ldavg[0] / scale, (long)(la.ldavg[0] % scale) * 100 / scale,
	    (long)la.ldavg[1] / scale, (long)(la.ldavg[1] % scale) * 100 / scale,
	    (long)la.ldavg[2] / scale, (long)(la.ldavg[2] % scale) * 100 / scale);
	return 0;
}

static int
c_date(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	put_date(now_us() / 1000000L);
	pf("\n");
	return 0;
}

static int
c_bootargs(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	pf("%s\n", bootargs);
	return 0;
}

static int
c_sysinfo(int argc, char **argv)
{
	char s[512];
	sysctl_str("kern.version", s, sizeof(s));
	pf("kernel      %s\n", s);
	sysctl_str("kern.osversion", s, sizeof(s));
	if (s[0]) {
		pf("os build    %s\n", s);
	}
	sysctl_str("hw.machine", s, sizeof(s));
	pf("machine     %s", s);
	sysctl_str("hw.model", s, sizeof(s));
	pf(" (%s)\n", s);
	sysctl_str("kern.hostname", s, sizeof(s));
	pf("hostname    %s\n", s);
	c_cpus(argc, argv);
	c_mem(argc, argv);
	pf("boot-args   %s\n", bootargs);
	pf("booted      ");
	put_date(boottime());
	pf("\n");
	c_uptime(argc, argv);
	return 0;
}

/* sysctl: names, a whole subtree (-a [prefix]), or name=value for int/string. */
static int
oid_fmt(int *mib, unsigned n, unsigned *kind, char *fmt, size_t cap)
{
	int q[CTL_MAXNAME + 2] = { 0, 4 };
	char b[256];
	size_t len = sizeof(b);
	for (unsigned i = 0; i < n; i++) {
		q[i + 2] = mib[i];
	}
	if (k_sysctl(q, n + 2, b, &len, 0, 0) < 0 || len < 4) {
		return -1;
	}
	memcpy(kind, b, 4);
	scopy(fmt, b + 4, cap);
	return 0;
}

static int
oid_name(int *mib, unsigned n, char *out, size_t cap)
{
	int q[CTL_MAXNAME + 2] = { 0, 1 };
	size_t len = cap;
	for (unsigned i = 0; i < n; i++) {
		q[i + 2] = mib[i];
	}
	if (k_sysctl(q, n + 2, out, &len, 0, 0) < 0) {
		return -1;
	}
	return 0;
}

static void
show_value(const char *name, int *mib, unsigned n)
{
	unsigned kind = 0;
	char fmt[64] = "";
	if (oid_fmt(mib, n, &kind, fmt, sizeof(fmt)) < 0) {
		return;
	}
	unsigned type = kind & CTLTYPE;
	if (type == CTLTYPE_NODE) {
		return;
	}
	size_t len = sizeof(sbuf) - 1;
	if (k_sysctl(mib, n, sbuf, &len, 0, 0) < 0) {
		return;
	}
	pf("%s: ", name);
	if (type == CTLTYPE_STRING) {
		sbuf[len] = 0;
		pf("%s\n", sbuf);
	} else if (type == CTLTYPE_INT) {
		for (size_t i = 0; i + 4 <= len; i += 4) {
			int32_t v;
			memcpy(&v, sbuf + i, 4);
			if (fmt[0] == 'I' && fmt[1] == 'U') {
				pf(i ? " %u" : "%u", (unsigned)v);
			} else {
				pf(i ? " %d" : "%d", v);
			}
		}
		pf("\n");
	} else if (type == CTLTYPE_QUAD) {
		for (size_t i = 0; i + 8 <= len; i += 8) {
			int64_t v;
			memcpy(&v, sbuf + i, 8);
			pf(i ? " %lld" : "%lld", (long long)v);
		}
		pf("\n");
	} else if (seq(fmt, "S,timeval") && len >= sizeof(struct timeval)) {
		struct timeval tv;
		memcpy(&tv, sbuf, sizeof(tv));
		pf("{ sec = %ld, usec = %d } ", (long)tv.tv_sec, (int)tv.tv_usec);
		put_date(tv.tv_sec);
		pf("\n");
	} else if (seq(fmt, "S,loadavg") && len >= sizeof(struct loadavg)) {
		struct loadavg la;
		memcpy(&la, sbuf, sizeof(la));
		pf("{ %u %u %u / %ld }\n", la.ldavg[0], la.ldavg[1], la.ldavg[2], (long)la.fscale);
	} else {
		pf("<%s, %zu bytes>", fmt[0] ? fmt : "opaque", len);
		for (size_t i = 0; i < len && i < 32; i++) {
			pf(" %02x", (unsigned char)sbuf[i]);
		}
		pf(len > 32 ? " ...\n" : "\n");
	}
}

static int
name2mib(const char *name, int *mib, unsigned *n)
{
	int q[2] = { 0, 3 };
	size_t len = CTL_MAXNAME * sizeof(int);
	if (k_sysctl(q, 2, mib, &len, (void *)name, slen(name)) < 0) {
		return -1;
	}
	*n = (unsigned)(len / sizeof(int));
	return 0;
}

static int
sysctl_all(const char *prefix)
{
	int mib[CTL_MAXNAME] = { 1 }, next[CTL_MAXNAME];
	unsigned n = 1;
	size_t plen = slen(prefix);
	if (plen && name2mib(prefix, mib, &n) < 0) {
		pf("sysctl: %s: unknown oid\n", prefix);
		return 1;
	}
	if (plen) {
		char name[256];
		if (oid_name(mib, n, name, sizeof(name)) == 0) {
			unsigned kind;
			char fmt[64];
			if (oid_fmt(mib, n, &kind, fmt, sizeof(fmt)) == 0 && (kind & CTLTYPE) != CTLTYPE_NODE) {
				show_value(name, mib, n);
				return 0;
			}
		}
	}
	unsigned count = 0;
	for (;;) {
		int q[CTL_MAXNAME + 2] = { 0, 2 };
		size_t len = sizeof(next);
		for (unsigned i = 0; i < n; i++) {
			q[i + 2] = mib[i];
		}
		if (k_sysctl(q, n + 2, next, &len, 0, 0) < 0 || !len) {
			break;
		}
		n = (unsigned)(len / sizeof(int));
		memcpy(mib, next, len);
		char name[256];
		if (oid_name(mib, n, name, sizeof(name)) < 0) {
			continue;
		}
		if (plen && !(spre(name, prefix) && (name[plen] == '.' || !name[plen]))) {
			break;
		}
		show_value(name, mib, n);
		count++;
	}
	return count ? 0 : 1;
}

static int
c_sysctl(int argc, char **argv)
{
	if (argc < 2) {
		pf("usage: sysctl -a [prefix] | name[=value] ...\n");
		return 1;
	}
	if (seq(argv[1], "-a")) {
		return sysctl_all(argc > 2 ? argv[2] : "");
	}
	int rc = 0;
	for (int i = 1; i < argc; i++) {
		char name[256];
		char *eq = argv[i];
		while (*eq && *eq != '=') {
			eq++;
		}
		size_t nl = (size_t)(eq - argv[i]);
		if (nl >= sizeof(name)) {
			nl = sizeof(name) - 1;
		}
		memcpy(name, argv[i], nl);
		name[nl] = 0;
		int mib[CTL_MAXNAME];
		unsigned n;
		if (name2mib(name, mib, &n) < 0) {
			pf("sysctl: %s: unknown oid\n", name);
			rc = 1;
			continue;
		}
		if (*eq == '=') {
			unsigned kind = 0;
			char fmt[64];
			long r;
			oid_fmt(mib, n, &kind, fmt, sizeof(fmt));
			if ((kind & CTLTYPE) == CTLTYPE_STRING) {
				r = k_sysctl(mib, n, 0, 0, eq + 1, slen(eq + 1));
			} else if ((kind & CTLTYPE) == CTLTYPE_INT || (kind & CTLTYPE) == CTLTYPE_QUAD) {
				int ok;
				long long v = satol(eq + 1, &ok);
				int32_t v32 = (int32_t)v;
				if (!ok) {
					pf("sysctl: %s: not a number\n", eq + 1);
					rc = 1;
					continue;
				}
				r = (kind & CTLTYPE) == CTLTYPE_INT ? k_sysctl(mib, n, 0, 0, &v32, 4) : k_sysctl(mib, n, 0, 0, &v, 8);
			} else {
				pf("sysctl: %s: only int and string values can be set\n", name);
				rc = 1;
				continue;
			}
			if (r < 0) {
				perr(name, r);
				rc = 1;
				continue;
			}
		}
		show_value(name, mib, n);
	}
	return rc;
}

static const char *
pstate(char s)
{
	switch (s) {
	case SIDL: return "I";
	case SRUN: return "R";
	case SSLEEP: return "S";
	case SSTOP: return "T";
	case SZOMB: return "Z";
	default: return "?";
	}
}

static int
c_ps(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t len = sizeof(sbuf);
	long r = k_sysctl(mib, 4, sbuf, &len, 0, 0);
	if (r < 0) {
		perr("ps", r);
		return 1;
	}
	size_t n = len / sizeof(struct kinfo_proc);
	pf("  PID  PPID  UID STAT  TIME     COMMAND\n");
	long t = now_us() / 1000000L;
	for (size_t i = n; i-- > 0;) {    /* the kernel lists newest first */
		struct kinfo_proc kp;
		memcpy(&kp, sbuf + i * sizeof(kp), sizeof(kp));
		long start = kp.kp_proc.p_un.__p_starttime.tv_sec;
		pf("%5d %5d %4u %-4s  ", kp.kp_proc.p_pid, kp.kp_eproc.e_ppid, kp.kp_eproc.e_pcred.p_ruid,
		    pstate(kp.kp_proc.p_stat));
		put_duration(start && t >= start ? t - start : 0);
		pf("  %s\n", kp.kp_proc.p_comm);
	}
	pf("%zu processes\n", n);
	return 0;
}

static int
c_dmesg(int argc, char **argv)
{
	long want = argc > 1 ? satol(argv[1], 0) : 40;  /* last N lines, 0 = all */
	long r = S6(SYS_proc_info_, 4 /* PROC_INFO_CALL_KERNMSGBUF */, 0, 0, 0, sbuf, sizeof(sbuf) - 1);
	if (r < 0) {
		perr("dmesg", r);
		return 1;
	}
	size_t len = 0;
	while (len < sizeof(sbuf) - 1 && sbuf[len]) {
		len++;
	}
	size_t start = 0;
	if (want > 0) {
		long lines = 0;
		for (size_t i = len; i-- > 0;) {
			if (sbuf[i] == '\n' && ++lines > want) {
				start = i + 1;
				break;
			}
		}
	}
	for (size_t i = start; i < len; i++) {
		if (sbuf[i]) {
			outc(sbuf[i]);
		}
	}
	if (len && sbuf[len - 1] != '\n') {
		outc('\n');
	}
	return 0;
}

static void
put_mode(mode_t m)
{
	char s[11] = "----------";
	switch (m & S_IFMT) {
	case S_IFDIR: s[0] = 'd'; break;
	case S_IFCHR: s[0] = 'c'; break;
	case S_IFBLK: s[0] = 'b'; break;
	case S_IFLNK: s[0] = 'l'; break;
	case S_IFIFO: s[0] = 'p'; break;
	case S_IFSOCK: s[0] = 's'; break;
	}
	for (int i = 0; i < 9; i++) {
		if (m & (0400 >> i)) {
			s[1 + i] = "rwxrwxrwx"[i];
		}
	}
	outs(s);
}

static void
put_stat(const char *path, const char *shown, int lng)
{
	struct stat st;
	long r = S2(SYS_lstat64_, path, &st);
	if (!lng) {
		pf("%s%s\n", shown, r >= 0 && S_ISDIR(st.st_mode) ? "/" : "");
		return;
	}
	if (r < 0) {
		pf("?????????? %s\n", shown);
		return;
	}
	put_mode(st.st_mode);
	if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
		pf(" %3u %4u %4u %3d,%4d %s\n", st.st_nlink, st.st_uid, st.st_gid,
		    (int)((st.st_rdev >> 24) & 0xff), (int)(st.st_rdev & 0xffffff), shown);
	} else {
		pf(" %3u %4u %4u %8lld %s\n", st.st_nlink, st.st_uid, st.st_gid, (long long)st.st_size, shown);
	}
}

static int
c_ls(int argc, char **argv)
{
	int lng = 0, rc = 0, paths = 0;
	for (int i = 1; i < argc; i++) {
		if (seq(argv[i], "-l")) {
			lng = 1;
		}
	}
	for (int i = 1; i <= argc; i++) {
		const char *p = i < argc ? argv[i] : ".";
		if (i < argc && argv[i][0] == '-') {
			continue;
		}
		if (i == argc && paths) {
			break;
		}
		paths++;
		struct stat st;
		long r = S2(SYS_stat64_, p, &st);
		if (r < 0) {
			perr(p, r);
			rc = 1;
			continue;
		}
		if (!S_ISDIR(st.st_mode)) {
			put_stat(p, p, lng);
			continue;
		}
		long fd = k_open(p, O_RDONLY | O_DIRECTORY);
		if (fd < 0) {
			perr(p, fd);
			rc = 1;
			continue;
		}
		static char db[8192];
		off_t base = 0;
		for (;;) {
			long n = S4(SYS_getdirentries64_, fd, db, sizeof(db), &base);
			if (n <= 0) {
				if (n < 0) {
					perr(p, n);
					rc = 1;
				}
				break;
			}
			for (long off = 0; off < n;) {
				struct dirent *d = (struct dirent *)(void *)(db + off);
				if (!d->d_reclen) {
					break;
				}
				off += d->d_reclen;
				if (d->d_name[0] == '.' && (!d->d_name[1] || (d->d_name[1] == '.' && !d->d_name[2]))) {
					continue;
				}
				char full[1100];
				size_t pl = slen(p);
				scopy(full, p, sizeof(full));
				if (pl && full[pl - 1] != '/' && pl + 1 < sizeof(full)) {
					full[pl++] = '/';
					full[pl] = 0;
				}
				scopy(full + pl, d->d_name, sizeof(full) - pl);
				put_stat(full, d->d_name, lng);
			}
		}
		k_close((int)fd);
	}
	return rc;
}

static int
c_cat(int argc, char **argv)
{
	int rc = 0;
	for (int i = 1; i < argc; i++) {
		long fd = k_open(argv[i], O_RDONLY);
		if (fd < 0) {
			perr(argv[i], fd);
			rc = 1;
			continue;
		}
		long n;
		while ((n = k_read((int)fd, sbuf, sizeof(sbuf))) > 0) {
			outn(sbuf, (size_t)n);
		}
		if (n < 0) {
			perr(argv[i], n);
			rc = 1;
		}
		k_close((int)fd);
	}
	return rc;
}

static int
c_xxd(int argc, char **argv)
{
	if (argc < 2) {
		pf("usage: xxd FILE [bytes]\n");
		return 1;
	}
	long max = argc > 2 ? satol(argv[2], 0) : 256;
	long fd = k_open(argv[1], O_RDONLY);
	if (fd < 0) {
		perr(argv[1], fd);
		return 1;
	}
	long total = 0, n;
	unsigned char line[16];
	while (total < max && (n = k_read((int)fd, line, (size_t)(max - total < 16 ? max - total : 16))) > 0) {
		pf("%08lx: ", total);
		for (int j = 0; j < 16; j++) {
			if (j < n) {
				pf("%02x", line[j]);
			} else {
				pf("  ");
			}
			if (j & 1) {
				outc(' ');
			}
		}
		for (int j = 0; j < n; j++) {
			outc(line[j] >= 32 && line[j] < 127 ? (char)line[j] : '.');
		}
		outc('\n');
		total += n;
	}
	k_close((int)fd);
	return 0;
}

static int
c_mount(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	static struct statfs fs[16];
	long n = S3(SYS_getfsstat64_, fs, sizeof(fs), 2 /* MNT_NOWAIT */);
	if (n < 0) {
		perr("mount", n);
		return 1;
	}
	for (long i = 0; i < n && i < 16; i++) {
		long long bs = fs[i].f_bsize;
		pf("%s on %s (%s%s) %lld MiB, %lld MiB free\n", fs[i].f_mntfromname, fs[i].f_mntonname,
		    fs[i].f_fstypename, (fs[i].f_flags & MNT_RDONLY) ? ", read-only" : "",
		    (long long)fs[i].f_blocks * bs >> 20, (long long)fs[i].f_bavail * bs >> 20);
	}
	return 0;
}

static int
c_sleep(int argc, char **argv)
{
	sleep_ms(argc > 1 ? satol(argv[1], 0) * 1000 : 1000);
	return 0;
}

static int
c_kill(int argc, char **argv)
{
	if (argc < 2) {
		pf("usage: kill [-SIG] PID ...\n");
		return 1;
	}
	int sig = SIGTERM, i = 1, rc = 0;
	if (argv[1][0] == '-') {
		sig = (int)satol(argv[1] + 1, 0);
		i++;
	}
	for (; i < argc; i++) {
		long pid = satol(argv[i], 0);
		if (pid == 1) {
			pf("kill: not killing init\n");
			rc = 1;
			continue;
		}
		long r = S3(SYS_kill_, pid, sig, 1);
		if (r < 0) {
			perr(argv[i], r);
			rc = 1;
		}
	}
	return rc;
}

static int
c_pid(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	pf("pid %ld, parent %ld\n", k_getpid(), S0(SYS_getppid_));
	return 0;
}

static int
c_env(int argc, char **argv);

static unsigned long
spin(unsigned long iters)
{
	unsigned long x = 0x9e3779b97f4a7c15UL;
	for (unsigned long i = 0; i < iters; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		__asm__ volatile("" : "+r"(x));
	}
	return x;
}

/* One worker per CPU (fork), each doing the same fixed work: 1 vs N wall time. */
static int
c_bench(int argc, char **argv)
{
	long ncpu = argc > 1 ? satol(argv[1], 0) : (long)sysctl_num("hw.ncpu", 1);
	const unsigned long iters = 100000000UL;
	if (ncpu < 1 || ncpu > 64) {
		ncpu = 1;
	}
	for (int round = 0; round < 2; round++) {
		long workers = round ? ncpu : 1, t0 = now_us();
		pf("bench: %ld worker%s x %lu xorshift steps ... ", workers, workers > 1 ? "s" : "", iters);
		flush();
		for (long w = 0; w < workers; w++) {
			long pid = k_fork();
			if (pid == 0) {
				k_exit((int)(spin(iters) & 1));
			}
			if (pid < 0) {
				perr("fork", pid);
				break;
			}
		}
		int st;
		while (S4(SYS_wait4_, -1, &st, 0, 0) > 0) {
		}
		long dt = now_us() - t0;
		pf("%ld.%03ld s (%ld M steps/s)\n", dt / 1000000, (dt / 1000) % 1000,
		    dt ? (long)((unsigned long)workers * iters / (unsigned long)dt) : 0);
		if (ncpu == 1) {
			break;
		}
	}
	return 0;
}

static int
c_reboot(int argc, char **argv)
{
	(void)argc;
	int halt = seq(argv[0], "halt");
	pf("%s ...\n", halt ? "halting" : "rebooting");
	flush();
	long r = S2(SYS_reboot_, halt ? RB_HALT : RB_AUTOBOOT, 0);
	perr(argv[0], r);
	return 1;
}

static int
c_clear(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	pad(60, '\n');
	return 0;
}

static void status_line(void);

static int
c_status(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	status_line();
	return 0;
}

static int c_true(int argc, char **argv) { (void)argc; (void)argv; return 0; }

static char self_path[256] = "/sbin/launchd";
static char **g_envp;

/* sh / exec: execve this same program again ("sh" in a child, "exec" replaces a nested shell). */
static int
c_sh(int argc, char **argv)
{
	char *args[16];
	int n = 0;
	args[n++] = "dsh";
	for (int i = 1; i < argc && n < 15; i++) {
		args[n++] = argv[i];
	}
	args[n] = 0;
	flush();
	long r = S3(SYS_execve_, self_path, args, g_envp);
	perr(self_path, r);
	return 127;
}

static int
c_exit(int argc, char **argv)
{
	if (g_pid1) {
		pf("exit: this is init (PID 1): use reboot or halt\n");
		return 1;
	}
	flush();
	k_exit(argc > 1 ? (int)satol(argv[1], 0) : 0);
}

static const struct cmd cmds[] = {
	{ "help", c_help, "this list", 0 },
	{ "sysinfo", c_sysinfo, "kernel, machine, CPUs, memory, boot-args, uptime", 0 },
	{ "uname", c_uname, "[-a|-r|-m|-v] system name", 0 },
	{ "sysctl", c_sysctl, "-a [prefix] | name[=value] ...", 0 },
	{ "ps", c_ps, "processes", 0 },
	{ "dmesg", c_dmesg, "[N] last N lines of the kernel log (0 = all)", 0 },
	{ "ls", c_ls, "[-l] [path ...]", 0 },
	{ "cat", c_cat, "file ...", 0 },
	{ "xxd", c_xxd, "file [bytes] hex dump", 0 },
	{ "mount", c_mount, "mounted file systems", 0 },
	{ "cpus", c_cpus, "processors", 0 },
	{ "mem", c_mem, "memory", 0 },
	{ "uptime", c_uptime, "time since boot and load", 0 },
	{ "status", c_status, "the one-line status the idle console prints", 0 },
	{ "date", c_date, "current time (UTC)", 0 },
	{ "bootargs", c_bootargs, "the kernel command line", 0 },
	{ "bench", c_bench, "[N] integer benchmark on 1 then N CPUs (fork)", 0 },
	{ "echo", c_echo, "text", 0 },
	{ "sleep", c_sleep, "seconds", 0 },
	{ "kill", c_kill, "[-SIG] pid ...", 0 },
	{ "pid", c_pid, "this shell's pid", 1 },
	{ "env", c_env, "environment", 0 },
	{ "sh", c_sh, "[command] a new shell (execve of this program)", 0 },
	{ "exec", c_sh, "[command] replace this shell (not in init)", 1 },
	{ "logo", c_logo, "the banner", 0 },
	{ "clear", c_clear, "scroll the screen clean", 0 },
	{ "true", c_true, "nothing, successfully", 0 },
	{ "exit", c_exit, "[code] leave a nested shell", 1 },
	{ "reboot", c_reboot, "restart the phone", 0 },
	{ "halt", c_reboot, "stop the system", 0 },
	{ 0, 0, 0, 0 },
};

static int
c_env(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	for (char **e = g_envp; e && *e; e++) {
		pf("%s\n", *e);
	}
	return 0;
}

/* ---- the shell -------------------------------------------------------------- */

static void
set_signals(int ignore)
{
	struct __sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.__sigaction_u.__sa_handler = ignore ? SIG_IGN : SIG_DFL;
	static const int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGHUP, SIGPIPE };
	for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
		S3(SYS_sigaction_, sigs[i], &sa, 0);
	}
}

static int
run_argv(int argc, char **argv)
{
	const struct cmd *c = cmds;
	while (c->name && !seq(c->name, argv[0])) {
		c++;
	}
	if (!c->name) {
		pf("%s: command not found (try help)\n", argv[0]);
		return 127;
	}
	if (c->fn == c_sh && c->in_parent && g_pid1) {
		pf("exec: this is init (PID 1): use sh\n");
		return 1;
	}
	if (c->in_parent) {
		int r = c->fn(argc, argv);
		flush();
		return r;
	}
	flush();
	long pid = k_fork();
	if (pid < 0) {
		perr("fork", pid);
		return 1;
	}
	if (pid == 0) {
		g_pid1 = 0;
		set_signals(0);
		int r = c->fn(argc, argv);
		flush();
		k_exit(r);
	}
	int st = 0;
	long w;
	while ((w = S4(SYS_wait4_, pid, &st, 0, 0)) == -EINTR) {
	}
	if (w < 0) {
		perr("wait", w);
		return 1;
	}
	if (WIFSIGNALED(st)) {
		pf("[%s: killed by signal %d%s]\n", argv[0], WTERMSIG(st), WCOREDUMP(st) ? ", core" : "");
		return 128 + WTERMSIG(st);
	}
	return WEXITSTATUS(st);
}

/* Split on blanks with "..." quoting; ';' separates commands. */
static int
run_line(char *line)
{
	int rc = 0;
	char *p = line;
	while (*p) {
		char *argv[32];
		int argc = 0, end = 0;
		while (!end) {
			while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
				p++;
			}
			if (!*p || *p == ';') {
				if (*p) {
					p++;
				}
				break;
			}
			char *w = p, *o = p;
			int q = 0;
			while (*p && (q || (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != ';'))) {
				if (*p == '"') {
					q = !q;
					p++;
					continue;
				}
				*o++ = *p++;
			}
			end = *p == ';' || !*p;
			if (*p) {
				p++;            /* past the blank or ';' (o stays behind p) */
			}
			*o = 0;
			if (argc < 31) {
				argv[argc++] = w;
			}
		}
		argv[argc] = 0;
		if (argc) {
			rc = run_argv(argc, argv);
		}
	}
	return rc;
}

static void
status_line(void)
{
	long bt = boottime(), t = now_us() / 1000000L;
	struct loadavg la = { { 0, 0, 0 }, 1 };
	size_t len = sizeof(la);
	k_sysctlbyname("vm.loadavg", &la, &len, 0, 0);
	long scale = la.fscale ? la.fscale : 1;
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t plen = sizeof(sbuf);
	if (k_sysctl(mib, 4, sbuf, &plen, 0, 0) < 0) {
		plen = 0;
	}
	long long page = sysctl_num("hw.pagesize", 16384);
	long long freep = sysctl_num("vm.page_free_count", 0) + sysctl_num("vm.page_speculative_count", 0);
	pf("\n-- darwin: up ");
	put_duration(bt ? t - bt : 0);
	pf(" | load %ld.%02ld | %zu procs | %lld MiB free | %lld cpus --\n",
	    (long)la.ldavg[0] / scale, (long)(la.ldavg[0] % scale) * 100 / scale,
	    plen / sizeof(struct kinfo_proc), (freep * page) >> 20, sysctl_num("hw.activecpu", 0));
}

static void
prompt(void)
{
	pf(g_depth ? "darwin(%d)# " : "darwin# ", g_depth);
	flush();
}

/*
 * Wait for input on fd 0 up to ms: 1 ready, 0 timeout. If the console cannot be
 * selected on, wait the time out with a plain timer instead, so the status line
 * never floods the screen.
 */
static int
wait_input(long ms)
{
	uint32_t set[1] = { 1 };
	struct timeval tv = { ms / 1000, (int)(ms % 1000) * 1000 };
	long t0 = now_us();
	long r = S6(SYS_select_, 1, set, 0, 0, &tv, 0);
	if (r > 0) {
		return 1;
	}
	long left = ms - (now_us() - t0) / 1000;
	if (left > 0 && r != -EINTR) {
		sleep_ms(left);
	}
	return 0;
}

static void
reap(void)
{
	int st;
	while (S4(SYS_wait4_, -1, &st, WNOHANG, 0) > 0) {
	}
}

static void
shell_loop(long tick_s)
{
	static char line[1024];
	int fails = 0;
	prompt();
	for (;;) {
		if (tick_s > 0 && !wait_input(tick_s * 1000)) {
			reap();
			status_line();
			prompt();
			continue;
		}
		long n = k_read(0, line, sizeof(line) - 1);
		if (n <= 0) {
			if (!g_pid1 && n == 0) {
				pf("\n");
				flush();
				k_exit(0);
			}
			/* No usable input (EOF or error on the console): stay alive, idle quietly. */
			if (++fails > 3) {
				sleep_ms(tick_s > 0 ? tick_s * 1000 : 60000);
				if (tick_s > 0) {
					status_line();
				}
			}
			continue;
		}
		fails = 0;
		line[n] = 0;
		/* A tty in canonical mode returns one line per read; a pipe may return several. */
		for (char *l = line; *l;) {
			char *e = l;
			while (*e && *e != '\n') {
				e++;
			}
			int more = *e == '\n';
			*e = 0;
			run_line(l);
			reap();
			l = more ? e + 1 : e;
		}
		prompt();
	}
}

static void
autorun(void)
{
	static char run[512];
	char v[8];
	if (bootarg("dshauto", v, sizeof(v)) && v[0] == '0') {
		return;
	}
	if (bootarg("dshrun", run, sizeof(run))) {
		for (char *p = run; *p; p++) {
			*p = *p == ',' ? ';' : *p == '+' ? ' ' : *p;
		}
	} else {
		scopy(run, "sysinfo;mount;ls -l /dev;ps;bench", sizeof(run));
	}
	char *p = run;
	while (*p) {
		char cmd[256];
		size_t i = 0;
		while (*p && *p != ';' && i + 1 < sizeof(cmd)) {
			cmd[i++] = *p++;
		}
		cmd[i] = 0;
		if (*p == ';') {
			p++;
		}
		pf("darwin# %s\n", cmd);
		run_line(cmd);
		pf("\n");
	}
	flush();
}

static void
init_console(void)
{
	S0(SYS_setsid_);
	long fd = k_open("/dev/console", O_RDWR);
	if (fd < 0) {
		return;         /* nothing to report to */
	}
	S2(SYS_dup2_, fd, 0);
	S2(SYS_dup2_, fd, 1);
	S2(SYS_dup2_, fd, 2);
	if (fd > 2) {
		k_close((int)fd);
	}
	S3(SYS_ioctl_, 0, TIOCSCTTY, 0);
	struct termios t;
	if (S3(SYS_ioctl_, 0, TIOCGETA, &t) >= 0) {
		t.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ISIG | IEXTEN;
		t.c_iflag |= ICRNL | IXON;
		t.c_oflag |= OPOST | ONLCR;
		t.c_cc[VERASE] = 0x7f;
		t.c_cc[VINTR] = 3;
		S3(SYS_ioctl_, 0, TIOCSETA, &t);
	}
}

static __attribute__((noreturn)) void
dsh_main(int argc, char **argv, char **envp)
{
	g_envp = envp;
	g_started_us = now_us();
	g_pid1 = k_getpid() == 1 || g_fake_init;
	sysctl_str("kern.bootargs", bootargs, sizeof(bootargs));
	if (g_fake_init && argc > 2) {
		scopy(bootargs, argv[2], sizeof(bootargs));
	}
	char v[16];
	long tick = 30;
	if (bootarg("dshtick", v, sizeof(v))) {
		tick = satol(v, 0);
	}
	if (g_pid1) {
		if (!g_fake_init) {
			init_console();
			set_signals(1);
			static const char host[] = "starbite";
			k_sysctlbyname("kern.hostname", 0, 0, (void *)host, sizeof(host) - 1);
		}
		c_logo(0, 0);
		pf("%s: init is PID 1, the root is a RAM disk, no iOS.\n", VERSION);
		pf("type help for commands.\n\n");
		flush();
		autorun();
		shell_loop(tick);
	}
	/* Nested shell or a one-shot command: argv[1..] is a command line. */
	if (argc > 1) {
		static char line[1024];
		size_t o = 0;
		for (int i = 1; i < argc; i++) {
			for (const char *s = argv[i]; *s && o + 2 < sizeof(line); s++) {
				line[o++] = *s;
			}
			if (o + 2 < sizeof(line)) {
				line[o++] = ' ';
			}
		}
		line[o] = 0;
		int r = run_line(line);
		flush();
		k_exit(r);
	}
	g_depth = 1;
	pf("%s (pid %ld)\n", VERSION, k_getpid());
	shell_loop(g_pid1 ? tick : 0);
	k_exit(0);
}

#ifdef DSH_HOST
int
main(int argc, char **argv, char **envp)
{
	/* On the Mac the program is not /sbin/launchd: sh re-executes argv[0]. */
	for (const char *c = argv[0]; *c; c++) {
		if (*c == '/') {
			scopy(self_path, argv[0], sizeof(self_path));
			break;
		}
	}
	/* dsh-host --as-init "BOOTARGS": the PID 1 path (banner, autorun, status ticks) on stdio. */
	if (argc > 1 && seq(argv[1], "--as-init")) {
		g_fake_init = 1;
	}
	dsh_main(argc, argv, envp);
}
#else
void cstart(long *sp);

void
cstart(long *sp)
{
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);
	char **envp = argv + argc + 1;
	if (argc > 0 && argv[0] && argv[0][0] == '/') {
		scopy(self_path, argv[0], sizeof(self_path));
	}
	dsh_main(argc, argv, envp);
}
#endif
