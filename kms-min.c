// kms-min.c
// gcc kms-min.c -o kms-min \
        $(pkg-config --cflags --libs libdrm)
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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

static inline void clear(framebuffer_t *fb, uint32_t argb)
{
   uint8_t *base = (uint8_t *)fb->pixels;

   for (uint32_t y = 0; y < fb->height; y++) {
      uint32_t *row = (uint32_t *)(base + (uint64_t)y * fb->pitch);
      for (uint32_t x = 0; x < fb->width; x++) {
         row[x] = argb;
      }
   }
}

static inline void put_pixel(framebuffer_t *fb, int x, int y, uint32_t argb)
{
   //if ((unsigned)x >= fb->width || (unsigned)y >= fb-> height) return;

   uint8_t  *base = (uint8_t *)fb->pixels;
   uint32_t *row  = (uint32_t *)(base + (uint64_t)y * fb->pitch);
   row[x] = argb;
   //plot_counter++;
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
      if (e2 >= dy) {err += dy; x0 += sx; }
      if (e2 <= dx) {err += dx; y0 += sy; }
   }
}

framebuffer_t init_framebuffer()
{
   // Open DRM device (display controller / DPU)
   int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
   // Query DRM ressources (connectors, encoders, CRTCs)
   drmModeRes *res = drmModeGetResources(fd);
   // Select first connected display and its first mode
   drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[0]);
   drmModeEncoder *enc =  drmModeGetEncoder(fd, conn->encoder_id);
   drmModeModeInfo mode = conn->modes[0];
   // Mode provides >> hdisplay, vdisplay

   // GEM = Graphical Execution Manager
   // Create a simple ("dumb") GEM buffer in kernel memory
   struct drm_mode_create_dumb creq = {0};
   creq.width = mode.hdisplay;
   creq.height = mode.vdisplay;
   creq.bpp = 32;
   ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
   // creq.width / creq.height, creq.pitch >> length of row in bytes
   // creq.size >> size of entire buffer in bytes
   // Create DRM framebuffer object referencing the GEM buffer
   uint32_t fb;
   drmModeAddFB(fd,creq.width, creq.height, 24, 32, creq.pitch, creq.handle, &fb);
   // Bind DRM framebuffer to CRTC and connector
   drmModeSetCrtc(fd, enc->crtc_id, fb, 0, 0, &conn->connector_id, 1, &mode);

   // Map GEM buffer into user space
   struct drm_mode_map_dumb mreq = {0};
   mreq.handle = creq.handle;
   ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);

   uint32_t *p = mmap(0, creq.size,PROT_READ|PROT_WRITE, MAP_SHARED, fd, mreq.offset);

   framebuffer_t  fb_t = {
       .pixels = (uint32_t *)p,
       .width = creq.width,
       .height = creq.height,
       .pitch  = creq.pitch,
       .size   = creq.size,
   };
   return fb_t;
}

int main() {
   int line_count = 100000;
   line_t line_list[line_count];

   framebuffer_t fb_t = init_framebuffer();
   srandom(time(NULL));
   while(1){
      //clear(&fb_t,0xFF000000u);
      plot_counter = 0;

      double t0 = get_seconds();
      for (int i = 0; i < line_count; i++) {
        line_list[i].x0 = random_int(0, fb_t.width-1);
        line_list[i].y0 = random_int(0, fb_t.height-1);
        line_list[i].x1 = random_int(0, fb_t.width-1);
        line_list[i].y1 = random_int(0, fb_t.height-1);
        line_list[i].c  =  0xFF000000u | (random() & 0x00FFFFFFu);
      }
      double t1 = get_seconds();

      for (int i = 0; i < line_count; i++) {
        draw_line(&fb_t,line_list[i].x0,line_list[i].y0,
                        line_list[i].x1,line_list[i].y1,
                        line_list[i].c);
      }
      double t2 = get_seconds();
      printf("Create Vert: %.6f sec \n", (t1 - t0));
      printf("Draw Lines : %.6f sec \n", (t2 - t1));
      printf("Total Time : %.6f sec \n", (t2 - t0));
      printf("Plot_Counter: %" PRIu32 "\n\n", plot_counter);

      sleep(1);
   }
}
