// Playback engine: one thread decodes, resamples to 44.1 kHz, applies the
// equalizer and volume, and writes 16-bit stereo to the output.
//
// Output on the Miyoo is OSS (/dev/dsp); OnionOS routes it to its audioserver
// when libpadsp.so is preloaded (see launch.sh). Without a device (desktop
// builds) the stream goes to a WAV file (MA_WAV_OUT) or is timed and dropped.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

#include "audio.h"
#include "decode.h"

const int EQ_FREQ[EQ_BANDS] = { 60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000 };

#define CHUNK 1024
#define WAVE_N 8192

static pthread_t thr;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int running;

// commands (under mtx)
static char *cmd_path;
static int cmd_open, cmd_start_ms, cmd_paused;
static int cmd_seek = -1, cmd_stop;

// status (under mtx)
static int st_state = A_STOPPED;
static int st_rate, st_ch, st_kbps;
static long long st_len_ms;
static int finished_flag, error_flag;

// position: base_ms + frames written since base
static volatile long long base_ms;
static volatile long long out_frames;

// settings (read by the audio thread)
static volatile float gain_l = 0.5f, gain_r = 0.5f;
static volatile int vol_v = 50, bal_v;
static pthread_mutex_t eq_mtx = PTHREAD_MUTEX_INITIALIZER;
static int eq_dirty = 1, eq_on;
static float eq_pre_db, eq_db[EQ_BANDS];

// visualization ring buffer (mono)
static float wave[WAVE_N];
static volatile unsigned wave_w;
static int out_latency = 2048;

// output
static int dsp_fd = -1;
static FILE *wav_out;
static long wav_bytes;
static int fast;

// ---------------------------------------------------------------- output
static void wav_header(FILE *f, long data)
{
    unsigned char h[44];
    memcpy(h, "RIFF", 4);
    unsigned v = (unsigned)(data + 36);
    h[4] = v; h[5] = v >> 8; h[6] = v >> 16; h[7] = v >> 24;
    memcpy(h + 8, "WAVEfmt ", 8);
    unsigned char fmt[16] = { 16, 0, 0, 0, 1, 0, 2, 0, 0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0 };
    memcpy(h + 16, fmt, 16);
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    v = (unsigned)data;
    h[40] = v; h[41] = v >> 8; h[42] = v >> 16; h[43] = v >> 24;
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

static int out_open(void)
{
    const char *wp = getenv("MA_WAV_OUT");
    fast = getenv("MA_FAST") != NULL;
    if (wp) {
        wav_out = fopen(wp, "wb");
        if (wav_out) wav_header(wav_out, 0);
        return 0;
    }
    if (getenv("MA_NO_DSP")) return 0;
    dsp_fd = open("/dev/dsp", O_WRONLY);
    if (dsp_fd < 0) { fprintf(stderr, "[audio] no /dev/dsp, playing silently\n"); return 0; }
    int frag = (4 << 16) | 12;  // 4 fragments of 4 KB (~93 ms)
    ioctl(dsp_fd, SNDCTL_DSP_SETFRAGMENT, &frag);
    int fmt = AFMT_S16_LE, ch = 2, rate = OUT_RATE;
    ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &fmt);
    ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &ch);
    ioctl(dsp_fd, SNDCTL_DSP_SPEED, &rate);
    fprintf(stderr, "[audio] /dev/dsp open: fmt %d ch %d rate %d\n", fmt, ch, rate);
    return 1;
}

static void out_write(const int16_t *s, int frames)
{
    if (dsp_fd >= 0) {
        const char *p = (const char *)s;
        size_t left = (size_t)frames * 4;
        while (left > 0) {
            ssize_t n = write(dsp_fd, p, left);
            if (n <= 0) { usleep(5000); if (!running) return; continue; }
            p += n; left -= (size_t)n;
        }
        return;
    }
    if (wav_out) { fwrite(s, 4, frames, wav_out); wav_bytes += frames * 4; }
    if (!fast) {
        // pace like a real device
        static struct timespec next;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (next.tv_sec == 0 || now.tv_sec - next.tv_sec > 1) next = now;
        long ns = (long)((long long)frames * 1000000000LL / OUT_RATE);
        next.tv_nsec += ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
}

// ---------------------------------------------------------------- equalizer
typedef struct { float b0, b1, b2, a1, a2; float z1[2], z2[2]; } biquad;
static biquad eq[EQ_BANDS];
static int eq_active;
static float eq_pre = 1;

static void eq_update(void)
{
    pthread_mutex_lock(&eq_mtx);
    if (!eq_dirty) { pthread_mutex_unlock(&eq_mtx); return; }
    eq_dirty = 0;
    int on = eq_on;
    float pre = eq_pre_db, db[EQ_BANDS];
    memcpy(db, eq_db, sizeof db);
    pthread_mutex_unlock(&eq_mtx);

    int any = 0;
    for (int i = 0; i < EQ_BANDS; i++) if (fabsf(db[i]) > 0.05f) any = 1;
    eq_active = on && (any || fabsf(pre) > 0.05f);
    eq_pre = on ? powf(10.f, pre / 20.f) : 1.f;
    for (int i = 0; i < EQ_BANDS; i++) {
        float A = powf(10.f, db[i] / 40.f);
        float w0 = 2.f * (float)M_PI * EQ_FREQ[i] / OUT_RATE;
        float alpha = sinf(w0) / (2.f * 1.2f);
        float c = cosf(w0);
        float a0 = 1 + alpha / A;
        biquad *q = &eq[i];
        q->b0 = (1 + alpha * A) / a0;
        q->b1 = -2 * c / a0;
        q->b2 = (1 - alpha * A) / a0;
        q->a1 = -2 * c / a0;
        q->a2 = (1 - alpha / A) / a0;
        if (fabsf(db[i]) <= 0.05f) { q->b0 = 1; q->b1 = q->b2 = q->a1 = q->a2 = 0; }
    }
}

static void eq_reset_state(void)
{
    for (int i = 0; i < EQ_BANDS; i++) memset(eq[i].z1, 0, sizeof eq[i].z1), memset(eq[i].z2, 0, sizeof eq[i].z2);
}

static void eq_process(float *x, int frames)
{
    if (!eq_active) return;
    for (int i = 0; i < EQ_BANDS; i++) {
        biquad *q = &eq[i];
        if (q->b0 == 1 && q->b1 == 0 && q->a1 == 0) continue;
        for (int c = 0; c < 2; c++) {
            float z1 = q->z1[c], z2 = q->z2[c];
            for (int n = 0; n < frames; n++) {
                float in = x[n * 2 + c];
                float out = q->b0 * in + z1;           // transposed direct form II
                z1 = q->b1 * in - q->a1 * out + z2;
                z2 = q->b2 * in - q->a2 * out;
                x[n * 2 + c] = out;
            }
            q->z1[c] = z1; q->z2[c] = z2;
        }
    }
    if (eq_pre != 1.f) for (int n = 0; n < frames * 2; n++) x[n] *= eq_pre;
}

// ---------------------------------------------------------------- source + resampler
static decoder *dec;
static dec_info info;
#define RBUF 4096
static float rbuf[(RBUF + 8) * 2];   // stereo frames
static int r_avail;                  // valid frames in rbuf
static double r_pos;                 // read position (frames) in rbuf
static int r_eof, r_end;             // decoder exhausted; frame index where real data ends

static void src_reset(void)
{
    memset(rbuf, 0, sizeof rbuf);
    r_avail = 1;      // one frame of silent history for the interpolator
    r_pos = 1;
    r_eof = 0;
    r_end = 1 << 30;
}

// read frames from the decoder as stereo into dst; returns frames
static int src_pull(float *dst, int frames)
{
    if (!dec) return 0;
    if (info.channels == 2) return dec_read(dec, dst, frames);
    static float mono[RBUF];
    if (frames > RBUF) frames = RBUF;
    int n = dec_read(dec, mono, frames);
    for (int i = 0; i < n; i++) dst[i * 2] = dst[i * 2 + 1] = mono[i];
    return n;
}

static void src_fill(void)
{
    // keep 1 frame of history before r_pos
    int keep_from = (int)r_pos - 1;
    if (keep_from > 0) {
        memmove(rbuf, rbuf + keep_from * 2, sizeof(float) * 2 * (r_avail - keep_from));
        r_avail -= keep_from;
        r_pos -= keep_from;
        if (r_end != (1 << 30)) r_end -= keep_from;
    }
    while (r_avail < RBUF && !r_eof) {
        int n = src_pull(rbuf + r_avail * 2, RBUF - r_avail);
        if (n <= 0) {
            r_eof = 1;
            r_end = r_avail;
            // pad so the interpolator can run off the end
            memset(rbuf + r_avail * 2, 0, sizeof(float) * 2 * 4);
            r_avail += 4;
            break;
        }
        r_avail += n;
    }
}

// produces up to `frames` output frames at OUT_RATE; returns count (0 = end)
static int src_render(float *out, int frames)
{
    double step = (double)info.samplerate / OUT_RATE;
    int made = 0;
    while (made < frames) {
        int ip = (int)r_pos;
        if (ip >= r_end) break;
        if (ip + 2 >= r_avail) {
            if (r_eof && ip + 2 >= r_avail) break;
            src_fill();
            continue;
        }
        if (step == 1.0) {
            // no resampling: copy straight
            int n = r_avail - 2 - ip;
            if (ip + n > r_end) n = r_end - ip;
            if (n > frames - made) n = frames - made;
            if (n <= 0) { src_fill(); if (r_eof && (int)r_pos >= r_end) break; continue; }
            memcpy(out + made * 2, rbuf + ip * 2, sizeof(float) * 2 * n);
            made += n;
            r_pos += n;
            continue;
        }
        float t = (float)(r_pos - ip);
        for (int c = 0; c < 2; c++) {
            float y0 = rbuf[(ip - 1) * 2 + c], y1 = rbuf[ip * 2 + c];
            float y2 = rbuf[(ip + 1) * 2 + c], y3 = rbuf[(ip + 2) * 2 + c];
            // Catmull-Rom (cubic Hermite)
            float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
            float b = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
            float cc = -0.5f * y0 + 0.5f * y2;
            out[made * 2 + c] = ((a * t + b) * t + cc) * t + y1;
        }
        made++;
        r_pos += step;
    }
    return made;
}

// ---------------------------------------------------------------- thread
static void close_track(void)
{
    if (dec) dec_close(dec);
    dec = NULL;
}

static void *audio_thread(void *arg)
{
    (void)arg;
    struct sched_param sp = { .sched_priority = 10 };
    pthread_setschedparam(pthread_self(), SCHED_RR, &sp);  // best effort
    static float fbuf[CHUNK * 2];
    static int16_t sbuf[CHUNK * 2];
    while (1) {
        pthread_mutex_lock(&mtx);
        while (running && !cmd_open && cmd_seek < 0 && !cmd_stop && st_state != A_PLAYING)
            pthread_cond_wait(&cv, &mtx);
        if (!running) { pthread_mutex_unlock(&mtx); break; }
        if (cmd_stop) {
            cmd_stop = 0;
            close_track();
            st_state = A_STOPPED;
            base_ms = 0; out_frames = 0;
        }
        if (cmd_open) {
            char *path = cmd_path;
            cmd_path = NULL;
            cmd_open = 0;
            int start = cmd_start_ms, paused = cmd_paused;
            pthread_mutex_unlock(&mtx);
            close_track();
            dec_info di;
            decoder *d = dec_open(path, &di);
            free(path);
            int64_t reached = 0;
            if (d && start > 0) {
                reached = dec_seek(d, (int64_t)start * di.samplerate / 1000);
                if (reached < 0) reached = 0;
            }
            pthread_mutex_lock(&mtx);
            dec = d;
            if (d) {
                info = di;
                st_rate = di.samplerate;
                st_ch = di.channels;
                st_kbps = di.kbps;
                st_len_ms = di.frames > 0 ? di.frames * 1000 / di.samplerate : 0;
                st_state = paused ? A_PAUSED : A_PLAYING;
                base_ms = reached * 1000 / di.samplerate;
                out_frames = 0;
                src_reset();
                eq_reset_state();
            } else {
                st_state = A_STOPPED;
                error_flag = 1;
            }
        }
        if (cmd_seek >= 0) {
            int ms = cmd_seek;
            cmd_seek = -1;
            if (dec) {
                int64_t r = dec_seek(dec, (int64_t)ms * info.samplerate / 1000);
                if (r >= 0) {
                    base_ms = r * 1000 / info.samplerate;
                    out_frames = 0;
                    src_reset();
                    eq_reset_state();
                }
            }
        }
        int playing = st_state == A_PLAYING && dec;
        pthread_mutex_unlock(&mtx);
        if (!playing) continue;

        eq_update();
        int n = src_render(fbuf, CHUNK);
        if (n <= 0) {
            pthread_mutex_lock(&mtx);
            close_track();
            st_state = A_STOPPED;
            finished_flag = 1;
            pthread_mutex_unlock(&mtx);
            continue;
        }
        eq_process(fbuf, n);
        float gl = gain_l, gr = gain_r;
        unsigned w = wave_w;
        for (int i = 0; i < n; i++) {
            float l = fbuf[i * 2], r = fbuf[i * 2 + 1];
            wave[(w + i) & (WAVE_N - 1)] = (l + r) * 0.5f;
            l *= gl; r *= gr;
            int il = (int)lrintf(l * 32767.f), ir = (int)lrintf(r * 32767.f);
            if (il > 32767) il = 32767; else if (il < -32768) il = -32768;
            if (ir > 32767) ir = 32767; else if (ir < -32768) ir = -32768;
            sbuf[i * 2] = (int16_t)il;
            sbuf[i * 2 + 1] = (int16_t)ir;
        }
        wave_w = w + n;
        out_write(sbuf, n);
        out_frames += n;
    }
    return NULL;
}

// ---------------------------------------------------------------- API
int audio_init(void)
{
    int real = out_open();
    const char *lat = getenv("MA_VIS_LATENCY");
    if (lat) out_latency = atoi(lat);
    else if (!real) out_latency = 0;
    running = 1;
    src_reset();
    pthread_create(&thr, NULL, audio_thread, NULL);
    return real;
}

void audio_quit(void)
{
    pthread_mutex_lock(&mtx);
    running = 0;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mtx);
    pthread_join(thr, NULL);
    close_track();
    if (dsp_fd >= 0) close(dsp_fd);
    if (wav_out) { wav_header(wav_out, wav_bytes); fclose(wav_out); }
}

void audio_play_file(const char *path, int start_ms, int paused)
{
    pthread_mutex_lock(&mtx);
    free(cmd_path);
    cmd_path = strdup(path);
    cmd_open = 1;
    cmd_start_ms = start_ms;
    cmd_paused = paused;
    cmd_seek = -1;
    finished_flag = 0;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mtx);
}

void audio_set_paused(int paused)
{
    pthread_mutex_lock(&mtx);
    if (cmd_open) cmd_paused = paused;
    else if (st_state != A_STOPPED) st_state = paused ? A_PAUSED : A_PLAYING;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mtx);
}

void audio_stop(void)
{
    pthread_mutex_lock(&mtx);
    free(cmd_path); cmd_path = NULL;
    cmd_open = 0;
    cmd_stop = 1;
    finished_flag = 0;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mtx);
}

void audio_seek(int ms)
{
    pthread_mutex_lock(&mtx);
    if (cmd_open) cmd_start_ms = ms < 0 ? 0 : ms;
    else cmd_seek = ms < 0 ? 0 : ms;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mtx);
}

int audio_state(void)
{
    pthread_mutex_lock(&mtx);
    int s = cmd_open ? (cmd_paused ? A_PAUSED : A_PLAYING) : st_state;
    pthread_mutex_unlock(&mtx);
    return s;
}

int audio_pos_ms(void)
{
    pthread_mutex_lock(&mtx);
    long long p = cmd_open ? cmd_start_ms : base_ms + out_frames * 1000 / OUT_RATE - (long long)out_latency * 1000 / OUT_RATE;
    if (cmd_seek >= 0) p = cmd_seek;
    pthread_mutex_unlock(&mtx);
    if (p < 0) p = 0;
    return (int)p;
}

int audio_len_ms(void)
{
    pthread_mutex_lock(&mtx);
    int l = (int)st_len_ms;
    pthread_mutex_unlock(&mtx);
    return l;
}

int audio_take_finished(void)
{
    pthread_mutex_lock(&mtx);
    int f = finished_flag;
    finished_flag = 0;
    pthread_mutex_unlock(&mtx);
    return f;
}

int audio_take_error(void)
{
    pthread_mutex_lock(&mtx);
    int f = error_flag;
    error_flag = 0;
    pthread_mutex_unlock(&mtx);
    return f;
}

void audio_stream_info(int *rate, int *channels, int *kbps)
{
    pthread_mutex_lock(&mtx);
    *rate = st_rate; *channels = st_ch; *kbps = st_kbps;
    pthread_mutex_unlock(&mtx);
}

static void update_gains(void)
{
    float g = vol_v / 100.f;
    g = g * g;  // perceptual curve
    float l = g, r = g;
    if (bal_v > 0) l *= 1.f - bal_v / 100.f;
    if (bal_v < 0) r *= 1.f + bal_v / 100.f;
    gain_l = l; gain_r = r;
}

void audio_set_volume(int v) { vol_v = v < 0 ? 0 : v > 100 ? 100 : v; update_gains(); }
void audio_set_balance(int b) { bal_v = b < -100 ? -100 : b > 100 ? 100 : b; update_gains(); }

void audio_set_eq(int on, float preamp_db, const float bands_db[EQ_BANDS])
{
    pthread_mutex_lock(&eq_mtx);
    eq_on = on;
    eq_pre_db = preamp_db;
    memcpy(eq_db, bands_db, sizeof eq_db);
    eq_dirty = 1;
    pthread_mutex_unlock(&eq_mtx);
}

int audio_get_wave(float *out, int n)
{
    if (n > WAVE_N / 2) n = WAVE_N / 2;
    unsigned end = wave_w - (unsigned)out_latency;
    for (int i = 0; i < n; i++) out[i] = wave[(end - n + i) & (WAVE_N - 1)];
    return n;
}
