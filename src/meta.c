// Tag + stream info readers: ID3v1/v2 + MPEG headers, FLAC, Ogg Vorbis, WAV.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <sys/stat.h>
#include "meta.h"

// ---------------------------------------------------------------- utf-8
void utf8_put(char **o, char *end, unsigned cp)
{
    char *p = *o;
    if (cp < 0x80) { if (p + 1 < end) *p++ = (char)cp; }
    else if (cp < 0x800) { if (p + 2 < end) { *p++ = 0xC0 | (cp >> 6); *p++ = 0x80 | (cp & 63); } }
    else if (cp < 0x10000) { if (p + 3 < end) { *p++ = 0xE0 | (cp >> 12); *p++ = 0x80 | ((cp >> 6) & 63); *p++ = 0x80 | (cp & 63); } }
    else if (p + 4 < end) { *p++ = 0xF0 | (cp >> 18); *p++ = 0x80 | ((cp >> 12) & 63); *p++ = 0x80 | ((cp >> 6) & 63); *p++ = 0x80 | (cp & 63); }
    *o = p;
    if (p < end) *p = 0;
}

int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p;
    if (!c) return 0;
    int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
    if (c >= 0x80 && c < 0xC0) { *s += 1; return 0xFFFD; }
    unsigned cp = n == 0 ? c : n == 1 ? c & 31 : n == 2 ? c & 15 : c & 7;
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += i; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 63);
    }
    *s += n + 1;
    return (int)cp;
}

static void set_latin1(char *dst, int n, const unsigned char *src, int len)
{
    char *o = dst, *end = dst + n;
    *o = 0;
    for (int i = 0; i < len && src[i]; i++) utf8_put(&o, end, src[i]);
}

static void set_utf8(char *dst, int n, const unsigned char *src, int len)
{
    if (len < 0) len = 0;
    if (len >= n) len = n - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
    // don't leave a cut multi-byte sequence at the end
    int i = len;
    while (i > 0 && ((unsigned char)dst[i - 1] & 0xC0) == 0x80) i--;
    if (i > 0 && (unsigned char)dst[i - 1] >= 0xC0) {
        unsigned c = (unsigned char)dst[i - 1];
        int need = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (len - (i - 1) < need) dst[i - 1] = 0;
    }
}

static void set_utf16(char *dst, int n, const unsigned char *src, int len, int be)
{
    char *o = dst, *end = dst + n;
    *o = 0;
    for (int i = 0; i + 1 < len; i += 2) {
        unsigned u = be ? (src[i] << 8 | src[i + 1]) : (src[i + 1] << 8 | src[i]);
        if (!u) break;
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < len) {
            unsigned l = be ? (src[i + 2] << 8 | src[i + 3]) : (src[i + 3] << 8 | src[i + 2]);
            if (l >= 0xDC00 && l < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00); i += 2; }
        }
        utf8_put(&o, end, u);
    }
}

static void trim(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t')) s[--n] = 0;
    int i = 0;
    while (s[i] == ' ') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
static uint32_t sync32(const unsigned char *p) { return (p[0] & 127) << 21 | (p[1] & 127) << 14 | (p[2] & 127) << 7 | (p[3] & 127); }

static long file_size(FILE *f)
{
    struct stat st;
    return fstat(fileno(f), &st) == 0 ? (long)st.st_size : 0;
}

// ---------------------------------------------------------------- ID3v2
static void id3_text(char *dst, int n, const unsigned char *p, int len)
{
    if (len < 1) return;
    int enc = p[0];
    p++; len--;
    if (enc == 0) set_latin1(dst, n, p, len);
    else if (enc == 3) set_utf8(dst, n, p, len);
    else if (enc == 2) set_utf16(dst, n, p, len, 1);
    else if (len >= 2) {
        int be = (p[0] == 0xFE && p[1] == 0xFF);
        int bom = (p[0] == 0xFE && p[1] == 0xFF) || (p[0] == 0xFF && p[1] == 0xFE);
        set_utf16(dst, n, p + (bom ? 2 : 0), len - (bom ? 2 : 0), be);
    }
    trim(dst);
}

// returns the tag size including header (0 if none)
static long id3v2_read(FILE *f, track_meta *m, long *tlen_ms)
{
    unsigned char h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return 0;
    int ver = h[3];
    long size = sync32(h + 6) + 10 + ((h[5] & 0x10) ? 10 : 0);
    long end = sync32(h + 6) + 10;
    long pos = 10;
    if ((h[5] & 0x40) && ver >= 3) { // extended header
        unsigned char e[4];
        if (fread(e, 1, 4, f) != 4) return size;
        long es = ver == 4 ? sync32(e) : be32(e) + 4;
        pos += es;
        fseek(f, pos, SEEK_SET);
    }
    while (pos + (ver == 2 ? 6 : 10) <= end) {
        unsigned char fh[10];
        int hl = ver == 2 ? 6 : 10;
        if (fread(fh, 1, hl, f) != (size_t)hl) break;
        if (!fh[0]) break;
        char id[5] = {0};
        long fl;
        if (ver == 2) { memcpy(id, fh, 3); fl = fh[3] << 16 | fh[4] << 8 | fh[5]; }
        else { memcpy(id, fh, 4); fl = ver == 4 ? sync32(fh + 4) : be32(fh + 4); }
        pos += hl;
        if (fl <= 0 || pos + fl > end) break;
        char *dst = NULL;
        int is_len = 0;
        if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) dst = m->title;
        else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) dst = m->artist;
        else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) dst = m->album;
        else if (!strcmp(id, "TLEN") || !strcmp(id, "TLE")) is_len = 1;
        if ((dst || is_len) && fl < 4096) {
            unsigned char buf[4096];
            if (fread(buf, 1, fl, f) != (size_t)fl) break;
            if (dst) id3_text(dst, 160, buf, (int)fl);
            else { char t[32] = {0}; id3_text(t, sizeof t, buf, (int)fl); *tlen_ms = atol(t); }
        } else {
            fseek(f, fl, SEEK_CUR);
        }
        pos += fl;
    }
    return size;
}

static void id3v1_read(FILE *f, track_meta *m)
{
    unsigned char t[128];
    if (fseek(f, -128, SEEK_END) != 0 || fread(t, 1, 128, f) != 128 || memcmp(t, "TAG", 3)) return;
    if (!m->title[0]) { set_latin1(m->title, 160, t + 3, 30); trim(m->title); }
    if (!m->artist[0]) { set_latin1(m->artist, 160, t + 33, 30); trim(m->artist); }
    if (!m->album[0]) { set_latin1(m->album, 160, t + 63, 30); trim(m->album); }
}

// ---------------------------------------------------------------- MPEG audio
static const int BR[2][16] = { // [mpeg1?][index] for layer III
    { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 } };
static const int BR_L2[16] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 };
static const int SR[4][3] = { { 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 } };

static int read_mp3(FILE *f, track_meta *m)
{
    long tlen = 0;
    long start = id3v2_read(f, m, &tlen);
    long fsize = file_size(f);
    unsigned char buf[8192];
    fseek(f, start, SEEK_SET);
    long base = start;
    for (int tries = 0; tries < 16; tries++) {
        size_t n = fread(buf, 1, sizeof buf, f);
        if (n < 64) break;
        for (size_t i = 0; i + 64 < n; i++) {
            if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;
            int verbits = (buf[i + 1] >> 3) & 3, layer = (buf[i + 1] >> 1) & 3;
            int bri = buf[i + 2] >> 4, sri = (buf[i + 2] >> 2) & 3, mode = buf[i + 3] >> 6;
            if (verbits == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3) continue;
            int mpeg1 = verbits == 3;
            int kbps = layer == 1 ? BR[mpeg1][bri] : (layer == 2 && mpeg1) ? BR_L2[bri] : BR[0][bri];
            if (layer == 3) kbps = mpeg1 ? (int[]){0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0}[bri] : BR_L2[bri];
            int sr = SR[verbits][sri];
            int spf = layer == 3 ? 384 : (layer == 1 && !mpeg1) ? 576 : 1152;
            m->samplerate = sr;
            m->channels = mode == 3 ? 1 : 2;
            m->kbps = kbps;
            // Xing/Info or VBRI header for VBR length
            int side = mpeg1 ? (mode == 3 ? 17 : 32) : (mode == 3 ? 9 : 17);
            const unsigned char *x = buf + i + 4 + side;
            long frames = 0;
            if (i + 4 + side + 16 < n && (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4)) && (be32(x + 4) & 1))
                frames = be32(x + 8);
            else if (i + 36 + 18 < n && !memcmp(buf + i + 36, "VBRI", 4))
                frames = be32(buf + i + 36 + 14);
            long audio = fsize - (base + (long)i);
            if (frames > 0) {
                m->dur_ms = (int)((double)frames * spf * 1000.0 / sr);
                if (m->dur_ms > 0) m->kbps = (int)(audio * 8.0 / m->dur_ms);
            } else if (kbps > 0) {
                m->dur_ms = (int)(audio * 8.0 / kbps);
            }
            if (tlen > 0 && frames == 0) m->dur_ms = (int)tlen;
            id3v1_read(f, m);
            return 1;
        }
        base += (long)n - 64;
        fseek(f, base, SEEK_SET);
    }
    id3v1_read(f, m);
    return 1;
}

// ---------------------------------------------------------------- Vorbis comments
static void vorbis_comments(track_meta *m, const unsigned char *p, long len)
{
    if (len < 8) return;
    long vl = le32(p);
    long pos = 4 + vl;
    if (pos + 4 > len) return;
    long count = le32(p + pos);
    pos += 4;
    for (long i = 0; i < count && pos + 4 <= len; i++) {
        long cl = le32(p + pos);
        pos += 4;
        if (cl < 0 || pos + cl > len) break;
        const char *c = (const char *)p + pos;
        const char *eq = memchr(c, '=', cl);
        if (eq) {
            int kl = (int)(eq - c);
            char *dst = NULL;
            if (kl == 5 && !strncasecmp(c, "TITLE", 5)) dst = m->title;
            else if (kl == 6 && !strncasecmp(c, "ARTIST", 6)) dst = m->artist;
            else if (kl == 5 && !strncasecmp(c, "ALBUM", 5)) dst = m->album;
            if (dst && !dst[0]) { set_utf8(dst, 160, (const unsigned char *)eq + 1, (int)(cl - kl - 1)); trim(dst); }
        }
        pos += cl;
    }
}

// ---------------------------------------------------------------- FLAC
static int read_flac(FILE *f, track_meta *m)
{
    unsigned char h[4];
    long tlen = 0;
    long start = id3v2_read(f, m, &tlen);
    fseek(f, start, SEEK_SET);
    if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) return 0;
    uint64_t total = 0;
    for (int blocks = 0; blocks < 64; blocks++) {
        unsigned char bh[4];
        if (fread(bh, 1, 4, f) != 4) break;
        int last = bh[0] >> 7, type = bh[0] & 127;
        long len = bh[1] << 16 | bh[2] << 8 | bh[3];
        if (type == 0 && len >= 18) {
            unsigned char s[34];
            if (fread(s, 1, 18, f) != 18) break;
            m->samplerate = s[10] << 12 | s[11] << 4 | s[12] >> 4;
            m->channels = ((s[12] >> 1) & 7) + 1;
            total = ((uint64_t)(s[13] & 15) << 32) | be32(s + 14);
            fseek(f, len - 18, SEEK_CUR);
        } else if (type == 4 && len < (1 << 20)) {
            unsigned char *p = malloc(len);
            if (!p || fread(p, 1, len, f) != (size_t)len) { free(p); break; }
            vorbis_comments(m, p, len);
            free(p);
        } else {
            fseek(f, len, SEEK_CUR);
        }
        if (last) break;
    }
    if (m->samplerate && total) {
        m->dur_ms = (int)(total * 1000 / m->samplerate);
        if (m->dur_ms > 0) m->kbps = (int)(file_size(f) * 8.0 / m->dur_ms);
    }
    return 1;
}

// ---------------------------------------------------------------- Ogg Vorbis
static int read_ogg(FILE *f, track_meta *m)
{
    long cap = 512 * 1024;
    unsigned char *pkt = malloc(cap);
    if (!pkt) return 0;
    long plen = 0;
    int npkt = 0;
    unsigned serial = 0;
    unsigned char ph[27], segs[255];
    while (npkt < 2) {
        if (fread(ph, 1, 27, f) != 27 || memcmp(ph, "OggS", 4)) break;
        unsigned s = le32(ph + 14);
        if (!serial) serial = s;
        int ns = ph[26];
        if (fread(segs, 1, ns, f) != (size_t)ns) break;
        for (int i = 0; i < ns && npkt < 2; i++) {
            int sl = segs[i];
            if (s != serial) { fseek(f, sl, SEEK_CUR); continue; }
            if (plen + sl <= cap) { if (fread(pkt + plen, 1, sl, f) != (size_t)sl) goto done; plen += sl; }
            else fseek(f, sl, SEEK_CUR);
            if (sl < 255) { // packet complete
                if (npkt == 0 && plen >= 30 && pkt[0] == 1 && !memcmp(pkt + 1, "vorbis", 6)) {
                    m->channels = pkt[11];
                    m->samplerate = le32(pkt + 12);
                    int nominal = (int)le32(pkt + 20);
                    if (nominal > 0) m->kbps = nominal / 1000;
                } else if (npkt == 1 && plen > 7 && pkt[0] == 3 && !memcmp(pkt + 1, "vorbis", 6)) {
                    vorbis_comments(m, pkt + 7, plen - 7);
                } else if (npkt == 0) {
                    goto done; // not vorbis (e.g. Opus)
                }
                npkt++;
                plen = 0;
            }
        }
    }
    // length: granule position of the last page
    {
        long fs = file_size(f), back = fs > 65536 ? 65536 : fs;
        unsigned char *t = realloc(pkt, back);
        if (t) {
            pkt = t;
            fseek(f, fs - back, SEEK_SET);
            long n = (long)fread(pkt, 1, back, f);
            for (long i = n - 27; i >= 0; i--) {
                if (!memcmp(pkt + i, "OggS", 4) && le32(pkt + i + 14) == serial) {
                    uint64_t g = (uint64_t)le32(pkt + i + 10) << 32 | le32(pkt + i + 6);
                    if (m->samplerate > 0 && g != (uint64_t)-1) m->dur_ms = (int)(g * 1000 / m->samplerate);
                    break;
                }
            }
            if (m->dur_ms > 0 && !m->kbps) m->kbps = (int)(fs * 8.0 / m->dur_ms);
        }
    }
done:
    free(pkt);
    return m->samplerate > 0;
}

// ---------------------------------------------------------------- WAV
static int read_wav(FILE *f, track_meta *m)
{
    unsigned char h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return 0;
    int bits = 16;
    for (int i = 0; i < 64; i++) {
        unsigned char c[8];
        if (fread(c, 1, 8, f) != 8) break;
        long len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4) && len >= 16) {
            unsigned char fm[16];
            if (fread(fm, 1, 16, f) != 16) break;
            m->channels = fm[2] | fm[3] << 8;
            m->samplerate = le32(fm + 4);
            bits = fm[14] | fm[15] << 8;
            fseek(f, len - 16 + (len & 1), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            if (m->samplerate && m->channels && bits) {
                long fs = file_size(f);
                long avail = fs - ftell(f);
                if (len <= 0 || len > avail) len = avail;
                m->dur_ms = (int)((double)len * 8000.0 / ((double)m->samplerate * m->channels * bits));
                m->kbps = m->samplerate * m->channels * bits / 1000;
            }
            break;
        } else {
            fseek(f, len + (len & 1), SEEK_CUR);
        }
    }
    return 1;
}

int meta_read(const char *path, track_meta *m)
{
    memset(m, 0, sizeof *m);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const char *e = strrchr(path, '.');
    e = e ? e + 1 : "";
    int ok = 0;
    if (!strcasecmp(e, "mp3")) ok = read_mp3(f, m);
    else if (!strcasecmp(e, "flac")) ok = read_flac(f, m);
    else if (!strcasecmp(e, "ogg") || !strcasecmp(e, "oga")) ok = read_ogg(f, m);
    else if (!strcasecmp(e, "wav")) ok = read_wav(f, m);
    fclose(f);
    return ok;
}

void meta_display_name(const track_meta *m, const char *path, char *out, int n)
{
    if (m && m->title[0] && m->artist[0]) snprintf(out, n, "%s - %s", m->artist, m->title);
    else if (m && m->title[0]) snprintf(out, n, "%s", m->title);
    else {
        const char *b = strrchr(path, '/');
        b = b ? b + 1 : path;
        snprintf(out, n, "%s", b);
        char *dot = strrchr(out, '.');
        if (dot && dot != out) *dot = 0;
    }
}
