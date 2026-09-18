/*=============================================================================
    zstr_net_source.c — Network UDP/TCP Source Device (AVInputFormat)
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
#include <libavutil/time.h>
#include <libavutil/error.h>

struct zstr_net_source {
    int fd;
    int listen_fd;
    zstr_net_protocol_t protocol;
    int buffer_size;
    int timeout_ms;
    struct sockaddr_in src_addr;
};

/* ---------------------------------------------------------------------------
 * C Direct API
 * --------------------------------------------------------------------------- */
zstr_net_source_t* zstr_net_source_create(const zstr_net_config_t *cfg)
{
    if (!cfg) return NULL;

    zstr_net_source_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->fd = -1;
    s->listen_fd = -1;
    s->protocol = cfg->protocol;
    s->buffer_size = cfg->buffer_size > 0 ? cfg->buffer_size : 65536;
    s->timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 1000;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port);

    if (cfg->host && cfg->host[0] != '\0') {
        inet_pton(AF_INET, cfg->host, &addr.sin_addr);
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (cfg->protocol == ZSTR_NET_PROTO_UDP) {
        s->fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (s->fd < 0) {
            free(s);
            return NULL;
        }

        int reuse = 1;
        setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
        setsockopt(s->fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
        int rcvbuf = s->buffer_size * 4;
        setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        struct sockaddr_in bind_addr = addr;
        /* If specific multicast address, bind to INADDR_ANY for reception */
        uint32_t ip_h = ntohl(addr.sin_addr.s_addr);
        if ((ip_h >= 0xE0000000 && ip_h <= 0xEFFFFFFF) || cfg->is_multicast) {
            bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        }

        if (bind(s->fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
            close(s->fd);
            free(s);
            return NULL;
        }

        /* Multicast join */
        if ((ip_h >= 0xE0000000 && ip_h <= 0xEFFFFFFF) || cfg->is_multicast) {
            struct ip_mreq mreq;
            mreq.imr_multiaddr = addr.sin_addr;
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            setsockopt(s->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        }
    } else if (cfg->protocol == ZSTR_NET_PROTO_TCP_SERVER) {
        s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (s->listen_fd < 0) {
            free(s);
            return NULL;
        }
        int reuse = 1;
        setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(s->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
            listen(s->listen_fd, 1) < 0)
        {
            close(s->listen_fd);
            free(s);
            return NULL;
        }
    } else if (cfg->protocol == ZSTR_NET_PROTO_TCP_CLIENT) {
        s->fd = socket(AF_INET, SOCK_STREAM, 0);
        if (s->fd < 0) {
            free(s);
            return NULL;
        }
        if (connect(s->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(s->fd);
            free(s);
            return NULL;
        }
    }

    return s;
}

int zstr_net_source_read(zstr_net_source_t *s, uint8_t *buf, int size)
{
    if (!s || !buf || size <= 0) return AVERROR(EINVAL);

    /* For TCP server, accept incoming client if not yet connected */
    if (s->protocol == ZSTR_NET_PROTO_TCP_SERVER && s->fd < 0) {
        struct pollfd pfd = { .fd = s->listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, s->timeout_ms);
        if (pr <= 0) return AVERROR(ETIMEDOUT);

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        s->fd = accept(s->listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (s->fd < 0) return AVERROR(errno);
    }

    struct pollfd pfd = { .fd = s->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, s->timeout_ms);
    if (pr < 0) return AVERROR(errno);
    if (pr == 0) return AVERROR(ETIMEDOUT);

    ssize_t n;
    if (s->protocol == ZSTR_NET_PROTO_UDP) {
        socklen_t slen = sizeof(s->src_addr);
        n = recvfrom(s->fd, buf, size, 0, (struct sockaddr*)&s->src_addr, &slen);
    } else {
        n = recv(s->fd, buf, size, 0);
    }

    if (n < 0) return AVERROR(errno);
    if (n == 0 && s->protocol != ZSTR_NET_PROTO_UDP) {
        /* TCP connection closed */
        close(s->fd);
        s->fd = -1;
        return AVERROR_EOF;
    }

    return (int)n;
}

int zstr_net_source_read_packet(zstr_net_source_t *s, AVPacket *pkt)
{
    if (!s || !pkt) return AVERROR(EINVAL);

    int ret = av_new_packet(pkt, s->buffer_size);
    if (ret < 0) return ret;

    ret = zstr_net_source_read(s, pkt->data, s->buffer_size);
    if (ret < 0) {
        av_packet_unref(pkt);
        return ret;
    }

    pkt->size = ret;
    pkt->pts = av_gettime_relative();
    pkt->dts = pkt->pts;
    return 0;
}

void zstr_net_source_close(zstr_net_source_t **ps)
{
    if (!ps || !*ps) return;
    zstr_net_source_t *s = *ps;

    if (s->fd >= 0) close(s->fd);
    if (s->listen_fd >= 0) close(s->listen_fd);
    free(s);
    *ps = NULL;
}

/* ---------------------------------------------------------------------------
 * FFmpeg AVInputFormat Demuxer
 * --------------------------------------------------------------------------- */
typedef struct NetDemuxContext {
    const AVClass *av_class;
    char *protocol_str;
    int port;
    int buffer_size;
    int timeout_ms;
    zstr_net_source_t *source;
} NetDemuxContext;

#define OFFSET(x) offsetof(NetDemuxContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption zstr_net_source_options[] = {
    { "protocol",    "Protocol (udp, tcp_client, tcp_server)", OFFSET(protocol_str), AV_OPT_TYPE_STRING, { .str = "udp" }, 0, 0, DEC },
    { "port",        "Port to bind or connect",                OFFSET(port),         AV_OPT_TYPE_INT,    { .i64 = 5004 },  0, 65535, DEC },
    { "buffer_size", "Socket buffer size",                     OFFSET(buffer_size),  AV_OPT_TYPE_INT,    { .i64 = 65536 }, 1024, 10485760, DEC },
    { "timeout",     "Read timeout in ms",                     OFFSET(timeout_ms),   AV_OPT_TYPE_INT,    { .i64 = 1000 },  0, 60000, DEC },
    { NULL }
};

static const AVClass zstr_net_source_class = {
    .class_name = "zstr_net_src",
    .item_name  = av_default_item_name,
    .option     = zstr_net_source_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int net_read_header(AVFormatContext *s)
{
    NetDemuxContext *ctx = s->priv_data;

    zstr_net_config_t cfg = {
        .protocol = ZSTR_NET_PROTO_UDP,
        .host = NULL,
        .port = (uint16_t)(ctx->port ? ctx->port : 5004),
        .buffer_size = ctx->buffer_size,
        .timeout_ms = ctx->timeout_ms,
        .is_multicast = false
    };

    /* Parse URL if provided, e.g. "udp://127.0.0.1:5004" or "tcp://0.0.0.0:8000" */
    char host_buf[128] = {0};
    if (s->url && s->url[0]) {
        const char *proto_sep = strstr(s->url, "://");
        if (proto_sep) {
            size_t proto_len = proto_sep - s->url;
            if (strncmp(s->url, "tcp", proto_len) == 0) {
                cfg.protocol = ZSTR_NET_PROTO_TCP_SERVER;
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

    ctx->source = zstr_net_source_create(&cfg);
    if (!ctx->source) return AVERROR(EIO);

    AVStream *st = avformat_new_stream(s, NULL);
    if (!st) return AVERROR(ENOMEM);
    st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    st->codecpar->codec_id = AV_CODEC_ID_NONE;
    st->time_base = (AVRational){ 1, 1000000 };

    return 0;
}

static int net_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    NetDemuxContext *ctx = s->priv_data;
    if (!ctx || !ctx->source) return AVERROR(EINVAL);
    return zstr_net_source_read_packet(ctx->source, pkt);
}

static int net_read_close(AVFormatContext *s)
{
    NetDemuxContext *ctx = s->priv_data;
    if (ctx && ctx->source) {
        zstr_net_source_close(&ctx->source);
    }
    return 0;
}

const AVInputFormat ff_zstr_net_source_demuxer = {
    .name           = "zstr_net_src",
    .long_name      = "zff Network UDP/TCP Source",
    .flags          = AVFMT_NOFILE,
    .priv_class     = &zstr_net_source_class,
    .priv_data_size = sizeof(NetDemuxContext),
    .read_header    = net_read_header,
    .read_packet    = net_read_packet,
    .read_close     = net_read_close,
};
