#ifndef META_H
#define META_H

typedef struct {
    char title[160], artist[160], album[160];
    int dur_ms;       // 0 if unknown
    int kbps;
    int samplerate;
    int channels;
} track_meta;

// Reads tags and stream properties without decoding audio. Returns 1 if the
// file could be read at all.
int meta_read(const char *path, track_meta *m);

// "Artist - Title", or the file name without its extension
void meta_display_name(const track_meta *m, const char *path, char *out, int n);

// utf-8 helpers
int utf8_next(const char **s);                 // returns codepoint, advances
void utf8_put(char **o, char *end, unsigned cp);

#endif
