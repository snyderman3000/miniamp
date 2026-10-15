// A "port" that plays silence straight to the chip, like steward-fu's SDL2.
// Run with the hook preloaded and a music feed in place; it checks that the
// music arrived at the right pitch and speed, and the game's buffer was left alone.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef struct { int eSamplerate, eBitwidth, eWorkmode, eSoundmode; uint32_t a, b, c, d; } attr_t;
typedef struct { int eBitwidth, eSoundmode; void *apVirAddr[16]; uint64_t ts; uint32_t seq, u32Len, pool[2]; void *src[16]; uint32_t srclen; } frame_t;
int MI_AO_SetPubAttr(int, attr_t *);
int MI_AO_SendFrame(int, int, frame_t *, int);
int MI_AO_Disable(int);
extern int16_t *chip_out;
extern size_t chip_len;
int main(int argc, char **argv)
{
    int rate = argc > 1 ? atoi(argv[1]) : 22050, ch = argc > 2 ? atoi(argv[2]) : 2;
    attr_t a = { rate, 0, 0, ch == 2 ? 1 : 0, 6, 1024, 0, (uint32_t)ch };
    MI_AO_SetPubAttr(0, &a);
    static int16_t mix[1024 * 2];
    int blocks = rate * 2 / 1024;  // ~2 s
    for (int i = 0; i < blocks; i++) {
        memset(mix, 0, sizeof mix);
        frame_t f = {0};
        f.eBitwidth = 0; f.eSoundmode = a.eSoundmode; f.apVirAddr[0] = mix; f.u32Len = 1024 * 2 * ch;
        MI_AO_SendFrame(0, 0, &f, 1);
        for (int k = 0; k < 1024 * ch; k++) if (mix[k]) { printf("FAIL: the game's buffer was changed\n"); return 1; }
    }
    MI_AO_Disable(0);
    size_t frames = chip_len / ch;
    int crossings = 0; size_t first = 0, last = 0;
    for (size_t i = rate / 10 + 1; i < frames; i++)
        if (chip_out[(i - 1) * ch] < 0 && chip_out[i * ch] >= 0) { if (!crossings) first = i; last = i; crossings++; }
    double hz = crossings > 1 ? (crossings - 1) * (double)rate / (double)(last - first) : 0;
    printf("%d Hz %d ch: music reached the chip at %.1f Hz (want 1000)\n", rate, ch, hz);
    return fabs(hz - 1000) < 5 ? 0 : 1;
}
