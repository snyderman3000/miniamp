// Background playback test for OnionOS: detaches from the launcher and plays
// music while you start a game, logging whether the audio keeps flowing.
//   bgtest start <logfile> <pidfile>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sched.h>
#include <ctype.h>
#include "decode.h"

// ---- diagnostics: which processes use the CPU, and scheduling experiments
typedef struct { int pid; long ticks; char comm[32]; } pinfo;
static pinfo prev_p[512];
static int nprev;

static long proc_ticks(int pid, char *comm)
{
    char path[64], b[512];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long t = -1;
    if (fgets(b, sizeof b, f)) {
        char *l = strchr(b, '('), *r = strrchr(b, ')');
        if (l && r) {
            int n = (int)(r - l - 1); if (n > 31) n = 31;
            memcpy(comm, l + 1, n); comm[n] = 0;
            unsigned long u = 0, s = 0;
            if (sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &u, &s) == 2) t = (long)(u + s);
        }
    }
    fclose(f);
    return t;
}

static void top_procs(double secs)
{
    pinfo cur[512];
    int n = 0;
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 512) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        cur[n].pid = atoi(e->d_name);
        cur[n].ticks = proc_ticks(cur[n].pid, cur[n].comm);
        if (cur[n].ticks >= 0) n++;
    }
    if (d) closedir(d);
    double hz = (double)sysconf(_SC_CLK_TCK);
    // deltas
    struct { double pct; char comm[32]; int pid; } top[4] = { { 0 } };
    for (int i = 0; i < n; i++) {
        long before = -1;
        for (int k = 0; k < nprev; k++) if (prev_p[k].pid == cur[i].pid) { before = prev_p[k].ticks; break; }
        if (before < 0 || secs <= 0) continue;
        double pct = (cur[i].ticks - before) / hz / secs * 100;
        for (int k = 0; k < 4; k++) if (pct > top[k].pct) {
            memmove(&top[k + 1], &top[k], sizeof top[0] * (3 - k));
            top[k].pct = pct; top[k].pid = cur[i].pid; snprintf(top[k].comm, 32, "%s", cur[i].comm);
            break;
        }
    }
    memcpy(prev_p, cur, sizeof cur[0] * n);
    nprev = n;
    char fr[32] = "?", gov[32] = "?", la[64] = "?";
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (f) { if (fscanf(f, "%31s", fr) != 1) strcpy(fr, "?"); fclose(f); }
    f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
    if (f) { if (fscanf(f, "%31s", gov) != 1) strcpy(gov, "?"); fclose(f); }
    f = fopen("/proc/loadavg", "r");
    if (f) { if (!fgets(la, sizeof la, f)) strcpy(la, "?"); la[strcspn(la, "\n")] = 0; fclose(f); }
    printf("     cpu %s kHz (%s), load %s; top:", fr, gov, la);
    for (int k = 0; k < 4 && top[k].comm[0]; k++) printf(" %s[%d] %.0f%%", top[k].comm, top[k].pid, top[k].pct);
    printf("\n");
}

static int audioserver_pid = -1, as_old_policy = -1;
static struct sched_param as_old_param;

static int find_pid(const char *name)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int found = -1;
    while (d && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char comm[32] = "";
        if (proc_ticks(atoi(e->d_name), comm) >= 0 && !strcmp(comm, name)) { found = atoi(e->d_name); break; }
    }
    if (d) closedir(d);
    return found;
}

static void boost_audioserver(void)
{
    audioserver_pid = find_pid("audioserver");
    if (audioserver_pid < 0) { printf("[bg] audioserver process not found\n"); return; }
    as_old_policy = sched_getscheduler(audioserver_pid);
    sched_getparam(audioserver_pid, &as_old_param);
    struct sched_param sp = { .sched_priority = 20 };
    int r = sched_setscheduler(audioserver_pid, SCHED_RR, &sp);
    printf("[bg] audioserver pid %d: policy %d prio %d -> realtime 20: %s\n", audioserver_pid, as_old_policy,
           as_old_param.sched_priority, r == 0 ? "ok" : "FAILED");
}

static void restore_audioserver(void)
{
    if (audioserver_pid > 0 && as_old_policy >= 0) {
        sched_setscheduler(audioserver_pid, as_old_policy, &as_old_param);
        printf("[bg] audioserver priority restored\n");
    }
}

static volatile int stop;
static void on_sig(int s) { (void)s; stop = 1; }

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static char *tracks[500];
static int ntracks;

static void scan(const char *dir, int depth)
{
    if (depth > 4 || ntracks >= 500) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && ntracks < 500) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) scan(p, depth + 1);
        else if (dec_supported(p)) tracks[ntracks++] = strdup(p);
    }
    closedir(d);
}

static void load_playlist(const char *m3u)
{
    FILE *f = fopen(m3u, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f) && ntracks < 500) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && line[0] != '#' && dec_supported(line)) tracks[ntracks++] = strdup(line);
    }
    fclose(f);
}

static long cpu_ticks(void)
{
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    char b[1024];
    long t = 0;
    if (fgets(b, sizeof b, f)) {
        char *q = strrchr(b, ')');
        unsigned long u = 0, s = 0;
        if (q && sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &u, &s) == 2) t = (long)(u + s);
    }
    fclose(f);
    return t;
}

int main(int argc, char **argv)
{
    if (argc < 4 || strcmp(argv[1], "start")) { fprintf(stderr, "usage: bgtest start LOG PIDFILE\n"); return 1; }
    // detach from the launcher
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid > 0) {
        FILE *pf = fopen(argv[3], "w");
        if (pf) { fprintf(pf, "%d\n", pid); fclose(pf); }
        return 0;
    }
    setsid();
    int fd = open(argv[2], O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) { dup2(nul, 0); close(nul); }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    printf("[bg] started, pid %d\n", getpid());
    load_playlist(getenv("BG_M3U") ? getenv("BG_M3U") : "/mnt/SDCARD/App/MiniAmp/data/playlist.m3u");
    if (ntracks) printf("[bg] using MiniAmp's playlist (%d tracks)\n", ntracks);
    const char *dirs[] = { "/mnt/SDCARD/Media/Music", "/mnt/SDCARD/Music", "/mnt/SDCARD/Media", NULL };
    for (int i = 0; dirs[i] && !ntracks; i++) scan(dirs[i], 0);
    if (!ntracks) { printf("[bg] no music found\n"); return 1; }

    const char *dp = getenv("BG_DSP");
    int dsp = open(dp ? dp : "/dev/dsp", O_WRONLY | (dp ? O_CREAT | O_TRUNC : 0), 0644);
    if (dsp < 0) { perror("[bg] open /dev/dsp"); return 1; }
    int frag = (4 << 16) | 12, fmt = AFMT_S16_LE, ch = 2, rate = 44100;
    ioctl(dsp, SNDCTL_DSP_SETFRAGMENT, &frag);
    ioctl(dsp, SNDCTL_DSP_SETFMT, &fmt);
    ioctl(dsp, SNDCTL_DSP_CHANNELS, &ch);
    ioctl(dsp, SNDCTL_DSP_SPEED, &rate);
    printf("[bg] /dev/dsp open (fmt %d ch %d rate %d)\n", fmt, ch, rate);

    static float in[4096 * 2];
    static short out[8192 * 2];
    double t0 = now(), t_log = t0, worst = 0;
    int chunk = 4096, phase = 1;
    printf("[bg] phase 1 (0-20s): 4096-frame writes, normal priority\n");
    top_procs(0);
    long long frames = 0, frames_log = 0;
    long cpu0 = cpu_ticks();
    int errors = 0;
    double hz = (double)sysconf(_SC_CLK_TCK);
    for (int ti = 0; !stop && now() - t0 < 3600; ti = (ti + 1) % ntracks) {
        dec_info info;
        decoder *d = dec_open(tracks[ti], &info);
        if (!d) { printf("[bg] cannot open %s\n", tracks[ti]); if (++errors > 20) break; continue; }
        printf("[bg] playing %s (%d Hz, %d ch)\n", tracks[ti], info.samplerate, info.channels);
        double pos = 0, step = (double)info.samplerate / 44100;
        int have = 0, idx = 0, nch = info.channels;
        float cur[2], nxt[2];
#define GET(f) ( (idx < have || ((have = dec_read(d, in, 4096)) > 0 && ((idx = 0), 1))) \
                 ? ((f)[0] = in[idx * nch], (f)[1] = in[idx * nch + (nch > 1)], idx++, 1) : 0 )
        if (!GET(cur) || !GET(nxt)) goto track_done;
        while (!stop) {
            int n = 0;
            while (n < chunk) {
                while (pos >= 1.0) {
                    cur[0] = nxt[0]; cur[1] = nxt[1];
                    if (!GET(nxt)) goto track_done;
                    pos -= 1.0;
                }
                for (int c = 0; c < 2; c++) {
                    float v = (cur[c] + (nxt[c] - cur[c]) * (float)pos) * 0.5f;
                    int sv = (int)(v * 32767);
                    out[n * 2 + c] = (short)(sv > 32767 ? 32767 : sv < -32768 ? -32768 : sv);
                }
                n++;
                pos += step;
            }
            {
                double w0 = now();
                const char *p = (const char *)out;
                size_t left = (size_t)n * 4;
                while (left && !stop) {
                    ssize_t r = write(dsp, p, left);
                    if (r < 0) { if (++errors < 5) perror("[bg] write"); usleep(20000); continue; }
                    p += r; left -= (size_t)r;
                }
                double w = now() - w0;
                if (w > worst) worst = w;
                frames += n;
            }
            double t = now();
            if (phase == 1 && t - t0 >= 20) {
                phase = 2; chunk = 1024;
                struct sched_param sp = { .sched_priority = 10 };
                int r = sched_setscheduler(0, SCHED_RR, &sp);
                printf("[bg] phase 2 (20-40s): 1024-frame writes, realtime priority for this player: %s\n", r == 0 ? "ok" : "FAILED");
            }
            if (phase == 2 && t - t0 >= 40) {
                phase = 3;
                printf("[bg] phase 3 (40s on): also realtime priority for audioserver\n");
                boost_audioserver();
            }
            if (t - t_log >= 5) {
                long cpu = cpu_ticks();
                printf("[bg] %.0fs: %.0f%% of real time, longest write %.0f ms, cpu %.0f%%, errors %d\n",
                       t - t0, (frames - frames_log) / 44100.0 / (t - t_log) * 100, worst * 1000,
                       (cpu - cpu0) / hz / (t - t_log) * 100, errors);
                top_procs(t - t_log);
                frames_log = frames; t_log = t; worst = 0; cpu0 = cpu;
            }
        }
    track_done:
        dec_close(d);
    }
    restore_audioserver();
    printf("[bg] stopped after %.0fs\n", now() - t0);
    close(dsp);
    return 0;
}
