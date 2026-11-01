#ifndef NETSTREAM_H
#define NETSTREAM_H
#include <stddef.h>

// A live HTTP audio stream with a small jitter buffer. A reader thread pulls
// bytes off the network; ns_read hands them to the decoder.
typedef struct netstream netstream;

int ns_is_url(const char *path);               // "http://..."
netstream *ns_open(const char *url);           // connects; NULL on failure
// Blocks until data is available. Waits for the buffer to refill after an
// underrun so a Wi-Fi hiccup gives one pause instead of constant stutter.
// Returns 0 when the stream has ended or stalled for too long.
size_t ns_read(netstream *s, void *buf, size_t n);
void ns_close(netstream *s);
// Wakes a stream blocked waiting for data (it then ends). Safe from any thread.
void ns_interrupt(void);

#endif
