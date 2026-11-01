// Minimal TCP/HTTP/UDP helpers for Cider Bridge (see net.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "net.h"

static void set_timeouts(int fd, int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

int net_connect(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) || !res) return -1;
    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    // non-blocking connect so an absent host can't hang us for minutes
    // (ioctl rather than fcntl: the toolchain's fcntl needs a newer glibc than the Miyoo has)
    int nb = 1;
    ioctl(fd, FIONBIO, &nb);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (r < 0) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select(fd + 1, NULL, &w, NULL, &tv) <= 0) { close(fd); return -1; }
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) { close(fd); return -1; }
    }
    nb = 0;
    ioctl(fd, FIONBIO, &nb);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    set_timeouts(fd, timeout_ms);
    return fd;
}

static int send_all(int fd, const char *p, int n)
{
    while (n > 0) {
        ssize_t k = send(fd, p, (size_t)n, MSG_NOSIGNAL);
        if (k <= 0) return -1;
        p += k; n -= (int)k;
    }
    return 0;
}

static int send_request(int fd, const char *host, int port, const char *method, const char *path, const char *body)
{
    char head[1024];
    int blen = body ? (int)strlen(body) : 0;
    int n;
    if (body)
        n = snprintf(head, sizeof head,
                     "%s %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: MiniAmp\r\nConnection: close\r\n"
                     "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n",
                     method, path, host, port, blen);
    else
        n = snprintf(head, sizeof head,
                     "%s %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: MiniAmp\r\nConnection: close\r\n\r\n",
                     method, path, host, port);
    if (n <= 0 || n >= (int)sizeof head) return -1;
    if (send_all(fd, head, n)) return -1;
    if (body && send_all(fd, body, blen)) return -1;
    return 0;
}

int net_http(const char *host, int port, const char *method, const char *path,
             const char *body, char **out, int *outlen, int timeout_ms)
{
    *out = NULL;
    if (outlen) *outlen = 0;
    int fd = net_connect(host, port, timeout_ms);
    if (fd < 0) return -1;
    if (send_request(fd, host, port, method, path, body)) { close(fd); return -1; }
    int cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return -1; }
    while (1) {
        if (len + 4096 + 1 > cap) {
            if (cap >= (4 << 20)) break;   // 4 MB is plenty for any list we ask for
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        ssize_t k = recv(fd, buf + len, (size_t)(cap - len - 1), 0);
        if (k <= 0) break;
        len += (int)k;
    }
    close(fd);
    buf[len] = 0;
    int status = -1;
    if (len < 12 || sscanf(buf, "HTTP/%*d.%*d %d", &status) != 1) { free(buf); return -1; }
    char *b = strstr(buf, "\r\n\r\n");
    if (!b) { free(buf); return -1; }
    b += 4;
    int blen = len - (int)(b - buf);
    memmove(buf, b, (size_t)blen + 1);
    *out = buf;
    if (outlen) *outlen = blen;
    return status;
}

int net_http_stream(const char *host, int port, const char *path, int timeout_ms)
{
    int fd = net_connect(host, port, timeout_ms);
    if (fd < 0) return -1;
    if (send_request(fd, host, port, "GET", path, NULL)) { close(fd); return -1; }
    // read the headers one byte at a time so nothing of the body is consumed
    char h[2048];
    int n = 0;
    while (n < (int)sizeof h - 1) {
        ssize_t k = recv(fd, h + n, 1, 0);
        if (k <= 0) { close(fd); return -1; }
        n++;
        if (n >= 4 && !memcmp(h + n - 4, "\r\n\r\n", 4)) break;
    }
    h[n] = 0;
    int status = 0;
    if (sscanf(h, "HTTP/%*d.%*d %d", &status) != 1 || status != 200) { close(fd); return -1; }
    return fd;
}

int net_discover(int udp_port, char *host, int hostn, int *port, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)udp_port);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const char *msg = "MINIAMP_DISCOVER";
    int found = 0;
    int waited = 0;
    while (!found && waited < timeout_ms) {
        sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof to);
        // wait up to 300 ms for a reply, then ask again (UDP can drop)
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        struct timeval tv = { 0, 300000 };
        int s = select(fd + 1, &r, NULL, NULL, &tv);
        waited += 300;
        if (s <= 0) continue;
        char buf[128];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t k = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (k <= 0) continue;
        buf[k] = 0;
        int p = 0;
        if (sscanf(buf, "CIDER_BRIDGE %d", &p) == 1 && p > 0) {
            inet_ntop(AF_INET, &from.sin_addr, host, (socklen_t)hostn);
            *port = p;
            found = 1;
        }
    }
    close(fd);
    return found;
}
