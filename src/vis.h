#ifndef VIS_H_INCLUDED
#define VIS_H_INCLUDED
#include <stdint.h>
#include "mini2d.h"

#define VIS_W 320
#define VIS_H 240
#define VIS_FFT 1024

typedef struct {
    float wave[VIS_FFT];       // latest mono samples
    float mag[VIS_FFT / 2];    // spectrum magnitude (0..~1, log-ish)
    float bars19[19], peaks19[19];
    float bars40[40], peaks40[40];
    float scope76[76];
    float bass, level;         // 0..1 smoothed
    int beat;                  // 1 on the frame a beat is detected
    double t;                  // seconds
} vis_data;

// pulls fresh samples from the player and updates everything (dt in seconds)
void vis_analyze(vis_data *v, float dt, int playing);

#define VIS_PRESETS 5
const char *vis_preset_name(int i);
// renders one frame of a full-screen preset into a VIS_W x VIS_H surface
void vis_render(int preset, const vis_data *v, m2d_surf *out, const uint32_t *skin_vis);

#endif
