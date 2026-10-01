// Classic skin loading (.wsz) and main window drawing.
// Sprite coordinates follow the classic skin format as documented by the
// Webamp project (MIT): https://github.com/captbaritone/webamp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_BMP
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "skin.h"
#include "zip.h"
#include "audio.h"
#include "meta.h"

static const char *SHEET_FILE[SK_COUNT] = {
    "main.bmp", "cbuttons.bmp", "titlebar.bmp", "text.bmp", "numbers.bmp", "playpaus.bmp",
    "monoster.bmp", "posbar.bmp", "volume.bmp", "balance.bmp", "shufrep.bmp" };

// ---------------------------------------------------------------- file source
typedef struct { zip_archive *zip; char dir[512]; } source;

static unsigned char *src_read(source *s, const char *name, size_t *len)
{
    if (s->zip) return zip_extract(s->zip, zip_find(s->zip, name), len);
    DIR *d = opendir(s->dir);
    if (!d) return NULL;
    struct dirent *e;
    char path[1024] = {0};
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, name)) { snprintf(path, sizeof path, "%s/%s", s->dir, e->d_name); break; }
    }
    closedir(d);
    if (!path[0]) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = n > 0 && n < 32L * 1024 * 1024 ? malloc(n + 1) : NULL;
    if (b && fread(b, 1, n, f) == (size_t)n) { b[n] = 0; *len = n; }
    else { free(b); b = NULL; }
    fclose(f);
    return b;
}

// ---------------------------------------------------------------- bitmaps
static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

// RLE4/RLE8 bitmaps (some older skins use them; stb_image does not)
static unsigned char *bmp_rle(const unsigned char *b, size_t len, int *w, int *h)
{
    if (len < 54 || b[0] != 'B' || b[1] != 'M') return NULL;
    unsigned off = rd32(b + 10), hs = rd32(b + 14);
    int W = (int)rd32(b + 18), H = (int)rd32(b + 22);
    int bpp = rd16(b + 28), comp = rd32(b + 30);
    if (!((comp == 1 && bpp == 8) || (comp == 2 && bpp == 4)) || W <= 0 || W > 4096 || H == 0 || H > 4096 || H < -4096) return NULL;
    int flip = H > 0;
    if (H < 0) H = -H;
    unsigned ncol = rd32(b + 46);
    if (!ncol) ncol = 1u << bpp;
    const unsigned char *pal = b + 14 + hs;
    if (14 + hs + ncol * 4 > len || off >= len) return NULL;
    unsigned char *idx = calloc((size_t)W * H, 1);
    const unsigned char *p = b + off, *end = b + len;
    int x = 0, y = 0;
    while (p + 1 < end && y < H) {
        int n = p[0], v = p[1];
        p += 2;
        if (n) {
            for (int i = 0; i < n && x < W; i++, x++)
                idx[y * W + x] = bpp == 8 ? v : (i & 1 ? v & 15 : v >> 4);
        } else if (v == 0) { x = 0; y++; }
        else if (v == 1) break;
        else if (v == 2) { if (p + 1 >= end) break; x += p[0]; y += p[1]; p += 2; }
        else {
            int bytes = bpp == 8 ? v : (v + 1) / 2;
            for (int i = 0; i < v && x < W; i++, x++) {
                int bi = bpp == 8 ? i : i / 2;
                if (p + bi >= end) break;
                idx[y * W + x] = bpp == 8 ? p[bi] : (i & 1 ? p[bi] & 15 : p[bi] >> 4);
            }
            p += (bytes + 1) & ~1;
        }
    }
    unsigned char *rgba = malloc((size_t)W * H * 4);
    for (int yy = 0; yy < H; yy++) {
        int sy = flip ? H - 1 - yy : yy;
        for (int xx = 0; xx < W; xx++) {
            unsigned c = idx[sy * W + xx];
            const unsigned char *q = c < ncol ? pal + c * 4 : pal;
            unsigned char *o = rgba + ((size_t)yy * W + xx) * 4;
            o[0] = q[2]; o[1] = q[1]; o[2] = q[0]; o[3] = 255;
        }
    }
    free(idx);
    *w = W; *h = H;
    return rgba;
}

static m2d_surf *load_sheet(source *src, const char *name)
{
    size_t len = 0;
    unsigned char *data = src_read(src, name, &len);
    if (!data) return NULL;
    int w = 0, h = 0, n = 0;
    unsigned char *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &n, 4);
    int from_stb = rgba != NULL;
    if (!rgba) rgba = bmp_rle(data, len, &w, &h);
    free(data);
    if (!rgba) return NULL;
    for (int i = 0; i < w * h; i++) rgba[i * 4 + 3] = 255;  // skins have no transparency
    m2d_surf *s = m2d_surface_new(w, h);
    if (s) m2d_surface_upload_rgba(s, rgba, w, h);
    if (from_stb) stbi_image_free(rgba); else free(rgba);
    return s;
}

// ---------------------------------------------------------------- text files
static uint32_t argb(int r, int g, int b) { return 0xff000000u | (r & 255) << 16 | (g & 255) << 8 | (b & 255); }

static int parse_viscolor(const char *t, uint32_t *out)
{
    int n = 0;
    while (*t && n < 24) {
        int v[3], k = 0;
        const char *line_end = strchr(t, '\n');
        if (!line_end) line_end = t + strlen(t);
        const char *p = t;
        while (p < line_end && k < 3) {
            if (p[0] == '/' && p[1] == '/') break;
            if (isdigit((unsigned char)*p)) { v[k++] = (int)strtol(p, (char **)&p, 10); continue; }
            p++;
        }
        if (k == 3) out[n++] = argb(v[0], v[1], v[2]);
        t = *line_end ? line_end + 1 : line_end;
    }
    return n;
}

static int parse_color(const char *v, uint32_t *out)
{
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '#') v++;
    char hex[7] = {0};
    int k = 0;
    while (k < 6 && isxdigit((unsigned char)v[k])) { hex[k] = v[k]; k++; }
    if (k != 6) return 0;
    *out = 0xff000000u | (uint32_t)strtoul(hex, NULL, 16);
    return 1;
}

static void parse_pledit(const char *t, skin *sk)
{
    char line[256];
    while (*t) {
        int n = 0;
        while (*t && *t != '\n' && n < 255) line[n++] = *t++;
        while (*t && *t != '\n') t++;
        if (*t) t++;
        line[n] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line;
        while (*k == ' ') k++;
        char *ke = eq - 1;
        while (ke > k && *ke == ' ') *ke-- = 0;
        if (!strcasecmp(k, "Normal")) parse_color(eq + 1, &sk->pl_normal);
        else if (!strcasecmp(k, "Current")) parse_color(eq + 1, &sk->pl_current);
        else if (!strcasecmp(k, "NormalBG")) parse_color(eq + 1, &sk->pl_bg);
        else if (!strcasecmp(k, "SelectedBG")) parse_color(eq + 1, &sk->pl_selbg);
    }
}

// ---------------------------------------------------------------- load
skin *skin_load(const char *path, const skin *fb)
{
    source src;
    memset(&src, 0, sizeof src);
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    if (S_ISDIR(st.st_mode)) snprintf(src.dir, sizeof src.dir, "%s", path);
    else if (!(src.zip = zip_open_file(path))) return NULL;

    skin *sk = calloc(1, sizeof *sk);
    int found = 0;
    for (int i = 0; i < SK_COUNT; i++) {
        m2d_surf *s = NULL;
        if (i == SK_NUMBERS) {
            s = load_sheet(&src, "nums_ex.bmp");
            if (s) sk->nums_ex = 1;
        }
        if (!s) s = load_sheet(&src, SHEET_FILE[i]);
        if (!s && i == SK_BALANCE) s = load_sheet(&src, "volume.bmp");  // classic fallback
        if (s) { sk->s[i] = s; sk->own[i] = 1; found++; }
        else if (fb) {
            sk->s[i] = fb->s[i];
            if (i == SK_NUMBERS) sk->nums_ex = fb->nums_ex;
        }
    }
    if (fb) {
        memcpy(sk->vis, fb->vis, sizeof sk->vis);
        sk->pl_normal = fb->pl_normal; sk->pl_current = fb->pl_current;
        sk->pl_bg = fb->pl_bg; sk->pl_selbg = fb->pl_selbg;
    } else {
        for (int i = 0; i < 24; i++) sk->vis[i] = argb(0, 200 - i * 6, 0);
        sk->vis[0] = argb(0, 0, 0);
        sk->pl_normal = argb(0, 255, 0); sk->pl_current = argb(255, 255, 255);
        sk->pl_bg = argb(0, 0, 0); sk->pl_selbg = argb(0, 0, 198);
    }
    size_t len;
    char *t = (char *)src_read(&src, "viscolor.txt", &len);
    if (t) { uint32_t v[24]; int n = parse_viscolor(t, v); memcpy(sk->vis, v, n * sizeof(uint32_t)); free(t); }
    t = (char *)src_read(&src, "pledit.txt", &len);
    if (t) { parse_pledit(t, sk); free(t); }
    zip_close(src.zip);

    if (!found || (!sk->s[SK_MAIN])) { skin_free(sk); return NULL; }
    for (int i = 0; i < SK_COUNT; i++) if (!sk->s[i]) { skin_free(sk); return NULL; }

    const char *b = strrchr(path, '/');
    snprintf(sk->name, sizeof sk->name, "%s", b ? b + 1 : path);
    char *dot = strrchr(sk->name, '.');
    if (dot && !strcasecmp(dot, ".wsz")) *dot = 0;
    for (char *p = sk->name; *p; p++) if (*p == '_') *p = ' ';
    return sk;
}

void skin_free(skin *sk)
{
    if (!sk) return;
    for (int i = 0; i < SK_COUNT; i++) if (sk->own[i]) m2d_surface_free(sk->s[i]);
    free(sk);
}

// ---------------------------------------------------------------- drawing
static void spr(m2d_surf *dst, const m2d_surf *sh, int sx, int sy, int w, int h, int dx, int dy)
{
    if (!sh || sx >= sh->w || sy >= sh->h) return;
    if (sx + w > sh->w) w = sh->w - sx;
    if (sy + h > sh->h) h = sh->h - sy;
    if (w <= 0 || h <= 0) return;
    m2d_blit((m2d_surf *)sh, sx, sy, w, h, dst, dx, dy, w, h, 0xffffffffu, M2D_REPLACE, 0);
}

// text.bmp layout: [row, col] of 5x6 cells
static signed char font_row[256], font_col[256];
static void font_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    for (int i = 0; i < 256; i++) { font_row[i] = 0; font_col[i] = 30; }
    for (int c = 'a'; c <= 'z'; c++) { font_row[c] = 0; font_col[c] = c - 'a'; font_row[toupper(c)] = 0; font_col[toupper(c)] = c - 'a'; }
    const char *r0 = "\"@";
    for (int i = 0; r0[i]; i++) { font_row[(unsigned char)r0[i]] = 0; font_col[(unsigned char)r0[i]] = 26 + i; }
    const char *r1 = "0123456789\x01.:()-'!_+\\/[]^&%,=$#";
    for (int i = 0; r1[i]; i++) { font_row[(unsigned char)r1[i]] = 1; font_col[(unsigned char)r1[i]] = i; }
    font_row['<'] = 1; font_col['<'] = 22; font_row['>'] = 1; font_col['>'] = 23;
    font_row['{'] = 1; font_col['{'] = 22; font_row['}'] = 1; font_col['}'] = 23;
    font_row['?'] = 2; font_col['?'] = 3; font_row['*'] = 2; font_col['*'] = 4;
    font_row['`'] = 1; font_col['`'] = 16;
    font_row[';'] = 1; font_col[';'] = 12;
    font_row['|'] = 1; font_col['|'] = 12;
    font_row['~'] = 1; font_col['~'] = 15;
}

// folds a codepoint to something text.bmp can show
static int fold(int cp)
{
    if (cp < 128) return cp;
    if (cp == 0xC5 || cp == 0xE5) return 0x100;  // Å
    if (cp == 0xD6 || cp == 0xF6) return 0x101;  // Ö
    if (cp == 0xC4 || cp == 0xE4) return 0x102;  // Ä
    if (cp == 0x2026) return 1;                  // …
    if (cp == 0x2018 || cp == 0x2019) return '\'';
    if (cp == 0x201C || cp == 0x201D) return '"';
    if (cp == 0x2013 || cp == 0x2014) return '-';
    static const char *lat = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    if (cp >= 0xC0 && cp <= 0xFF) return lat[cp - 0xC0];
    return ' ';
}

int skin_text_width(const char *s)
{
    int n = 0;
    while (*s) { if (utf8_next(&s)) n++; }
    return n * 5;
}

int skin_text(const skin *sk, m2d_surf *dst, int x, int y, const char *s, int cx0, int cx1)
{
    font_init();
    const m2d_surf *t = sk->s[SK_TEXT];
    int x0 = x;
    while (*s) {
        int cp = fold(utf8_next(&s));
        int row, col;
        if (cp >= 0x100) { row = 2; col = cp - 0x100; }
        else { row = font_row[cp & 255]; col = font_col[cp & 255]; }
        if (x + 5 > cx0 && x < cx1) {
            int sx = col * 5, w = 5, dx = x;
            if (dx < cx0) { sx += cx0 - dx; w -= cx0 - dx; dx = cx0; }
            if (dx + w > cx1) w = cx1 - dx;
            if (w > 0) spr(dst, t, sx, row * 6, w, 6, dx, y);
        }
        x += 5;
    }
    return x - x0;
}

static void digit(const skin *sk, m2d_surf *dst, int d, int x, int y)
{
    spr(dst, sk->s[SK_NUMBERS], d * 9, 0, 9, 13, x, y);
}

static void vis_draw(const skin *sk, m2d_surf *dst, const mainwin_state *st)
{
    const int X = 24, Y = 43, W = 76, H = 16;
    m2d_fill(dst, X, Y, W, H, sk->vis[0], M2D_REPLACE);
    for (int y = 1; y < H; y += 2)
        for (int x = 1; x < W; x += 2) dst->px[(Y + y) * dst->w + X + x] = sk->vis[1];
    if (st->vis_mode == 1 && st->bars) {
        for (int i = 0; i < 19; i++) {
            int h = (int)(st->bars[i] * H + 0.5f);
            if (h > H) h = H;
            for (int r = H - h; r < H; r++) m2d_fill(dst, X + i * 4, Y + r, 3, 1, sk->vis[2 + r], M2D_REPLACE);
            if (st->peaks) {
                int p = (int)(st->peaks[i] * H + 0.5f);
                if (p > 0) { int r = H - p; if (r < 0) r = 0; m2d_fill(dst, X + i * 4, Y + r, 3, 1, sk->vis[23], M2D_REPLACE); }
            }
        }
    } else if (st->vis_mode == 2 && st->scope) {
        int prev = -1;
        for (int x = 0; x < W; x++) {
            int y = (int)(8 + st->scope[x] * 8);
            if (y < 0) y = 0;
            if (y > H - 1) y = H - 1;
            int a = prev < 0 ? y : prev, b = y;
            if (a > b) { int t = a; a = b; b = t; }
            for (int yy = a; yy <= b; yy++) {
                int d = yy - 8; if (d < 0) d = -d - 1;
                int ci = 18 + (d / 2 > 4 ? 4 : d / 2);
                dst->px[(Y + yy) * dst->w + X + x] = sk->vis[ci];
            }
            prev = y;
        }
    }
}

void skin_draw_main(const skin *sk, m2d_surf *dst, const mainwin_state *st)
{
    m2d_surf *const *S = sk->s;
    spr(dst, S[SK_MAIN], 0, 0, MW_W, MW_H, 0, 0);
    spr(dst, S[SK_TITLEBAR], 27, 0, MW_W, 14, 0, 0);
    spr(dst, S[SK_TITLEBAR], 304, 0, 8, 43, 10, 22);  // clutter bar

    // play state indicator
    if (st->state == A_PLAYING) { spr(dst, S[SK_PLAYPAUS], 0, 0, 9, 9, 26, 28); spr(dst, S[SK_PLAYPAUS], 36, 0, 3, 9, 24, 28); }
    else if (st->state == A_PAUSED) spr(dst, S[SK_PLAYPAUS], 9, 0, 9, 9, 26, 28);
    else spr(dst, S[SK_PLAYPAUS], 18, 0, 9, 9, 26, 28);

    // time
    if (st->state != A_STOPPED && !st->blink_off) {
        int t = st->time_s;
        if (t > 99 * 60 + 59) t = 99 * 60 + 59;
        int m = t / 60, s = t % 60;
        if (st->remaining) {
            if (sk->nums_ex) spr(dst, S[SK_NUMBERS], 99, 0, 9, 13, 36, 26);
            else spr(dst, S[SK_NUMBERS], 20, 6, 5, 1, 38, 32);
        }
        digit(sk, dst, m / 10, 48, 26);
        digit(sk, dst, m % 10, 60, 26);
        digit(sk, dst, s / 10, 78, 26);
        digit(sk, dst, s % 10, 90, 26);
    }

    vis_draw(sk, dst, st);

    // song title
    if (st->marquee) {
        int tw = skin_text_width(st->marquee);
        if (tw <= 154) skin_text(sk, dst, 111, 27, st->marquee, 111, 265);
        else {
            int period = tw;
            int off = st->marquee_px % period;
            skin_text(sk, dst, 111 - off, 27, st->marquee, 111, 265);
            skin_text(sk, dst, 111 - off + period, 27, st->marquee, 111, 265);
        }
    }

    if (st->state != A_STOPPED && st->kbps > 0) {
        char b[8];
        int k = st->kbps > 999 ? 999 : st->kbps;
        snprintf(b, sizeof b, "%3d", k);
        skin_text(sk, dst, 111, 43, b, 111, 126);
        snprintf(b, sizeof b, "%2d", st->khz > 99 ? 99 : st->khz);
        skin_text(sk, dst, 156, 43, b, 156, 166);
    }
    int stereo = st->state != A_STOPPED && st->channels >= 2;
    int mono = st->state != A_STOPPED && st->channels == 1;
    spr(dst, S[SK_MONOSTER], 29, mono ? 0 : 12, 27, 12, 212, 41);
    spr(dst, S[SK_MONOSTER], 0, stereo ? 0 : 12, 29, 12, 239, 41);

    // volume + balance
    {
        int fr = (int)(st->volume * 28 / 100.f + 0.5f) - 1;
        if (fr < 0) fr = 0;
        if (fr > 27) fr = 27;
        spr(dst, S[SK_VOLUME], 0, fr * 15, 68, 13, 107, 57);
        if (S[SK_VOLUME]->h >= 433) spr(dst, S[SK_VOLUME], 15, 422, 14, 11, 107 + st->volume * 54 / 100, 58);
        int b = st->balance < 0 ? -st->balance : st->balance;
        int bf = b * 27 / 100;
        spr(dst, S[SK_BALANCE], 9, bf * 15, 38, 13, 177, 57);
        if (S[SK_BALANCE]->h >= 433) spr(dst, S[SK_BALANCE], 15, 422, 14, 11, 177 + (st->balance + 100) * 24 / 200, 58);
    }
    // EQ / PL toggles
    spr(dst, S[SK_SHUFREP], (st->pressed == PB_EQ ? 46 : 0), st->eq_on ? 73 : 61, 23, 12, 219, 58);
    spr(dst, S[SK_SHUFREP], 23 + (st->pressed == PB_PL ? 46 : 0), st->pl_on ? 73 : 61, 23, 12, 242, 58);

    // position bar
    spr(dst, S[SK_POSBAR], 0, 0, 248, 10, 16, 72);
    if (st->pos >= 0 && st->state != A_STOPPED) {
        float p = st->pos > 1 ? 1 : st->pos;
        spr(dst, S[SK_POSBAR], 248, 0, 29, 10, 16 + (int)(p * (248 - 29)), 72);
    }

    // transport buttons
    static const int bx[5] = { 16, 39, 62, 85, 108 }, bw[5] = { 23, 23, 23, 23, 22 };
    for (int i = 0; i < 5; i++) {
        int pr = st->pressed == PB_PREV + i;
        spr(dst, S[SK_CBUTTONS], i * 23, pr ? 18 : 0, bw[i], 18, bx[i], 88);
    }
    spr(dst, S[SK_CBUTTONS], 114, st->pressed == PB_EJECT ? 16 : 0, 22, 16, 136, 89);
    spr(dst, S[SK_SHUFREP], 28, (st->shuffle ? 30 : 0) + (st->pressed == PB_SHUFFLE ? 15 : 0), 47, 15, 164, 89);
    spr(dst, S[SK_SHUFREP], 0, (st->repeat ? 30 : 0) + (st->pressed == PB_REPEAT ? 15 : 0), 28, 15, 210, 89);
}
