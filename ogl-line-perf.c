// ogl-lines-es3.c
// DRM/KMS + GBM + EGL + OpenGL ES 3.0: draw many random colored lines, time it.
//
// Build (typisch):
//   cc ogl-lines-es3.c -o ogl-lines-es3 \
//      -ldrm -lgbm -lEGL -lGLESv2
//
// Run as root or with DRM permissions (no X/Wayland).

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

// ------------------------------------------------------------
// Timing + RNG like your KMS test
// ------------------------------------------------------------
static inline double get_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static inline int random_int(int min, int max) {
    // use random() to match your color generation (seeded by srandom())
    return min + (int)(random() % (unsigned)(max - min + 1));
}

// ------------------------------------------------------------
// Minimal shader helper (with basic error output)
// ------------------------------------------------------------
static GLuint mkshader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        GLsizei n = 0;
        glGetShaderInfoLog(s, (GLsizei)sizeof(log), &n, log);
        fprintf(stderr, "shader compile failed: %.*s\n", (int)n, log);
        _exit(1);
    }
    return s;
}

static GLuint mkprogram(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, mkshader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, mkshader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        GLsizei n = 0;
        glGetProgramInfoLog(p, (GLsizei)sizeof(log), &n, log);
        fprintf(stderr, "program link failed: %.*s\n", (int)n, log);
        _exit(1);
    }
    return p;
}

// ------------------------------------------------------------
// KMS/GBM/EGL bootstrap (minimal, but chooses first connected connector)
// ------------------------------------------------------------
typedef struct {
    int fd;
    drmModeModeInfo mode;
    uint32_t crtc_id;
    uint32_t conn_id;

    struct gbm_device *gbm;
    struct gbm_surface *gs;

    EGLDisplay dpy;
    EGLConfig cfg;
    EGLContext ctx;
    EGLSurface surf;
} gfx_t;

static gfx_t init_gfx(void) {
    gfx_t g = {0};

    g.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (g.fd < 0) { perror("open(/dev/dri/card0)"); _exit(1); }

    drmModeRes *res = drmModeGetResources(g.fd);
    if (!res) { perror("drmModeGetResources"); _exit(1); }

    drmModeConnector *conn = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(g.fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            conn = c;
            break;
        }
        drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "no connected connector\n"); _exit(1); }

    g.mode = conn->modes[0];
    g.conn_id = conn->connector_id;

    drmModeEncoder *enc = drmModeGetEncoder(g.fd, conn->encoder_id);
    if (!enc) { fprintf(stderr, "no encoder\n"); _exit(1); }
    g.crtc_id = enc->crtc_id;

    drmModeFreeEncoder(enc);
    // keep conn around only for ids/mode
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    g.gbm = gbm_create_device(g.fd);
    if (!g.gbm) { fprintf(stderr, "gbm_create_device failed\n"); _exit(1); }

    PFNEGLGETPLATFORMDISPLAYPROC eglGetPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYPROC)eglGetProcAddress("eglGetPlatformDisplay");
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    g.dpy = EGL_NO_DISPLAY;
    if (eglGetPlatformDisplay)
        g.dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, g.gbm, NULL);
    else
        g.dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, g.gbm, NULL);

    if (g.dpy == EGL_NO_DISPLAY) { fprintf(stderr, "EGL display failed\n"); _exit(1); }

    if (!eglInitialize(g.dpy, 0, 0)) { fprintf(stderr, "eglInitialize failed\n"); _exit(1); }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    EGLint n = 0;
    if (!eglChooseConfig(g.dpy, cfg_attribs, &g.cfg, 1, &n) || n != 1) {
        fprintf(stderr, "eglChooseConfig failed\n"); _exit(1);
    }

    EGLint fmt = 0;
    eglGetConfigAttrib(g.dpy, g.cfg, EGL_NATIVE_VISUAL_ID, &fmt);

    g.gs = gbm_surface_create(
        g.gbm,
        g.mode.hdisplay, g.mode.vdisplay,
        (uint32_t)fmt,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING
    );
    if (!g.gs) { fprintf(stderr, "gbm_surface_create failed\n"); _exit(1); }

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g.ctx = eglCreateContext(g.dpy, g.cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (g.ctx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed\n"); _exit(1); }

    g.surf = eglCreateWindowSurface(g.dpy, g.cfg, (EGLNativeWindowType)g.gs, NULL);
    if (g.surf == EGL_NO_SURFACE) { fprintf(stderr, "eglCreateWindowSurface failed\n"); _exit(1); }

    if (!eglMakeCurrent(g.dpy, g.surf, g.surf, g.ctx)) {
        fprintf(stderr, "eglMakeCurrent failed\n"); _exit(1);
    }

    return g;
}

// Create FB for a gbm_bo (single-plane, 32bpp). Minimal like your demo.
static uint32_t bo_to_fb(int fd, struct gbm_bo *bo, int w, int h) {
    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t stride = gbm_bo_get_stride(bo);

    uint32_t fb = 0;
    // Note: for some drivers/formats you may need drmModeAddFB2 with explicit format.
    if (drmModeAddFB(fd, w, h, 24, 32, stride, handle, &fb) != 0) {
        perror("drmModeAddFB");
        _exit(1);
    }
    return fb;
}

int main(void) {
    const int line_count = 100000;

    gfx_t g = init_gfx();

    const int W = g.mode.hdisplay;
    const int H = g.mode.vdisplay;

    // Disable vsync for "raw throughput" timing (optional).
    // Set to 1 if you WANT vblank-limited swap (typically 60/120 Hz).
    eglSwapInterval(g.dpy, 0);

    // Seed RNG
    srandom((unsigned)time(NULL));

    // ------------------------------------------------------------
    // GLES program: positions in NDC already, plus per-vertex color
    // ------------------------------------------------------------
    const char *vs =
        "#version 300 es\n"
        "layout(location=0) in vec2 pos;\n"
        "layout(location=1) in vec4 col;\n"
        "out vec4 vcol;\n"
        "void main(){ vcol = col; gl_Position = vec4(pos, 0.0, 1.0); }\n";

    const char *fs =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec4 vcol;\n"
        "out vec4 frag;\n"
        "void main(){ frag = vcol; }\n";

    GLuint prog = mkprogram(vs, fs);
    glUseProgram(prog);

    // Vertex format: {x,y,r,g,b,a} floats
    const int verts_per_line = 2;
    const int floats_per_vert = 6;
    const int total_verts = line_count * verts_per_line;
    const size_t vbytes = (size_t)total_verts * (size_t)floats_per_vert * sizeof(float);

    float *vdata = (float*)malloc(vbytes);
    if (!vdata) { fprintf(stderr, "malloc vdata failed\n"); _exit(1); }

    GLuint vao=0, vbo=0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vbytes, NULL, GL_STREAM_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, (GLsizei)(floats_per_vert*sizeof(float)), (void*)0);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, (GLsizei)(floats_per_vert*sizeof(float)), (void*)(2*sizeof(float)));

    glViewport(0, 0, W, H);

    // ------------------------------------------------------------
    // KMS: set an initial FB once, then do page-flips by re-setting CRTC (minimal)
    // For a real loop you’d typically use drmModePageFlip + event loop.
    // ------------------------------------------------------------
    // First swap to ensure a front buffer exists
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(g.dpy, g.surf);

    struct gbm_bo *bo = gbm_surface_lock_front_buffer(g.gs);
    uint32_t fb = bo_to_fb(g.fd, bo, W, H);
    if (drmModeSetCrtc(g.fd, g.crtc_id, fb, 0, 0, &g.conn_id, 1, &g.mode) != 0) {
        perror("drmModeSetCrtc");
        _exit(1);
    }

    // We keep a very small “previous buffer” handling (simple, not bullet-proof).
    struct gbm_bo *prev_bo = NULL;
    uint32_t prev_fb = 0;

    for (;;) {
        // ------------------------
        // CPU: generate vertex data
        // ------------------------
        double t0 = get_seconds();
        for (int i = 0; i < line_count; i++) {
            int x0 = random_int(0, W - 1);
            int y0 = random_int(0, H - 1);
            int x1 = random_int(0, W - 1);
            int y1 = random_int(0, H - 1);
            uint32_t c = 0xFF000000u | ((uint32_t)random() & 0x00FFFFFFu);

            // Convert to NDC [-1..1], with y flipped (top-left -> NDC)
            float fx0 = (2.0f * (float)x0 / (float)(W - 1)) - 1.0f;
            float fy0 = 1.0f - (2.0f * (float)y0 / (float)(H - 1));
            float fx1 = (2.0f * (float)x1 / (float)(W - 1)) - 1.0f;
            float fy1 = 1.0f - (2.0f * (float)y1 / (float)(H - 1));

            // ARGB -> normalized floats (premultiply: NO, like your dumb buffer)
            float a = (float)((c >> 24) & 0xFF) / 255.0f;
            float r = (float)((c >> 16) & 0xFF) / 255.0f;
            float gcol = (float)((c >>  8) & 0xFF) / 255.0f;
            float b = (float)((c >>  0) & 0xFF) / 255.0f;

            int base = i * 2 * floats_per_vert;

            // v0
            vdata[base + 0] = fx0;
            vdata[base + 1] = fy0;
            vdata[base + 2] = r;
            vdata[base + 3] = gcol;
            vdata[base + 4] = b;
            vdata[base + 5] = a;

            // v1
            vdata[base + 6]  = fx1;
            vdata[base + 7]  = fy1;
            vdata[base + 8]  = r;
            vdata[base + 9]  = gcol;
            vdata[base + 10] = b;
            vdata[base + 11] = a;
        }
        double t1 = get_seconds();

        // ------------------------
        // GPU: upload + draw + swap
        // ------------------------
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)vbytes, vdata);

        glDrawArrays(GL_LINES, 0, total_verts);

        // If you want "GPU time" excluding swap blocking, you can glFinish() here
        // (but it changes what you measure).
        // glFinish();

        double t2 = get_seconds();
        eglSwapBuffers(g.dpy, g.surf);
        double t3 = get_seconds();

        // ------------------------
        // Present via KMS: lock new front buffer, set it
        // ------------------------
        struct gbm_bo *new_bo = gbm_surface_lock_front_buffer(g.gs);
        uint32_t new_fb = bo_to_fb(g.fd, new_bo, W, H);

        if (drmModeSetCrtc(g.fd, g.crtc_id, new_fb, 0, 0, &g.conn_id, 1, &g.mode) != 0) {
            perror("drmModeSetCrtc");
            _exit(1);
        }

        // Cleanup previous
        if (prev_fb) drmModeRmFB(g.fd, prev_fb);
        if (prev_bo) gbm_surface_release_buffer(g.gs, prev_bo);

        prev_bo = new_bo;
        prev_fb = new_fb;

        // Print timings
        printf("rnd+pack: %.6f sec | upload+draw: %.6f sec | swap: %.6f sec\n",
               (t1 - t0), (t2 - t1), (t3 - t2));
        printf("Full-Time: %.6f sec \n",(t3-t0));
        // Optional: slow it down a bit like your original
        sleep(1);
    }

    return 0;
}
