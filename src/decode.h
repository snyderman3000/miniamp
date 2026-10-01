#ifndef DECODE_H
#define DECODE_H
#include <stdint.h>

typedef struct decoder decoder;

typedef struct {
    int samplerate;
    int channels;      // 1 or 2 after decoding (more are downmixed)
    int kbps;          // average bitrate, 0 if unknown
    int64_t frames;    // total PCM frames, 0 if unknown
} dec_info;

int dec_supported(const char *path);              // by file extension
decoder *dec_open(const char *path, dec_info *info);
// reads up to `frames` frames of interleaved float (info.channels) into out
int dec_read(decoder *d, float *out, int frames);
// seeks to a PCM frame; returns the frame actually reached (may be approximate for MP3)
int64_t dec_seek(decoder *d, int64_t frame);
void dec_close(decoder *d);

#endif
