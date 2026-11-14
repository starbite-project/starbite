// SPDX-License-Identifier: GPL-2.0-only
/*
 * sbgl-probe: does GL in the Linux guest run on Starbite's Mali (sbguest.ko's sbgpu, Track E 2026-11-14)?
 * EGL on the surfaceless platform (no window system: the render node alone), a GLES 2 context, a 64 x 64 texture
 * as the target: GL's renderer named, the target cleared to a colour and read back (the GPU's own work: right only
 * when the job chains really ran), then 100 clears + glFinish timed. Every line also goes to the kernel log as
 * "STARBITE-GPU: ..." (Starbite keeps those in pmsg), the last one PASS or FAIL:
 *   STARBITE-GPU: renderer <GL_RENDERER> | version <GL_VERSION>
 *   STARBITE-GPU: pixel rr gg bb aa (want 33 99 cc ff) ... | 100 frames in N ms
 *   STARBITE-GPU: PASS | FAIL <why>
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void say(const char *fmt, ...)
{
	char b[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	printf("STARBITE-GPU: %s\n", b);
	fflush(stdout);
	FILE *k = fopen("/dev/kmsg", "w");
	if (k) {
		fprintf(k, "STARBITE-GPU: %s\n", b);
		fclose(k);
	}
}

static double ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
	EGLint maj = 0, min = 0;
	if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) {
		say("FAIL no EGL display on the surfaceless platform (0x%x)", eglGetError());
		return 1;
	}
	say("EGL %d.%d, vendor %s", maj, min, eglQueryString(d, EGL_VENDOR));
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLConfig cfg;
	EGLint n = 0;
	const EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
	if (!eglChooseConfig(d, ca, &cfg, 1, &n) || n < 1) {
		say("FAIL no GLES 2 config (0x%x)", eglGetError());
		return 1;
	}
	const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
	if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
		say("FAIL no context (0x%x)", eglGetError());
		return 1;
	}
	const char *r = (const char *)glGetString(GL_RENDERER);
	say("renderer %s | version %s", r ? r : "-", (const char *)glGetString(GL_VERSION));
	GLuint tex, fb;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		say("FAIL the target is not complete");
		return 1;
	}
	glViewport(0, 0, 64, 64);
	glClearColor(0x33 / 255.0f, 0x99 / 255.0f, 0xcc / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	unsigned char px[4] = {0};
	glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	int ok = px[0] == 0x33 && px[1] == 0x99 && px[2] == 0xcc && px[3] == 0xff;
	double t0 = ms();
	for (int i = 0; i < 100; ++i) {
		glClearColor((i & 1) ? 1.0f : 0.0f, 0.5f, 0.25f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glFinish();
	}
	double t1 = ms();
	say("pixel %02x %02x %02x %02x (want 33 99 cc ff) | 100 frames in %.1f ms", px[0], px[1], px[2], px[3], t1 - t0);
	int mali = r && strstr(r, "Mali");
	if (mali && ok)
		say("PASS GL on the Mali through Starbite");
	else
		say("FAIL %s", !mali ? "the renderer is not the Mali (software or none)" : "the cleared pixel is wrong (the jobs did not run)");
	eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(d);
	return mali && ok ? 0 : 1;
}
