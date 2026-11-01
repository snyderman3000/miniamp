// Plays a live stream through the decoder for a few seconds and reports what it got.
//   build/test_stream http://host:8780/stream.mp3 [seconds]
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "decode.h"
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    double secs = argc > 2 ? atof(argv[2]) : 3;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    dec_info info;
    decoder *d = dec_open(argv[1], &info);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (!d) { printf("open failed\n"); return 1; }
    printf("opened in %.2f s: %d Hz, %d ch\n", (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9, info.samplerate, info.channels);
    static float buf[4096 * 2];
    long total = 0; double sq = 0; int zc = 0; float prev = 0;
    while (total < secs * info.samplerate) {
        int n = dec_read(d, buf, 1024);
        struct timespec ts = { 0, (long)(1024.0 / info.samplerate * 1e9) };
        nanosleep(&ts, NULL);   // play at real speed, like the audio device
        if (n <= 0) { printf("stream ended early\n"); break; }
        for (int i = 0; i < n; i++) { float s = buf[i * info.channels]; sq += s * s; if ((s >= 0) != (prev >= 0)) zc++; prev = s; }
        total += n;
    }
    printf("decoded %.2f s, rms %.3f, ~%.0f Hz tone\n", (double)total / info.samplerate, sqrt(sq / total), zc / 2.0 / ((double)total / info.samplerate));
    dec_close(d);
    return 0;
}
