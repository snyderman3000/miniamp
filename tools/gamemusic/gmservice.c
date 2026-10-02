// Prototype background music service: decodes MiniAmp's playlist into shared
// memory, where the libpadsp wrapper mixes it into the menu's or a game's audio.
//   gmservice start LOG PIDFILE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "decode.h"
#include "bgm_shm.h"

static volatile int stop;
static void on_sig(int s) { (void)s; stop = 1; }

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static char *tracks[1000];
static int ntracks;

static void scan(const char *dir, int depth)
{
    if (depth > 4 || ntracks >= 1000) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && ntracks < 1000) {
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
    while (fgets(line, sizeof line, f) && ntracks < 1000) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && line[0] != '#' && dec_supported(line)) tracks[ntracks++] = strdup(line);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 4 || strcmp(argv[1], "start")) { fprintf(stderr, "usage: gmservice start LOG PIDFILE\n"); return 1; }
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
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    printf("[svc] started, pid %d\n", getpid());
    load_playlist(getenv("BG_M3U") ? getenv("BG_M3U") : "/mnt/SDCARD/App/MiniAmp/data/playlist.m3u");
    if (ntracks) printf("[svc] using MiniAmp's playlist (%d tracks)\n", ntracks);
    const char *dirs[] = { "/mnt/SDCARD/Media/Music", "/mnt/SDCARD/Music", "/mnt/SDCARD/Media", NULL };
    for (int i = 0; dirs[i] && !ntracks; i++) scan(dirs[i], 0);
    if (!ntracks) { printf("[svc] no music found\n"); return 1; }

    const char *shp = getenv("BG_SHM") ? getenv("BG_SHM") : BGM_SHM_PATH;
    int sfd = open(shp, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (sfd < 0 || ftruncate(sfd, sizeof(bgm_shm)) != 0) { perror("[svc] shm"); return 1; }
    bgm_shm *s = mmap(NULL, sizeof(bgm_shm), PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
    close(sfd);
    if (s == MAP_FAILED) { perror("[svc] mmap"); return 1; }
    memset(s, 0, sizeof *s);
    s->version = 1;
    s->volume = 160;   // ~60%
    __sync_synchronize();
    s->magic = BGM_MAGIC;

    const uint32_t target = 6000;   // keep ~125 ms ready
    static float in[4096 * 2];
    double t0 = now(), t_log = t0;
    uint32_t r_log = 0, und_log = 0, mix_log = 0;
    for (int ti = 0; !stop; ti = (ti + 1) % ntracks) {
        dec_info info;
        decoder *d = dec_open(tracks[ti], &info);
        if (!d) { printf("[svc] cannot open %s\n", tracks[ti]); continue; }
        printf("[svc] next track: %s (%d Hz)\n", tracks[ti], info.samplerate);
        double pos = 0, step = (double)info.samplerate / BGM_RATE;
        int have = 0, idx = 0, nch = info.channels;
        float cur[2], nxt[2];
#define GET(f) ( (idx < have || ((have = dec_read(d, in, 4096)) > 0 && ((idx = 0), 1))) \
                 ? ((f)[0] = in[idx * nch], (f)[1] = in[idx * nch + (nch > 1)], idx++, 1) : 0 )
        if (!GET(cur) || !GET(nxt)) { dec_close(d); continue; }
        int eof = 0;
        while (!stop && !eof) {
            uint32_t fill = s->wpos - s->rpos;
            if (fill >= target) {
                usleep(5000);
            } else {
                uint32_t w = s->wpos, n = target - fill;
                if (n > 1024) n = 1024;
                for (uint32_t i = 0; i < n; i++) {
                    while (pos >= 1.0) {
                        cur[0] = nxt[0]; cur[1] = nxt[1];
                        if (!GET(nxt)) { eof = 1; break; }
                        pos -= 1.0;
                    }
                    if (eof) { n = i; break; }
                    int16_t *o = &s->ring[((w + i) & (BGM_RING - 1)) * 2];
                    for (int c = 0; c < 2; c++) {
                        float v = cur[c] + (nxt[c] - cur[c]) * (float)pos;
                        int sv = (int)(v * 32767);
                        o[c] = (int16_t)(sv > 32767 ? 32767 : sv < -32768 ? -32768 : sv);
                    }
                    pos += step;
                }
                __sync_synchronize();
                s->wpos = w + n;
            }
            double t = now();
            if (t - t_log >= 5) {
                uint32_t r = s->rpos;
                printf("[svc] %.0fs: music mixed at %.0f%% of real time by %s[%d], underruns %u, mixed writes %u\n",
                       t - t0, (r - r_log) / (double)BGM_RATE / (t - t_log) * 100,
                       s->owner_pid ? s->owner_name : "nobody", s->owner_pid, s->underruns - und_log, s->mixed_writes - mix_log);
                r_log = r; und_log = s->underruns; mix_log = s->mixed_writes; t_log = t;
            }
        }
        dec_close(d);
    }
    s->magic = 0;
    __sync_synchronize();
    munmap(s, sizeof *s);
    unlink(shp);
    printf("[svc] stopped after %.0fs\n", now() - t0);
    return 0;
}
