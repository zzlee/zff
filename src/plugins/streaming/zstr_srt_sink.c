/*=============================================================================
    zstr_srt_sink.c — SRT (Secure Reliable Transport) Sink Muxer
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

struct zstr_srt_sink {
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

static int srt_sink_ensure_connected(zstr_srt_sink_t *s)
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

        zstr_srt_apply_socket_options(s->conn_sock, &s->cfg, true);

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

zstr_srt_sink_t* zstr_srt_sink_create(const zstr_srt_config_t *cfg)
{
    if (!cfg) return NULL;

    zstr_srt_global_init();

    zstr_srt_sink_t *s = calloc(1, sizeof(*s));
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

        zstr_srt_apply_socket_options(s->listen_sock, &s->cfg, true);

        struct sockaddr_in bind_addr;
        if (setup_sockaddr(s->cfg.host, s->cfg.port, &bind_addr) < 0 ||
            srt_bind(s->listen_sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) == SRT_ERROR ||
            srt_listen(s->listen_sock, 1) == SRT_ERROR) {
            srt_close(s->listen_sock);
            free(s);
            zstr_srt_global_cleanup();
            return NULL;
        }
    } else if (s->cfg.mode == ZSTR_SRT_MODE_CALLER) {
        srt_sink_ensure_connected(s);
    }

    return s;
}

zstr_srt_sink_t* zstr_srt_sink_create_from_url(const char *url)
{
    zstr_srt_config_t cfg;
    char host[128] = {0};
    char pass[128] = {0};
    char streamid[256] = {0};

    if (zstr_srt_parse_url(url, &cfg, host, sizeof(host), pass, sizeof(pass), streamid, sizeof(streamid)) < 0) {
        return NULL;
    }
    return zstr_srt_sink_create(&cfg);
}

int zstr_srt_sink_write(zstr_srt_sink_t *s, const uint8_t *buf, int size)
{
    if (!s || !buf || size <= 0) return AVERROR(EINVAL);

    int ret = srt_sink_ensure_connected(s);
    if (ret < 0) return ret;

    ret = srt_sendmsg(s->conn_sock, (const char*)buf, size, -1, 0);
    if (ret < 0) {
        int srt_err = srt_getlasterror(NULL);
        if (srt_err == SRT_ETIMEOUT) return AVERROR(ETIMEDOUT);
        return AVERROR(EIO);
    }
    return ret;
}

int zstr_srt_sink_write_packet(zstr_srt_sink_t *s, const AVPacket *pkt)
{
    if (!s || !pkt || !pkt->data || pkt->size <= 0) return AVERROR(EINVAL);
    return zstr_srt_sink_write(s, pkt->data, pkt->size);
}

void zstr_srt_sink_close(zstr_srt_sink_t **ps)
{
    if (!ps || !*ps) return;
    zstr_srt_sink_t *s = *ps;

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
 * FFmpeg FFOutputFormat Muxer
 * --------------------------------------------------------------------------- */
typedef struct SrtMuxContext {
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

    zstr_srt_sink_t *sink;
} SrtMuxContext;

#define OFFSET(x) offsetof(SrtMuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption zstr_srt_mux_options[] = {
    { "host",         "Host or IP address", OFFSET(host),         AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },
    { "port",         "Port number",        OFFSET(port),         AV_OPT_TYPE_INT,    { .i64 = 0 },    0, 65535, ENC },
    { "mode",         "Connection mode",    OFFSET(mode_str),     AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },
    { "latency",      "Latency in ms",      OFFSET(latency),      AV_OPT_TYPE_INT,    { .i64 = 0 },    0, 10000, ENC },
    { "passphrase",   "AES Passphrase",     OFFSET(passphrase),   AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },
    { "pbkeylen",     "Key length in bytes",OFFSET(pbkeylen),     AV_OPT_TYPE_INT,    { .i64 = 0 },    0, 32, ENC },
    { "streamid",     "Stream ID",          OFFSET(streamid),     AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },
    { "payload_size", "Payload size",       OFFSET(payload_size), AV_OPT_TYPE_INT,    { .i64 = 0 },    0, 65536, ENC },
    { "timeout",      "Timeout in ms",      OFFSET(timeout_ms),   AV_OPT_TYPE_INT,    { .i64 = 0 },    0, 60000, ENC },
    { NULL }
};

static const AVClass zstr_srt_mux_class = {
    .class_name = "zstr_srt_sink",
    .item_name  = av_default_item_name,
    .option     = zstr_srt_mux_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int srt_write_header(AVFormatContext *s)
{
    SrtMuxContext *ctx = s->priv_data;

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

    ctx->sink = zstr_srt_sink_create(&cfg);
    if (!ctx->sink) return AVERROR(EIO);
    return 0;
}

static int srt_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    SrtMuxContext *ctx = s->priv_data;
    if (!ctx || !ctx->sink || !pkt) return 0;
    int ret = zstr_srt_sink_write_packet(ctx->sink, pkt);
    return ret >= 0 ? 0 : ret;
}

static int srt_write_trailer(AVFormatContext *s)
{
    SrtMuxContext *ctx = s->priv_data;
    if (ctx && ctx->sink) {
        zstr_srt_sink_close(&ctx->sink);
    }
    return 0;
}

const FFOutputFormat ff_zstr_srt_sink_muxer = {
    .p = {
        .name           = "zstr_srt_sink",
        .long_name      = "zff SRT (Secure Reliable Transport) Sink",
        .extensions     = NULL,
        .audio_codec    = AV_CODEC_ID_NONE,
        .video_codec    = AV_CODEC_ID_NONE,
        .subtitle_codec = AV_CODEC_ID_NONE,
        .flags          = AVFMT_NOFILE | AVFMT_NOTIMESTAMPS,
        .priv_class     = &zstr_srt_mux_class,
    },
    .priv_data_size = sizeof(SrtMuxContext),
    .write_header   = srt_write_header,
    .write_packet   = srt_write_packet,
    .write_trailer  = srt_write_trailer,
};
