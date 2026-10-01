// mini2d - small 2D software renderer for the Miyoo Mini / Mini Plus (see mini2d.c)
#ifndef MINI2D_H
#define MINI2D_H
#include <stdint.h>
#include <stddef.h>

typedef struct m2d_surf {
    uint32_t *px;      // ARGB8888 premultiplied
    uint64_t phy;      // physical address (back buffer only)
    int w, h, stride;  // stride in bytes
    // optional run-length table of non-transparent runs per row (static images):
    // spans[rowidx[y] .. rowidx[y+1]) are triplets {x0, x1, opaque}
    uint32_t *rowidx;
    uint16_t *spans;
} m2d_surf;

enum { M2D_REPLACE = 0, M2D_ALPHA = 1, M2D_ADD = 2 };

m2d_surf *m2d_surface_new(int w, int h);
void m2d_surface_free(m2d_surf *s);
void m2d_surface_build_spans(m2d_surf *s);
void m2d_surface_upload_rgba(m2d_surf *s, const uint8_t *rgba, int sw, int sh);
void m2d_surface_copy(m2d_surf *dst, m2d_surf *src);
m2d_surf *m2d_backbuffer(void);
void m2d_set_clip(int x, int y, int w, int h);
void m2d_reset_clip(void);
void m2d_clear(m2d_surf *d, uint32_t argb, int use_clip);
void m2d_fill(m2d_surf *d, int x, int y, int w, int h, uint32_t argb, int blend);
void m2d_blit(m2d_surf *s, int sx, int sy, int sw, int sh,
              m2d_surf *d, int dx, int dy, int dw, int dh,
              uint32_t tint, int blend, int flip);
void m2d_line(m2d_surf *d, int x0, int y0, int x1, int y1, int width, uint32_t argb, int blend);
void m2d_circle(m2d_surf *d, int cx, int cy, int r, uint32_t argb, int blend, int fill, int lw);
int m2d_init(int w, int h);
void m2d_present(void);
int m2d_poll_key(int *code, int *value);
int m2d_frame_count(void);
void m2d_quit(void);

#endif
