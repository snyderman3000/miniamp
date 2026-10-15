// Host test for bgm_mix.h: the background music must keep its pitch and speed
// whatever rate the game plays at.
//   gcc -O2 -Isrc tools/test_mix.c -o build/test_mix -lm && build/test_mix
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bgm_shm.h"
#include "bgm_mix.h"

#define RING_FRAMES BGM_RING
static int16_t ring[RING_FRAMES * 2];
static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// a 1 kHz tone in the ring, at BGM_RATE
static void fill_tone(double hz)
{
    for (int i = 0; i < RING_FRAMES; i++) {
        int16_t v = (int16_t)(12000 * sin(2 * M_PI * hz * i / BGM_RATE));
        ring[i * 2] = v;
        ring[i * 2 + 1] = v;
    }
}

// Plays `seconds` of music into silent game audio at `rate`, in writes of
// `chunk` frames where the device accepts only `accept` of them each time
// (like a full pipe), and measures the tone's frequency in the output.
static double measure(int rate, int ch, size_t chunk, size_t accept, double seconds, uint32_t *music_used)
{
    size_t total = (size_t)(rate * seconds);
    int16_t *in = calloc(total * ch, 2), *out = calloc(total * ch, 2), *tmp = calloc(chunk * ch, 2);
    uint32_t rpos = 0, frac = 0, step = bgm_step(rate, BGM_RATE);
    size_t done = 0;
    while (done < total) {
        size_t n = total - done < chunk ? total - done : chunk;
        uint32_t avail = RING_FRAMES - 2;  // plenty ready (the ring wraps, the tone is continuous enough)
        bgm_mix(tmp, in + done * ch, n, ch, ring, RING_FRAMES - 1, rpos, avail, frac, step, 256, NULL);
        size_t w = n < accept ? n : accept;
        memcpy(out + done * ch, tmp, w * ch * 2);
        rpos += bgm_advance(w, frac, step, avail, &frac);
        done += w;
    }
    // count rising zero crossings on the first channel, skipping the start
    int crossings = 0;
    size_t first = 0, last = 0;
    for (size_t i = rate / 10 + 1; i < total; i++) {
        if (out[(i - 1) * ch] < 0 && out[i * ch] >= 0) {
            if (!crossings) first = i;
            last = i;
            crossings++;
        }
    }
    *music_used = rpos;
    free(in); free(out); free(tmp);
    return crossings > 1 ? (crossings - 1) * (double)rate / (double)(last - first) : 0;
}

int main(void)
{
    // A tone that fits the ring exactly, so wrapping around doesn't add a click.
    double tone = BGM_RATE / 48.0;  // 1000 Hz at 48 kHz
    fill_tone(tone);
    int rates[] = {48000, 44100, 32000, 22050, 11025};
    for (unsigned i = 0; i < sizeof rates / sizeof *rates; i++) {
        for (int ch = 1; ch <= 2; ch++) {
            uint32_t used;
            double hz = measure(rates[i], ch, 1024, 1024, 2.0, &used);
            double speed = (double)used / (2.0 * BGM_RATE);
            printf("%6d Hz, %d ch: tone %.1f Hz (want %.0f), music used %.4fx real time\n", rates[i], ch, hz, tone, speed);
            CHECK(fabs(hz - tone) < tone * 0.005, "%d Hz: pitch %.1f", rates[i], hz);
            CHECK(fabs(speed - 1.0) < 0.002, "%d Hz: speed %.4f", rates[i], speed);
        }
    }
    // The device takes only part of each write: music must stay in step.
    uint32_t used;
    double hz = measure(32000, 2, 1024, 700, 2.0, &used);
    CHECK(fabs(hz - tone) < tone * 0.005, "partial writes: pitch %.1f", hz);
    CHECK(fabs((double)used / (2.0 * BGM_RATE) - 1.0) < 0.002, "partial writes: speed");

    // At the music's own rate the samples pass through unchanged.
    int16_t in[8] = {0}, out[8];
    bgm_mix(out, in, 4, 2, ring, RING_FRAMES - 1, 100, 1000, 0, bgm_step(48000, BGM_RATE), 256, NULL);
    CHECK(!memcmp(out, &ring[200], sizeof out), "48 kHz isn't a straight copy");

    // Running out of music: the game's own sound still plays, music is silent, flagged.
    int16_t g[8] = {100, 100, 100, 100, 100, 100, 100, 100};
    int under = 0;
    bgm_mix(out, g, 4, 2, ring, RING_FRAMES - 1, 0, 2, 0, 65536u, 0, &under);
    CHECK(under == 1 && out[6] == 100, "underrun");
    uint32_t frac;
    CHECK(bgm_advance(4, 0, 65536u, 2, &frac) == 2 && frac == 0, "advance stops at what's there");

    // Loud game + loud music clips instead of wrapping around.
    int16_t loud[2] = {30000, -30000}, lring[4] = {30000, -30000, 30000, -30000};
    bgm_mix(out, loud, 1, 2, lring, 1, 0, 2, 0, 65536u, 256, NULL);
    CHECK(out[0] == 32767 && out[1] == -32768, "clipping");

    printf(fails ? "%d FAILED\n" : "all good\n", fails);
    return fails != 0;
}
