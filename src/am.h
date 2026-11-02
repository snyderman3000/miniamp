#ifndef AM_H
#define AM_H
#include <stdint.h>

// Apple Music through Cider Bridge (a small service on the user's PC that
// streams Cider's audio and relays browse/playback commands; see
// bridge/README.md). All network work happens on a background thread;
// these calls never block the UI.

typedef struct {
    char id[96], type[48], href[256];   // type "_section": a heading, not an item
    char name[200], artist[160], album[160];
    char art[400];          // bridge path for the artwork, "" if none
    int dur_ms, tracks;
} am_item;

typedef struct {
    int found;              // bridge located on the network
    int connected;          // last request to it succeeded
    int searching;          // looking for it right now
    int cider;              // Cider is running on the PC
    int playing, shuffle, repeat, kbps;
    int generation;         // bumps when the PC skips/seeks/starts something
    int has_track;
    char name[200], artist[160], album[160];
    int dur_ms;
    double pos_s;           // position reported by Cider...
    double pos_at;          // ...at this time (am_now())
    char host[64];
    int port;
} am_status;

void am_init(const char *data_dir);
void am_quit(void);
double am_now(void);

// Polls the bridge while active (Apple Music screen open or playing).
void am_set_active(int on);
void am_get_status(am_status *out);
void am_rediscover(void);
// http://host:port/stream.mp3, or 0 if the bridge hasn't been found
int am_stream_url(char *out, int n);

// Playback commands: "play", "pause", "playpause", "next", "previous", "shuffle", "repeat"
void am_cmd(const char *name);
void am_seek(int seconds);
void am_play_item(const am_item *it);
// Sends "pause" and waits for it (used when quitting or switching to local files).
void am_pause_now(void);

// Lists. Slots are independent views (0..AM_SLOTS-1) filled in the background.
#define AM_SLOTS 4
void am_list_load(int slot, const char *api_path);    // e.g. "/api/library/playlists"
void am_list_more(int slot);                          // next page, if any
void am_list_clear(int slot);
int am_list_count(int slot);
int am_list_get(int slot, int i, am_item *out);
// returns 1 while loading; *more = another page exists; err gets a message on failure
int am_list_state(int slot, int *more, char *err, int errn);

// Album art for the current track: returns 1 when it changed since the last
// call. *rgba (malloc'd, caller frees) is NULL when there is no art.
int am_take_art(uint8_t **rgba, int *w, int *h);

#endif
