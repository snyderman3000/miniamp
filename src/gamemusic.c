// Music in games: wrapper install/uninstall and the background service.
//
// The service keeps playing MiniAmp's playlist after the player is closed. It
// decodes into shared memory (bgm_shm.h); the libpadsp wrapper (padsp_wrap.c)
// mixes that into the sound of whatever program is playing (the menu or a
// game). When no program is playing sound, the service plays directly.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "gamemusic.h"
#include "bgm_shm.h"
#include "core.h"
#include "audio.h"
#include "playlist.h"

// ---------------------------------------------------------------- files
static const char *libdir(void)
{
    const char *d = getenv("MA_LIBDIR");
    return d ? d : "/mnt/SDCARD/miyoo/lib";
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static int has_marker(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char buf[65536];
    const char *m = BGM_WRAP_MARKER;
    size_t ml = strlen(m), n, keep = 0;
    int found = 0;
    while (!found && (n = fread(buf + keep, 1, sizeof buf - keep, f)) > 0) {
        size_t len = keep + n;
        for (size_t i = 0; i + ml <= len; i++) if (buf[i] == m[0] && !memcmp(buf + i, m, ml)) { found = 1; break; }
        keep = ml - 1 < len ? ml - 1 : len;
        memmove(buf, buf + len - keep, keep);
    }
    fclose(f);
    return found;
}

// copies src over dst atomically (programs using the old file keep it)
static int copy_atomic(const char *src, const char *dst)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *in = fopen(src, "rb");
    if (!in) return 0;
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); return 0; }
    char buf[16384];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, n, out) != n) ok = 0;
    fclose(in);
    if (fflush(out) != 0) ok = 0;
    fsync(fileno(out));
    fclose(out);
    chmod(tmp, 0755);
    if (!ok || rename(tmp, dst) != 0) { unlink(tmp); return 0; }
    return 1;
}

int gm_installed(void)
{
    char cur[600];
    snprintf(cur, sizeof cur, "%s/libpadsp.so", libdir());
    return has_marker(cur);
}

int gm_install(void)
{
    char cur[600], orig[600], wrap[600];
    snprintf(cur, sizeof cur, "%s/libpadsp.so", libdir());
    snprintf(orig, sizeof orig, "%s/libpadsp_orig.so", libdir());
    path_join(wrap, sizeof wrap, app_dir, "lib/libpadsp_wrap.so");
    if (!exists(wrap)) { fprintf(stderr, "[gm] wrapper missing: %s\n", wrap); return 0; }
    // whatever is in place and isn't ours is the original (maybe a newer one after an OnionOS update)
    if (exists(cur) && !has_marker(cur)) {
        if (!copy_atomic(cur, orig)) { fprintf(stderr, "[gm] backup failed\n"); return 0; }
    }
    if (!exists(orig) || has_marker(orig)) {
        if (!exists("/customer/lib/libpadsp.so") || !copy_atomic("/customer/lib/libpadsp.so", orig)) {
            fprintf(stderr, "[gm] no original libpadsp.so to keep\n");
            return 0;
        }
    }
    if (exists(cur) && has_marker(cur)) {
        // already ours: only refresh if the bundled wrapper differs in size
        struct stat a, b;
        if (stat(cur, &a) == 0 && stat(wrap, &b) == 0 && a.st_size == b.st_size) return 1;
    }
    int ok = copy_atomic(wrap, cur);
    sync();
    fprintf(stderr, "[gm] wrapper %s\n", ok ? "installed" : "install FAILED");
    return ok;
}

int gm_uninstall(void)
{
    char cur[600], orig[600];
    snprintf(cur, sizeof cur, "%s/libpadsp.so", libdir());
    snprintf(orig, sizeof orig, "%s/libpadsp_orig.so", libdir());
    if (!exists(orig) || has_marker(orig)) {
        if (has_marker(cur) && exists("/customer/lib/libpadsp.so")) copy_atomic("/customer/lib/libpadsp.so", cur);
        return !has_marker(cur);
    }
    int ok = 1;
    if (!exists(cur) || has_marker(cur)) ok = copy_atomic(orig, cur);
    if (ok) unlink(orig);
    sync();
    fprintf(stderr, "[gm] original %s\n", ok ? "restored" : "restore FAILED");
    return ok;
}

// ---------------------------------------------------------------- service control
static void pid_path(char *out, int n) { path_join(out, n, data_dir, "service.pid"); }

int service_running(void)
{
    char p[600];
    pid_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    if (pid <= 0 || kill(pid, 0) != 0) return 0;
    // make sure it's really us (pids get reused)
    char cp[64], cmd[256] = "";
    snprintf(cp, sizeof cp, "/proc/%d/cmdline", pid);
    FILE *c = fopen(cp, "r");
    if (c) { size_t n = fread(cmd, 1, sizeof cmd - 1, c); cmd[n] = 0; fclose(c); }
    for (size_t i = 0; i + 1 < sizeof cmd && (cmd[i] || cmd[i + 1]); i++) if (!cmd[i]) cmd[i] = ' ';
    return strstr(cmd, "--service") ? pid : 0;
}

void service_stop(void)
{
    int pid = service_running();
    if (!pid) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 60 && kill(pid, 0) == 0; i++) usleep(50000);
    if (kill(pid, 0) == 0) kill(pid, SIGKILL);
}

void service_spawn(void)
{
    char exe[600];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    pid_t pid = fork();
    if (pid == 0) {
        // close everything but stdio so the service doesn't hold the display or input
        for (int fd = 3; fd < 256; fd++) close(fd);
        execl(exe, exe, "--service", (char *)NULL);
        _exit(1);
    }
    if (pid > 0) waitpid(pid, NULL, 0);   // the service forks again and this child exits at once
}

// ---------------------------------------------------------------- service
static volatile int svc_quit, svc_signal;
static bgm_shm *shm;
static pthread_mutex_t act = PTHREAD_MUTEX_INITIALIZER;
static int my_pid;

static void on_term(int s) { (void)s; svc_quit = 1; svc_signal = 1; }

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

static void say(const char *fmt, const char *a)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(stderr, "[%02d:%02d:%02d] ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
}

// programs that play sound through an unwrapped libpadsp (we'd fight them for the audioserver)
static int foreign_audio(void)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    char line[512];
    while (!found && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        int pid = atoi(e->d_name);
        if (pid == my_pid) continue;
        char p[64];
        snprintf(p, sizeof p, "/proc/%d/maps", pid);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        int padsp = 0, orig = 0;
        while (fgets(line, sizeof line, f)) {
            if (strstr(line, "libpadsp_orig.so")) { orig = 1; break; }
            if (strstr(line, "libpadsp")) padsp = 1;
        }
        fclose(f);
        if (padsp && !orig) found = pid;
    }
    closedir(d);
    return found;
}

static int hooked_alive(void)
{
    int any = 0;
    for (int i = 0; i < 8; i++) {
        int pid = shm->dsp_pids[i];
        if (!pid) continue;
        if (kill(pid, 0) != 0) { __sync_bool_compare_and_swap(&shm->dsp_pids[i], pid, 0); continue; }
        any = 1;
    }
    return any;
}

// plays the music directly while no other program is making sound
static void *direct_thread(void *arg)
{
    (void)arg;
    int fd = -1, foreign = 0;
    uint32_t foreign_t = 0;
    static int16_t buf[1024 * 2];
    while (!svc_quit) {
        uint32_t t = now_ms();
        int owner = shm->owner_pid;
        int fresh = owner && owner != my_pid && t - shm->owner_ms < 300;
        int want = !fresh && !hooked_alive() && audio_state() == A_PLAYING;
        if (want && t - foreign_t > 2000) { foreign = foreign_audio(); foreign_t = t; }
        if (want && foreign) want = 0;
        if (!want) {
            if (fd >= 0) { close(fd); fd = -1; say("direct playback off%s", foreign ? " (another program has the sound)" : ""); }
            usleep(20000);
            continue;
        }
        if (fd < 0) {
            fd = open("/dev/dsp", O_WRONLY);
            if (fd < 0) { usleep(1000000); continue; }
            int frag = (4 << 16) | 12, fmt = AFMT_S16_LE, ch = 2, rate = BGM_RATE;
            ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag);
            ioctl(fd, SNDCTL_DSP_SETFMT, &fmt);
            ioctl(fd, SNDCTL_DSP_CHANNELS, &ch);
            ioctl(fd, SNDCTL_DSP_SPEED, &rate);
            say("direct playback on%s", "");
        }
        uint32_t r = shm->rpos, avail = shm->wpos - r;
        if (avail > BGM_RING) avail = 0;
        uint32_t n = avail < 1024 ? avail : 1024;
        if (n < 256) { usleep(5000); continue; }
        for (uint32_t i = 0; i < n; i++) {
            const int16_t *m = &shm->ring[((r + i) & (BGM_RING - 1)) * 2];
            buf[i * 2] = m[0];
            buf[i * 2 + 1] = m[1];
        }
        shm->rpos = r + n;
        shm->owner_pid = my_pid;
        shm->owner_ms = now_ms();
        const char *p = (const char *)buf;
        size_t left = n * 4;
        while (left && !svc_quit) {
            ssize_t w = write(fd, p, left);
            if (w <= 0) break;
            p += w; left -= (size_t)w;
        }
    }
    if (fd >= 0) close(fd);
    return NULL;
}

// SELECT + L/R skip, + Up/Down volume, + START pause
static void *input_thread(void *arg)
{
    (void)arg;
    int fd = open("/dev/input/event0", O_RDONLY);
    if (fd < 0) return NULL;
    int select_down = 0;
    struct input_event ev;
    while (!svc_quit) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        struct timeval tv = { 0, 200000 };
        if (select(fd + 1, &rs, NULL, NULL, &tv) <= 0) continue;
        if (read(fd, &ev, sizeof ev) != sizeof ev) continue;
        if (ev.type != EV_KEY) continue;
        if (ev.code == 97) { select_down = ev.value != 0; continue; }
        if (!select_down || ev.value != 1) continue;
        pthread_mutex_lock(&act);
        switch (ev.code) {
        case 18: core_skip(-1); say("SELECT+L: previous%s", ""); break;
        case 20: core_skip(1); say("SELECT+R: next%s", ""); break;
        case 103: S.volume = S.volume + 10 > 100 ? 100 : S.volume + 10; core_apply_audio(); say("SELECT+Up: volume up%s", ""); break;
        case 108: S.volume = S.volume - 10 < 0 ? 0 : S.volume - 10; core_apply_audio(); say("SELECT+Down: volume down%s", ""); break;
        case 28: core_toggle_pause(); say("SELECT+START: %s", audio_state() == A_PAUSED ? "paused" : "playing"); break;
        }
        pthread_mutex_unlock(&act);
    }
    close(fd);
    return NULL;
}

static void save_place(void)
{
    if (current >= 0) { S.cur_index = current; S.cur_pos_ms = audio_pos_ms(); }
    settings_save();
}

int service_main(void)
{
    // detach from the player
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid > 0) _exit(0);
    setsid();
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    my_pid = getpid();

    char p[600];
    path_join(p, sizeof p, data_dir, "service.log");
    struct stat st;
    int fd = open(p, O_WRONLY | O_CREAT | (stat(p, &st) == 0 && st.st_size > 262144 ? O_TRUNC : O_APPEND), 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) { dup2(nul, 0); close(nul); }
    setvbuf(stderr, NULL, _IOLBF, 0);
    pid_path(p, sizeof p);
    FILE *pf = fopen(p, "w");
    if (pf) { fprintf(pf, "%d\n", my_pid); fclose(pf); }

    settings_load();
    say("===== background service started (MiniAmp %s)", getenv("MA_VERSION") ? getenv("MA_VERSION") : "");
    if (!S.game_music) { say("music in games is off%s", ""); unlink(p); return 0; }
    if (!gm_install()) say("could not set up the sound wrapper; music plays only when nothing else does%s", "");

    pl_init();
    path_join(p, sizeof p, data_dir, "playlist.m3u");
    pl_load_m3u(p);
    if (S.cur_index < 0 || S.cur_index >= pl_count()) { say("nothing to play%s", ""); pid_path(p, sizeof p); unlink(p); return 0; }

    int sfd = open(BGM_SHM_PATH, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (sfd < 0 || ftruncate(sfd, sizeof(bgm_shm)) != 0) { say("shared memory failed%s", ""); return 1; }
    shm = mmap(NULL, sizeof(bgm_shm), PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
    close(sfd);
    if (shm == MAP_FAILED) { say("mmap failed%s", ""); return 1; }
    memset(shm, 0, sizeof *shm);
    shm->version = 2;
    shm->volume = 256;     // MiniAmp's own volume is applied before the ring
    __sync_synchronize();
    shm->magic = BGM_MAGIC;

    audio_init_ring(shm);
    core_apply_audio();
    if (S.shuffle) core_build_shuffle();
    core_play_index(S.cur_index, S.cur_pos_ms, 0);
    say("playing %s", pl_path(S.cur_index));

    pthread_t th_direct, th_input;
    pthread_create(&th_direct, NULL, direct_thread, NULL);
    pthread_create(&th_input, NULL, input_thread, NULL);

    uint32_t last_save = now_ms(), last_stats = now_ms(), r_stats = 0, und_stats = 0;
    int errors = 0;
    while (!svc_quit) {
        usleep(50000);
        pthread_mutex_lock(&act);
        if (audio_take_finished()) {
            errors = 0;
            if (!core_track_finished()) { say("end of the playlist%s", ""); svc_quit = 1; }
            else say("next: %s", pl_path(current));
        }
        if (audio_take_error()) {
            say("could not play %s", pl_path(current));
            if (++errors >= pl_count()) svc_quit = 1;
            else { int k = core_next_index(1); if (k >= 0) core_play_index(k, 0, 0); else svc_quit = 1; }
        }
        uint32_t t = now_ms();
        if (t - last_save > 5000) { save_place(); last_save = t; }
        pthread_mutex_unlock(&act);
        if (t - last_stats > 60000) {
            char b[200];
            uint32_t r = shm->rpos;
            snprintf(b, sizeof b, "music %.0f%% of real time, carried by %s, underruns %u",
                     (r - r_stats) / (double)BGM_RATE / ((t - last_stats) / 1000.0) * 100,
                     shm->owner_pid == my_pid ? "MiniAmp" : shm->owner_pid ? (const char *)shm->owner_name : "nobody",
                     shm->underruns - und_stats);
            say("%s", b);
            r_stats = r; und_stats = shm->underruns; last_stats = t;
        }
    }
    pthread_mutex_lock(&act);
    S.resume_playing = svc_signal && audio_state() == A_PLAYING;
    save_place();
    pthread_mutex_unlock(&act);
    shm->magic = 0;
    __sync_synchronize();
    pthread_join(th_direct, NULL);
    pthread_join(th_input, NULL);
    audio_quit();
    unlink(BGM_SHM_PATH);
    pid_path(p, sizeof p);
    unlink(p);
    say("stopped%s", svc_signal ? " (MiniAmp opened)" : "");
    return 0;
}
