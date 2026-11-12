/* SPDX-License-Identifier: GPL-2.0-only
 * sbpkg: the guest's App Store daemon (docs/STARBITE-LINUX-APPS.md L6; contract v2 in ../sbshm.h).
 *
 *   - maps the shared region (/dev/sbshm; SBPKG_SHM=/a/plain/file for tests) up to SBSHM_BLK_OFF,
 *   - reads the curated store index from the repository mount (/repo/store/index.tsv, built by
 *     rootfs/mk-repo.sh), fills struct sbshm_store and the store's icons (pool 64..159) under the store seqlock,
 *   - computes INSTALLED / SYSTEM / NO_SPACE / TOO_BIG (dpkg's status, the overlay's statvfs, /proc/meminfo),
 *   - is a CLIENT of sbwl on /run/sbwl.sock (sbwl owns the rings): it reads "op IDX OP SEQ" lines and writes
 *     "pkg IDX STATE PCT", "rescan" and "store SEQ" lines; it reconnects when sbwl is not (yet) there,
 *   - installs / removes with apt-get from the offline repository (deb [trusted=yes] file:/repo ./) into the live
 *     root (the overlay), one operation at a time from a queue; progress from APT::Status-Fd.
 *
 * Robustness: on start (and after any failed operation) `dpkg --configure -a` + `apt-get -f install`; a failed
 * install that still leaves dpkg unhappy is rolled back by removing what it brought; apt waits for the dpkg lock
 * (DPkg::Lock::Timeout); a remove that would take a base package with it is refused (simulated first).
 *
 * Seq rule (an interpretation of "indices are valid for one seq"): store indices only change when the index is
 * (re)loaded, so an op is accepted when its SEQ is not older than the publish that loaded the index (layout_seq)
 * and not newer than the current seq. Progress updates bump the seq without invalidating what the host read.
 *
 * Environment: SBPKG_SHM, SBPKG_REPO (/repo), SBPKG_SOCK (/run/sbwl.sock), SBPKG_ROOT (/: the overlay, for
 * statvfs), SBPKG_RUN (/run/sbpkg), SBPKG_BASE_LIST (/usr/share/starbite/base-packages.txt), SBPKG_MEM_MB (pretend
 * MemTotal, tests), SBPKG_RAM_LIMIT_MB (TOO_BIG above this; default MemTotal - 600), SBPKG_RESERVE_MB (64: overlay
 * space never handed out), SBPKG_NO_REPAIR=1 (skip the start-up repair).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../sbshm.h"

#define MAP_LEN SBSHM_BLK_OFF           /* header .. icon pool: all sbpkg touches */
#define ICON_FIRST SBSHM_ICONS_CAT
#define ICON_N (SBSHM_ICONS_MAX - SBSHM_ICONS_CAT)
#define APT_MAX 12                      /* packages per store entry */

/* ---- configuration ---- */
static const char *shm_path, *repo, *sock_path, *root_path, *run_dir, *base_list;
static long mem_mb_override, ram_limit_override = -1, reserve_mb = 64;

/* ---- the store, our side ---- */
struct ent {
	char key[40], title[32], summary[96], cat[16];
	char *apt[APT_MAX];
	int napt;
	uint32_t dl_kb, inst_kb, ram_mb, featured;
	uint32_t flags, state, pct;
	char err[32];
	int has_icon;
};
static struct ent ents[SBSHM_PKGS_MAX];
static unsigned nents;
static int repo_ok;
static struct stat index_st;            /* to notice a new / removed repo */

static uint8_t *shm;
static struct sbshm_store *st;          /* in shm */
static uint32_t seq;                    /* our copy of st->seq (even between publishes) */
static uint32_t layout_seq;             /* the seq at which the current indices were first published */
static int icons_dirty;
static uint32_t ov_total_kb, ov_free_kb, mem_total_mb;

/* installed / base package sets: sorted string arrays */
struct set { char **v; size_t n, cap; };
static struct set installed, base;

/* ---- the sbwl connection ---- */
static int sfd = -1;
static char rbuf[1024];
static size_t rlen;
static uint64_t next_connect_ms;

/* ---- the queue and the running job ---- */
struct op { int idx, op; };
static struct op q[64];
static unsigned qh, qt;
enum { JOB_NONE, JOB_INSTALL, JOB_REMOVE };
static struct {
	int kind, idx;
	pid_t pid;
	int stfd, outfd;
	int in_dpkg, cancel;
	char st_line[512];
	size_t st_len;
	char out[4096];                 /* the tail of apt's output, for the error */
	size_t out_len;
	char pmerror[96];
	uint64_t last_pub_ms;
} job = {.stfd = -1, .outfd = -1};

static volatile sig_atomic_t quit;

/* ======================================================================================================= */
static void logf_(const char *fmt, ...)
{
	char b[600];
	int n = snprintf(b, sizeof(b), "sbpkg: ");
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(b + n, sizeof(b) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(b) - 2)
		n = (int)sizeof(b) - 2;
	b[n++] = '\n';
	(void)!write(2, b, (size_t)n);  /* one write: one kernel log record when stderr is /dev/kmsg */
}
#define say(...) logf_(__VA_ARGS__)

static uint64_t ms_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static const char *env_or(const char *n, const char *d)
{
	const char *v = getenv(n);
	return v && *v ? v : d;
}

static void scpy(char *d, size_t n, const char *s)
{
	size_t l = strlen(s);
	if (l >= n) {                   /* cut on a UTF-8 boundary */
		l = n - 1;
		while (l > 0 && ((unsigned char)s[l] & 0xc0) == 0x80)
			--l;
	}
	memcpy(d, s, l);
	memset(d + l, 0, n - l);
}

/* ---- string sets ---- */
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void set_clear(struct set *s)
{
	for (size_t i = 0; i < s->n; ++i)
		free(s->v[i]);
	s->n = 0;
}
static void set_add(struct set *s, const char *x)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 1024;
		s->v = realloc(s->v, s->cap * sizeof(*s->v));
	}
	s->v[s->n++] = strdup(x);
}
static void set_sort(struct set *s) { qsort(s->v, s->n, sizeof(*s->v), cmpstr); }
static int set_has(const struct set *s, const char *x)
{
	return s->n && bsearch(&x, s->v, s->n, sizeof(*s->v), cmpstr) != NULL;
}

/* ---- running commands ---- */
/* in a child: put src on fd dst even when the numbers collide (src may be 0..3 itself) */
static void child_fds(int in, int out, int status)
{
	in = fcntl(in, F_DUPFD, 10);
	out = fcntl(out, F_DUPFD, 10);
	status = fcntl(status, F_DUPFD, 10);
	dup2(in, 0);
	dup2(out, 1);
	dup2(out, 2);
	dup2(status, 3);
}

static void child_env(void)
{
	setenv("DEBIAN_FRONTEND", "noninteractive", 1);
	setenv("DEBCONF_NONINTERACTIVE_SEEN", "true", 1);
	setenv("APT_LISTCHANGES_FRONTEND", "none", 1);
	setenv("LC_ALL", "C.UTF-8", 1);
	setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
	if (!getenv("HOME"))
		setenv("HOME", "/root", 1);
}

/* the apt options every apt-get call gets: only our repository, waiting for the dpkg lock */
static char src_opt[400], parts_opt[400];
static int apt_argv(char **v, int n)
{
	static char *const fixed[] = {"apt-get", "-y", "-q", "--no-install-recommends", "-o", "Dpkg::Use-Pty=0",
	                              "-o", "DPkg::Lock::Timeout=120", "-o", "APT::Status-Fd=3",
	                              "-o", "Dpkg::Options::=--force-confdef", "-o", "Dpkg::Options::=--force-confold",
	                              /* the guest's clock may be 1970 (no RTC, no network time): a Release dated 2026
	                               * would be "not valid yet"; the repository is local and trusted anyway */
	                              "-o", "Acquire::Check-Date=false", "-o", "Acquire::Check-Valid-Until=false",
	                              /* file: sources: no drop to _apt (it may not read /repo) */
	                              "-o", "APT::Sandbox::User=root"};
	for (unsigned i = 0; i < sizeof(fixed) / sizeof(*fixed); ++i)
		v[n++] = fixed[i];
	v[n++] = "-o";
	v[n++] = src_opt;
	v[n++] = "-o";
	v[n++] = parts_opt;
	return n;
}

/* run argv to completion; its output (stdout + stderr) into out (NUL-terminated tail); returns the exit status
 * (-1: could not run, 128 + sig: killed) */
static int run(char *const argv[], char *out, size_t outsz)
{
	int p[2];
	if (pipe2(p, O_CLOEXEC))
		return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		return -1;
	}
	if (!pid) {
		child_fds(open("/dev/null", O_RDONLY), p[1], p[1]);   /* apt's status fd, if asked for: into the output */
		child_env();
		execvp(argv[0], argv);
		_exit(127);
	}
	close(p[1]);
	size_t len = 0;
	char tmp[4096];
	ssize_t r;
	while ((r = read(p[0], tmp, sizeof(tmp))) > 0 || (r < 0 && errno == EINTR)) {
		if (r <= 0)
			continue;
		if (!out || !outsz)
			continue;
		if (len + (size_t)r >= outsz) {     /* keep the tail */
			size_t drop = len + (size_t)r - (outsz - 1);
			if (drop > len)
				drop = len;
			memmove(out, out + drop, len - drop);
			len -= drop;
		}
		size_t c = (size_t)r < outsz - 1 - len ? (size_t)r : outsz - 1 - len;
		memcpy(out + len, tmp + (size_t)r - c, c);
		len += c;
	}
	if (out && outsz)
		out[len] = 0;
	close(p[0]);
	int stt;
	while (waitpid(pid, &stt, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(stt) ? WEXITSTATUS(stt) : 128 + WTERMSIG(stt);
}

static int run_apt(const char *const *args, int nargs, char *out, size_t outsz)
{
	char *v[64];
	int n = apt_argv(v, 0);
	for (int i = 0; i < nargs && n < 62; ++i)
		v[n++] = (char *)args[i];
	v[n] = NULL;
	return run(v, out, outsz);
}

/* ---- reading the system ---- */
static void load_base(void)
{
	set_clear(&base);
	FILE *f = fopen(base_list, "r");
	if (!f) {
		say("no %s: nothing is SYSTEM", base_list);
		return;
	}
	char l[256];
	while (fgets(l, sizeof(l), f)) {
		l[strcspn(l, "\r\n:")] = 0;     /* "pkg" or "pkg:arm64" */
		if (*l)
			set_add(&base, l);
	}
	fclose(f);
	set_sort(&base);
}

static void load_installed(void)
{
	set_clear(&installed);
	char *argv[] = {"dpkg-query", "-W", "-f=${db:Status-Abbrev} ${Package}\\n", NULL};
	static char out[1 << 20];
	if (run(argv, out, sizeof(out)) != 0 && !*out) {
		say("dpkg-query failed");
		return;
	}
	for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n"))
		if (!strncmp(l, "ii ", 3)) {
			char *p = l + 3;
			while (*p == ' ')
				++p;
			set_add(&installed, p);
		}
	set_sort(&installed);
}

static void read_space_mem(void)
{
	struct statvfs v;
	if (!statvfs(root_path, &v)) {
		ov_total_kb = (uint32_t)((uint64_t)v.f_blocks * v.f_frsize / 1024);
		ov_free_kb = (uint32_t)((uint64_t)v.f_bavail * v.f_frsize / 1024);
	}
	if (mem_mb_override > 0) {
		mem_total_mb = (uint32_t)mem_mb_override;
		return;
	}
	FILE *f = fopen("/proc/meminfo", "r");
	char l[128];
	unsigned long kb;
	while (f && fgets(l, sizeof(l), f))
		if (sscanf(l, "MemTotal: %lu", &kb) == 1)
			mem_total_mb = (uint32_t)(kb / 1024);
	if (f)
		fclose(f);
}

static uint32_t ram_limit_mb(void)
{
	if (ram_limit_override >= 0)
		return (uint32_t)ram_limit_override;
	return mem_total_mb > 900 ? mem_total_mb - 600 : 300;
}

static uint32_t entry_flags(const struct ent *e)
{
	uint32_t f = e->featured ? SBSHM_PKG_FEATURED : 0;
	int inst = e->napt > 0, sys = e->napt > 0;
	for (int i = 0; i < e->napt; ++i) {
		inst &= set_has(&installed, e->apt[i]);
		sys &= set_has(&base, e->apt[i]);
	}
	if (inst)
		f |= SBSHM_PKG_INSTALLED;
	if (sys)
		f |= SBSHM_PKG_SYSTEM;
	if (e->ram_mb > ram_limit_mb())
		f |= SBSHM_PKG_TOO_BIG;
	if (!inst && (uint64_t)e->inst_kb + (uint64_t)reserve_mb * 1024 > ov_free_kb)
		f |= SBSHM_PKG_NO_SPACE;
	return f;
}

/* recompute every entry's flags; 1 if any changed */
static int refresh_flags(void)
{
	int ch = 0;
	for (unsigned i = 0; i < nents; ++i) {
		uint32_t f = entry_flags(&ents[i]);
		ch |= f != ents[i].flags;
		ents[i].flags = f;
	}
	return ch;
}

/* /run/sbpkg/apps.tsv: "store index <TAB> key <TAB> desktop id" for the installed entries' .desktop files, so sbwl
 * can fill sbshm_app.pkg (written whole, then renamed) */
static void write_apps_map(void)
{
	char tmp[300], dst[300];
	snprintf(tmp, sizeof(tmp), "%s/apps.tsv.tmp", run_dir);
	snprintf(dst, sizeof(dst), "%s/apps.tsv", run_dir);
	FILE *o = fopen(tmp, "w");
	if (!o)
		return;
	for (unsigned i = 0; i < nents; ++i) {
		if (!(ents[i].flags & SBSHM_PKG_INSTALLED))
			continue;
		for (int k = 0; k < ents[i].napt; ++k) {
			char p[300];
			FILE *f = NULL;
			const char *suf[] = {"", ":arm64", ":all"};
			for (unsigned s = 0; s < 3 && !f; ++s) {
				snprintf(p, sizeof(p), "/var/lib/dpkg/info/%s%s.list", ents[i].apt[k], suf[s]);
				f = fopen(p, "r");
			}
			char l[512];
			while (f && fgets(l, sizeof(l), f)) {
				l[strcspn(l, "\n")] = 0;
				const char *pre = "/usr/share/applications/";
				size_t n = strlen(l);
				if (!strncmp(l, pre, strlen(pre)) && n > 8 && !strcmp(l + n - 8, ".desktop") &&
				    !strchr(l + strlen(pre), '/'))
					fprintf(o, "%u\t%s\t%.*s\n", i, ents[i].key, (int)(n - strlen(pre) - 8), l + strlen(pre));
			}
			if (f)
				fclose(f);
		}
	}
	fclose(o);
	rename(tmp, dst);
}

/* ---- the index ---- */
static char *field(char **p)
{
	char *s = *p;
	if (!s)
		return (char *)"";
	char *t = strchr(s, '\t');
	if (t) {
		*t = 0;
		*p = t + 1;
	} else {
		s[strcspn(s, "\r\n")] = 0;
		*p = NULL;
	}
	return s;
}

static void free_ents(void)
{
	for (unsigned i = 0; i < nents; ++i)
		for (int k = 0; k < ents[i].napt; ++k)
			free(ents[i].apt[k]);
	memset(ents, 0, sizeof(ents));
	nents = 0;
}

/* index.tsv: "#" lines are comments; fields: key title summary category ram_mb featured dl_kb inst_kb apt
 * [ram_method]; apt = the packages to install, space-separated (the first is the main one) */
static int load_index(void)
{
	char p[300];
	snprintf(p, sizeof(p), "%s/store/index.tsv", repo);
	free_ents();
	repo_ok = 0;
	FILE *f = fopen(p, "r");
	if (!f) {
		memset(&index_st, 0, sizeof(index_st));
		say("no store index at %s", p);
		return 0;
	}
	fstat(fileno(f), &index_st);
	char l[2048];
	while (fgets(l, sizeof(l), f) && nents < SBSHM_PKGS_MAX) {
		if (l[0] == '#' || l[0] == '\n')
			continue;
		char *c = l;
		struct ent *e = &ents[nents];
		scpy(e->key, sizeof(e->key), field(&c));
		scpy(e->title, sizeof(e->title), field(&c));
		scpy(e->summary, sizeof(e->summary), field(&c));
		scpy(e->cat, sizeof(e->cat), field(&c));
		e->ram_mb = (uint32_t)strtoul(field(&c), NULL, 10);
		e->featured = (uint32_t)strtoul(field(&c), NULL, 10);
		e->dl_kb = (uint32_t)strtoul(field(&c), NULL, 10);
		e->inst_kb = (uint32_t)strtoul(field(&c), NULL, 10);
		char *apt = field(&c);
		for (char *s = strtok(apt, " "); s && e->napt < APT_MAX; s = strtok(NULL, " "))
			e->apt[e->napt++] = strdup(s);
		if (!e->key[0] || !e->napt)
			continue;
		nents++;
	}
	fclose(f);
	snprintf(p, sizeof(p), "%s/Packages", repo);
	repo_ok = nents > 0 && access(p, R_OK) == 0;
	icons_dirty = 1;
	say("index: %u entries from %s/store/index.tsv (repo %s)", nents, repo, repo_ok ? "ok" : "NOT ok");
	return repo_ok;
}

/* ---- the shared table ---- */
static void wcopy(volatile void *dst, const void *src, size_t n)    /* word copies into the NC region */
{
	volatile uint64_t *d = dst;
	const uint64_t *s = src;
	for (size_t i = 0; i < n / 8; ++i)
		d[i] = s[i];
	volatile uint8_t *db = (volatile uint8_t *)dst;
	for (size_t i = n & ~(size_t)7; i < n; ++i)
		db[i] = ((const uint8_t *)src)[i];
}

static void write_icons(void)
{
	static uint32_t px[SBSHM_ICON_PX * SBSHM_ICON_PX];
	for (unsigned i = 0; i < nents && i < ICON_N; ++i) {
		char p[400];
		snprintf(p, sizeof(p), "%s/store/icons/%s.argb", repo, ents[i].key);
		int fd = open(p, O_RDONLY | O_CLOEXEC);
		ents[i].has_icon = fd >= 0 && read(fd, px, sizeof(px)) == (ssize_t)sizeof(px);
		if (fd >= 0)
			close(fd);
		if (ents[i].has_icon)
			wcopy(shm + SBSHM_ICON_OFF + (size_t)(ICON_FIRST + i) * SBSHM_ICON_BYTES, px, sizeof(px));
	}
}

static void sock_send(const char *fmt, ...);

/* the seqlock: odd, dmb, the table (and icons), dmb, even. Then "store SEQ" to sbwl. */
static void publish(void)
{
	static struct sbshm_store t;
	memset(&t, 0, sizeof(t));
	t.n = nents;
	t.overlay_total_kb = ov_total_kb;
	t.overlay_free_kb = ov_free_kb;
	t.repo_ok = (uint32_t)repo_ok;
	for (unsigned i = 0; i < nents; ++i) {
		struct sbshm_pkg *p = &t.pkg[i];
		const struct ent *e = &ents[i];
		scpy(p->pkg, sizeof(p->pkg), e->key);
		scpy(p->name, sizeof(p->name), e->title);
		scpy(p->summary, sizeof(p->summary), e->summary);
		scpy(p->category, sizeof(p->category), e->cat);
		scpy(p->err, sizeof(p->err), e->err);
		p->dl_kb = e->dl_kb;
		p->inst_kb = e->inst_kb;
		p->ram_mb = e->ram_mb;
		p->flags = e->flags;
		p->state = e->state;
		p->pct = e->pct;
		p->icon = i < ICON_N ? ICON_FIRST + i : SBSHM_NONE;
	}
	seq |= 1;                           /* odd: being written */
	st->seq = seq;
	sbshm_dmb();
	if (icons_dirty) {
		write_icons();
		icons_dirty = 0;
		layout_seq = seq + 1;           /* the indices of this publish */
	}
	for (unsigned i = 0; i < nents && i < ICON_N; ++i)
		if (!ents[i].has_icon)
			t.pkg[i].icon = SBSHM_NONE;
	wcopy((uint8_t *)st + sizeof(st->seq), (uint8_t *)&t + sizeof(t.seq), sizeof(t) - sizeof(t.seq));
	sbshm_dmb();
	seq += 1;                           /* even: whole */
	st->seq = seq;
	sbshm_dmb();
	sock_send("store %u", seq);
}

static void pkg_line(unsigned i)
{
	sock_send("pkg %u %u %u", i, ents[i].state, ents[i].pct);
}

/* state change of entry i: table + "pkg" + "store" */
static void set_state(unsigned i, uint32_t state, uint32_t pct, const char *err)
{
	ents[i].state = state;
	ents[i].pct = pct;
	scpy(ents[i].err, sizeof(ents[i].err), err ? err : "");
	pkg_line(i);
	publish();
}

/* ---- the sbwl socket ---- */
static void sock_close(const char *why)
{
	if (sfd >= 0) {
		say("sbwl socket closed (%s)", why);
		close(sfd);
	}
	sfd = -1;
	rlen = 0;
	next_connect_ms = ms_now() + 1000;
}

static void sock_send(const char *fmt, ...)
{
	if (sfd < 0)
		return;                     /* the table holds the state; sbwl reads it when it comes */
	char b[128];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof(b) - 1, fmt, ap);
	va_end(ap);
	b[n++] = '\n';
	ssize_t w = send(sfd, b, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT);
	if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		say("sbwl is not reading: dropped '%.*s'", n - 1, b);   /* the table still holds it */
	else if (w != n)
		sock_close("write failed");
}

static void sock_try_connect(void)
{
	if (sfd >= 0 || ms_now() < next_connect_ms)
		return;
	next_connect_ms = ms_now() + 1000;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	scpy(a.sun_path, sizeof(a.sun_path), sock_path);
	if (fd < 0)
		return;
	if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
		static int said;
		if (!said++)
			say("waiting for sbwl on %s (%s)", sock_path, strerror(errno));
		close(fd);
		return;
	}
	sfd = fd;
	say("connected to sbwl on %s", sock_path);
	sock_send("store %u", seq);
}

/* ---- the queue ---- */
static int queued(int idx)
{
	for (unsigned k = qt; k != qh; ++k)
		if (q[k % 64].idx == idx && q[k % 64].op)
			return 1;
	return 0;
}

static void handle_op(unsigned idx, unsigned op, uint32_t opseq)
{
	if ((int32_t)(opseq - layout_seq) < 0 || (int32_t)(seq - opseq) < 0) {
		say("op %u %u: stale seq %u (layout %u, now %u): ignored", idx, op, opseq, layout_seq, seq);
		return;
	}
	if (idx >= nents) {
		say("op %u %u: no such entry", idx, op);
		return;
	}
	struct ent *e = &ents[idx];
	if (op == SBG_PKG_OP_CANCEL) {
		if (job.kind != JOB_NONE && job.idx == (int)idx) {
			if (!job.in_dpkg) {
				say("cancel %s: stopping apt-get before dpkg", e->key);
				job.cancel = 2;
				kill(job.pid, SIGTERM);
			} else if (job.kind == JOB_INSTALL) {
				say("cancel %s: dpkg is running; removing it again when it is done", e->key);
				job.cancel = 1;
			} else {
				say("cancel %s: the removal is in dpkg already; it finishes", e->key);
			}
			return;
		}
		int found = 0;
		for (unsigned k = qt; k != qh; ++k)
			if (q[k % 64].idx == (int)idx) {
				q[k % 64].op = 0;   /* a hole the runner skips */
				found = 1;
			}
		if (found || e->state == SBG_PKG_FAILED)
			set_state(idx, SBG_PKG_IDLE, 0, NULL);
		return;
	}
	if (op != SBG_PKG_OP_INSTALL && op != SBG_PKG_OP_REMOVE) {
		say("op %u: unknown op %u", idx, op);
		return;
	}
	if ((job.kind != JOB_NONE && job.idx == (int)idx) || queued((int)idx)) {
		say("op %u %u: %s is busy already", idx, op, e->key);
		return;
	}
	if (qh - qt >= 64) {
		set_state(idx, SBG_PKG_FAILED, 0, "too many requests");
		return;
	}
	q[qh++ % 64] = (struct op){(int)idx, (int)op};
	set_state(idx, SBG_PKG_QUEUED, 0, NULL);
}

static void sock_read(void)
{
	ssize_t r = recv(sfd, rbuf + rlen, sizeof(rbuf) - 1 - rlen, 0);
	if (r <= 0) {
		if (r < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		sock_close(r == 0 ? "sbwl hung up" : strerror(errno));
		return;
	}
	rlen += (size_t)r;
	char *nl;
	while ((nl = memchr(rbuf, '\n', rlen))) {
		*nl = 0;
		unsigned idx, op, s;
		if (sscanf(rbuf, "op %u %u %u", &idx, &op, &s) == 3) {
			say("<- op %u %u %u", idx, op, s);
			handle_op(idx, op, s);
		} else if (rbuf[0]) {
			say("<- unknown line '%.60s'", rbuf);
		}
		size_t used = (size_t)(nl + 1 - rbuf);
		memmove(rbuf, nl + 1, rlen - used);
		rlen -= used;
	}
	if (rlen == sizeof(rbuf) - 1)
		rlen = 0;                   /* a line too long: drop it */
}

/* ---- repair: leave dpkg consistent ---- */
static int dpkg_audit_clean(void)
{
	char out[2048];
	char *a[] = {"dpkg", "--audit", NULL};
	run(a, out, sizeof(out));
	return out[0] == 0;
}

static void repair(void)
{
	char out[2048];
	char *cfg[] = {"dpkg", "--force-confdef", "--force-confold", "--configure", "-a", NULL};
	int r1 = run(cfg, out, sizeof(out));
	const char *fix[] = {"-f", "install"};
	int r2 = run_apt(fix, 2, out, sizeof(out));
	say("repair: dpkg --configure -a = %d, apt-get -f install = %d%s", r1, r2,
	    dpkg_audit_clean() ? "" : " (dpkg --audit still complains)");
}

/* apt's E: / W: lines (and dpkg's errors) into the log, one record each */
static void log_apt_errors(const char *what, int rc, const char *out)
{
	say("%s failed (%d)", what, rc);
	int n = 0;
	for (const char *l = out; *l && n < 12; ) {
		size_t len = strcspn(l, "\n");
		if (!strncmp(l, "E: ", 3) || !strncmp(l, "W: ", 3) || !strncmp(l, "Err:", 4) || !strncmp(l, "dpkg: ", 6) ||
		    !strncmp(l, "pmerror:", 8)) {
			say("  %.*s", (int)(len > 400 ? 400 : len), l);
			n++;
		}
		l += len + (l[len] == '\n');
	}
	if (!n)
		say("  (no E: line; the tail: %.300s)", strlen(out) > 300 ? out + strlen(out) - 300 : out);
}

static void apt_update(void)
{
	if (!repo_ok)
		return;
	static char out[16384];
	const char *u[] = {"update"};
	int r = run_apt(u, 1, out, sizeof(out));
	if (r) {
		log_apt_errors("apt-get update", r, out);
		repo_ok = 0;
	}
}

/* ---- jobs ---- */
static int other_installed_needs(unsigned self, const char *pkg)
{
	for (unsigned i = 0; i < nents; ++i) {
		if (i == self || !(ents[i].flags & SBSHM_PKG_INSTALLED))
			continue;
		for (int k = 0; k < ents[i].napt; ++k)
			if (!strcmp(ents[i].apt[k], pkg))
				return 1;
	}
	return 0;
}

static const char *classify(const char *out, int rc)
{
	if (strstr(out, "No space left") || strstr(out, "You don't have enough free space"))
		return "not enough space";
	if (strstr(out, "Unable to locate package") || strstr(out, "has no installation candidate"))
		return "not in the repository";
	if (strstr(out, "Unmet dependencies") || strstr(out, "held broken packages"))
		return "missing dependencies";
	if (strstr(out, "Could not get lock") || strstr(out, "Unable to acquire the dpkg"))
		return "the system is busy";
	if (strstr(out, "Hash Sum mismatch") || strstr(out, "Size mismatch"))
		return "damaged package";
	if (strstr(out, "Sub-process /usr/bin/dpkg returned"))
		return "dpkg failed";
	static char b[32];
	snprintf(b, sizeof(b), "apt-get failed (%d)", rc);
	return b;
}

static void job_start(int idx, int kind)
{
	struct ent *e = &ents[idx];
	const char *refuse = NULL;
	char sim[8192];
	const char *pk[APT_MAX];
	int npk = 0;

	refresh_flags();
	if (!repo_ok && kind == JOB_INSTALL)
		refuse = "no repository";
	else if (e->flags & SBSHM_PKG_SYSTEM)
		refuse = kind == JOB_INSTALL ? "part of the system" : "a system app";
	else if (kind == JOB_INSTALL && (e->flags & SBSHM_PKG_INSTALLED)) {
		set_state((unsigned)idx, SBG_PKG_IDLE, 100, NULL);
		return;
	} else if (kind == JOB_INSTALL && (e->flags & SBSHM_PKG_NO_SPACE))
		refuse = "not enough space";
	else if (kind == JOB_REMOVE && !(e->flags & SBSHM_PKG_INSTALLED)) {
		/* nothing of it (or only a part): remove the parts that are there */
		int any = 0;
		for (int k = 0; k < e->napt; ++k)
			any |= set_has(&installed, e->apt[k]);
		if (!any) {
			set_state((unsigned)idx, SBG_PKG_IDLE, 0, NULL);
			return;
		}
	}
	if (refuse) {
		say("%s %s refused: %s", kind == JOB_INSTALL ? "install" : "remove", e->key, refuse);
		set_state((unsigned)idx, SBG_PKG_FAILED, 0, refuse);
		return;
	}
	for (int k = 0; k < e->napt; ++k) {
		if (kind == JOB_REMOVE &&
		    (set_has(&base, e->apt[k]) || !set_has(&installed, e->apt[k]) || other_installed_needs((unsigned)idx, e->apt[k])))
			continue;               /* shared with another installed entry, or the base's */
		pk[npk++] = e->apt[k];
	}
	if (kind == JOB_REMOVE) {
		if (!npk) {
			set_state((unsigned)idx, SBG_PKG_IDLE, 0, NULL);
			return;
		}
		/* simulate first: never take a base package with it */
		const char *a[APT_MAX + 4] = {"-s", "purge", "--autoremove"};
		int na = 3;
		for (int k = 0; k < npk; ++k)
			a[na++] = pk[k];
		run_apt(a, na, sim, sizeof(sim));
		for (char *l = strtok(sim, "\n"); l; l = strtok(NULL, "\n")) {
			char name[128];
			if ((sscanf(l, "Purg %127s", name) == 1 || sscanf(l, "Remv %127s", name) == 1) &&
			    set_has(&base, name)) {
				say("remove %s refused: it would remove the base's %s", e->key, name);
				set_state((unsigned)idx, SBG_PKG_FAILED, 0, "needed by the system");
				return;
			}
		}
	}

	int sp[2], op[2];
	if (pipe2(sp, O_CLOEXEC) || pipe2(op, O_CLOEXEC)) {
		set_state((unsigned)idx, SBG_PKG_FAILED, 0, "no pipes");
		return;
	}
	char *v[64];
	int n = apt_argv(v, 0);
	if (kind == JOB_INSTALL) {
		v[n++] = "install";
	} else {
		v[n++] = "purge";
		v[n++] = "--autoremove";
	}
	for (int k = 0; k < npk; ++k)
		v[n++] = (char *)pk[k];
	v[n] = NULL;
	pid_t pid = fork();
	if (pid < 0) {
		close(sp[0]); close(sp[1]); close(op[0]); close(op[1]);
		set_state((unsigned)idx, SBG_PKG_FAILED, 0, "cannot fork");
		return;
	}
	if (!pid) {
		child_fds(open("/dev/null", O_RDONLY), op[1], sp[1]);
		child_env();
		setpgid(0, 0);
		execvp(v[0], v);
		_exit(127);
	}
	close(sp[1]);
	close(op[1]);
	memset(&job, 0, sizeof(job));
	job.kind = kind;
	job.idx = idx;
	job.pid = pid;
	job.stfd = sp[0];
	job.outfd = op[0];
	say("%s %s: apt-get %s (pid %d)", kind == JOB_INSTALL ? "install" : "remove", e->key,
	    kind == JOB_INSTALL ? "install" : "purge --autoremove", pid);
	set_state((unsigned)idx, kind == JOB_INSTALL ? SBG_PKG_INSTALLING : SBG_PKG_REMOVING, 1, NULL);
	job.last_pub_ms = ms_now();
}

static void job_progress(uint32_t pct)
{
	struct ent *e = &ents[job.idx];
	if (pct > 99)
		pct = 99;                   /* 100 = done */
	if (pct <= e->pct)
		return;
	uint64_t now = ms_now();
	if (now - job.last_pub_ms < 250 && pct < e->pct + 5)
		return;                     /* at most 4 updates a second, unless it jumps */
	job.last_pub_ms = now;
	set_state((unsigned)job.idx, e->state, pct, NULL);
}

static void job_status_line(char *l)
{
	/* pmstatus:PKG:PCT:DESC, pmerror:PKG:PCT:MSG, dlstatus:N:PCT:DESC */
	char *a = strchr(l, ':');
	if (!a)
		return;
	*a++ = 0;
	/* the package may carry ":arm64": the percent is the first field that is a number */
	char *b = a, *msg = NULL;
	double pct = -1;
	while ((b = strchr(b, ':'))) {
		char *end;
		double v = strtod(b + 1, &end);
		if (end > b + 1 && (*end == ':' || !*end)) {
			*b = 0;
			pct = v;
			msg = *end ? end : NULL;
			break;
		}
		++b;
	}
	if (pct < 0)
		return;
	if (!strcmp(l, "pmstatus")) {
		job.in_dpkg = 1;
		job_progress((uint32_t)pct);
	} else if (!strcmp(l, "dlstatus")) {
		job_progress((uint32_t)(pct / 10));
	} else if (!strcmp(l, "pmerror")) {
		snprintf(job.pmerror, sizeof(job.pmerror), "%s: %s", a, msg ? msg + 1 : "");
		say("dpkg error: %s", job.pmerror);
	}
}

static void job_read_status(void)
{
	char b[512];
	ssize_t r = read(job.stfd, b, sizeof(b));
	if (r <= 0) {
		if (r == 0 || errno != EINTR) {
			close(job.stfd);
			job.stfd = -1;
		}
		return;
	}
	for (ssize_t i = 0; i < r; ++i) {
		if (b[i] == '\n') {
			job.st_line[job.st_len] = 0;
			job_status_line(job.st_line);
			job.st_len = 0;
		} else if (job.st_len < sizeof(job.st_line) - 1) {
			job.st_line[job.st_len++] = b[i];
		}
	}
}

static void job_read_out(void)
{
	char b[1024];
	ssize_t r = read(job.outfd, b, sizeof(b));
	if (r <= 0) {
		if (r == 0 || errno != EINTR) {
			close(job.outfd);
			job.outfd = -1;
		}
		return;
	}
	if (job.out_len + (size_t)r >= sizeof(job.out)) {
		size_t drop = job.out_len + (size_t)r - (sizeof(job.out) - 1);
		if (drop > job.out_len)
			drop = job.out_len;
		memmove(job.out, job.out + drop, job.out_len - drop);
		job.out_len -= drop;
	}
	size_t c = (size_t)r < sizeof(job.out) - 1 - job.out_len ? (size_t)r : sizeof(job.out) - 1 - job.out_len;
	memcpy(job.out + job.out_len, b + (size_t)r - c, c);
	job.out_len += c;
	job.out[job.out_len] = 0;
}

static void after_change(void)
{
	load_installed();
	read_space_mem();
	refresh_flags();
	write_apps_map();
}

static void job_finish(int status)
{
	int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
	unsigned idx = (unsigned)job.idx;
	struct ent *e = &ents[idx];
	int kind = job.kind, cancel = job.cancel;
	while (job.stfd >= 0)
		job_read_status();
	while (job.outfd >= 0)
		job_read_out();
	job.kind = JOB_NONE;

	if (cancel == 2) {                  /* stopped before dpkg: nothing changed */
		say("%s: cancelled", e->key);
		repair();
		after_change();
		set_state(idx, SBG_PKG_IDLE, 0, NULL);
		sock_send("rescan");
		return;
	}
	if (rc == 0) {
		after_change();
		int ok = kind == JOB_INSTALL ? !!(e->flags & SBSHM_PKG_INSTALLED) : !(e->flags & SBSHM_PKG_INSTALLED);
		say("%s %s: %s (free %u MB)", kind == JOB_INSTALL ? "install" : "remove", e->key,
		    ok ? "done" : "apt-get said yes but dpkg disagrees", ov_free_kb / 1024);
		set_state(idx, ok ? SBG_PKG_IDLE : SBG_PKG_FAILED, ok ? 100 : 0, ok ? NULL : "incomplete");
		sock_send("rescan");
		if (ok && kind == JOB_INSTALL && cancel == 1) {
			say("%s: cancelled during dpkg: removing it again", e->key);
			q[qh++ % 64] = (struct op){(int)idx, SBG_PKG_OP_REMOVE};
			set_state(idx, SBG_PKG_QUEUED, 0, NULL);
		}
		return;
	}
	char why[32];
	scpy(why, sizeof(why), job.pmerror[0] && !strstr(job.out, "No space left") ? "dpkg failed" : classify(job.out, rc));
	say("%s %s failed (%d): %s", kind == JOB_INSTALL ? "install" : "remove", e->key, rc, why);
	log_apt_errors(kind == JOB_INSTALL ? "apt-get install" : "apt-get purge", rc, job.out);
	repair();
	if (kind == JOB_INSTALL && !dpkg_audit_clean()) {
		/* still broken: take back what this install brought */
		const char *a[APT_MAX + 2] = {"purge", "--autoremove"};
		int na = 2;
		for (int k = 0; k < e->napt; ++k)
			if (!set_has(&base, e->apt[k]))
				a[na++] = e->apt[k];
		char out[2048];
		int r = run_apt(a, na, out, sizeof(out));
		say("rollback of %s: apt-get purge = %d", e->key, r);
		repair();
	}
	after_change();
	set_state(idx, SBG_PKG_FAILED, 0, why);
	sock_send("rescan");
}

static void job_next(void)
{
	while (job.kind == JOB_NONE && qt != qh) {
		struct op o = q[qt++ % 64];
		if (!o.op)
			continue;               /* cancelled while queued */
		job_start(o.idx, o.op == SBG_PKG_OP_INSTALL ? JOB_INSTALL : JOB_REMOVE);
	}
}

/* ---- the repo mount may come and go ---- */
static void check_repo(void)
{
	char p[300];
	struct stat s;
	snprintf(p, sizeof(p), "%s/store/index.tsv", repo);
	int there = stat(p, &s) == 0;
	int same = there ? (s.st_ino == index_st.st_ino && s.st_dev == index_st.st_dev &&
	                    s.st_mtime == index_st.st_mtime && s.st_size == index_st.st_size)
	                 : index_st.st_ino == 0;
	if (same)
		return;
	say("the repository changed: reloading");
	load_index();
	apt_update();
	after_change();
	publish();
}

static void on_term(int s) { (void)s; quit = 1; }

static void *map_region(void)
{
	static int said;
	int fd = open(shm_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		if (!said++)
			say("%s: %s (waiting for it)", shm_path, strerror(errno));
		return NULL;
	}
	struct stat s;
	if (!fstat(fd, &s) && S_ISREG(s.st_mode) && s.st_size < MAP_LEN && ftruncate(fd, MAP_LEN)) {
		say("%s: too small", shm_path);
		close(fd);
		return NULL;
	}
	void *m = mmap(NULL, MAP_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	return m == MAP_FAILED ? NULL : m;
}

int main(void)
{
	shm_path = env_or("SBPKG_SHM", "/dev/sbshm");
	repo = env_or("SBPKG_REPO", "/repo");
	sock_path = env_or("SBPKG_SOCK", "/run/sbwl.sock");
	root_path = env_or("SBPKG_ROOT", "/");
	run_dir = env_or("SBPKG_RUN", "/run/sbpkg");
	base_list = env_or("SBPKG_BASE_LIST", "/usr/share/starbite/base-packages.txt");
	mem_mb_override = atol(env_or("SBPKG_MEM_MB", "0"));
	if (getenv("SBPKG_RAM_LIMIT_MB"))
		ram_limit_override = atol(getenv("SBPKG_RAM_LIMIT_MB"));
	reserve_mb = atol(env_or("SBPKG_RESERVE_MB", "64"));
	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa = {.sa_handler = on_term};
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	for (int i = 0; !(shm = map_region()); ++i) {
		sleep(2);
		if (quit)
			return 1;
	}
	st = (struct sbshm_store *)(shm + SBSHM_STORE_OFF);
	seq = st->seq;
	seq = (seq + 1) & ~1U;             /* continue after an earlier sbpkg (even) */

	/* our apt sources: only the repository */
	mkdir(run_dir, 0755);
	char p[300];
	snprintf(p, sizeof(p), "%s/parts.d", run_dir);
	mkdir(p, 0755);
	snprintf(parts_opt, sizeof(parts_opt), "Dir::Etc::SourceParts=%s", p);
	snprintf(p, sizeof(p), "%s/sources.list", run_dir);
	FILE *f = fopen(p, "w");
	if (f) {
		fprintf(f, "deb [trusted=yes] file:%s ./\n", repo);
		fclose(f);
	}
	snprintf(src_opt, sizeof(src_opt), "Dir::Etc::SourceList=%s", p);

	load_base();
	load_index();
	after_change();
	publish();                          /* the table early: the store can show it while we repair */
	say("ready: %u entries, overlay %u/%u MB free, RAM %u MB (TOO_BIG above %u MB), seq %u", nents,
	    ov_free_kb / 1024, ov_total_kb / 1024, mem_total_mb, ram_limit_mb(), seq);
	if (!getenv("SBPKG_NO_REPAIR"))
		repair();
	apt_update();
	after_change();
	publish();

	uint64_t last_tick = ms_now(), last_full = ms_now();
	while (!quit || job.kind != JOB_NONE) {
		sock_try_connect();
		struct pollfd pf[3];
		int n = 0, isock = -1, ist = -1, iout = -1;
		if (sfd >= 0) {
			isock = n;
			pf[n++] = (struct pollfd){.fd = sfd, .events = POLLIN};
		}
		if (job.stfd >= 0) {
			ist = n;
			pf[n++] = (struct pollfd){.fd = job.stfd, .events = POLLIN};
		}
		if (job.outfd >= 0) {
			iout = n;
			pf[n++] = (struct pollfd){.fd = job.outfd, .events = POLLIN};
		}
		int r = poll(pf, (nfds_t)n, 200);
		if (r > 0) {
			if (ist >= 0 && pf[ist].revents)
				job_read_status();
			if (iout >= 0 && pf[iout].revents)
				job_read_out();
			if (isock >= 0 && pf[isock].revents)
				sock_read();
		}
		if (job.kind != JOB_NONE) {
			int status;
			if (waitpid(job.pid, &status, WNOHANG) == job.pid)
				job_finish(status);
		}
		if (!quit)
			job_next();
		uint64_t now = ms_now();
		if (now - last_tick >= 2000 && job.kind == JOB_NONE) {
			last_tick = now;
			uint32_t old_free = ov_free_kb;
			check_repo();
			if (now - last_full >= 30000) {     /* someone may have used apt in a terminal */
				last_full = now;
				load_installed();
				write_apps_map();
			}
			read_space_mem();
			uint32_t d = old_free > ov_free_kb ? old_free - ov_free_kb : ov_free_kb - old_free;
			if (refresh_flags() || d >= 16 * 1024)
				publish();
		}
	}
	say("exiting");
	return 0;
}
