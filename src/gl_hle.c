/*
 * gl_hle.c: the game's Xbox D3D8 (XDK 5849) mapped onto OpenGL ES 3.0 --
 * WebGL2 in the browser, a GL 4.3 core context (which takes ES 3.00 shaders)
 * on Windows. The second host back end beside d3d9_hle.c: the same wrapped
 * entry points, the same guest side (d3d_hle_shared.h), and a build compiles
 * one of the two (RECOMP_RENDERER in CMakeLists.txt).
 *
 * D3D9 brought a fixed-function pipeline; GL ES has none, so this generates
 * GLSL per state combination: the vertex half from the FVF, lighting and
 * texture-coordinate state (or the title's own vertex program, translated
 * from NV2A microcode), the fragment half from the texture stages, fog and
 * alpha test. Programs are cached on that key.
 *
 * Orientation: render targets keep the guest's row order, row 0 at the top,
 * so guest memory, textures and render targets agree without flips. That
 * makes GL's window space the guest's screen space: clip y is negated after
 * projection, a D3D clockwise triangle is GL counter-clockwise, and only the
 * blit to the window flips.
 *
 * Xbox D3D keeps most state as the NV2A's own values, which are OpenGL's:
 * compare functions, blend factors and equations, stencil operations and
 * cull faces go to GL unchanged.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#else
#include <windows.h>
#include <GL/gl.h>
#endif

#include "overlay.h"
#include "d3d_hle_shared.h"
#include "d3d8_swizzle.h"

/* -- GL entry points ---------------------------------------------------------
 * Under Emscripten they are the GLES3 headers' functions. On Windows,
 * opengl32.dll exports GL 1.1 only and the rest come from wglGetProcAddress:
 * every function used here is in this list, as a pointer named after it. */

#if !defined(__EMSCRIPTEN__)
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE_WRAP_R 0x8072
#define GL_TEXTURE_MIN_LOD 0x813A
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_RGBA8 0x8058
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_FUNC_ADD 0x8006
#define GL_NUM_EXTENSIONS 0x821D
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3

#define GL_FUNCS(X) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint *)) \
    X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glBlendEquation, (GLenum)) \
    X(void, glBlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClearDepthf, (GLfloat)) \
    X(void, glDepthRangef, (GLfloat, GLfloat)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glDeleteShader, (GLuint)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint *)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glDisableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glGenSamplers, (GLsizei, GLuint *)) \
    X(void, glBindSampler, (GLuint, GLuint)) \
    X(void, glSamplerParameteri, (GLuint, GLenum, GLint)) \
    X(void, glSamplerParameterf, (GLuint, GLenum, GLfloat)) \
    X(const GLubyte *, glGetStringi, (GLenum, GLuint))

#define GL_DECL(ret, name, args) typedef ret (APIENTRY *PFN_##name) args; static PFN_##name p_##name;
GL_FUNCS(GL_DECL)
#define glActiveTexture p_glActiveTexture
#define glCompressedTexImage2D p_glCompressedTexImage2D
#define glGenFramebuffers p_glGenFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glGenRenderbuffers p_glGenRenderbuffers
#define glDeleteRenderbuffers p_glDeleteRenderbuffers
#define glBindRenderbuffer p_glBindRenderbuffer
#define glRenderbufferStorage p_glRenderbufferStorage
#define glBlitFramebuffer p_glBlitFramebuffer
#define glBlendEquation p_glBlendEquation
#define glBlendColor p_glBlendColor
#define glClearDepthf p_glClearDepthf
#define glDepthRangef p_glDepthRangef
#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glDeleteShader p_glDeleteShader
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glBindAttribLocation p_glBindAttribLocation
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform1i p_glUniform1i
#define glUniform1f p_glUniform1f
#define glUniform4fv p_glUniform4fv
#define glUniformMatrix4fv p_glUniformMatrix4fv
#define glGenBuffers p_glGenBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glGenVertexArrays p_glGenVertexArrays
#define glBindVertexArray p_glBindVertexArray
#define glVertexAttribPointer p_glVertexAttribPointer
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glDisableVertexAttribArray p_glDisableVertexAttribArray
#define glVertexAttrib4f p_glVertexAttrib4f
#define glGenSamplers p_glGenSamplers
#define glBindSampler p_glBindSampler
#define glSamplerParameteri p_glSamplerParameteri
#define glSamplerParameterf p_glSamplerParameterf
#define glGetStringi p_glGetStringi

static int gl_load(void)
{
    HMODULE lib = GetModuleHandleA("opengl32.dll");
    int ok = 1;
#define GL_LOAD(ret, name, args) \
    p_##name = (PFN_##name)(void *)wglGetProcAddress(#name); \
    if (!p_##name && lib) p_##name = (PFN_##name)(void *)GetProcAddress(lib, #name); \
    if (!p_##name) { fprintf(stderr, "[GL] missing %s\n", #name); ok = 0; }
    GL_FUNCS(GL_LOAD)
    return ok;
}
#endif

/* -- Platform: lock, clock, window --------------------------------------- */

#if defined(_WIN32)
static CRITICAL_SECTION s_lock_cs;
static void lock_init(void) { InitializeCriticalSection(&s_lock_cs); }
static void lock(void) { EnterCriticalSection(&s_lock_cs); }
static void unlock(void) { LeaveCriticalSection(&s_lock_cs); }
static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static void sleep_ms(double ms) { if (ms >= 1) Sleep((DWORD)ms); else Sleep(0); }
#else
static pthread_mutex_t s_lock_mx = PTHREAD_MUTEX_INITIALIZER;
static void lock_init(void) {}
static void lock(void) { pthread_mutex_lock(&s_lock_mx); }
static void unlock(void) { pthread_mutex_unlock(&s_lock_mx); }
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static void sleep_ms(double ms) { usleep(ms >= 1 ? (useconds_t)(ms * 1000) : 0); }
#endif

extern void *xbox_FramebufferWindowHwnd(void);
extern void xbox_FramebufferWindowExternalPresent(int on);

static int s_ready, s_failed, s_s3tc;
static uint32_t s_scale = 2;
static OverlayD3dStats s_st;
static unsigned s_hd_hits;
static uint32_t s_frame;
static GLuint s_vao, s_vbo, s_ibo, s_fbo, s_fbo_read, s_up_tex, s_samp[4];
static uint32_t s_up_w, s_up_h;

#if defined(__EMSCRIPTEN__)
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE s_ctx;

static int ctx_create(void)
{
    EmscriptenWebGLContextAttributes a;
    emscripten_webgl_init_context_attributes(&a);
    a.majorVersion = 2;
    a.minorVersion = 0;
    a.alpha = 0;
    a.depth = 0;
    a.stencil = 0;
    a.antialias = 0;               /* a multisampled window cannot take a blit */
    a.preserveDrawingBuffer = 0;
    /* The canvas is an OffscreenCanvas owned by this pthread, which never
     * returns to an event loop: frames are committed by hand. */
    a.explicitSwapControl = 1;
    s_ctx = emscripten_webgl_create_context("#canvas", &a);
    if (s_ctx <= 0 || emscripten_webgl_make_context_current(s_ctx) != EMSCRIPTEN_RESULT_SUCCESS)
        return 0;
    s_s3tc = emscripten_webgl_enable_extension(s_ctx, "WEBGL_compressed_texture_s3tc");
    return 1;
}

static void win_size(int *w, int *h)
{
    emscripten_webgl_get_drawing_buffer_size(s_ctx, w, h);
}

/* A worker's OffscreenCanvas shows its frame only when the worker returns to
 * its event loop, and this thread never does (commit() is gone from Chrome).
 * So each frame leaves as an ImageBitmap for the page, which draws it on the
 * visible canvas (web/index.html). */
EM_JS(void, web_post_frame, (void), {
    const c = GL.currentContext.GLctx.canvas;
    if (!self.skFrames) self.skFrames = new BroadcastChannel('sk-frames');
    self.skFrames.postMessage(c.transferToImageBitmap());
});

static void swap_window(void) { web_post_frame(); }
#else
static HDC s_hdc;
static HGLRC s_glrc;

static int ctx_create(void)
{
    typedef HGLRC (WINAPI *PFN_cca)(HDC, HGLRC, const int *);
    typedef BOOL (WINAPI *PFN_swi)(int);
    static const int attrs[] = {
        0x2091 /* MAJOR */, 4, 0x2092 /* MINOR */, 3,
        0x9126 /* PROFILE_MASK */, 1 /* CORE */, 0,
    };
    PIXELFORMATDESCRIPTOR pfd;
    HWND hwnd = (HWND)xbox_FramebufferWindowHwnd();
    HGLRC tmp;
    PFN_cca cca;
    PFN_swi swi;
    int pf, i, n = 0;

    if (!hwnd)
        return -1;                          /* no window yet: try again later */
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    s_hdc = GetDC(hwnd);
    pf = ChoosePixelFormat(s_hdc, &pfd);
    if (!pf || !SetPixelFormat(s_hdc, pf, &pfd)) {
        fprintf(stderr, "[GL] pixel format %d: error %lu\n", pf, GetLastError());
        return 0;
    }
    tmp = wglCreateContext(s_hdc);
    if (!tmp || !wglMakeCurrent(s_hdc, tmp)) {
        fprintf(stderr, "[GL] wglCreateContext: error %lu\n", GetLastError());
        return 0;
    }
    cca = (PFN_cca)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    s_glrc = cca ? cca(s_hdc, NULL, attrs) : NULL;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(tmp);
    if (!s_glrc || !wglMakeCurrent(s_hdc, s_glrc) || !gl_load()) {
        fprintf(stderr, "[GL] GL 4.3 core context: error %lu\n", GetLastError());
        return 0;
    }
    swi = (PFN_swi)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swi)
        swi(0);
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (i = 0; i < n; i++)
        if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i), "GL_EXT_texture_compression_s3tc"))
            s_s3tc = 1;
    {
        /* 640x480 a scale step, shrunk to fit the screen; as d3d9_hle.c. */
        RECT wa, wr = { 0, 0, (LONG)(640 * s_scale), (LONG)(480 * s_scale) };
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        while (wr.right > (wa.right - wa.left) * 9 / 10 || wr.bottom > (wa.bottom - wa.top) * 9 / 10) {
            wr.right = wr.right * 3 / 4;
            wr.bottom = wr.bottom * 3 / 4;
        }
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    return 1;
}

static void win_size(int *w, int *h)
{
    RECT cr;
    *w = *h = 0;
    if (GetClientRect((HWND)xbox_FramebufferWindowHwnd(), &cr)) {
        *w = cr.right - cr.left;
        *h = cr.bottom - cr.top;
    }
}

static void swap_window(void) { SwapBuffers(s_hdc); }
#endif

static int dev_ready(void)
{
    if (s_ready)
        return 1;
    if (s_failed)
        return 0;
    switch (ctx_create()) {
    case 1: break;
    case -1: return 0;
    default:
        fprintf(stderr, "[GL] context creation failed\n");
        s_failed = 1;
        return 0;
    }
    xbox_FramebufferWindowExternalPresent(1);
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glGenBuffers(1, &s_ibo);
    glGenFramebuffers(1, &s_fbo);
    glGenFramebuffers(1, &s_fbo_read);
    glGenSamplers(4, s_samp);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    fprintf(stderr, "[GL] %s, %s; rendering at %ux%s\n", (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION), s_scale, s_s3tc ? "" : " (no S3TC)");
    s_ready = 1;
    return 1;
}

/* -- Render targets: one host target per guest surface address ------------ */

typedef struct {
    uint32_t data, w, h;            /* guest address and size (1x) */
    uint32_t xfmt, pitch;
    uint32_t ghash, synced;
    int depth;
    GLuint tex, rb;                 /* colour texture, or depth-stencil renderbuffer */
} HostRt;

#define MAX_RT 32
static HostRt s_rt[MAX_RT];
static int s_nrt;
static HostRt *s_cur_rt, *s_cur_ds;

static HostRt *rt_find(uint32_t data)
{
    int i;
    for (i = 0; i < s_nrt; i++)
        if (s_rt[i].data == data && !s_rt[i].depth)
            return &s_rt[i];
    return NULL;
}

static HostRt *rt_get(uint32_t res, int depth)
{
    uint32_t data = res_data(res), w, h, pitch;
    HostRt *r;
    int i;

    surf_dims(res, &w, &h, &pitch);
    for (i = 0; i < s_nrt; i++) {
        r = &s_rt[i];
        if (r->data == data && r->depth == depth && r->w == w && r->h == h)
            return r;
    }
    /* ponytail: no eviction; past MAX_RT the last slot is reused. */
    r = &s_rt[s_nrt < MAX_RT ? s_nrt++ : MAX_RT - 1];
    if (r->tex) glDeleteTextures(1, &r->tex);
    if (r->rb) glDeleteRenderbuffers(1, &r->rb);
    memset(r, 0, sizeof *r);
    r->data = data; r->w = w; r->h = h; r->depth = depth;
    r->xfmt = (MEM32(res + 0xC) >> 8) & 0xFF;
    r->pitch = pitch ? pitch : w * 4;
    r->synced = (uint32_t)-1;
    if (depth) {
        glGenRenderbuffers(1, &r->rb);
        glBindRenderbuffer(GL_RENDERBUFFER, r->rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)(w * s_scale), (GLsizei)(h * s_scale));
    } else {
        glGenTextures(1, &r->tex);
        glBindTexture(GL_TEXTURE_2D, r->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)(w * s_scale), (GLsizei)(h * s_scale), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    fprintf(stderr, "[GL] %s target %08X %ux%u\n", depth ? "depth" : "colour", data, w, h);
    return r;
}

static uint32_t rt_guest_bytes(const HostRt *r) { return r->pitch * r->h; }

/* What the CPU wrote to a colour target (the XMV player decodes movie frames
 * straight into the back buffer) goes up to the host target; see d3d9_hle.c. */
static uint8_t *s_conv;
static size_t s_conv_cap;

static uint8_t *conv_buf(size_t n)
{
    if (n > s_conv_cap) {
        free(s_conv);
        s_conv_cap = n * 2;
        s_conv = (uint8_t *)malloc(s_conv_cap);
    }
    return s_conv;
}

static void rt_upload(HostRt *r)
{
    uint8_t *d = conv_buf((size_t)r->w * r->h * 4);
    uint32_t x, y, bpp16 = r->xfmt == 0x11 || r->xfmt == 0x05;

    s_st.rt_uploads++;
    for (y = 0; y < r->h; y++) {
        const uint8_t *s = (const uint8_t *)gp(r->data) + (size_t)y * r->pitch;
        uint8_t *o = d + (size_t)y * r->w * 4;
        for (x = 0; x < r->w; x++, o += 4) {
            if (bpp16) {
                uint32_t p = ((const uint16_t *)s)[x];
                uint32_t R = (p >> 11) & 31, G = (p >> 5) & 63, B = p & 31;
                o[0] = (uint8_t)(R << 3 | R >> 2); o[1] = (uint8_t)(G << 2 | G >> 4);
                o[2] = (uint8_t)(B << 3 | B >> 2); o[3] = 255;
            } else {
                o[0] = s[x * 4 + 2]; o[1] = s[x * 4 + 1]; o[2] = s[x * 4]; o[3] = 255;
            }
        }
    }
    if (!s_up_tex || s_up_w != r->w || s_up_h != r->h) {
        if (!s_up_tex) glGenTextures(1, &s_up_tex);
        glBindTexture(GL_TEXTURE_2D, s_up_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)r->w, (GLsizei)r->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        s_up_w = r->w;
        s_up_h = r->h;
    }
    glBindTexture(GL_TEXTURE_2D, s_up_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)r->w, (GLsizei)r->h, GL_RGBA, GL_UNSIGNED_BYTE, d);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo_read);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_up_tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, (GLint)r->w, (GLint)r->h, 0, 0, (GLint)(r->w * s_scale), (GLint)(r->h * s_scale),
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    s_cur_rt = NULL;                         /* the draw FBO's attachments changed */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
}

static void rt_sync(HostRt *r, int force)
{
    uint32_t h;
    if (!r || r->depth || !r->tex || (!force && r->synced == s_frame))
        return;
    r->synced = s_frame;
    h = hash_bytes((const uint8_t *)gp(r->data), rt_guest_bytes(r));
    if (h != r->ghash) {
        r->ghash = h;
        rt_upload(r);
    }
}

typedef struct { int32_t x1, y1, x2, y2; } Rect;

static void rt_fill(HostRt *r, const Rect *rects, uint32_t n, uint32_t argb)
{
    Rect full = { 0, 0, 0, 0 };
    uint32_t i, x, y, bpp16 = r->xfmt == 0x11 || r->xfmt == 0x05;
    uint16_t c16 = (uint16_t)(((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0) | ((argb >> 3) & 0x001F));

    full.x2 = (int32_t)r->w;
    full.y2 = (int32_t)r->h;
    if (!n) { rects = &full; n = 1; }
    for (i = 0; i < n; i++) {
        int32_t x1 = rects[i].x1 < 0 ? 0 : rects[i].x1, y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
        int32_t x2 = rects[i].x2 > (int32_t)r->w ? (int32_t)r->w : rects[i].x2;
        int32_t y2 = rects[i].y2 > (int32_t)r->h ? (int32_t)r->h : rects[i].y2;
        for (y = (uint32_t)y1; (int32_t)y < y2; y++) {
            uint8_t *row = (uint8_t *)gp(r->data) + (size_t)y * r->pitch;
            for (x = (uint32_t)x1; (int32_t)x < x2; x++) {
                if (bpp16) ((uint16_t *)row)[x] = c16;
                else ((uint32_t *)row)[x] = argb;
            }
        }
    }
    r->ghash = hash_bytes((const uint8_t *)gp(r->data), rt_guest_bytes(r));
}

/* Viewport as the title last set it (1x pixels). */
static struct { uint32_t x, y, w, h; float minz, maxz; } s_vp = { 0, 0, 640, 480, 0.0f, 1.0f };

static void bind_targets(void)
{
    uint32_t dev = xb_device();
    uint32_t rt = MEM32(dev + 0x1A04), ds = MEM32(dev + 0x1A08);
    HostRt *c = rt ? rt_get(rt, 0) : NULL;
    HostRt *d = ds ? rt_get(ds, 1) : NULL;
    uint32_t w = s_vp.w, h = s_vp.h;

    rt_sync(c, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    if (c != s_cur_rt) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c ? c->tex : 0, 0);
        s_cur_rt = c;
    }
    if (d != s_cur_ds) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, d ? d->rb : 0);
        s_cur_ds = d;
    }
    if (c) {
        if (s_vp.x + w > c->w) w = c->w > s_vp.x ? c->w - s_vp.x : 0;
        if (s_vp.y + h > c->h) h = c->h > s_vp.y ? c->h - s_vp.y : 0;
    }
    glViewport((GLint)(s_vp.x * s_scale), (GLint)(s_vp.y * s_scale), (GLsizei)(w * s_scale), (GLsizei)(h * s_scale));
    glDepthRangef(s_vp.minz, s_vp.maxz);
}

/* -- Textures: everything but DXT becomes RGBA8 on the CPU ----------------- */

typedef struct {
    uint8_t xfmt, bits, swizzled;
    uint8_t kind;      /* K_* below */
} XFmt;

enum { K_L8, K_AL8, K_A1RGB5, K_X1RGB5, K_ARGB4, K_RGB565, K_ARGB8, K_XRGB8,
       K_P8, K_DXT1, K_DXT3, K_DXT5, K_A8, K_A8L8, K_ABGR8 };

static const XFmt s_fmts[] = {
    { 0x00,  8, 1, K_L8 },     { 0x01,  8, 1, K_AL8 },
    { 0x02, 16, 1, K_A1RGB5 }, { 0x03, 16, 1, K_X1RGB5 },
    { 0x04, 16, 1, K_ARGB4 },  { 0x05, 16, 1, K_RGB565 },
    { 0x06, 32, 1, K_ARGB8 },  { 0x07, 32, 1, K_XRGB8 },
    { 0x0B,  8, 1, K_P8 },
    { 0x0C,  4, 0, K_DXT1 },   { 0x0E,  8, 0, K_DXT3 }, { 0x0F,  8, 0, K_DXT5 },
    { 0x10, 16, 0, K_A1RGB5 }, { 0x11, 16, 0, K_RGB565 },
    { 0x12, 32, 0, K_ARGB8 },  { 0x13,  8, 0, K_L8 },
    { 0x19,  8, 1, K_A8 },     { 0x1A, 16, 1, K_A8L8 },
    { 0x1B,  8, 0, K_AL8 },
    { 0x1C, 16, 0, K_X1RGB5 }, { 0x1D, 16, 0, K_ARGB4 },
    { 0x1E, 32, 0, K_XRGB8 },  { 0x1F,  8, 0, K_A8 },
    { 0x20, 16, 0, K_A8L8 },
    { 0x3A, 32, 1, K_ABGR8 },  { 0x3F, 32, 0, K_ABGR8 },
};

static const XFmt *fmt_find(uint32_t xfmt)
{
    size_t i;
    for (i = 0; i < sizeof s_fmts / sizeof s_fmts[0]; i++)
        if (s_fmts[i].xfmt == xfmt)
            return &s_fmts[i];
    return NULL;
}

static int fmt_is_dxt(const XFmt *f) { return f->kind == K_DXT1 || f->kind == K_DXT3 || f->kind == K_DXT5; }

static uint32_t level_bytes(const XFmt *f, uint32_t w, uint32_t h)
{
    if (fmt_is_dxt(f))
        return ((w + 3) / 4) * ((h + 3) / 4) * (f->bits * 2);
    return w * h * f->bits / 8;
}

static GLenum dxt_gl(const XFmt *f)
{
    return f->kind == K_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
         : f->kind == K_DXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
}

static uint8_t c5(uint32_t v) { return (uint8_t)(v << 3 | v >> 2); }
static uint8_t c4(uint32_t v) { return (uint8_t)(v << 4 | v); }

/* One texel of a guest format as RGBA bytes. */
static void texel_rgba(const XFmt *f, const uint8_t *s, const uint32_t *pal, uint8_t *o)
{
    uint32_t p;
    switch (f->kind) {
    case K_L8: o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
    case K_AL8: o[0] = o[1] = o[2] = o[3] = s[0]; break;
    case K_A8: o[0] = o[1] = o[2] = 0; o[3] = s[0]; break;
    case K_A8L8: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
    case K_A1RGB5: case K_X1RGB5:
        p = *(const uint16_t *)s;
        o[0] = c5((p >> 10) & 31); o[1] = c5((p >> 5) & 31); o[2] = c5(p & 31);
        o[3] = f->kind == K_X1RGB5 || (p & 0x8000) ? 255 : 0; break;
    case K_ARGB4:
        p = *(const uint16_t *)s;
        o[0] = c4((p >> 8) & 15); o[1] = c4((p >> 4) & 15); o[2] = c4(p & 15); o[3] = c4(p >> 12); break;
    case K_RGB565:
        p = *(const uint16_t *)s;
        o[0] = c5((p >> 11) & 31); o[1] = (uint8_t)(((p >> 5) & 63) << 2 | ((p >> 5) & 63) >> 4);
        o[2] = c5(p & 31); o[3] = 255; break;
    case K_ARGB8: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3]; break;
    case K_XRGB8: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = 255; break;
    case K_ABGR8: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3]; break;
    case K_P8:
        p = pal ? pal[s[0]] : 0xFFFFFFFFu;
        o[0] = (uint8_t)(p >> 16); o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)p; o[3] = (uint8_t)(p >> 24); break;
    default: o[0] = o[1] = o[2] = o[3] = 255; break;
    }
}

/* One level of a guest texture to `target` (a 2D target or cube face). */
static void upload_level(const XFmt *f, GLenum target, int lv, const uint8_t *src,
                         uint32_t w, uint32_t h, uint32_t src_pitch, const uint32_t *pal)
{
    uint32_t bpp = f->bits / 8, x, y;
    uint8_t *tmp = NULL, *d;

    if (fmt_is_dxt(f)) {
        if (s_s3tc)
            glCompressedTexImage2D(target, lv, dxt_gl(f), (GLsizei)w, (GLsizei)h, 0,
                                   (GLsizei)level_bytes(f, w, h), src);
        /* ponytail: no S3TC means no DXT textures; decode on the CPU when a
         * browser without WEBGL_compressed_texture_s3tc matters. */
        return;
    }
    if (f->swizzled) {
        tmp = (uint8_t *)malloc((size_t)w * h * bpp);
        xbox_unswizzle_rect(tmp, src, w, h, bpp);
        src = tmp;
        src_pitch = w * bpp;
    }
    d = conv_buf((size_t)w * h * 4);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            texel_rgba(f, src + (size_t)y * src_pitch + x * bpp, pal, d + ((size_t)y * w + x) * 4);
    glTexImage2D(target, lv, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d);
    free(tmp);
}

typedef struct {
    uint32_t data, format, size, palette;
    uint32_t hash, checked;
    uint32_t w, h;
    int cube;
    GLuint tex;
} HostTex;

#define MAX_TEX 2048
static HostTex s_tex[MAX_TEX];
static int s_ntex;

static GLuint tex_create(const XFmt *f, uint32_t data, uint32_t format, uint32_t w, uint32_t h,
                         uint32_t pitch, const uint32_t *pal, int cube)
{
    uint32_t levels = f->swizzled || fmt_is_dxt(f) ? (format >> 16) & 0xF : 1;
    GLenum bind = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    const uint8_t *src = (const uint8_t *)gp(data);
    int faces = cube ? 6 : 1, face, lv;
    GLuint t;

    s_st.tex_creates++;
    if (!levels) levels = 1;
    if (fmt_is_dxt(f) && !s_s3tc)
        return 0;
    glGenTextures(1, &t);
    glBindTexture(bind, t);
    for (face = 0; face < faces; face++) {
        uint32_t lw = w, lh = h, face_bytes = 0;
        GLenum target = cube ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)face : GL_TEXTURE_2D;
        for (lv = 0; lv < (int)levels; lv++) {
            upload_level(f, target, lv, src, lw, lh, pitch ? pitch : lw * f->bits / 8, pal);
            face_bytes += level_bytes(f, lw, lh);
            src += level_bytes(f, lw, lh);
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        if (cube)                                /* faces start on 128 bytes */
            src += ((face_bytes + 127) & ~127u) - face_bytes;
    }
    glTexParameteri(bind, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    return t;
}

/* RECOMP_HD_TEX=<dir>: <dir>/<hash>.dds replaces a guest texture (DXT only,
 * so it needs S3TC); see d3d9_hle.c. */
static GLuint hd_load(uint32_t hash)
{
    static const char *dir = (const char *)-1;
    char path[512];
    uint8_t hdr[128], *buf = NULL;
    uint32_t w, h, mips, fourcc, bpb, lv, off = 0;
    GLenum fmt;
    GLuint t = 0;
    long n;
    FILE *f;

    if (dir == (const char *)-1)
        dir = getenv("RECOMP_HD_TEX");
    if (!dir || !*dir || !s_s3tc)
        return 0;
    snprintf(path, sizeof path, "%s/%08x.dds", dir, hash);
    if (!(f = fopen(path, "rb")))
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f) - 128;
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || fread(hdr, 1, 128, f) != 128 || memcmp(hdr, "DDS ", 4)
        || !(buf = (uint8_t *)malloc((size_t)n)) || fread(buf, 1, (size_t)n, f) != (size_t)n)
        goto done;
    memcpy(&h, hdr + 12, 4);
    memcpy(&w, hdr + 16, 4);
    memcpy(&mips, hdr + 28, 4);
    memcpy(&fourcc, hdr + 84, 4);
    if (!mips) mips = 1;
    if (fourcc == 0x31545844u) { fmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; bpb = 8; }        /* DXT1 */
    else if (fourcc == 0x33545844u) { fmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; bpb = 16; }  /* DXT3 */
    else if (fourcc == 0x35545844u) { fmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; bpb = 16; }  /* DXT5 */
    else goto done;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    for (lv = 0; lv < mips; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
        uint32_t bytes = (lw + 3) / 4 * bpb * ((lh + 3) / 4);
        if (off + bytes > (uint32_t)n)
            break;
        glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)lv, fmt, (GLsizei)lw, (GLsizei)lh, 0, (GLsizei)bytes, buf + off);
        off += bytes;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)(lv ? lv - 1 : 0));
    if (s_hd_hits++ < 8)
        fprintf(stderr, "[HD] %08x -> %ux%u\n", hash, w, h);
done:
    free(buf);
    fclose(f);
    return t;
}

/* The host texture for a guest texture (and palette). *lin_w/*lin_h are set
 * for linear textures, whose coordinates the title gives in texels; *cube for
 * a cube map. */
static GLuint tex_get(uint32_t res, uint32_t pal_res, float *lin_w, float *lin_h, int *cube)
{
    uint32_t data = res_data(res), format = MEM32(res + 0xC), size = MEM32(res + 0x10);
    uint32_t w, h, pitch, xf = (format >> 8) & 0xFF, bytes, hash, pal = 0;
    const XFmt *f;
    HostRt *rt;
    HostTex *t = NULL;
    int i;

    surf_dims(res, &w, &h, &pitch);
    *lin_w = *lin_h = 0;
    *cube = (format >> 2) & 1;
    if ((rt = rt_find(data)) != NULL) {
        if (size) { *lin_w = (float)rt->w; *lin_h = (float)rt->h; }
        *cube = 0;
        return rt->tex;
    }
    f = fmt_find(xf);
    if (!f) {
        static uint32_t said[64];
        if (!said[xf & 63]++)
            fprintf(stderr, "[GL] texture format 0x%02X not handled\n", xf);
        return 0;
    }
    if (xf == 0x0B && pal_res)
        pal = res_data(pal_res);
    if (!f->swizzled && !fmt_is_dxt(f)) {
        *lin_w = (float)w;
        *lin_h = (float)h;
    }
    for (i = 0; i < s_ntex; i++)
        if (s_tex[i].data == data && s_tex[i].format == format && s_tex[i].size == size
            && s_tex[i].palette == pal) {
            t = &s_tex[i];
            break;
        }
    if (t && t->checked == s_frame)
        return t->tex;
    bytes = pitch ? pitch * h : level_bytes(f, w, h);
    hash = hash_bytes((const uint8_t *)gp(data), bytes);
    if (pal)
        hash ^= hash_bytes((const uint8_t *)gp(pal), 1024) * 31u;
    if (t && t->hash == hash) {
        t->checked = s_frame;
        return t->tex;
    }
    if (!t)                     /* ponytail: past MAX_TEX the last slot is recycled */
        t = &s_tex[s_ntex < MAX_TEX ? s_ntex++ : MAX_TEX - 1];
    if (t->tex)
        glDeleteTextures(1, &t->tex);
    memset(t, 0, sizeof *t);
    t->data = data; t->format = format; t->size = size; t->palette = pal;
    t->hash = hash; t->checked = s_frame; t->w = w; t->h = h; t->cube = *cube;
    t->tex = !pal && !pitch && !*cube ? hd_load(hash) : 0;
    if (!t->tex)
        t->tex = tex_create(f, data, format, w, h, pitch, pal ? (const uint32_t *)gp(pal) : NULL, *cube);
    return t->tex;
}

/* -- Mirrored state ---------------------------------------------------------- */

typedef struct { float m[16]; } Mat;
typedef struct {
    uint32_t type;
    float diffuse[4], specular[4], ambient[4], position[3], direction[3];
    float range, falloff, att0, att1, att2, theta, phi;
} Light;                                     /* D3DLIGHT8 */
typedef struct { float diffuse[4], ambient[4], specular[4], emissive[4], power; } Material;

static uint32_t s_stage_tex[4], s_stage_pal[4];
static uint32_t s_vshader;                  /* FVF, or a shader handle | 1 */
static uint32_t s_pshader;
static uint32_t s_stream_vb[16], s_stream_stride[16];
static uint32_t s_ib, s_base_vertex;
static Mat s_xf[10];                         /* view, proj, tex0-3, world0-3 */
static Light s_light[8];
static uint32_t s_light_on;
static Material s_material;
static float s_lin[4][2];                    /* 1/texel size of linear textures */
static int s_tex_cube[4];

static void mat_mul(const float *a, const float *b, float *o)   /* row-vector: o = a * b */
{
    int r, c;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}

static void argb_vec(uint32_t c, float *o)
{
    o[0] = ((c >> 16) & 0xFF) / 255.0f; o[1] = ((c >> 8) & 0xFF) / 255.0f;
    o[2] = (c & 0xFF) / 255.0f; o[3] = (c >> 24) / 255.0f;
}

/* -- Programmable vertex shaders ----------------------------------------- */

#define NV2A_VS_MAX_INSTRUCTIONS 136
#define NV2A_VS_MAX_CONSTANTS    192

typedef struct {
    uint32_t handle;
    uint32_t decl[96];
    uint32_t mc[NV2A_VS_MAX_INSTRUCTIONS * 4];
    int nmc;
    uint16_t inputs;
    int uses_vp;
    char *glsl;                       /* translated body, built on first use */
} HostVs;

#define MAX_VS 128
static HostVs s_vs[MAX_VS];
static int s_nvs;
static float s_vsc[NV2A_VS_MAX_CONSTANTS][4];

static HostVs *vs_find(uint32_t handle)
{
    int i;
    for (i = 0; i < s_nvs; i++)
        if (s_vs[i].handle == handle)
            return &s_vs[i];
    return NULL;
}

/* -- Shader generation ------------------------------------------------------ */

/* Everything the generated program depends on. Zeroed, then filled, so it
 * compares with memcmp. */
typedef struct {
    HostVs *vs;                       /* a vertex program, or NULL: fixed function */
    /* fixed-function vertex half */
    uint8_t rhw, normal, diffuse, specular, ntc, lighting, local_viewer, normalize;
    uint8_t mat_src[4];               /* diffuse, ambient, specular, emissive: 0 material, 1 colour1, 2 colour2 */
    uint8_t light_type[8];            /* 0 off, 1 point, 2 spot, 3 directional */
    uint8_t tc_set[4], tc_gen[4], tc_count[4], tc_proj[4], tc_size[8];
    /* fragment half */
    uint8_t colorop[4], colorarg[4][3], alphaop[4], alphaarg[4][3], result_temp[4];
    uint8_t tex[4];                   /* 0 none, 1 2D, 2 cube */
    uint8_t spec_add, fog, fog_table, alpha_test, alpha_func, flat, point_sprite;
} Key;

typedef struct {
    Key key;
    GLuint prog;
    GLint u_wv, u_proj, u_texm, u_mat, u_amb, u_ld, u_ls, u_la, u_lp, u_ldir, u_latt, u_lspot;
    GLint u_rhw, u_fog, u_fogcolor, u_tf, u_aref, u_tscale, u_bump, u_c, u_psize, u_tex[4];
} Prog;

#define MAX_PROG 1024
static Prog s_prog[MAX_PROG];
static int s_nprog;

/* Xbox D3DTOP (see tr_texop in d3d9_hle.c). */
enum { XOP_DISABLE = 1, XOP_SELECTARG1, XOP_SELECTARG2, XOP_MODULATE, XOP_MODULATE2X, XOP_MODULATE4X,
       XOP_ADD, XOP_ADDSIGNED, XOP_ADDSIGNED2X, XOP_SUBTRACT, XOP_ADDSMOOTH, XOP_BLENDDIFFUSEALPHA,
       XOP_BLENDCURRENTALPHA, XOP_BLENDTEXTUREALPHA, XOP_BLENDFACTORALPHA, XOP_BLENDTEXTUREALPHAPM,
       XOP_PREMODULATE, XOP_MODULATEALPHA_ADDCOLOR, XOP_MODULATECOLOR_ADDALPHA,
       XOP_MODULATEINVALPHA_ADDCOLOR, XOP_MODULATEINVCOLOR_ADDALPHA, XOP_DOTPRODUCT3, XOP_MULTIPLYADD,
       XOP_LERP, XOP_BUMPENVMAP, XOP_BUMPENVMAPLUMINANCE };

/* GLSL for a texture-stage argument (D3DTA: source in the low bits,
 * COMPLEMENT 0x10, ALPHAREPLICATE 0x20). */
static void ff_arg(Sb *s, uint32_t a, int st)
{
    static const char *const src[] = { "dif", "cur", "tx", "tf", "spe", "tmp" };
    char base[32];
    if ((a & 7) == 2)
        snprintf(base, sizeof base, "tx%d", st);
    else
        snprintf(base, sizeof base, "%s", (a & 7) < 6 ? src[a & 7] : "cur");
    if (a & 0x20) {
        if (a & 0x10) sb(s, "vec4(1.0 - %s.a)", base); else sb(s, "vec4(%s.a)", base);
    } else {
        if (a & 0x10) sb(s, "(1.0 - %s)", base); else sb(s, "%s", base);
    }
}

/* One stage operation on a0..a2, into `out` (a vec4 expression's channels). */
static void ff_op(Sb *s, uint32_t op, int st)
{
    switch (op) {
    case XOP_SELECTARG1: sb(s, "a1"); break;
    case XOP_SELECTARG2: sb(s, "a2"); break;
    case XOP_MODULATE: sb(s, "a1 * a2"); break;
    case XOP_MODULATE2X: sb(s, "a1 * a2 * 2.0"); break;
    case XOP_MODULATE4X: sb(s, "a1 * a2 * 4.0"); break;
    case XOP_ADD: sb(s, "a1 + a2"); break;
    case XOP_ADDSIGNED: sb(s, "a1 + a2 - 0.5"); break;
    case XOP_ADDSIGNED2X: sb(s, "(a1 + a2 - 0.5) * 2.0"); break;
    case XOP_SUBTRACT: sb(s, "a1 - a2"); break;
    case XOP_ADDSMOOTH: sb(s, "a1 + a2 - a1 * a2"); break;
    case XOP_BLENDDIFFUSEALPHA: sb(s, "mix(a2, a1, dif.a)"); break;
    case XOP_BLENDCURRENTALPHA: sb(s, "mix(a2, a1, cur.a)"); break;
    case XOP_BLENDTEXTUREALPHA: sb(s, "mix(a2, a1, tx%d.a)", st); break;
    case XOP_BLENDFACTORALPHA: sb(s, "mix(a2, a1, tf.a)"); break;
    case XOP_BLENDTEXTUREALPHAPM: sb(s, "a1 + a2 * (1.0 - tx%d.a)", st); break;
    case XOP_MODULATEALPHA_ADDCOLOR: sb(s, "a1 + a1.a * a2"); break;
    case XOP_MODULATECOLOR_ADDALPHA: sb(s, "a1 * a2 + a1.a"); break;
    case XOP_MODULATEINVALPHA_ADDCOLOR: sb(s, "(1.0 - a1.a) * a2 + a1"); break;
    case XOP_MODULATEINVCOLOR_ADDALPHA: sb(s, "(1.0 - a1) * a2 + a1.a"); break;
    case XOP_DOTPRODUCT3: sb(s, "vec4(clamp(dot((a1.rgb - 0.5) * 2.0, (a2.rgb - 0.5) * 2.0), 0.0, 1.0))"); break;
    case XOP_MULTIPLYADD: sb(s, "a0 + a1 * a2"); break;
    case XOP_LERP: sb(s, "mix(a2, a1, a0)"); break;
    /* ponytail: PREMODULATE and the bump ops pass the first argument;
     * bump mapping is not done. */
    default: sb(s, "a1"); break;
    }
}

static const char *const k_common =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n";

static void gen_vs(const Key *k, Sb *s)
{
    int i;
    const char *fl = k->flat ? "flat " : "";

    sb(s, "%s", k_common);
    /* WebGL2 promises 256 vertex uniform vectors: a program's 192 constants
     * and the fixed-function lights are never declared together. */
    sb(s, "uniform vec4 u_fog;\nuniform float u_psize;\n");
    if (k->vs)
        sb(s, "uniform vec4 u_c[192];\n");
    else
        sb(s, "uniform mat4 u_wv, u_proj, u_texm[4];\n"
              "uniform vec4 u_mat[5], u_amb, u_ld[8], u_ls[8], u_la[8], u_lp[8], u_ldir[8], u_latt[8], u_lspot[8];\n"
              "uniform vec4 u_rhw;\n");
    sb(s, "%sout vec4 v_d0;\n%sout vec4 v_d1;\nout vec4 v_t0, v_t1, v_t2, v_t3;\nout float v_fog, v_eyez;\n", fl, fl);
    if (k->vs) {
        /* The title's vertex program (d3d_hle_shared.h vsh_body), with the
         * GL clip conversion added. */
        for (i = 0; i < 16; i++)
            if (k->vs->inputs & (1u << i))
                sb(s, "in vec4 v%d;\n", i);
        sb(s, "float nv_fog(float d) { d = abs(d); return u_fog.x == 3.0 ? (u_fog.z - d) / (u_fog.z - u_fog.y)\n"
              "  : u_fog.x == 1.0 ? exp(-d * u_fog.w) : u_fog.x == 2.0 ? exp(-(d * u_fog.w) * (d * u_fog.w)) : 1.0 - d; }\n"
              "float rcc(float x) { float f = 1.0 / x; float m = clamp(abs(f), 5.42101e-20, 1.884467e19); return f < 0.0 ? -m : m; }\n"
              "vec4 nv_log(float x) { float a = abs(x); float e = floor(log2(a)); return a == 0.0 ? vec4(-1e38, 1.0, -1e38, 1.0) : vec4(e, a / exp2(e), log2(a), 1.0); }\n"
              "vec4 nv_lit(vec4 c) { float p = clamp(c.w, -127.9961, 127.9961); return vec4(1.0, max(c.x, 0.0), c.x > 0.0 ? pow(max(c.y, 0.0), p) : 0.0, 1.0); }\n"
              "#define c u_c\n"
              "void main() {\n"
              "  vec4 R0 = vec4(0.0), R1 = R0, R2 = R0, R3 = R0, R4 = R0, R5 = R0, R6 = R0, R7 = R0, R8 = R0, R9 = R0, R10 = R0, R11 = R0, R12 = R0;\n"
              "  vec4 oD0 = vec4(0.0, 0.0, 0.0, 1.0), oD1 = oD0, oFog = vec4(0.0);\n"
              "  vec4 oT0 = vec4(0.0, 0.0, 0.0, 1.0), oT1 = oT0, oT2 = oT0, oT3 = oT0;\n"
              "  int a0 = 0;\n");
        sb(s, "%s", k->vs->glsl);
        sb(s, "  vec4 p = %s;\n", k->vs->uses_vp ? "vec4(R12.xyz * R12.w, R12.w)" : "R12");
        sb(s, "  gl_Position = vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w);\n"
              "  v_d0 = clamp(oD0, 0.0, 1.0); v_d1 = clamp(oD1, 0.0, 1.0); v_fog = clamp(nv_fog(oFog.x), 0.0, 1.0); v_eyez = 0.0;\n"
              "  v_t0 = oT0; v_t1 = oT1; v_t2 = oT2; v_t3 = oT3;\n"
              "  gl_PointSize = u_psize;\n}\n");
        return;
    }
    sb(s, "in vec4 a_pos;\n");
    if (k->normal) sb(s, "in vec3 a_normal;\n");
    if (k->diffuse) sb(s, "in vec4 a_diffuse;\n");
    if (k->specular) sb(s, "in vec4 a_specular;\n");
    for (i = 0; i < k->ntc; i++)
        sb(s, "in vec4 a_tc%d;\n", i);
    sb(s, "void main() {\n");
    sb(s, "  vec4 dif_in = %s, spe_in = %s;\n",
       k->diffuse ? "a_diffuse.zyxw" : "vec4(1.0)", k->specular ? "a_specular.zyxw" : "vec4(0.0)");
    if (k->rhw) {
        /* Pre-transformed: guest pixels (centres on integers) to clip space. */
        sb(s, "  float w = a_pos.w != 0.0 ? 1.0 / a_pos.w : 1.0;\n"
              "  vec2 ndc = (a_pos.xy + 0.5 - u_rhw.xy) / u_rhw.zw * 2.0 - 1.0;\n"
              "  gl_Position = vec4(ndc * w, (2.0 * a_pos.z - 1.0) * w, w);\n"
              "  vec3 P = vec3(0.0), N = vec3(0.0, 0.0, 1.0);\n"
              "  v_eyez = w;\n");
    } else {
        sb(s, "  vec4 eye = u_wv * vec4(a_pos.xyz, 1.0);\n"
              "  vec4 p = u_proj * eye;\n"
              "  gl_Position = vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w);\n"
              "  vec3 P = eye.xyz;\n"
              "  v_eyez = abs(eye.z);\n");
        sb(s, "  vec3 N = %s;\n", k->normal ? (k->normalize ? "normalize(mat3(u_wv) * a_normal)" : "mat3(u_wv) * a_normal")
                                            : "vec3(0.0, 0.0, 1.0)");
    }
    if (k->lighting && !k->rhw) {
        static const char *const srcs[3] = { "", "dif_in", "spe_in" };
        sb(s, "  vec4 md = %s, ma = %s, ms = %s, me = %s;\n",
           k->mat_src[0] ? srcs[k->mat_src[0]] : "u_mat[0]", k->mat_src[1] ? srcs[k->mat_src[1]] : "u_mat[1]",
           k->mat_src[2] ? srcs[k->mat_src[2]] : "u_mat[2]", k->mat_src[3] ? srcs[k->mat_src[3]] : "u_mat[3]");
        sb(s, "  vec3 amb = u_amb.rgb, dsum = vec3(0.0), ssum = vec3(0.0);\n"
              "  vec3 V = %s;\n", k->local_viewer ? "-normalize(P)" : "vec3(0.0, 0.0, -1.0)");
        for (i = 0; i < 8; i++) {
            if (!k->light_type[i])
                continue;
            sb(s, "  {\n");
            if (k->light_type[i] == 3) {
                sb(s, "    vec3 L = -u_ldir[%d].xyz; float att = 1.0;\n", i);
            } else {
                sb(s, "    vec3 d = u_lp[%d].xyz - P; float dist = length(d); vec3 L = d / max(dist, 1e-6);\n"
                      "    float att = dist > u_lp[%d].w ? 0.0 : 1.0 / max(u_latt[%d].x + u_latt[%d].y * dist + u_latt[%d].z * dist * dist, 1e-6);\n",
                   i, i, i, i, i);
                if (k->light_type[i] == 2)
                    sb(s, "    float rho = dot(-L, u_ldir[%d].xyz);\n"
                          "    att *= rho > u_lspot[%d].x ? 1.0 : rho <= u_lspot[%d].y ? 0.0\n"
                          "         : pow((rho - u_lspot[%d].y) / max(u_lspot[%d].x - u_lspot[%d].y, 1e-6), u_ldir[%d].w);\n",
                       i, i, i, i, i, i, i);
            }
            sb(s, "    amb += att * u_la[%d].rgb;\n"
                  "    float nl = max(dot(N, L), 0.0);\n"
                  "    dsum += att * nl * u_ld[%d].rgb;\n"
                  "    if (nl > 0.0) ssum += att * pow(max(dot(N, normalize(L + V)), 0.0), u_mat[4].x) * u_ls[%d].rgb;\n"
                  "  }\n", i, i, i);
        }
        sb(s, "  v_d0 = vec4(clamp(me.rgb + ma.rgb * amb + md.rgb * dsum, 0.0, 1.0), md.a);\n"
              "  v_d1 = vec4(clamp(ms.rgb * ssum, 0.0, 1.0), spe_in.a);\n");
    } else {
        sb(s, "  v_d0 = dif_in; v_d1 = spe_in;\n");
    }
    for (i = 0; i < 4; i++) {
        const char *in;
        char buf[64];
        if (k->tc_gen[i] == 1) in = "vec4(N, 1.0)";
        else if (k->tc_gen[i] == 2) in = "vec4(P, 1.0)";
        else if (k->tc_gen[i] == 3) in = "vec4(reflect(normalize(P), N), 1.0)";
        else if (k->tc_set[i] < k->ntc) {
            /* D3D pads a 2D coordinate as (u, v, 1, 0) for its texture
             * matrix, so a 2D transform's translation is in the third row. */
            int n = k->tc_size[k->tc_set[i]];
            snprintf(buf, sizeof buf, n == 1 ? "vec4(a_tc%d.x, 1.0, 0.0, 0.0)" : n == 2 ? "vec4(a_tc%d.xy, 1.0, 0.0)"
                     : n == 3 ? "vec4(a_tc%d.xyz, 1.0)" : "a_tc%d", k->tc_set[i]);
            in = buf;
        } else in = "vec4(0.0, 0.0, 0.0, 1.0)";
        if (k->tc_count[i])
            sb(s, "  v_t%d = u_texm[%d] * %s;\n", i, i, in);
        else
            sb(s, "  v_t%d = %s;\n", i, in);
    }
    if (k->fog && !k->fog_table)
        sb(s, "  v_fog = spe_in.a;\n");
    else
        sb(s, "  v_fog = 1.0;\n");
    sb(s, "  gl_PointSize = u_psize;\n}\n");
}

static void gen_fs(const Key *k, Sb *s)
{
    int st;
    const char *fl = k->flat ? "flat " : "";

    sb(s, "%s", k_common);
    sb(s, "%sin vec4 v_d0;\n%sin vec4 v_d1;\nin vec4 v_t0, v_t1, v_t2, v_t3;\nin float v_fog, v_eyez;\n", fl, fl);
    sb(s, "uniform vec4 u_tf, u_fog, u_fogcolor, u_tscale[4];\nuniform float u_aref;\n");
    for (st = 0; st < 4; st++)
        if (k->tex[st])
            sb(s, "uniform %s u_tex%d;\n", k->tex[st] == 2 ? "samplerCube" : "sampler2D", st);
    sb(s, "out vec4 o_color;\n"
          "void main() {\n"
          "  vec4 dif = v_d0, spe = v_d1, tf = u_tf, cur = dif, tmp = vec4(0.0), a0, a1, a2;\n");
    for (st = 0; st < 4; st++) {
        if (k->tex[st] == 2)
            sb(s, "  vec4 tx%d = texture(u_tex%d, v_t%d.xyz);\n", st, st, st);
        else if (k->tex[st] && k->tc_proj[st])
            sb(s, "  vec4 tx%d = texture(u_tex%d, v_t%d.xy / v_t%d.%c * u_tscale[%d].xy);\n", st, st, st, st,
               k->tc_count[st] == 3 ? 'z' : k->tc_count[st] == 2 ? 'y' : 'w', st);
        else if (k->tex[st])
            sb(s, "  vec4 tx%d = texture(u_tex%d, v_t%d.xy * u_tscale[%d].xy);\n", st, st, st, st);
        else
            sb(s, "  vec4 tx%d = vec4(1.0);\n", st);
    }
    for (st = 0; st < 4; st++) {
        if (k->colorop[st] == XOP_DISABLE || !k->colorop[st])
            break;
        sb(s, "  a0 = "); ff_arg(s, k->colorarg[st][0], st);
        sb(s, "; a1 = "); ff_arg(s, k->colorarg[st][1], st);
        sb(s, "; a2 = "); ff_arg(s, k->colorarg[st][2], st);
        sb(s, ";\n  vec3 c%d = clamp((", st); ff_op(s, k->colorop[st], st); sb(s, ").rgb, 0.0, 1.0);\n");
        if (k->alphaop[st] == XOP_DISABLE || !k->alphaop[st]) {
            sb(s, "  float x%d = cur.a;\n", st);
        } else {
            sb(s, "  a0 = "); ff_arg(s, k->alphaarg[st][0], st);
            sb(s, "; a1 = "); ff_arg(s, k->alphaarg[st][1], st);
            sb(s, "; a2 = "); ff_arg(s, k->alphaarg[st][2], st);
            sb(s, ";\n  float x%d = clamp((", st); ff_op(s, k->alphaop[st], st); sb(s, ").a, 0.0, 1.0);\n");
        }
        sb(s, "  %s = vec4(c%d, x%d);\n", k->result_temp[st] ? "tmp" : "cur", st, st);
    }
    if (k->spec_add)
        sb(s, "  cur.rgb = clamp(cur.rgb + spe.rgb, 0.0, 1.0);\n");
    if (k->fog) {
        if (k->fog_table)
            sb(s, "  float f = u_fog.x == 3.0 ? (u_fog.z - v_eyez) / (u_fog.z - u_fog.y)\n"
                  "          : u_fog.x == 1.0 ? exp(-v_eyez * u_fog.w) : exp(-(v_eyez * u_fog.w) * (v_eyez * u_fog.w));\n");
        else
            sb(s, "  float f = v_fog;\n");
        sb(s, "  cur.rgb = mix(u_fogcolor.rgb, cur.rgb, clamp(f, 0.0, 1.0));\n");
    }
    if (k->alpha_test) {
        /* GL_NEVER 0x200 .. GL_ALWAYS 0x207, as the Xbox stores them. */
        static const char *const cmp[8] = { "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        int f = k->alpha_func & 7;
        if (f == 0) sb(s, "  discard;\n");
        else if (f != 7) sb(s, "  if (!(floor(cur.a * 255.0 + 0.5) %s u_aref)) discard;\n", cmp[f]);
    }
    sb(s, "  o_color = cur;\n}\n");
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "[GL] %s shader does not compile:\n%s\n--- source ---\n%s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log, src);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static char s_src[256 * 1024];

static Prog *prog_get(const Key *k)
{
    static const char *const attr[] = { "a_pos", "a_normal", "a_diffuse", "a_specular", "a_psize",
                                        "a_tc0", "a_tc1", "a_tc2", "a_tc3", "a_tc4", "a_tc5", "a_tc6", "a_tc7" };
    Prog *p;
    GLuint vs, fs;
    GLint ok = 0;
    Sb s = { s_src, sizeof s_src, 0 };
    int i;
    char name[32];

    for (i = 0; i < s_nprog; i++)
        if (!memcmp(&s_prog[i].key, k, sizeof *k))
            return s_prog[i].prog ? &s_prog[i] : NULL;
    if (s_nprog == MAX_PROG)
        return NULL;
    p = &s_prog[s_nprog++];
    memset(p, 0, sizeof *p);
    p->key = *k;
    s_st.vs_compiles++;
    s_src[0] = 0;
    gen_vs(k, &s);
    {
        const char *dump = getenv("RECOMP_GL_DUMP_SHADERS");
        if (dump) {
            char path[512];
            FILE *f;
            snprintf(path, sizeof path, "%s/prog_%04d.vert", dump, s_nprog - 1);
            if ((f = fopen(path, "w")) != NULL) { fputs(s_src, f); fclose(f); }
        }
    }
    vs = compile(GL_VERTEX_SHADER, s_src);
    s.len = 0;
    s_src[0] = 0;
    gen_fs(k, &s);
    fs = compile(GL_FRAGMENT_SHADER, s_src);
    if (!vs || !fs)
        return NULL;
    p->prog = glCreateProgram();
    glAttachShader(p->prog, vs);
    glAttachShader(p->prog, fs);
    if (k->vs) {
        for (i = 0; i < 16; i++) {
            snprintf(name, sizeof name, "v%d", i);
            glBindAttribLocation(p->prog, (GLuint)i, name);
        }
    } else {
        for (i = 0; i < (int)(sizeof attr / sizeof attr[0]); i++)
            glBindAttribLocation(p->prog, (GLuint)i, attr[i]);
    }
    glLinkProgram(p->prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
        fprintf(stderr, "[GL] program does not link: %s\n", log);
        p->prog = 0;
        return NULL;
    }
#define U(f, n) p->f = glGetUniformLocation(p->prog, n)
    U(u_wv, "u_wv"); U(u_proj, "u_proj"); U(u_texm, "u_texm"); U(u_mat, "u_mat"); U(u_amb, "u_amb");
    U(u_ld, "u_ld"); U(u_ls, "u_ls"); U(u_la, "u_la"); U(u_lp, "u_lp"); U(u_ldir, "u_ldir");
    U(u_latt, "u_latt"); U(u_lspot, "u_lspot"); U(u_rhw, "u_rhw"); U(u_fog, "u_fog");
    U(u_fogcolor, "u_fogcolor"); U(u_tf, "u_tf"); U(u_aref, "u_aref"); U(u_tscale, "u_tscale");
    U(u_c, "u_c"); U(u_psize, "u_psize");
#undef U
    glUseProgram(p->prog);
    for (i = 0; i < 4; i++) {
        snprintf(name, sizeof name, "u_tex%d", i);
        p->u_tex[i] = glGetUniformLocation(p->prog, name);
        if (p->u_tex[i] >= 0)
            glUniform1i(p->u_tex[i], i);
    }
    return p;
}

/* -- Per-draw state ------------------------------------------------------ */

static void set_cap(GLenum cap, int on) { if (on) glEnable(cap); else glDisable(cap); }

static void apply_render_state(void)
{
    uint32_t cull = xb_rs(XRS_CULLMODE), cw = xb_rs(XRS_COLORWRITEENABLE);
    int depth = s_cur_ds != NULL;

    set_cap(GL_DEPTH_TEST, depth && xb_rs(XRS_ZENABLE));
    glDepthFunc(xb_rs(XRS_ZFUNC) >= 0x200 && xb_rs(XRS_ZFUNC) <= 0x207 ? xb_rs(XRS_ZFUNC) : GL_ALWAYS);
    glDepthMask(xb_rs(XRS_ZWRITEENABLE) ? GL_TRUE : GL_FALSE);
    set_cap(GL_BLEND, xb_rs(XRS_ALPHABLENDENABLE) != 0);
    glBlendFunc(xb_rs(XRS_SRCBLEND), xb_rs(XRS_DESTBLEND));
    glBlendEquation(xb_rs(XRS_BLENDOP) ? xb_rs(XRS_BLENDOP) : GL_FUNC_ADD);
    {
        float bc[4];
        argb_vec(xb_rs(XRS_BLENDCOLOR), bc);
        glBlendColor(bc[0], bc[1], bc[2], bc[3]);
    }
    glColorMask((cw >> 16) & 1, (cw >> 8) & 1, cw & 1, (cw >> 24) & 1);
    set_cap(GL_STENCIL_TEST, depth && xb_rs(XRS_STENCILENABLE));
    glStencilFunc(xb_rs(XRS_STENCILFUNC) >= 0x200 ? xb_rs(XRS_STENCILFUNC) : GL_ALWAYS,
                  (GLint)xb_rs(XRS_STENCILREF), xb_rs(XRS_STENCILMASK));
    glStencilMask(xb_rs(XRS_STENCILWRITEMASK));
    glStencilOp(xb_rs(XRS_STENCILFAIL) ? xb_rs(XRS_STENCILFAIL) : GL_ZERO,
                xb_rs(XRS_STENCILZFAIL) ? xb_rs(XRS_STENCILZFAIL) : GL_ZERO,
                xb_rs(XRS_STENCILPASS) ? xb_rs(XRS_STENCILPASS) : GL_ZERO);
    /* A D3D clockwise triangle is counter-clockwise in GL's (unflipped)
     * window space: see the top of the file. */
    set_cap(GL_CULL_FACE, cull == 0x900 || cull == 0x901);
    glCullFace(GL_BACK);
    glFrontFace(cull == 0x900 ? GL_CW : GL_CCW);
    {
        int solid = xb_rs(XRS_SOLIDOFFSETENABLE) != 0;
        float slope = solid ? f32(xb_rs(XRS_POLYGONOFFSETZSLOPESCALE)) : 0.0f;
        float units = solid ? f32(xb_rs(XRS_POLYGONOFFSETZOFFSET)) : 0.0f;
        units -= (float)xb_rs(XRS_ZBIAS) * 256.0f;          /* as d3d9_hle.c, in 2^-24 steps */
        set_cap(GL_POLYGON_OFFSET_FILL, slope != 0.0f || units != 0.0f);
        glPolygonOffset(slope, units);
    }
    glDisable(GL_SCISSOR_TEST);
}

static GLint gl_wrap(uint32_t v)
{
    return v == 1 ? GL_REPEAT : v == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
}

static void apply_stages(Key *k)
{
    int st;
    for (st = 0; st < 4; st++) {
        GLuint t = 0;
        float lw = 0, lh = 0;
        int cube = 0;
        uint32_t mag = xb_tss(st, XTSS_MAGFILTER), min = xb_tss(st, XTSS_MINFILTER), mip = xb_tss(st, XTSS_MIPFILTER);

        if (s_stage_tex[st])
            t = tex_get(s_stage_tex[st], s_stage_pal[st], &lw, &lh, &cube);
        s_lin[st][0] = lw ? 1.0f / lw : 0;
        s_lin[st][1] = lh ? 1.0f / lh : 0;
        s_tex_cube[st] = cube;
        k->tex[st] = t ? (cube ? 2 : 1) : 0;
        glActiveTexture(GL_TEXTURE0 + (GLenum)st);
        glBindTexture(cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, t);
        glBindSampler((GLuint)st, s_samp[st]);
        glSamplerParameteri(s_samp[st], GL_TEXTURE_WRAP_S, gl_wrap(xb_tss(st, XTSS_ADDRESSU)));
        glSamplerParameteri(s_samp[st], GL_TEXTURE_WRAP_T, gl_wrap(xb_tss(st, XTSS_ADDRESSV)));
        glSamplerParameteri(s_samp[st], GL_TEXTURE_WRAP_R, gl_wrap(xb_tss(st, XTSS_ADDRESSW)));
        glSamplerParameteri(s_samp[st], GL_TEXTURE_MAG_FILTER, mag == 1 ? GL_NEAREST : GL_LINEAR);
        glSamplerParameteri(s_samp[st], GL_TEXTURE_MIN_FILTER,
                            mip == 0 ? (min == 1 ? GL_NEAREST : GL_LINEAR)
                            : mip == 1 ? (min == 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST)
                            : (min == 1 ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR));
        glSamplerParameterf(s_samp[st], GL_TEXTURE_MIN_LOD, (float)xb_tss(st, XTSS_MAXMIPLEVEL));
        k->colorop[st] = (uint8_t)xb_tss(st, XTSS_COLOROP);
        k->alphaop[st] = (uint8_t)xb_tss(st, XTSS_ALPHAOP);
        k->colorarg[st][0] = (uint8_t)xb_tss(st, XTSS_COLORARG0);
        k->colorarg[st][1] = (uint8_t)xb_tss(st, XTSS_COLORARG1);
        k->colorarg[st][2] = (uint8_t)xb_tss(st, XTSS_COLORARG2);
        k->alphaarg[st][0] = (uint8_t)xb_tss(st, XTSS_ALPHAARG0);
        k->alphaarg[st][1] = (uint8_t)xb_tss(st, XTSS_ALPHAARG1);
        k->alphaarg[st][2] = (uint8_t)xb_tss(st, XTSS_ALPHAARG2);
        k->result_temp[st] = xb_tss(st, XTSS_RESULTARG) == 5;
    }
    glActiveTexture(GL_TEXTURE0);
}

static void key_vertex(Key *k)
{
    uint32_t fvf = s_vshader, ntc = (fvf >> 8) & 0xF, i;

    if (k->vs)
        return;
    k->rhw = (fvf & 0x00E) == 0x004;
    k->normal = (fvf & 0x010) != 0;
    k->diffuse = (fvf & 0x040) != 0;
    k->specular = (fvf & 0x080) != 0;
    k->ntc = (uint8_t)(ntc > 8 ? 8 : ntc);
    for (i = 0; i < k->ntc; i++) {
        uint32_t sz = (fvf >> (16 + 2 * i)) & 3;
        k->tc_size[i] = (uint8_t)(sz == 0 ? 2 : sz == 1 ? 3 : sz == 2 ? 4 : 1);
    }
    k->lighting = xb_rs(XRS_LIGHTING) != 0 && !k->rhw;
    if (k->lighting) {
        k->local_viewer = xb_rs(XRS_LOCALVIEWER) != 0;
        k->normalize = xb_rs(XRS_NORMALIZENORMALS) != 0;
        if (xb_rs(XRS_COLORVERTEX)) {
            k->mat_src[0] = (uint8_t)xb_rs(XRS_DIFFUSEMATERIALSOURCE);
            k->mat_src[1] = (uint8_t)xb_rs(XRS_AMBIENTMATERIALSOURCE);
            k->mat_src[2] = (uint8_t)xb_rs(XRS_SPECULARMATERIALSOURCE);
            k->mat_src[3] = (uint8_t)xb_rs(XRS_EMISSIVEMATERIALSOURCE);
            for (i = 0; i < 4; i++)               /* a colour the FVF lacks reads the material */
                if ((k->mat_src[i] == 1 && !k->diffuse) || (k->mat_src[i] == 2 && !k->specular) || k->mat_src[i] > 2)
                    k->mat_src[i] = 0;
        }
        for (i = 0; i < 8; i++)
            if ((s_light_on >> i) & 1)
                k->light_type[i] = (uint8_t)(s_light[i].type >= 1 && s_light[i].type <= 3 ? s_light[i].type : 0);
    }
    for (i = 0; i < 4; i++) {
        uint32_t tci = xb_tss(i, XTSS_TEXCOORDINDEX), ttf = xb_tss(i, XTSS_TEXTURETRANSFORMFLAGS);
        k->tc_set[i] = (uint8_t)(tci & 7);
        k->tc_gen[i] = (uint8_t)((tci >> 16) & 3);
        k->tc_count[i] = (uint8_t)(ttf & 7);
        k->tc_proj[i] = (ttf & 0x100) != 0;
    }
}

static void upload_uniforms(const Prog *p)
{
    float v[16 * 4], wv[16], t[4];
    int i;

    if (p->u_wv >= 0) {
        mat_mul(s_xf[6].m, s_xf[0].m, wv);                  /* world0 * view */
        glUniformMatrix4fv(p->u_wv, 1, GL_FALSE, wv);
    }
    if (p->u_proj >= 0) {
        /* The movie quad sits exactly on the far plane; see d3d9_hle.c. */
        Mat pr = s_xf[1];
        const float kz = 1.0f - 1.0f / 4194304.0f;
        pr.m[2] *= kz; pr.m[6] *= kz; pr.m[10] *= kz; pr.m[14] *= kz;
        glUniformMatrix4fv(p->u_proj, 1, GL_FALSE, pr.m);
    }
    if (p->u_texm >= 0) {
        for (i = 0; i < 4; i++)
            memcpy(v + 16 * i, s_xf[2 + i].m, 64);
        glUniformMatrix4fv(p->u_texm, 4, GL_FALSE, v);
    }
    if (p->u_mat >= 0) {
        memcpy(v, s_material.diffuse, 16); memcpy(v + 4, s_material.ambient, 16);
        memcpy(v + 8, s_material.specular, 16); memcpy(v + 12, s_material.emissive, 16);
        v[16] = s_material.power; v[17] = v[18] = v[19] = 0;
        glUniform4fv(p->u_mat, 5, v);
    }
    if (p->u_amb >= 0) {
        argb_vec(xb_rs(XRS_AMBIENT), t);
        glUniform4fv(p->u_amb, 1, t);
    }
    if (p->u_ld >= 0) {
        /* Lights go to eye space here, where the shader lights. */
        float ld[32], ls[32], la[32], lp[32], ldir[32], latt[32], lspot[32];
        const float *vw = s_xf[0].m;
        for (i = 0; i < 8; i++) {
            const Light *L = &s_light[i];
            const float *P = L->position, *D = L->direction;
            memcpy(ld + 4 * i, L->diffuse, 16); memcpy(ls + 4 * i, L->specular, 16); memcpy(la + 4 * i, L->ambient, 16);
            lp[4 * i] = P[0] * vw[0] + P[1] * vw[4] + P[2] * vw[8] + vw[12];
            lp[4 * i + 1] = P[0] * vw[1] + P[1] * vw[5] + P[2] * vw[9] + vw[13];
            lp[4 * i + 2] = P[0] * vw[2] + P[1] * vw[6] + P[2] * vw[10] + vw[14];
            lp[4 * i + 3] = L->range > 0 ? L->range : 1e30f;
            ldir[4 * i] = D[0] * vw[0] + D[1] * vw[4] + D[2] * vw[8];
            ldir[4 * i + 1] = D[0] * vw[1] + D[1] * vw[5] + D[2] * vw[9];
            ldir[4 * i + 2] = D[0] * vw[2] + D[1] * vw[6] + D[2] * vw[10];
            {
                float n = (float)sqrt(ldir[4 * i] * ldir[4 * i] + ldir[4 * i + 1] * ldir[4 * i + 1] + ldir[4 * i + 2] * ldir[4 * i + 2]);
                if (n > 0) { ldir[4 * i] /= n; ldir[4 * i + 1] /= n; ldir[4 * i + 2] /= n; }
            }
            ldir[4 * i + 3] = L->falloff;
            latt[4 * i] = L->att0; latt[4 * i + 1] = L->att1; latt[4 * i + 2] = L->att2; latt[4 * i + 3] = 0;
            lspot[4 * i] = (float)cos(L->theta * 0.5f); lspot[4 * i + 1] = (float)cos(L->phi * 0.5f);
            lspot[4 * i + 2] = lspot[4 * i + 3] = 0;
        }
        glUniform4fv(p->u_ld, 8, ld); glUniform4fv(p->u_ls, 8, ls); glUniform4fv(p->u_la, 8, la);
        glUniform4fv(p->u_lp, 8, lp); glUniform4fv(p->u_ldir, 8, ldir); glUniform4fv(p->u_latt, 8, latt);
        glUniform4fv(p->u_lspot, 8, lspot);
    }
    if (p->u_rhw >= 0) {
        t[0] = (float)s_vp.x; t[1] = (float)s_vp.y; t[2] = (float)(s_vp.w ? s_vp.w : 1); t[3] = (float)(s_vp.h ? s_vp.h : 1);
        glUniform4fv(p->u_rhw, 1, t);
    }
    if (p->u_fog >= 0) {
        t[0] = (float)xb_rs(XRS_FOGTABLEMODE); t[1] = f32(xb_rs(XRS_FOGSTART));
        t[2] = f32(xb_rs(XRS_FOGEND)); t[3] = f32(xb_rs(XRS_FOGDENSITY));
        glUniform4fv(p->u_fog, 1, t);
    }
    if (p->u_fogcolor >= 0) { argb_vec(xb_rs(XRS_FOGCOLOR), t); glUniform4fv(p->u_fogcolor, 1, t); }
    if (p->u_tf >= 0) { argb_vec(xb_rs(XRS_TEXTUREFACTOR), t); glUniform4fv(p->u_tf, 1, t); }
    if (p->u_aref >= 0) glUniform1f(p->u_aref, (float)(xb_rs(XRS_ALPHAREF) & 0xFF));
    if (p->u_tscale >= 0) {
        for (i = 0; i < 4; i++) {
            v[4 * i] = s_lin[i][0] ? s_lin[i][0] : 1.0f;
            v[4 * i + 1] = s_lin[i][1] ? s_lin[i][1] : 1.0f;
            v[4 * i + 2] = v[4 * i + 3] = 1.0f;
        }
        glUniform4fv(p->u_tscale, 4, v);
    }
    if (p->u_c >= 0 && p->key.vs) {
        /* c[58]/c[59] identity: the translated program undoes D3D's divide. */
        s_vsc[58][0] = s_vsc[58][1] = s_vsc[58][2] = s_vsc[58][3] = 1.0f;
        s_vsc[59][0] = s_vsc[59][1] = s_vsc[59][2] = s_vsc[59][3] = 0.0f;
        glUniform4fv(p->u_c, NV2A_VS_MAX_CONSTANTS, &s_vsc[0][0]);
    }
    if (p->u_psize >= 0) glUniform1f(p->u_psize, f32(xb_rs(XRS_POINTSIZE)) * (float)s_scale);
}

/* -- Draws ------------------------------------------------------------------- */

static uint32_t fvf_stride(uint32_t fvf)
{
    uint32_t n = 0, i, tc = (fvf >> 8) & 0xF;
    switch (fvf & 0x00E) {
    case 0x002: n = 12; break; case 0x004: n = 16; break; case 0x006: n = 16; break;
    case 0x008: n = 20; break; case 0x00A: n = 24; break; case 0x00C: n = 28; break;
    }
    if (fvf & 0x010) n += 12;
    if (fvf & 0x020) n += 4;
    if (fvf & 0x040) n += 4;
    if (fvf & 0x080) n += 4;
    for (i = 0; i < tc; i++) {
        uint32_t sz = (fvf >> (16 + 2 * i)) & 3;
        n += sz == 0 ? 8 : sz == 1 ? 12 : sz == 2 ? 16 : 4;
    }
    return n;
}

/* Attribute pointers for an FVF vertex in the bound array buffer. */
static void fvf_attribs(uint32_t fvf, uint32_t stride)
{
    uint32_t off = 0, i, ntc = (fvf >> 8) & 0xF, pos = fvf & 0x00E;
    for (i = 0; i < 13; i++)
        glDisableVertexAttribArray(i);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, pos == 0x004 ? 4 : 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)(uintptr_t)off);
    off += pos == 0x004 ? 16 : 12;
    if (pos >= 0x006 && pos <= 0x00C)             /* ponytail: blend weights are skipped, world0 only */
        off += 4 * ((pos - 0x004) / 2);
    if (fvf & 0x010) {
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)(uintptr_t)off);
        off += 12;
    }
    if (fvf & 0x020) off += 4;
    if (fvf & 0x040) {
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, (const void *)(uintptr_t)off);
        off += 4;
    }
    if (fvf & 0x080) {
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, (const void *)(uintptr_t)off);
        off += 4;
    }
    for (i = 0; i < ntc && i < 8; i++) {
        uint32_t sz = (fvf >> (16 + 2 * i)) & 3, n = sz == 0 ? 2 : sz == 1 ? 3 : sz == 2 ? 4 : 1;
        glEnableVertexAttribArray(5 + i);
        glVertexAttribPointer(5 + i, (GLint)n, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)(uintptr_t)off);
        off += n * 4;
    }
}

static uint8_t *s_vsbuf;
static size_t s_vsbuf_cap;

/* Vertices [lo, hi] for a vertex program, as float4 per register; see
 * d3d9_hle.c's vs_vertices. */
static const void *vs_vertices(HostVs *h, uint32_t base, uint32_t lo, uint32_t hi,
                               const uint8_t *up, uint32_t up_stride, uint32_t *stride_out, uint16_t *regs_out)
{
    VsInput in[16];
    int n = vs_inputs(h->decl, in), slot[16], ns = 0, k;
    uint16_t regs = h->inputs;
    uint32_t i, stride;
    const uint8_t *src[16];
    uint32_t sstride[16];

    for (k = 0; k < n; k++)
        regs |= 1u << in[k].reg;
    for (k = 0; k < 16; k++)
        slot[k] = (regs & (1u << k)) ? ns++ : -1;
    if (!ns) {
        regs = 1;
        slot[0] = ns++;
    }
    stride = 16 * (uint32_t)ns;
    if ((size_t)(hi - lo + 1) * stride > s_vsbuf_cap) {
        free(s_vsbuf);
        s_vsbuf_cap = (size_t)(hi - lo + 1) * stride * 2;
        s_vsbuf = (uint8_t *)malloc(s_vsbuf_cap);
    }
    for (k = 0; k < 16; k++) {
        if (k == 0 && up) {
            src[k] = up; sstride[k] = up_stride;
        } else {
            src[k] = s_stream_vb[k] ? (const uint8_t *)gp(res_data(s_stream_vb[k])) : NULL;
            sstride[k] = s_stream_stride[k];
        }
    }
    for (i = lo; i <= hi; i++) {
        float *o = (float *)(s_vsbuf + (size_t)(i - lo) * stride);
        for (k = 0; k < ns; k++) {
            o[k * 4] = o[k * 4 + 1] = o[k * 4 + 2] = 0.0f; o[k * 4 + 3] = 1.0f;
        }
        for (k = 0; k < n; k++) {
            const uint8_t *s = src[in[k].stream];
            if (s)
                vsdt_read(in[k].type, s + (size_t)(base + i) * sstride[in[k].stream] + in[k].offset,
                          o + slot[in[k].reg] * 4);
        }
    }
    *stride_out = stride;
    *regs_out = regs;
    return s_vsbuf;
}

static char s_vsh_body[128 * 1024];

static Prog *prepare_draw(HostVs **vs_out)
{
    Key k;
    HostVs *vs = NULL;

    if (!dev_ready())
        return NULL;
    if (s_vshader & 1) {
        vs = vs_find(s_vshader);
        if (!vs)
            return NULL;
        if (!vs->glsl) {
            Sb s = { s_vsh_body, sizeof s_vsh_body, 0 };
            s_vsh_body[0] = 0;
            vsh_body(vs->mc, vs->nmc, &s, &vs->inputs, &vs->uses_vp, 1);
            vs->glsl = (char *)malloc(s.len + 1);
            memcpy(vs->glsl, s_vsh_body, s.len + 1);
        }
    }
    if (s_pshader) {
        static int said;
        if (!said++)
            fprintf(stderr, "[GL] pixel shaders not done yet; using texture stages\n");
    }
    bind_targets();
    if (!s_cur_rt)
        return NULL;
    memset(&k, 0, sizeof k);
    k.vs = vs;
    key_vertex(&k);
    apply_render_state();
    apply_stages(&k);
    k.spec_add = xb_rs(XRS_SPECULARENABLE) != 0;
    k.fog = xb_rs(XRS_FOGENABLE) != 0;
    k.fog_table = k.fog && xb_rs(XRS_FOGTABLEMODE) != 0 && !vs;
    k.alpha_test = xb_rs(XRS_ALPHATESTENABLE) != 0;
    k.alpha_func = k.alpha_test ? (uint8_t)(xb_rs(XRS_ALPHAFUNC) - 0x200) : 0;
    k.flat = xb_rs(XRS_SHADEMODE) == 0x1D00;
    *vs_out = vs;
    {
        Prog *p = prog_get(&k);
        if (!p)
            return NULL;
        glUseProgram(p->prog);
        upload_uniforms(p);
        return p;
    }
}

static GLenum prim_gl(uint32_t xprim, uint32_t n, uint32_t *count)
{
    switch (xprim) {
    case 1: *count = n; return GL_POINTS;
    case 2: *count = n; return GL_LINES;
    case 3: *count = n; return GL_LINE_LOOP;
    case 4: *count = n; return GL_LINE_STRIP;
    case 5: *count = n; return GL_TRIANGLES;
    case 6: case 9: *count = n; return GL_TRIANGLE_STRIP;   /* quad strip: same order */
    case 7: case 10: *count = n; return GL_TRIANGLE_FAN;    /* polygon: convex */
    }
    *count = 0;
    return GL_TRIANGLES;
}

static uint16_t *s_qidx;
static size_t s_qidx_cap;

static uint16_t *qidx(size_t n)
{
    if (n > s_qidx_cap) {
        free(s_qidx);
        s_qidx_cap = n * 2;
        s_qidx = (uint16_t *)malloc(s_qidx_cap * sizeof *s_qidx);
    }
    return s_qidx;
}

/* One draw. Vertices come from `up` (stream 0 given with the call) or the
 * bound streams, starting at vertex `base`; idx == NULL draws [0, n). */
static void draw(uint32_t xprim, uint32_t n, const uint16_t *idx,
                 const uint8_t *up, uint32_t up_stride, uint32_t base)
{
    uint32_t lo = 0, hi = n ? n - 1 : 0, i, stride, count;
    const uint8_t *verts;
    HostVs *vs = NULL;
    uint16_t *ix = NULL;
    GLenum mode;

    if (!n || !prepare_draw(&vs))
        return;
    if (idx) {
        lo = 0xFFFF; hi = 0;
        for (i = 0; i < n; i++) {
            if (idx[i] < lo) lo = idx[i];
            if (idx[i] > hi) hi = idx[i];
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    if (vs) {
        uint16_t regs;
        int r;
        verts = (const uint8_t *)vs_vertices(vs, base, lo, hi, up, up_stride, &stride, &regs);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)(hi - lo + 1) * stride), verts, GL_STREAM_DRAW);
        for (r = 0, i = 0; r < 16; r++) {
            if (regs & (1u << r)) {
                glEnableVertexAttribArray((GLuint)r);
                glVertexAttribPointer((GLuint)r, 4, GL_FLOAT, GL_FALSE, (GLsizei)stride, (const void *)(uintptr_t)(16 * i));
                i++;
            } else {
                glDisableVertexAttribArray((GLuint)r);
                glVertexAttrib4f((GLuint)r, 0, 0, 0, 1);
            }
        }
    } else {
        stride = up ? up_stride : s_stream_stride[0];
        if (up)
            verts = up;
        else if (s_stream_vb[0])
            verts = (const uint8_t *)gp(res_data(s_stream_vb[0])) + (size_t)base * stride;
        else
            return;
        if (!stride)
            stride = fvf_stride(s_vshader);
        if (!stride)
            return;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)(hi - lo + 1) * stride), verts + (size_t)lo * stride, GL_STREAM_DRAW);
        fvf_attribs(s_vshader, stride);
    }
    /* Vertices went up from `lo`: indices are rebased onto that. Quads have
     * no GL primitive and are split into triangles. */
    if (xprim == 8) {
        uint32_t q;
        ix = qidx((size_t)n / 4 * 6);
        for (q = 0; q < n / 4; q++) {
            uint16_t v[4];
            for (i = 0; i < 4; i++)
                v[i] = (uint16_t)((idx ? idx[q * 4 + i] : q * 4 + i) - lo);
            ix[q * 6] = v[0]; ix[q * 6 + 1] = v[1]; ix[q * 6 + 2] = v[2];
            ix[q * 6 + 3] = v[0]; ix[q * 6 + 4] = v[2]; ix[q * 6 + 5] = v[3];
        }
        mode = GL_TRIANGLES;
        count = n / 4 * 6;
    } else {
        mode = prim_gl(xprim, n, &count);
        if (idx) {
            ix = qidx(n);
            for (i = 0; i < n; i++)
                ix[i] = (uint16_t)(idx[i] - lo);
        }
    }
    if (!count)
        return;
    if (ix) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(count * 2), ix, GL_STREAM_DRAW);
        glDrawElements(mode, (GLsizei)count, GL_UNSIGNED_SHORT, NULL);
    } else {
        glDrawArrays(mode, 0, (GLsizei)count);
    }
    s_st.draws++;
    s_st.prims += count;
}

/* -- Present ----------------------------------------------------------------- */

static void dump_bmp(HostRt *r)
{
    static const char *prefix = (const char *)-1;
    static int every, seq;
    uint32_t W = r->w, H = r->h, x, y, sx, sy, S = s_scale;
    uint8_t *px;
    char path[512];
    FILE *f;

    if (prefix == (const char *)-1) {
        const char *e = getenv("RECOMP_D3D9_DUMP_EVERY");
        prefix = getenv("RECOMP_D3D9_DUMP");
        every = e ? atoi(e) : 600;
    }
    if (!prefix || !every || s_frame % every != 0)
        return;
    px = (uint8_t *)malloc((size_t)W * S * H * S * 4);
    if (!px)
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo_read);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
    glReadPixels(0, 0, (GLsizei)(W * S), (GLsizei)(H * S), GL_RGBA, GL_UNSIGNED_BYTE, px);
    {
        uint32_t row = W * 3, pad = (4 - (row & 3)) & 3, filesz = 54 + (row + pad) * H;
        uint8_t hdr[54] = { 'B', 'M' };
        memcpy(hdr + 2, &filesz, 4);
        hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &W, 4);
        memcpy(hdr + 22, &H, 4);
        hdr[26] = 1; hdr[28] = 24;
        snprintf(path, sizeof path, "%s%05d.bmp", prefix, seq++);
        if ((f = fopen(path, "wb")) != NULL) {
            fwrite(hdr, 1, sizeof hdr, f);
            /* Rows are top-down in the target; a BMP is bottom-up. */
            for (y = H; y-- > 0; ) {
                for (x = 0; x < W; x++) {
                    uint32_t acc[3] = { 0, 0, 0 }, n = S * S;
                    uint8_t bgr[3];
                    for (sy = 0; sy < S; sy++)
                        for (sx = 0; sx < S; sx++) {
                            const uint8_t *p = px + ((size_t)(y * S + sy) * W * S + x * S + sx) * 4;
                            acc[0] += p[2]; acc[1] += p[1]; acc[2] += p[0];
                        }
                    bgr[0] = (uint8_t)(acc[0] / n); bgr[1] = (uint8_t)(acc[1] / n); bgr[2] = (uint8_t)(acc[2] / n);
                    fwrite(bgr, 1, 3, f);
                }
                fwrite("\0\0\0", 1, pad, f);
            }
            fclose(f);
            fprintf(stderr, "[GL] frame dump %s\n", path);
        }
    }
    free(px);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
}

static void present(uint32_t data)
{
    HostRt *r = data ? rt_find(data) : NULL;
    int ww, wh;

    if (!r)
        r = s_cur_rt;
    rt_sync(r, 1);
    s_frame++;
    {
        static int stats = -1;
        static double t0;
        static uint32_t f0;
        double now = now_ms();
        if (stats < 0)
            stats = getenv("RECOMP_D3D9_STATS") && strcmp(getenv("RECOMP_D3D9_STATS"), "0") != 0;
        if (stats && now - t0 >= 1000) {
            if (t0)
                fprintf(stderr, "[GL] %.1f presents/s\n", (s_frame - f0) * 1000.0 / (now - t0));
            t0 = now;
            f0 = s_frame;
        }
    }
#ifdef __EMSCRIPTEN__
    {
        extern void web_cheats_run(void);
        web_cheats_run();
    }
#endif
    if (!r || !r->tex)
        return;
    dump_bmp(r);
    win_size(&ww, &wh);
    {
        static int said;
        if (!said++)
            fprintf(stderr, "[GL] presenting into %dx%d\n", ww, wh);
    }
    if (ww > 0 && wh > 0) {
        int aw = game_widescreen() ? r->w * 4 / 3 : r->w;   /* anamorphic 16:9 */
        int fw = ww, fh = (int)((int64_t)ww * r->h / aw), fx, fy;
        if (fh > wh) { fh = wh; fw = (int)((int64_t)wh * aw / r->h); }
        fx = (ww - fw) / 2;
        fy = (wh - fh) / 2;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport(0, 0, ww, wh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo_read);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->tex, 0);
        /* Target row 0 is the top; the window's is the bottom: flip. */
        glBlitFramebuffer(0, 0, (GLint)(r->w * s_scale), (GLint)(r->h * s_scale),
                          fx, wh - fy, fx + fw, wh - fy - fh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        s_st.rts = (unsigned)s_nrt; s_st.rts_max = MAX_RT;
        s_st.texs = (unsigned)s_ntex; s_st.texs_max = MAX_TEX;
        s_st.vss = (unsigned)s_nvs; s_st.vss_max = MAX_VS;
        s_st.vdecls = (unsigned)s_nprog; s_st.vdecls_max = MAX_PROG;
        s_st.scale = s_scale; s_st.sc_w = (unsigned)ww; s_st.sc_h = (unsigned)wh; s_st.frame = s_frame;
        s_st.hd_textures = s_hd_hits;
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
        {
            /* The pointer over the game's menus, in guest 640x480. */
            int gx = -1, gy = -1;
#if defined(__EMSCRIPTEN__)
            extern void web_pointer(int *gx, int *gy);
            web_pointer(&gx, &gy);          /* the page's canvas is the picture */
#else
            HWND hwnd = (HWND)xbox_FramebufferWindowHwnd();
            POINT pt;
            if (GetForegroundWindow() == hwnd && GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)
                && pt.x >= fx && pt.x < fx + fw && pt.y >= fy && pt.y < fy + fh) {
                gx = (pt.x - fx) * 640 / fw;
                gy = (pt.y - fy) * 480 / fh;
            }
#endif
            if (gx >= 0 && game_widescreen())   /* the menus are squeezed to the middle 3/4 */
                gx = 320 + (gx - 320) * 4 / 3;
            /* As d3d9_hle.c: no hover while the King is in play. */
            {
                int play = game_in_play();
                menu_mouse_update(play ? -1 : gx, gy);
                mkb_set_menu_cursor(!play);
            }
        }
#endif
        swap_window();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    memset(&s_st, 0, sizeof s_st);
}

/* -- Handlers ---------------------------------------------------------------- */

static void hle_Swap(void)                  /* Swap(Flags) */
{
    uint32_t flags = ARG(0);
    uint32_t bb = MEM32(xb_device() + 0x1A14), data = bb ? res_data(bb) : 0;

    sub_00200110_gen();
    if (flags == 0 || (flags & 4)) {
        /* At most one present per 60 Hz frame; see d3d9_hle.c. */
        static double last;
        double now = now_ms();
        if (last) {
            double due = last + 1000.0 / 60.0;
            while (now < due) {
                sleep_ms(due - now - 1);
                now = now_ms();
            }
        }
        last = now;
        lock();
        if (dev_ready())
            present(data);
        unlock();
    }
}

/* Clear(Count, pRects, Flags, Color, Z, Stencil) */
static void hle_Clear(void)
{
    uint32_t count = ARG(0), rects = ARG(1), flags = ARG(2), color = ARG(3);
    float z = f32(ARG(4));
    uint32_t stencil = ARG(5), i;
    GLbitfield f = 0;
    Rect r[16];

    lock();
    if (dev_ready()) {
        bind_targets();
        if ((flags & 0xF0) && s_cur_rt) f |= GL_COLOR_BUFFER_BIT;
        if (s_cur_ds && (flags & 1)) f |= GL_DEPTH_BUFFER_BIT;
        if (s_cur_ds && (flags & 2)) f |= GL_STENCIL_BUFFER_BIT;
        if (count > 16) count = 16;
        for (i = 0; i < count && rects; i++) {
            r[i].x1 = SMEM32(rects + 16 * i); r[i].y1 = SMEM32(rects + 16 * i + 4);
            r[i].x2 = SMEM32(rects + 16 * i + 8); r[i].y2 = SMEM32(rects + 16 * i + 12);
        }
        if (f) {
            float c[4];
            argb_vec(color, c);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            glStencilMask(0xFF);
            glClearColor(c[0], c[1], c[2], c[3]);
            glClearDepthf(z);
            glClearStencil((GLint)stencil);
            if (rects && count) {
                glEnable(GL_SCISSOR_TEST);
                for (i = 0; i < count; i++) {
                    glScissor(r[i].x1 * (GLint)s_scale, r[i].y1 * (GLint)s_scale,
                              (r[i].x2 - r[i].x1) * (GLsizei)s_scale, (r[i].y2 - r[i].y1) * (GLsizei)s_scale);
                    glClear(f);
                }
                glDisable(GL_SCISSOR_TEST);
            } else {
                glDisable(GL_SCISSOR_TEST);
                glClear(f);
            }
        }
        if ((f & GL_COLOR_BUFFER_BIT) && s_cur_rt)
            rt_fill(s_cur_rt, r, rects ? count : 0, color);
    }
    unlock();
    sub_001F91D0_gen();
}

static void hle_SetViewport(void)           /* SetViewport(pViewport) */
{
    uint32_t p = ARG(0);
    if (p) {
        s_vp.x = MEM32(p); s_vp.y = MEM32(p + 4);
        s_vp.w = MEM32(p + 8); s_vp.h = MEM32(p + 12);
        s_vp.minz = f32(MEM32(p + 16)); s_vp.maxz = f32(MEM32(p + 20));
    }
    sub_001F1590_gen();
}

static void hle_SetTransform(void)          /* SetTransform(State, pMatrix) */
{
    uint32_t st = ARG(0), p = ARG(1);
    if (st < 10 && p)
        memcpy(&s_xf[st], gp(p), sizeof(Mat));
    sub_001EFB70_gen();
}

static void hle_SetLight(void)              /* SetLight(Index, pLight) */
{
    uint32_t i = ARG(0), p = ARG(1);
    if (i < 8 && p)
        memcpy(&s_light[i], gp(p), sizeof(Light));
    sub_001F17C0_gen();
}

static void hle_LightEnable(void)           /* LightEnable(Index, Enable) */
{
    uint32_t i = ARG(0);
    if (i < 8)
        s_light_on = (s_light_on & ~(1u << i)) | ((uint32_t)(ARG(1) != 0) << i);
    sub_001F1A80_gen();
}

static void hle_SetMaterial(void)           /* SetMaterial(pMaterial) */
{
    if (ARG(0))
        memcpy(&s_material, gp(ARG(0)), sizeof s_material);
    sub_001EFD60_gen();
}

static void hle_SetTexture(void)            /* SetTexture(Stage, pTexture) */
{
    if (ARG(0) < 4)
        s_stage_tex[ARG(0)] = ARG(1);
    sub_001F1C10_gen();
}

static void hle_SetPalette(void)            /* SetPalette(Stage, pPalette) */
{
    if (ARG(0) < 4)
        s_stage_pal[ARG(0)] = ARG(1);
    sub_001F1DF0_gen();
}

static void hle_SetStreamSource(void)       /* SetStreamSource(Stream, pVB, Stride) */
{
    if (ARG(0) < 16) {
        s_stream_vb[ARG(0)] = ARG(1);
        s_stream_stride[ARG(0)] = ARG(2);
    }
    sub_001F3AA0_gen();
}

static void hle_SetIndices(void)            /* SetIndices(pIB, BaseVertexIndex) */
{
    s_ib = ARG(0);
    s_base_vertex = ARG(1);
    sub_001F1E80_gen();
}

static void hle_SetVertexShader(void)       /* SetVertexShader(Handle) */
{
    s_vshader = ARG(0);
    sub_001F4000_gen();
}

static void hle_SetPixelShader(void)        /* SetPixelShader(Handle) */
{
    s_pshader = ARG(0);
    sub_00200AF0_gen();
}

static void hle_DrawVerticesUP(void)
{
    lock();
    if (ARG(2))
        draw(ARG(0), ARG(1), NULL, (const uint8_t *)gp(ARG(2)), ARG(3), 0);
    unlock();
    stdcall_ret(4, 0);
}

static void hle_DrawIndexedVerticesUP(void)
{
    lock();
    if (ARG(2) && ARG(3))
        draw(ARG(0), ARG(1), (const uint16_t *)gp(ARG(2)), (const uint8_t *)gp(ARG(3)), ARG(4), 0);
    unlock();
    stdcall_ret(5, 0);
}

static void hle_DrawVertices(void)
{
    lock();
    draw(ARG(0), ARG(2), NULL, NULL, 0, ARG(1));
    unlock();
    stdcall_ret(3, 0);
}

static void hle_DrawIndexedVertices(void)
{
    lock();
    if (ARG(2))
        draw(ARG(0), ARG(1), (const uint16_t *)gp(ARG(2)), NULL, 0, s_base_vertex);
    unlock();
    stdcall_ret(3, 0);
}

static void hle_CreateVertexShader(void)
{
    uint32_t decl = ARG(0), func = ARG(1), ph = ARG(2);
    HostVs *h;
    int i;

    sub_001F34C0_gen();
    if (!ph || !MEM32(ph) || !func)
        return;
    lock();
    h = vs_find(MEM32(ph));
    if (!h && s_nvs < MAX_VS)
        h = &s_vs[s_nvs++];
    if (h) {
        free(h->glsl);
        memset(h, 0, sizeof *h);
        h->handle = MEM32(ph);
        for (i = 0; i < 96; i++) {
            h->decl[i] = decl ? MEM32(decl + 4 * i) : 0xFFFFFFFFu;
            if (h->decl[i] == 0xFFFFFFFFu)
                break;
        }
        if (i == 96) h->decl[95] = 0xFFFFFFFFu;
        h->nmc = (int)(MEM32(func) >> 16);
        if (h->nmc > NV2A_VS_MAX_INSTRUCTIONS) h->nmc = NV2A_VS_MAX_INSTRUCTIONS;
        memcpy(h->mc, gp(func + 4), (size_t)h->nmc * 16);
        /* ponytail: a recreated handle keeps any program cached on the old
         * microcode; this title creates each once. */
    }
    unlock();
}

static void vsc_mirror(int32_t reg, uint32_t p, uint32_t count)
{
    uint32_t k;
    for (k = 0; k < count && reg + (int32_t)k < NV2A_VS_MAX_CONSTANTS && p; k++)
        if (reg + (int32_t)k >= 0)
            memcpy(s_vsc[reg + k], gp(p + 16 * k), 16);
}

static void hle_SetVertexShaderConstant1(void) { vsc_mirror((int32_t)g_ecx, g_edx, 1); sub_001F3620_gen(); }
static void hle_SetVertexShaderConstant4(void) { vsc_mirror((int32_t)g_ecx, g_edx, 4); sub_001F36D0_gen(); }
static void hle_SetVertexShaderConstantNotInline(void) { vsc_mirror((int32_t)g_ecx, g_edx, ARG(0) / 4); sub_001F3860_gen(); }

/* -- Init -------------------------------------------------------------------- */

void d3d9_hle_init(void)
{
    const char *e = getenv("RECOMP_SCALE");
    int i;
    lock_init();
    for (i = 0; i < 10; i++) {          /* D3D's defaults: identity */
        memset(&s_xf[i], 0, sizeof s_xf[i]);
        s_xf[i].m[0] = s_xf[i].m[5] = s_xf[i].m[10] = s_xf[i].m[15] = 1.0f;
    }
    if (e && atoi(e) >= 1 && atoi(e) <= 8)
        s_scale = (uint32_t)atoi(e);
}

/* Not replaced: forward to the lifted body. */
#define hle_Direct3D_CreateDevice sub_001FF350_gen
#define hle_SetRenderTarget sub_001F0A90_gen
#define hle_GetBackBuffer2 sub_001F0E30_gen
#define hle_GetDepthStencilSurface2 sub_001F1570_gen
#define hle_CopyRects sub_001F1220_gen
#define hle_DeleteVertexShader sub_001F3EF0_gen
#define hle_CreatePixelShader sub_00200640_gen
#define hle_SetPixelShaderConstant sub_00200CF0_gen
#define hle_DeletePixelShader sub_00200690_gen
#define hle_SetShaderConstantMode sub_001F3D90_gen
#define hle_EnableOverlay sub_00201180_gen
#define hle_UpdateOverlay sub_00201000_gen
#define hle_GetOverlayUpdateStatus sub_00200FC0_gen
#define hle_MakeSpace sub_001EEF60_gen
#define hle_KickOff sub_001EEAD0_gen
#define hle_BlockOnFence sub_001EFF50_gen
#define hle_KickOffAndWaitForIdle sub_001EEFC0_gen
#define hle_InsertFence sub_001F0330_gen

void sub_001FF350(void) { hle_trace("Direct3D_CreateDevice", 6); hle_Direct3D_CreateDevice(); }
void sub_00200110(void) { hle_trace("Swap", 1); hle_Swap(); }
void sub_001F91D0(void) { hle_trace("Clear", 6); hle_Clear(); }
void sub_001F0A90(void) { hle_trace("SetRenderTarget", 2); hle_SetRenderTarget(); }
void sub_001F0E30(void) { hle_trace("GetBackBuffer2", 1); hle_GetBackBuffer2(); }
void sub_001F1570(void) { hle_trace("GetDepthStencilSurface2", 0); hle_GetDepthStencilSurface2(); }
void sub_001F1220(void) { hle_trace("CopyRects", 5); hle_CopyRects(); }
void sub_001F1590(void) { hle_trace("SetViewport", 1); hle_SetViewport(); }
void sub_001EFB70(void) { hle_trace("SetTransform", 2); hle_SetTransform(); }
void sub_001F17C0(void) { hle_trace("SetLight", 2); hle_SetLight(); }
void sub_001F1A80(void) { hle_trace("LightEnable", 2); hle_LightEnable(); }
void sub_001EFD60(void) { hle_trace("SetMaterial", 1); hle_SetMaterial(); }
void sub_001F1C10(void) { hle_trace("SetTexture", 2); hle_SetTexture(); }
void sub_001F1DF0(void) { hle_trace("SetPalette", 2); hle_SetPalette(); }
void sub_001F3AA0(void) { hle_trace("SetStreamSource", 3); hle_SetStreamSource(); }
void sub_001F1E80(void) { hle_trace("SetIndices", 2); hle_SetIndices(); }
void sub_001FEA80(void) { hle_trace("DrawVerticesUP", 4); hle_DrawVerticesUP(); }
void sub_001FEBF0(void) { hle_trace("DrawIndexedVerticesUP", 5); hle_DrawIndexedVerticesUP(); }
void sub_001FED30(void) { hle_trace("DrawVertices", 3); hle_DrawVertices(); }
void sub_001FEDD0(void) { hle_trace("DrawIndexedVertices", 3); hle_DrawIndexedVertices(); }
void sub_001F34C0(void) { hle_trace("CreateVertexShader", 4); hle_CreateVertexShader(); }
void sub_001F4000(void) { hle_trace("SetVertexShader", 1); hle_SetVertexShader(); }
void sub_001F3EF0(void) { hle_trace("DeleteVertexShader", 1); hle_DeleteVertexShader(); }
void sub_001F3620(void) { hle_trace("SetVertexShaderConstant1", 0); hle_SetVertexShaderConstant1(); }
void sub_001F36D0(void) { hle_trace("SetVertexShaderConstant4", 0); hle_SetVertexShaderConstant4(); }
void sub_001F3860(void) { hle_trace("SetVertexShaderConstantNotInline", 1); hle_SetVertexShaderConstantNotInline(); }
void sub_00200640(void) { hle_trace("CreatePixelShader", 2); hle_CreatePixelShader(); }
void sub_00200AF0(void) { hle_trace("SetPixelShader", 1); hle_SetPixelShader(); }
void sub_00200CF0(void) { hle_trace("SetPixelShaderConstant", 3); hle_SetPixelShaderConstant(); }
void sub_00200690(void) { hle_trace("DeletePixelShader", 1); hle_DeletePixelShader(); }
void sub_001F3D90(void) { hle_trace("SetShaderConstantMode", 1); hle_SetShaderConstantMode(); }
void sub_00201180(void) { hle_trace("EnableOverlay", 1); hle_EnableOverlay(); }
void sub_00201000(void) { hle_trace("UpdateOverlay", 5); hle_UpdateOverlay(); }
void sub_00200FC0(void) { hle_trace("GetOverlayUpdateStatus", 0); hle_GetOverlayUpdateStatus(); }
void sub_001EEF60(void) { hle_trace("MakeSpace", 0); hle_MakeSpace(); }
void sub_001EEAD0(void) { hle_trace("KickOff", 0); hle_KickOff(); }
void sub_001EFF50(void) { hle_trace("BlockOnFence", 1); hle_BlockOnFence(); }
void sub_001EEFC0(void) { hle_trace("KickOffAndWaitForIdle", 0); hle_KickOffAndWaitForIdle(); }
void sub_001F0330(void) { hle_trace("InsertFence", 0); hle_InsertFence(); }
