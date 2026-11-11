// sbgl.cpp (docs/WIN11-GPU-CAPS.md 5a, step 1): OpenGL on the Mali through Mesa's WGL ICD (sb-patches/0136).
// Run next to an app-local opengl32.dll + libgallium_wgl.dll (Mesa's), or with the ICD registered.
//   sbgl.exe [hw|any] [LOG] [PPM]
// hw: fail unless the renderer is Panfrost (not softpipe, llvmpipe or Microsoft's "GDI Generic").
// Checks, each read back with glReadPixels:
//   1 clear; 2 a fixed-function triangle (GL 1.1, what GDI Generic also does);
//   3 a GLSL program drawing from a VBO (GL 2.0: what cocos2d-x / Geometry Dash needs first);
//   4 a texture sampled by that program; 5 render to an FBO texture, then sample it;
//   6 SwapBuffers pace (frames per second over 3 s).
// Writes PPM (default sbgl.ppm: the last frame read back). Exit code = failed checks (or 100 + step for setup failures).
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE0 0x84C0
#define GL_CLAMP_TO_EDGE 0x812F

#define FNS(X) \
  X(GLuint, glCreateShader, (GLenum)) X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
  X(void, glCompileShader, (GLuint)) X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
  X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) X(GLuint, glCreateProgram, (void)) \
  X(void, glAttachShader, (GLuint, GLuint)) X(void, glLinkProgram, (GLuint)) X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
  X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) X(void, glUseProgram, (GLuint)) \
  X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
  X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) X(void, glUniform1i, (GLint, GLint)) \
  X(void, glGenBuffers, (GLsizei, GLuint *)) X(void, glBindBuffer, (GLenum, GLuint)) \
  X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) X(void, glEnableVertexAttribArray, (GLuint)) \
  X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
  X(void, glActiveTexture, (GLenum)) X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
  X(void, glBindFramebuffer, (GLenum, GLuint)) X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
  X(GLenum, glCheckFramebufferStatus, (GLenum))
#define DECL(r, n, a) typedef r (APIENTRY *PFN_##n) a; static PFN_##n p_##n;
FNS(DECL)

static FILE *logf;
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (logf) { va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap); fflush(logf); }
}

static const int W = 256, H = 256;
static int fails;

static void px(int x, int y, unsigned char out[4]) { glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out); }
static bool close_to(const unsigned char *p, int r, int g, int b) { return abs(p[0] - r) < 24 && abs(p[1] - g) < 24 && abs(p[2] - b) < 24; }
static void check(const char *name, int x, int y, int r, int g, int b)
{
    unsigned char p[4];
    px(x, y, p);
    bool ok = close_to(p, r, g, b);
    say("%s %s: (%d,%d) = %d,%d,%d,%d, want %d,%d,%d\n", ok ? "PASS" : "FAIL", name, x, y, p[0], p[1], p[2], p[3], r, g, b);
    if (!ok) fails++;
}

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[1024] = {0}; p_glGetShaderInfoLog(s, sizeof l, NULL, l); say("shader compile failed: %s\n", l); }
    return s;
}

static void save_ppm(const char *path)
{
    static unsigned char rgba[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; y--)
        for (int x = 0; x < W; x++) fwrite(rgba + (y * W + x) * 4, 1, 3, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    bool hw = argc > 1 && !strcmp(argv[1], "hw");
    const char *ppm = argc > 3 ? argv[3] : "sbgl.ppm";
    logf = fopen(argc > 2 ? argv[2] : "sbgl.log", "a");
    say("sbgl start (%s)\n", hw ? "hw" : "any");

    char mod[MAX_PATH] = {0};
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (gl) GetModuleFileNameA(gl, mod, sizeof mod);
    say("opengl32 loaded from %s\n", mod);

    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "sbgl";
    RegisterClassA(&wc);
    RECT rc = {0, 0, W, H};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND wnd = CreateWindowA("sbgl", "sbgl", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, rc.right - rc.left, rc.bottom - rc.top,
                             NULL, NULL, wc.hInstance, NULL);
    if (!wnd) { say("FAIL CreateWindow %lu\n", GetLastError()); return 101; }
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24; pfd.cStencilBits = 8; pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) { say("FAIL pixel format %d (%lu)\n", pf, GetLastError()); return 102; }
    HGLRC rcx = wglCreateContext(dc);
    if (!rcx || !wglMakeCurrent(dc, rcx)) { say("FAIL wglCreateContext/MakeCurrent (%lu)\n", GetLastError()); return 103; }

    const char *ven = (const char *)glGetString(GL_VENDOR), *ren = (const char *)glGetString(GL_RENDERER);
    const char *ver = (const char *)glGetString(GL_VERSION), *sl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    say("GL_VENDOR %s\nGL_RENDERER %s\nGL_VERSION %s\nGLSL %s\n", ven ? ven : "-", ren ? ren : "-", ver ? ver : "-", sl ? sl : "-");
    bool sw = !ren || strstr(ren, "softpipe") || strstr(ren, "llvmpipe") || strstr(ren, "GDI Generic");
    if (hw && sw) { say("FAIL not the Mali (renderer above)\n"); fails++; }

    glViewport(0, 0, W, H);
    // 1 clear
    glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    check("clear", 10, 10, 51, 102, 204);
    // 2 fixed function
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex2f(-0.8f, -0.8f); glVertex2f(0.8f, -0.8f); glVertex2f(0.0f, 0.8f);
    glEnd();
    check("fixed-function triangle", W / 2, H / 2, 255, 0, 0);
    check("outside the triangle", 5, H - 5, 51, 102, 204);

    // 3-5 need GL 2.0 entry points
#define LOAD(r, n, a) p_##n = (PFN_##n)wglGetProcAddress(#n);
    FNS(LOAD)
    bool gl2 = p_glCreateShader && p_glGenBuffers && p_glVertexAttribPointer && p_glUseProgram;
    say("GL 2.0 entry points: %s\n", gl2 ? "present" : "MISSING (a GL 2.0 app such as cocos2d-x crashes here)");
    if (!gl2) { fails++; save_ppm(ppm); return fails; }

    const char *vs = "attribute vec2 pos; varying vec2 uv; void main(){ uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }";
    const char *fs = "uniform vec4 col; uniform sampler2D tex; uniform int usetex; varying vec2 uv;\n"
                     "void main(){ gl_FragColor = usetex != 0 ? texture2D(tex, uv) : col; }";
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
    p_glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
    p_glBindAttribLocation(prog, 0, "pos");
    p_glLinkProgram(prog);
    GLint linked = 0; p_glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) { char l[1024] = {0}; p_glGetProgramInfoLog(prog, sizeof l, NULL, l); say("FAIL link: %s\n", l); fails++; }
    p_glUseProgram(prog);
    GLint ucol = p_glGetUniformLocation(prog, "col"), utex = p_glGetUniformLocation(prog, "tex"), uuse = p_glGetUniformLocation(prog, "usetex");
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint vbo; p_glGenBuffers(1, &vbo); p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    p_glEnableVertexAttribArray(0); p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    p_glUniform1i(uuse, 0); p_glUniform4f(ucol, 0.0f, 1.0f, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    check("GLSL program + VBO", W / 2, H / 2, 0, 255, 0);

    // 4 a 2x2 texture: left half red, right half blue (nearest)
    static const unsigned char texels[] = {255, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 255, 255};
    GLuint tex; glGenTextures(1, &tex);
    p_glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    p_glUniform1i(utex, 0); p_glUniform1i(uuse, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    check("texture left", W / 4, H / 2, 255, 0, 0);
    check("texture right", 3 * W / 4, H / 2, 0, 0, 255);

    // 5 render into an FBO texture (yellow), then sample it onto the window
    if (p_glGenFramebuffers && p_glBindFramebuffer && p_glFramebufferTexture2D && p_glCheckFramebufferStatus) {
        GLuint ft, fb;
        glGenTextures(1, &ft); glBindTexture(GL_TEXTURE_2D, ft);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        p_glGenFramebuffers(1, &fb); p_glBindFramebuffer(GL_FRAMEBUFFER, fb);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ft, 0);
        GLenum st = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) { say("FAIL FBO status 0x%x\n", st); fails++; }
        glViewport(0, 0, 64, 64);
        p_glUniform1i(uuse, 0); p_glUniform4f(ucol, 1.0f, 1.0f, 0.0f, 1.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, W, H);
        p_glUniform1i(uuse, 1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        check("FBO render-to-texture", W / 2, H / 2, 255, 255, 0);
    } else {
        say("FAIL no framebuffer objects\n"); fails++;
    }
    GLenum err = glGetError();
    if (err) { say("FAIL glGetError 0x%x\n", err); fails++; }
    save_ppm(ppm);

    // 6 pace: alternate two colours, SwapBuffers, 3 s
    DWORD t0 = GetTickCount(); int frames = 0;
    MSG m;
    while (GetTickCount() - t0 < 3000) {
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        p_glUniform1i(uuse, 0);
        p_glUniform4f(ucol, (frames & 1) ? 1.0f : 0.0f, 0.5f, (frames & 1) ? 0.0f : 1.0f, 1.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        SwapBuffers(dc);
        frames++;
    }
    say("swap pace: %d frames in 3 s = %.1f fps (%dx%d window)\n", frames, frames / 3.0, W, H);
    if (frames < 3) { say("FAIL swaps stall\n"); fails++; }

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rcx);
    ReleaseDC(wnd, dc);
    DestroyWindow(wnd);
    say("sbgl end: %d failed\n", fails);
    if (logf) fclose(logf);
    return fails;
}
