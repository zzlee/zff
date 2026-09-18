/*=============================================================================
    zstr_srt_source.c — SRT (Secure Reliable Transport) Source Demuxer
=============================================================================*/
#define _POSIX_C_SOURCE 200809L

#include "zstr_srt_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>

struct zstr_srt_source {
    zstr_srt_config_t cfg;
    char host_buf[128];
    char pass_buf[128];
    char streamid_buf[256];

    SRTSOCKET listen_sock;
    SRTSOCKET conn_sock;
};

static int setup_sockaddr(const char *host, uint16_t port, struct sockaddr_in *addr)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);

    if (!host || host[0] == '\0' || strcmp(host, "0.0.0.0") == 0) {
        addr->sin_addr.s_addr = INADDR_ANY;
        return 0;
    }

    if (inet_pton(AF_INET, host, &addr->sin_addr) <= 0) {
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) {
            return AVERROR(EINVAL);
        }
        memcpy(&addr->sin_addr, he->h_addr_list[0], sizeof(addr->sin_addr));
    }
    return 0;
}

static int srt_source_ensure_connected(zstr_srt_source_t *s)
{
    if (s->conn_sock != SRT_INVALID_SOCK) return 0;

    if (s->cfg.mode == ZSTR_SRT_MODE_LISTENER) {
        if (s->listen_sock == SRT_INVALID_SOCK) return AVERROR(EIO);

        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        s->conn_sock = srt_accept(s->listen_sock, (struct sockaddr*)&client_addr, &client_len);
        if (s->conn_sock == SRT_INVALID_SOCK) {
            return AVERROR(ETIMEDOUT);
        }
        return 0;
    }

    if (s->cfg.mode == ZSTR_SRT_MODE_CALLER) {
        s->conn_sock = srt_create_socket();
        if (s->conn_sock == SRT_INVALID_SOCK) return AVERROR(EIO);

        zstr_srt_apply_socket_options(s->conn_sock, &s->cfg, false);

        struct sockaddr_in remote_addr;
        if (setup_sockaddr(s->cfg.host, s->cfg.port, &remote_addr) < 0) {
            srt_close(s->conn_sock);
            s->conn_sock = SRT_INVALID_SOCK;
            return AVERROR(EINVAL);
        }

        if (srt_connect(s->conn_sock, (struct sockaddr*)&remote_addr, sizeof(remote_addr)) == SRT_ERROR) {
            srt_close(s->conn_sock);
            s->conn_sock = SRT_INVALID_SOCK;
            return AVERROR(ECONNREFUSED);
        }
        return 0;
    }

    return AVERROR(EINVAL);
}

zstr_srt_source_t* zstr_srt_source_create(const zstr_srt_config_t *cfg)
{
    if (!cfg) return NULL;

    zstr_srt_global_init();

    zstr_srt_source_t *s = calloc(1, sizeof(*s));
    if (!s) {
        zstr_srt_global_cleanup();
        return NULL;
    }

    s->listen_sock = SRT_INVALID_SOCK;
    s->conn_sock = SRT_INVALID_SOCK;
    s->cfg = *cfg;

    if (cfg->host) {
        strncpy(s->host_buf, cfg->host, sizeof(s->host_buf) - 1);
        s->cfg.host = s->host_buf;
    }
    if (cfg->passphrase) {
        strncpy(s->pass_buf, cfg->passphrase, sizeof(s->pass_buf) - 1);
        s->cfg.passphrase = s->pass_buf;
    }
    if (cfg->streamid) {
        strncpy(s->streamid_buf, cfg->streamid, sizeof(s->streamid_buf) - 1);
        s->cfg.streamid = s->streamid_buf;
    }

    if (s->cfg.payload_size <= 0) s->cfg.payload_size = 1316;
    if (s->cfg.timeout_ms <= 0) s->cfg.timeout_ms = 3000;

    if (s->cfg.mode == ZSTR_SRT_MODE_LISTENER) {
        s->listen_sock = srt_create_socket();
        if (s->listen_sock == SRT_INVALID_SOCK) {
            free(s);
            zstr_srt_global_cleanup();
            return NULL;
        }

        zstr_srt_apply_socket_options(s->listen_sock, &s->cfg, false);

        struct sockaddr_in bind_addr;
        if (setup_sockaddr(s->cfg.host, s->cfg.port, &bind_addr) < 0 ||
            srt_bind(s->listen_sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) == SRT_ERROR ||
            srt_listen(s->listen_sock, 1) == SRT_ERROR) {
            srt_close(s->listen_sock);
            free(s);
            zstr_srt_global_cleanup();
            return NULL;
        }
    }

    return s;
}

zstr_srt_source_t* zstr_srt_source_create_from_url(const char *url)
{
    zstr_srt_config_t cfg;
    char host[128] = {0};
    char pass[128] = {0};
    char streamid[256] = {0};

    if (zstr_srt_parse_url(url, &cfg, host, sizeof(host), pass, sizeof(pass), streamid, sizeof(streamid)) < 0) {
        return NULL;
    }
    return zstr_srt_source_create(&cfg);
}

int zstr_srt_source_read(zstr_srt_source_t *s, uint8_t *buf, int size)
{
    if (!s || !buf || size <= 0) return AVERROR(EINVAL);

    int ret = srt_source_ensure_connected(s);
    if (ret < 0) return ret;

    ret = srt_recvmsg(s->conn_sock, (char*)buf, size);
    if (ret < 0) {
        int srt_err = srt_getlasterror(NULL);
        if (srt_err == SRT_ETIMEOUT) return AVERROR(ETIMEDOUT);
        return AVERROR(EIO);
    }
    return ret;
}

int zstr_srt_source_read_packet(zstr_srt_source_t *s, AVPacket *pkt)
{
    if (!s || !pkt) return AVERROR(EINVAL);

    int max_sz = s->cfg.payload_size > 0 ? s->cfg.payload_size : 1316;
    if (max_sz < 1500) max_sz = 1500;

    int ret = av_new_packet(pkt, max_sz);
    if (ret < 0) return ret;

    ret = zstr_srt_source_read(s, pkt->data, max_sz);
    if (ret < 0) {
        av_packet_unref(pkt);
        return ret;
    }

    pkt->size = ret;
    pkt->pts = AV_NOPTS_VALUE;
    pkt->dts = AV_NOPTS_VALUE;
    return 0;
}

void zstr_srt_source_close(zstr_srt_source_t **ps)
{
    if (!ps || !*ps) return;
    zstr_srt_source_t *s = *ps;

    if (s->conn_sock != SRT_INVALID_SOCK) {
        srt_close(s->conn_sock);
        s->conn_sock = SRT_INVALID_SOCK;
    }
    if (s->listen_sock != SRT_INVALID_SOCK) {
        srt_close(s->listen_sock);
        s->listen_sock = SRT_INVALID_SOCK;
    }

    free(s);
    *ps = NULL;
    zstr_srt_global_cleanup();
}

/* ---------------------------------------------------------------------------
 * FFmpeg AVInputFormat Demuxer
 * --------------------------------------------------------------------------- */
typedef struct SrtDemuxContext {
    const AVClass *av_class;
    char *host;
    int port;
    char *mode_str;
    int latency;
    char *passphrase;
    int pbkeylen;
    char *streamid;
    int payload_size;
    int timeout_ms;

    zstr_srt_source_t *src;
} SrtDemuxContext;

#define OFFSET(x) offsetof(SrtDemuxContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM

static const AVOption zstr_srt_demux_options[] = {
    { "host",         "Host or IP address", OFFSET(host),         AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, DEC },
    { "port",         "Port number",        OFFSET(port),         AV_OPT_TYPE_INT,    { .i64 = 0 },    0, 65535, DEC },
    { "mode",         "Connection mode",    OFFSET(mode_str),     AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, DEC },
    { "latency",      "Latency in ms",      OFFSET(latency),      AV_OPT_TYPE_INT,    { .i64 = 0 },    0, 10000, DEC },
    { "passphrase",   "AES Passphrase",     OFFSET(passphrase),   AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, DEC },
    { "pbkeylen",     "Key length in bytes",OFFSET(pbkeylen),     AV_OPT_TYPE_INT,    { .i64 = 0 },    0, 32, DEC },
    { "streamid",     "Stream ID",          OFFSET(streamid),     AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, DEC },
    { "payload_size", "Payload size",       OFFSET(payload_size), AV_OPT_TYPE_INT,    { .i64 = 0 },    0, 65536, DEC },
    { "timeout",      "Timeout in ms",      OFFSET(timeout_ms),   AV_OPT_TYPE_INT,    { .i64 = 0 },    0, 60000, DEC },
    { NULL }
};

static const AVClass zstr_srt_demux_class = {
    .class_name = "zstr_srt_src",
    .item_name  = av_default_item_name,
    .option     = zstr_srt_demux_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int srt_read_header(AVFormatContext *s)
{
    SrtDemuxContext *ctx = s->priv_data;

    zstr_srt_config_t cfg;
    char host_buf[128] = {0};
    char pass_buf[128] = {0};
    char streamid_buf[256] = {0};

    if (s->url && s->url[0]) {
        zstr_srt_parse_url(s->url, &cfg, host_buf, sizeof(host_buf),
                           pass_buf, sizeof(pass_buf), streamid_buf, sizeof(streamid_buf));
    } else {
        memset(&cfg, 0, sizeof(cfg));
        cfg.mode = ZSTR_SRT_MODE_CALLER;
        cfg.host = "127.0.0.1";
        cfg.port = 9000;
        cfg.latency_ms = 120;
        cfg.payload_size = 1316;
        cfg.timeout_ms = 3000;
        cfg.pbkeylen = 16;
    }

    if (ctx->host && ctx->host[0]) cfg.host = ctx->host;
    if (ctx->port > 0) cfg.port = (uint16_t)ctx->port;
    if (ctx->latency > 0) cfg.latency_ms = ctx->latency;
    if (ctx->passphrase && ctx->passphrase[0]) cfg.passphrase = ctx->passphrase;
    if (ctx->pbkeylen > 0) cfg.pbkeylen = ctx->pbkeylen;
    if (ctx->streamid && ctx->streamid[0]) cfg.streamid = ctx->streamid;
    if (ctx->payload_size > 0) cfg.payload_size = ctx->payload_size;
    if (ctx->timeout_ms > 0) cfg.timeout_ms = ctx->timeout_ms;

    if (ctx->mode_str && ctx->mode_str[0]) {
        if (strcmp(ctx->mode_str, "listener") == 0) cfg.mode = ZSTR_SRT_MODE_LISTENER;
        else if (strcmp(ctx->mode_str, "rendezvous") == 0) cfg.mode = ZSTR_SRT_MODE_RENDEZVOUS;
        else if (strcmp(ctx->mode_str, "caller") == 0) cfg.mode = ZSTR_SRT_MODE_CALLER;
    }

    ctx->src = zstr_srt_source_create(&cfg);
    if (!ctx->src) return AVERROR(EIO);

    AVStream *st = avformat_new_stream(s, NULL);
    if (!st) return AVERROR(ENOMEM);
    st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    st->time_base = (AVRational){ 1, 1000 };

    return 0;
}

static int srt_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    SrtDemuxContext *ctx = s->priv_data;
    if (!ctx || !ctx->src) return AVERROR(EIO);
    return zstr_srt_source_read_packet(ctx->src, pkt);
}

static int srt_read_close(AVFormatContext *s)
{
    SrtDemuxContext *ctx = s->priv_data;
    if (ctx && ctx->src) {
        zstr_srt_source_close(&ctx->src);
    }
    return 0;
}

const AVInputFormat ff_zstr_srt_source_demuxer = {
    .name           = "zstr_srt_src",
    .long_name      = "zff SRT (Secure Reliable Transport) Source",
    .flags          = AVFMT_NOFILE,
    .priv_data_size = sizeof(SrtDemuxContext),
    .priv_class     = &zstr_srt_demux_class,
    .read_header    = srt_read_header,
    .read_packet    = srt_read_packet,
    .read_close     = srt_read_close,
};
