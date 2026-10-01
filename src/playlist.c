// The play list, plus a background thread that reads tags for its entries.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include "playlist.h"
#include "decode.h"

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static int running;
static pl_item *items;
static int count, cap;
static unsigned gen;
static int hint;

void pl_lock(void) { pthread_mutex_lock(&mtx); }
void pl_unlock(void) { pthread_mutex_unlock(&mtx); }

static void name_from_path(const char *path, char *out, int n)
{
    meta_display_name(NULL, path, out, n);
}

static void *meta_worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mtx);
    while (running) {
        int idx = -1;
        // first unread item at or after the hint, then from the top
        for (int k = 0; k < count && idx < 0; k++) {
            int i = (hint + k) % count;
            if (!items[i].has_meta) idx = i;
        }
        if (idx < 0) { pthread_cond_wait(&cv, &mtx); continue; }
        char *path = strdup(items[idx].path);
        unsigned g = gen;
        pthread_mutex_unlock(&mtx);
        track_meta m;
        int ok = meta_read(path, &m);
        pthread_mutex_lock(&mtx);
        if (g == gen && idx < count && !strcmp(items[idx].path, path)) {
            items[idx].has_meta = 1;
            if (ok) {
                meta_display_name(&m, path, items[idx].name, sizeof items[idx].name);
                items[idx].dur_ms = m.dur_ms;
            }
        }
        free(path);
    }
    pthread_mutex_unlock(&mtx);
    return NULL;
}

void pl_init(void)
{
    running = 1;
    pthread_create(&worker, NULL, meta_worker, NULL);
}

void pl_quit(void)
{
    pthread_mutex_lock(&mtx);
    running = 0;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mtx);
    pthread_join(worker, NULL);
}

int pl_count(void)
{
    pthread_mutex_lock(&mtx);
    int n = count;
    pthread_mutex_unlock(&mtx);
    return n;
}

void pl_clear(void)
{
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < count; i++) free(items[i].path);
    count = 0;
    gen++;
    pthread_mutex_unlock(&mtx);
}

static void add_locked(const char *path)
{
    if (count == cap) {
        cap = cap ? cap * 2 : 256;
        items = realloc(items, sizeof *items * cap);
    }
    pl_item *it = &items[count++];
    memset(it, 0, sizeof *it);
    it->path = strdup(path);
    name_from_path(path, it->name, sizeof it->name);
}

void pl_add(const char *path)
{
    pthread_mutex_lock(&mtx);
    add_locked(path);
    gen++;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mtx);
}

int name_cmp(const char *a, const char *b)
{
    // case-insensitive, numbers compared by value ("2 x" before "10 x")
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea, *eb;
            unsigned long long va = strtoull(a, &ea, 10), vb = strtoull(b, &eb, 10);
            if (va != vb) return va < vb ? -1 : 1;
            a = ea; b = eb;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int cmp_str(const void *x, const void *y) { return name_cmp(*(char *const *)x, *(char *const *)y); }

static int add_dir_rec(const char *dir, int recursive, int depth, int *budget)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    char **files = NULL, **dirs = NULL;
    int nf = 0, nd = 0, cf = 0, cd = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!recursive || depth >= 8) continue;
            if (nd == cd) { cd = cd ? cd * 2 : 16; dirs = realloc(dirs, sizeof(char *) * cd); }
            dirs[nd++] = strdup(e->d_name);
        } else if (dec_supported(e->d_name)) {
            if (nf == cf) { cf = cf ? cf * 2 : 64; files = realloc(files, sizeof(char *) * cf); }
            files[nf++] = strdup(e->d_name);
        }
    }
    closedir(d);
    if (nf) qsort(files, nf, sizeof(char *), cmp_str);
    if (nd) qsort(dirs, nd, sizeof(char *), cmp_str);
    int added = 0;
    for (int i = 0; i < nf; i++) {
        if (*budget > 0) {
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", dir, files[i]);
            pthread_mutex_lock(&mtx);
            add_locked(p);
            pthread_mutex_unlock(&mtx);
            added++;
            (*budget)--;
        }
        free(files[i]);
    }
    for (int i = 0; i < nd; i++) {
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, dirs[i]);
        if (*budget > 0) added += add_dir_rec(p, recursive, depth + 1, budget);
        free(dirs[i]);
    }
    free(files);
    free(dirs);
    return added;
}

int pl_add_dir(const char *dir, int recursive)
{
    int budget = 10000;
    int n = add_dir_rec(dir, recursive, 0, &budget);
    pthread_mutex_lock(&mtx);
    gen++;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mtx);
    return n;
}

int pl_get(int i, pl_item *out)
{
    pthread_mutex_lock(&mtx);
    int ok = i >= 0 && i < count;
    if (ok) *out = items[i];
    pthread_mutex_unlock(&mtx);
    return ok;
}

const char *pl_path(int i)
{
    pthread_mutex_lock(&mtx);
    const char *p = i >= 0 && i < count ? items[i].path : NULL;
    pthread_mutex_unlock(&mtx);
    return p;
}

void pl_set_hint(int first)
{
    pthread_mutex_lock(&mtx);
    hint = first < 0 ? 0 : first;
    pthread_mutex_unlock(&mtx);
}

long long pl_total_ms(int *complete)
{
    pthread_mutex_lock(&mtx);
    long long t = 0;
    int all = 1;
    for (int i = 0; i < count; i++) { t += items[i].dur_ms; if (!items[i].has_meta) all = 0; }
    pthread_mutex_unlock(&mtx);
    if (complete) *complete = all;
    return t;
}

void pl_remove(int i)
{
    pthread_mutex_lock(&mtx);
    if (i >= 0 && i < count) {
        free(items[i].path);
        memmove(items + i, items + i + 1, sizeof *items * (count - i - 1));
        count--;
        gen++;
    }
    pthread_mutex_unlock(&mtx);
}

int pl_save_m3u(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    fprintf(f, "#EXTM3U\n");
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < count; i++) fprintf(f, "%s\n", items[i].path);
    pthread_mutex_unlock(&mtx);
    fclose(f);
    return rename(tmp, path) == 0;
}

int pl_load_m3u(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    char base[1024];
    snprintf(base, sizeof base, "%s", path);
    char *sl = strrchr(base, '/');
    if (sl) *sl = 0; else strcpy(base, ".");
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char p[2048];
        if (line[0] == '/') snprintf(p, sizeof p, "%s", line);
        else snprintf(p, sizeof p, "%s/%s", base, line);
        for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
        pthread_mutex_lock(&mtx);
        add_locked(p);
        pthread_mutex_unlock(&mtx);
        n++;
    }
    fclose(f);
    pthread_mutex_lock(&mtx);
    gen++;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mtx);
    return n;
}
