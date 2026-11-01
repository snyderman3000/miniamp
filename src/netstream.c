// Live HTTP audio stream with a jitter buffer (see netstream.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "netstream.h"
#include "net.h"

#define RING (256 * 1024)        // ~10 s of 192 kbps MP3
#define PREBUF (24 * 1024)       // ~1 s at 192 kbps before playback starts
#define MAXFILL (PREBUF * 4)     // if we fall this far behind live, drop back to PREBUF
#define STALL_MS 8000            // no data this long = stream is gone
#define STALL_GRACE_MS 250       // waiting longer than this for data = a real stall

struct netstream {
    int fd;
    pthread_t thr;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    unsigned char *ring;
    size_t r, w;                 // monotonically increasing byte counters
    int eof, buffering;
    volatile int stop;
};

// the stream currently open, so the player can cut a stalled one short
static pthread_mutex_t live_mtx = PTHREAD_MUTEX_INITIALIZER;
static netstream *live;

void ns_interrupt(void)
{
    pthread_mutex_lock(&live_mtx);
    if (live) { live->stop = 1; shutdown(live->fd, SHUT_RDWR); }
    pthread_mutex_unlock(&live_mtx);
}

int ns_is_url(const char *path) { return path && !strncmp(path, "http://", 7); }

static void *reader(void *arg)
{
    netstream *s = arg;
    unsigned char buf[4096];
    while (!s->stop) {
        ssize_t k = recv(s->fd, buf, sizeof buf, 0);
        if (k <= 0) break;   // closed, error, or no data for STALL_MS
        pthread_mutex_lock(&s->mtx);
        for (ssize_t i = 0; i < k; i++) s->ring[(s->w + i) % RING] = buf[i];
        s->w += (size_t)k;
        size_t backlog = s->w - s->r;
        if (backlog > MAXFILL) {
            // our clock is slower than the PC's, or we stalled and the backlog
            // arrived at once: skip ahead to stay close to live
            s->r = s->w - PREBUF;
            fprintf(stderr, "[stream] %zu bytes behind live, skipping ahead\n", backlog);
        }
        pthread_cond_broadcast(&s->cv);
        pthread_mutex_unlock(&s->mtx);
    }
    pthread_mutex_lock(&s->mtx);
    s->eof = 1;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mtx);
    return NULL;
}

netstream *ns_open(const char *url)
{
    if (!ns_is_url(url)) return NULL;
    char host[128], path[512] = "/";
    int port = 80;
    const char *h = url + 7;
    const char *slash = strchr(h, '/');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    if (hl >= sizeof host) return NULL;
    memcpy(host, h, hl);
    host[hl] = 0;
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    if (slash) snprintf(path, sizeof path, "%s", slash);

    int fd = net_http_stream(host, port, path, 4000);
    if (fd < 0) { fprintf(stderr, "[stream] couldn't connect to %s\n", url); return NULL; }
    struct timeval tv = { STALL_MS / 1000, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    netstream *s = calloc(1, sizeof *s);
    if (!s) { close(fd); return NULL; }
    s->ring = malloc(RING);
    if (!s->ring) { close(fd); free(s); return NULL; }
    s->fd = fd;
    s->buffering = 1;
    pthread_mutex_init(&s->mtx, NULL);
    pthread_cond_init(&s->cv, NULL);
    pthread_create(&s->thr, NULL, reader, s);
    pthread_mutex_lock(&live_mtx);
    live = s;
    pthread_mutex_unlock(&live_mtx);
    fprintf(stderr, "[stream] connected to %s\n", url);
    return s;
}

static void deadline_in(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

size_t ns_read(netstream *s, void *out, size_t n)
{
    pthread_mutex_lock(&s->mtx);
    struct timespec dl;
    deadline_in(&dl, STALL_MS);
    // An empty ring on its own is normal: the MP3 decoder keeps ~16 KB (about
    // 0.7 s) of its own. Only a wait longer than STALL_GRACE_MS means the audio
    // really ran dry; then refill properly so one hiccup doesn't turn into
    // constant stutter.
    if (s->w - s->r == 0 && !s->buffering && !s->eof) {
        struct timespec grace;
        deadline_in(&grace, STALL_GRACE_MS);
        while (s->w - s->r == 0 && !s->eof)
            if (pthread_cond_timedwait(&s->cv, &s->mtx, &grace)) break;
        if (s->w - s->r == 0 && !s->eof) {
            s->buffering = 1;
            fprintf(stderr, "[stream] stalled, rebuffering\n");
        }
    }
    while (s->buffering && !s->eof && s->w - s->r < PREBUF) {
        if (pthread_cond_timedwait(&s->cv, &s->mtx, &dl)) break;
    }
    s->buffering = 0;
    while (s->w - s->r == 0 && !s->eof) {
        if (pthread_cond_timedwait(&s->cv, &s->mtx, &dl)) break;
    }
    size_t fill = s->w - s->r;
    if (n > fill) n = fill;
    for (size_t i = 0; i < n; i++) ((unsigned char *)out)[i] = s->ring[(s->r + i) % RING];
    s->r += n;
    pthread_mutex_unlock(&s->mtx);
    return n;
}

void ns_close(netstream *s)
{
    if (!s) return;
    pthread_mutex_lock(&live_mtx);
    if (live == s) live = NULL;
    pthread_mutex_unlock(&live_mtx);
    s->stop = 1;
    shutdown(s->fd, SHUT_RDWR);   // wakes the reader out of recv()
    pthread_join(s->thr, NULL);
    close(s->fd);
    pthread_mutex_destroy(&s->mtx);
    pthread_cond_destroy(&s->cv);
    free(s->ring);
    free(s);
}
