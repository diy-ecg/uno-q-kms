// ogl-min-es3.c
// Minimal example: DRM/KMS + GBM + EGL + OpenGL ES 3.0 -> draw one triangle, show via KMS
//
// Data flow (no CPU pixel copies in this path):
//   GBM allocates buffers usable for BOTH rendering (EGL/GLES) and scanout (KMS).
//   GLES renders into an EGLSurface backed by a GBM surface.
//   After eglSwapBuffers(), we lock the GBM front buffer and hand it to KMS as a FB.

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <gbm.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

static GLuint mkshader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);
    return s;
}

int main() {

    // -------------------------------------------------------------------------
    // 1) DRM/KMS: Open KMS device and pick a connected connector + mode + CRTC
    // -------------------------------------------------------------------------
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    drmModeRes *res = drmModeGetResources(fd);

    drmModeConnector *conn = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
        drmModeFreeConnector(c);
    }

    drmModeModeInfo mode = conn->modes[0];
    drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
    uint32_t crtc_id = enc->crtc_id;
    uint32_t conn_id = conn->connector_id;

    // -------------------------------------------------------------------------
    // 2) GBM: Create GBM device (surface created later after EGL config selection)
    // -------------------------------------------------------------------------
    struct gbm_device *gbm = gbm_create_device(fd);

    // -------------------------------------------------------------------------
    // 3) EGL: Platform GBM display, choose EGLConfig, match GBM surface format
    // -------------------------------------------------------------------------
    PFNEGLGETPLATFORMDISPLAYPROC eglGetPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYPROC)eglGetProcAddress("eglGetPlatformDisplay");
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");

    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (eglGetPlatformDisplay)
        dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    else
        dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);

    eglInitialize(dpy, 0, 0);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_NONE
    };

    EGLConfig cfg;
    EGLint n;
    eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &n);

    // Create GBM surface with the EGLConfig's native visual format.
    EGLint fmt;
    eglGetConfigAttrib(dpy, cfg, EGL_NATIVE_VISUAL_ID, &fmt);

    struct gbm_surface *gs = gbm_surface_create(
        gbm,
        mode.hdisplay, mode.vdisplay,
        (uint32_t)fmt,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING
    );

    // Create ES3 context + EGL surface backed by GBM.
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)gs, NULL);
    eglMakeCurrent(dpy, surf, surf, ctx);

    // -------------------------------------------------------------------------
    // 4) GLES3: Shaders (GLSL ES 3.00) + draw a triangle
    // -------------------------------------------------------------------------
    const char *vs =
        "#version 300 es\n"
        "in vec2 pos;\n"
        "void main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n";

    const char *fs =
        "#version 300 es\n"
        "precision mediump float;\n"
        "out vec4 frag;\n"
        "void main(){ frag = vec4(1.0, 0.8, 0.1, 1.0); }\n";

    GLuint prog = glCreateProgram();
    glAttachShader(prog, mkshader(GL_VERTEX_SHADER, vs));
    glAttachShader(prog, mkshader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(prog);
    glUseProgram(prog);

    GLint loc = glGetAttribLocation(prog, "pos");
    GLfloat v[] = {  0.0f,  0.7f,   -0.7f, -0.7f,   0.7f, -0.7f };

    glViewport(0, 0, mode.hdisplay, mode.vdisplay);

    // Background color (RGBA in [0..1])
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnableVertexAttribArray(loc);
    glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, v);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    eglSwapBuffers(dpy, surf);

    // -------------------------------------------------------------------------
    // 5) KMS scanout: lock rendered front buffer, create FB, set CRTC
    // -------------------------------------------------------------------------
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);

    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t stride = gbm_bo_get_stride(bo);

    uint32_t fb;
    drmModeAddFB(fd, mode.hdisplay, mode.vdisplay, 24, 32, stride, handle, &fb);
    drmModeSetCrtc(fd, crtc_id, fb, 0, 0, &conn_id, 1, &mode);

    for (;;) pause();
}
