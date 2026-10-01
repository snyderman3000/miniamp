// checks decoding, seeking and tag reading on a few files
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "decode.h"
#include "meta.h"
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        track_meta m; meta_read(argv[i], &m);
        char name[300]; meta_display_name(&m, argv[i], name, sizeof name);
        dec_info di; decoder *d = dec_open(argv[i], &di);
        printf("%s\n  meta: '%s' album='%s' dur=%dms kbps=%d sr=%d ch=%d\n", argv[i], name, m.album, m.dur_ms, m.kbps, m.samplerate, m.channels);
        if (!d) { printf("  OPEN FAILED\n"); continue; }
        printf("  dec: sr=%d ch=%d frames=%lld (%.2fs)\n", di.samplerate, di.channels, (long long)di.frames, di.frames / (double)di.samplerate);
        static float buf[4096 * 2];
        double t0 = now();
        long long target = di.frames > 0 ? di.frames * 3 / 4 : di.samplerate * 10;
        long long r = dec_seek(d, target);
        double t1 = now();
        int n = dec_read(d, buf, 4096);
        double e = 0; for (int k = 0; k < n * di.channels; k++) e += buf[k] * buf[k];
        printf("  seek to %lld -> %lld in %.1f ms, read %d frames rms %.3f\n", target, r, (t1 - t0) * 1000, n, sqrt(e / (n * di.channels + 1)));
        r = dec_seek(d, 1000); n = dec_read(d, buf, 4096);
        printf("  seek back to 1000 -> %lld, read %d\n", r, n);
        long long total = 0; dec_seek(d, 0); while ((n = dec_read(d, buf, 4096)) > 0) total += n;
        printf("  full decode frames %lld\n", total);
        dec_close(d);
    }
}
