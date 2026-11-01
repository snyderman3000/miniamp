#ifndef NET_H
#define NET_H

// Minimal networking for talking to Cider Bridge on the local network.

// TCP connect with a timeout; returns a socket or -1.
int net_connect(const char *host, int port, int timeout_ms);

// HTTP/1.0 request. body may be NULL (sent as JSON when given).
// Returns the HTTP status code, or -1 on a network error. On success *out is a
// malloc'd, NUL-terminated response body (caller frees) and *outlen its length.
int net_http(const char *host, int port, const char *method, const char *path,
             const char *body, char **out, int *outlen, int timeout_ms);

// Opens a streaming GET and consumes the response headers.
// Returns the socket positioned at the body, or -1.
int net_http_stream(const char *host, int port, const char *path, int timeout_ms);

// Broadcasts "MINIAMP_DISCOVER" on UDP `udp_port` and waits for a
// "CIDER_BRIDGE <port>" reply. Returns 1 and fills host/port on success.
int net_discover(int udp_port, char *host, int hostn, int *port, int timeout_ms);

#endif
