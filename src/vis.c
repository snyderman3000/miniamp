// Spectrum analysis and the full-screen visualizer presets (feedback effects
// in the spirit of classic music-player visualizers, written from scratch).
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vis.h"
#include "audio.h"

// ---------------------------------------------------------------- FFT
static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.f * (float)M_PI / len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                float ur = re[i + k], ui = im[i + k];
                float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

static void make_bars(const float *mag, float *bars, float *peaks, int n, float dt)
{
    // log-spaced bands from ~40 Hz to 16 kHz
    const float f0 = 40.f, f1 = 16000.f, binhz = (float)OUT_RATE / VIS_FFT;
    for (int i = 0; i < n; i++) {
        float a = f0 * powf(f1 / f0, (float)i / n), b = f0 * powf(f1 / f0, (float)(i + 1) / n);
        int lo = (int)(a / binhz), hi = (int)(b / binhz);
        if (hi <= lo) hi = lo + 1;
        if (hi > VIS_FFT / 2) hi = VIS_FFT / 2;
        float m = 0;
        for (int k = lo; k < hi; k++) if (mag[k] > m) m = mag[k];
        // tilt so highs are visible like on classic players
        m *= 1.f + 0.6f * i / n;
        if (m > 1) m = 1;
        float fall = dt * 2.2f;
        bars[i] = m > bars[i] ? m : (bars[i] - fall > m ? bars[i] - fall : m);
        float pf = dt * 0.7f;
        peaks[i] = bars[i] > peaks[i] ? bars[i] : (peaks[i] - pf > 0 ? peaks[i] - pf : 0);
    }
}

void vis_analyze(vis_data *v, float dt, int playing)
{
    v->t += dt;
    v->beat = 0;
    if (playing) audio_get_wave(v->wave, VIS_FFT);
    else for (int i = 0; i < VIS_FFT; i++) v->wave[i] *= 0.8f;
    static float re[VIS_FFT], im[VIS_FFT], win[VIS_FFT];
    static int init;
    if (!init) { for (int i = 0; i < VIS_FFT; i++) win[i] = 0.5f - 0.5f * cosf(2.f * (float)M_PI * i / (VIS_FFT - 1)); init = 1; }
    for (int i = 0; i < VIS_FFT; i++) { re[i] = v->wave[i] * win[i]; im[i] = 0; }
    fft(re, im, VIS_FFT);
    float bass = 0, level = 0;
    for (int k = 0; k < VIS_FFT / 2; k++) {
        float m = sqrtf(re[k] * re[k] + im[k] * im[k]) / (VIS_FFT / 4);
        float db = 20.f * log10f(m + 1e-6f);      // ~ -120 .. 0
        float y = (db + 60.f) / 60.f;             // 60 dB range
        v->mag[k] = y < 0 ? 0 : y > 1 ? 1 : y;
        if (k >= 1 && k <= 6) bass += m;
    }
    for (int i = 0; i < VIS_FFT; i++) level += v->wave[i] * v->wave[i];
    level = sqrtf(level / VIS_FFT) * 3.f;
    bass = bass * 1.5f;
    if (bass > 1) bass = 1;
    if (level > 1) level = 1;
    static float avg;
    static double last_beat;
    if (bass > avg * 1.35f && bass > 0.12f && v->t - last_beat > 0.25) { v->beat = 1; last_beat = v->t; }
    avg = avg * 0.95f + bass * 0.05f;
    v->bass = v->bass * 0.6f + bass * 0.4f;
    v->level = v->level * 0.7f + level * 0.3f;
    make_bars(v->mag, v->bars19, v->peaks19, 19, dt);
    make_bars(v->mag, v->bars40, v->peaks40, 40, dt);
    for (int x = 0; x < 76; x++) {
        float s = v->wave[VIS_FFT - 76 * 4 + x * 4] * 2.f;
        v->scope76[x] = s < -1 ? -1 : s > 1 ? 1 : s;
    }
}

// ---------------------------------------------------------------- drawing helpers
static uint32_t buf[2][VIS_W * VIS_H];
static int cur;
static int *maps[VIS_PRESETS];

static inline uint32_t fade_px(uint32_t c, uint32_t f)
{
    uint32_t rb = ((c & 0x00ff00ff) * f >> 8) & 0x00ff00ff;
    uint32_t g = ((c & 0x0000ff00) * f >> 8) & 0x0000ff00;
    return 0xff000000u | rb | g;
}

static inline void add_px(uint32_t *p, uint32_t c)
{
    uint32_t d = *p;
    uint32_t r = ((d >> 16) & 255) + ((c >> 16) & 255);
    uint32_t g = ((d >> 8) & 255) + ((c >> 8) & 255);
    uint32_t b = (d & 255) + (c & 255);
    *p = 0xff000000u | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

static inline uint32_t scale_col(uint32_t c, float k)
{
    if (k <= 0) return 0;
    if (k > 1) k = 1;
    return fade_px(c, (uint32_t)(k * 256));
}

static void dot(uint32_t *b, int x, int y, uint32_t c)
{
    if (x < 1 || y < 1 || x >= VIS_W - 1 || y >= VIS_H - 1) return;
    uint32_t *p = b + y * VIS_W + x;
    add_px(p, c);
    uint32_t h = scale_col(c, 0.35f);
    add_px(p - 1, h); add_px(p + 1, h); add_px(p - VIS_W, h); add_px(p + VIS_W, h);
}

static void line(uint32_t *b, float x0, float y0, float x1, float y1, uint32_t c)
{
    float dx = x1 - x0, dy = y1 - y0;
    int n = (int)(fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy)) + 1;
    if (n > 400) n = 400;
    for (int i = 0; i <= n; i++) dot(b, (int)(x0 + dx * i / n), (int)(y0 + dy * i / n), c);
}

static uint32_t hsv(float h, float s, float v)
{
    h = fmodf(h, 1.f);
    if (h < 0) h += 1;
    float r, g, b, f = h * 6;
    int i = (int)f;
    f -= i;
    float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return 0xff000000u | (uint32_t)(r * 255) << 16 | (uint32_t)(g * 255) << 8 | (uint32_t)(b * 255);
}

// displacement maps: for each pixel, which pixel of the previous frame it takes
static int *build_map(int preset)
{
    int *m = malloc(sizeof(int) * VIS_W * VIS_H);
    const float cx = VIS_W / 2.f, cy = VIS_H / 2.f;
    for (int y = 0; y < VIS_H; y++) {
        for (int x = 0; x < VIS_W; x++) {
            float dx = x - cx, dy = y - cy, sx = x, sy = y;
            if (preset == 0) {          // slow zoom in + twist
                float a = 0.012f, z = 0.965f;
                sx = cx + (dx * cosf(a) - dy * sinf(a)) * z;
                sy = cy + (dx * sinf(a) + dy * cosf(a)) * z;
            } else if (preset == 1) {   // rise with a wobble (flames)
                sx = x + sinf(y * 0.11f) * 1.2f;
                sy = y + 2.2f;
            } else if (preset == 2) {   // zoom out: things rush towards you
                float r = sqrtf(dx * dx + dy * dy);
                float z = 0.94f + 0.0002f * r;
                sx = cx + dx * z;
                sy = cy + dy * z;
            } else if (preset == 4) {   // kaleidoscope swirl
                float r = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx);
                a += 0.03f * (1.f - r / 200.f);
                r *= 0.985f;
                sx = cx + r * cosf(a);
                sy = cy + r * sinf(a);
            }
            int ix = (int)(sx + 0.5f), iy = (int)(sy + 0.5f);
            if (ix < 0) ix = 0;
            if (ix >= VIS_W) ix = VIS_W - 1;
            if (iy < 0 || iy >= VIS_H) { m[y * VIS_W + x] = -1; continue; }
            m[y * VIS_W + x] = iy * VIS_W + ix;
        }
    }
    return m;
}

static void feedback(int preset, uint32_t fade)
{
    if (!maps[preset]) maps[preset] = build_map(preset);
    const int *m = maps[preset];
    const uint32_t *src = buf[cur];
    uint32_t *dst = buf[cur ^ 1];
    for (int i = 0; i < VIS_W * VIS_H; i++) {
        int k = m[i];
        dst[i] = k < 0 ? 0xff000000u : fade_px(src[k], fade);
    }
    cur ^= 1;
}

const char *vis_preset_name(int i)
{
    static const char *names[VIS_PRESETS] = { "Tidal Ring", "Spectrum Fire", "Scope Tunnel", "Classic Bars", "Kaleido" };
    return names[i % VIS_PRESETS];
}

// ---------------------------------------------------------------- presets
static void p_ring(const vis_data *v, uint32_t *b)
{
    float cx = VIS_W / 2.f, cy = VIS_H / 2.f;
    float r0 = 48 + v->bass * 40;
    uint32_t c = hsv(0.08f + 0.06f * sinf((float)v->t * 0.2f) + (v->beat ? 0.5f : 0), 0.75f, 0.9f);
    float px = 0, py = 0;
    for (int i = 0; i <= 256; i++) {
        float a = 2.f * (float)M_PI * i / 256;
        float s = v->wave[(i % 256) * 4];
        float r = r0 + s * 40;
        float x = cx + r * cosf(a), y = cy + r * sinf(a);
        if (i) line(b, px, py, x, y, c);
        px = x; py = y;
    }
}

static void p_fire(const vis_data *v, uint32_t *b)
{
    for (int i = 0; i < 40; i++) {
        int h = (int)(v->bars40[i] * 110);
        int x0 = i * 8;
        for (int y = 0; y < h; y++) {
            float t = (float)y / 110;
            uint32_t c = hsv(0.13f - 0.13f * t, 0.9f, 1.f - 0.4f * t);
            uint32_t *row = b + (VIS_H - 1 - y) * VIS_W + x0;
            for (int x = 1; x < 7; x++) row[x] = c;
        }
    }
}

static void p_tunnel(const vis_data *v, uint32_t *b)
{
    uint32_t c = hsv((float)v->t * 0.05f, 0.7f, 1.f);
    float px = 0, py = 0;
    for (int x = 0; x < VIS_W; x += 2) {
        float s = v->wave[VIS_FFT - VIS_W * 2 + x * 2];
        float y = VIS_H / 2.f + s * 70;
        if (x) line(b, px, py, (float)x, y, c);
        px = (float)x; py = y;
    }
    if (v->beat) for (int i = 0; i < 64; i++) {
        float a = 2.f * (float)M_PI * i / 64;
        dot(b, (int)(VIS_W / 2 + 20 * cosf(a)), (int)(VIS_H / 2 + 20 * sinf(a)), hsv((float)v->t * 0.05f + 0.5f, 0.6f, 1.f));
    }
}

static void p_bars(const vis_data *v, uint32_t *b, const uint32_t *sv)
{
    uint32_t bg = sv[0], dots = sv[1];
    for (int i = 0; i < VIS_W * VIS_H; i++) b[i] = bg;
    for (int y = 3; y < VIS_H; y += 6) for (int x = 3; x < VIS_W; x += 6) b[y * VIS_W + x] = dots;
    const int top = 20, H = 200;
    for (int i = 0; i < 40; i++) {
        int h = (int)(v->bars40[i] * H);
        int x0 = 4 + i * 8;
        for (int y = 0; y < h; y++) {
            int row = top + H - 1 - y;
            int ci = 2 + (row - top) * 16 / H;
            uint32_t c = sv[ci];
            for (int x = 0; x < 6; x++) b[row * VIS_W + x0 + x] = c;
        }
        int p = (int)(v->peaks40[i] * H);
        if (p > 0) {
            int row = top + H - 1 - p;
            if (row >= top) for (int yy = row; yy < row + 2; yy++) for (int x = 0; x < 6; x++) b[yy * VIS_W + x0 + x] = sv[23];
        }
    }
}

static void p_kaleido(const vis_data *v, uint32_t *b)
{
    float cx = VIS_W / 2.f, cy = VIS_H / 2.f;
    int q = VIS_FFT / 4;
    float hue = (float)v->t * 0.03f;
    for (int i = 0; i < 300; i++) {
        float x = v->wave[VIS_FFT - 600 + i * 2], y = v->wave[VIS_FFT - 600 + i * 2 - q / 4];
        float sx = x * 90, sy = y * 90;
        uint32_t c = hsv(hue + i / 900.f, 0.8f, 0.55f);
        dot(b, (int)(cx + sx), (int)(cy + sy), c);
        dot(b, (int)(cx - sx), (int)(cy + sy), c);
        dot(b, (int)(cx + sx), (int)(cy - sy), c);
        dot(b, (int)(cx - sx), (int)(cy - sy), c);
    }
}

void vis_render(int preset, const vis_data *v, m2d_surf *out, const uint32_t *skin_vis)
{
    preset %= VIS_PRESETS;
    uint32_t *b;
    switch (preset) {
    case 0: feedback(0, 236); b = buf[cur]; p_ring(v, b); break;
    case 1: feedback(1, 228); b = buf[cur]; p_fire(v, b); break;
    case 2: feedback(2, 232); b = buf[cur]; p_tunnel(v, b); break;
    case 3: b = buf[cur]; p_bars(v, b, skin_vis); break;
    default: feedback(4, 240); b = buf[cur]; p_kaleido(v, b); break;
    }
    memcpy(out->px, b, sizeof(uint32_t) * VIS_W * VIS_H);
}
