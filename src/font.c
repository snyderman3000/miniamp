// TrueType text via stb_truetype, with a per-size glyph cache of small surfaces.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "font.h"
#include "meta.h"

typedef struct { unsigned char *data; stbtt_fontinfo info; char path[512]; } ttf;

typedef struct { int cp; m2d_surf *s; int xoff, yoff, adv; } glyph;

struct font {
    ttf *main, *fb;
    float scale_main, scale_fb;
    int ascent, line_h;
    glyph *cache;
    int cap, used;
};

static ttf *ttfs[8];
static int nttf;

static ttf *ttf_get(const char *path)
{
    if (!path) return NULL;
    for (int i = 0; i < nttf; i++) if (!strcmp(ttfs[i]->path, path)) return ttfs[i];
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    ttf *t = calloc(1, sizeof *t);
    t->data = malloc(n);
    if (fread(t->data, 1, n, f) != (size_t)n || !stbtt_InitFont(&t->info, t->data, stbtt_GetFontOffsetForIndex(t->data, 0))) {
        fclose(f); free(t->data); free(t); return NULL;
    }
    fclose(f);
    snprintf(t->path, sizeof t->path, "%s", path);
    if (nttf < 8) ttfs[nttf++] = t;
    return t;
}

font *font_open(const char *path, const char *fallback_path, int px)
{
    ttf *m = ttf_get(path);
    if (!m) { fprintf(stderr, "[font] cannot load %s\n", path); return NULL; }
    font *f = calloc(1, sizeof *f);
    f->main = m;
    f->fb = ttf_get(fallback_path);
    f->scale_main = stbtt_ScaleForMappingEmToPixels(&m->info, (float)px);
    if (f->fb) f->scale_fb = stbtt_ScaleForMappingEmToPixels(&f->fb->info, (float)px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&m->info, &asc, &desc, &gap);
    f->ascent = (int)ceilf(asc * f->scale_main);
    f->line_h = (int)ceilf((asc - desc) * f->scale_main);
    f->cap = 256;
    f->cache = calloc(f->cap, sizeof(glyph));
    for (int i = 0; i < f->cap; i++) f->cache[i].cp = -1;
    return f;
}

static glyph *lookup(font *f, int cp);

static void grow(font *f)
{
    glyph *old = f->cache;
    int oc = f->cap;
    f->cap *= 2;
    f->cache = calloc(f->cap, sizeof(glyph));
    for (int i = 0; i < f->cap; i++) f->cache[i].cp = -1;
    f->used = 0;
    for (int i = 0; i < oc; i++) {
        if (old[i].cp < 0) continue;
        unsigned h = (unsigned)old[i].cp * 2654435761u;
        int k = h & (f->cap - 1);
        while (f->cache[k].cp >= 0) k = (k + 1) & (f->cap - 1);
        f->cache[k] = old[i];
        f->used++;
    }
    free(old);
}

static glyph *lookup(font *f, int cp)
{
    unsigned h = (unsigned)cp * 2654435761u;
    int k = h & (f->cap - 1);
    while (f->cache[k].cp >= 0) {
        if (f->cache[k].cp == cp) return &f->cache[k];
        k = (k + 1) & (f->cap - 1);
    }
    if (f->used * 2 >= f->cap) { grow(f); return lookup(f, cp); }
    glyph *g = &f->cache[k];
    g->cp = cp;
    f->used++;
    ttf *t = f->main;
    float sc = f->scale_main;
    int gi = stbtt_FindGlyphIndex(&t->info, cp);
    if (!gi && f->fb) {
        int g2 = stbtt_FindGlyphIndex(&f->fb->info, cp);
        if (g2) { t = f->fb; sc = f->scale_fb; gi = g2; }
    }
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&t->info, gi, &adv, &lsb);
    g->adv = (int)lrintf(adv * sc);
    int w, h2, xo, yo;
    unsigned char *bm = stbtt_GetGlyphBitmap(&t->info, sc, sc, gi, &w, &h2, &xo, &yo);
    if (bm && w > 0 && h2 > 0) {
        g->s = m2d_surface_new(w, h2);
        for (int i = 0; i < w * h2; i++) {
            uint32_t a = bm[i];
            g->s->px[i] = a << 24 | a << 16 | a << 8 | a;
        }
        m2d_surface_build_spans(g->s);
        g->xoff = xo;
        g->yoff = yo + f->ascent;
    }
    if (bm) stbtt_FreeBitmap(bm, NULL);
    return g;
}

static uint32_t premul(uint32_t c)
{
    uint32_t a = c >> 24;
    if (a == 255) return c;
    uint32_t r = ((c >> 16) & 255) * a / 255, g = ((c >> 8) & 255) * a / 255, b = (c & 255) * a / 255;
    return a << 24 | r << 16 | g << 8 | b;
}

int font_draw(m2d_surf *d, font *f, int x, int y, const char *s, uint32_t color)
{
    if (!f) return 0;
    uint32_t tint = premul(color);
    int x0 = x;
    while (*s) {
        int cp = utf8_next(&s);
        if (cp == '\n') continue;
        glyph *g = lookup(f, cp);
        if (g->s) m2d_blit(g->s, 0, 0, g->s->w, g->s->h, d, x + g->xoff, y + g->yoff, g->s->w, g->s->h, tint, M2D_ALPHA, 0);
        x += g->adv;
    }
    return x - x0;
}

int font_draw_mid(m2d_surf *d, font *f, int x, int cy, const char *s, uint32_t color)
{
    if (!f) return 0;
    return font_draw(d, f, x, cy - f->line_h / 2, s, color);
}

int font_width(font *f, const char *s)
{
    if (!f) return 0;
    int w = 0;
    while (*s) w += lookup(f, utf8_next(&s))->adv;
    return w;
}

int font_line_height(font *f) { return f ? f->line_h : 0; }
int font_ascent(font *f) { return f ? f->ascent : 0; }

void font_fit(font *f, const char *s, int maxw, char *out, int n)
{
    if (n < 8) { if (n > 0) out[0] = 0; return; }
    snprintf(out, n, "%s", s);
    if (font_width(f, out) <= maxw) return;
    int ell = font_width(f, "…");
    int w = 0;
    const char *p = s, *cut = s;
    while (*p) {
        const char *q = p;
        int cw = lookup(f, utf8_next(&q))->adv;
        if (w + cw + ell > maxw) break;
        w += cw;
        p = q;
        cut = p;
    }
    int len = (int)(cut - s);
    if (len > n - 4) len = n - 4;
    memcpy(out, s, len);
    // trim trailing spaces before the ellipsis
    while (len > 0 && out[len - 1] == ' ') len--;
    memcpy(out + len, "…", 4);
}

void font_fit_left(font *f, const char *s, int maxw, char *out, int n)
{
    if (font_width(f, s) <= maxw) { snprintf(out, n, "%s", s); return; }
    const char *p = s;
    while (*p) {
        utf8_next(&p);
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "…%s", p);
        if (font_width(f, tmp) <= maxw) { snprintf(out, n, "%s", tmp); return; }
    }
    snprintf(out, n, "…");
}
