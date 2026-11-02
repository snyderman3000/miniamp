// Apple Music through Cider Bridge: background thread that finds the bridge,
// polls what Cider is playing, sends commands, loads library lists and fetches
// album art. The bridge answers in a plain-text format (?fmt=text) so nothing
// here needs a JSON parser.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "am.h"
#include "net.h"
#include "stb_image.h"   // implementation lives in skin.c

#define DISCOVERY_PORT 8780
#define POLL_S 1.0
#define TIMEOUT_MS 2500

static pthread_t thr;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int running, active, kick;
static char cfg_path[700];

static am_status st;                 // under mtx
static char host[64];
static int port, found;
static int fails;
static double next_poll, next_search;
static int want_rediscover;

typedef struct { char path[64]; char body[600]; } cmd;
#define QN 16
static cmd q[QN];
static int qh, qt;

typedef struct {
    am_item *items;
    int n, cap;
    int loading, more, next_offset;
    int job;                 // 0 none, 1 load from scratch, 2 next page
    int epoch;
    char path[400];
    char err[160];
} list;
static list L[AM_SLOTS];

static char art_path[400];           // artwork of the current track (bridge path)
static char art_loaded[400];
static uint8_t *art_px;
static int art_w, art_h, art_new;

double am_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

// copies at most n-1 bytes without cutting a UTF-8 character in half
static void copy_utf8(char *dst, int n, const char *src)
{
    int len = (int)strlen(src);
    if (len > n - 1) {
        len = n - 1;
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    }
    memcpy(dst, src, (size_t)len);
    dst[len] = 0;
}

// ---------------------------------------------------------------- bridge location
static void save_host(void)
{
    FILE *f = fopen(cfg_path, "w");
    if (!f) return;
    fprintf(f, "%s:%d\n", host, port);
    fclose(f);
}

static int load_host(char *h, int hn, int *p)
{
    FILE *f = fopen(cfg_path, "r");
    if (!f) return 0;
    char line[160] = "";
    int ok = 0;
    if (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n \t")] = 0;
        char *c = strrchr(line, ':');
        if (c) { *c = 0; *p = atoi(c + 1); } else *p = 8780;
        if (line[0] && *p > 0) { copy_utf8(h, hn, line); ok = 1; }
    }
    fclose(f);
    return ok;
}

static int ping(const char *h, int p)
{
    char *out = NULL;
    int code = net_http(h, p, "GET", "/api/status?fmt=text", NULL, &out, NULL, 1500);
    free(out);
    return code == 200;
}

static void find_bridge(void)
{
    char h[64];
    int p = 0;
    pthread_mutex_lock(&mtx);
    st.searching = 1;
    pthread_mutex_unlock(&mtx);
    int ok = 0;
    if (load_host(h, sizeof h, &p) && ping(h, p)) ok = 1;
    else if (net_discover(DISCOVERY_PORT, h, sizeof h, &p, 2400)) ok = 1;
    pthread_mutex_lock(&mtx);
    st.searching = 0;
    if (ok) {
        snprintf(host, sizeof host, "%s", h);
        port = p;
        found = 1;
        fails = 0;
        st.found = 1;
        snprintf(st.host, sizeof st.host, "%s", host);
        st.port = port;
        next_poll = 0;
    } else {
        st.found = 0;
        st.connected = 0;
        next_search = am_now() + 5;
    }
    pthread_mutex_unlock(&mtx);
    if (ok) { save_host(); fprintf(stderr, "[am] bridge at %s:%d\n", h, p); }
    else fprintf(stderr, "[am] bridge not found\n");
}

// ---------------------------------------------------------------- requests
// runs a request against the bridge; returns body (caller frees) or NULL
static char *request(const char *method, const char *path, const char *body)
{
    char h[64];
    int p;
    pthread_mutex_lock(&mtx);
    snprintf(h, sizeof h, "%s", host);
    p = port;
    pthread_mutex_unlock(&mtx);
    char *out = NULL;
    int code = net_http(h, p, method, path, body, &out, NULL, TIMEOUT_MS);
    pthread_mutex_lock(&mtx);
    if (code < 0) {
        st.connected = 0;
        if (++fails >= 3) { found = 0; st.found = 0; next_search = 0; }   // the PC may have a new address
    } else {
        st.connected = 1;
        fails = 0;
    }
    pthread_mutex_unlock(&mtx);
    if (code != 200) {
        if (code > 0) fprintf(stderr, "[am] %s %s -> %d %s\n", method, path, code, out ? out : "");
        free(out);
        return NULL;
    }
    return out;
}

// key=value lines -> status (caller holds no lock)
static void apply_status(char *text, int full)
{
    am_status n;
    pthread_mutex_lock(&mtx);
    n = st;
    pthread_mutex_unlock(&mtx);
    if (full) {
        n.has_track = 0;
        n.name[0] = n.artist[0] = n.album[0] = 0;
        n.dur_ms = 0;
        n.pos_s = 0;
    }
    char art[400] = "";
    int saw_art = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "generation")) n.generation = atoi(v);
        else if (!full) continue;   // command replies only carry the generation
        else if (!strcmp(k, "cider")) n.cider = atoi(v);
        else if (!strcmp(k, "playing")) n.playing = atoi(v);
        else if (!strcmp(k, "shuffle")) n.shuffle = atoi(v);
        else if (!strcmp(k, "repeat")) n.repeat = atoi(v);
        else if (!strcmp(k, "kbps")) n.kbps = atoi(v);
        else if (!strcmp(k, "track_name")) { copy_utf8(n.name, sizeof n.name, v); n.has_track = 1; }
        else if (!strcmp(k, "track_artist")) copy_utf8(n.artist, sizeof n.artist, v);
        else if (!strcmp(k, "track_album")) copy_utf8(n.album, sizeof n.album, v);
        else if (!strcmp(k, "track_duration_ms")) n.dur_ms = atoi(v);
        else if (!strcmp(k, "track_position_s")) n.pos_s = atof(v);
        else if (!strcmp(k, "track_artwork")) { copy_utf8(art, sizeof art, v); saw_art = 1; }
    }
    if (full) n.pos_at = am_now();
    pthread_mutex_lock(&mtx);
    st = n;
    if (full) snprintf(art_path, sizeof art_path, "%s", saw_art ? art : "");
    pthread_mutex_unlock(&mtx);
}

static void poll_status(void)
{
    char *t = request("GET", "/api/status?fmt=text", NULL);
    if (t) { apply_status(t, 1); free(t); }
}

// ---------------------------------------------------------------- lists
// "I\tid\ttype\thref\tname\tartist\talbum\tduration_ms\ttrack_count\tartwork" lines; "N\toffset"
static void parse_items(char *text, list *l, int epoch)
{
    int next = -1;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == 'N' && line[1] == '\t') { next = atoi(line + 2); continue; }
        if (line[0] == 'S' && line[1] == '\t') {
            // a section header ("Songs", "Made for You"): stored as an item of type _section
            am_item sec;
            memset(&sec, 0, sizeof sec);
            snprintf(sec.type, sizeof sec.type, "_section");
            copy_utf8(sec.name, sizeof sec.name, line + 2);
            pthread_mutex_lock(&mtx);
            if (l->epoch == epoch) {
                if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->items = realloc(l->items, sizeof *l->items * l->cap); }
                l->items[l->n++] = sec;
            }
            pthread_mutex_unlock(&mtx);
            continue;
        }
        if (line[0] != 'I' || line[1] != '\t') continue;
        char *f[9];
        char *p = line + 2;
        for (int i = 0; i < 9; i++) {
            if (!p) { f[i] = ""; continue; }
            f[i] = p;
            char *tab = strchr(p, '\t');
            if (tab) { *tab = 0; p = tab + 1; } else p = NULL;
        }
        am_item it;
        memset(&it, 0, sizeof it);
        copy_utf8(it.id, sizeof it.id, f[0]);
        copy_utf8(it.type, sizeof it.type, f[1]);
        copy_utf8(it.href, sizeof it.href, f[2]);
        copy_utf8(it.name, sizeof it.name, f[3]);
        copy_utf8(it.artist, sizeof it.artist, f[4]);
        copy_utf8(it.album, sizeof it.album, f[5]);
        it.dur_ms = atoi(f[6]);
        it.tracks = atoi(f[7]);
        copy_utf8(it.art, sizeof it.art, f[8]);
        if (!it.name[0]) continue;
        pthread_mutex_lock(&mtx);
        if (l->epoch == epoch) {
            if (l->n == l->cap) {
                l->cap = l->cap ? l->cap * 2 : 64;
                l->items = realloc(l->items, sizeof *l->items * l->cap);
            }
            l->items[l->n++] = it;
        }
        pthread_mutex_unlock(&mtx);
    }
    pthread_mutex_lock(&mtx);
    if (l->epoch == epoch) {
        l->more = next >= 0;
        l->next_offset = next;
    }
    pthread_mutex_unlock(&mtx);
}

static void run_list_job(int slot)
{
    list *l = &L[slot];
    char path[512];
    pthread_mutex_lock(&mtx);
    int epoch = l->epoch, job = l->job;
    int off = job == 2 ? l->next_offset : 0;
    snprintf(path, sizeof path, "%s%sfmt=text&offset=%d", l->path, strchr(l->path, '?') ? "&" : "?", off);
    l->job = 0;
    pthread_mutex_unlock(&mtx);
    char *t = request("GET", path, NULL);
    if (t) parse_items(t, l, epoch);
    pthread_mutex_lock(&mtx);
    if (l->epoch == epoch) {
        l->loading = 0;
        if (!t) snprintf(l->err, sizeof l->err, "%s", st.connected ? "Couldn't load this list" : "Lost the connection to your PC");
    }
    pthread_mutex_unlock(&mtx);
    free(t);
}

// ---------------------------------------------------------------- art
static void fetch_art(const char *path)
{
    char *h = NULL;
    int p;
    pthread_mutex_lock(&mtx);
    h = strdup(host);
    p = port;
    pthread_mutex_unlock(&mtx);
    uint8_t *px = NULL;
    int w = 0, hh = 0;
    if (path[0]) {
        char *out = NULL;
        int len = 0;
        if (net_http(h, p, "GET", path, NULL, &out, &len, TIMEOUT_MS) == 200 && len > 0) {
            int n;
            px = stbi_load_from_memory((const unsigned char *)out, len, &w, &hh, &n, 4);
        }
        free(out);
    }
    free(h);
    pthread_mutex_lock(&mtx);
    free(art_px);
    art_px = px;
    art_w = w;
    art_h = hh;
    art_new = 1;
    snprintf(art_loaded, sizeof art_loaded, "%s", path);
    pthread_mutex_unlock(&mtx);
}

// ---------------------------------------------------------------- thread
static void *worker(void *arg)
{
    (void)arg;
    while (1) {
        pthread_mutex_lock(&mtx);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (!kick) pthread_cond_timedwait(&cv, &mtx, &ts);
        kick = 0;
        if (!running) { pthread_mutex_unlock(&mtx); break; }
        int pending = qh != qt;
        for (int i = 0; i < AM_SLOTS; i++) if (L[i].job) pending = 1;
        int act = active, have = found;
        if (want_rediscover) { want_rediscover = 0; found = have = 0; next_search = 0; }
        double t = am_now();
        int search = !have && (act || pending) && t >= next_search;
        pthread_mutex_unlock(&mtx);

        if (search) { find_bridge(); continue; }
        if (!have) {
            // can't do anything yet: fail list jobs so the UI can say why
            pthread_mutex_lock(&mtx);
            for (int i = 0; i < AM_SLOTS; i++) if (L[i].job) {
                L[i].job = 0;
                L[i].loading = 0;
                snprintf(L[i].err, sizeof L[i].err, "Can't find Cider Bridge on your network");
            }
            qh = qt;   // drop commands
            pthread_mutex_unlock(&mtx);
            continue;
        }

        // commands first: they're what the user is waiting on
        while (1) {
            cmd c;
            pthread_mutex_lock(&mtx);
            if (qh == qt) { pthread_mutex_unlock(&mtx); break; }
            c = q[qh];
            qh = (qh + 1) % QN;
            pthread_mutex_unlock(&mtx);
            char *r = request("POST", c.path, c.body[0] ? c.body : "{}");
            if (r) { apply_status(r, 0); free(r); }
            next_poll = 0;   // show the result right away
        }
        for (int i = 0; i < AM_SLOTS; i++) {
            pthread_mutex_lock(&mtx);
            int job = L[i].job;
            pthread_mutex_unlock(&mtx);
            if (job) run_list_job(i);
        }
        if (act && am_now() >= next_poll) {
            poll_status();
            next_poll = am_now() + POLL_S;
            char want[400];
            int need;
            pthread_mutex_lock(&mtx);
            snprintf(want, sizeof want, "%s", art_path);
            need = strcmp(want, art_loaded) != 0;
            pthread_mutex_unlock(&mtx);
            if (need) fetch_art(want);
        }
    }
    return NULL;
}

static void wake(void)
{
    kick = 1;
    pthread_cond_broadcast(&cv);
}

void am_init(const char *data_dir)
{
    snprintf(cfg_path, sizeof cfg_path, "%s/cider_bridge.txt", data_dir);
    running = 1;
    pthread_create(&thr, NULL, worker, NULL);
}

void am_quit(void)
{
    pthread_mutex_lock(&mtx);
    running = 0;
    wake();
    pthread_mutex_unlock(&mtx);
    pthread_join(thr, NULL);
}

void am_set_active(int on)
{
    pthread_mutex_lock(&mtx);
    if (on && !active) { next_poll = 0; next_search = 0; }
    active = on;
    wake();
    pthread_mutex_unlock(&mtx);
}

void am_get_status(am_status *out)
{
    pthread_mutex_lock(&mtx);
    *out = st;
    pthread_mutex_unlock(&mtx);
}

void am_rediscover(void)
{
    pthread_mutex_lock(&mtx);
    want_rediscover = 1;
    wake();
    pthread_mutex_unlock(&mtx);
}

int am_stream_url(char *out, int n)
{
    pthread_mutex_lock(&mtx);
    int ok = found;
    if (ok) snprintf(out, n, "http://%s:%d/stream.mp3", host, port);
    pthread_mutex_unlock(&mtx);
    return ok;
}

static void push(const char *path, const char *body)
{
    pthread_mutex_lock(&mtx);
    int nt = (qt + 1) % QN;
    if (nt != qh) {
        snprintf(q[qt].path, sizeof q[qt].path, "%s", path);
        snprintf(q[qt].body, sizeof q[qt].body, "%s", body ? body : "");
        qt = nt;
    }
    wake();
    pthread_mutex_unlock(&mtx);
}

void am_cmd(const char *name)
{
    char p[64];
    snprintf(p, sizeof p, "/api/%s?fmt=text", name);
    push(p, NULL);
}

void am_seek(int seconds)
{
    char b[48];
    snprintf(b, sizeof b, "{\"seconds\": %d}", seconds < 0 ? 0 : seconds);
    push("/api/seek?fmt=text", b);
    // show the new position right away instead of waiting for the next poll
    pthread_mutex_lock(&mtx);
    st.pos_s = seconds < 0 ? 0 : seconds;
    st.pos_at = am_now();
    pthread_mutex_unlock(&mtx);
}

// JSON string escaping for values we send back to the bridge
static void json_str(char *out, int n, const char *s)
{
    int o = 0;
    out[o++] = '"';
    for (; *s && o < n - 3; s++) {
        if (*s == '"' || *s == '\\') { out[o++] = '\\'; out[o++] = *s; }
        else if ((unsigned char)*s >= 0x20) out[o++] = *s;
    }
    out[o++] = '"';
    out[o] = 0;
}

void am_play_item(const am_item *it)
{
    char b[600], a[300], c[120];
    if (it->href[0]) {
        json_str(a, sizeof a, it->href);
        snprintf(b, sizeof b, "{\"href\": %s}", a);
    } else {
        json_str(a, sizeof a, it->type);
        json_str(c, sizeof c, it->id);
        snprintf(b, sizeof b, "{\"type\": %s, \"id\": %s}", a, c);
    }
    push("/api/play_item?fmt=text", b);
}

void am_pause_now(void)
{
    char h[64];
    int p, ok;
    pthread_mutex_lock(&mtx);
    ok = found;
    snprintf(h, sizeof h, "%s", host);
    p = port;
    pthread_mutex_unlock(&mtx);
    if (!ok) return;
    char *out = NULL;
    net_http(h, p, "POST", "/api/pause?fmt=text", "{}", &out, NULL, 1200);
    free(out);
}

// ---------------------------------------------------------------- lists API
void am_list_load(int slot, const char *api_path)
{
    if (slot < 0 || slot >= AM_SLOTS) return;
    pthread_mutex_lock(&mtx);
    list *l = &L[slot];
    l->epoch++;
    l->n = 0;
    l->more = 0;
    l->next_offset = 0;
    l->err[0] = 0;
    l->loading = 1;
    l->job = 1;
    snprintf(l->path, sizeof l->path, "%s", api_path);
    wake();
    pthread_mutex_unlock(&mtx);
}

void am_list_more(int slot)
{
    if (slot < 0 || slot >= AM_SLOTS) return;
    pthread_mutex_lock(&mtx);
    list *l = &L[slot];
    if (l->more && !l->loading) {
        l->loading = 1;
        l->job = 2;
        wake();
    }
    pthread_mutex_unlock(&mtx);
}

void am_list_clear(int slot)
{
    if (slot < 0 || slot >= AM_SLOTS) return;
    pthread_mutex_lock(&mtx);
    L[slot].epoch++;
    L[slot].n = 0;
    L[slot].job = 0;
    L[slot].loading = 0;
    L[slot].more = 0;
    L[slot].err[0] = 0;
    pthread_mutex_unlock(&mtx);
}

int am_list_count(int slot)
{
    if (slot < 0 || slot >= AM_SLOTS) return 0;
    pthread_mutex_lock(&mtx);
    int n = L[slot].n;
    pthread_mutex_unlock(&mtx);
    return n;
}

int am_list_get(int slot, int i, am_item *out)
{
    if (slot < 0 || slot >= AM_SLOTS) return 0;
    pthread_mutex_lock(&mtx);
    int ok = i >= 0 && i < L[slot].n;
    if (ok) *out = L[slot].items[i];
    pthread_mutex_unlock(&mtx);
    return ok;
}

int am_list_state(int slot, int *more, char *err, int errn)
{
    if (slot < 0 || slot >= AM_SLOTS) return 0;
    pthread_mutex_lock(&mtx);
    int loading = L[slot].loading;
    if (more) *more = L[slot].more;
    if (err) snprintf(err, errn, "%s", L[slot].err);
    pthread_mutex_unlock(&mtx);
    return loading;
}

int am_take_art(uint8_t **rgba, int *w, int *h)
{
    pthread_mutex_lock(&mtx);
    int n = art_new;
    if (n) {
        *rgba = art_px;
        *w = art_w;
        *h = art_h;
        art_px = NULL;   // ownership moves to the caller
        art_new = 0;
    }
    pthread_mutex_unlock(&mtx);
    return n;
}
