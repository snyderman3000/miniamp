#ifndef FONT_H
#define FONT_H
#include <stdint.h>
#include "mini2d.h"

typedef struct font font;

// px is the em size in pixels; glyphs missing from the font come from `fallback_path`
font *font_open(const char *path, const char *fallback_path, int px);
int font_draw(m2d_surf *d, font *f, int x, int y, const char *utf8, uint32_t argb);  // y = top of line
int font_width(font *f, const char *utf8);
int font_line_height(font *f);
int font_ascent(font *f);
// draws text vertically centred on cy
int font_draw_mid(m2d_surf *d, font *f, int x, int cy, const char *utf8, uint32_t argb);
// copies s into out, cut with "…" so it fits maxw pixels
void font_fit(font *f, const char *s, int maxw, char *out, int n);
// same, but keeps the end of the string ("…/Music/Albums")
void font_fit_left(font *f, const char *s, int maxw, char *out, int n);

#endif
