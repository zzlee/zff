/*=============================================================================
    zstr_net_sink.c — Network UDP/TCP Sink Device (AVOutputFormat)
=============================================================================*/
#define _GNU_SOURCE

#include "zff/plugins/zstr_net.h"
#include "zstr_net_internal.h"
#include "zff/zff_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#include <libavutil/opt.h>
#include <libavutil/error.h>

struct zstr_net_sink {
    int fd;
    int listen_fd;
    zstr_net_protocol_t protocol;
    int timeout_ms;
    struct sockaddr_in dest_addr;
};

/* ---------------------------------------------------------------------------
 * C Direct API
 * --------------------------------------------------------------------------- */
zstr_net_sink_t* zstr_net_sink_create(const zstr_net_config_t *cfg)
{
    if (!cfg) return NULL;

    zstr_net_sink_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->fd = -1;
    s->listen_fd = -1;
    s->protocol = cfg->protocol;
    s->timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 1000;

    memset(&s->dest_addr, 0, sizeof(s->dest_addr));
    s->dest_addr.sin_family = AF_INET;
    s->dest_addr.sin_port = htons(cfg->port ? cfg->port : 5004);

    const char *host = (cfg->host && cfg->host[0]) ? cfg->host : "127.0.0.1";
    inet_pton(AF_INET, host, &s->dest_addr.sin_addr);

    if (cfg->protocol == ZSTR_NET_PROTO_UDP) {
        s->fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (s->fd < 0) {
            free(s);
            return NULL;
        }

        uint32_t ip_h = ntohl(s->dest_addr.sin_addr.s_addr);
        if (ip_h >= 0xE0000000 && ip_h <= 0xEFFFFFFF) {
            /* Multicast */
            int ttl = cfg->ttl > 0 ? cfg->ttl : 1;
            setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
            int loop = cfg->loopback ? 1 : 0;
            setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        }

        int sndbuf = (cfg->buffer_size > 0 ? cfg->buffer_size : 65536) * 4;
        setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    } else if (cfg->protocol == ZSTR_NET_PROTO_TCP_CLIENT) {
        s->fd = socket(AF_INET, SOCK_STREAM, 0);
        if (s->fd < 0) {
            free(s);
            return NULL;
        }
        if (connect(s->fd, (struct sockaddr*)&s->dest_addr, sizeof(s->dest_addr)) < 0) {
            close(s->fd);
            free(s);
            return NULL;
        }
    } else if (cfg->protocol == ZSTR_NET_PROTO_TCP_SERVER) {
        s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (s->listen_fd < 0) {
            free(s);
            return NULL;
        }
        int reuse = 1;
        setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in bind_addr = s->dest_addr;
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s->listen_fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0 ||
            listen(s->listen_fd, 1) < 0)
        {
            close(s->listen_fd);
            free(s);
            return NULL;
        }
    }

    return s;
}

int zstr_net_sink_write(zstr_net_sink_t *s, const uint8_t *buf, int size)
{
    if (!s || !buf || size <= 0) return AVERROR(EINVAL);

    /* For TCP server, accept client if not yet connected */
    if (s->protocol == ZSTR_NET_PROTO_TCP_SERVER && s->fd < 0) {
        struct pollfd pfd = { .fd = s->listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, s->timeout_ms);
        if (pr <= 0) return AVERROR(ETIMEDOUT);

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        s->fd = accept(s->listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (s->fd < 0) return AVERROR(errno);
    }

    ssize_t n;
    if (s->protocol == ZSTR_NET_PROTO_UDP) {
        n = sendto(s->fd, buf, size, 0, (struct sockaddr*)&s->dest_addr, sizeof(s->dest_addr));
    } else {
        n = send(s->fd, buf, size, MSG_NOSIGNAL);
    }

    if (n < 0) return AVERROR(errno);
    return (int)n;
}

int zstr_net_sink_write_packet(zstr_net_sink_t *s, const AVPacket *pkt)
{
    if (!s || !pkt || !pkt->data || pkt->size <= 0) return AVERROR(EINVAL);
    return zstr_net_sink_write(s, pkt->data, pkt->size);
}

void zstr_net_sink_close(zstr_net_sink_t **ps)
{
    if (!ps || !*ps) return;
    zstr_net_sink_t *s = *ps;

    if (s->fd >= 0) close(s->fd);
    if (s->listen_fd >= 0) close(s->listen_fd);
    free(s);
    *ps = NULL;
}

/* ---------------------------------------------------------------------------
 * FFmpeg AVOutputFormat Muxer
 * --------------------------------------------------------------------------- */
typedef struct NetMuxContext {
    const AVClass *av_class;
    char *protocol_str;
    char *host;
    int port;
    int timeout_ms;
    zstr_net_sink_t *sink;
} NetMuxContext;

#define OFFSET(x) offsetof(NetMuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption zstr_net_sink_options[] = {
    { "protocol", "Protocol (udp, tcp_client, tcp_server)", OFFSET(protocol_str), AV_OPT_TYPE_STRING, { .str = "udp" },       0, 0, ENC },
    { "host",     "Destination host/IP",                   OFFSET(host),         AV_OPT_TYPE_STRING, { .str = "127.0.0.1" }, 0, 0, ENC },
    { "port",     "Destination port",                      OFFSET(port),         AV_OPT_TYPE_INT,    { .i64 = 5004 },        0, 65535, ENC },
    { "timeout",  "Socket timeout in ms",                  OFFSET(timeout_ms),   AV_OPT_TYPE_INT,    { .i64 = 1000 },        0, 60000, ENC },
    { NULL }
};

static const AVClass zstr_net_sink_class = {
    .class_name = "zstr_net_sink",
    .item_name  = av_default_item_name,
    .option     = zstr_net_sink_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int net_write_header(AVFormatContext *s)
{
    NetMuxContext *ctx = s->priv_data;

    zstr_net_config_t cfg = {
        .protocol = ZSTR_NET_PROTO_UDP,
        .host = ctx->host ? ctx->host : "127.0.0.1",
        .port = (uint16_t)(ctx->port ? ctx->port : 5004),
        .buffer_size = 65536,
        .timeout_ms = ctx->timeout_ms ? ctx->timeout_ms : 1000,
        .ttl = 1,
        .loopback = true
    };

    /* Parse URL if provided, e.g. "udp://127.0.0.1:5004" */
    char host_buf[128] = {0};
    if (s->url && s->url[0]) {
        const char *proto_sep = strstr(s->url, "://");
        if (proto_sep) {
            size_t proto_len = proto_sep - s->url;
            if (strncmp(s->url, "tcp", proto_len) == 0) {
                cfg.protocol = ZSTR_NET_PROTO_TCP_CLIENT;
            } else {
                cfg.protocol = ZSTR_NET_PROTO_UDP;
            }
            const char *hp = proto_sep + 3;
            const char *colon = strrchr(hp, ':');
            if (colon) {
                size_t hlen = colon - hp;
                if (hlen < sizeof(host_buf)) {
                    strncpy(host_buf, hp, hlen);
                    cfg.host = host_buf;
                }
                cfg.port = (uint16_t)atoi(colon + 1);
            }
        }
    }

    if (ctx->protocol_str) {
        if (strcmp(ctx->protocol_str, "tcp_client") == 0) cfg.protocol = ZSTR_NET_PROTO_TCP_CLIENT;
        else if (strcmp(ctx->protocol_str, "tcp_server") == 0) cfg.protocol = ZSTR_NET_PROTO_TCP_SERVER;
        else cfg.protocol = ZSTR_NET_PROTO_UDP;
    }

    ctx->sink = zstr_net_sink_create(&cfg);
    if (!ctx->sink) return AVERROR(EIO);
    return 0;
}

static int net_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    NetMuxContext *ctx = s->priv_data;
    if (!ctx || !ctx->sink || !pkt) return 0;
    int ret = zstr_net_sink_write_packet(ctx->sink, pkt);
    return ret >= 0 ? 0 : ret;
}

static int net_write_trailer(AVFormatContext *s)
{
    NetMuxContext *ctx = s->priv_data;
    if (ctx && ctx->sink) {
        zstr_net_sink_close(&ctx->sink);
    }
    return 0;
}

const FFOutputFormat ff_zstr_net_sink_muxer = {
    .p = {
        .name           = "zstr_net_sink",
        .long_name      = "zff Network UDP/TCP Sink",
        .extensions     = NULL,
        .audio_codec    = AV_CODEC_ID_NONE,
        .video_codec    = AV_CODEC_ID_NONE,
        .subtitle_codec = AV_CODEC_ID_NONE,
        .flags          = AVFMT_NOFILE | AVFMT_NOTIMESTAMPS,
        .priv_class     = &zstr_net_sink_class,
    },
    .priv_data_size = sizeof(NetMuxContext),
    .write_header   = net_write_header,
    .write_packet   = net_write_packet,
    .write_trailer  = net_write_trailer,
    .check_bitstream = NULL,
};
