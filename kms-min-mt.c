// kms-min.c
// gcc kms-min.c -O3 -o kms-min \
//     $(pkg-config --cflags --libs libdrm) -pthread

#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <inttypes.h>

uint32_t plot_counter = 0;

typedef struct {
   int x0;
   int y0;
   int x1;
   int y1;
   uint32_t c;
} line_t;

typedef struct {
   uint32_t *pixels;
   uint32_t width;
   uint32_t height;
   uint32_t pitch;
   uint32_t size;
} framebuffer_t;

typedef struct {
   framebuffer_t *fb;
   line_t *line_list;
   int start;
   int end;
} thread_arg_t;

static inline double get_seconds()
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC,&ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static inline int random_int(int min, int max)
{
   return min + (int)(random() % (unsigned)(max - min + 1));
}

static inline void put_pixel(framebuffer_t *fb, int x, int y, uint32_t argb)
{
   uint8_t  *base = (uint8_t *)fb->pixels;
   uint32_t *row  = (uint32_t *)(base + (uint64_t)y * fb->pitch);
   row[x] = argb;
}

static inline void draw_line(framebuffer_t *fb, int x0, int y0, int x1, int y1, uint32_t argb)
{
   int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
   int sx = (x0 < x1) ? 1 : -1;
   int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
   int sy = (y0 < y1) ? 1 : -1;
   int err = dx + dy;

   for(;;) {
      put_pixel(fb, x0, y0, argb);
      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
   }
}

framebuffer_t init_framebuffer()
{
   int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
   drmModeRes *res = drmModeGetResources(fd);
   drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[0]);
   drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
   drmModeModeInfo mode = conn->modes[0];

   struct drm_mode_create_dumb creq = {0};
   creq.width = mode.hdisplay;
   creq.height = mode.vdisplay;
   creq.bpp = 32;
   ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);

   uint32_t fb;
   drmModeAddFB(fd,creq.width, creq.height, 24, 32, creq.pitch, creq.handle, &fb);
   drmModeSetCrtc(fd, enc->crtc_id, fb, 0, 0, &conn->connector_id, 1, &mode);

   struct drm_mode_map_dumb mreq = {0};
   mreq.handle = creq.handle;
   ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);

   uint32_t *p = mmap(0, creq.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, mreq.offset);

   framebuffer_t fb_t = {
       .pixels = p,
       .width = creq.width,
       .height = creq.height,
       .pitch  = creq.pitch,
       .size   = creq.size,
   };
   return fb_t;
}

void *worker(void *arg)
{
   thread_arg_t *a = (thread_arg_t*)arg;

   for (int i = a->start; i < a->end; i++) {
      a->line_list[i].x0 = random_int(0, a->fb->width-1);
      a->line_list[i].y0 = random_int(0, a->fb->height-1);
      a->line_list[i].x1 = random_int(0, a->fb->width-1);
      a->line_list[i].y1 = random_int(0, a->fb->height-1);
      a->line_list[i].c  = 0xFF000000u | (random() & 0x00FFFFFFu);
   }

   for (int i = a->start; i < a->end; i++) {
      draw_line(a->fb,
                a->line_list[i].x0,
                a->line_list[i].y0,
                a->line_list[i].x1,
                a->line_list[i].y1,
                a->line_list[i].c);
   }

   return NULL;
}

int main()
{
   int line_count = 100000;
   line_t line_list[line_count];

   framebuffer_t fb_t = init_framebuffer();
   srandom(time(NULL));

   const int THREADS = 6;
   pthread_t threads[THREADS];
   thread_arg_t args[THREADS];

   while(1)
   {
      double t0 = get_seconds();

      int chunk = line_count / THREADS;

      for (int t = 0; t < THREADS; t++) {
         args[t].fb = &fb_t;
         args[t].line_list = line_list;
         args[t].start = t * chunk;
         args[t].end = (t == THREADS-1) ? line_count : (t+1)*chunk;
         pthread_create(&threads[t], NULL, worker, &args[t]);
      }

      for (int t = 0; t < THREADS; t++)
         pthread_join(threads[t], NULL);

      double t1 = get_seconds();

      printf("Total Time : %.6f sec\n\n", (t1 - t0));

      sleep(1);
   }
}
