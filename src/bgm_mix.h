// Mixing MiniAmp's background music into a program's own audio, at the
// program's sample rate. Used by the libpadsp wrapper (padsp_wrap.c); kept
// separate so it can be tested on its own (tools/test_mix.c).
//
// The music in the ring is at BGM_RATE. A program can play at any rate
// (OnionOS's menu uses 48000, Mario 64 32000, Balatro 44100, Petals Around
// the Rose 22050), so the music is resampled to it with linear interpolation.
// Without that it would play too slow or too fast.
#ifndef BGM_MIX_H
#define BGM_MIX_H
#include <stddef.h>
#include <stdint.h>

static inline int16_t bgm_sat(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

// Step through the music (16.16 fixed point) per output frame at this rate.
static inline uint32_t bgm_step(int out_rate, int music_rate)
{
    if (out_rate <= 0 || out_rate == music_rate) return 65536u;
    if (out_rate < 4000) out_rate = 4000;
    return (uint32_t)(((uint64_t)music_rate << 16) / (uint64_t)out_rate);
}

// Mixes `frames` frames of `in` (ch channels) with music read from `ring`
// (stereo, `ring_mask`+1 frames), starting `r0` frames in plus `frac`/65536 of
// a frame, into `out`. `avail` is how many music frames are ready. Music runs
// out (underrun) silently. Returns the music position after `frames` output
// frames as 16.16 relative to r0; the caller advances by what it actually
// wrote with bgm_advance.
static inline void bgm_mix(int16_t *out, const int16_t *in, size_t frames, int ch,
                           const int16_t *ring, uint32_t ring_mask, uint32_t r0, uint32_t avail,
                           uint32_t frac, uint32_t step, int vol, int *underrun)
{
    uint64_t pos = frac;
    for (size_t i = 0; i < frames; i++, pos += step) {
        uint32_t idx = (uint32_t)(pos >> 16);
        int ml = 0, mr = 0;
        if (idx < avail) {
            const int16_t *a = &ring[((r0 + idx) & ring_mask) * 2];
            if (step == 65536u || idx + 1 >= avail) {
                ml = a[0];
                mr = a[1];
            } else {
                const int16_t *b = &ring[((r0 + idx + 1) & ring_mask) * 2];
                int f = (int)(pos & 0xffff);
                ml = a[0] + (int)(((int64_t)(b[0] - a[0]) * f) >> 16);
                mr = a[1] + (int)(((int64_t)(b[1] - a[1]) * f) >> 16);
            }
            ml = ml * vol >> 8;
            mr = mr * vol >> 8;
        } else if (underrun) {
            *underrun = 1;
        }
        if (ch == 2) {
            out[i * 2] = bgm_sat(in[i * 2] + ml);
            out[i * 2 + 1] = bgm_sat(in[i * 2 + 1] + mr);
        } else {
            out[i] = bgm_sat(in[i] + (ml + mr) / 2);
        }
    }
}

// After `written` output frames: how many whole music frames were used (at
// most avail), and the leftover fraction for next time.
static inline uint32_t bgm_advance(size_t written, uint32_t frac, uint32_t step, uint32_t avail, uint32_t *frac_out)
{
    uint64_t pos = (uint64_t)frac + (uint64_t)written * step;
    uint32_t used = (uint32_t)(pos >> 16);
    if (used >= avail) {
        *frac_out = 0;
        return avail;
    }
    *frac_out = (uint32_t)(pos & 0xffff);
    return used;
}

#endif
