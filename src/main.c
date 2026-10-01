// MiniAmp - a classic-skin music player for the Miyoo Mini Plus.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>

#include "mini2d.h"
#include "audio.h"
#include "decode.h"
#include "meta.h"
#include "skin.h"
#include "font.h"
#include "vis.h"
#include "playlist.h"

#define SCREEN_W 640
#define SCREEN_H 480

// evdev key codes of the Miyoo Mini buttons
enum { K_UP = 103, K_DOWN = 108, K_LEFT = 105, K_RIGHT = 106, K_A = 57, K_B = 29, K_X = 42, K_Y = 56,
       K_L = 18, K_R = 20, K_L2 = 15, K_R2 = 14, K_START = 28, K_SELECT = 97, K_MENU = 1, K_POWER = 116 };

enum { SCR_PLAYLIST, SCR_EQ, SCR_BROWSER, SCR_OPTIONS, SCR_SKINS, SCR_VIS, SCR_PRESETS };

// ---------------------------------------------------------------- settings
typedef struct {
    int volume, balance;
    int eq_on;
    float eq_pre, eq[EQ_BANDS];
    char eq_preset[40];
    int shuffle, repeat;          // repeat: 0 off, 1 all, 2 one
    int vis_mode;                 // main window: 1 spectrum, 2 scope, 0 off
    int vis_preset;               // full screen
    int remaining;
    char skin[512];
    char last_dir[1024];
    int cur_index, cur_pos_ms;
} settings;

static settings S = { .volume = 80, .vis_mode = 1, .cur_index = -1 };
static char app_dir[512], data_dir[512], root_dir[512];

static void path_join(char *out, int n, const char *a, const char *b) { snprintf(out, n, "%s/%s", a, b); }

static void settings_load(void)
{
    char p[1024];
    path_join(p, sizeof p, data_dir, "settings.ini");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "volume")) S.volume = atoi(v);
        else if (!strcmp(k, "balance")) S.balance = atoi(v);
        else if (!strcmp(k, "eq_on")) S.eq_on = atoi(v);
        else if (!strcmp(k, "eq_pre")) S.eq_pre = (float)atof(v);
        else if (!strcmp(k, "eq")) {
            const char *p2 = v;
            for (int i = 0; i < EQ_BANDS && *p2; i++) {
                S.eq[i] = strtof(p2, (char **)&p2);
                while (*p2 == ',' || *p2 == ' ') p2++;
            }
        }
        else if (!strcmp(k, "eq_preset")) snprintf(S.eq_preset, sizeof S.eq_preset, "%s", v);
        else if (!strcmp(k, "shuffle")) S.shuffle = atoi(v);
        else if (!strcmp(k, "repeat")) S.repeat = atoi(v);
        else if (!strcmp(k, "vis_mode")) S.vis_mode = atoi(v);
        else if (!strcmp(k, "vis_preset")) S.vis_preset = atoi(v);
        else if (!strcmp(k, "remaining")) S.remaining = atoi(v);
        else if (!strcmp(k, "skin")) snprintf(S.skin, sizeof S.skin, "%s", v);
        else if (!strcmp(k, "last_dir")) snprintf(S.last_dir, sizeof S.last_dir, "%s", v);
        else if (!strcmp(k, "cur_index")) S.cur_index = atoi(v);
        else if (!strcmp(k, "cur_pos_ms")) S.cur_pos_ms = atoi(v);
    }
    fclose(f);
}

static void settings_save(void)
{
    char p[1024], tmp[1100];
    path_join(p, sizeof p, data_dir, "settings.ini");
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "volume=%d\nbalance=%d\neq_on=%d\neq_pre=%.1f\neq=", S.volume, S.balance, S.eq_on, S.eq_pre);
    for (int i = 0; i < EQ_BANDS; i++) fprintf(f, "%s%.1f", i ? "," : "", S.eq[i]);
    fprintf(f, "\neq_preset=%s\nshuffle=%d\nrepeat=%d\nvis_mode=%d\nvis_preset=%d\nremaining=%d\nskin=%s\nlast_dir=%s\ncur_index=%d\ncur_pos_ms=%d\n",
            S.eq_preset, S.shuffle, S.repeat, S.vis_mode, S.vis_preset, S.remaining, S.skin, S.last_dir, S.cur_index, S.cur_pos_ms);
    fclose(f);
    rename(tmp, p);
}

// ---------------------------------------------------------------- state
static m2d_surf *bb;
static skin *sk_default, *sk;
static font *f_row, *f_small, *f_mid, *f_head, *f_key, *f_title, *f_big;
static m2d_surf *mw, *vis_surf;
static vis_data vd;
static int screen = SCR_PLAYLIST, panel = SCR_PLAYLIST;  // panel: what sits under the main window
static int quit;
static double now_t;
static int current = -1;      // playlist index being played
static int sel, scroll;       // playlist cursor
static int screen_off;
static double sleep_at;       // 0 = no sleep timer
static int sleep_min;
static int pressed = PB_NONE;
static double pressed_until;
static char marquee[400];
static char msg[120];
static double msg_until;
static double marquee_t0;
static char toast[160];
static double toast_until;
static int *shuffle_order, shuffle_n;
static int has_dsp;
static double last_save;
static int playlist_dirty;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void show_toast(const char *fmt, const char *arg)
{
    snprintf(toast, sizeof toast, fmt, arg);
    toast_until = now_t + 2.2;
}

static void show_msg(const char *s)
{
    snprintf(msg, sizeof msg, "%s", s);
    msg_until = now_t + 1.2;
}

static void press_flash(int b) { pressed = b; pressed_until = now_t + 0.18; }

// ---------------------------------------------------------------- colors
#define C_BG 0xff101216u
#define C_PANEL 0xff1a1d24u
#define C_LINE 0xff2f343eu
#define C_TEXT 0xffeef0f4u
#define C_DIM 0xff8a909cu
#define C_MUTED 0xffc9ced8u
#define C_KEY 0xff2c3038u
#define C_SEL 0xff1f2a48u
#define C_GREEN 0xff5fd38du
#define C_GREENBG 0xff1d3a2cu

static int luma(uint32_t c) { return (int)(((c >> 16) & 255) * 0.3 + ((c >> 8) & 255) * 0.59 + (c & 255) * 0.11); }

static uint32_t accent(void)
{
    uint32_t c = sk ? sk->pl_current : 0xffffb347u;
    return luma(c) < 90 ? 0xffffb347u : c;
}

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    int r = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
    int g = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
    int bl = (int)((a & 255) * (1 - t) + (b & 255) * t);
    return 0xff000000u | r << 16 | g << 8 | bl;
}

static void fill(int x, int y, int w, int h, uint32_t c) { m2d_fill(bb, x, y, w, h, c, (c >> 24) == 255 ? M2D_REPLACE : M2D_ALPHA); }

static uint32_t premul(uint32_t c)
{
    uint32_t a = c >> 24;
    return a << 24 | (((c >> 16) & 255) * a / 255) << 16 | (((c >> 8) & 255) * a / 255) << 8 | ((c & 255) * a / 255);
}

static void rrect(int x, int y, int w, int h, int r, uint32_t c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    uint32_t pc = premul(c);
    int blend = (c >> 24) == 255 ? M2D_REPLACE : M2D_ALPHA;
    m2d_fill(bb, x, y + r, w, h - 2 * r, pc, blend);
    for (int i = 0; i < r; i++) {
        float dy = r - i - 0.5f;
        int s = (int)(r - sqrtf((float)r * r - dy * dy) + 0.5f);
        m2d_fill(bb, x + s, y + i, w - 2 * s, 1, pc, blend);
        m2d_fill(bb, x + s, y + h - 1 - i, w - 2 * s, 1, pc, blend);
    }
}

static void fmt_time(char *out, int n, int ms)
{
    int s = ms / 1000;
    if (s >= 3600) snprintf(out, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

// ---------------------------------------------------------------- hint bar
typedef struct { const char *key, *label; } hint;

static void hint_bar(const hint *h, int n)
{
    const int y = 444;
    fill(0, y, SCREEN_W, 36, 0xff14161bu);
    int x = SCREEN_W - 20;
    for (int i = n - 1; i >= 0; i--) {
        int lw = font_width(f_small, h[i].label);
        int kw = font_width(f_key, h[i].key);
        int one = (int)strlen(h[i].key) == 1;
        int bw = one ? (kw + 10 > 20 ? kw + 10 : 20) : kw + 14;
        if (x - lw - 6 - bw < 8) break;
        x -= lw;
        font_draw_mid(bb, f_small, x, y + 18, h[i].label, C_DIM);
        x -= 6 + bw;
        rrect(x, y + 8, bw, 20, one ? 10 : 4, C_KEY);
        font_draw_mid(bb, f_key, x + (bw - kw) / 2, y + 18, h[i].key, C_TEXT);
        x -= 18;
    }
}

// ---------------------------------------------------------------- playback
static void build_shuffle(void)
{
    int n = pl_count();
    free(shuffle_order);
    shuffle_order = malloc(sizeof(int) * (n ? n : 1));
    shuffle_n = n;
    for (int i = 0; i < n; i++) shuffle_order[i] = i;
    for (int i = n - 1; i > 0; i--) { int j = rand() % (i + 1); int t = shuffle_order[i]; shuffle_order[i] = shuffle_order[j]; shuffle_order[j] = t; }
    // current track first
    for (int i = 0; i < n; i++) if (shuffle_order[i] == current) { shuffle_order[i] = shuffle_order[0]; shuffle_order[0] = current; break; }
}

static void update_marquee(void)
{
    pl_item it;
    if (current >= 0 && pl_get(current, &it)) {
        char d[16] = "";
        if (it.dur_ms > 0) { char t[12]; fmt_time(t, sizeof t, it.dur_ms); snprintf(d, sizeof d, " (%s)", t); }
        snprintf(marquee, sizeof marquee, "%d. %s%s  ***  ", current + 1, it.name, d);
    } else {
        snprintf(marquee, sizeof marquee, "MINIAMP  ***  PRESS B TO BROWSE YOUR MUSIC  ***  ");
    }
}

static void play_index(int i, int start_ms, int paused)
{
    const char *p = pl_path(i);
    if (!p) return;
    current = i;
    audio_play_file(p, start_ms, paused);
    marquee_t0 = now_t;
    S.cur_index = i;
    if (S.shuffle && shuffle_n != pl_count()) build_shuffle();
}

static int next_index(int dir)
{
    int n = pl_count();
    if (n == 0) return -1;
    if (S.shuffle) {
        if (shuffle_n != n) build_shuffle();
        int k = 0;
        for (int i = 0; i < n; i++) if (shuffle_order[i] == current) { k = i; break; }
        k += dir;
        if (k >= n) { if (!S.repeat) return -1; build_shuffle(); k = shuffle_order[0] == current && n > 1 ? 1 : 0; }
        if (k < 0) k = 0;
        return shuffle_order[k];
    }
    int k = current + dir;
    if (k >= n) return S.repeat ? 0 : -1;
    if (k < 0) return S.repeat ? n - 1 : 0;
    return k;
}

static void skip(int dir)
{
    int k = next_index(dir);
    if (dir < 0 && audio_pos_ms() > 3000 && current >= 0) k = current;   // "previous" restarts first
    if (k < 0) return;
    play_index(k, 0, 0);
    sel = k;
}

static void toggle_pause(void)
{
    int st = audio_state();
    if (st == A_PLAYING) audio_set_paused(1);
    else if (st == A_PAUSED) audio_set_paused(0);
    else if (pl_count()) play_index(current >= 0 ? current : 0, 0, 0);
}

static void apply_audio_settings(void)
{
    audio_set_volume(S.volume);
    audio_set_balance(S.balance);
    audio_set_eq(S.eq_on, S.eq_pre, S.eq);
}

// ---------------------------------------------------------------- backlight
static int bl_saved = -1;
static const char *BL = "/sys/class/pwm/pwmchip0/pwm0/duty_cycle";

static void set_screen_off(int off)
{
    if (off == screen_off) return;
    screen_off = off;
    if (off) {
        // black frame first, in case the backlight can't be switched off
        m2d_clear(m2d_backbuffer(), 0xff000000u, 0);
        m2d_present();
        m2d_clear(m2d_backbuffer(), 0xff000000u, 0);
        m2d_present();
        FILE *f = fopen(BL, "r");
        if (f) { if (fscanf(f, "%d", &bl_saved) != 1) bl_saved = -1; fclose(f); }
        f = fopen(BL, "w");
        if (f) { fputs("0", f); fclose(f); }
        fprintf(stderr, "[screen] off (saved brightness %d)\n", bl_saved);
    } else {
        FILE *f = fopen(BL, "w");
        if (f) { fprintf(f, "%d", bl_saved > 0 ? bl_saved : 50); fclose(f); }
        fprintf(stderr, "[screen] on\n");
    }
}

// ---------------------------------------------------------------- main window
static void draw_main_window(void)
{
    mainwin_state st;
    memset(&st, 0, sizeof st);
    int as = audio_state();
    st.state = as;
    int pos = audio_pos_ms(), len = audio_len_ms();
    if (len <= 0 && current >= 0) { pl_item it; if (pl_get(current, &it)) len = it.dur_ms; }
    if (len > 0 && pos > len) pos = len;
    st.time_s = S.remaining && len > 0 ? (len - pos + 999) / 1000 : pos / 1000;
    st.remaining = S.remaining && len > 0;
    st.blink_off = as == A_PAUSED && fmod(now_t, 1.0) < 0.5;
    if (msg_until > now_t) st.marquee = msg;
    else {
        update_marquee();
        st.marquee = marquee;
        st.marquee_px = (int)((now_t - marquee_t0) * 22);
    }
    int rate, ch, kbps;
    audio_stream_info(&rate, &ch, &kbps);
    if (kbps <= 0 && current >= 0) {
        static int cached_idx = -2, cached_kbps;
        if (cached_idx != current) {
            track_meta m;
            const char *p = pl_path(current);
            cached_kbps = p && meta_read(p, &m) ? m.kbps : 0;
            cached_idx = current;
        }
        kbps = cached_kbps;
    }
    st.kbps = kbps;
    st.khz = (rate + 500) / 1000;
    st.channels = ch;
    st.volume = S.volume;
    st.balance = S.balance;
    st.eq_on = S.eq_on;
    st.pl_on = panel == SCR_PLAYLIST;
    st.pos = len > 0 ? (float)pos / len : -1;
    st.shuffle = S.shuffle;
    st.repeat = S.repeat;
    st.pressed = pressed_until > now_t ? pressed : PB_NONE;
    st.vis_mode = S.vis_mode;
    st.bars = vd.bars19;
    st.peaks = vd.peaks19;
    st.scope = vd.scope76;
    skin_draw_main(sk, mw, &st);
    m2d_blit(mw, 0, 0, MW_W, MW_H, bb, 45, 8, MW_W * 2, MW_H * 2, 0xffffffffu, M2D_REPLACE, 0);
}

// ---------------------------------------------------------------- playlist panel
#define PX 45
#define PY 248
#define PW 550
#define PH 190
#define ROW_H 20
#define ROWS ((PH - 26) / ROW_H)

static void panel_frame(const char *title, const char *right)
{
    uint32_t bg = sk->pl_bg;
    fill(PX, PY, PW, PH, bg);
    fill(PX, PY, PW, 22, mix(bg, 0xffffffffu, 0.07f));
    fill(PX, PY + 22, PW, 1, mix(bg, 0xffffffffu, 0.15f));
    font_draw_mid(bb, f_head, PX + 12, PY + 11, title, accent());
    if (right) {
        char b[200];
        font_fit_left(f_small, right, PW - 140, b, sizeof b);
        font_draw_mid(bb, f_small, PX + PW - 12 - font_width(f_small, b), PY + 11, b, mix(sk->pl_normal, bg, 0.35f));
    }
}

static void clamp_scroll(int n)
{
    if (sel >= n) sel = n - 1;
    if (sel < 0) sel = 0;
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + ROWS) scroll = sel - ROWS + 1;
    if (scroll > n - ROWS) scroll = n - ROWS;
    if (scroll < 0) scroll = 0;
}

static void draw_playlist_panel(void)
{
    int n = pl_count();
    int complete;
    long long tot = pl_total_ms(&complete);
    char right[120], t[24];
    fmt_time(t, sizeof t, (int)(tot > 2000000000LL ? 2000000000LL : tot));
    if (n) snprintf(right, sizeof right, "%d track%s  ·  %s%s", n, n == 1 ? "" : "s", t, complete ? "" : "+");
    else snprintf(right, sizeof right, "empty");
    panel_frame("PLAYLIST", right);
    clamp_scroll(n);
    pl_set_hint(scroll);
    if (!n) {
        const char *a = "Your playlist is empty.", *b = "Press B to open your music library.";
        font_draw_mid(bb, f_row, PX + (PW - font_width(f_row, a)) / 2, PY + 90, a, sk->pl_normal);
        font_draw_mid(bb, f_small, PX + (PW - font_width(f_small, b)) / 2, PY + 116, b, mix(sk->pl_normal, sk->pl_bg, 0.35f));
        return;
    }
    for (int r = 0; r < ROWS && scroll + r < n; r++) {
        int i = scroll + r;
        pl_item it;
        if (!pl_get(i, &it)) break;
        int y = PY + 26 + r * ROW_H;
        int is_cur = i == current, is_sel = i == sel && screen == SCR_PLAYLIST;
        if (is_sel) fill(PX + 3, y, PW - 16, ROW_H, sk->pl_selbg);
        uint32_t col = is_cur ? sk->pl_current : sk->pl_normal;
        char num[16];
        snprintf(num, sizeof num, "%d.", i + 1);
        font_draw_mid(bb, f_row, PX + 12, y + ROW_H / 2, num, is_cur ? col : mix(col, sk->pl_bg, 0.4f));
        int nx = PX + 16 + font_width(f_row, n >= 1000 ? "0000." : n >= 100 ? "000." : "00.");
        char d[16] = "";
        if (it.dur_ms > 0) fmt_time(d, sizeof d, it.dur_ms);
        int dw = font_width(f_row, d);
        char nm[220];
        font_fit(f_row, it.name, PX + PW - 30 - dw - nx, nm, sizeof nm);
        font_draw_mid(bb, f_row, nx, y + ROW_H / 2, nm, col);
        if (d[0]) font_draw_mid(bb, f_row, PX + PW - 20 - dw, y + ROW_H / 2, d, col);
    }
    // scroll bar
    if (n > ROWS) {
        int track = PH - 32, th = track * ROWS / n;
        if (th < 12) th = 12;
        int ty = PY + 27 + (track - th) * scroll / (n - ROWS);
        fill(PX + PW - 9, PY + 27, 4, track, mix(sk->pl_bg, 0xffffffffu, 0.08f));
        fill(PX + PW - 9, ty, 4, th, mix(sk->pl_bg, 0xffffffffu, 0.3f));
    }
}

// ---------------------------------------------------------------- equalizer
typedef struct { const char *name; float pre; float b[EQ_BANDS]; } eq_preset;
static const eq_preset PRESETS[] = {
    { "Flat", 0, { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
    { "Small Speaker", -3, { -6, -3, 0, 3, 4, 4, 3, 2, 1, 0 } },
    { "Headphones", -3, { 4, 5, 2, -1, -1, 1, 3, 4, 5, 5 } },
    { "Bass Boost", -4, { 7, 6, 4, 2, 0, 0, 0, 0, 0, 0 } },
    { "Treble Boost", -4, { 0, 0, 0, 0, 0, 2, 4, 6, 7, 7 } },
    { "Rock", -3, { 4, 3, 1, -1, -1, 1, 3, 4, 4, 4 } },
    { "Pop", -2, { -1, 1, 3, 4, 3, 1, -1, -1, -1, -1 } },
    { "Dance", -4, { 6, 5, 2, 0, 0, -2, -2, 0, 3, 3 } },
    { "Classical", 0, { 0, 0, 0, 0, 0, 0, -3, -4, -4, -5 } },
    { "Vocal", -3, { -2, -2, 0, 3, 4, 4, 3, 1, 0, -1 } },
    { "Loudness", -4, { 5, 4, 1, 0, -1, 0, 1, 3, 4, 4 } },
};
#define NPRESETS ((int)(sizeof PRESETS / sizeof PRESETS[0]))
static int eq_sel = 1;      // 0 = preamp, 1..10 bands
static int preset_sel;

static void draw_eq_panel(void)
{
    char right[80];
    snprintf(right, sizeof right, "Preset: %s", S.eq_preset[0] ? S.eq_preset : "Custom");
    panel_frame("EQUALIZER", right);
    uint32_t acc = accent();
    // on/off pill
    int bx = PX + 112;
    rrect(bx, PY + 4, 44, 15, 4, S.eq_on ? C_GREENBG : mix(sk->pl_bg, 0xffffffffu, 0.12f));
    const char *lab = S.eq_on ? "ON" : "OFF";
    font_draw_mid(bb, f_key, bx + (44 - font_width(f_key, lab)) / 2, PY + 11, lab, S.eq_on ? C_GREEN : C_DIM);

    static const char *names[11] = { "PRE", "60", "170", "310", "600", "1K", "3K", "6K", "12K", "14K", "16K" };
    const int top = PY + 42, bot = PY + 152, mid = (top + bot) / 2;
    uint32_t grid = mix(sk->pl_bg, 0xffffffffu, 0.1f);
    fill(PX + 92, mid, PW - 112, 1, grid);
    font_draw_mid(bb, f_small, PX + 14, top + 2, "+12", C_DIM);
    font_draw_mid(bb, f_small, PX + 20, mid, "0", C_DIM);
    font_draw_mid(bb, f_small, PX + 14, bot - 2, "-12", C_DIM);
    int px_prev = -1, py_prev = 0;
    for (int i = 0; i < 11; i++) {
        float v = i == 0 ? S.eq_pre : S.eq[i - 1];
        int cx = PX + 64 + i * 44 + (i > 0 ? 18 : 0);
        rrect(cx - 3, top, 7, bot - top, 3, mix(sk->pl_bg, 0xffffffffu, 0.1f));
        int ty = (int)(mid - v / 12.f * (bot - top) / 2);
        int a = ty < mid ? ty : mid, b = ty < mid ? mid : ty;
        fill(cx - 3, a, 7, b - a + 1, i ? mix(acc, sk->pl_bg, 0.45f) : mix(C_DIM, sk->pl_bg, 0.4f));
        int is_sel = screen == SCR_EQ && i == eq_sel;
        rrect(cx - 12, ty - 6, 25, 12, 3, is_sel ? acc : C_MUTED);
        font_draw_mid(bb, f_key, cx - font_width(f_key, names[i]) / 2, bot + 16, names[i], is_sel ? acc : C_DIM);
        if (is_sel) {
            char vb[16];
            snprintf(vb, sizeof vb, "%+.0f dB", v);
            font_draw_mid(bb, f_small, cx - font_width(f_small, vb) / 2, top - 10, vb, acc);
        }
        if (i > 0) {
            if (px_prev >= 0) m2d_line(bb, px_prev, py_prev, cx, ty, 2, premul((acc & 0x00ffffffu) | 0x90000000u), M2D_ALPHA);
            px_prev = cx; py_prev = ty;
        }
    }
}

static void eq_apply_preset(int i)
{
    S.eq_pre = PRESETS[i].pre;
    memcpy(S.eq, PRESETS[i].b, sizeof S.eq);
    snprintf(S.eq_preset, sizeof S.eq_preset, "%s", PRESETS[i].name);
    S.eq_on = 1;
    apply_audio_settings();
}

// ---------------------------------------------------------------- generic list card (options, presets)
static void card(int x, int y, int w, int h, const char *title)
{
    fill(0, 0, SCREEN_W, SCREEN_H, 0xc8060709u);
    rrect(x - 1, y - 1, w + 2, h + 2, 13, C_LINE);
    rrect(x, y, w, h, 12, C_PANEL);
    font_draw_mid(bb, f_title, x + 24, y + 26, title, accent());
}

static void pill(int right_x, int cy, const char *val, int on)
{
    int tw = font_width(f_key, val) + 20;
    rrect(right_x - tw, cy - 11, tw, 22, 11, on ? C_GREENBG : 0xff262a31u);
    font_draw_mid(bb, f_key, right_x - tw + 10, cy, val, on ? C_GREEN : C_MUTED);
}

// ---------------------------------------------------------------- options
enum { OPT_SCREEN, OPT_SHUFFLE, OPT_REPEAT, OPT_SLEEP, OPT_VOLUME, OPT_BALANCE, OPT_MINIVIS, OPT_TIME, OPT_SKIN, OPT_CLEAR, OPT_QUIT, OPT_N };
static int opt_sel;
static const int SLEEP_STEPS[] = { 0, 15, 30, 45, 60, 90, 120 };

static void opt_value(int i, char *out, int n, int *on)
{
    *on = 0;
    out[0] = 0;
    switch (i) {
    case OPT_SHUFFLE: snprintf(out, n, "%s", S.shuffle ? "On" : "Off"); *on = S.shuffle; break;
    case OPT_REPEAT: snprintf(out, n, "%s", S.repeat == 1 ? "All" : S.repeat == 2 ? "One" : "Off"); *on = S.repeat != 0; break;
    case OPT_SLEEP:
        if (sleep_at > 0) { int m = (int)((sleep_at - now_t) / 60 + 0.99); snprintf(out, n, "%d min left", m); *on = 1; }
        else snprintf(out, n, "Off");
        break;
    case OPT_VOLUME: snprintf(out, n, "%d%%", S.volume); break;
    case OPT_BALANCE:
        if (S.balance == 0) snprintf(out, n, "Center");
        else snprintf(out, n, "%d%% %s", abs(S.balance), S.balance < 0 ? "Left" : "Right");
        break;
    case OPT_MINIVIS: snprintf(out, n, "%s", S.vis_mode == 1 ? "Spectrum" : S.vis_mode == 2 ? "Oscilloscope" : "Off"); break;
    case OPT_TIME: snprintf(out, n, "%s", S.remaining ? "Remaining" : "Elapsed"); break;
    case OPT_SKIN: snprintf(out, n, "%s", sk->name); break;
    }
}

static void draw_options(void)
{
    static const char *labels[OPT_N] = { "Turn screen off", "Shuffle", "Repeat", "Sleep timer", "Volume", "Balance",
                                         "Mini visualizer", "Time display", "Skin", "Clear playlist", "Quit MiniAmp" };
    const int x = 110, y = 14, w = 420, h = 424, rh = 32;
    card(x, y, w, h, "OPTIONS");
    for (int i = 0; i < OPT_N; i++) {
        int ry = y + 50 + i * (rh + 2);
        if (i == opt_sel) rrect(x + 12, ry, w - 24, rh, 8, C_SEL);
        font_draw_mid(bb, f_mid, x + 26, ry + rh / 2, labels[i], C_TEXT);
        char v[64];
        int on;
        opt_value(i, v, sizeof v, &on);
        if (i == OPT_SCREEN && i == opt_sel) font_draw_mid(bb, f_small, x + w - 26 - font_width(f_small, "START pauses · L/R skip"), ry + rh / 2, "START pauses · L/R skip", C_DIM);
        if (v[0]) {
            char fv[64];
            font_fit(f_key, v, 170, fv, sizeof fv);
            pill(x + w - 26, ry + rh / 2, fv, on);
        }
    }
}

static void open_skins(void);

static void option_change(int dir, int activate)
{
    switch (opt_sel) {
    case OPT_SCREEN: if (activate) { screen = panel; set_screen_off(1); } break;
    case OPT_SHUFFLE: S.shuffle = !S.shuffle; if (S.shuffle) build_shuffle(); break;
    case OPT_REPEAT: S.repeat = (S.repeat + (dir < 0 ? 2 : 1)) % 3; break;
    case OPT_SLEEP: {
        int k = 0;
        for (int i = 0; i < 7; i++) if (SLEEP_STEPS[i] == sleep_min) k = i;
        k = (k + (dir < 0 ? 6 : 1)) % 7;
        sleep_min = SLEEP_STEPS[k];
        sleep_at = sleep_min ? now_t + sleep_min * 60.0 : 0;
        break;
    }
    case OPT_VOLUME:
        S.volume += activate ? 0 : dir * 5;
        if (S.volume < 0) S.volume = 0;
        if (S.volume > 100) S.volume = 100;
        apply_audio_settings();
        { char b[40]; snprintf(b, sizeof b, "VOLUME: %d%%", S.volume); show_msg(b); }
        break;
    case OPT_BALANCE:
        if (activate) S.balance = 0; else S.balance += dir * 10;
        if (S.balance < -100) S.balance = -100;
        if (S.balance > 100) S.balance = 100;
        apply_audio_settings();
        break;
    case OPT_MINIVIS: S.vis_mode = (S.vis_mode + (dir < 0 ? 2 : 1)) % 3; break;
    case OPT_TIME: S.remaining = !S.remaining; break;
    case OPT_SKIN: if (activate) open_skins(); break;
    case OPT_CLEAR:
        if (activate) {
            audio_stop();
            pl_clear();
            current = -1; sel = 0; scroll = 0; S.cur_index = -1;
            playlist_dirty = 1;
            screen = panel = SCR_PLAYLIST;
            show_toast("%s", "Playlist cleared");
        }
        break;
    case OPT_QUIT: if (activate) quit = 1; break;
    }
}

// ---------------------------------------------------------------- presets card
static void draw_presets(void)
{
    const int x = 170, y = 40, w = 300, h = 400, rh = 30;
    card(x, y, w, h, "PRESETS");
    for (int i = 0; i < NPRESETS; i++) {
        int ry = y + 50 + i * rh;
        if (i == preset_sel) rrect(x + 12, ry, w - 24, rh - 2, 8, C_SEL);
        font_draw_mid(bb, f_mid, x + 26, ry + rh / 2 - 1, PRESETS[i].name, !strcmp(S.eq_preset, PRESETS[i].name) ? accent() : C_TEXT);
    }
}

// ---------------------------------------------------------------- skins
typedef struct { char path[600]; char name[128]; } skin_entry;
static skin_entry *skins;
static int nskins, skin_sel, skin_scroll;
static skin *preview;
static double preview_due;
static int preview_idx = -1;
static skin *skin_before;

static int cmp_skin(const void *a, const void *b) { return name_cmp(((const skin_entry *)a)->name, ((const skin_entry *)b)->name); }

static void scan_skins(void)
{
    nskins = 0;
    const char *dirs[2];
    char d0[600];
    path_join(d0, sizeof d0, app_dir, "skins");
    dirs[0] = d0;
    dirs[1] = getenv("MA_SKINS");
    int cap = 0;
    for (int k = 0; k < 2; k++) {
        if (!dirs[k]) continue;
        DIR *d = opendir(dirs[k]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[600];
            snprintf(p, sizeof p, "%s/%s", dirs[k], e->d_name);
            const char *dot = strrchr(e->d_name, '.');
            struct stat st;
            if (stat(p, &st)) continue;
            int ok = (dot && !strcasecmp(dot, ".wsz") && S_ISREG(st.st_mode)) || S_ISDIR(st.st_mode);
            if (!ok) continue;
            if (nskins == cap) { cap = cap ? cap * 2 : 32; skins = realloc(skins, sizeof *skins * cap); }
            snprintf(skins[nskins].path, sizeof skins[nskins].path, "%s", p);
            snprintf(skins[nskins].name, sizeof skins[nskins].name, "%s", e->d_name);
            char *dt = strrchr(skins[nskins].name, '.');
            if (dt && !strcasecmp(dt, ".wsz")) *dt = 0;
            for (char *c = skins[nskins].name; *c; c++) if (*c == '_') *c = ' ';
            nskins++;
        }
        closedir(d);
    }
    if (nskins) qsort(skins, nskins, sizeof *skins, cmp_skin);
}

static void open_skins(void)
{
    scan_skins();
    skin_sel = 0;
    for (int i = 0; i < nskins; i++) if (!strcmp(skins[i].path, S.skin)) skin_sel = i;
    skin_scroll = 0;
    skin_before = sk;
    preview_idx = skin_sel;
    screen = SCR_SKINS;
}

static void close_skins(int apply)
{
    if (apply && preview && preview != sk) {
        if (sk != sk_default) skin_free(sk);
        sk = preview;
        snprintf(S.skin, sizeof S.skin, "%s", skins[preview_idx].path);
    } else if (apply && preview_idx >= 0 && preview_idx < nskins) {
        snprintf(S.skin, sizeof S.skin, "%s", skins[preview_idx].path);
    }
    if (!apply && preview && preview != sk && preview != sk_default) skin_free(preview);
    preview = NULL;
    sk = apply ? sk : skin_before;
    screen = panel;
}

static void skins_update(void)
{
    if (skin_sel == preview_idx || now_t < preview_due || !nskins) return;
    skin *old = preview;
    skin *s = NULL;
    // the default skin is already loaded
    char defp[600];
    path_join(defp, sizeof defp, app_dir, "skins/Graphite.wsz");
    if (!strcmp(skins[skin_sel].path, defp)) s = sk_default;
    else s = skin_load(skins[skin_sel].path, sk_default);
    preview_idx = skin_sel;
    if (!s) { show_toast("%s", "That skin couldn't be loaded"); return; }
    if (old && old != sk && old != sk_default && old != skin_before) skin_free(old);
    preview = s;
}

static void draw_skins_panel(void)
{
    char right[64];
    snprintf(right, sizeof right, "%d of %d  ·  put .wsz files in App/MiniAmp/skins", nskins ? skin_sel + 1 : 0, nskins);
    panel_frame("SKINS", right);
    if (skin_sel < skin_scroll) skin_scroll = skin_sel;
    if (skin_sel >= skin_scroll + ROWS) skin_scroll = skin_sel - ROWS + 1;
    for (int r = 0; r < ROWS && skin_scroll + r < nskins; r++) {
        int i = skin_scroll + r, y = PY + 26 + r * ROW_H;
        if (i == skin_sel) fill(PX + 3, y, PW - 6, ROW_H, sk->pl_selbg);
        int active = !strcmp(skins[i].path, S.skin);
        char nm[160];
        font_fit(f_row, skins[i].name, PW - 120, nm, sizeof nm);
        font_draw_mid(bb, f_row, PX + 14, y + ROW_H / 2, nm, active ? sk->pl_current : sk->pl_normal);
        if (active) font_draw_mid(bb, f_small, PX + PW - 20 - font_width(f_small, "in use"), y + ROW_H / 2, "in use", mix(sk->pl_normal, sk->pl_bg, 0.3f));
    }
}

// ---------------------------------------------------------------- browser
typedef struct { char *name; int dir, m3u; } b_entry;
static char cwd[1024];
static b_entry *ents;
static int nents, b_sel, b_scroll;
#define B_TOP 58
#define B_ROWH 30
#define B_ROWS 11

static int cmp_ent(const void *a, const void *b)
{
    const b_entry *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return name_cmp(x->name, y->name);
}

static int is_m3u(const char *n)
{
    const char *d = strrchr(n, '.');
    return d && (!strcasecmp(d, ".m3u") || !strcasecmp(d, ".m3u8"));
}

static void browse(const char *dir, const char *select_name)
{
    for (int i = 0; i < nents; i++) free(ents[i].name);
    nents = 0;
    snprintf(cwd, sizeof cwd, "%s", dir);
    DIR *d = opendir(dir);
    int cap = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[1400];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(p, &st)) continue;
            int isdir = S_ISDIR(st.st_mode);
            int m3u = !isdir && is_m3u(e->d_name);
            if (!isdir && !m3u && !dec_supported(e->d_name)) continue;
            if (nents == cap) { cap = cap ? cap * 2 : 64; ents = realloc(ents, sizeof *ents * cap); }
            ents[nents].name = strdup(e->d_name);
            ents[nents].dir = isdir;
            ents[nents].m3u = m3u;
            nents++;
        }
        closedir(d);
    }
    if (nents) qsort(ents, nents, sizeof *ents, cmp_ent);
    b_sel = 0;
    b_scroll = 0;
    if (select_name) for (int i = 0; i < nents; i++) if (!strcmp(ents[i].name, select_name)) b_sel = i;
    snprintf(S.last_dir, sizeof S.last_dir, "%s", cwd);
}

static int dir_exists(const char *p)
{
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void open_browser(void)
{
    if (dir_exists(S.last_dir) && !strncmp(S.last_dir, root_dir, strlen(root_dir))) browse(S.last_dir, NULL);
    else {
        static const char *cands[] = { "Media/Music", "Music", "Media", NULL };
        char p[1024];
        const char *pick = root_dir;
        for (int i = 0; cands[i]; i++) {
            path_join(p, sizeof p, root_dir, cands[i]);
            if (dir_exists(p)) { pick = p; break; }
        }
        browse(pick, NULL);
    }
    screen = SCR_BROWSER;
}

static void browser_up(void)
{
    if (!strcmp(cwd, root_dir)) { screen = panel; return; }
    char parent[1024], name[512];
    snprintf(parent, sizeof parent, "%s", cwd);
    char *sl = strrchr(parent, '/');
    if (!sl) { screen = panel; return; }
    snprintf(name, sizeof name, "%s", sl + 1);
    if (sl == parent) sl[1] = 0; else *sl = 0;
    if (strlen(parent) < strlen(root_dir)) snprintf(parent, sizeof parent, "%s", root_dir);
    browse(parent, name);
}

static void play_after_load(int idx, const char *what, int n)
{
    current = -1;
    sel = idx; scroll = 0;
    playlist_dirty = 1;
    if (S.shuffle) { current = idx; build_shuffle(); }
    play_index(idx, 0, 0);
    char b[160];
    snprintf(b, sizeof b, "%s (%d track%s)", what, n, n == 1 ? "" : "s");
    show_toast("%s", b);
    screen = panel = SCR_PLAYLIST;
}

static void browser_activate(int action)   // 0 = A, 1 = X (add), 2 = Y (play folder)
{
    char p[1400];
    if (action == 2) {
        const char *dir = cwd;
        if (nents && ents[b_sel].dir) { path_join(p, sizeof p, cwd, ents[b_sel].name); dir = p; }
        audio_stop();
        pl_clear();
        int n = pl_add_dir(dir, 1);
        if (!n) { show_toast("%s", "No music in that folder"); return; }
        play_after_load(0, "Playing folder", n);
        return;
    }
    if (!nents) return;
    b_entry *e = &ents[b_sel];
    path_join(p, sizeof p, cwd, e->name);
    if (action == 1) {
        int n = e->dir ? pl_add_dir(p, 1) : e->m3u ? pl_load_m3u(p) : (pl_add(p), 1);
        playlist_dirty = 1;
        char b[64];
        snprintf(b, sizeof b, "Added %d track%s", n, n == 1 ? "" : "s");
        show_toast("%s", b);
        return;
    }
    if (e->dir) { browse(p, NULL); return; }
    audio_stop();
    pl_clear();
    if (e->m3u) {
        int n = pl_load_m3u(p);
        if (n) play_after_load(0, "Playing list", n);
        return;
    }
    // play this folder's tracks, starting at the chosen one
    int n = 0, start = 0;
    for (int i = 0; i < nents; i++) {
        if (ents[i].dir || ents[i].m3u) continue;
        char q[1400];
        path_join(q, sizeof q, cwd, ents[i].name);
        pl_add(q);
        if (i == b_sel) start = n;
        n++;
    }
    play_after_load(start, "Playing folder", n);
}

static void folder_icon(int x, int cy, uint32_t c)
{
    fill(x, cy - 6, 7, 2, c);
    fill(x, cy - 4, 18, 11, c);
    fill(x + 2, cy - 2, 14, 7, mix(c, C_BG, 0.25f));
}

static void draw_browser(void)
{
    m2d_clear(bb, C_BG, 0);
    fill(0, 0, SCREEN_W, 50, C_PANEL);
    fill(0, 50, SCREEN_W, 1, C_LINE);
    font_draw_mid(bb, f_title, 20, 25, "LIBRARY", accent());
    const char *rel = cwd + strlen(root_dir);
    char shown[1100];
    snprintf(shown, sizeof shown, "%s", rel[0] ? rel : "/");
    char fit[1100];
    font_fit_left(f_small, shown, 440, fit, sizeof fit);
    font_draw_mid(bb, f_small, SCREEN_W - 20 - font_width(f_small, fit), 25, fit, C_DIM);
    if (!nents) {
        const char *t = "No folders or music files here";
        font_draw_mid(bb, f_mid, (SCREEN_W - font_width(f_mid, t)) / 2, 200, t, C_DIM);
    }
    if (b_sel < b_scroll) b_scroll = b_sel;
    if (b_sel >= b_scroll + B_ROWS) b_scroll = b_sel - B_ROWS + 1;
    uint32_t acc = accent();
    for (int r = 0; r < B_ROWS && b_scroll + r < nents; r++) {
        int i = b_scroll + r, y = B_TOP + r * B_ROWH;
        b_entry *e = &ents[i];
        if (i == b_sel) rrect(10, y, SCREEN_W - 20, B_ROWH - 2, 8, C_SEL);
        int cy = y + B_ROWH / 2 - 1;
        if (e->dir) folder_icon(24, cy, acc);
        else font_draw_mid(bb, f_mid, 26, cy, e->m3u ? "≡" : "♪", e->m3u ? C_GREEN : C_DIM);
        char nm[600];
        char base[512];
        snprintf(base, sizeof base, "%s", e->name);
        if (!e->dir) { char *d = strrchr(base, '.'); if (d && d != base) *d = 0; }
        font_fit(f_mid, base, SCREEN_W - 140, nm, sizeof nm);
        font_draw_mid(bb, f_mid, 54, cy, nm, e->dir ? C_TEXT : C_MUTED);
        if (!e->dir) {
            const char *ext = strrchr(e->name, '.');
            if (ext) {
                char up[8];
                int k = 0;
                for (ext++; *ext && k < 5; ext++) up[k++] = (*ext >= 'a' && *ext <= 'z') ? *ext - 32 : *ext;
                up[k] = 0;
                font_draw_mid(bb, f_key, SCREEN_W - 26 - font_width(f_key, up), cy, up, C_DIM);
            }
        }
    }
    if (nents > B_ROWS) {
        int track = B_ROWS * B_ROWH, th = track * B_ROWS / nents;
        if (th < 16) th = 16;
        int ty = B_TOP + (track - th) * b_scroll / (nents - B_ROWS);
        fill(SCREEN_W - 6, B_TOP, 3, track, C_LINE);
        fill(SCREEN_W - 6, ty, 3, th, C_DIM);
    }
    // now playing strip
    fill(0, 412, SCREEN_W, 32, C_PANEL);
    if (current >= 0) {
        pl_item it;
        if (pl_get(current, &it)) {
            char t[16], line[300], fitl[300];
            fmt_time(t, sizeof t, audio_pos_ms());
            snprintf(line, sizeof line, "%s", it.name);
            font_fit(f_small, line, SCREEN_W - 120, fitl, sizeof fitl);
            font_draw_mid(bb, f_small, 20, 428, audio_state() == A_PLAYING ? "▶" : "❚❚", acc);
            font_draw_mid(bb, f_small, 40, 428, fitl, C_MUTED);
            font_draw_mid(bb, f_small, SCREEN_W - 20 - font_width(f_small, t), 428, t, C_DIM);
        }
    }
}

// ---------------------------------------------------------------- visualizer screen
static double vis_info_until;

static void draw_vis(void)
{
    vis_render(S.vis_preset, &vd, vis_surf, sk->vis);
    m2d_blit(vis_surf, 0, 0, VIS_W, VIS_H, bb, 0, 0, SCREEN_W, SCREEN_H, 0xffffffffu, M2D_REPLACE, 0);
    if (vis_info_until > now_t) {
        float a = (float)(vis_info_until - now_t);
        if (a > 1) a = 1;
        uint32_t al = (uint32_t)(a * 255);
        pl_item it;
        char line[260] = "", sub[120];
        if (current >= 0 && pl_get(current, &it)) font_fit(f_mid, it.name, 520, line, sizeof line);
        char t[16], l[16];
        fmt_time(t, sizeof t, audio_pos_ms());
        fmt_time(l, sizeof l, audio_len_ms());
        snprintf(sub, sizeof sub, "%s / %s   ·   %s", t, l, vis_preset_name(S.vis_preset));
        int w = font_width(f_mid, line);
        int sw = font_width(f_small, sub);
        if (sw > w) w = sw;
        rrect(16, 14, w + 32, 56, 8, (uint32_t)(170 * a) << 24 | 0x0a0c10);
        font_draw_mid(bb, f_mid, 32, 32, line, (al << 24) | 0xf0f2f6);
        font_draw_mid(bb, f_small, 32, 54, sub, (al << 24) | 0xc9ced8);
        hint h[] = { { "←/→", "Effect" }, { "A", "Pause" }, { "L/R", "Skip" }, { "B", "Back" } };
        hint_bar(h, 4);
    }
}

// ---------------------------------------------------------------- input
static void seek_by(int delta_ms)
{
    if (current < 0 || audio_state() == A_STOPPED) return;
    int len = audio_len_ms();
    int p = audio_pos_ms() + delta_ms;
    if (p < 0) p = 0;
    if (len > 0 && p > len - 500) p = len - 500;
    audio_seek(p);
    char a[16], b[16], m[64];
    fmt_time(a, sizeof a, p);
    fmt_time(b, sizeof b, len);
    snprintf(m, sizeof m, "SEEK TO: %s/%s (%d%%)", a, b, len > 0 ? p * 100 / len : 0);
    show_msg(m);
}

static double held_since;

static void on_key(int k, int repeat)
{
    // keys that work everywhere except text-like lists
    if (screen == SCR_OPTIONS) {
        if (k == K_UP) opt_sel = (opt_sel + OPT_N - 1) % OPT_N;
        else if (k == K_DOWN) opt_sel = (opt_sel + 1) % OPT_N;
        else if (k == K_LEFT) option_change(-1, 0);
        else if (k == K_RIGHT) option_change(1, 0);
        else if (k == K_A && !repeat) option_change(1, 1);
        else if ((k == K_B || k == K_SELECT || k == K_MENU) && !repeat) screen = panel;
        return;
    }
    if (screen == SCR_PRESETS) {
        if (k == K_UP) preset_sel = (preset_sel + NPRESETS - 1) % NPRESETS;
        else if (k == K_DOWN) preset_sel = (preset_sel + 1) % NPRESETS;
        else if (k == K_A) { eq_apply_preset(preset_sel); screen = SCR_EQ; }
        else if (k == K_B || k == K_X || k == K_MENU) screen = SCR_EQ;
        return;
    }
    if (screen == SCR_SKINS) {
        if (k == K_UP && nskins) { skin_sel = (skin_sel + nskins - 1) % nskins; preview_due = now_t + 0.18; }
        else if (k == K_DOWN && nskins) { skin_sel = (skin_sel + 1) % nskins; preview_due = now_t + 0.18; }
        else if (k == K_L2 && nskins) { skin_sel = skin_sel - ROWS < 0 ? 0 : skin_sel - ROWS; preview_due = now_t + 0.18; }
        else if (k == K_R2 && nskins) { skin_sel = skin_sel + ROWS >= nskins ? nskins - 1 : skin_sel + ROWS; preview_due = now_t + 0.18; }
        else if (k == K_A && !repeat) { preview_due = 0; skins_update(); close_skins(1); show_toast("Skin: %s", sk->name); }
        else if ((k == K_B || k == K_MENU) && !repeat) close_skins(0);
        return;
    }
    if (screen == SCR_BROWSER) {
        if (k == K_UP && nents) b_sel = (b_sel + nents - 1) % nents;
        else if (k == K_DOWN && nents) b_sel = (b_sel + 1) % nents;
        else if (k == K_L2 || k == K_LEFT) b_sel = b_sel - B_ROWS < 0 ? 0 : b_sel - B_ROWS;
        else if (k == K_R2 || k == K_RIGHT) b_sel = nents ? (b_sel + B_ROWS >= nents ? nents - 1 : b_sel + B_ROWS) : 0;
        else if (repeat) return;
        else if (k == K_A) browser_activate(0);
        else if (k == K_X) browser_activate(1);
        else if (k == K_Y) browser_activate(2);
        else if (k == K_B) browser_up();
        else if (k == K_START) toggle_pause();
        else if (k == K_SELECT || k == K_MENU) screen = panel;
        else if (k == K_L) skip(-1);
        else if (k == K_R) skip(1);
        return;
    }
    if (screen == SCR_VIS) {
        if (repeat) return;
        if (k == K_LEFT) S.vis_preset = (S.vis_preset + VIS_PRESETS - 1) % VIS_PRESETS;
        else if (k == K_RIGHT) S.vis_preset = (S.vis_preset + 1) % VIS_PRESETS;
        else if (k == K_A || k == K_START) toggle_pause();
        else if (k == K_L) skip(-1);
        else if (k == K_R) skip(1);
        else if (k == K_B || k == K_X || k == K_MENU) { screen = panel; return; }
        vis_info_until = now_t + 3;
        return;
    }
    // player screens (playlist / EQ panel)
    if (k == K_L && !repeat) { press_flash(PB_PREV); skip(-1); return; }
    if (k == K_R && !repeat) { press_flash(PB_NEXT); skip(1); return; }
    if (k == K_START && !repeat) { int st = audio_state(); press_flash(st == A_PLAYING ? PB_PAUSE : PB_PLAY); toggle_pause(); return; }
    if ((k == K_SELECT || k == K_MENU) && !repeat) { opt_sel = 0; screen = SCR_OPTIONS; return; }
    if (k == K_X && screen == SCR_PLAYLIST && !repeat) { screen = SCR_VIS; vis_info_until = now_t + 3; return; }
    if (screen == SCR_PLAYLIST) {
        int n = pl_count();
        if (k == K_UP && n) sel = (sel + n - 1) % n;
        else if (k == K_DOWN && n) sel = (sel + 1) % n;
        else if (k == K_L2) { sel -= ROWS; if (sel < 0) sel = 0; }
        else if (k == K_R2) { sel += ROWS; if (sel >= n) sel = n - 1; }
        else if (k == K_LEFT || k == K_RIGHT) {
            if (!repeat) held_since = now_t;
            int step = now_t - held_since > 2.0 ? 30000 : now_t - held_since > 0.8 ? 10000 : 5000;
            seek_by(k == K_LEFT ? -step : step);
        }
        else if (repeat) return;
        else if (k == K_A && n) {
            if (sel == current && audio_state() != A_STOPPED) { press_flash(audio_state() == A_PLAYING ? PB_PAUSE : PB_PLAY); toggle_pause(); }
            else { press_flash(PB_PLAY); play_index(sel, 0, 0); }
        }
        else if (k == K_B) { press_flash(PB_EJECT); open_browser(); }
        else if (k == K_Y) { press_flash(PB_EQ); screen = panel = SCR_EQ; }
        return;
    }
    if (screen == SCR_EQ) {
        if (k == K_LEFT) eq_sel = (eq_sel + 10) % 11;
        else if (k == K_RIGHT) eq_sel = (eq_sel + 1) % 11;
        else if (k == K_UP || k == K_DOWN) {
            float *v = eq_sel == 0 ? &S.eq_pre : &S.eq[eq_sel - 1];
            *v += k == K_UP ? 1 : -1;
            if (*v > 12) *v = 12;
            if (*v < -12) *v = -12;
            S.eq_preset[0] = 0;
            S.eq_on = 1;
            apply_audio_settings();
        }
        else if (repeat) return;
        else if (k == K_A) { S.eq_on = !S.eq_on; apply_audio_settings(); press_flash(PB_EQ); }
        else if (k == K_X) {
            preset_sel = 0;
            for (int i = 0; i < NPRESETS; i++) if (!strcmp(PRESETS[i].name, S.eq_preset)) preset_sel = i;
            screen = SCR_PRESETS;
        }
        else if (k == K_B || k == K_Y) { press_flash(PB_PL); screen = panel = SCR_PLAYLIST; }
    }
}

// ---------------------------------------------------------------- frame
static void draw_frame(void)
{
    bb = m2d_backbuffer();
    if (screen == SCR_BROWSER) {
        draw_browser();
        hint h[] = { { "A", "Open / Play" }, { "X", "Add" }, { "Y", "Play all" }, { "B", "Back" } };
        hint_bar(h, 4);
    } else if (screen == SCR_VIS) {
        draw_vis();
    } else {
        m2d_clear(bb, C_BG, 0);
        skin *real = sk;
        if (screen == SCR_SKINS && preview) sk = preview;
        draw_main_window();
        sk = real;
        if (screen == SCR_SKINS) {
            skin *keep = sk;
            if (preview) sk = preview;
            draw_skins_panel();
            sk = keep;
            hint h[] = { { "A", "Use skin" }, { "B", "Cancel" } };
            hint_bar(h, 2);
        } else if (panel == SCR_EQ) {
            draw_eq_panel();
            hint h[] = { { "↑/↓", "Level" }, { "←/→", "Band" }, { "A", "On/Off" }, { "X", "Presets" }, { "B", "Back" } };
            hint_bar(h, 5);
        } else {
            draw_playlist_panel();
            hint h[] = { { "A", "Play" }, { "B", "Library" }, { "Y", "EQ" }, { "X", "Visuals" }, { "SELECT", "Menu" } };
            hint_bar(h, 5);
        }
        if (screen == SCR_OPTIONS) {
            draw_options();
            hint h[] = { { "←/→", "Change" }, { "A", "Select" }, { "B", "Close" } };
            hint_bar(h, 3);
        } else if (screen == SCR_PRESETS) {
            draw_presets();
            hint h[] = { { "A", "Apply" }, { "B", "Back" } };
            hint_bar(h, 2);
        }
    }
    if (toast_until > now_t) {
        int w = font_width(f_mid, toast) + 36;
        int x = (SCREEN_W - w) / 2, y = screen == SCR_BROWSER ? 370 : 396;
        rrect(x, y, w, 34, 17, 0xf0262b36u);
        font_draw_mid(bb, f_mid, x + 18, y + 17, toast, C_TEXT);
    }
}

// ---------------------------------------------------------------- main
static void on_signal(int s) { (void)s; quit = 1; }

static void save_all(void)
{
    S.cur_index = current;
    S.cur_pos_ms = current >= 0 ? audio_pos_ms() : 0;
    settings_save();
    if (playlist_dirty) {
        char p[1024];
        path_join(p, sizeof p, data_dir, "playlist.m3u");
        pl_save_m3u(p);
        playlist_dirty = 0;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    const char *home = getenv("MA_HOME");
    if (home) snprintf(app_dir, sizeof app_dir, "%s", home);
    else if (!getcwd(app_dir, sizeof app_dir)) strcpy(app_dir, ".");
    const char *dd = getenv("MA_DATA");
    if (dd) snprintf(data_dir, sizeof data_dir, "%s", dd);
    else path_join(data_dir, sizeof data_dir, app_dir, "data");
    mkdir(data_dir, 0755);
    const char *rd = getenv("MA_ROOT");
    snprintf(root_dir, sizeof root_dir, "%s", rd ? rd : dir_exists("/mnt/SDCARD") ? "/mnt/SDCARD" : (getenv("HOME") ? getenv("HOME") : "/"));
    if (strlen(root_dir) > 1 && root_dir[strlen(root_dir) - 1] == '/') root_dir[strlen(root_dir) - 1] = 0;
    (void)argv;
    srand((unsigned)time(NULL));
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    settings_load();
    if (m2d_init(SCREEN_W, SCREEN_H) != 0) { fprintf(stderr, "display init failed\n"); return 1; }

    char p[1024], fb[1024];
    path_join(fb, sizeof fb, app_dir, "fonts/DejaVuSans.ttf");
    path_join(p, sizeof p, app_dir, "fonts/Rubik-400.ttf");
    f_row = font_open(p, fb, 15);
    f_small = font_open(p, fb, 13);
    f_mid = font_open(p, fb, 16);
    path_join(p, sizeof p, app_dir, "fonts/Rubik-700.ttf");
    f_key = font_open(p, fb, 12);
    path_join(p, sizeof p, app_dir, "fonts/ChakraPetch-Bold.ttf");
    f_head = font_open(p, fb, 14);
    f_title = font_open(p, fb, 20);
    f_big = font_open(p, fb, 28);
    if (!f_row || !f_key || !f_head) { fprintf(stderr, "fonts missing\n"); return 1; }

    path_join(p, sizeof p, app_dir, "skins/Graphite.wsz");
    sk_default = skin_load(p, NULL);
    if (!sk_default) { fprintf(stderr, "default skin missing: %s\n", p); return 1; }
    sk = sk_default;
    if (S.skin[0] && strcmp(S.skin, p)) {
        skin *s = skin_load(S.skin, sk_default);
        if (s) sk = s;
    }
    if (!S.skin[0]) snprintf(S.skin, sizeof S.skin, "%s", p);
    mw = m2d_surface_new(MW_W, MW_H);
    vis_surf = m2d_surface_new(VIS_W, VIS_H);

    has_dsp = audio_init();
    apply_audio_settings();
    pl_init();
    path_join(p, sizeof p, data_dir, "playlist.m3u");
    pl_load_m3u(p);
    if (S.cur_index >= 0 && S.cur_index < pl_count()) {
        sel = S.cur_index;
        play_index(S.cur_index, S.cur_pos_ms, 1);   // resume where we left off, paused
    }
    if (S.shuffle) build_shuffle();

    double last = now();
    now_t = last;
    last_save = last;
    int held_key = -1;
    double next_repeat = 0;
    int errors = 0;
    const char *mf = getenv("MA_MAX_FRAMES");
    int max_frames = mf ? atoi(mf) : 0;
    int profile = getenv("MA_PROFILE") != NULL;
    while (!quit) {
        if (max_frames && m2d_frame_count() >= max_frames) break;
        now_t = now();
        float dt = (float)(now_t - last);
        last = now_t;
        int code, val;
        while (m2d_poll_key(&code, &val)) {
            if (code == K_POWER) continue;
            if (screen_off) {
                if (val != 1) continue;
                if (code == K_START) toggle_pause();
                else if (code == K_L) skip(-1);
                else if (code == K_R) skip(1);
                else set_screen_off(0);
                continue;
            }
            if (val == 1) {
                on_key(code, 0);
                int rep = code == K_UP || code == K_DOWN || code == K_LEFT || code == K_RIGHT || code == K_L2 || code == K_R2;
                if (rep) { held_key = code; next_repeat = now_t + 0.32; }
            } else if (val == 0 && code == held_key) held_key = -1;
        }
        if (held_key >= 0 && now_t >= next_repeat && !screen_off) {
            on_key(held_key, 1);
            next_repeat = now_t + ((held_key == K_LEFT || held_key == K_RIGHT) && screen == SCR_PLAYLIST ? 0.16 : 0.07);
        }
        if (audio_take_finished()) {
            errors = 0;
            if (S.repeat == 2 && current >= 0) play_index(current, 0, 0);
            else {
                int k = next_index(1);
                if (k >= 0) { play_index(k, 0, 0); if (screen != SCR_PLAYLIST || sel == current) sel = k; }
                else S.cur_pos_ms = 0;
            }
        }
        if (audio_take_error()) {
            const char *bad = pl_path(current);
            fprintf(stderr, "[play] could not open %s\n", bad ? bad : "?");
            show_toast("%s", "Couldn't play that file — skipping");
            if (++errors < pl_count()) { int k = next_index(1); if (k >= 0) play_index(k, 0, 0); }
        }
        if (sleep_at > 0 && now_t >= sleep_at) {
            sleep_at = 0; sleep_min = 0;
            audio_set_paused(1);
            show_toast("%s", "Sleep timer: paused");
        }
        if (now_t - last_save > 15) { save_all(); last_save = now_t; }
        if (screen == SCR_SKINS) skins_update();

        if (screen_off) { usleep(40000); continue; }
        vis_analyze(&vd, dt, audio_state() == A_PLAYING);
        draw_frame();
        double drawn = now();
        m2d_present();
        double spent = now() - now_t;
        if (profile) {
            static double acc, worst, t_last;
            static int frames;
            static long long cpu_last;
            acc += drawn - now_t;
            if (drawn - now_t > worst) worst = drawn - now_t;
            frames++;
            if (now_t - t_last > 10) {
                long long cpu = 0;
                FILE *ps = fopen("/proc/self/stat", "r");
                if (ps) {
                    char b[1024];
                    if (fgets(b, sizeof b, ps)) {
                        char *q = strrchr(b, ')');
                        unsigned long ut = 0, stt = 0;
                        if (q && sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &stt) == 2) cpu = (long long)(ut + stt);
                    }
                    fclose(ps);
                }
                double hz = (double)sysconf(_SC_CLK_TCK);
                if (t_last > 0) fprintf(stderr, "[profile] %d frames, draw avg %.1f ms max %.1f ms, cpu %.0f%%\n",
                                        frames, acc / frames * 1000, worst * 1000, (cpu - cpu_last) / hz / (now_t - t_last) * 100);
                cpu_last = cpu; t_last = now_t; acc = 0; worst = 0; frames = 0;
            }
        }
        double target = 1.0 / 30;
        if (spent < target) usleep((useconds_t)((target - spent) * 1e6));
    }
    if (screen_off) set_screen_off(0);
    save_all();
    audio_quit();
    pl_quit();
    m2d_quit();
    return 0;
}
