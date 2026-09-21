/*=============================================================================
    zstr_whip.c — Minimal WHIP (RFC 9725) client over raw POSIX sockets

    HTTP/1.1 only, "http://" scheme, Content-Length framing, no chunked
    encoding, no TLS, no redirects. Sufficient for WHIP ingestion against
    any compliant endpoint (and the stub server in test_webrtc_whip.c).
 =============================================================================*/
#define _GNU_SOURCE

#include "zff/plugins/zstr_whip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

#include <libavutil/error.h>

#define WHIP_RECV_CAP (256 * 1024)
#define WHIP_TIMEOUT_MS 15000

typedef struct {
    char host[256];
    int port;
    char path[1024];
} whip_url_t;

static int parse_url(const char *url, whip_url_t *out)
{
    /* http://host[:port][/path] */
    if (strncmp(url, "http://", 7) != 0) return AVERROR(EINVAL);
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    size_t hostlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hostlen == 0 || hostlen >= 200) return AVERROR(EINVAL);

    char hostport[256] = "";
    memcpy(hostport, p, hostlen);
    char *colon = strrchr(hostport, ':');
    if (colon && !strchr(colon, ']')) {
        *colon = '\0';
        out->port = atoi(colon + 1);
        if (out->port <= 0 || out->port > 65535) return AVERROR(EINVAL);
    } else {
        out->port = 80;
    }
    snprintf(out->host, sizeof(out->host), "%s", hostport);
    snprintf(out->path, sizeof(out->path), "%s", slash ? slash : "/");
    return 0;
}

static int dial(const whip_url_t *u, int timeout_ms)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", u->port);

    struct addrinfo *res = NULL;
    if (getaddrinfo(u->host, portstr, &hints, &res) != 0 || !res) return AVERROR(EIO);

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int flags = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));
        /* Non-blocking connect with timeout */
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        fcntl(fd, F_SETFL, fl);
        break;
    }
    freeaddrinfo(res);
    return fd >= 0 ? fd : AVERROR(EIO);
}

static int send_all(int fd, const char *buf, size_t len, int timeout_ms)
{
    size_t off = 0;
    while (off < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        if (poll(&pfd, 1, timeout_ms) <= 0) return AVERROR(ETIMEDOUT);
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) return AVERROR(EIO);
        off += (size_t)n;
    }
    return 0;
}

typedef struct {
    int status;
    char *location; /* malloc'd, may be NULL */
    char *body;     /* malloc'd, may be NULL */
    size_t body_len;
} http_resp_t;

static void resp_free(http_resp_t *r)
{
    if (!r) return;
    free(r->location);
    free(r->body);
    memset(r, 0, sizeof(*r));
}

/* Read until full headers + Content-Length body (no chunked support). */
static int recv_response(int fd, http_resp_t *out, int timeout_ms)
{
    memset(out, 0, sizeof(*out));
    char *buf = malloc(WHIP_RECV_CAP + 1);
    if (!buf) return AVERROR(ENOMEM);
    size_t total = 0;
    size_t body_len = 0;
    int have_headers = 0;
    size_t hdr_len = 0;

    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            free(buf);
            return AVERROR(ETIMEDOUT);
        }
        ssize_t n = recv(fd, buf + total, WHIP_RECV_CAP - total, 0);
        if (n <= 0) {
            free(buf);
            return AVERROR(EIO);
        }
        total += (size_t)n;
        buf[total] = '\0';

        if (!have_headers) {
            char *end = strstr(buf, "\r\n\r\n");
            if (!end) {
                if (total >= WHIP_RECV_CAP) {
                    free(buf);
                    return AVERROR(EFBIG);
                }
                continue;
            }
            hdr_len = (size_t)(end - buf) + 4;
            have_headers = 1;

            /* Status line */
            int code = 0;
            if (sscanf(buf, "HTTP/%*d.%*d %d", &code) != 1) {
                free(buf);
                return AVERROR(EIO);
            }
            out->status = code;

            /* Headers we care about */
            char *line = buf;
            while (line < end) {
                char *eol = strstr(line, "\r\n");
                if (!eol || eol > end) break;
                if (strncasecmp(line, "Content-Length:", 15) == 0)
                    body_len = (size_t)atol(line + 15);
                else if (strncasecmp(line, "Location:", 9) == 0) {
                    const char *v = line + 9;
                    while (*v == ' ') v++;
                    size_t vl = (size_t)(eol - v);
                    out->location = malloc(vl + 1);
                    if (out->location) {
                        memcpy(out->location, v, vl);
                        out->location[vl] = '\0';
                    }
                }
                line = eol + 2;
            }
        }

        if (have_headers && total >= hdr_len + body_len) break;
        if (total >= WHIP_RECV_CAP) {
            free(buf);
            return AVERROR(EFBIG);
        }
    }

    if (body_len > 0) {
        out->body = malloc(body_len + 1);
        if (!out->body) {
            free(buf);
            return AVERROR(EIO);
        }
        memcpy(out->body, buf + hdr_len, body_len);
        out->body[body_len] = '\0';
        out->body_len = body_len;
    }
    free(buf);
    return 0;
}

/* Resolve a possibly-relative Location against the request URL. */
static void resolve_url(const whip_url_t *base, const char *loc,
                        char *out, size_t cap)
{
    if (!loc || !loc[0]) {
        snprintf(out, cap, "http://%s:%d%s", base->host, base->port, base->path);
        return;
    }
    if (strncasecmp(loc, "http://", 7) == 0) {
        snprintf(out, cap, "%s", loc);
        return;
    }
    if (loc[0] == '/') {
        snprintf(out, cap, "http://%s:%d%s", base->host, base->port, loc);
        return;
    }
    /* Relative path: merge with base directory */
    const char *slash = strrchr(base->path, '/');
    size_t dirlen = slash ? (size_t)(slash - base->path) + 1 : 1;
    snprintf(out, cap, "http://%s:%d%.*s%s",
             base->host, base->port, (int)dirlen, base->path, loc);
}

int zstr_whip_post_offer(const char *whip_url, const char *offer_sdp,
                         char **answer_sdp, char **resource_url)
{
    if (!whip_url || !offer_sdp || !answer_sdp || !resource_url) return AVERROR(EINVAL);
    *answer_sdp = NULL;
    *resource_url = NULL;

    whip_url_t u;
    if (parse_url(whip_url, &u) < 0) return AVERROR(EINVAL);

    int fd = dial(&u, WHIP_TIMEOUT_MS);
    if (fd < 0) return fd;

    size_t sdp_len = strlen(offer_sdp);
    char hdr[2048];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "POST %s HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Content-Type: application/sdp\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        u.path, u.host, u.port, sdp_len);
    int ret = send_all(fd, hdr, (size_t)hlen, WHIP_TIMEOUT_MS);
    if (ret == 0) ret = send_all(fd, offer_sdp, sdp_len, WHIP_TIMEOUT_MS);

    http_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    if (ret == 0) ret = recv_response(fd, &resp, WHIP_TIMEOUT_MS);
    close(fd);
    if (ret < 0) {
        resp_free(&resp);
        return ret;
    }

    if (resp.status != 201 && resp.status != 200) {
        resp_free(&resp);
        return AVERROR(EIO);
    }
    if (!resp.body || resp.body_len == 0) {
        resp_free(&resp);
        return AVERROR(EIO);
    }

    char resolved[2048] = "";
    resolve_url(&u, resp.location, resolved, sizeof(resolved));

    *answer_sdp = resp.body;
    resp.body = NULL;
    *resource_url = strdup(resolved[0] ? resolved : whip_url);
    resp_free(&resp);
    if (!*resource_url) {
        free(*answer_sdp);
        *answer_sdp = NULL;
        return AVERROR(ENOMEM);
    }
    return 0;
}

int zstr_whip_patch_candidate(const char *resource_url, const char *candidate)
{
    if (!resource_url || !candidate) return AVERROR(EINVAL);

    whip_url_t u;
    if (parse_url(resource_url, &u) < 0) return AVERROR(EINVAL);

    /* WHIP trickle fragment body is the raw candidate line */
    size_t frag_len = strlen(candidate);
    char hdr[2048];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "PATCH %s HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Content-Type: application/trickle-ice-sdpfrag\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        u.path, u.host, u.port, frag_len);

    int fd = dial(&u, WHIP_TIMEOUT_MS);
    if (fd < 0) return fd;
    int ret = send_all(fd, hdr, (size_t)hlen, WHIP_TIMEOUT_MS);
    if (ret == 0) ret = send_all(fd, candidate, frag_len, WHIP_TIMEOUT_MS);

    http_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    if (ret == 0) ret = recv_response(fd, &resp, WHIP_TIMEOUT_MS);
    close(fd);
    int status = resp.status;
    resp_free(&resp);
    if (ret < 0) return ret;
    return (status >= 200 && status < 300) ? 0 : AVERROR(EIO);
}

int zstr_whip_delete(const char *resource_url)
{
    if (!resource_url) return AVERROR(EINVAL);

    whip_url_t u;
    if (parse_url(resource_url, &u) < 0) return AVERROR(EINVAL);

    char hdr[2048];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "DELETE %s HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        u.path, u.host, u.port);

    int fd = dial(&u, WHIP_TIMEOUT_MS);
    if (fd < 0) return fd;
    int ret = send_all(fd, hdr, (size_t)hlen, WHIP_TIMEOUT_MS);

    http_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    if (ret == 0) ret = recv_response(fd, &resp, WHIP_TIMEOUT_MS);
    close(fd);
    int status = resp.status;
    resp_free(&resp);
    if (ret < 0) return ret;
    return (status >= 200 && status < 300) || status == 404 ? 0 : AVERROR(EIO);
}
