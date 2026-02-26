// minimal_es3_kms_readable.c
// Build:
// gcc ogl-min-line-perf.c -o ogl-min-line-perf 
//         $(pkg-config --cflags --libs egl glesv2 gbm libdrm)

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
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
    uint32_t previous_framebuffer;

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

    gfx.gbm_device = gbm_create_device(gfx.drm_fd);

    gfx.egl_display = eglGetDisplay((EGLNativeDisplayType)gfx.gbm_device);
    //gfx.egl_display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gfx.gbm_device, 0);
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
    struct gbm_bo *new_bo = gbm_surface_lock_front_buffer(gfx->gbm_surface);

    uint32_t new_fb;
    drmModeAddFB(gfx->drm_fd, gfx->screen_width, gfx->screen_height,
                 24, 32, gbm_bo_get_stride(new_bo),gbm_bo_get_handle(new_bo).u32,
                 &new_fb);

    drmModeSetCrtc(gfx->drm_fd, gfx->encoder->crtc_id, new_fb, 0, 0,
                   &gfx->connector->connector_id, 1, &gfx->mode);

    if (gfx->previous_framebuffer) drmModeRmFB(gfx->drm_fd, gfx->previous_framebuffer);
    if (gfx->previous_bo) gbm_surface_release_buffer(gfx->gbm_surface, gfx->previous_bo);

    gfx->previous_bo = new_bo;
    gfx->previous_framebuffer = new_fb;
}

int main(void)
{
    GraphicsContext gfx = graphics_init();

    int line_count = 100000;
    int vertices_per_line = 2;
    int total_vertices = line_count * vertices_per_line;
    int floats_per_vertex = 6;

    size_t vertex_buffer_size = (size_t)total_vertices * (size_t)floats_per_vertex * sizeof(float);

    float *vertex_data = malloc(vertex_buffer_size);

    glBufferData(GL_ARRAY_BUFFER, vertex_buffer_size, 0, GL_STREAM_DRAW);

    srandom(time(0));

    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(gfx.egl_display, gfx.egl_surface);
    graphics_present(&gfx);

    for (;;)
    {
        double t0 = get_seconds();
        for (int i = 0; i < line_count; i++)
        {
            int x0 = random() % gfx.screen_width;
            int y0 = random() % gfx.screen_height;
            int x1 = random() % gfx.screen_width;
            int y1 = random() % gfx.screen_height;

            float fx0 = 2.0f * x0 / (gfx.screen_width - 1) - 1.0f;
            float fy0 = 1.0f - 2.0f * y0 / (gfx.screen_height - 1);

            float fx1 = 2.0f * x1 / (gfx.screen_width - 1) - 1.0f;
            float fy1 = 1.0f - 2.0f * y1 / (gfx.screen_height - 1);

            float r = (random() % 256) / 255.0f;
            float g = (random() % 256) / 255.0f;
            float b = (random() % 256) / 255.0f;

            int base = i * vertices_per_line * floats_per_vertex;

            vertex_data[base + 0]  = fx0;
            vertex_data[base + 1]  = fy0;
            vertex_data[base + 2]  = r;
            vertex_data[base + 3]  = g;
            vertex_data[base + 4]  = b;
            vertex_data[base + 5]  = 1.0f;

            vertex_data[base + 6]  = fx1;
            vertex_data[base + 7]  = fy1;
            vertex_data[base + 8]  = r;
            vertex_data[base + 9]  = g;
            vertex_data[base + 10] = b;
            vertex_data[base + 11] = 1.0f;
        }
        double t1 = get_seconds();
        glClear(GL_COLOR_BUFFER_BIT);
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertex_buffer_size, vertex_data);
        glDrawArrays(GL_LINES, 0, total_vertices);
        eglSwapBuffers(gfx.egl_display, gfx.egl_surface);
        graphics_present(&gfx);
        double t2 = get_seconds();
        printf("Create Vert: %.6f sec \n", (t1 - t0));
        printf("Draw Lines : %.6f sec \n", (t2 - t1));
        printf("Total Time : %.6f sec \n \n", (t2 - t0));
        sleep(1);
    }

    return 0;
}