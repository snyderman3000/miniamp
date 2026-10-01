#ifndef SKIN_H
#define SKIN_H
#include <stdint.h>
#include "mini2d.h"

enum { SK_MAIN, SK_CBUTTONS, SK_TITLEBAR, SK_TEXT, SK_NUMBERS, SK_PLAYPAUS, SK_MONOSTER,
       SK_POSBAR, SK_VOLUME, SK_BALANCE, SK_SHUFREP, SK_COUNT };

typedef struct skin {
    m2d_surf *s[SK_COUNT];
    int own[SK_COUNT];
    int nums_ex;            // numbers sheet is NUMS_EX.BMP (has its own minus sign)
    uint32_t vis[24];       // viscolor.txt (ARGB)
    uint32_t pl_normal, pl_current, pl_bg, pl_selbg;  // pledit.txt
    char name[128];
} skin;

// Loads a .wsz (zip) or an unpacked skin folder. Sheets the skin lacks come
// from `fallback` (the default skin). Returns NULL if nothing usable was found.
skin *skin_load(const char *path, const skin *fallback);
void skin_free(skin *s);

// pressed-button ids for main window drawing
enum { PB_NONE, PB_PREV, PB_PLAY, PB_PAUSE, PB_STOP, PB_NEXT, PB_EJECT, PB_SHUFFLE, PB_REPEAT, PB_EQ, PB_PL };

typedef struct {
    int state;             // A_STOPPED / A_PLAYING / A_PAUSED
    int time_s;            // seconds shown
    int remaining;         // show minus sign
    int blink_off;         // hide digits (pause blink)
    const char *marquee;   // UTF-8 title text
    int marquee_px;        // scroll offset in pixels
    int kbps, khz, channels;
    int volume, balance;   // 0..100, -100..100
    int eq_on, pl_on;
    float pos;             // 0..1, <0 hides the thumb
    int shuffle, repeat;
    int pressed;           // PB_*
    int vis_mode;          // 0 off, 1 spectrum, 2 oscilloscope
    const float *bars;     // 19 bar heights 0..1 (spectrum)
    const float *peaks;    // 19 peak heights 0..1
    const float *scope;    // 76 samples -1..1
} mainwin_state;

#define MW_W 275
#define MW_H 116

void skin_draw_main(const skin *sk, m2d_surf *dst, const mainwin_state *st);
// draws text with the skin's TEXT.BMP font; returns width in pixels
int skin_text(const skin *sk, m2d_surf *dst, int x, int y, const char *utf8, int clip_x0, int clip_x1);
int skin_text_width(const char *utf8);

#endif
