// libpadsp.so wrapper for OnionOS: mixes MiniAmp's background music into the
// audio of whatever program is playing (a game, or the menu).
//
// OnionOS starts the menu and every game with LD_PRELOAD=.../libpadsp.so, the
// library that sends OSS (/dev/dsp) audio to the audioserver. This file takes
// its place; the original is kept next to it as libpadsp_orig.so. On load,
// the wrapper restarts the program with both preloaded (wrapper first), so its
// open/ioctl/write/close run in front of the original's.
//
// Music comes from shared memory (bgm_shm.h) filled by the MiniAmp service.
// If the service isn't running, the wrapper only passes calls through.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include "bgm_shm.h"
#include "bgm_mix.h"

#define LOG_PATH "/mnt/SDCARD/App/MiniAmp/data/hook.log"

// lets MiniAmp tell this wrapper apart from the original library
__attribute__((used)) const char miniamp_wrapper_marker[] = BGM_WRAP_MARKER;

static int (*r_open)(const char *, int, ...);
static int (*r_openat)(int, const char *, int, ...);
static int (*r_ioctl)(int, unsigned long, ...);
static ssize_t (*r_write)(int, const void *, size_t);
static int (*r_close)(int);

static void resolve(void)
{
    if (r_write) return;
    r_open = dlsym(RTLD_NEXT, "open");
    r_openat = dlsym(RTLD_NEXT, "openat");
    r_ioctl = dlsym(RTLD_NEXT, "ioctl");
    r_close = dlsym(RTLD_NEXT, "close");
    r_write = dlsym(RTLD_NEXT, "write");
}

static void logf_(const char *fmt, ...)
{
    resolve();
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof b - 1) n = sizeof b - 1;
    const char *lp = getenv("MA_HOOK_LOG");
    const char *path = lp ? lp : LOG_PATH;
    // Keep the log small: once it's 64 KB, it becomes hook.log.1 and a new one starts.
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size >= 65536) {
        char old[300];
        snprintf(old, sizeof old, "%s.1", path);
        rename(path, old);
    }
    int fd = r_open ? r_open(path, O_WRONLY | O_APPEND | O_CREAT, 0644) : -1;
    if (fd >= 0) {
        r_write(fd, b, n);
        r_close(fd);
    }
}

static void comm_name(char *out, int n)
{
    out[0] = 0;
    FILE *f = fopen("/proc/self/comm", "r");
    if (!f) return;
    if (fgets(out, n, f)) out[strcspn(out, "\n")] = 0;
    fclose(f);
}

// ---------------------------------------------------------------- re-exec with the original library
__attribute__((constructor)) static void wrap_init(void)
{
    Dl_info di;
    if (!dladdr((void *)wrap_init, &di) || !di.dli_fname) return;
    char self[256], orig[256];
    snprintf(self, sizeof self, "%s", di.dli_fname);
    char *sl = strrchr(self, '/');
    if (!sl) return;
    snprintf(orig, sizeof orig, "%.*s/libpadsp_orig.so", (int)(sl - self), self);
    const char *pre = getenv("LD_PRELOAD");
    if (pre && strstr(pre, "libpadsp_orig.so")) {          // already chained
        // put back the process name the restart changed (OnionOS looks programs up by it)
        const char *nm = getenv("MA_COMM");
        if (nm) { prctl(PR_SET_NAME, nm, 0, 0, 0); unsetenv("MA_COMM"); }
        return;
    }
    if (access(orig, R_OK) != 0) return;                 // nothing to chain to

    // argv from /proc/self/cmdline
    static char cmd[8192];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return;
    int len = (int)read(fd, cmd, sizeof cmd - 1);
    close(fd);
    if (len <= 0) return;
    cmd[len] = 0;
    char *argv[256];
    int argc = 0;
    for (int i = 0; i < len && argc < 255; ) {
        argv[argc++] = cmd + i;
        i += (int)strlen(cmd + i) + 1;
    }
    argv[argc] = NULL;

    // new preload list: wrapper, original, then anything else that was there
    static char np[1024];
    snprintf(np, sizeof np, "%s %s", self, orig);
    if (pre) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", pre);
        for (char *tok = strtok(tmp, " :"); tok; tok = strtok(NULL, " :")) {
            if (strstr(tok, "libpadsp")) continue;
            size_t l = strlen(np);
            snprintf(np + l, sizeof np - l, " %s", tok);
        }
    }
    char exe[512];
    ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (el <= 0) return;
    exe[el] = 0;
    char comm[32];
    comm_name(comm, sizeof comm);
    char *oldpre = pre ? strdup(pre) : NULL;
    setenv("LD_PRELOAD", np, 1);
    if (comm[0]) setenv("MA_COMM", comm, 1);
    execv(exe, argv);
    unsetenv("MA_COMM");
    // exec failed: carry on without the original (no sound for this program)
    if (oldpre) setenv("LD_PRELOAD", oldpre, 1);
    free(oldpre);
}

// ---------------------------------------------------------------- dsp tracking
static int dsp_fd = -1, dsp_fmt = AFMT_S16_LE, dsp_ch = 2, dsp_rate = 0;
static uint32_t music_frac;   // position between two music frames (16.16), for resampling
static void map_shm_now(int force);
static void slot_set(int on);
static bgm_shm *shm;
static uint32_t shm_try_ms;
static int my_pid;

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

static int is_dsp(const char *p)
{
    return p && (!strcmp(p, "/dev/dsp") || !strcmp(p, "/dev/dsp1") || !strcmp(p, "/dev/adsp") || !strcmp(p, "/dev/audio"));
}

static void track_open(const char *path, int fd)
{
    if (fd < 0 || !is_dsp(path)) return;
    dsp_fd = fd;
    dsp_fmt = AFMT_S16_LE;
    dsp_ch = 2;
    dsp_rate = 0;
    music_frac = 0;
    my_pid = getpid();
    char c[32];
    comm_name(c, sizeof c);
    logf_("[hook] %s[%d] opened %s (fd %d)\n", c, my_pid, path, fd);
    map_shm_now(1);
    slot_set(1);
}

int open(const char *path, int flags, ...)
{
    resolve();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    int fd = r_open(path, flags, mode);
    track_open(path, fd);
    return fd;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    resolve();
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    int fd = r_openat(dirfd, path, flags, mode);
    track_open(path, fd);
    return fd;
}

int ioctl(int fd, unsigned long req, ...)
{
    resolve();
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    int r = r_ioctl(fd, req, arg);
    if (fd == dsp_fd && fd >= 0 && arg) {
        int v = *(int *)arg;
        int changed = 1;
        if (req == SNDCTL_DSP_SETFMT) dsp_fmt = v;
        else if (req == SNDCTL_DSP_CHANNELS) dsp_ch = v;
        else if (req == SNDCTL_DSP_STEREO) dsp_ch = v ? 2 : 1;
        else if (req == SNDCTL_DSP_SPEED) { dsp_rate = v; music_frac = 0; }
        else changed = 0;
        if (changed) logf_("[hook]   format %d, %d ch, %d Hz\n", dsp_fmt, dsp_ch, dsp_rate);
    }
    return r;
}

int close(int fd)
{
    resolve();
    if (fd == dsp_fd && fd >= 0) {
        dsp_fd = -1;
        if (shm && shm->owner_pid == my_pid) shm->owner_pid = 0;
        slot_set(0);
        logf_("[hook] pid %d closed the sound device\n", getpid());
    }
    return r_close(fd);
}

static void map_shm_now(int force);
static void map_shm(void) { map_shm_now(0); }

static void map_shm_now(int force)
{
    uint32_t t = now_ms();
    if (shm) {
        if (shm->magic == BGM_MAGIC) return;
        munmap(shm, sizeof *shm);   // service stopped
        shm = NULL;
    }
    if (!force && t - shm_try_ms < 1000) return;
    shm_try_ms = t;
    int fd = r_open(BGM_SHM_PATH, O_RDWR);
    if (fd < 0) return;
    void *p = mmap(NULL, sizeof(bgm_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    r_close(fd);
    if (p == MAP_FAILED) return;
    if (((bgm_shm *)p)->magic != BGM_MAGIC) { munmap(p, sizeof(bgm_shm)); return; }
    shm = p;
}

// records that this program has the sound device open (the service won't play directly meanwhile)
static void slot_set(int on)
{
    if (!shm) return;
    int pid = getpid();
    for (int i = 0; i < 8; i++) {
        if (on) {
            if (shm->dsp_pids[i] == pid) return;
            if (__sync_bool_compare_and_swap(&shm->dsp_pids[i], 0, pid)) return;
        } else if (shm->dsp_pids[i] == pid) {
            shm->dsp_pids[i] = 0;
        }
    }
}

ssize_t write(int fd, const void *buf, size_t n)
{
    resolve();
    if (fd != dsp_fd || fd < 0 || dsp_fmt != AFMT_S16_LE || (dsp_ch != 1 && dsp_ch != 2)) return r_write(fd, buf, n);
    map_shm();
    if (!shm || shm->paused) return r_write(fd, buf, n);
    uint32_t t = now_ms();
    if (shm->owner_pid != my_pid) {
        if (shm->owner_pid != 0 && t - shm->owner_ms < 300) return r_write(fd, buf, n);
        shm->owner_pid = my_pid;
        comm_name((char *)shm->owner_name, sizeof shm->owner_name);
    }
    shm->owner_ms = t;

    static int16_t tmp[4096 * 2];
    const int16_t *in = buf;
    size_t frames = n / (2 * (size_t)dsp_ch), done = 0;
    ssize_t total = 0;
    int vol = shm->volume;
    uint32_t step = bgm_step(dsp_rate, BGM_RATE);
    while (done < frames) {
        size_t chunk = frames - done;
        if (chunk > 4096) chunk = 4096;
        uint32_t r0 = shm->rpos, avail = shm->wpos - r0;
        if (avail > BGM_RING) avail = 0;   // service restarted
        __sync_synchronize();
        int under = 0;
        bgm_mix(tmp, in + done * dsp_ch, chunk, dsp_ch, shm->ring, BGM_RING - 1, r0, avail, music_frac, step, vol, &under);
        if (under) shm->underruns++;
        size_t bytes = chunk * 2 * dsp_ch;
        ssize_t w = r_write(fd, tmp, bytes);
        if (w <= 0) {
            if (total == 0) return w;
            break;
        }
        size_t wf = (size_t)w / (2 * dsp_ch);
        shm->rpos = r0 + bgm_advance(wf, music_frac, step, avail, &music_frac);
        shm->mixed_writes++;
        total += w;
        done += wf;
        if ((size_t)w < bytes) break;
    }
    // bytes the caller passed that don't make up a whole frame
    if (done == frames && (size_t)total < n) {
        ssize_t w = r_write(fd, (const char *)buf + total, n - total);
        if (w > 0) total += w;
    }
    return total;
}
