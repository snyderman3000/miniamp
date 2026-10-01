// mini2d - small 2D software renderer for the Miyoo Mini / Mini Plus.
//
// All drawing is done by the CPU into ARGB8888 premultiplied-alpha surfaces.
// Only the back buffer lives in physically contiguous (MMA) memory; present()
// uses the SigmaStar 2D engine (GE) to scale + rotate it onto the framebuffer.
//
// Build with -DM2D_HOST for a desktop stub: back buffer in RAM, frames can be
// dumped as PPM, and key presses can be scripted from a file.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef M2D_HOST
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include "mi_sys.h"
#include "mi_gfx.h"
#endif

#include "mini2d.h"

static m2d_surf back;
#define MAX_VIEWS 16
static struct { m2d_surf *s; int x, y; } views[MAX_VIEWS];
static int fb_w = 640, fb_h = 480;
static int frame_no = 0;

// statistics (pixels touched per path), read + reset by m2d_stats()
static double st[8];
enum { ST_BLIT_CALLS, ST_PX_COPY, ST_PX_ALPHA, ST_PX_GENERIC, ST_PX_FILL, ST_PX_CLEAR, ST_FILL_CALLS, ST_PX_SCALED };
void m2d_stats(double *out) { for (int i = 0; i < 8; i++) { out[i] = st[i]; st[i] = 0; } }

// clip rectangle (applies to every draw op), in destination pixels
static int clip_on = 0, clip_x0, clip_y0, clip_x1, clip_y1;

#ifndef M2D_HOST
static int fb_fd = -1, in_fd = -1;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static uint64_t fb_phy;
#else
static FILE *script = NULL;
static int script_frame = -1, script_code, script_value;
#endif

// ---------------------------------------------------------------- surfaces
m2d_surf *m2d_surface_new(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    m2d_surf *s = (m2d_surf *)calloc(1, sizeof(m2d_surf));
    if (!s) return NULL;
    s->w = w; s->h = h; s->stride = w * 4;
    s->px = (uint32_t *)calloc((size_t)w * h, 4);
    if (!s->px) { free(s); return NULL; }
    return s;
}

void m2d_surface_free(m2d_surf *s)
{
    if (!s || s == &back) return;
    free(s->px);
    free(s->rowidx);
    free(s->spans);
    free(s);
}

// Build the run table: lets alpha blits skip transparent pixels and memcpy opaque runs.
void m2d_surface_build_spans(m2d_surf *s)
{
    free(s->rowidx); free(s->spans);
    s->rowidx = NULL; s->spans = NULL;
    size_t n = 0;
    for (int pass = 0; pass < 2; pass++) {
        size_t k = 0;
        for (int y = 0; y < s->h; y++) {
            if (pass) s->rowidx[y] = (uint32_t)k;
            const uint32_t *row = (const uint32_t *)((uint8_t *)s->px + y * s->stride);
            int x = 0;
            while (x < s->w) {
                uint32_t a = row[x] >> 24;
                if (a == 0) { x++; continue; }
                int op = (a == 255);
                int x0 = x;
                while (x < s->w) {
                    uint32_t b = row[x] >> 24;
                    if (b == 0 || (b == 255) != op) break;
                    x++;
                }
                if (pass) { s->spans[k * 3] = x0; s->spans[k * 3 + 1] = x; s->spans[k * 3 + 2] = op; }
                k++;
            }
        }
        if (!pass) {
            n = k;
            s->rowidx = (uint32_t *)malloc(sizeof(uint32_t) * (s->h + 1));
            s->spans = (uint16_t *)malloc(sizeof(uint16_t) * 3 * (n ? n : 1));
            if (!s->rowidx || !s->spans) { free(s->rowidx); free(s->spans); s->rowidx = NULL; s->spans = NULL; return; }
        } else {
            s->rowidx[s->h] = (uint32_t)k;
        }
    }
}

// a surface that aliases a rectangle of another surface (no copy, not freed)
m2d_surf *m2d_surface_view(m2d_surf *parent, int x, int y, int w, int h)
{
    m2d_surf *s = (m2d_surf *)calloc(1, sizeof(m2d_surf));
    if (!s) return NULL;
    s->w = w; s->h = h; s->stride = parent->stride;
    s->px = (uint32_t *)((uint8_t *)parent->px + y * parent->stride) + x;
    s->phy = 0;
    if (parent == &back) { // follow the back buffer when it flips
        for (int i = 0; i < MAX_VIEWS; i++)
            if (!views[i].s) { views[i].s = s; views[i].x = x; views[i].y = y; break; }
    }
    return s;
}

void m2d_surface_view_free(m2d_surf *s)
{
    for (int i = 0; i < MAX_VIEWS; i++) if (views[i].s == s) views[i].s = NULL;
    free(s);
}

// composite over black: premultiplied colour stays, alpha becomes 255
void m2d_surface_flatten(m2d_surf *s)
{
    for (int y = 0; y < s->h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)s->px + y * s->stride);
        for (int x = 0; x < s->w; x++) row[x] |= 0xff000000u;
    }
}

int m2d_surface_is_opaque(m2d_surf *s)
{
    for (int y = 0; y < s->h; y++) {
        const uint32_t *row = (const uint32_t *)((uint8_t *)s->px + y * s->stride);
        for (int x = 0; x < s->w; x++) if ((row[x] >> 24) != 255) return 0;
    }
    return 1;
}

// resample src (premultiplied) into dst's size: box filter down, nearest up
void m2d_surface_resample(m2d_surf *d, m2d_surf *src)
{
    int sw = src->w, sh = src->h, dw = d->w, dh = d->h;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        uint32_t *drow = (uint32_t *)((uint8_t *)d->px + y * d->stride);
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint32_t *srow = (const uint32_t *)((uint8_t *)src->px + yy * src->stride);
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t c = srow[xx];
                    a += c >> 24; r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255; n++;
                }
            }
            drow[x] = (((a + n / 2) / n) << 24) | (((r + n / 2) / n) << 16) | (((g + n / 2) / n) << 8) | ((b + n / 2) / n);
        }
    }
}

size_t m2d_surface_bytes(m2d_surf *s) { return s ? (size_t)s->stride * s->h : 0; }

static inline uint32_t premul(const uint8_t *p)
{
    uint32_t a = p[3];
    if (a == 255) return 0xff000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    if (a == 0) return 0;
    uint32_t r = (p[0] * a + 127) / 255;
    uint32_t g = (p[1] * a + 127) / 255;
    uint32_t b = (p[2] * a + 127) / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// Upload straight-alpha RGBA8 (LÖVE ImageData) of size sw x sh into the whole
// surface, resampling to the surface size (box filter when shrinking).
void m2d_surface_upload_rgba(m2d_surf *s, const uint8_t *rgba, int sw, int sh)
{
    free(s->rowidx); free(s->spans); s->rowidx = NULL; s->spans = NULL;
    int dw = s->w, dh = s->h;
    if (dw == sw && dh == sh) {
        for (int y = 0; y < sh; y++) {
            uint32_t *d = (uint32_t *)((uint8_t *)s->px + y * s->stride);
            const uint8_t *p = rgba + (size_t)y * sw * 4;
            for (int x = 0; x < sw; x++, p += 4) d[x] = premul(p);
        }
        return;
    }
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        uint32_t *d = (uint32_t *)((uint8_t *)s->px + y * s->stride);
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = (int)((int64_t)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            uint32_t sa = 0, sr = 0, sg = 0, sb = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *p = rgba + ((size_t)yy * sw + x0) * 4;
                for (int xx = x0; xx < x1; xx++, p += 4) {
                    uint32_t a = p[3];
                    sa += a;
                    sr += p[0] * a; sg += p[1] * a; sb += p[2] * a;
                    n++;
                }
            }
            if (sa == 0) { d[x] = 0; continue; }
            uint32_t A = (sa + n / 2) / n;
            uint32_t R = (sr / 255 + n / 2) / n, G = (sg / 255 + n / 2) / n, B = (sb / 255 + n / 2) / n;
            if (R > A) R = A;
            if (G > A) G = A;
            if (B > A) B = A;
            d[x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
}

// Upload a luminance+alpha glyph (w x h) at x0,y0 as premultiplied white.
void m2d_surface_upload_la8(m2d_surf *s, const uint8_t *la, int x0, int y0, int w, int h)
{
    for (int y = 0; y < h; y++) {
        if (y0 + y < 0 || y0 + y >= s->h) continue;
        uint32_t *d = (uint32_t *)((uint8_t *)s->px + (y0 + y) * s->stride);
        const uint8_t *p = la + (size_t)y * w * 2;
        for (int x = 0; x < w; x++, p += 2) {
            if (x0 + x < 0 || x0 + x >= s->w) continue;
            uint32_t a = p[1];
            uint32_t l = (p[0] * a + 127) / 255;
            d[x0 + x] = (a << 24) | (l << 16) | (l << 8) | l;
        }
    }
}

// Read back as straight-alpha RGBA8 (for canvas:newImageData)
void m2d_surface_download_rgba(m2d_surf *s, uint8_t *rgba)
{
    for (int y = 0; y < s->h; y++) {
        const uint32_t *row = (const uint32_t *)((uint8_t *)s->px + y * s->stride);
        for (int x = 0; x < s->w; x++, rgba += 4) {
            uint32_t c = row[x], a = c >> 24;
            if (a == 0) { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; continue; }
            rgba[0] = (uint8_t)((((c >> 16) & 255) * 255 + a / 2) / a);
            rgba[1] = (uint8_t)((((c >> 8) & 255) * 255 + a / 2) / a);
            rgba[2] = (uint8_t)(((c & 255) * 255 + a / 2) / a);
            rgba[3] = (uint8_t)a;
        }
    }
}

void m2d_surface_copy(m2d_surf *dst, m2d_surf *src)
{
    if (dst->w == src->w && dst->h == src->h) memcpy(dst->px, src->px, (size_t)src->stride * src->h);
}

m2d_surf *m2d_backbuffer(void) { return &back; }

// ---------------------------------------------------------------- clip
void m2d_set_clip(int x, int y, int w, int h)
{
    clip_on = 1;
    clip_x0 = x; clip_y0 = y; clip_x1 = x + w; clip_y1 = y + h;
}

void m2d_reset_clip(void) { clip_on = 0; }

// intersect [x0,x1)x[y0,y1) with surface and clip; returns 0 if empty
static inline int do_clip(m2d_surf *d, int *x0, int *y0, int *x1, int *y1)
{
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > d->w) *x1 = d->w;
    if (*y1 > d->h) *y1 = d->h;
    if (clip_on) {
        if (*x0 < clip_x0) *x0 = clip_x0;
        if (*y0 < clip_y0) *y0 = clip_y0;
        if (*x1 > clip_x1) *x1 = clip_x1;
        if (*y1 > clip_y1) *y1 = clip_y1;
    }
    return *x0 < *x1 && *y0 < *y1;
}

// ---------------------------------------------------------------- pixel ops
static inline uint32_t mul_px(uint32_t c, uint32_t f) // f: 0..256
{
    uint32_t rb = ((c & 0x00ff00ff) * f >> 8) & 0x00ff00ff;
    uint32_t ag = (((c >> 8) & 0x00ff00ff) * f) & 0xff00ff00;
    return rb | ag;
}

static inline uint32_t over(uint32_t s, uint32_t d)
{
    uint32_t a = s >> 24;
    if (a == 255) return s;
    if (a == 0) return d;
    return s + mul_px(d, 256 - a);
}

static inline uint32_t add_px(uint32_t s, uint32_t d)
{
    uint32_t r = ((s >> 16) & 255) + ((d >> 16) & 255);
    uint32_t g = ((s >> 8) & 255) + ((d >> 8) & 255);
    uint32_t b = (s & 255) + (d & 255);
    uint32_t a = (s >> 24) + (d >> 24);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (a > 255) a = 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline uint32_t tint_px(uint32_t c, uint32_t tint)
{
    uint32_t a = ((c >> 24) * ((tint >> 24) + 1)) >> 8;
    uint32_t r = (((c >> 16) & 255) * (((tint >> 16) & 255) + 1)) >> 8;
    uint32_t g = (((c >> 8) & 255) * (((tint >> 8) & 255) + 1)) >> 8;
    uint32_t b = ((c & 255) * ((tint & 255) + 1)) >> 8;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

enum { BLEND_REPLACE = 0, BLEND_ALPHA = 1, BLEND_ADD = 2 };

static inline uint32_t blend_px(uint32_t s, uint32_t d, int mode)
{
    if (mode == BLEND_ALPHA) return over(s, d);
    if (mode == BLEND_ADD) return add_px(s, d);
    return s;
}

// clear a whole surface (ignores clip unless use_clip)
void m2d_clear(m2d_surf *d, uint32_t argb, int use_clip)
{
    st[ST_PX_CLEAR] += (double)d->w * d->h;
    if (!use_clip || !clip_on) {
        if (argb == 0) { memset(d->px, 0, (size_t)d->stride * d->h); return; }
        for (int y = 0; y < d->h; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)d->px + y * d->stride);
            for (int x = 0; x < d->w; x++) row[x] = argb;
        }
        return;
    }
    int x0 = 0, y0 = 0, x1 = d->w, y1 = d->h;
    if (!do_clip(d, &x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)d->px + y * d->stride);
        for (int x = x0; x < x1; x++) row[x] = argb;
    }
}

void m2d_fill(m2d_surf *d, int x, int y, int w, int h, uint32_t argb, int blend)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (w <= 0 || h <= 0 || !do_clip(d, &x0, &y0, &x1, &y1)) return;
    uint32_t a = argb >> 24;
    if (blend == BLEND_ALPHA && a == 0) return;
    if (blend == BLEND_ALPHA && a == 255) blend = BLEND_REPLACE;
    st[ST_FILL_CALLS] += 1; st[ST_PX_FILL] += (double)(x1 - x0) * (y1 - y0);
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = (uint32_t *)((uint8_t *)d->px + yy * d->stride);
        if (blend == BLEND_REPLACE) for (int xx = x0; xx < x1; xx++) row[xx] = argb;
        else if (blend == BLEND_ALPHA) {
            uint32_t inv = 256 - a;
            for (int xx = x0; xx < x1; xx++) row[xx] = argb + mul_px(row[xx], inv);
        } else for (int xx = x0; xx < x1; xx++) row[xx] = add_px(argb, row[xx]);
    }
}

// Blit src rect (sx,sy,sw,sh) to dst rect (dx,dy,dw,dh) with nearest scaling.
// flip: bit0 = mirror x, bit1 = mirror y. tint: premultiplied ARGB (0xffffffff = none)
static int nodraw = -1;
void m2d_blit(m2d_surf *s, int sx, int sy, int sw, int sh,
              m2d_surf *d, int dx, int dy, int dw, int dh,
              uint32_t tint, int blend, int flip)
{
#ifdef M2D_HOST
    if (nodraw < 0) nodraw = getenv("M2D_NODRAW") != NULL;
    if (nodraw) return;
#endif
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    // keep source rect inside the surface
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx + sw > s->w) sw = s->w - sx;
    if (sy + sh > s->h) sh = s->h - sy;
    if (sw <= 0 || sh <= 0) return;

    int x0 = dx, y0 = dy, x1 = dx + dw, y1 = dy + dh;
    if (!do_clip(d, &x0, &y0, &x1, &y1)) return;

    uint32_t stepx = (uint32_t)(((uint64_t)sw << 16) / dw);
    uint32_t stepy = (uint32_t)(((uint64_t)sh << 16) / dh);
    uint32_t fx0 = (uint32_t)(x0 - dx) * stepx;
    uint32_t fy = (uint32_t)(y0 - dy) * stepy;
    int unscaled = (sw == dw && sh == dh);
    int notint = (tint == 0xffffffffu);
    int fx_flip = flip & 1, fy_flip = flip & 2;
    if (tint == 0 && blend != BLEND_REPLACE) return; // fully transparent
    st[ST_BLIT_CALLS] += 1;
    {
        double px = (double)(x1 - x0) * (y1 - y0);
        if (!fx_flip && unscaled && notint && blend == BLEND_REPLACE) st[ST_PX_COPY] += px;
        else if (!fx_flip && unscaled && notint && blend == BLEND_ALPHA) st[ST_PX_ALPHA] += px;
        else st[ST_PX_GENERIC] += px;
        if (!unscaled) st[ST_PX_SCALED] += px;
    }

    for (int y = y0; y < y1; y++, fy += stepy) {
        int srcy = (int)(fy >> 16);
        if (srcy >= sh) srcy = sh - 1;
        if (fy_flip) srcy = sh - 1 - srcy;
        const uint32_t *srow = (const uint32_t *)((uint8_t *)s->px + (sy + srcy) * s->stride) + sx;
        uint32_t *drow = (uint32_t *)((uint8_t *)d->px + y * d->stride);

        if (!fx_flip && unscaled && notint) {
            const uint32_t *sp = srow + (x0 - dx);
            if (blend == BLEND_REPLACE) { memcpy(drow + x0, sp, (x1 - x0) * 4); continue; }
            if (blend == BLEND_ALPHA && s->rowidx) {
                // walk only the non-transparent runs of this source row
                int off = sx - dx;                // src x = dst x + off
                int lo = x0 + off, hi = x1 + off; // visible src range
                int r = sy + srcy;
                for (uint32_t k = s->rowidx[r]; k < s->rowidx[r + 1]; k++) {
                    int a0 = s->spans[k * 3], a1 = s->spans[k * 3 + 1];
                    if (a1 <= lo) continue;
                    if (a0 >= hi) break;
                    if (a0 < lo) a0 = lo;
                    if (a1 > hi) a1 = hi;
                    const uint32_t *src = (const uint32_t *)((uint8_t *)s->px + r * s->stride) + a0;
                    uint32_t *dst = drow + (a0 - off);
                    if (s->spans[k * 3 + 2]) memcpy(dst, src, (a1 - a0) * 4);
                    else for (int i = 0; i < a1 - a0; i++) { uint32_t c = src[i]; dst[i] = c + mul_px(dst[i], 256 - (c >> 24)); }
                }
                continue;
            }
            if (blend == BLEND_ALPHA) {
                for (int x = x0; x < x1; x++, sp++) {
                    uint32_t c = *sp, a = c >> 24;
                    if (a == 255) drow[x] = c;
                    else if (a) drow[x] = c + mul_px(drow[x], 256 - a);
                }
                continue;
            }
        }
        if (fx_flip && unscaled && notint && blend == BLEND_ALPHA) {
            // mirrored 1:1 copy (e.g. the second player's board frame)
            const uint32_t *sp = srow + (sw - 1 - (x0 - dx));
            for (int x = x0; x < x1; x++, sp--) {
                uint32_t c = *sp, a = c >> 24;
                if (a == 255) drow[x] = c;
                else if (a) drow[x] = c + mul_px(drow[x], 256 - a);
            }
            continue;
        }
        uint32_t fx = fx0;
        for (int x = x0; x < x1; x++, fx += stepx) {
            int srcx = (int)(fx >> 16);
            if (srcx >= sw) srcx = sw - 1;
            if (fx_flip) srcx = sw - 1 - srcx;
            uint32_t c = srow[srcx];
            if (!notint) c = tint_px(c, tint);
            drow[x] = blend_px(c, drow[x], blend);
        }
    }
}

// thick line from (x0,y0) to (x1,y1); width in pixels
void m2d_line(m2d_surf *d, int x0, int y0, int x1, int y1, int width, uint32_t argb, int blend)
{
    if (width < 1) width = 1;
    int half = width / 2;
    if (y0 == y1) {
        int a = x0 < x1 ? x0 : x1, b = x0 < x1 ? x1 : x0;
        m2d_fill(d, a, y0 - half, b - a + 1, width, argb, blend);
        return;
    }
    if (x0 == x1) {
        int a = y0 < y1 ? y0 : y1, b = y0 < y1 ? y1 : y0;
        m2d_fill(d, x0 - half, a, width, b - a + 1, argb, blend);
        return;
    }
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        m2d_fill(d, x0 - half, y0 - half, width, width, argb, blend);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// filled circle, or ring of thickness lw when fill == 0
void m2d_circle(m2d_surf *d, int cx, int cy, int r, uint32_t argb, int blend, int fill, int lw)
{
    if (r <= 0) return;
    int r2 = r * r;
    int ri = fill ? -1 : r - (lw < 1 ? 1 : lw);
    int ri2 = ri > 0 ? ri * ri : -1;
    for (int y = -r; y <= r; y++) {
        int yy = y * y;
        int xo = 0;
        while ((xo + 1) * (xo + 1) + yy <= r2) xo++;
        if (fill || ri2 < 0 || yy > ri2) {
            m2d_fill(d, cx - xo, cy + y, 2 * xo + 1, 1, argb, blend);
        } else {
            int xi = 0;
            while ((xi + 1) * (xi + 1) + yy <= ri2) xi++;
            m2d_fill(d, cx - xo, cy + y, xo - xi, 1, argb, blend);
            m2d_fill(d, cx + xi + 1, cy + y, xo - xi, 1, argb, blend);
        }
    }
}

// ---------------------------------------------------------------- init / present / input
// Two back buffers: the CPU draws into one while the other is being shown.
// On the device, presenting (cache flush, GE blit, page flip, vsync wait) runs
// on its own thread, so a frame only has to *draw* within 16.7 ms to hit 60 fps.
static struct { uint32_t *px; uint64_t phy; } bufs[2];
static int cur = 0;


static void point_views(void)
{
    for (int i = 0; i < MAX_VIEWS; i++)
        if (views[i].s) views[i].s->px = (uint32_t *)((uint8_t *)back.px + views[i].y * back.stride) + views[i].x;
}

#ifndef M2D_HOST
#include <pthread.h>
static pthread_t pthr;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int job = -1, quitting = 0, threaded = 0;

static void show_buffer(int b)
{
    MI_SYS_FlushInvCache(bufs[b].px, back.stride * back.h);
    MI_GFX_Surface_t ss, ds; MI_GFX_Rect_t sr, dr; MI_GFX_Opt_t opt; MI_U16 fence = 0;
    ss.phyAddr = bufs[b].phy; ss.eColorFmt = E_MI_GFX_FMT_ARGB8888;
    ss.u32Width = back.w; ss.u32Height = back.h; ss.u32Stride = back.stride;
    ds.phyAddr = fb_phy + (uint64_t)fb_w * vinfo.yoffset * 4;
    ds.eColorFmt = E_MI_GFX_FMT_ARGB8888;
    ds.u32Width = fb_w; ds.u32Height = fb_h; ds.u32Stride = fb_w * 4;
    int sc = fb_w / back.w < fb_h / back.h ? fb_w / back.w : fb_h / back.h;
    if (sc < 1) sc = 1;
    sr.s32Xpos = 0; sr.s32Ypos = 0; sr.u32Width = back.w; sr.u32Height = back.h;
    dr.u32Width = back.w * sc; dr.u32Height = back.h * sc;
    dr.s32Xpos = (fb_w - (int)dr.u32Width) / 2; dr.s32Ypos = (fb_h - (int)dr.u32Height) / 2;
    memset(&opt, 0, sizeof(opt));
    opt.stClipRect.u32Width = fb_w; opt.stClipRect.u32Height = fb_h;
    opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
    opt.eRotate = E_MI_GFX_ROTATE_180;
    MI_GFX_BitBlit(&ss, &sr, &ds, &dr, &opt, &fence);
    MI_GFX_WaitAllDone(TRUE, fence);
    ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo); // waits for vblank
    vinfo.yoffset ^= fb_h;
}

static void *present_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mtx);
    for (;;) {
        while (job < 0 && !quitting) pthread_cond_wait(&cv, &mtx);
        if (job < 0 && quitting) break;
        int b = job;
        pthread_mutex_unlock(&mtx);
        show_buffer(b);
        pthread_mutex_lock(&mtx);
        job = -1;
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&mtx);
    return NULL;
}
#endif

static int alloc_buffer(int i, size_t size)
{
#ifndef M2D_HOST
    MI_PHY phy = 0;
    void *vir = NULL;
    if (MI_SYS_MMA_Alloc(NULL, size, &phy) != 0) return -1;
    if (MI_SYS_Mmap(phy, size, &vir, TRUE) != 0) return -1;
    bufs[i].phy = phy;
    bufs[i].px = (uint32_t *)vir;
#else
    bufs[i].px = (uint32_t *)malloc(size);
    if (!bufs[i].px) return -1;
#endif
    memset(bufs[i].px, 0, size);
    return 0;
}

int m2d_init(int w, int h)
{
    back.w = w; back.h = h; back.stride = w * 4;
    size_t size = (size_t)back.stride * h;
#ifndef M2D_HOST
    if (MI_SYS_Init() != 0) return -1;
    if (MI_GFX_Open() != 0) return -2;
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) return -3;
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    fb_w = vinfo.xres; fb_h = vinfo.yres;
    vinfo.yoffset = 0;
    vinfo.yres_virtual = vinfo.yres * 2;
    ioctl(fb_fd, FBIOPUT_VSCREENINFO, &vinfo);
    fb_phy = finfo.smem_start;
    MI_SYS_MemsetPa(fb_phy, 0, fb_w * fb_h * 4 * 2);
    in_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
#else
    const char *sp = getenv("M2D_INPUT");
    if (sp) script = fopen(sp, "r");
#endif
    if (alloc_buffer(0, size) != 0) return -4;
    if (alloc_buffer(1, size) != 0) return -5;
    cur = 0;
    back.px = bufs[0].px; back.phy = bufs[0].phy;
#ifndef M2D_HOST
    const char *t = getenv("M2D_SYNC_PRESENT");
    threaded = !(t && *t == '1');
    if (threaded && pthread_create(&pthr, NULL, present_thread, NULL) != 0) threaded = 0;
#endif
    return 0;
}

int m2d_fb_width(void) { return fb_w; }
int m2d_fb_height(void) { return fb_h; }
int m2d_frame_count(void) { return frame_no; }

// Hands the finished frame to the display and switches drawing to the other buffer.
// Blocks only while the previous frame is still waiting for its vblank.
void m2d_present(void)
{
    frame_no++;
#ifndef M2D_HOST
    if (threaded) {
        pthread_mutex_lock(&mtx);
        while (job >= 0) pthread_cond_wait(&cv, &mtx);
        job = cur;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mtx);
    } else {
        show_buffer(cur);
    }
#else
    const char *dir = getenv("M2D_DUMP");
    const char *ev = getenv("M2D_DUMP_EVERY");
    int every = ev ? atoi(ev) : 60;
    if (every < 1) every = 1;
    if (dir && frame_no % every == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/f%06d.ppm", dir, frame_no);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", back.w, back.h);
            for (int i = 0; i < back.w * back.h; i++) {
                uint32_t c = back.px[i];
                unsigned char rgb[3] = { (c >> 16) & 255, (c >> 8) & 255, c & 255 };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
    }
#endif
    cur ^= 1;
    back.px = bufs[cur].px; back.phy = bufs[cur].phy;
    point_views();
}

// returns 1 and fills code/value when a key event is available
int m2d_poll_key(int *code, int *value)
{
#ifndef M2D_HOST
    struct input_event ev;
    while (in_fd >= 0 && read(in_fd, &ev, sizeof ev) == sizeof ev) {
        if (ev.type == EV_KEY) { *code = ev.code; *value = ev.value; return 1; }
    }
    return 0;
#else
    if (!script) return 0;
    if (script_frame < 0) {
        if (fscanf(script, "%d %d %d", &script_frame, &script_code, &script_value) != 3) {
            fclose(script); script = NULL; return 0;
        }
    }
    if (frame_no >= script_frame) {
        *code = script_code; *value = script_value;
        script_frame = -1;
        return 1;
    }
    return 0;
#endif
}

void m2d_quit(void)
{
#ifndef M2D_HOST
    if (threaded) {
        pthread_mutex_lock(&mtx);
        while (job >= 0) pthread_cond_wait(&cv, &mtx);
        quitting = 1;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mtx);
        pthread_join(pthr, NULL);
    }
    MI_SYS_MemsetPa(fb_phy, 0, fb_w * fb_h * 4 * 2);
    vinfo.yoffset = 0;
    ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
    for (int i = 0; i < 2; i++) {
        MI_SYS_Munmap(bufs[i].px, back.stride * back.h);
        MI_SYS_MMA_Free(bufs[i].phy);
    }
    if (in_fd >= 0) close(in_fd);
    close(fb_fd);
    MI_GFX_Close();
    MI_SYS_Exit();
#else
    free(bufs[0].px); free(bufs[1].px);
#endif
}
