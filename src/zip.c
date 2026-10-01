// Minimal zip reader (stored + deflate) for .wsz skins, using zlib.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include "zip.h"

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

zip_archive *zip_open_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 22 || n > 64L * 1024 * 1024) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(n);
    if (!d || fread(d, 1, n, f) != (size_t)n) { free(d); fclose(f); return NULL; }
    fclose(f);
    zip_archive *z = calloc(1, sizeof *z);
    z->data = d;
    z->size = n;
    // find the end of central directory record
    long eocd = -1;
    for (long i = n - 22; i >= 0 && i >= n - 65557; i--)
        if (rd32(d + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { zip_close(z); return NULL; }
    unsigned count = rd16(d + eocd + 10);
    unsigned long cd = rd32(d + eocd + 16);
    z->entries = calloc(count ? count : 1, sizeof *z->entries);
    unsigned long p = cd;
    for (unsigned i = 0; i < count; i++) {
        if (p + 46 > (unsigned long)n || rd32(d + p) != 0x02014b50) break;
        zip_entry *e = &z->entries[z->count];
        e->method = rd16(d + p + 10);
        e->csize = rd32(d + p + 20);
        e->usize = rd32(d + p + 24);
        unsigned nl = rd16(d + p + 28), xl = rd16(d + p + 30), cl = rd16(d + p + 32);
        e->offset = rd32(d + p + 42);
        if (p + 46 + nl > (unsigned long)n) break;
        int len = nl < sizeof e->name - 1 ? (int)nl : (int)sizeof e->name - 1;
        memcpy(e->name, d + p + 46, len);
        e->name[len] = 0;
        z->count++;
        p += 46 + nl + xl + cl;
    }
    return z;
}

static const char *base_name(const char *s)
{
    const char *b = s;
    for (const char *p = s; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

const zip_entry *zip_find(const zip_archive *z, const char *name)
{
    const zip_entry *best = NULL;
    int best_depth = 1 << 20;
    for (int i = 0; i < z->count; i++) {
        const zip_entry *e = &z->entries[i];
        if (strcasecmp(base_name(e->name), name)) continue;
        if (!strncmp(e->name, "__MACOSX", 8)) continue;
        int depth = 0;
        for (const char *p = e->name; *p; p++) depth += (*p == '/' || *p == '\\');
        if (depth < best_depth) { best = e; best_depth = depth; }
    }
    return best;
}

unsigned char *zip_extract(const zip_archive *z, const zip_entry *e, size_t *out_len)
{
    if (!e || e->offset + 30 > (unsigned long)z->size) return NULL;
    const unsigned char *lh = z->data + e->offset;
    if (rd32(lh) != 0x04034b50) return NULL;
    unsigned long start = e->offset + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (start + e->csize > (unsigned long)z->size || e->usize > 32u * 1024 * 1024) return NULL;
    unsigned char *out = malloc(e->usize + 1);
    if (!out) return NULL;
    if (e->method == 0) {
        memcpy(out, z->data + start, e->usize);
    } else if (e->method == 8) {
        z_stream s;
        memset(&s, 0, sizeof s);
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { free(out); return NULL; }
        s.next_in = (unsigned char *)z->data + start;
        s.avail_in = e->csize;
        s.next_out = out;
        s.avail_out = e->usize;
        int r = inflate(&s, Z_FINISH);
        inflateEnd(&s);
        if (r != Z_STREAM_END && s.total_out != e->usize) { free(out); return NULL; }
    } else {
        free(out);
        return NULL;
    }
    out[e->usize] = 0;
    *out_len = e->usize;
    return out;
}

void zip_close(zip_archive *z)
{
    if (!z) return;
    free(z->entries);
    free(z->data);
    free(z);
}
