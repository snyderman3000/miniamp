// Audio file decoding: MP3 (dr_mp3), FLAC (dr_flac), WAV (dr_wav), Ogg Vorbis (stb_vorbis).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
#undef L
#undef C
#undef R

#include "decode.h"

enum { F_MP3, F_FLAC, F_WAV, F_OGG };

struct decoder {
    int fmt;
    int src_ch;          // channels in the file
    int out_ch;          // 1 or 2
    char *path;
    drmp3 mp3;
    drmp3_seek_point *seek_points;
    drflac *flac;
    drwav wav;
    stb_vorbis *ogg;
    float *tmp;          // for downmixing >2 channels
    int tmp_frames;
};

static const char *ext_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) return "";
    return dot + 1;
}

static int format_of(const char *path)
{
    const char *e = ext_of(path);
    if (!strcasecmp(e, "mp3")) return F_MP3;
    if (!strcasecmp(e, "flac")) return F_FLAC;
    if (!strcasecmp(e, "wav")) return F_WAV;
    if (!strcasecmp(e, "ogg") || !strcasecmp(e, "oga")) return F_OGG;
    return -1;
}

int dec_supported(const char *path) { return format_of(path) >= 0; }

decoder *dec_open(const char *path, dec_info *info)
{
    int fmt = format_of(path);
    if (fmt < 0) return NULL;
    decoder *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->fmt = fmt;
    memset(info, 0, sizeof *info);
    int ok = 0;
    switch (fmt) {
    case F_MP3:
        if (drmp3_init_file(&d->mp3, path, NULL)) {
            ok = 1;
            d->src_ch = d->mp3.channels;
            info->samplerate = d->mp3.sampleRate;
            if (d->mp3.totalPCMFrameCount != DRMP3_UINT64_MAX)
                info->frames = (int64_t)d->mp3.totalPCMFrameCount - d->mp3.delayInPCMFrames - d->mp3.paddingInPCMFrames;
        }
        break;
    case F_FLAC:
        d->flac = drflac_open_file(path, NULL);
        if (d->flac) {
            ok = 1;
            d->src_ch = d->flac->channels;
            info->samplerate = d->flac->sampleRate;
            info->frames = (int64_t)d->flac->totalPCMFrameCount;
        }
        break;
    case F_WAV:
        if (drwav_init_file(&d->wav, path, NULL)) {
            ok = 1;
            d->src_ch = d->wav.channels;
            info->samplerate = d->wav.sampleRate;
            info->frames = (int64_t)d->wav.totalPCMFrameCount;
            info->kbps = (int)((int64_t)d->wav.sampleRate * d->wav.channels * d->wav.bitsPerSample / 1000);
        }
        break;
    case F_OGG: {
        int err = 0;
        d->ogg = stb_vorbis_open_filename(path, &err, NULL);
        if (d->ogg) {
            stb_vorbis_info vi = stb_vorbis_get_info(d->ogg);
            ok = 1;
            d->src_ch = vi.channels;
            info->samplerate = vi.sample_rate;
            info->frames = stb_vorbis_stream_length_in_samples(d->ogg);
        }
        break;
    }
    }
    if (!ok || d->src_ch < 1 || info->samplerate < 8000) {
        dec_close(d);
        return NULL;
    }
    d->path = strdup(path);
    d->out_ch = d->src_ch >= 2 ? 2 : 1;
    info->channels = d->out_ch;
    return d;
}

static int read_raw(decoder *d, float *out, int frames)
{
    switch (d->fmt) {
    case F_MP3: return (int)drmp3_read_pcm_frames_f32(&d->mp3, frames, out);
    case F_FLAC: return (int)drflac_read_pcm_frames_f32(d->flac, frames, out);
    case F_WAV: return (int)drwav_read_pcm_frames_f32(&d->wav, frames, out);
    case F_OGG: return stb_vorbis_get_samples_float_interleaved(d->ogg, d->src_ch, out, frames * d->src_ch);
    }
    return 0;
}

int dec_read(decoder *d, float *out, int frames)
{
    if (d->src_ch <= 2) return read_raw(d, out, frames);
    // downmix surround to stereo: odd channels left, even channels right
    if (d->tmp_frames < frames) {
        free(d->tmp);
        d->tmp = malloc(sizeof(float) * frames * d->src_ch);
        d->tmp_frames = d->tmp ? frames : 0;
        if (!d->tmp) return 0;
    }
    int n = read_raw(d, d->tmp, frames);
    int c = d->src_ch;
    for (int i = 0; i < n; i++) {
        float l = 0, r = 0;
        for (int k = 0; k < c; k++) { if (k & 1) r += d->tmp[i * c + k]; else l += d->tmp[i * c + k]; }
        out[i * 2] = l / ((c + 1) / 2);
        out[i * 2 + 1] = r / (c / 2);
    }
    return n;
}

// MP3 seeking without a table decodes everything up to the target, so the
// first seek scans the file's frame headers (no decoding) to build one.
static void mp3_build_seek_table(decoder *d)
{
    drmp3 scan;
    if (!drmp3_init_file(&scan, d->path, NULL)) return;
    drmp3_uint32 n = 1024;
    drmp3_seek_point *pts = malloc(sizeof *pts * n);
    if (pts && drmp3_calculate_seek_points(&scan, &n, pts) && n > 0) {
        d->seek_points = pts;
        drmp3_bind_seek_table(&d->mp3, n, pts);
    } else {
        free(pts);
    }
    drmp3_uninit(&scan);
}

int64_t dec_seek(decoder *d, int64_t frame)
{
    if (frame < 0) frame = 0;
    switch (d->fmt) {
    case F_MP3:
        if (!d->seek_points) mp3_build_seek_table(d);
        if (!drmp3_seek_to_pcm_frame(&d->mp3, (drmp3_uint64)frame)) return -1;
        return (int64_t)d->mp3.currentPCMFrame;
    case F_FLAC:
        return drflac_seek_to_pcm_frame(d->flac, (drflac_uint64)frame) ? frame : -1;
    case F_WAV:
        return drwav_seek_to_pcm_frame(&d->wav, (drwav_uint64)frame) ? frame : -1;
    case F_OGG:
        return stb_vorbis_seek(d->ogg, (unsigned)frame) ? frame : -1;
    }
    return -1;
}

void dec_close(decoder *d)
{
    if (!d) return;
    switch (d->fmt) {
    case F_MP3: if (d->mp3.onRead) drmp3_uninit(&d->mp3); break;
    case F_FLAC: if (d->flac) drflac_close(d->flac); break;
    case F_WAV: if (d->wav.onRead) drwav_uninit(&d->wav); break;
    case F_OGG: if (d->ogg) stb_vorbis_close(d->ogg); break;
    }
    free(d->seek_points);
    free(d->tmp);
    free(d->path);
    free(d);
}
