// Build:
// gcc ogl-min-line-perf-pageflip.c -o ogl-min-line-perf-pageflip \
//         $(pkg-config --cflags --libs egl glesv2 gbm libdrm)

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <sys/select.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

typedef struct {
    int drm_fd;
    int screen_width;
    int screen_height;

    drmModeModeInfo mode;
    drmModeConnector *connector;
    drmModeEncoder   *encoder;

    struct gbm_device  *gbm_device;
    struct gbm_surface *gbm_surface;
    struct gbm_bo *previous_bo;

    uint32_t crtc_id;
    uint32_t connector_id;
    int did_modeset;

    volatile int flip_done;

    EGLDisplay egl_display;
    EGLConfig  egl_config;
    EGLContext egl_context;
    EGLSurface egl_surface;

    GLuint shader_program;
    GLuint vertex_array_object;
    GLuint vertex_buffer_object;

} GraphicsContext;

static inline double get_seconds()
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC,&ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static GLuint create_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, 0);
    glCompileShader(shader);
    return shader;
}

static GLuint create_program(const char *vs, const char *fs)
{
    GLuint program = glCreateProgram();
    glAttachShader(program, create_shader(GL_VERTEX_SHADER, vs));
    glAttachShader(program, create_shader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(program);
    return program;
}

/* ---------- Pageflip event ---------- */

static void page_flip_handler(int fd, unsigned int frame,
                              unsigned int sec, unsigned int usec,
                              void *data)
{
    (void)fd; (void)frame; (void)sec; (void)usec;
    ((GraphicsContext*)data)->flip_done = 1;
}

static void wait_for_flip(GraphicsContext *gfx)
{
    drmEventContext ev = (drmEventContext){0};
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = page_flip_handler;

    while (!gfx->flip_done) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(gfx->drm_fd, &fds);
        select(gfx->drm_fd + 1, &fds, NULL, NULL, NULL);
        drmHandleEvent(gfx->drm_fd, &ev);
    }
    gfx->flip_done = 0;
}

/* ---------- FB caching per GBM BO ---------- */

typedef struct {
    int drm_fd;
    uint32_t fb_id;
} FbData;

static void fbdata_destroy(struct gbm_bo *bo, void *data)
{
    (void)bo;
    FbData *d = (FbData*)data;
    if (d) {
        if (d->fb_id) drmModeRmFB(d->drm_fd, d->fb_id);
        free(d);
    }
}

static uint32_t get_or_create_fb(GraphicsContext *gfx, struct gbm_bo *bo)
{
    FbData *d = (FbData*)gbm_bo_get_user_data(bo);
    if (d) return d->fb_id;

    d = (FbData*)calloc(1, sizeof(*d));
    d->drm_fd = gfx->drm_fd;

    uint32_t handles[4] = { gbm_bo_get_handle(bo).u32, 0, 0, 0 };
    uint32_t strides[4] = { gbm_bo_get_stride(bo), 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };

    int ret = drmModeAddFB2(gfx->drm_fd, gfx->screen_width, gfx->screen_height,
                            DRM_FORMAT_XRGB8888, handles, strides, offsets,
                            &d->fb_id, 0);
    if (ret) {
        perror("drmModeAddFB2");
        exit(1);
    }

    gbm_bo_set_user_data(bo, d, fbdata_destroy);
    return d->fb_id;
}

static GraphicsContext graphics_init(void)
{
    GraphicsContext gfx = {0};

    gfx.drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);

    drmModeRes *resources = drmModeGetResources(gfx.drm_fd);
    gfx.connector = drmModeGetConnector(gfx.drm_fd, resources->connectors[0]);
    gfx.mode = gfx.connector->modes[0];
    gfx.encoder = drmModeGetEncoder(gfx.drm_fd, gfx.connector->encoder_id);

    gfx.screen_width  = gfx.mode.hdisplay;
    gfx.screen_height = gfx.mode.vdisplay;

    gfx.crtc_id = gfx.encoder->crtc_id;
    gfx.connector_id = gfx.connector->connector_id;

    gfx.gbm_device = gbm_create_device(gfx.drm_fd);

    gfx.egl_display = eglGetDisplay((EGLNativeDisplayType)gfx.gbm_device);
    eglInitialize(gfx.egl_display, 0, 0);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint config_attributes[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    EGLint num_configs;
    eglChooseConfig(gfx.egl_display, config_attributes, &gfx.egl_config, 1, &num_configs);

    EGLint format;
    eglGetConfigAttrib(gfx.egl_display, gfx.egl_config, EGL_NATIVE_VISUAL_ID, &format);

    gfx.gbm_surface = gbm_surface_create(
        gfx.gbm_device,
        gfx.screen_width,
        gfx.screen_height,
        format,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING
    );

    gfx.egl_context = eglCreateContext(
        gfx.egl_display,
        gfx.egl_config,
        EGL_NO_CONTEXT,
        (EGLint[]){ EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE }
    );

    gfx.egl_surface = eglCreateWindowSurface(
        gfx.egl_display,
        gfx.egl_config,
        (EGLNativeWindowType)gfx.gbm_surface,
        0
    );

    eglMakeCurrent(gfx.egl_display,
                   gfx.egl_surface,
                   gfx.egl_surface,
                   gfx.egl_context);

    const char *vertex_shader_source =
        "#version 300 es\n"
        "layout(location=0) in vec2 position;"
        "layout(location=1) in vec4 color;"
        "out vec4 vColor;"
        "void main(){"
        "vColor = color;"
        "gl_Position = vec4(position,0.0,1.0);"
        "}";

    const char *fragment_shader_source =
        "#version 300 es\n"
        "precision mediump float;"
        "in vec4 vColor;"
        "out vec4 fragColor;"
        "void main(){"
        "fragColor = vColor;"
        "}";

    gfx.shader_program = create_program(vertex_shader_source,
                                        fragment_shader_source);

    glUseProgram(gfx.shader_program);

    glGenVertexArrays(1, &gfx.vertex_array_object);
    glBindVertexArray(gfx.vertex_array_object);

    glGenBuffers(1, &gfx.vertex_buffer_object);
    glBindBuffer(GL_ARRAY_BUFFER, gfx.vertex_buffer_object);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), 0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glViewport(0, 0, gfx.screen_width, gfx.screen_height);

    eglSwapInterval(gfx.egl_display, 0);

    return gfx;
}

static void graphics_present(GraphicsContext *gfx)
{
    // after eglSwapBuffers(): lock next front buffer
    double a = get_seconds();
    struct gbm_bo *new_bo = gbm_surface_lock_front_buffer(gfx->gbm_surface);
    double b = get_seconds();

    uint32_t new_fb = get_or_create_fb(gfx, new_bo);
    double c = get_seconds();
    if (!gfx->did_modeset) {
        drmModeSetCrtc(gfx->drm_fd, gfx->crtc_id, new_fb, 0, 0,
                       &gfx->connector_id, 1, &gfx->mode);
        gfx->did_modeset = 1;
    } else {
        gfx->flip_done = 0;
        drmModePageFlip(gfx->drm_fd, gfx->crtc_id, new_fb,
                        DRM_MODE_PAGE_FLIP_EVENT, gfx);
        wait_for_flip(gfx);
    }
    double d = get_seconds();

    // now safe: release previous BO (FB is freed when BO is destroyed via user_data callback)
    if (gfx->previous_bo)
        gbm_surface_release_buffer(gfx->gbm_surface, gfx->previous_bo);

    gfx->previous_bo = new_bo;
    printf("present breakdown: lock=%.3fms flipwait=%.3fms\n",(b-a)*1000.0, (d-c)*1000.0);
}

int main(void)
{
    GraphicsContext gfx = graphics_init();

    int line_count = 100000;

    // each line -> 2 triangles -> 6 vertices
    int vertices_per_line = 6;
    int total_vertices = line_count * vertices_per_line;
    int floats_per_vertex = 6;

    size_t vertex_buffer_size = (size_t)total_vertices * (size_t)floats_per_vertex * sizeof(float);
    float *vertex_data = malloc(vertex_buffer_size);

    glBufferData(GL_ARRAY_BUFFER, vertex_buffer_size, 0, GL_STREAM_DRAW);

    srandom(time(0));

    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(gfx.egl_display, gfx.egl_surface);
    graphics_present(&gfx);

    // line width in pixels
    float line_width_px = 2.0f;

    // pixel -> NDC scale (anisotropic to match screen aspect)
    float sx = 2.0f / (float)gfx.screen_width;
    float sy = 2.0f / (float)gfx.screen_height;

    for (;;)
    {
        double t0 = get_seconds();

        for (int i = 0; i < line_count; i++)
        {
            int x0 = random() % gfx.screen_width;
            int y0 = random() % gfx.screen_height;
            int x1 = random() % gfx.screen_width;
            int y1 = random() % gfx.screen_height;

            // endpoints in NDC
            float p0x = 2.0f * x0 / (gfx.screen_width  - 1) - 1.0f;
            float p0y = 1.0f - 2.0f * y0 / (gfx.screen_height - 1);
            float p1x = 2.0f * x1 / (gfx.screen_width  - 1) - 1.0f;
            float p1y = 1.0f - 2.0f * y1 / (gfx.screen_height - 1);

            // direction in NDC
            float dx = p1x - p0x;
            float dy = p1y - p0y;

            // normalize (avoid div0)
            float len2 = dx*dx + dy*dy;
            float invlen = (len2 > 1e-12f) ? (1.0f / sqrtf(len2)) : 0.0f;
            dx *= invlen;
            dy *= invlen;

            // perpendicular, scaled to ~line_width_px (account for NDC scaling)
            // n = (-dy, dx) then scale with pixel->NDC and half-width
            float nx = -dy * sx * (line_width_px * 0.5f);
            float ny =  dx * sy * (line_width_px * 0.5f);

            // quad corners
            float Ax = p0x + nx, Ay = p0y + ny;
            float Bx = p0x - nx, By = p0y - ny;
            float Cx = p1x + nx, Cy = p1y + ny;
            float Dx = p1x - nx, Dy = p1y - ny;

            float r = (random() % 256) / 255.0f;
            float g = (random() % 256) / 255.0f;
            float b = (random() % 256) / 255.0f;

            int base = i * vertices_per_line * floats_per_vertex;

            // tri 1: A, B, C
            vertex_data[base +  0] = Ax; vertex_data[base +  1] = Ay;
            vertex_data[base +  2] = r;  vertex_data[base +  3] = g;
            vertex_data[base +  4] = b;  vertex_data[base +  5] = 1.0f;

            vertex_data[base +  6] = Bx; vertex_data[base +  7] = By;
            vertex_data[base +  8] = r;  vertex_data[base +  9] = g;
            vertex_data[base + 10] = b;  vertex_data[base + 11] = 1.0f;

            vertex_data[base + 12] = Cx; vertex_data[base + 13] = Cy;
            vertex_data[base + 14] = r;  vertex_data[base + 15] = g;
            vertex_data[base + 16] = b;  vertex_data[base + 17] = 1.0f;

            // tri 2: C, B, D
            vertex_data[base + 18] = Cx; vertex_data[base + 19] = Cy;
            vertex_data[base + 20] = r;  vertex_data[base + 21] = g;
            vertex_data[base + 22] = b;  vertex_data[base + 23] = 1.0f;

            vertex_data[base + 24] = Bx; vertex_data[base + 25] = By;
            vertex_data[base + 26] = r;  vertex_data[base + 27] = g;
            vertex_data[base + 28] = b;  vertex_data[base + 29] = 1.0f;

            vertex_data[base + 30] = Dx; vertex_data[base + 31] = Dy;
            vertex_data[base + 32] = r;  vertex_data[base + 33] = g;
            vertex_data[base + 34] = b;  vertex_data[base + 35] = 1.0f;
        }

        double t1 = get_seconds();

        glClear(GL_COLOR_BUFFER_BIT);
        glBufferData(GL_ARRAY_BUFFER, vertex_buffer_size, NULL, GL_STREAM_DRAW); // orphan
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertex_buffer_size, vertex_data);
        glDrawArrays(GL_TRIANGLES, 0, total_vertices);

        eglSwapBuffers(gfx.egl_display, gfx.egl_surface);
        graphics_present(&gfx);

        double t2 = get_seconds();

        printf("Create Vert: %.6f sec \n", (t1 - t0));
        printf("Draw Tris  : %.6f sec \n", (t2 - t1));
        printf("Total Time : %.6f sec \n \n", (t2 - t0));

        sleep(1);
    }

    return 0;
}